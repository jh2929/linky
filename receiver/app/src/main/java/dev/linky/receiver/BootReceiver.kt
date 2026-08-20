package dev.linky.receiver

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat
import dev.linky.receiver.service.LinkyService

/**
 * Arranca el receptor Linky al encender el TV (como el módulo AirPlay): si el
 * usuario dejó el receptor encendido y el auto-inicio está activo, se anuncia
 * `_linky._tcp` y se abre el canal de control sin abrir ninguna app.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED &&
            intent.action != "android.intent.action.QUICKBOOT_POWERON"
        ) return

        val prefs = LinkyPrefs(context)
        if (!prefs.enabled || !prefs.bootAutoStart) return

        ContextCompat.startForegroundService(
            context,
            Intent(context, LinkyService::class.java)
                .setAction(LinkyService.ACTION_START),
        )
    }
}