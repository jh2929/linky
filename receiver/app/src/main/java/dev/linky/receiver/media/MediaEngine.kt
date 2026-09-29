package dev.linky.receiver.media

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.view.Surface
import dev.linky.receiver.net.Au
import dev.linky.receiver.net.Hello
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean

/** Destino de presentación: Surface del SurfaceView + estado de UI. */
interface MediaSink {
    val surface: Surface?
    fun onStatus(text: String)
}

/**
 * Motor de reproducción del receptor Linky:
 *  - vídeo: MediaCodec (h264/hevc segun welcome) con LOW_LATENCY → Surface
 *  - audio: Opus via MediaCodec (API 31+) → AudioTrack LOW_LATENCY
 *  - jitter de 25 ms antes de alimentar el decodificador
 */
class MediaEngine(
    private val sink: MediaSink,
) {
    private val running = AtomicBoolean(false)
    private var videoCodec: MediaCodec? = null
    private var audioCodec: MediaCodec? = null
    private var audioTrack: AudioTrack? = null

    private val jitterThread = HandlerThread("linky-jitter").apply { start() }
    private val jitterHandler = Handler(jitterThread.looper)

    private var baseVideoTs = -1L
    private var baseAudioTs = -1L
    private var videoMime = "video/avc"

    fun start(hello: Hello, codecName: String, audioName: String) {
        running.set(true)
        videoMime = when (codecName) {
            "h265" -> "video/hevc"
            "av1" -> "video/av01"
            else -> "video/avc"
        }
        val surface = sink.surface

        val w = if (hello.width > 0) hello.width else 1920
        val h = if (hello.height > 0) hello.height else 1080
        val fps = if (hello.fps > 0) hello.fps else 60

        val vf = MediaFormat.createVideoFormat(videoMime, w, h).apply {
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, maxOf(1048576, w * h))
            if (Build.VERSION.SDK_INT >= 26) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                setInteger(MediaFormat.KEY_OPERATING_RATE, fps)
            }
        }

        android.util.Log.i("linky-dec", "Inicializando decodificador $videoMime (${w}x${h}@${fps})")
        val codec = MediaCodec.createDecoderByType(videoMime)
        try {
            codec.configure(vf, surface, null, 0)
        } catch (e: Exception) {
            android.util.Log.w("linky-dec", "Configuración LOW_LATENCY falló; reintentando modo estándar", e)
            val fallbackFmt = MediaFormat.createVideoFormat(videoMime, w, h)
            codec.configure(fallbackFmt, surface, null, 0)
        }
        codec.start()
        videoCodec = codec
        Thread({ videoOutLoop() }, "linky-render").apply { isDaemon = true }.start()

        if (audioName.contains("opus") && Build.VERSION.SDK_INT >= 31) {
            val af = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, 48000, 2)
            audioCodec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS).also { c ->
                c.configure(af, null, null, 0)
                c.start()
            }
            audioTrack = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(48000)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .build()
                )
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .build()
            audioTrack?.play()
            Thread({ audioLoop() }, "linky-dec-audio").apply { isDaemon = true }.start()
        } else {
            sink.onStatus(if (Build.VERSION.SDK_INT >= 31) "audio: no demandado" else "audio: Opus requiere Android 12+")
        }
    }

    fun stop() {
        running.set(false)
        runCatching { videoCodec?.stop() }
        runCatching { videoCodec?.release() }
        runCatching { audioCodec?.stop() }
        runCatching { audioCodec?.release() }
        jitterHandler.removeCallbacksAndMessages(null)
        jitterThread.quitSafely()
        audioTrack?.apply {
            runCatching { pause() }
            runCatching { flush() }
            runCatching { release() }
        }
    }

    // ── Entrada desde la red ───────────────────────────────────────────────
    fun onVideoAu(au: Au) {
        if (!running.get()) return
        // Jitter: 25 ms de colchón antes de tocar el decodificador.
        jitterHandler.postDelayed(
            { feedVideo(au) },
            25,
        )
    }

    fun onAudioPkt(pkt: ByteArray, tsRtp: Long) {
        if (!running.get()) return
        jitterHandler.postDelayed(
            { feedAudio(pkt, tsRtp) },
            25,
        )
    }

    // ── Decodificadores ────────────────────────────────────────────────────
    private fun feedVideo(au: Au) {
        val c = videoCodec ?: return
        if (baseVideoTs < 0) baseVideoTs = au.tsRtp
        val ptsUs = (au.tsRtp - baseVideoTs) * 1000 / 90 // RTP vídeo: 90 kHz
        sendToCodec(c, au.data, ptsUs, 0)
    }

    private fun feedAudio(pkt: ByteArray, tsRtp: Long) {
        val c = audioCodec ?: return
        if (baseAudioTs < 0) baseAudioTs = tsRtp
        val ptsUs = (tsRtp - baseAudioTs) * 1_000_000 / 48_000 // RTP opus: 48 kHz
        sendToCodec(c, pkt, ptsUs, 0)
    }

    private fun sendToCodec(c: MediaCodec, data: ByteArray, ptsUs: Long, flags: Int) {
        try {
            val idx = c.dequeueInputBuffer(20_000)
            if (idx < 0) return
            val buf = c.getInputBuffer(idx) ?: return
            buf.clear()
            if (data.size > buf.remaining()) {
                android.util.Log.w("linky-dec", "Buffer de MediaCodec insuficiente: capacidad=${buf.remaining()}, payload=${data.size}")
                return
            }
            buf.put(data)
            c.queueInputBuffer(idx, 0, data.size, maxOf(0L, ptsUs), flags)
        } catch (e: Exception) {
            android.util.Log.w("linky-dec", "Error en sendToCodec", e)
        }
    }

    private fun videoOutLoop() {
        val c = videoCodec ?: return
        val info = MediaCodec.BufferInfo()
        while (running.get()) {
            try {
                val idx = c.dequeueOutputBuffer(info, 10_000)
                when {
                    idx >= 0 -> {
                        if (info.size > 0) c.releaseOutputBuffer(idx, true) else c.releaseOutputBuffer(idx, false)
                    }
                    idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val fmt = c.outputFormat
                        sink.onStatus("vídeo ${fmt.getString(MediaFormat.KEY_MIME)} ${fmt.getInteger("width")}x${fmt.getInteger("height")}")
                    }
                }
            } catch (_: IllegalStateException) {
                return
            }
        }
    }

    private fun audioLoop() {
        val ac = audioCodec ?: return
        val at = audioTrack ?: return
        val info = MediaCodec.BufferInfo()
        val pcm = ByteArray(16384)
        while (running.get()) {
            try {
                val idx = ac.dequeueOutputBuffer(info, 10_000)
                if (idx >= 0) {
                    if (info.size > 0) {
                        val buf = ac.getOutputBuffer(idx) ?: continue
                        buf.position(info.offset)
                        buf.limit(info.offset + info.size)
                        val n = minOf(pcm.size, info.size)
                        buf.get(pcm, 0, n)
                        at.write(pcm, 0, n)
                    }
                    ac.releaseOutputBuffer(idx, false)
                }
            } catch (_: IllegalStateException) {
                return
            }
        }
    }
}