package com.tahiel.navigation

import android.Manifest
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.*
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationManager
import android.os.*
import android.preference.PreferenceManager
import android.provider.Settings
import android.text.InputType
import android.util.Base64
import android.util.Log
import android.view.Menu
import android.view.MenuItem
import android.widget.*
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.gson.Gson
import com.google.gson.annotations.SerializedName
import okhttp3.*
import okhttp3.MediaType.Companion.toMediaTypeOrNull
import okhttp3.RequestBody.Companion.toRequestBody
import org.osmdroid.config.Configuration
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.util.BoundingBox
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Polyline
import java.io.File
import java.util.*
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

class MainActivity : AppCompatActivity() {

    // ── UI ────────────────────────────────────────────────────────────────────
    private lateinit var mapView: MapView
    private lateinit var btnConnect: Button
    private lateinit var btnSetDestination: Button
    private lateinit var btnStartSending: Button
    private lateinit var btnSaveRoute: Button
    private lateinit var btnLoadRoute: Button
    private lateinit var etDestination: EditText
    private lateinit var tvStatus: TextView
    private lateinit var spinnerRoadType: Spinner
    private lateinit var statusDot: android.view.View

    // ── Service binding ───────────────────────────────────────────────────────
    private var navService: NavService? = null
    private var isBound = false

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, b: IBinder) {
            navService = (b as NavService.LocalBinder).getService()
            isBound = true
            navService!!.onLocationChanged = { loc ->
                runOnUiThread {
                    updateMapLocation(loc)
                }
            }
            navService!!.onWriteComplete = { success ->
                if (!success) Log.w(TAG, "BLE write failed — OTA may stall")
                otaWriteSemaphore.release()
            }
            Log.d(TAG, "Bound to NavService")
        }

        override fun onServiceDisconnected(name: ComponentName) {
            isBound = false
            navService = null
        }
    }

    // ── OTA semaphore ─────────────────────────────────────────────────────────
    // Released by NavService.onWriteComplete (BluetoothGattCallback.onCharacteristicWrite).
    // The OTA thread acquires before each send so we never overlap writes.
    private val otaWriteSemaphore = Semaphore(0)

    // ── Map overlays ──────────────────────────────────────────────────────────
    private var currentMarker: Marker? = null
    private var destinationMarker: Marker? = null
    private var routePolyline: Polyline? = null
    private var destinationLocation: GeoPoint? = null
    private var currentRouteWaypoints: List<GeoPoint>? = null

    // ── Road type ─────────────────────────────────────────────────────────────
    private var selectedRoadType = RoadTypePreference.ALL_ROADS

    // ── BLE UUIDs ─────────────────────────────────────────────────────────────
    private val SERVICE_UUID = UUID.fromString("4fafc201-1fb5-459e-8fcc-c5c9c331914b")
    private val CHAR_UUID    = UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a8")

    companion object {
        private const val TAG = "MainActivity"
        private const val REQUEST_PERMISSIONS = 1
        private const val REQUEST_ENABLE_BT   = 2
        private const val REQUEST_ENABLE_LOC  = 3
        private const val ROUTES_DIR          = "routes"
        private const val ROUTE_EXT           = ".bcr"

        private const val FW_URL         = "https://raw.githubusercontent.com/tahielmarcellino/Tahvia-GPS-Navigator/main/Firmware/.pio/build/nodemcu-32s/firmware.bin"
        private const val FW_VERSION_URL = "https://raw.githubusercontent.com/tahielmarcellino/Tahvia-GPS-Navigator/main/README.md"

        // OTA chunk sizing.
        // Usable BLE payload = MTU - 3 (ATT overhead).
        // Each chunk is base64-encoded and wrapped in a JSON envelope (~60 bytes overhead).
        // base64 expansion = 4/3, so: maxBinary = floor((usable - overhead) * 3 / 4).
        // Capped at FIRMWARE_CHUNK_BYTES to match the ESP32's OTA_BUF_SIZE.
        private const val JSON_ENVELOPE_OVERHEAD  = 60
        private const val FIRMWARE_CHUNK_BYTES    = 192
        private const val WRITE_CONFIRM_TIMEOUT_MS = 3000L

        private const val BLE_SCAN_TIMEOUT_MS     = 10_000L
        private const val BLE_CONNECT_DELAY_MS    = 300L
        private const val BLE_MTU_DISCOVER_DELAY  = 300L
        private const val BLE_REQUEST_MTU         = 512
    }

    // ── Road type enum ────────────────────────────────────────────────────────
    enum class RoadTypePreference(
        val displayName: String,
        val avoidTags: List<String>,
        val preferTags: List<String>
    ) {
        ALL_ROADS(
            "All Roads",
            emptyList(), emptyList()
        ),
        AVOID_HIGHWAYS(
            "Avoid Highways",
            listOf("motorway", "trunk"), emptyList()
        ),
        AVOID_AVENUES(
            "Avoid Major Roads",
            listOf("motorway", "trunk", "primary"),
            listOf("tertiary", "residential", "living_street")
        ),
        PREFER_STREETS(
            "Prefer Side Streets",
            listOf("motorway", "trunk", "primary", "secondary"),
            listOf("residential", "living_street", "tertiary")
        ),
        RESIDENTIAL_ONLY(
            "Residential Only",
            listOf("motorway", "trunk", "primary", "secondary", "tertiary"),
            listOf("residential", "living_street")
        )
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        Configuration.getInstance().load(this, PreferenceManager.getDefaultSharedPreferences(this))
        Configuration.getInstance().userAgentValue = packageName

        initViews()
        checkPermissions()
        setupMap()
        ensureRoutesDir()
        startAndBindService()
    }

    override fun onResume()  { super.onResume();  mapView.onResume()  }
    override fun onPause()   { super.onPause();   mapView.onPause()   }

    override fun onDestroy() {
        super.onDestroy()
        if (isBound) {
            unbindService(serviceConnection)
            isBound = false
        }
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.menu_main, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == R.id.action_firmware) { showFirmwareDialog(); return true }
        return super.onOptionsItemSelected(item)
    }

    // ── Service ───────────────────────────────────────────────────────────────
    private fun startAndBindService() {
        val intent = Intent(this, NavService::class.java)
        startForegroundService(intent)
        bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
    }

    // ── Views ─────────────────────────────────────────────────────────────────
    private fun initViews() {
        mapView           = findViewById(R.id.mapView)
        btnConnect        = findViewById(R.id.btnConnect)
        btnSetDestination = findViewById(R.id.btnSetDestination)
        btnStartSending   = findViewById(R.id.btnStartSending)
        btnSaveRoute      = findViewById(R.id.btnSaveRoute)
        btnLoadRoute      = findViewById(R.id.btnLoadRoute)
        etDestination     = findViewById(R.id.etDestination)
        tvStatus          = findViewById(R.id.tvStatus)
        spinnerRoadType   = findViewById(R.id.spinnerRoadType)
        statusDot         = findViewById(R.id.viewStatusDot)

        findViewById<ImageButton>(R.id.btnFirmware).setOnClickListener { showFirmwareDialog() }

        // Road type spinner
        ArrayAdapter(
            this,
            android.R.layout.simple_spinner_item,
            RoadTypePreference.values().map { it.displayName }
        ).also { adapter ->
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            spinnerRoadType.adapter = adapter
        }
        spinnerRoadType.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: android.view.View?, pos: Int, id: Long) {
                selectedRoadType = RoadTypePreference.values()[pos]
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        btnConnect.setOnClickListener        { if (checkBluetoothAndLocation()) scanAndConnect() }
        btnSetDestination.setOnClickListener { setDestination() }
        btnStartSending.setOnClickListener   { toggleSending() }
        btnSaveRoute.setOnClickListener      { showSaveRouteDialog() }
        btnLoadRoute.setOnClickListener      { showLoadRouteDialog() }

        btnStartSending.isEnabled   = false
        btnSetDestination.isEnabled = false
        btnSaveRoute.isEnabled      = false
        btnLoadRoute.isEnabled      = true
    }

    // ── Dot indicator ─────────────────────────────────────────────────────────
    private enum class DotState { IDLE, CONNECTED, ERROR }

    private fun setDotState(state: DotState) {
        val res = when (state) {
            DotState.IDLE      -> R.drawable.dot_idle
            DotState.CONNECTED -> R.drawable.dot_connected
            DotState.ERROR     -> R.drawable.dot_error
        }
        statusDot.setBackgroundResource(res)
    }

    // ── Permissions ───────────────────────────────────────────────────────────
    private fun checkPermissions() {
        val perms = mutableListOf(
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            perms += listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            perms += listOf(Manifest.permission.BLUETOOTH, Manifest.permission.BLUETOOTH_ADMIN)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perms += Manifest.permission.POST_NOTIFICATIONS
        }

        val needed = perms.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (needed.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, needed.toTypedArray(), REQUEST_PERMISSIONS)
        } else {
            checkBluetoothAndLocation()
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_PERMISSIONS) {
            if (grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
                checkBluetoothAndLocation()
            } else {
                Toast.makeText(
                    this,
                    "Location and Bluetooth access are required to use Tahvia.",
                    Toast.LENGTH_LONG
                ).show()
            }
        }
    }

    private fun checkBluetoothAndLocation(): Boolean {
        if (!isBluetoothEnabled()) { promptEnableBluetooth(); return false }
        if (!isLocationEnabled())  { promptEnableLocation();  return false }
        return true
    }

    private fun isBluetoothEnabled(): Boolean =
        (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter?.isEnabled == true

    private fun isLocationEnabled(): Boolean {
        val mgr = getSystemService(Context.LOCATION_SERVICE) as LocationManager
        return mgr.isProviderEnabled(LocationManager.GPS_PROVIDER) ||
                mgr.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
    }

    private fun promptEnableBluetooth() {
        AlertDialog.Builder(this)
            .setTitle("Bluetooth Required")
            .setMessage("Turn on Bluetooth to connect to your navigation device.")
            .setPositiveButton("Enable") { _, _ ->
                startActivityForResult(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQUEST_ENABLE_BT)
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun promptEnableLocation() {
        AlertDialog.Builder(this)
            .setTitle("Location Required")
            .setMessage("Location access is needed for real-time GPS navigation.")
            .setPositiveButton("Enable") { _, _ ->
                startActivityForResult(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS), REQUEST_ENABLE_LOC)
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        when (requestCode) {
            REQUEST_ENABLE_BT  ->
                if (!isBluetoothEnabled())
                    Toast.makeText(this, "Bluetooth must be enabled to continue.", Toast.LENGTH_LONG).show()
            REQUEST_ENABLE_LOC ->
                if (!isLocationEnabled())
                    Toast.makeText(this, "Location must be enabled to continue.", Toast.LENGTH_LONG).show()
        }
    }

    // ── Map ───────────────────────────────────────────────────────────────────
    private fun setupMap() {
        mapView.setTileSource(TileSourceFactory.MAPNIK)
        mapView.setMultiTouchControls(true)
        mapView.controller.setZoom(15.0)
        mapView.controller.setCenter(GeoPoint(0.0, 0.0))
    }

    private fun updateMapLocation(location: Location) {
        val point = GeoPoint(location.latitude, location.longitude)
        if (currentMarker == null) {
            currentMarker = Marker(mapView).apply {
                position = point
                setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
                title = "Current Location"
                mapView.overlays.add(this)
            }
            mapView.controller.setCenter(point)
        } else {
            currentMarker!!.position = point
        }
        if (location.hasBearing()) currentMarker!!.rotation = location.bearing
        mapView.invalidate()
    }

    // ── Destination / Routing ─────────────────────────────────────────────────
    private fun setDestination() {
        val input = etDestination.text.toString().trim()
        if (input.isEmpty()) {
            Toast.makeText(this, "Enter a destination to navigate.", Toast.LENGTH_SHORT).show()
            return
        }

        val coordRegex = Regex("""^-?\d+\.?\d*\s*,\s*-?\d+\.?\d*$""")
        if (coordRegex.matches(input)) {
            val parts = input.split(",")
            destinationLocation = GeoPoint(parts[0].trim().toDouble(), parts[1].trim().toDouble())
            fetchRoute()
        } else {
            geocodeAddress(input)
        }
    }

    private fun geocodeAddress(address: String) {
        tvStatus.text = "Finding location…"
        Thread {
            try {
                val client   = OkHttpClient()
                val url      = "https://nominatim.openstreetmap.org/search?q=$address&format=json&limit=1"
                val response = client.newCall(
                    Request.Builder().url(url).addHeader("User-Agent", packageName).build()
                ).execute()

                if (response.isSuccessful) {
                    val results = Gson().fromJson(
                        response.body?.string(), Array<NominatimResult>::class.java
                    )
                    if (results.isNotEmpty()) {
                        destinationLocation = GeoPoint(
                            results[0].lat.toDouble(), results[0].lon.toDouble()
                        )
                        runOnUiThread {
                            tvStatus.text = results[0].displayName
                            fetchRoute()
                        }
                    } else {
                        runOnUiThread {
                            Toast.makeText(
                                this,
                                "We couldn't find that address. Try a more specific search.",
                                Toast.LENGTH_SHORT
                            ).show()
                            tvStatus.text = "Ready"
                        }
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Geocode error", e)
                runOnUiThread {
                    Toast.makeText(
                        this, "Location lookup failed: ${e.message}", Toast.LENGTH_SHORT
                    ).show()
                }
            }
        }.start()
    }

    private fun fetchRoute() {
        val svc  = navService ?: return
        val curr = svc.currentLocation ?: run {
            Toast.makeText(this, "Waiting for GPS signal…", Toast.LENGTH_SHORT).show()
            return
        }
        val dest = destinationLocation ?: return
        tvStatus.text = "Calculating route…"
        fetchORSRoute(curr, dest)
    }

    private fun fetchORSRoute(current: Location, dest: GeoPoint) {
        Thread {
            try {
                val client = OkHttpClient()
                val body   = buildORSRequest(current, dest)
                    .toRequestBody("application/json".toMediaTypeOrNull())

                val response = client.newCall(
                    Request.Builder()
                        .url("https://api.openrouteservice.org/v2/directions/driving-car")
                        .post(body)
                        .addHeader("Authorization", "YOUR_ORS_API_KEY_HERE")
                        .addHeader("Content-Type", "application/json")
                        .build()
                ).execute()

                if (response.isSuccessful) {
                    val ors = Gson().fromJson(response.body?.string(), ORSResponse::class.java)
                    if (ors.routes.isNotEmpty()) {
                        val waypoints = ors.routes[0].geometry.coordinates
                            .map { GeoPoint(it[1], it[0]) }
                        runOnUiThread { onRouteReceived(waypoints, ors.routes[0].summary.distance) }
                        return@Thread
                    }
                }

                runOnUiThread {
                    Toast.makeText(
                        this,
                        "Road preference filters unavailable. Showing fastest route.",
                        Toast.LENGTH_SHORT
                    ).show()
                }
                fetchOSRMRoute(current, dest)

            } catch (e: Exception) {
                Log.e(TAG, "ORS error", e)
                fetchOSRMRoute(current, dest)
            }
        }.start()
    }

    private fun buildORSRequest(current: Location, dest: GeoPoint): String {
        val coords = listOf(
            listOf(current.longitude, current.latitude),
            listOf(dest.longitude, dest.latitude)
        )
        val options = mutableMapOf<String, Any>()
        when (selectedRoadType) {
            RoadTypePreference.AVOID_HIGHWAYS -> {
                options["avoid_features"] = listOf("highways")
            }
            RoadTypePreference.AVOID_AVENUES -> {
                options["avoid_features"] = listOf("highways")
                options["profile_params"]  = mapOf("weightings" to mapOf("green" to 0.5, "quiet" to 0.8))
            }
            RoadTypePreference.PREFER_STREETS -> {
                options["avoid_features"] = listOf("highways")
                options["profile_params"]  = mapOf("weightings" to mapOf("green" to 0.8, "quiet" to 1.0))
            }
            RoadTypePreference.RESIDENTIAL_ONLY -> {
                options["avoid_features"] = listOf("highways", "tollways")
                options["profile_params"]  = mapOf(
                    "restrictions" to mapOf("maximum_speed" to 50),
                    "weightings"   to mapOf("green" to 1.0, "quiet" to 1.0)
                )
            }
            else -> { /* ALL_ROADS — no filters */ }
        }
        val req = mutableMapOf<String, Any>("coordinates" to coords)
        if (options.isNotEmpty()) req["options"] = options
        return Gson().toJson(req)
    }

    private fun fetchOSRMRoute(current: Location, dest: GeoPoint) {
        Thread {
            try {
                val url = "http://router.project-osrm.org/route/v1/driving/" +
                        "${current.longitude},${current.latitude};" +
                        "${dest.longitude},${dest.latitude}?overview=full&geometries=geojson"

                val response = OkHttpClient()
                    .newCall(Request.Builder().url(url).build()).execute()

                if (response.isSuccessful) {
                    val osrm = Gson().fromJson(response.body?.string(), OSRMResponse::class.java)
                    if (osrm.routes.isNotEmpty()) {
                        val waypoints = osrm.routes[0].geometry.coordinates
                            .map { GeoPoint(it[1], it[0]) }
                        runOnUiThread { onRouteReceived(waypoints, osrm.routes[0].distance) }
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "OSRM error", e)
                runOnUiThread {
                    Toast.makeText(
                        this, "Couldn't calculate a route: ${e.message}", Toast.LENGTH_SHORT
                    ).show()
                    tvStatus.text = "Ready"
                }
            }
        }.start()
    }

    private fun onRouteReceived(waypoints: List<GeoPoint>, distanceMeters: Double) {
        drawRoute(waypoints)
        currentRouteWaypoints  = waypoints
        btnSaveRoute.isEnabled = true
        val km = "%.1f".format(distanceMeters / 1000)
        tvStatus.text = "$km km · ${selectedRoadType.displayName}"

        navService?.sendWaypoints(
            waypoints,
            onProgress = { done, total -> tvStatus.text = "Sending route to device ($done/$total)…" },
            onDone     = {
                tvStatus.text = "Route active · $km km"
                Toast.makeText(this, "Route sent to device", Toast.LENGTH_SHORT).show()
            }
        )
    }

    private fun drawRoute(waypoints: List<GeoPoint>) {
        routePolyline?.let { mapView.overlays.remove(it) }
        routePolyline = Polyline().apply {
            setPoints(waypoints)
            outlinePaint.color       = android.graphics.Color.BLUE
            outlinePaint.strokeWidth = 8f
            mapView.overlays.add(this)
        }
        if (destinationMarker == null) {
            destinationMarker = Marker(mapView).apply {
                position = waypoints.last()
                setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
                title = "Destination"
                mapView.overlays.add(this)
            }
        } else {
            destinationMarker!!.position = waypoints.last()
        }
        mapView.zoomToBoundingBox(BoundingBox.fromGeoPoints(waypoints), true, 100)
        mapView.invalidate()
    }

    // ── Firmware update ───────────────────────────────────────────────────────
    private fun showFirmwareDialog() {
        val dialogView = layoutInflater.inflate(R.layout.dialog_firmware, null)
        val tvFwStatus = dialogView.findViewById<TextView>(R.id.tvFwStatus)
        val progressBar = dialogView.findViewById<android.widget.ProgressBar>(R.id.fwProgressBar)
        val tvFwPercent = dialogView.findViewById<TextView>(R.id.tvFwPercent)
        val btnCheck = dialogView.findViewById<Button>(R.id.btnCheckFw)
        val btnFlash = dialogView.findViewById<Button>(R.id.btnFlashFw)

        var downloadedBytes: ByteArray? = null

        progressBar.visibility = android.view.View.GONE
        tvFwPercent.visibility = android.view.View.GONE
        btnFlash.isEnabled = false

        val connected = navService?.isConnected == true
        btnCheck.isEnabled = connected
        tvFwStatus.text = if (connected)
            "Connected · tap Check to look for updates."
        else
            "Connect your device first to check for updates."

        val dialog = AlertDialog.Builder(this)
            .setTitle("Software Update")
            .setView(dialogView)
            .setNegativeButton("Close") { _, _ ->
                navService?.onNotificationReceived = null
            }
            .create()

        dialog.show()
        val btnClose = dialog.getButton(AlertDialog.BUTTON_NEGATIVE)

        // ── Check button ──────────────────────────────────────────────────────────
        btnCheck.setOnClickListener {
            val svc = navService
            if (svc?.isConnected != true) {
                tvFwStatus.text = "Connect your device first to check for updates."
                btnCheck.isEnabled = false
                return@setOnClickListener
            }

            btnCheck.isEnabled = false
            btnFlash.isEnabled = false
            downloadedBytes = null
            progressBar.visibility = android.view.View.GONE
            tvFwPercent.visibility = android.view.View.GONE
            tvFwStatus.text = "Asking device for version…"

            svc.onNotificationReceived = null

            svc.onNotificationReceived = { payload ->
                svc.onNotificationReceived = null  // one-shot
                val deviceVersion = try {
                    com.google.gson.JsonParser.parseString(payload)
                        .asJsonObject.get("version")?.asString
                } catch (_: Exception) {
                    null
                }

                runOnUiThread {
                    if (deviceVersion == null) {
                        tvFwStatus.text = "Device did not return a valid version. Try reconnecting."
                        btnCheck.isEnabled = true
                        return@runOnUiThread
                    }

                    tvFwStatus.text = "Device is running $deviceVersion. Fetching latest version…"

                    Thread {
                        try {
                            val client = OkHttpClient()
                            val remoteVersion = client
                                .newCall(Request.Builder().url(FW_VERSION_URL).build())
                                .execute()
                                .body?.string()?.trim() ?: "unknown"

                            if (remoteVersion == deviceVersion) {
                                runOnUiThread {
                                    tvFwStatus.text = "Device is up to date ($deviceVersion)."
                                    btnCheck.isEnabled = true
                                }
                                return@Thread
                            }

                            runOnUiThread {
                                tvFwStatus.text =
                                    "$deviceVersion → $remoteVersion available. Downloading…"
                            }

                            val fwResp = client
                                .newCall(Request.Builder().url(FW_URL).build())
                                .execute()
                            if (!fwResp.isSuccessful) throw Exception("Server returned ${fwResp.code}")

                            val body =
                                fwResp.body ?: throw Exception("No data received from server")
                            val totalBytes = body.contentLength()
                            val totalKib = totalBytes / 1024
                            val buffer = ByteArray(totalBytes.toInt())
                            val stream = body.byteStream()
                            var read = 0

                            runOnUiThread {
                                progressBar.max = 100
                                progressBar.progress = 0
                                progressBar.visibility = android.view.View.VISIBLE
                                tvFwPercent.visibility = android.view.View.VISIBLE
                            }

                            while (read < buffer.size) {
                                val n = stream.read(buffer, read, buffer.size - read)
                                if (n < 0) break
                                read += n
                                val pct =
                                    if (totalBytes > 0) (read * 100L / totalBytes).toInt() else 0
                                val kibRead = read / 1024
                                runOnUiThread {
                                    progressBar.progress = pct
                                    tvFwPercent.text = "$pct% · $kibRead / $totalKib KiB"
                                }
                            }

                            downloadedBytes = buffer

                            runOnUiThread {
                                tvFwStatus.text =
                                    "$deviceVersion → $remoteVersion · ${buffer.size / 1024} KiB ready to install."
                                btnCheck.isEnabled = true
                                btnFlash.isEnabled = svc.isConnected
                            }

                        } catch (e: Exception) {
                            Log.e(TAG, "Firmware download failed", e)
                            runOnUiThread {
                                tvFwStatus.text = "Download failed: ${e.message}"
                                btnCheck.isEnabled = true
                            }
                        }
                    }.start()
                }
            }

            svc.writeRaw("""{"type":"version"}""")

            Handler(Looper.getMainLooper()).postDelayed({
                if (svc.onNotificationReceived != null) {
                    svc.onNotificationReceived = null
                    tvFwStatus.text = "Device did not respond. Try reconnecting."
                    btnCheck.isEnabled = true
                }
            }, 3_000)
        }

        // ── Flash button ──────────────────────────────────────────────────────────
        btnFlash.setOnClickListener {
            val fw = downloadedBytes ?: return@setOnClickListener
            val svc = navService ?: return@setOnClickListener
            if (!svc.isConnected) {
                Toast.makeText(
                    this,
                    "Device disconnected. Reconnect and try again.",
                    Toast.LENGTH_SHORT
                ).show()
                btnFlash.isEnabled = false
                return@setOnClickListener
            }

            AlertDialog.Builder(this)
                .setTitle("Install Update?")
                .setMessage(
                    "Your device will restart after installation. " +
                            "Keep the app open and stay within Bluetooth range until the process completes."
                )
                .setPositiveButton("Install") { _, _ ->
                    btnFlash.isEnabled = false
                    btnCheck.isEnabled = false
                    btnClose.isEnabled = false
                    tvFwStatus.text = "Installing update…"
                    progressBar.progress = 0
                    flashFirmware(fw, svc, progressBar, tvFwPercent, tvFwStatus, btnClose)
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
    }

    /**
     * Transfer firmware over BLE using confirmed, MTU-aware writes.
     *
     * - Chunk size is derived from the negotiated MTU so JSON packets always
     *   fit in one BLE write without fragmentation.
     * - Each write is confirmed via [otaWriteSemaphore] before the next is
     *   sent — this is the correct BLE pacing mechanism instead of sleep().
     * - The start packet is confirmed before any chunk is queued.
     * - A timeout per chunk aborts cleanly rather than stalling silently.
     */
    private fun flashFirmware(
        fw: ByteArray,
        svc: NavService,
        progressBar: android.widget.ProgressBar,
        tvPercent: TextView,
        tvStatus: TextView,
        btnClose: Button
    ) {
        val mtu         = svc.mtuSize.coerceAtLeast(23)
        val usable      = mtu - 3
        val chunkSize   = ((usable - JSON_ENVELOPE_OVERHEAD) * 3 / 4)
            .coerceIn(1, FIRMWARE_CHUNK_BYTES)
        val totalChunks = (fw.size + chunkSize - 1) / chunkSize
        val totalKib    = fw.size / 1024

        Log.d(TAG, "OTA start — MTU=$mtu usable=$usable chunkSize=$chunkSize totalChunks=$totalChunks fwSize=${fw.size}")

        otaWriteSemaphore.drainPermits()

        fun abortWithError(msg: String) {
            Log.e(TAG, "OTA abort: $msg")
            runOnUiThread {
                tvStatus.text      = msg
                btnClose.isEnabled = true   // let the user close on error
            }
        }

        Thread {
            try {
                runOnUiThread {
                    tvStatus.text        = "Preparing update · $totalKib KiB"
                    progressBar.max      = totalChunks
                    progressBar.progress = 0
                }

                svc.writeRaw("""{"type":"start_fw_update","chunkCount":$totalChunks}""")
                if (!otaWriteSemaphore.tryAcquire(WRITE_CONFIRM_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                    abortWithError("Connection timed out. Make sure your device is in range and try again.")
                    return@Thread
                }

                Thread.sleep(100)

                for (i in 0 until totalChunks) {
                    val offset    = i * chunkSize
                    val slice     = fw.copyOfRange(offset, minOf(offset + chunkSize, fw.size))
                    val b64       = Base64.encodeToString(slice, Base64.NO_WRAP)
                    val chunkJson = """{"type":"fw_chunk","index":$i,"size":${slice.size},"data":"$b64"}"""

                    if (chunkJson.length > usable) {
                        Log.w(TAG, "Block $i JSON length ${chunkJson.length} > usable $usable — may fragment")
                    }

                    svc.writeRaw(chunkJson)

                    if (!otaWriteSemaphore.tryAcquire(WRITE_CONFIRM_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                        abortWithError("Installation interrupted at block $i. Please reconnect and try again.")
                        return@Thread
                    }

                    val bytesSent = minOf(offset + chunkSize, fw.size)
                    val kibSent   = bytesSent / 1024
                    val pct       = ((i + 1) * 100) / totalChunks

                    runOnUiThread {
                        progressBar.progress = i + 1
                        tvPercent.text       = "$pct% · $kibSent / $totalKib KiB"
                    }
                }

                runOnUiThread {
                    progressBar.progress = totalChunks
                    tvPercent.text       = "100% · $totalKib / $totalKib KiB"
                    tvStatus.text        = "Update transferred. Your device is applying the changes and will restart shortly."
                    // Close stays disabled — device is rebooting, nothing useful to do
                }

            } catch (e: Exception) {
                Log.e(TAG, "OTA error", e)
                abortWithError("Installation failed: ${e.message}")
            }
        }.start()
    }

    // ── BLE scanning / connecting ─────────────────────────────────────────────
    private fun scanAndConnect() {
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN)
            != PackageManager.PERMISSION_GRANTED) return

        val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        if (!adapter.isEnabled) { promptEnableBluetooth(); return }

        val scanner = adapter.bluetoothLeScanner
        tvStatus.text        = "Looking for your device…"
        btnConnect.isEnabled = false

        val scanCb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                if (ActivityCompat.checkSelfPermission(
                        this@MainActivity, Manifest.permission.BLUETOOTH_CONNECT
                    ) != PackageManager.PERMISSION_GRANTED) return

                val name = result.device.name
                val addr = result.device.address
                Log.d(TAG, "Found BLE device: $name — $addr")

                val isTarget = name == "ESP32_Speedometer" ||
                        (name == null && (
                                addr.startsWith("24:") ||
                                        addr.startsWith("30:") ||
                                        addr.startsWith("7C:")
                                ))

                if (isTarget) {
                    scanner.stopScan(this)
                    runOnUiThread { tvStatus.text = "Connecting…" }
                    connectToDevice(result.device)
                }
            }

            override fun onScanFailed(errorCode: Int) {
                runOnUiThread {
                    tvStatus.text        = "Scan failed (code $errorCode). Check Bluetooth permissions."
                    btnConnect.isEnabled = true
                }
            }
        }

        scanner.startScan(
            null,
            android.bluetooth.le.ScanSettings.Builder()
                .setScanMode(android.bluetooth.le.ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build(),
            scanCb
        )

        Handler(Looper.getMainLooper()).postDelayed({
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN)
                == PackageManager.PERMISSION_GRANTED) {
                scanner.stopScan(scanCb)
                if (navService?.isConnected != true) {
                    runOnUiThread {
                        tvStatus.text        = "Device not found. Make sure it's powered on and nearby."
                        btnConnect.isEnabled = true
                    }
                }
            }
        }, BLE_SCAN_TIMEOUT_MS)
    }

    private fun connectToDevice(device: BluetoothDevice) {
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
            != PackageManager.PERMISSION_GRANTED) return

        navService?.bluetoothGatt?.close()
        navService?.bluetoothGatt = null

        Handler(Looper.getMainLooper()).postDelayed({
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) return@postDelayed

            val gatt = device.connectGatt(this, false, object : BluetoothGattCallback() {

                override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
                    Log.d(TAG, "BLE state change: status=$status newState=$newState")
                    when (newState) {
                        BluetoothProfile.STATE_CONNECTED -> {
                            if (status == BluetoothGatt.GATT_SUCCESS) {
                                navService?.isConnected = true
                                runOnUiThread { tvStatus.text = "Connected. Configuring link…" }
                                Handler(Looper.getMainLooper()).postDelayed({
                                    if (ActivityCompat.checkSelfPermission(
                                            this@MainActivity,
                                            Manifest.permission.BLUETOOTH_CONNECT
                                        ) == PackageManager.PERMISSION_GRANTED
                                    ) gatt.requestMtu(BLE_REQUEST_MTU)
                                }, 600)
                            } else {
                                runOnUiThread {
                                    tvStatus.text        = "Could not connect. Please try again."
                                    btnConnect.isEnabled = true
                                    setDotState(DotState.ERROR)
                                }
                                gatt.close()
                            }
                        }
                        BluetoothProfile.STATE_DISCONNECTED -> {
                            navService?.isConnected    = false
                            navService?.bluetoothGatt  = null
                            navService?.characteristic = null
                            // Unblock any OTA thread waiting on a write confirm
                            otaWriteSemaphore.release()
                            runOnUiThread {
                                tvStatus.text               = "Device disconnected"
                                btnStartSending.isEnabled   = false
                                btnSetDestination.isEnabled = true
                                btnConnect.isEnabled        = true
                                if (navService?.isSending == true) navService?.stopSending()
                                btnStartSending.text = "Start Nav."
                                setDotState(DotState.IDLE)
                            }
                            gatt.close()
                        }
                    }
                }

                override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        navService?.mtuSize = mtu
                        Log.d(TAG, "MTU negotiated: $mtu bytes")
                    }
                    runOnUiThread { tvStatus.text = "Optimizing connection…" }
                    Handler(Looper.getMainLooper()).postDelayed({
                        if (ActivityCompat.checkSelfPermission(
                                this@MainActivity, Manifest.permission.BLUETOOTH_CONNECT
                            ) == PackageManager.PERMISSION_GRANTED
                        ) gatt.discoverServices()
                    }, BLE_MTU_DISCOVER_DELAY)
                }

                override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        val char = gatt.getService(SERVICE_UUID)?.getCharacteristic(CHAR_UUID)
                        if (char != null) {
                            navService?.bluetoothGatt  = gatt
                            navService?.characteristic = char

                            // Subscribe to notifications so we receive the version reply
                            gatt.setCharacteristicNotification(char, true)
                            val descriptor = char.getDescriptor(
                                UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
                            )
                            if (descriptor != null) {
                                descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                                gatt.writeDescriptor(descriptor)
                            }

                            runOnUiThread {
                                tvStatus.text               = "Device ready"
                                btnStartSending.isEnabled   = true
                                btnSetDestination.isEnabled = true
                                btnConnect.isEnabled        = false
                                setDotState(DotState.CONNECTED)
                            }
                        } else {
                            Log.e(TAG, "Required service or characteristic not found")
                            runOnUiThread {
                                tvStatus.text        = "Incompatible device firmware. Please update."
                                btnConnect.isEnabled = true
                                setDotState(DotState.ERROR)
                            }
                        }
                    } else {
                        runOnUiThread {
                            tvStatus.text        = "Service discovery failed. Try reconnecting."
                            btnConnect.isEnabled = true
                            setDotState(DotState.ERROR)
                        }
                    }
                }
                override fun onCharacteristicChanged(
                    gatt: BluetoothGatt,
                    characteristic: BluetoothGattCharacteristic
                ) {
                    val payload = characteristic.value?.toString(Charsets.UTF_8) ?: return
                    Log.d(TAG, "BLE notify received: $payload")
                    navService?.onNotificationReceived?.invoke(payload)
                }
                override fun onCharacteristicWrite(
                    gatt: BluetoothGatt,
                    characteristic: BluetoothGattCharacteristic,
                    status: Int
                ) {
                    val success = status == BluetoothGatt.GATT_SUCCESS
                    if (!success) Log.e(TAG, "Characteristic write failed: status=$status")
                    navService?.onWriteComplete?.invoke(success)
                }
            }, BluetoothDevice.TRANSPORT_LE)

            navService?.bluetoothGatt = gatt

        }, BLE_CONNECT_DELAY_MS)
    }

    private fun toggleSending() {
        val svc = navService ?: return
        if (svc.isSending) {
            svc.stopSending()
            btnStartSending.text = "Start Nav."
        } else {
            svc.startSending()
            btnStartSending.text = "Stop Nav."
        }
    }

    // ── Route save / load ─────────────────────────────────────────────────────
    private fun ensureRoutesDir() {
        File(filesDir, ROUTES_DIR).mkdirs()
    }

    private fun showSaveRouteDialog() {
        val waypoints = currentRouteWaypoints
        if (waypoints.isNullOrEmpty()) {
            Toast.makeText(this, "Record a route first before saving.", Toast.LENGTH_SHORT).show()
            return
        }

        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            hint = "Route name"
        }
        AlertDialog.Builder(this)
            .setTitle("Save Route")
            .setView(input)
            .setPositiveButton("Save") { _, _ ->
                val alias = input.text.toString().trim()
                if (alias.isEmpty())
                    Toast.makeText(this, "Please enter a name for this route.", Toast.LENGTH_SHORT).show()
                else
                    saveRoute(alias, waypoints)
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun saveRoute(alias: String, waypoints: List<GeoPoint>) {
        try {
            val data = SavedRoute(
                alias              = alias,
                waypoints          = waypoints.map { WaypointData(it.latitude, it.longitude) },
                savedDate          = System.currentTimeMillis(),
                roadTypePreference = selectedRoadType.name
            )
            File(File(filesDir, ROUTES_DIR), "$alias$ROUTE_EXT").writeText(Gson().toJson(data))
            Toast.makeText(this, "\"$alias\" saved", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Toast.makeText(this, "Couldn't save route: ${e.message}", Toast.LENGTH_SHORT).show()
        }
    }

    private fun showLoadRouteDialog() {
        val dir   = File(filesDir, ROUTES_DIR)
        val files = dir.listFiles { f -> f.extension == ROUTE_EXT.removePrefix(".") }
            ?.sortedByDescending { it.lastModified() }

        if (files.isNullOrEmpty()) {
            Toast.makeText(this, "No saved routes yet.", Toast.LENGTH_SHORT).show()
            return
        }

        AlertDialog.Builder(this)
            .setTitle("Load Route")
            .setItems(files.map { it.nameWithoutExtension }.toTypedArray()) { _, idx ->
                loadRoute(files[idx])
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun loadRoute(file: File) {
        try {
            val data      = Gson().fromJson(file.readText(), SavedRoute::class.java)
            val waypoints = data.waypoints.map { GeoPoint(it.latitude, it.longitude) }
            drawRoute(waypoints)
            currentRouteWaypoints  = waypoints
            btnSaveRoute.isEnabled = true

            val svc = navService
            val km  = if (svc != null) {
                "%.1f".format(
                    waypoints.zipWithNext().sumOf { (a, b) -> svc.calcDistance(a, b) } / 1000
                )
            } else "?"

            tvStatus.text = "${data.alias} · $km km"
            Toast.makeText(this, "Route loaded", Toast.LENGTH_SHORT).show()

            svc?.sendWaypoints(
                waypoints,
                onProgress = { done, total -> tvStatus.text = "Sending route to device ($done/$total)…" },
                onDone     = { tvStatus.text = "Route active · $km km" }
            )
        } catch (e: Exception) {
            Toast.makeText(this, "Couldn't load route: ${e.message}", Toast.LENGTH_SHORT).show()
        }
    }

    // ── Data classes ──────────────────────────────────────────────────────────
    data class NominatimResult(
        val lat: String,
        val lon: String,
        @SerializedName("display_name") val displayName: String
    )

    data class OSRMResponse(val routes: List<OSRMRoute>)
    data class OSRMRoute(val distance: Double, val geometry: Geometry)
    data class Geometry(val coordinates: List<List<Double>>)

    data class ORSResponse(val routes: List<ORSRoute>)
    data class ORSRoute(val summary: ORSSummary, val geometry: ORSGeometry)
    data class ORSSummary(val distance: Double, val duration: Double)
    data class ORSGeometry(val coordinates: List<List<Double>>)

    data class WaypointData(val latitude: Double, val longitude: Double)
    data class SavedRoute(
        val alias: String,
        val waypoints: List<WaypointData>,
        val savedDate: Long,
        val roadTypePreference: String? = null
    )
}