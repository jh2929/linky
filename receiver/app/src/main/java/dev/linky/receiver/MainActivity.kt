package dev.linky.receiver

import android.os.Bundle
import android.view.SurfaceHolder
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import dev.linky.receiver.adapter.LinkyAdapter
import dev.linky.receiver.auth.AuthStore
import dev.linky.receiver.media.MediaSink

class MainActivity : AppCompatActivity(), MediaSink {

    private lateinit var surfaceView: android.view.SurfaceView
    private lateinit var statusText: TextView
    private lateinit var requestBar: LinearLayout
    private lateinit var requestText: TextView
    private lateinit var adapter: LinkyAdapter
    private var mediaSurface: android.view.Surface? = null

    override val surface: android.view.Surface?
        get() = mediaSurface

    override fun onStatus(text: String) {
        runOnUiThread { statusText.text = text }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        surfaceView = findViewById(R.id.video_surface)
        statusText = findViewById(R.id.status_text)
        requestBar = findViewById(R.id.request_bar)
        requestText = findViewById(R.id.request_text)
        val btnAccept: Button = findViewById(R.id.btn_accept)
        val btnDeny: Button = findViewById(R.id.btn_deny)

        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) {
                mediaSurface = h.surface
            }

            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hgt: Int) {}

            override fun surfaceDestroyed(h: SurfaceHolder) {
                mediaSurface = null
            }
        })

        btnAccept.setOnClickListener { adapter.respondAuthorized(); hideRequest() }
        btnDeny.setOnClickListener { adapter.respondDenied(); hideRequest() }

        adapter = LinkyAdapter(applicationContext, AuthStore(this), this)
        adapter.onRequest = { name ->
            runOnUiThread {
                requestText.text = getString(R.string.request_title, name)
                requestBar.visibility = View.VISIBLE
            }
        }
        adapter.start()
        statusText.text = getString(R.string.status_idle)
    }

    private fun hideRequest() {
        requestBar.visibility = View.GONE
    }

    override fun onDestroy() {
        super.onDestroy()
        runCatching { adapter.stop() }
    }
}