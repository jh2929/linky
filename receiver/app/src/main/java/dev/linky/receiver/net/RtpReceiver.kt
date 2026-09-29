package dev.linky.receiver.net

import android.os.SystemClock
import android.util.Log
import java.io.ByteArrayOutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.util.concurrent.atomic.AtomicBoolean

/** Unidad de acceso (NAL/SPS/PPS completo de un access unit). */
data class Au(val data: ByteArray, val tsRtp: Long)

/**
 * Receptor RTP/UDP (video), con despaquetización
 * H.264 (RFC 6184) / H.265 (RFC 7798) / Opus (RFC 7587) y RTCP:
 * RR periódico, NACK en huecos de secuencia y PLI si se atasca.
 * Puertos fijos del protocolo Linky: video 61034, audio 61035, RTCP 61036.
 */
class RtpReceiver {
    interface Sink {
        fun onVideoAu(au: Au)
        fun onAudioPkt(pkt: ByteArray, tsRtp: Long)
    }

    private val running = AtomicBoolean(false)
    private var sink: Sink? = null
    private var video: DatagramSocket? = null
    private var audio: DatagramSocket? = null
    private var rtcp: DatagramSocket? = null
    private var rtcpPeer: InetAddress? = null
    private var rtcpPortPeer = 0

    private var expectSeq: Int = -1
    private var lastNackTs = 0L
    private var lastPliTs = 0L
    private var lastRrTs = 0L

    private val fuStream = ByteArrayOutputStream(65536)
    private val auStream = ByteArrayOutputStream(262144)
    private var auTs: Long = 0

    companion object {
        private const val TAG = "linky-rtp"
        private val START_CODE = byteArrayOf(0, 0, 0, 1)

        private fun createUdpSocket(port: Int): DatagramSocket {
            return DatagramSocket(null).apply {
                reuseAddress = true
                bind(InetSocketAddress(port))
            }
        }
    }

    fun start(sink: Sink) {
        this.sink = sink
        running.set(true)
        try {
            video = createUdpSocket(61034)
            audio = createUdpSocket(61035)
            rtcp = createUdpSocket(61036)
            Log.i(TAG, "Sockets UDP inicializados en puertos 61034 (video), 61035 (audio), 61036 (rtcp)")
        } catch (e: Exception) {
            Log.e(TAG, "Error inicializando sockets UDP", e)
            stop()
            throw e
        }

        Thread({ videoLoop() }, "linky-rtp-video").apply { isDaemon = true }.start()
        Thread({ audioLoop() }, "linky-rtp-audio").apply { isDaemon = true }.start()
        Thread({ rtcpLoop() }, "linky-rtcp").apply { isDaemon = true }.start()
    }

    fun stop() {
        running.set(false)
        runCatching { video?.close() }
        runCatching { audio?.close() }
        runCatching { rtcp?.close() }
        video = null
        audio = null
        rtcp = null
        expectSeq = -1
        synchronized(auStream) {
            fuStream.reset()
            auStream.reset()
        }
        Log.i(TAG, "RtpReceiver detenido y sockets cerrados")
    }

    private fun videoLoop() {
        val s = video ?: return
        try {
            val buf = ByteArray(65535)
            while (running.get()) {
                val p = DatagramPacket(buf, buf.size)
                s.receive(p)
                if (!running.get()) break
                handleVideoPacket(p)
            }
        } catch (e: Exception) {
            if (running.get()) Log.w(TAG, "Excepción en bucle de vídeo", e)
        }
    }

    private fun audioLoop() {
        val s = audio ?: return
        try {
            val buf = ByteArray(4096)
            while (running.get()) {
                val p = DatagramPacket(buf, buf.size)
                s.receive(p)
                if (!running.get()) break
                val ts = rtpTsOf(p)
                sink?.onAudioPkt(buf.copyOf(p.length), ts)
            }
        } catch (e: Exception) {
            if (running.get()) Log.w(TAG, "Excepción en bucle de audio", e)
        }
    }

    private fun rtpTsOf(p: DatagramPacket): Long {
        val b = p.data
        return ((b[4].toLong() and 0xff) shl 24) or
            ((b[5].toLong() and 0xff) shl 16) or
            ((b[6].toLong() and 0xff) shl 8) or
            (b[7].toLong() and 0xff)
    }

