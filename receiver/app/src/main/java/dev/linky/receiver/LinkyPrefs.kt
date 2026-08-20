package dev.linky.receiver

import android.content.Context

/** Preferencias del receptor Linky (estado del botón + auto-inicio al boot). */
class LinkyPrefs(context: Context) {
    private val prefs = context.getSharedPreferences("linky_prefs", Context.MODE_PRIVATE)

    /** El receptor estaba encendido la última vez (el botón "Apagar" lo pone a false). */
    var enabled: Boolean
        get() = prefs.getBoolean("enabled", true)
        set(v) = prefs.edit().putBoolean("enabled", v).apply()

    /** Iniciar el receptor automáticamente cuando el TV enciende. */
    var bootAutoStart: Boolean
        get() = prefs.getBoolean("boot_auto_start", true)
        set(v) = prefs.edit().putBoolean("boot_auto_start", v).apply()
}