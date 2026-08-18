package dev.linky.receiver.adapter

/**
 * Único punto de entrada de un protocolo de transmisión (Linky nativo, AirPlay, …).
 * El núcleo de la app nunca conoce la implementación; solo esta interfaz.
 */
interface ProtocolAdapter {
    fun start()
    fun stop()
}