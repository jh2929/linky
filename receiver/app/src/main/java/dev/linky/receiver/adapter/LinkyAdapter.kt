package dev.linky.receiver.adapter

import android.content.Context
import dev.linky.receiver.auth.AuthStore
import dev.linky.receiver.media.MediaEngine
import dev.linky.receiver.media.MediaSink
import dev.linky.receiver.net.ControlServer
import dev.linky.receiver.net.Hello
import dev.linky.receiver.net.RtpReceiver
import dev.linky.receiver.nsd.NsdAnnouncer

/**
 * Adaptador del protocolo Linky (RTP/UDP + TCP/JSON), implementado sobre los
 * módulos de red/media. No sabe nada de AirPlay.
 *
 * Vive en un [android.app.Service] (ver `service.LinkyService`): anuncia el
 * receptor por NSD y escucha el canal de control las 24 h, aunque la UI no
 * esté abierta. La reproducción de vídeo/audio solo arranca cuando hay una
 * superficie disponible ([startMedia]).
 */
class LinkyAdapter(
    private val context: Context,
    private val auth: AuthStore,
    private val requestSink: MediaSink,
) : ProtocolAdapter {

    private lateinit var control: ControlServer
    private var media: MediaEngine? = null
    private var rtp: RtpReceiver? = null
    private var nsd: NsdAnnouncer? = null

    /** Llamado cuando un emisor desconocido pide conexión (pendiente de trámite UI). */
    var onRequest: ((senderName: String) -> Unit)? = null

    /** Sesión negociada: la UI/servicio decide cuándo arrancar [startMedia]. */
    var onWelcome: ((hello: Hello, codec: String, audio: String) -> Unit)? = null

    /** La sesión terminó (bye o desconexión del emisor). */
    var onSessionEnd: (() -> Unit)? = null

    /** Hay reproducción activa en este momento. */
    @Volatile
    var mediaActive = false
        private set

    override fun start() {
        nsd = NsdAnnouncer(context)
        nsd?.announce()

        control = ControlServer(61032) { hello, accept, deny ->
            val trusted = auth.isTrusted(hello.senderId)
            if (trusted) {
                accept()
            } else {
                pending = Pending(hello.senderId, hello.senderName, accept, deny)
                onRequest?.invoke(hello.senderName)
            }
        }
        control.onWelcome = { hello, codec, audio -> onWelcome?.invoke(hello, codec, audio) }
        control.onBye = {
            stopMedia()
            onSessionEnd?.invoke()
        }
        control.start()
    }

    /** Arranca la reproducción una vez negociada la sesión y hay superficie. */
    @Synchronized
    fun startMedia(hello: Hello, codec: String, audio: String) {
        stopMedia()
        try {
            val m = MediaEngine(requestSink)
            m.start(hello, codec, audio)
            media = m
            rtp = RtpReceiver().also { r ->
                r.start(object : RtpReceiver.Sink {
                    override fun onVideoAu(au: dev.linky.receiver.net.Au) = m.onVideoAu(au)
                    override fun onAudioPkt(pkt: ByteArray, tsRtp: Long) = m.onAudioPkt(pkt, tsRtp)
                })
            }
            mediaActive = true
        } catch (_: Exception) {
            media = null
            rtp = null
            mediaActive = false
        }
    }

    /** Detiene la reproducción en curso (fin de sesión o apagado). */
    @Synchronized
    fun stopMedia() {
        rtp?.stop()
        rtp = null
        media?.stop()
        media = null
        mediaActive = false
    }

    private class Pending(
        val senderId: String,
        val senderName: String,
        val accept: () -> Unit,
        val deny: () -> Unit,
    )

    private var pending: Pending? = null

    fun respondAuthorized() {
        val p = pending
        pending = null
        if (p != null) {
            auth.trust(p.senderId)
            p.accept()
        }
    }

    fun respondDenied() {
        val p = pending
        pending = null
        p?.deny()
    }

    override fun stop() {
        stopMedia()
        control.stop()
        nsd?.stop()
    }
}