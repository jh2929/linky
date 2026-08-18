package dev.linky.receiver.auth

import android.content.Context
import java.security.MessageDigest

/**
 * Emisores de confianza: hash SHA-256 del sender_id una vez el usuario
 * acepta la petición de conexión.
 */
class AuthStore(context: Context) {
    private val prefs = context.getSharedPreferences("linky_auth", Context.MODE_PRIVATE)

    fun isTrusted(senderId: String): Boolean =
        prefs.getBoolean("trust_" + sha256(senderId), false)

    fun trust(senderId: String) {
        prefs.edit().putBoolean("trust_" + sha256(senderId), true).apply()
    }

    fun forget(senderId: String) {
        prefs.edit().remove("trust_" + sha256(senderId)).apply()
    }

    private fun sha256(s: String): String =
        MessageDigest.getInstance("SHA-256")
            .digest(s.toByteArray())
            .joinToString("") { "%02x".format(it) }
}