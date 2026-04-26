package com.tahiel.navigation

import android.Manifest
import android.app.*
import android.bluetooth.*
import android.content.*
import android.content.pm.PackageManager
import android.location.Location
import android.os.*
import android.util.Log
import androidx.annotation.RequiresPermission
import androidx.core.app.NotificationCompat
import com.google.android.gms.location.*
import com.google.gson.Gson
import org.osmdroid.util.GeoPoint
import kotlin.math.*

class NavService : Service() {

    companion object {
        private const val TAG = "NavService"
        const val CHANNEL_ID = "nav_channel"
        const val NOTIF_ID   = 1

        const val ACTION_START_SENDING = "START_SENDING"
        const val ACTION_STOP_SENDING  = "STOP_SENDING"
        const val ACTION_STOP          = "STOP"

        private const val GPS_UPDATE_INTERVAL_MS = 500L
        private const val SEND_INTERVAL_MS       = 500L
        private const val WAYPOINT_CHUNK_SIZE    = 10
        private const val WAYPOINT_CHUNK_DELAY   = 250L
        private const val ROUTE_START_DELAY      = 100L
    }

    // ── BLE ───────────────────────────────────────────────────────────────────
    var bluetoothGatt: BluetoothGatt? = null
    var characteristic: BluetoothGattCharacteristic? = null
    var isConnected = false
    var mtuSize = 23

    // ── GPS ───────────────────────────────────────────────────────────────────
    var currentLocation: Location? = null
    var gpsSpeed   = 0f
    var gpsHeading = 0f

    // ── State ─────────────────────────────────────────────────────────────────
    var isSending      = false
    var isTransmitting = false

    // ── Callbacks (set by MainActivity) ───────────────────────────────────────
    var onWriteComplete: ((Boolean) -> Unit)? = null
    var onLocationChanged: ((Location) -> Unit)? = null
    var onNotificationReceived: ((String) -> Unit)? = null

    // ── Internals ─────────────────────────────────────────────────────────────
    private lateinit var fusedLocationClient: FusedLocationProviderClient
    private lateinit var locationCallback: LocationCallback
    private val handler = Handler(Looper.getMainLooper())

    private val sendRunnable = object : Runnable {
        override fun run() {
            if (isSending && isConnected) {
                sendGpsData()
                handler.postDelayed(this, SEND_INTERVAL_MS)
            }
        }
    }

    // ── Binder ────────────────────────────────────────────────────────────────
    inner class LocalBinder : Binder() {
        fun getService(): NavService = this@NavService
    }

