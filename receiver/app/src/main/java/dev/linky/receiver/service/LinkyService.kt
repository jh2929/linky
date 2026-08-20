package dev.linky.receiver.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.util.Log
import android.view.Surface
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import dev.linky.receiver.LinkyPrefs
import dev.linky.receiver.MainActivity
import dev.linky.receiver.R
import dev.linky.receiver.adapter.LinkyAdapter
import dev.linky.receiver.auth.AuthStore
import dev.linky.receiver.media.MediaSink
import dev.linky.receiver.net.Hello

/**
 * Receptor Linky como servicio en primer plano: anuncia `_linky._tcp` por NSD
 * y escucha el canal de control (61032) aunque la app esté cerrada, igual que
 * el módulo AirPlay. La UI ([MainActivity]) se enlaza para ofrecer la
 * superficie de vídeo y el botón de encender/apagar.
 *
 * El vídeo se arranca de forma perezosa: la sesión se negocia en el hilo de
 * red, pero [MediaEngine] solo se crea cuando hay una superficie disponible
 * (la UI abierta). Si nadie está mirando, se lanza la actividad.
 */
class LinkyService : Service() {

    companion object {
        const val ACTION_START = "dev.linky.receiver.START"
        const val ACTION_STOP = "dev.linky.receiver.STOP"
        private const val CHANNEL_ID = "linky"
        private const val NOTIFICATION_ID = 2
        private const val TAG = "linky"
    }

    private val binder = LocalBinder()

    @Volatile
    var running = false
        private set

    @Volatile
    var stateText: String = ""
        private set

    /** Actividad enlazada (superficie de vídeo + estado en pantalla). */
    @Volatile
    private var sink: MediaSink? = null

    /** La actividad enlazada se entera del cambio encendido/apagado. */
    var stateListener: ((running: Boolean) -> Unit)? = null

    /** La actividad enlazada muestra la barra "emisor quiere transmitir". */
    var requestListener: ((senderName: String) -> Unit)? = null

    private var adapter: LinkyAdapter? = null

    /** Sesión negociada esperando superficie (hello, codec, audio). */
    @Volatile
    private var pending: Triple<Hello, String, String>? = null

    /** Última sesión negociada (activa o reanudable al volver a la UI). */
    @Volatile
    private var lastSession: Triple<Hello, String, String>? = null

    /** Nombre del emisor cuya petición de conexión sigue sin responder. */
    @Volatile
    private var pendingRequestName: String? = null

    inner class LocalBinder : Binder() {
        val service get() = this@LinkyService
    }

    override fun onBind(intent: Intent?): IBinder? = binder

    override fun onCreate() {
        super.onCreate()
        stateText = getString(R.string.status_off)
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopLinky()
                ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
                stopSelf(startId)
            }
            ACTION_START -> startLinky()
            else -> startLinky()
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        adapter?.stop()
        adapter = null
        pending = null
        lastSession = null
        pendingRequestName = null
        running = false
        sink = null
        super.onDestroy()
    }

    // ── Arranque / parada del receptor ─────────────────────────────────────

    private fun startLinky() {
        if (running) return
        running = true
        stateText = getString(R.string.status_idle)
        stateListener?.invoke(true)
        sink?.onStatus(stateText)
        promoteToForeground()
        Log.i(TAG, "receptor encendido")

        adapter = LinkyAdapter(
            this,
            AuthStore(this),
            object : MediaSink {
                override val surface: Surface? get() = sink?.surface
                override fun onStatus(text: String) {
                    stateText = text
                    sink?.onStatus(text)
                }
            },
        )
        adapter?.onRequest = { name ->
            Log.i(TAG, "petición de conexión de '$name'")
            pendingRequestName = name
            launchUi()
            requestListener?.invoke(name)
        }
        adapter?.onWelcome = { hello, codec, audio ->
            Log.i(TAG, "sesión negociada (codec=$codec, audio=$audio)")
            val t = Triple(hello, codec, audio)
            pending = t
            lastSession = t
            launchUi()
            tryStartMedia()
        }
        adapter?.onSessionEnd = {
            if (running) {
                pending = null
                lastSession = null
                stateText = getString(R.string.status_idle)
                sink?.onStatus(stateText)
            }
        }
        adapter?.start()
    }

    private fun stopLinky() {
        if (!running) return
        adapter?.stop()
        adapter = null
        pending = null
        lastSession = null
        pendingRequestName = null
        running = false
        stateText = getString(R.string.status_off)
        stateListener?.invoke(false)
        sink?.onStatus(stateText)
    }

    // ── API para la UI (MainActivity) ──────────────────────────────────────

    /** Botón "Encender": arranca el receptor y recuerda el estado. */
    fun requestStart() {
        LinkyPrefs(this).enabled = true
        if (!running) {
            startService(Intent(this, LinkyService::class.java).setAction(ACTION_START))
        }
    }

    /** Botón "Apagar": detiene el receptor y recuerda el estado. */
    fun requestStop() {
        LinkyPrefs(this).enabled = false
        if (running) {
            startService(Intent(this, LinkyService::class.java).setAction(ACTION_STOP))
        }
    }

    /** La actividad se enlaza: entrega superficie y estado en pantalla. */
    fun registerSink(sink: MediaSink) {
        this.sink = sink
        sink.onStatus(stateText)
        pendingRequestName?.let { requestListener?.invoke(it) }
        tryStartMedia()
    }

    fun unregisterSink() {
        sink = null
    }

    /** La SurfaceView de la UI acaba de crear su superficie. */
    fun notifySurfaceReady() {
        if (pending != null) {
            tryStartMedia()
            return
        }
        // Sesión activa sin superficie (p. ej. el usuario volvió de otra app):
        // reiniciar la reproducción sobre la superficie nueva.
        val last = lastSession ?: return
        val a = adapter ?: return
        if (a.mediaActive && sink?.surface != null) {
            a.startMedia(last.first, last.second, last.third)
        }
    }

    fun respondAuthorized() {
        pendingRequestName = null
        adapter?.respondAuthorized()
    }

    fun respondDenied() {
        pendingRequestName = null
        adapter?.respondDenied()
    }

    // ── Reproducción perezosa ──────────────────────────────────────────────

    private fun tryStartMedia() {
        val p = pending ?: return
        val s = sink ?: return
        if (s.surface == null) return
        pending = null
        Log.i(TAG, "arrancando media con superficie disponible")
        adapter?.startMedia(p.first, p.second, p.third)
    }

    private fun launchUi() {
        val intent = Intent(this, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
        startActivity(intent)
    }

    // ── Notificación de servicio en primer plano ──────────────────────────

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.notification_channel),
            NotificationManager.IMPORTANCE_LOW,
        )
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val pi = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(getString(R.string.notification_title))
            .setContentText(getString(R.string.notification_text))
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
    }

    private fun promoteToForeground() {
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            buildNotification(),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
        )
    }
}