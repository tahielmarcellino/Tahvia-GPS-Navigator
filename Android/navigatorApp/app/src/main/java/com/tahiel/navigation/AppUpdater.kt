package com.tahiel.navigation

import android.app.DownloadManager
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.core.content.FileProvider
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File

/**
 * AppUpdater
 *
 * Mirrors the firmware OTA pattern in MainActivity but targets the Android APK.
 *
 * Version check:
 *   README.md line 0 → firmware version  (ignored here)
 *   README.md line 1 → app version       (compared against R.string.app_version)
 *
 * The installed version is read from res/values/strings.xml:
 *   <string name="app_version">1.0.0</string>
 *
 * This is the single source of truth — update it there when you ship a new build.
 *
 * Download:
 *   Streams the APK via DownloadManager so the OS handles progress, Wi-Fi vs mobile,
 *   and storage permissions automatically.
 *
 * Install:
 *   Prompts via ACTION_VIEW + FileProvider once the download is complete.
 *   Requires  <uses-permission android:name="android.permission.REQUEST_INSTALL_PACKAGES"/>
 *   and a FileProvider authority declared in AndroidManifest.xml.
 *
 * Usage (from MainActivity):
 *   AppUpdater(this).showDialog()
 */
class AppUpdater(private val context: Context) {

    companion object {
        private const val TAG = "AppUpdater"

        private const val README_URL =
            "https://raw.githubusercontent.com/tahielmarcellino/Tahvia-GPS-Navigator/refs/heads/main/README.md"

        private const val APK_URL =
            "https://github.com/tahielmarcellino/Tahvia-GPS-Navigator/raw/refs/heads/main/Android/navigatorApp/app/release/app-release.apk"

        private const val APK_FILE_NAME = "tahvia-update.apk"

        /**
         * FileProvider authority — must match AndroidManifest.xml exactly.
         * Example manifest entry:
         *
         *   <provider
         *       android:name="androidx.core.content.FileProvider"
         *       android:authorities="${applicationId}.provider"
         *       android:exported="false"
         *       android:grantUriPermissions="true">
         *       <meta-data
         *           android:name="android.support.FILE_PROVIDER_PATHS"
         *           android:resource="@xml/file_paths" />
         *   </provider>
         *
         * And res/xml/file_paths.xml:
         *   <paths>
         *       <external-files-path name="apk_downloads" path="." />
         *   </paths>
         */
        private const val FILE_PROVIDER_AUTHORITY = "com.tahiel.navigation.provider"
    }

    // ── Installed version ─────────────────────────────────────────────────────
    //
    // Sourced from res/values/strings.xml → <string name="app_version">1.0.0</string>
    // This is the single source of truth. Update it there when you ship a new build.
    // Do NOT fall back to packageManager — versionName in build.gradle and the string
    // resource can drift; keeping one authoritative location prevents silent mismatches.
    //
    private val installedVersion: String
        get() = context.getString(R.string.app_version)