    private fun handleVideoPacket(p: DatagramPacket) {
        val b = p.data
        val len = p.length
        if (len < 12) return
        val seq = ((b[2].toInt() and 0xff) shl 8) or (b[3].toInt() and 0xff)
        val ts = rtpTsOf(p)
        val marker = (b[1].toInt() and 0x80) != 0

        // Detección de pérdida de paquetes: hueco en seq
        if (expectSeq >= 0) {
            val diff = (seq - expectSeq) and 0xffff
            if (diff in 2..500) {
                Log.d(TAG, "Pérdida detectada: esperado $expectSeq, recibido $seq (diff=$diff)")
                nack(expectSeq)
            }
        }
        expectSeq = (seq + 1) and 0xffff

        synchronized(auStream) {
            depacketize(b, len, ts)
            if (marker) flushAu()
        }
    }

    private fun depacketize(b: ByteArray, len: Int, ts: Long) {
        if (len < 14) return
        auTs = ts
        val off = 12
        val b0 = b[off].toInt()
        val t264 = b0 and 0x1f
        val t265 = (b0 shr 1) and 0x3f

        if (t264 == 28 || t265 == 49) {
            // FU-A (H.264) o FU (H.265)
            val fu = b[off + 1].toInt()
            val start = (fu and 0x80) != 0
            val end = (fu and 0x40) != 0
            val payloadLen = len - (off + 2)

            if (start) {
                fuStream.reset()
                fuStream.write(b, off + 2, payloadLen)
            } else if (fuStream.size() > 0) {
                fuStream.write(b, off + 2, payloadLen)
            }

            if (end && fuStream.size() > 0) {
                auStream.write(START_CODE)
                fuStream.writeTo(auStream)
                fuStream.reset()
            }
        } else {
            // NAL individual completo (SPS/PPS/VPS/IDR slice)
            auStream.write(START_CODE)
            auStream.write(b, off, len - off)
        }
    }

    private fun flushAu() {
        if (auStream.size() == 0) return
        val fullAu = auStream.toByteArray()
        auStream.reset()
        sink?.onVideoAu(Au(fullAu, auTs))
    }

    // ── RTCP ───────────────────────────────────────────────────────────────
    private fun nack(seq: Int) {
        val now = SystemClock.uptimeMillis()
        if (now - lastNackTs < 80) return
        lastNackTs = now
        rtcpSend(
            byteArrayOf(
                0x81.toByte(), 205.toByte(), 0, 2,
                0, 0, 0, 1,                          // SSRC del receptor
                0, 0, 0, 1,                          // SSRC del emisor
                ((seq shr 8) and 0xff).toByte(), (seq and 0xff).toByte(), 0, 0,
            )
        )
    }

    private fun pli() {
        val now = SystemClock.uptimeMillis()
        if (now - lastPliTs < 2000) return
        lastPliTs = now
        Log.i(TAG, "Solicitando keyframe (PLI)")
        rtcpSend(
            byteArrayOf(
                0x81.toByte(), 206.toByte(), 0, 2,
                0, 0, 0, 1,
                0, 0, 0, 1,
            )
        )
    }

    private fun rtcpSend(pkt: ByteArray) {
        val peer = rtcpPeer ?: return
        val s = rtcp ?: return
        runCatching {
            s.send(DatagramPacket(pkt, pkt.size, peer, rtcpPortPeer))
        }
    }

    private fun rtcpLoop() {
        val s = rtcp ?: return
        try {
            val buf = ByteArray(4096)
            while (running.get()) {
                val p = DatagramPacket(buf, buf.size)
                s.receive(p)
                if (!running.get()) break
                if (p.length >= 4) {
                    val pt = p.data[1].toInt() and 0xff
                    if (pt == 200 && rtcpPeer == null) {
                        rtcpPeer = p.address
                        rtcpPortPeer = p.port
                        Log.i(TAG, "RTCP emisor detectado en $rtcpPeer:$rtcpPortPeer")
                    }
                    val now = SystemClock.uptimeMillis()
                    if (now - lastRrTs > 1000) {
                        lastRrTs = now
                        rtcpSend(
                            byteArrayOf(
                                0x81.toByte(), 201.toByte(), 0, 7,
                                0, 0, 0, 1,
                                0, 0, 0, 1,
                                0, 0, 0, 0,
                                0, 0, 0, 0,
                                0, 0, 0, 0,
                                0, 0, 0, 0,
                            )
                        )
                    }
                }
            }
        } catch (e: Exception) {
            if (running.get()) Log.w(TAG, "Excepción en bucle RTCP", e)
        }
    }
}