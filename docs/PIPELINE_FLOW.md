# Pipeline de Streaming Linky — Documentación y Diagnóstico

Este documento detalla el flujo de datos y control de extremo a extremo (E2E) entre el Emisor Linux y el Receptor Android TV, las fallas críticas identificadas en cada punto y la estrategia de verificación por subfases.

---

## 1. Diagrama de Flujo del Pipeline E2E

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│                                 FASE 1A: CONECTIVIDAD                             │
│                                                                                   │
│  [Emisor Linux]                                           [Receptor Android TV]   │
│  Avahi Browser (mDNS) ──────── _linky._tcp ─────────────► NsdAnnouncer (mDNS)    │
│  TCP Connect :61032   ──────────────────────────────────► ControlServer :61032    │
│  Send {hello}         ──────────────────────────────────► onHello (auth check)   │
│  Receive {welcome}    ◄── {welcome: codec, ports...} ───  Accept (pairing/trusted)│
│  (Session Active)                                         (Session Active)        │
└────────────────────────────────────────┬──────────────────────────────────────────┘
                                         │
┌────────────────────────────────────────▼──────────────────────────────────────────┐
│                             FASE 1B: PIPELINE RTP Y TRANSPORTE                    │
│                                                                                   │
│  [Emisor: Capture + HW Encode]                            [Receptor: RTP Network] │
│  Wayland/X11 Frame -> VAAPI/FFmpeg                        UDP :61034 (Video)      │
│  Access Unit (AU) con SPS/PPS/IDR                         UDP :61035 (Audio)      │
│  RTP Packetizer (RFC 6184 / RFC 7798)                     UDP :61036 (RTCP)       │
│  Fragmentación FU-A (H.264) / FU (H.265) ──► UDP ───────► RtpReceiver             │
│  RTCP Sender Report (SR) ──────────────────► UDP ───────► Jitter Buffer (20-40ms) │
│  RTCP NACK/PLI Handler ◄───────────────────◄ UDP ◄─────── NACK / PLI Generator   │
│                                                           Reensamblador NAL / AU  │
└────────────────────────────────────────┬──────────────────────────────────────────┘
                                         │
┌────────────────────────────────────────▼──────────────────────────────────────────┐
│                         FASE 1C: DECODIFICACIÓN Y RENDERIZADO                     │
│                                                                                   │
│  Access Units completos (SPS, PPS, IDR, Slices)                                  │
│                          │                                                        │
│                          ▼                                                        │
│       MediaCodec (video/avc o video/hevc) con LOW_LATENCY                         │
│       - Inicializado con resolución y framerate reales (hello vres/vrate)         │
│       - Buffer de entrada con start codes 0x00000001 y NAL headers válidos        │
│                          │                                                        │
│                          ▼                                                        │
│       SurfaceView (Hardware Surface directa) -> Renderizado en pantalla           │
│                                                                                   │
│       Audio Opus (48 kHz) -> MediaCodec / AudioTrack LOW_LATENCY                  │
└───────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Puntos Críticos y Causas Raíz Identificadas

### 2.1. Fase 1A – Conectividad y Control
1. **Dangling Reference en el Emisor (`sender_app.cpp:190`):**
   - En `begin_media()`, la lambda `[this, &w]` capturaba por referencia el objeto de bienvenida `w` (`ControlClient::Welcome`), que era una variable en la pila destruida inmediatamente al terminar la llamada. El hilo `rtcp_thread_` causaba comportamiento indefinido y lectura de memoria corrupta.
2. **Ciclo de vida y Reutilización de Sockets UDP (`RtpReceiver.kt`):**
   - Los sockets en los puertos 61034, 61035 y 61036 no habilitaban `reuseAddress = true`. Un cierre y reconexión provocaba excepciones `BindException: Address already in use`.
3. **Condición de carrera en RTCP (`RtpReceiver.kt`):**
   - `videoLoop()` invocaba `nack()` en cuanto detectaba un hueco de secuencia, llamando a `rtcpSend()`. Sin embargo, `rtcp` se instanciaba dentro de un hilo secundario (`rtcpLoop()`) sin sincronización, provocando NullPointerException o descarte silencioso.
4. **Detección de Pantalla en Linux (`main.cpp`):**
   - `screen_size()` llamaba rígidamente a `xrandr --current`, que en entornos Wayland puros (Hyprland, Sway, sin Xwayland activo) falla, saliendo el proceso con código de error 2 sin llegar a conectar.

### 2.2. Fase 1B – Pipeline RTP
1. **Corrupción de NAL Units en Reensamblador (`RtpReceiver.kt:117-131`):**
   - Para H.264 (RFC 6184, FU-A tipo 28): El despaquetizador tomaba el payload a partir de `off + 2` pero **no reconstruía el byte de cabecera NAL** `(fu_indicator & 0xE0) | (fu_header & 0x1F)`.
   - Para H.265 (RFC 7798, FU tipo 49): Omitía la reconstrucción del encabezado de 2 bytes de HEVC.
   - **Efecto:** Todas las tramas fragmentadas perdían su identidad de NAL. `MediaCodec` descartaba silenciosamente el 100 % de los paquetes de vídeo por considerarlos stream corrupto.
2. **Validación de Delimitación de Access Unit (AU):**
   - Es necesario garantizar que los paquetes NAL de configuración (SPS, PPS, VPS) se agreguen al inicio del Access Unit del Keyframe para que el decodificador nunca reciba un slice sin sus parámetros de secuencia.

### 2.3. Fase 1C – Decodificación y Renderizado
1. **Crash por resolución 0x0 (`MediaEngine.kt:53`):**
   - Se ejecutaba `MediaFormat.createVideoFormat(videoMime, 0, 0)`.
   - La mayoría de decodificadores de hardware en chipsets Android TV (Amlogic, Realtek, MediaTek) rechazan terminantemente resoluciones en 0 y lanzan `CodecException` o `IllegalArgumentException`.
   - Solución: Alimentar `MediaFormat` con la resolución y framerate reportados en el handshake (`vres`, `vrate`).

---

## 3. Matriz de Criterios de Aceptación por Subfase

| Subfase | Criterios Objetivos para Cerrar la Fase |
|---|---|
| **Fase 1A: Conectividad** | 1. Handshake TCP exitoso en menos de 500 ms.<br>2. Sockets UDP (61034-61036) enlazados limpiamente con `SO_REUSEADDR`.<br>3. Zero crashes por condiciones de carrera o referencias inválidas.<br>4. Reconexión limpia inmediata tras desconexión abrupta. |
| **Fase 1B: Pipeline RTP** | 1. NAL headers reconstruidos 100 % fieles a RFC 6184 y RFC 7798.<br>2. Verificación de reensamblado de paquetes grandes (> 1400 bytes).<br>3. NACK funcional y verificado ante pérdida inducida de paquetes.<br>4. Access Units íntegros entregados al sink de media. |
| **Fase 1C: Decodificación** | 1. `MediaCodec` inicializado con resolución y fps reales.<br>2. Decodificación de vídeo fluida en SurfaceView a 30/60 fps.<br>3. Streaming ininterrumpido sin congelamientos durante al menos 30 min.<br>4. Recuperación automática ante petición de Keyframe (PLI). |