    private val binder = LocalBinder()
    override fun onBind(intent: Intent): IBinder = binder

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(NOTIF_ID, buildNotification("Running in background"))
        setupLocation()
        Log.d(TAG, "NavService started")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_SENDING -> startSending()
            ACTION_STOP_SENDING  -> stopSending()
            ACTION_STOP          -> stopSelf()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        super.onDestroy()
        stopSending()
        runCatching { fusedLocationClient.removeLocationUpdates(locationCallback) }
        Log.d(TAG, "NavService stopped")
    }

    // ── BLE write (raw, no framing) ───────────────────────────────────────────
    fun writeRaw(json: String) {
        val char = characteristic ?: return
        val gatt = bluetoothGatt  ?: return
        char.value = json.toByteArray(Charsets.UTF_8)
        if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
            == PackageManager.PERMISSION_GRANTED
        ) {
            gatt.writeCharacteristic(char)
        }
    }

    // ── Location ──────────────────────────────────────────────────────────────
    private fun setupLocation() {
        fusedLocationClient = LocationServices.getFusedLocationProviderClient(this)

        locationCallback = object : LocationCallback() {
            override fun onLocationResult(result: LocationResult) {
                result.lastLocation?.let { loc ->
                    currentLocation = loc
                    gpsSpeed        = loc.speed
                    if (loc.hasBearing()) gpsHeading = loc.bearing
                    onLocationChanged?.invoke(loc)
                    updateNotification(
                        "%.5f, %.5f  ·  %.0f km/h".format(
                            loc.latitude, loc.longitude, gpsSpeed * 3.6f
                        )
                    )
                }
            }
        }

        try {
            val req = LocationRequest.Builder(Priority.PRIORITY_HIGH_ACCURACY, GPS_UPDATE_INTERVAL_MS).build()
            fusedLocationClient.requestLocationUpdates(req, locationCallback, Looper.getMainLooper())
        } catch (e: SecurityException) {
            Log.e(TAG, "Location permission missing", e)
        }
    }

    // ── Sending toggle ────────────────────────────────────────────────────────
    fun startSending() {
        isSending = true
        handler.post(sendRunnable)
        Log.d(TAG, "GPS data stream started")
    }

    fun stopSending() {
        isSending = false
        handler.removeCallbacks(sendRunnable)
        Log.d(TAG, "GPS data stream stopped")
    }

    // ── GPS → BLE ─────────────────────────────────────────────────────────────
    private fun sendGpsData() {
        val loc = currentLocation ?: return
        val payload = mapOf(
            "coordinates" to mapOf(
                "latitude"  to loc.latitude,
                "longitude" to loc.longitude
            ),
            "speed"   to gpsSpeed,
            "heading" to gpsHeading
        )
        sendBLE(Gson().toJson(payload))
    }

    @RequiresPermission(Manifest.permission.BLUETOOTH_CONNECT)
    fun sendBLE(json: String) {
        if (!isConnected || characteristic == null) return
        try {
            val bytes      = json.toByteArray()
            val maxPayload = (mtuSize - 3).coerceAtMost(512)
            characteristic!!.value     = bytes
            characteristic!!.writeType = if (bytes.size <= maxPayload)
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            else
                BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            bluetoothGatt?.writeCharacteristic(characteristic)
        } catch (e: Exception) {
            Log.e(TAG, "BLE write error", e)
        }
    }

    // ── Waypoint transmission ─────────────────────────────────────────────────
    fun sendWaypoints(
        waypoints: List<GeoPoint>,
        onProgress: (Int, Int) -> Unit,
        onDone: () -> Unit
    ) {
        if (!isConnected || isTransmitting) return
        isTransmitting = true

        Thread {
            try {
                val totalChunks = (waypoints.size + WAYPOINT_CHUNK_SIZE - 1) / WAYPOINT_CHUNK_SIZE
                Log.d(TAG, "Sending ${waypoints.size} waypoints in $totalChunks chunks")

                sendBLE(Gson().toJson(mapOf(
                    "type"           to "route_start",
                    "totalWaypoints" to waypoints.size,
                    "totalChunks"    to totalChunks,
                    "chunkSize"      to WAYPOINT_CHUNK_SIZE
                )))
                Thread.sleep(ROUTE_START_DELAY)

                for (i in 0 until totalChunks) {
                    val start = i * WAYPOINT_CHUNK_SIZE
                    val end   = minOf(start + WAYPOINT_CHUNK_SIZE, waypoints.size)
                    val chunk = waypoints.subList(start, end)

                    sendBLE(Gson().toJson(mapOf(
                        "type"       to "route_chunk",
                        "chunkIndex" to i,
                        "waypoints"  to chunk.map { mapOf("lat" to it.latitude, "lon" to it.longitude) }
                    )))

                    handler.post { onProgress(i + 1, totalChunks) }
                    Thread.sleep(WAYPOINT_CHUNK_DELAY)
                }

                sendBLE(Gson().toJson(mapOf(
                    "type"           to "route_complete",
                    "totalWaypoints" to waypoints.size
                )))

                Log.d(TAG, "Waypoint transmission complete")
                handler.post { onDone() }

            } catch (e: Exception) {
                Log.e(TAG, "Waypoint send error", e)
            } finally {
                isTransmitting = false
            }
        }.start()
    }

    // ── Haversine distance ────────────────────────────────────────────────────
    fun calcDistance(p1: GeoPoint, p2: GeoPoint): Double {
        val R    = 6_371_000.0
        val lat1 = Math.toRadians(p1.latitude)
        val lat2 = Math.toRadians(p2.latitude)
        val dLat = Math.toRadians(p2.latitude  - p1.latitude)
        val dLon = Math.toRadians(p2.longitude - p1.longitude)
        val a = sin(dLat / 2).pow(2) + cos(lat1) * cos(lat2) * sin(dLon / 2).pow(2)
        return R * 2 * atan2(sqrt(a), sqrt(1 - a))
    }

    // ── Notification ──────────────────────────────────────────────────────────
    private fun createNotificationChannel() {
        val ch = NotificationChannel(
            CHANNEL_ID,
            "Tahvia Navigation",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Maintains GPS and device connection while the screen is off"
        }
        getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
    }

    private fun buildNotification(text: String): Notification {
        val stopIntent = PendingIntent.getService(
            this, 0,
            Intent(this, NavService::class.java).apply { action = ACTION_STOP },
            PendingIntent.FLAG_IMMUTABLE
        )
        val openIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Tahvia Navigation")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_mylocation)
            .setContentIntent(openIntent)
            .addAction(android.R.drawable.ic_delete, "Stop", stopIntent)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java)
            .notify(NOTIF_ID, buildNotification(text))
    }
}