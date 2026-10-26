package net.vd.yansi

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.Collections
import kotlin.math.roundToInt

class MainActivity : Activity(), SurfaceHolder.Callback, StreamClient.Listener {

    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var info: TextView
    private lateinit var hud: TextView

    private var client: StreamClient? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var hudVisible = false
    private var videoW = 0
    private var videoH = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= 28) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }

        surfaceView = SurfaceView(this)
        root.addView(
            surfaceView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
                Gravity.CENTER,
            ),
        )

        info = TextView(this).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            gravity = Gravity.CENTER
            setLineSpacing(0f, 1.2f)
        }
        root.addView(
            info,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER,
            ),
        )

        hud = TextView(this).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            typeface = Typeface.MONOSPACE
            setBackgroundColor(0x99000000.toInt())
            val p = dp(8)
            setPadding(p, p, p, p)
            visibility = View.GONE
        }
        root.addView(
            hud,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            ).apply { setMargins(dp(12), dp(12), 0, 0) },
        )

        setContentView(root)
        root.setOnClickListener { toggleHud() }
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> root.post { fitVideo() } }
        surfaceView.holder.addCallback(this)
        showIdle()
        hideSystemBars()
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
        acquireWifiLock()
    }

    override fun onPause() {
        wifiLock?.let { if (it.isHeld) it.release() }
        super.onPause()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    // ------------------------------------------------------------ yüzey

    override fun surfaceCreated(holder: SurfaceHolder) {
        val c = StreamClient(holder.surface, Build.MODEL ?: "Android", this)
        try {
            c.start()
            client = c
        } catch (e: Exception) {
            info.text = "Port ${Proto.VIDEO_PORT} açılamadı:\n${e.message}"
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        client?.stop()
        client = null
    }

    // ------------------------------------------------------------ dinleyici

    override fun onSenderChanged(sender: String?) {
        runOnUiThread {
            if (sender == null) {
                showIdle()
            } else {
                info.visibility = View.GONE
            }
        }
    }

    override fun onVideoSize(width: Int, height: Int) {
        runOnUiThread {
            videoW = width
            videoH = height
            fitVideo()
        }
    }

    override fun onStats(text: String) {
        runOnUiThread { if (hudVisible) hud.text = text }
    }

    // ------------------------------------------------------------ arayüz

    private fun showIdle() {
        val ips = localIps()
        val ipText = if (ips.isEmpty()) "Wi-Fi bağlantısı yok" else ips.joinToString("\n")
        val first = ips.firstOrNull() ?: "<tablet-ip>"
        info.text = buildString {
            append("Yansı hazır\n\n")
            append(ipText)
            append("\n\nBilgisayarda çalıştır:\n")
            append("yansi-mac  ya da  yansi-win.exe\n")
            append("(otomatik bulamazsa: --to $first)")
        }
        info.visibility = View.VISIBLE
    }

    private fun toggleHud() {
        hudVisible = !hudVisible
        hud.visibility = if (hudVisible) View.VISIBLE else View.GONE
        if (hudVisible) hud.text = "istatistik bekleniyor…"
    }

    private fun fitVideo() {
        val vw = videoW
        val vh = videoH
        val pw = root.width
        val ph = root.height
        if (vw <= 0 || vh <= 0 || pw <= 0 || ph <= 0) return
        val s = minOf(pw.toFloat() / vw, ph.toFloat() / vh)
        val w = (vw * s).roundToInt()
        val h = (vh * s).roundToInt()
        val lp = surfaceView.layoutParams as FrameLayout.LayoutParams
        if (lp.width != w || lp.height != h) {
            lp.width = w
            lp.height = h
            lp.gravity = Gravity.CENTER
            surfaceView.layoutParams = lp
        }
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                    View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                )
        }
    }

    @Suppress("DEPRECATION")
    private fun acquireWifiLock() {
        if (wifiLock == null) {
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            val mode = if (Build.VERSION.SDK_INT >= 29) {
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }
            wifiLock = wm.createWifiLock(mode, "yansi").apply { setReferenceCounted(false) }
        }
        wifiLock?.let { if (!it.isHeld) it.acquire() }
    }

    private fun localIps(): List<String> {
        val out = ArrayList<String>()
        try {
            for (ni in Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp || ni.isLoopback) continue
                if (ni.name.startsWith("rmnet") || ni.name.startsWith("dummy")) continue
                for (a in Collections.list(ni.inetAddresses)) {
                    if (a is Inet4Address) a.hostAddress?.let { out.add(it) }
                }
            }
        } catch (_: Exception) {
        }
        return out
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).roundToInt()
}
