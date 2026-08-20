package dev.linky.receiver.net

import android.util.Log
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import org.json.JSONObject

/** Hello del emisor (canal de control). */
data class Hello(
    val senderId: String,
    val senderName: String,
    val codecs: List<String>,
)

/**
 * Canal de control TCP (puerto 61032), líneas JSON:
 *  emisor → {type:hello, sender_id, sender_name, codecs:[…]}
 *  receptor → {type:welcome, session, codec, audio, vport, vrtcp, aport, artcp}
 *  receptor → {type:denied} / {type:bye}
 */
class ControlServer(
    private val port: Int,
    private val onHello: (Hello, accept: () -> Unit, deny: () -> Unit) -> Unit,
) {
    private val running = AtomicBoolean(false)
    private val sockets = mutableListOf<Socket>()
    private var server: ServerSocket? = null

    fun start() {
        running.set(true)
        Thread({ acceptLoop() }, "linky-control").apply { isDaemon = true }.start()
        Log.i(TAG, "control escuchando en :$port")
    }

    fun stop() {
        running.set(false)
        server?.close()
        synchronized(sockets) {
            sockets.forEach { runCatching { it.close() } }
            sockets.clear()
        }
    }

    private fun acceptLoop() {
        try {
            ServerSocket(port).use { ss ->
                server = ss
                while (running.get()) {
                    val s = ss.accept() ?: continue
                    synchronized(sockets) { sockets.add(s) }
                    Thread({ handle(s) }, "linky-control-sess").apply { isDaemon = true }.start()
                }
            }
        } catch (_: Exception) {
            // server cerrado en stop()
        }
    }

    private fun handle(socket: Socket) {
        var welcomeSent = false
        try {
            val reader = BufferedReader(InputStreamReader(socket.getInputStream()))
            val writer = OutputStreamWriter(socket.getOutputStream())
            var pendingDeny = false
            Log.i(TAG, "nueva conexión de control ${socket.inetAddress?.hostAddress}")

            val accept = {
                if (!welcomeSent) {
                    val codec = pickCodec(negotiatedCodecs)
                    val hello = negotiated
                    val session = UUID.randomUUID().toString()
                    val w = JSONObject()
                        .put("type", "welcome")
                        .put("session", session)
                        .put("codec", codec)
                        .put("audio", negotiatedAudio)
                        .put("vport", 61034)
                        .put("vrtcp", 61036)
                        .put("aport", 61035)
                        .put("artcp", 61036)
                    writer.write(w.toString() + "\n")
                    writer.flush()
                    welcomeSent = true
                    Log.i(TAG, "welcome enviado (codec=$codec, audio=$negotiatedAudio)")
                    onWelcome?.invoke(hello, codec, negotiatedAudio)
                }
            }
            val deny = {
                if (!welcomeSent) {
                    writer.write(JSONObject().put("type", "denied").toString() + "\n")
                    writer.flush()
                    welcomeSent = true
                    pendingDeny = true
                }
            }

            while (running.get()) {
                val line = reader.readLine() ?: break
                if (line.isBlank()) continue
                val o = JSONObject(line)
                if (o.optString("type") == "hello") {
                    negotiated = Hello(
                        senderId = o.optString("senderId").ifEmpty { o.optString("sender_id") },
                        senderName = o.optString("device").ifEmpty { o.optString("sender_name") },
                        codecs = o.optJSONArray("codecs")?.let {
                            (0 until it.length()).map { n -> it.getString(n) }
                        } ?: o.optString("codecs").split(",").map { it.trim() }
                            .filter { it.isNotEmpty() },
                    )
                    negotiatedCodecs = negotiated.codecs
                    Log.i(TAG, "hello recibido de '${negotiated.senderName}' codecs=${negotiated.codecs}")
                    onHello(negotiated, accept, deny)
                    if (pendingDeny) break
                } else if (o.optString("type") == "bye") {
                    break
                }
            }
            Log.i(TAG, "sesión de control terminada")
        } catch (e: Exception) {
            Log.w(TAG, "error en sesión de control", e)
        } finally {
            runCatching { socket.close() }
            synchronized(sockets) { sockets.remove(socket) }
            if (welcomeSent) onBye?.invoke()
        }
    }

    /** Callbacks del sistema de media. */
    var onWelcome: ((Hello, codec: String, audio: String) -> Unit)? = null

    /** La sesión terminó (bye o desconexión del emisor). */
    var onBye: (() -> Unit)? = null

    private var negotiated = Hello("", "", emptyList())
    private var negotiatedCodecs: List<String> = emptyList()
    private var negotiatedAudio: String = "opus"

    /** Elige el mejor códec del emisor respetando capacidades del TV. */
    private fun pickCodec(codecs: List<String>): String {
        if (codecs.contains("h265") && android.os.Build.VERSION.SDK_INT >= 21) return "h265"
        if (codecs.contains("h264")) return "h264"
        return "h264"
    }

    private companion object {
        const val TAG = "linky"
    }
}