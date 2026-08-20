package dev.linky.receiver

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import android.view.SurfaceHolder
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import dev.linky.receiver.media.MediaSink
import dev.linky.receiver.service.LinkyService

/**
 * UI del receptor Linky: botón Encender/Apagar del servicio, interruptor de
 * auto-inicio al boot, superficie de vídeo y barra de aceptación de emisores.
 * Todo el protocolo vive en [LinkyService]; esta actividad solo se enlaza.
 */
class MainActivity : AppCompatActivity(), MediaSink {

    private lateinit var surfaceView: android.view.SurfaceView
    private lateinit var statusText: TextView
    private lateinit var requestBar: LinearLayout
    private lateinit var requestText: TextView
    private lateinit var powerBtn: Button
    private lateinit var bootSwitch: Switch
    private var mediaSurface: android.view.Surface? = null
    private var service: LinkyService? = null
    private var bound = false

    override val surface: android.view.Surface?
        get() = mediaSurface

    override fun onStatus(text: String) {
        runOnUiThread { statusText.text = text }
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
        statusText = findViewById(R.id.status_text)
        requestBar = findViewById(R.id.request_bar)
        requestText = findViewById(R.id.request_text)
        powerBtn = findViewById(R.id.btn_power)
        bootSwitch = findViewById(R.id.switch_boot)
        val btnAccept: Button = findViewById(R.id.btn_accept)
        val btnDeny: Button = findViewById(R.id.btn_deny)
        val btnAirplay: Button = findViewById(R.id.btn_airplay)

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

        bootSwitch.isChecked = LinkyPrefs(this).bootAutoStart
        bootSwitch.setOnCheckedChangeListener { _, checked ->
            LinkyPrefs(this).bootAutoStart = checked
        }

        btnAccept.setOnClickListener { service?.respondAuthorized(); hideRequest() }
        btnDeny.setOnClickListener { service?.respondDenied(); hideRequest() }
        btnAirplay.setOnClickListener { enterAirPlay() }
    }

    override fun onStart() {
        super.onStart()
        if (!bound) {
            bindService(Intent(this, LinkyService::class.java), conn, Context.BIND_AUTO_CREATE)
        }
        // Abrir la app enciende el receptor (a menos que se apagara explícitamente).
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

    private fun updatePowerUi(running: Boolean) {
        powerBtn.text = getString(if (running) R.string.power_off else R.string.power_on)
    }

    private fun showRequest(senderName: String) {
        requestText.text = getString(R.string.request_title, senderName)
        requestBar.visibility = View.VISIBLE
    }

    private fun hideRequest() {
        requestBar.visibility = View.GONE
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
}