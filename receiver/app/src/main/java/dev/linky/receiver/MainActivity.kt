package dev.linky.receiver

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import dev.linky.receiver.media.MediaSink
import dev.linky.receiver.service.LinkyService
import java.net.Inet4Address
import java.net.NetworkInterface

/**
 * UI del receptor Linky para Android TV (10-Foot UI):
 * - Pantalla Standby moderna con IP local, nombre del dispositivo y estado en vivo.
 * - Superficie de vídeo a pantalla completa con ocultación automática de UI.
 * - Diálogo modal de autorización accesible mediante mando a distancia (D-Pad).
 * - HUD / OSD de métricas de rendimiento en tiempo real alternable con el control remoto.
 */
class MainActivity : AppCompatActivity(), MediaSink {

    private lateinit var surfaceView: android.view.SurfaceView
    private lateinit var standbyContainer: ScrollView
    private lateinit var statusDot: View
    private lateinit var statusText: TextView
    private lateinit var deviceNameText: TextView
    private lateinit var deviceIpText: TextView
    private lateinit var powerBtn: Button
    private lateinit var airplayBtn: Button
    private lateinit var bootSwitch: Switch

    private lateinit var hudContainer: LinearLayout
    private lateinit var hudCodecText: TextView
    private lateinit var hudStatsText: TextView

    private lateinit var requestDialogContainer: FrameLayout
    private lateinit var requestDialogMessage: TextView
    private lateinit var btnAccept: Button
    private lateinit var btnDeny: Button

    private var mediaSurface: android.view.Surface? = null
    private var service: LinkyService? = null
    private var bound = false
    private var isStreaming = false

    override val surface: android.view.Surface?
        get() = mediaSurface

