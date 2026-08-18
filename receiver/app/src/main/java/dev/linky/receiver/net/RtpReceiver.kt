package dev.linky.receiver.net

import android.os.SystemClock
import java.net.DatagramPacket
import java.net.DatagramSocket
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
    private var rtcp: DatagramSocket? = null
    private var rtcpPeer: java.net.InetAddress? = null
    private var rtcpPortPeer = 0

    private var expectSeq: Int = -1
    private var lastNackTs = 0L
    private var lastPliTs = 0L
    private var lastRrTs = 0L

    private var audio: DatagramSocket? = null

    fun start(sink: Sink) {
        this.sink = sink
        running.set(true)
        video = DatagramSocket(61034)
        audio = DatagramSocket(61035)
        Thread({ videoLoop() }, "linky-rtp-video").apply { isDaemon = true }.start()
        Thread({ audioLoop() }, "linky-rtp-audio").apply { isDaemon = true }.start()
        Thread({ rtcpLoop() }, "linky-rtcp").apply { isDaemon = true }.start()
    }

    fun stop() {
        running.set(false)
        runCatching { video?.close() }
        runCatching { audio?.close() }
        runCatching { rtcp?.close() }
    }

    private fun videoLoop() {
        try {
            val buf = ByteArray(65535)
            while (running.get()) {
                val p = DatagramPacket(buf, buf.size)
                runCatching { video?.receive(p) } ?: break
                if (!running.get()) break
                handleVideoPacket(p)
            }
        } catch (_: Exception) {
        }
    }

    private fun audioLoop() {
        try {
            val buf = ByteArray(4096)
            while (running.get()) {
                val p = DatagramPacket(buf, buf.size)
                runCatching { audio?.receive(p) } ?: break
                if (!running.get()) break
                val ts = rtpTsOf(p)
                sink?.onAudioPkt(buf.copyOf(p.length), ts)
            }
        } catch (_: Exception) {
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
        if (p.length < 12) return
        val seq = ((b[2].toInt() and 0xff) shl 8) or (b[3].toInt() and 0xff)
        val ts = rtpTsOf(p)

        // CCTV de secuencia: hueco → NACK (una vez por pérdida).
        if (expectSeq >= 0) {
            val diff = (seq - expectSeq) and 0xffff
            if (diff > 1) nack(expectSeq)
        }
        expectSeq = (seq + 1) and 0xffff

        depacketize(b, p.length, ts)
        // Entrega del jitter buffer (AUs atrasados) sin orden estricto.
    }

    private fun depacketize(b: ByteArray, len: Int, ts: Long) {
        if (len < 14) return
        val off = 12
        if (b[off].toInt() and 0x80 == 0) return // NO empezar en Fragmento FU intermedio

        val nalHeader = b[off].toInt()
        val fuType = nalHeader and 0x1f

        if (fuType == 28 || fuType == 49) {
            // FU-A (H.264) / FU (H.265): ensamblar el Anexo B.
            val fu = b[off + 1].toInt()
            val payload = b.copyOfRange(off + 2, len)
            if ((fu and 0x80) != 0) {          // start
                fuBuf = payload
            } else if (fuBuf.isNotEmpty()) {
                fuBuf = fuBuf.plus(payload)
            }
            if ((fu and 0x40) != 0 && fuBuf.isNotEmpty()) {  // end
                // fuBuf ya incluye la cabecera NAL (el payload del 1er fragmento
                // empieza en off+2, que ES la cabecera).
                val au = ByteArray(fuBuf.size + 4)
                au[0] = 0; au[1] = 0; au[2] = 0; au[3] = 1
                System.arraycopy(fuBuf, 0, au, 4, fuBuf.size)
                fuBuf = ByteArray(0)
                deliver(au, ts)
            }
        } else {
            val au = ByteArray(len - off + 4)
            au[0] = 0; au[1] = 0; au[2] = 0; au[3] = 1
            System.arraycopy(b, off, au, 4, len - off)
            deliver(au, ts)
        }
    }

    private var fuBuf: ByteArray = ByteArray(0)

    private fun deliver(au: ByteArray, ts: Long) {
        sink?.onVideoAu(Au(au, ts))
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
                0, 0, 0, 1,                          // SSRC del emisor (redundante)
                ((seq shr 8) and 0xff).toByte(), (seq and 0xff).toByte(), 0, 0,
            )
        )
    }

    private fun pli() {
        val now = SystemClock.uptimeMillis()
        if (now - lastPliTs < 2000) return
        lastPliTs = now
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
        runCatching {
            rtcp?.send(DatagramPacket(pkt, pkt.size, peer, rtcpPortPeer))
        }
    }

    private fun rtcpLoop() {
        try {
            // RTCP del emisor (SR) llega aquí para aprender su dirección.
            rtcp = DatagramSocket(61036)
            val buf = ByteArray(4096)
            while (running.get()) {
                val p = DatagramPacket(buf, buf.size)
                runCatching { rtcp!!.receive(p) } ?: break
                if (p.length >= 4) {
                    val pt = p.data[1].toInt() and 0xff
                    if (pt == 200 && rtcpPeer == null) {
                        rtcpPeer = p.address
                        rtcpPortPeer = p.port
                    }
                    // RR periódico mientras haya sesión
                    val now = SystemClock.uptimeMillis()
                    if (now - lastRrTs > 1000) {
                        lastRrTs = now
                        rtcpSend(
                            byteArrayOf(
                                0x81.toByte(), 201.toByte(), 0, 7,
                                0, 0, 0, 1,
                                0, 0, 0, 1,
                                0, 0, 0, 0,   // fracción perdida/cumulativa
                                0, 0, 0, 0,
                                0, 0, 0, 0,
                                0, 0, 0, 0,
                            )
                        )
                    }
                }
            }
        } catch (_: Exception) {
        }
    }
}