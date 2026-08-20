package dev.linky.receiver.nsd

import android.content.Context
import android.database.ContentObserver
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings

/**
 * Anuncia el receptor en la red local como _linky._tcp (puerto 61032), usando
 * el nombre que el usuario configuró en el TV (Android TV: Ajustes > Nombre
 * del dispositivo). Si el usuario renombra el TV, se re-anuncia el servicio.
 */
class NsdAnnouncer(private val context: Context) {

    private var manager: NsdManager? = null
    private var registered = false
    private var serviceInfo: NsdServiceInfo? = null
    private var listener: NsdManager.RegistrationListener? = null
    private var observer: ContentObserver? = null

    /** Nombre del TV configurado por el usuario, con respaldo por si no está definido. */
    private fun deviceName(): String {
        val cr = context.contentResolver
        if (Build.VERSION.SDK_INT >= 25) {
            Settings.Global.getString(cr, Settings.Global.DEVICE_NAME)
                ?.takeIf { it.isNotBlank() }
                ?.let { return it }
        }
        Settings.Global.getString(cr, "device_name")
            ?.takeIf { it.isNotBlank() }
            ?.let { return it }
        return "Linky Receiver"
    }

    fun announce() {
        if (Build.VERSION.SDK_INT < 21) return
        stop()
        manager = context.getSystemService(NsdManager::class.java)
        val info = NsdServiceInfo().apply {
            serviceName = deviceName()
            serviceType = "_linky._tcp"
            setPort(61032)
        }
        serviceInfo = info
        listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(i: NsdServiceInfo) { registered = true }
            override fun onRegistrationFailed(i: NsdServiceInfo, e: Int) { }
            override fun onServiceUnregistered(i: NsdServiceInfo) { registered = false }
            override fun onUnregistrationFailed(i: NsdServiceInfo?, e: Int) { }
        }
        try {
            manager?.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
        } catch (_: Exception) {
            // NSD no disponible (emulador/etc.): el TV puede entrar por IP/mDNS manual.
        }

        if (observer == null) {
            val obs = object : ContentObserver(Handler(Looper.getMainLooper())) {
                override fun onChange(selfChange: Boolean) { announce() }
            }
            observer = obs
            context.contentResolver.registerContentObserver(
                Settings.Global.getUriFor(Settings.Global.DEVICE_NAME),
                false,
                obs,
            )
        }
    }

    fun stop() {
        if (registered) runCatching { manager?.unregisterService(listener) }
        registered = false
        val obs = observer
        observer = null
        obs?.let { runCatching { context.contentResolver.unregisterContentObserver(it) } }
    }
}