    override fun onStatus(text: String) {
        runOnUiThread {
            statusText.text = text
            if (text.startsWith("vídeo", ignoreCase = true)) {
                isStreaming = true
                standbyContainer.visibility = View.GONE
                hudCodecText.text = text
            } else if (text == getString(R.string.status_idle) || text == getString(R.string.status_off)) {
                isStreaming = false
                standbyContainer.visibility = View.VISIBLE
                hudContainer.visibility = View.GONE
            }
        }
    }

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            service = (binder as LinkyService.LocalBinder).service
            bound = true
            val svc = service!!
            svc.registerSink(this@MainActivity)
            svc.stateListener = { running -> runOnUiThread { updatePowerUi(running) } }
            svc.requestListener = { sender -> runOnUiThread { showRequest(sender) } }
            runOnUiThread {
                updatePowerUi(svc.running)
                statusText.text = svc.stateText
            }
        }

        override fun onServiceDisconnected(name: ComponentName) {
            bound = false
            service = null
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        surfaceView = findViewById(R.id.video_surface)
        standbyContainer = findViewById(R.id.standby_container)
        statusDot = findViewById(R.id.status_dot)
        statusText = findViewById(R.id.status_text)
        deviceNameText = findViewById(R.id.tv_device_name)
        deviceIpText = findViewById(R.id.tv_device_ip)
        powerBtn = findViewById(R.id.btn_power)
        airplayBtn = findViewById(R.id.btn_airplay)
        bootSwitch = findViewById(R.id.switch_boot)

        hudContainer = findViewById(R.id.hud_container)
        hudCodecText = findViewById(R.id.hud_codec_text)
        hudStatsText = findViewById(R.id.hud_stats_text)

        requestDialogContainer = findViewById(R.id.request_dialog_container)
        requestDialogMessage = findViewById(R.id.request_dialog_message)
        btnAccept = findViewById(R.id.btn_accept)
        btnDeny = findViewById(R.id.btn_deny)

        // Asignar nombre del dispositivo e IP local
        val deviceName = "${Build.MANUFACTURER} ${Build.MODEL}".trim()
        deviceNameText.text = if (deviceName.isNotEmpty()) deviceName else "Android TV"
        deviceIpText.text = getLocalIpAddress()

        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) {
                mediaSurface = h.surface
                service?.notifySurfaceReady()
            }

            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hgt: Int) {}

            override fun surfaceDestroyed(h: SurfaceHolder) {
                mediaSurface = null
            }
        })

        powerBtn.setOnClickListener {
            val svc = service
            if (svc != null && svc.running) {
                svc.requestStop()
            } else if (svc != null) {
                svc.requestStart()
            } else {
                LinkyPrefs(this).enabled = true
                startService(Intent(this, LinkyService::class.java)
                    .setAction(LinkyService.ACTION_START))
            }
        }

        airplayBtn.setOnClickListener { enterAirPlay() }

        bootSwitch.isChecked = LinkyPrefs(this).bootAutoStart
        bootSwitch.setOnCheckedChangeListener { _, checked ->
            LinkyPrefs(this).bootAutoStart = checked
        }

        btnAccept.setOnClickListener { service?.respondAuthorized(); hideRequest() }
        btnDeny.setOnClickListener { service?.respondDenied(); hideRequest() }

        // Foco inicial en el botón principal para navegación inmediata con mando
        powerBtn.requestFocus()
    }

    override fun onStart() {
        super.onStart()
        deviceIpText.text = getLocalIpAddress()
        if (!bound) {
            bindService(Intent(this, LinkyService::class.java), conn, Context.BIND_AUTO_CREATE)
        }
        if (LinkyPrefs(this).enabled) {
            startService(Intent(this, LinkyService::class.java)
                .setAction(LinkyService.ACTION_START))
        }
    }

    override fun onStop() {
        if (bound) {
            service?.unregisterSink()
            unbindService(conn)
            bound = false
            service = null
        }
        super.onStop()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_DPAD_DOWN || keyCode == KeyEvent.KEYCODE_DPAD_CENTER || keyCode == KeyEvent.KEYCODE_MENU) {
            if (isStreaming) {
                toggleHud()
                return true
            }
        }
        return super.onKeyDown(keyCode, event)
    }

    private fun toggleHud() {
        hudContainer.visibility = if (hudContainer.visibility == View.VISIBLE) View.GONE else View.VISIBLE
    }

    private fun updatePowerUi(running: Boolean) {
        powerBtn.text = getString(if (running) R.string.power_off else R.string.power_on)
        powerBtn.setBackgroundResource(R.drawable.btn_tv_minimal)
        statusDot.setBackgroundColor(androidx.core.content.ContextCompat.getColor(this, if (running) R.color.tv_dot_active else R.color.tv_dot_inactive))
    }

    private fun showRequest(senderName: String) {
        requestDialogMessage.text = getString(R.string.request_dialog_msg, senderName)
        requestDialogContainer.visibility = View.VISIBLE
        btnAccept.requestFocus()
    }

    private fun hideRequest() {
        requestDialogContainer.visibility = View.GONE
        powerBtn.requestFocus()
    }

    private fun enterAirPlay() {
        // Linky y AirPlay no conviven: se apaga el receptor Linky primero.
        val svc = service
        if (svc != null && svc.running) svc.requestStop()
        else LinkyPrefs(this).enabled = false
        val intent = Intent().apply {
            component = ComponentName(
                "dev.linky.receiver", "io.github.jqssun.airplay.MainActivity",
            )
        }
        startActivity(intent)
    }

    private fun getLocalIpAddress(): String {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val iface = interfaces.nextElement()
                if (iface.isLoopback || !iface.isUp) continue
                val addrs = iface.inetAddresses
                while (addrs.hasMoreElements()) {
                    val addr = addrs.nextElement()
                    if (!addr.isLoopbackAddress && addr is Inet4Address) {
                        return addr.hostAddress ?: ""
                    }
                }
            }
        } catch (_: Exception) {}
        return "127.0.0.1"
    }
}