    // ── Public entry point ────────────────────────────────────────────────────
    fun showDialog() {
        val dialogView = android.view.LayoutInflater.from(context)
            .inflate(R.layout.dialog_app_updater, null)

        val tvStatus    = dialogView.findViewById<TextView>(R.id.tvAppUpdateStatus)
        val progressBar = dialogView.findViewById<ProgressBar>(R.id.appUpdateProgressBar)
        val tvPercent   = dialogView.findViewById<TextView>(R.id.tvAppUpdatePercent)
        val btnCheck    = dialogView.findViewById<Button>(R.id.btnCheckAppUpdate)
        val btnDownload = dialogView.findViewById<Button>(R.id.btnDownloadApp)

        progressBar.visibility = View.GONE
        tvPercent.visibility   = View.GONE
        btnDownload.isEnabled  = false

        tvStatus.text = "Installed: $installedVersion\nTap Check to look for updates."

        var remoteVersion: String? = null
        var downloadId: Long       = -1L

        val dialog = AlertDialog.Builder(context)
            .setTitle("App Update")
            .setView(dialogView)
            .setNegativeButton("Close", null)
            .create()

        dialog.show()

        // ── Check ─────────────────────────────────────────────────────────────
        btnCheck.setOnClickListener {
            btnCheck.isEnabled     = false
            btnDownload.isEnabled  = false
            remoteVersion          = null
            progressBar.visibility = View.GONE
            tvPercent.visibility   = View.GONE
            tvStatus.text          = "Checking for updates…"

            Thread {
                try {
                    val client   = OkHttpClient()
                    val response = client.newCall(
                        Request.Builder().url(README_URL).build()
                    ).execute()

                    if (!response.isSuccessful) throw Exception("Server returned ${response.code}")

                    val lines = response.body?.string()
                        ?.lines()
                        ?.map { it.trim() }
                        ?.filter { it.isNotEmpty() }
                        ?: emptyList()

                    // Line 0 = firmware version (used by the firmware dialog in MainActivity)
                    // Line 1 = app version
                    val appLine = lines.getOrNull(1)
                        ?: throw Exception("App version not found in README")

                    remoteVersion = appLine
                    val installed = installedVersion

                    runOnUi {
                        btnCheck.isEnabled = true
                        if (remoteVersion == installed) {
                            tvStatus.text = "You're up to date ($installed)."
                        } else {
                            tvStatus.text =
                                "Update available!\n\nInstalled: $installed\nLatest:    $remoteVersion"
                            btnDownload.isEnabled = true
                        }
                    }

                } catch (e: Exception) {
                    Log.e(TAG, "Version check failed", e)
                    runOnUi {
                        tvStatus.text      = "Check failed: ${e.message}"
                        btnCheck.isEnabled = true
                    }
                }
            }.start()
        }

        // ── Download & install ────────────────────────────────────────────────
        btnDownload.setOnClickListener {
            btnDownload.isEnabled       = false
            btnCheck.isEnabled          = false
            tvStatus.text               = "Downloading update…"
            progressBar.visibility      = View.VISIBLE
            progressBar.isIndeterminate = true
            tvPercent.visibility        = View.VISIBLE
            tvPercent.text              = "Starting download…"

            // Clean up any previous download
            val destFile = File(
                context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS),
                APK_FILE_NAME
            )
            if (destFile.exists()) destFile.delete()

            val request = DownloadManager.Request(Uri.parse(APK_URL)).apply {
                setTitle("Tahvia Update")
                setDescription("$remoteVersion")
                setNotificationVisibility(
                    DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED
                )
                setDestinationInExternalFilesDir(
                    context,
                    Environment.DIRECTORY_DOWNLOADS,
                    APK_FILE_NAME
                )
                setMimeType("application/vnd.android.package-archive")
            }

            val dm = context.getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
            downloadId = dm.enqueue(request)

            // Poll progress on a daemon thread — no leaked reference; dialog owns the views
            val pollThread = Thread {
                var running = true
                while (running) {
                    Thread.sleep(500)
                    val cur = dm.query(DownloadManager.Query().setFilterById(downloadId))
                    if (cur != null && cur.moveToFirst()) {
                        val status     = cur.getInt(cur.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS))
                        val downloaded = cur.getLong(cur.getColumnIndexOrThrow(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR))
                        val total      = cur.getLong(cur.getColumnIndexOrThrow(DownloadManager.COLUMN_TOTAL_SIZE_BYTES))
                        cur.close()

                        when (status) {
                            DownloadManager.STATUS_RUNNING -> {
                                val pct    = if (total > 0) (downloaded * 100 / total).toInt() else 0
                                val kibDl  = downloaded / 1024
                                val kibTot = total / 1024
                                runOnUi {
                                    progressBar.isIndeterminate = false
                                    progressBar.max      = 100
                                    progressBar.progress = pct
                                    tvPercent.text       = "$pct% · $kibDl / $kibTot KiB"
                                }
                            }
                            DownloadManager.STATUS_SUCCESSFUL -> {
                                running = false
                                runOnUi {
                                    progressBar.progress = 100
                                    tvPercent.text       = "Download complete"
                                    tvStatus.text        = "Download finished. Installing…"
                                    installApk(destFile)
                                }
                            }
                            DownloadManager.STATUS_FAILED -> {
                                running = false
                                val reason = cur.getInt(
                                    cur.getColumnIndexOrThrow(DownloadManager.COLUMN_REASON)
                                )
                                runOnUi {
                                    tvStatus.text          = "Download failed (reason $reason). Try again."
                                    btnCheck.isEnabled     = true
                                    progressBar.visibility = View.GONE
                                    tvPercent.visibility   = View.GONE
                                }
                            }
                            DownloadManager.STATUS_PAUSED -> {
                                runOnUi { tvPercent.text = "Download paused…" }
                            }
                        }
                    }
                }
            }
            pollThread.isDaemon = true
            pollThread.start()
        }
    }

    // ── Install helper ────────────────────────────────────────────────────────
    private fun installApk(apkFile: File) {
        // On Android 8+ ensure "Install unknown apps" is granted for this app
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            if (!context.packageManager.canRequestPackageInstalls()) {
                val intent = android.content.Intent(
                    android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:${context.packageName}")
                )
                intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                context.startActivity(intent)
                return
            }
        }

        // Wait until the file size stops changing (DownloadManager may still be flushing)
        Thread {
            try {
                var lastSize = -1L
                var stableCount = 0
                var attempts = 0
                val maxAttempts = 30 // 30 × 300ms = 9 seconds max wait

                while (stableCount < 3 && attempts < maxAttempts) {
                    Thread.sleep(300)
                    val currentSize = apkFile.length()
                    Log.d(TAG, "APK size check #$attempts: $currentSize bytes")

                    if (currentSize > 0 && currentSize == lastSize) {
                        stableCount++
                    } else {
                        stableCount = 0
                    }
                    lastSize = currentSize
                    attempts++
                }

                if (!apkFile.exists() || apkFile.length() == 0L) {
                    Log.e(TAG, "APK file missing or empty after wait: ${apkFile.absolutePath}")
                    return@Thread
                }

                Log.d(TAG, "APK ready to install: ${apkFile.absolutePath} (${apkFile.length()} bytes)")

                runOnUi {
                    try {
                        val uri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                            FileProvider.getUriForFile(context, FILE_PROVIDER_AUTHORITY, apkFile)
                        } else {
                            @Suppress("DEPRECATION")
                            Uri.fromFile(apkFile)
                        }

                        val install = android.content.Intent(android.content.Intent.ACTION_VIEW).apply {
                            setDataAndType(uri, "application/vnd.android.package-archive")
                            addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
                            addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                        }
                        context.startActivity(install)

                    } catch (e: Exception) {
                        Log.e(TAG, "Install failed: ${e.message}", e)
                    }
                }

            } catch (e: Exception) {
                Log.e(TAG, "Error while waiting for APK: ${e.message}", e)
            }
        }.also { it.isDaemon = true }.start()
    }

    // ── UI thread helper ──────────────────────────────────────────────────────
    private fun runOnUi(block: () -> Unit) {
        android.os.Handler(android.os.Looper.getMainLooper()).post(block)
    }
}