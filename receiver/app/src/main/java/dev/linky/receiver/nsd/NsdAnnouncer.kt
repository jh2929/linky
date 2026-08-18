package dev.linky.receiver.nsd

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build

/** Anuncia el receptor en la red local como _linky._tcp (puerto 61032). */
class NsdAnnouncer(private val context: Context) {

    private var manager: NsdManager? = null
    private var registered = false
    private var serviceInfo: NsdServiceInfo? = null
    private var listener: NsdManager.RegistrationListener? = null

    fun announce() {
        if (Build.VERSION.SDK_INT < 21) return
        val info = NsdServiceInfo().apply {
            serviceName = "Linky Receiver"
            serviceType = "_linky._tcp"
            setPort(61032)
        }
        serviceInfo = info
        try {
            manager = context.getSystemService(NsdManager::class.java)
            listener = object : NsdManager.RegistrationListener {
                override fun onServiceRegistered(i: NsdServiceInfo) { registered = true }
                override fun onRegistrationFailed(i: NsdServiceInfo, e: Int) { }
                override fun onServiceUnregistered(i: NsdServiceInfo) { registered = false }
                override fun onUnregistrationFailed(i: NsdServiceInfo?, e: Int) { }
            }
            manager?.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
        } catch (_: Exception) {
            // NSD no disponible (emulador/etc.): el TV aun puede entrar por IP/mDNS manual.
        }
    }

    fun stop() {
        if (registered) runCatching { manager?.unregisterService(listener) }
        registered = false
    }
}