package dev.linky.receiver.adapter

import android.content.Context
import dev.linky.receiver.auth.AuthStore
import dev.linky.receiver.media.MediaEngine
import dev.linky.receiver.media.MediaSink
import dev.linky.receiver.net.ControlServer
import dev.linky.receiver.net.RtpReceiver
import dev.linky.receiver.nsd.NsdAnnouncer

/**
 * Adaptador del protocolo Linky (RTP/UDP + TCP/JSON), implementado sobre los
 * módulos de red/media. No sabe nada de AirPlay.
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
        control.onWelcome = { hello, codec, audio ->
            val m = MediaEngine(requestSink)
            m.start(hello, codec, audio)
            media = m
            rtp = RtpReceiver().also { r ->
                r.start(object : RtpReceiver.Sink {
                    override fun onVideoAu(au: dev.linky.receiver.net.Au) = m.onVideoAu(au)
                    override fun onAudioPkt(pkt: ByteArray, tsRtp: Long) = m.onAudioPkt(pkt, tsRtp)
                })
            }
        }
        control.start()
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
        control.stop()
        rtp?.stop()
        media?.stop()
        nsd?.stop()
    }
}