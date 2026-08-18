# Linky — Arquitectura del sistema

Receptor universal para Android TV + emisor para Linux. Transmisión de pantalla
de muy baja latencia, 100 % en red local. Sin nube, sin cuentas, sin Internet.

---

## 1. Objetivos

| Objetivo | Prioridad |
|---|---|
| Latencia mínima (objetivo < 100 ms de extremo a extremo en LAN) | Crítica |
| Estabilidad (sin reinicios, reconexión automática) | Crítica |
| Máxima calidad posible dentro de la restricción de latencia | Alta |
| Aceleración por hardware automática | Alta |
| Compatibilidad Wayland + X11, Android TV + Google TV | Alta |
| Arquitectura modular, protocolo-agnóstica en el receptor | Alta |
| Consumo mínimo de CPU | Media |

Restricción transversal: **nunca sacrificar estabilidad por funciones nuevas**.
Cada módulo se diseña como un componente independiente con una interfaz única
(SOLID, Clean Architecture), de forma que los adaptadores de protocolo
(AirPlay, Linky nativo, futuros: Windows/Miracast…) sean intercambiables sin
tocar el núcleo.

---

## 2. Contexto de la máquina emisora (inspección real, 2026-08-18)

El emisor se ha diseñado y compilado contra el hardware real de esta máquina:

| Componente | Valor detectado | Implicación de diseño |
|---|---|---|
| OS | CachyOS (rolling, base Arch), kernel 7.0.10-cachyos | diana x86_64 Linux; paquetes Arch añadidos al README |
| Sesión gráfica | Wayland (Hyprland), `wayland-1`; X11 también disponible | Captura primaria vía PipeWire/portal; respaldo X11 (x11grab) |
| GPU | Intel UHD Graphics 620 (Kaby Lake-R GT2) | VAAPI `iHD`; QSV; sin AV1 en hardware → negociación elige H.265/H.264 |
| Aceleración disponible | `/dev/dri/renderD128`, `iHD_drv_video.so`, FFmpeg 8.1.1 | `h264_vaapi`, `hevc_vaapi` disponibles; AV1 solo software (no usado) |
| Audio | PipeWire 1.6.6, monitor `alsa_output…analog-stereo.monitor` 48 kHz | Captura de audio de sistema (loopback) vía PipeWire |
| mDNS | avahi-daemon activo | Descubrimiento DNS-SD con Avahi |
| Toolkit UI | GTK4 4.22.4 | Interfaz mínima GTK4 + modo CLI headless |
| CPU | 8 hilos | Límites de hilos en codificación para dejar cabeza libre |

Regla de diseño derivada: la capa de encoders es **auto-detectiva**
(VAAPI → QSV → NVENC → AMF → CPU) y la negociación de códecs es bidireccional;
en esta máquina el flujo real será `Wayland/portal → swscale → hwupload →
h264_vaapi|hevc_vaapi`, con `hevc_vaapi` preferido si el receptor lo soporta.

---

## 3. Visión de módulos

### 3.1 Receptor Android TV (`receiver/`)

```
┌────────────────────────────────────────────────────────────────┐
│                        UI (TV)                                 │
│  Esperando ●  ·  "Equipo X desea transmitir" Aceptar/Cancelar │
│                           │ pantalla completa                  │
└───────────────┬────────────────────────────────────────────────┘
                ▼
┌──────────────────────────  Receiver Core  ─────────────────────┐
│  SessionManager  ──  AuthorizationStore (dispositivos ok)      │
│       ▲                    ▲                                   │
│       │ Control TCP/JSON   │ Eventos (conectado, perdido…)     │
│  ┌────┴────┐   ┌───────────┴───────────┐                       │
│  │ Linky   │   │ AirPlayAdapter        │  ← aislado,           │
│  │ Adapter │   │ (android-airplay-     │    reemplazable       │
│  │ (RTP    │   │  server/UxPlay)       │                       │
│  │ nativo) │   └───────────────────────┘                       │
│  └────┬────┘                                                   │
│       ▼                                                         │
│  MediaEngine: JitterBuffer → MediaCodec(HW) → SurfaceView      │
│               RTP/RTCP(mDNS)    → MediaCodec → AudioTrack      │
│               A/V sync por RTCP SR                              │
└────────────────────────────────────────────────────────────────┘
```

- **Discovery**: NsdManager anuncia `_linky._tcp` (puerto de control + TXT con
  capacidades). Sin dependencias externas.
- **ProtocolAdapter** (interfaz): `start()`, `stop()`, `onControlMessage()`,
  `onMedia(ByteBuffer…)`, `onStats()`. LinkyAdapter y AirPlayAdapter la
  implementan. El núcleo solo conoce la interfaz → cualquier protocolo futuro
  entra como un adaptador nuevo.
- **MediaEngine**: buffer de jitter (~30 ms), MediaCodec con
  `KEY_LOW_LATENCY`/`KEY_MAX_B_FRAMES=0`, salida a `SurfaceView` (mínima
  latencia de composición en TV) y audio a `AudioTrack` con
  `AUDIO_OUTPUT_FLAG_LOW_LATENCY`. Sincronización A/V: audio es el maestro;
  el video se alinea con los timestamps RTCP SR del emisor.

### 3.2 Emisor Linux (`sender/`)

```
Captura(Video) ──► Encoder(HW auto) ──► Packetizer(RTP) ──► UDP/RTCP ──┐
Captura(Audio) ──► Encoder(Opus)  ──► Packetizer(RTP) ──► UDP/RTCP ──┤
                                                                      ▼
Discovery(mDNS) ─► Session/Control(TCP+JSON, pairing) ◄──────────  Receptor
                                                                      ▲
UI (GTK4 o CLI) ─► Stats/Diagnostics ◄─── RTCP RR/NACK/PLI() ─────────┘
```

Módulos: `discovery` (Avahi), `capture` (portal PipeWire / X11 / audio),
`encode` (auto-detección HW), `rtp` (paquetización mpaa), `session` (control
TCP + negociación + pareamiento), `ui`, `logger`, `stats`.

---

## 4. Análisis de transporte (decisión técnica justificada)

Objetivo: LAN cableada/Wi-Fi doméstica, < 100 ms E2E, aceleración HW en ambos
extremos, implementación mantenible.

| Criterio | **RTP/UDP + RTCP** | WebRTC | QUIC | SRT |
|---|---|---|---|---|
| Latencia teórica | ~0 ms (sin reordenación) | 30–100 ms (ICE+DTLS+SRTP+jitter, renegociación) | 0–1 RTT en streams; HoL en los confiables | 1–2 RTT (ARQ) |
| Sobrecarga por paquete | 12 B (RTP) + 8 B IP | SRTP+ICE+DTLS ≈ 45–60 B | ≈ 30–40 B + frame deferido | ≈ 30 B |
| Sobre redes LAN | Excelente (pérdida ≈ 0) | Sobredimensionado | Bien para control; datagramas experimentales para media | Pensado para WAN con pérdida alta |
| Decodificación directa en Android TV | Sí: MediaCodec consume los flujos despaquetizados directamente | Requiere libwebrtc/NativeDecoder (aportes de complejidad y latencia) | No hay pila QUIC nativa en Android TV | No hay pila nativa en Android TV |
| App-agnosticismo (RTCP SR para A/V sync, SRTP opcional) | Modelo probado: AirPlay, Miracast, DVB, SIP | Cerrado (libwebrtc) | Relativamente joven | Enfocado a transporte |
| Complejidad de implementación | Baja-media (RFC 6184/7798/9159/7587) | Muy alta (libwebrtc completa) | Alta en pila completa, media con quiche/quinn | Media |
| Control/pareamiento | TCP JSON propio (sencillo, auditable) | Canal de señalización externo | Forma parte de QUIC (ventaja) | No prevé control |

**Decisión: RTP sobre UDP para media + TCP (JSON) para control, con RTCP
(SR, RR, NACK, PLI) para sincronía y pérdida. SRTP opcional vía comodín en
fase de endurecimiento.**

Justificación técnica:

1. **La LAN no es Internet.** Con pérdida ≈ 0, los mecanismos anti-pérdida de
   SRT/QUIC/WebRTC (ARQ, reeordenación agresiva) aportan latencia sin
   beneficio medible. Un NACK por fragmento perdido y una solicitud de
   keyframe (PLI) bastan.
2. **MediaCodec en Android TV decodifica Annex B/RTP tal cual.** Con WebRTC o
   QUIC tendríamos que reconstruir el flujo dentro de una pila pesada
   (libwebrtc ≈ 20 MB, SIGSEGV frecuentes en TV) antes de alimentar MediaCodec;
   con RTP el despaquetizador es ~200 líneas y determinista.
3. **Los precedentes de latencia mínima usan RTP:** AirPlay (RTSP control +
   RTP media con SRTP), Miracast (RTP/RTSP), DASH-low-latency no (excluido:
   segmentos). Nuestro receptor comparte mentalidad con AirPlay, lo que
   simplifica el adaptador.
4. **Sincronización A/V** con RTCP SR (NTP↔RTP) es directa y soportada por
   ambos lados.
5. **Coste nulo en los extremos:** el emisor también consume media vía FFmpeg
   → la ruta `avformat → RTP muxer` existe, pero implementamos muxer propio en
   C++ para control fino de fragmentación, timestamps y reenvíos.

Se descartan explícitamente: **WebRTC** (complejidad/peso/latencia extra sin
beneficio en LAN), **QUIC** (la ventaja real está en el control, que ya
cubrimos con TCP+JSON; para media es HoL o feature experimental),
**SRT** (latencia añadida por ARQ pensada para WAN pérdida).

---

## 5. Negociación de códecs

Ambos extremos publican capacidades (TXT en mDNS + campos en el handshake):

```
video: h264 (baseline→high), hevc (main), av1  → orden de preferencia: av1 > hevc > h264
audio: opus (RFC 7587, 48k), aac (LATM, 48k)   → orden: opus > aac
```

Reglas:

- El emisor sondea encoders HW (VAAPI/QSV/NVENC/AMF/CPU) y solo ofrece lo que
  realmente puede codificar; el receptor solo acepta lo que MediaCodec
  garantiza (`MediaCodecList` con `SECURE`/`SOFTWARE` descontado para
  HEVC/AV1 HW).
- Elección final: mejor códec mutuo ordenando la lista de preferencias del
  receptor.
- Parámetros fijos de baja latencia: `max_b_frames=0`, `trellis` desactivado,
  GOP/IDR 1–2 s, tasa adaptada por ancho de banda (estimación por RTCP RR y
  perdidas) y por sobrecarga de CPU del encoder.

En esta máquina (KBL sin AV1 HW): par HEVC↔HEVC si el TV lo decodifica por HW
(lo normal desde 2016), si no H.264 High. AV1 entra automáticamente cuando
exista HW (o CPU si se habilita explícitamente en futuro).

---

## 6. Descubrimiento (mDNS/DNS-SD)

- Receptor: anuncia `_linky._tcp` (TXT: `model`, `codecs`, `apiver`, `ver`).
  El módulo AirPlay anuncia por su cuenta `airplay._tcp`/`raop._tcp` (lo hace
  android-airplay-server/UxPlay internamente) — invisible para el núcleo.
- Emisor: navega `_linky._tcp` (Avahi) y lista receptores en la UI sin IP
  manual. La IP file:// nunca es necesaria (pero `--ip` existe como override
  de diagnóstico).

## 7. Seguridad / pareamiento

1. **Primer uso**: handshake `hello` (con `senderId` = SHA-256 de identidad
   del emisor). El receptor muestra *«Equipo 'X' desea transmitir su
   pantalla»* → Aceptar/Cancelar.
2. **Recordar**: `senderId` persistido (SharedPreferences, lista de
   autorizados). Reintentos del mismo emisor se autoaceptan (asociación
   previa), manteniendo SIEMPRE el dialogo la primera vez.
3. **Autenticación de todo el tráfico**: opción `requirePair` que deriva una
   clave de sesión (HKDF sobre secreto compartido del pareado) y firma
   HMAC-SHA256 cada mensaje de control; SRTP (AES-GCM) para media queda como
   extensión en fase de endurecimiento (fase 9). En LAN doméstica la
   autorización por dispositivo es el modelo de confianza primario.
4. Denegación, expiración de sesión, y límite de una sesión activa a la vez.

## 8. Estrategia de latencia (emisor)

| Etapa | Mecanismo |
|---|---|
| Captura | Cascada automática: `zwlr_screencopy` (wlr-screencopy, sin portal) → Portal ScreenCast → dmabuf PipeWire → VAAPI directo (zero-copy cuando aplica); descartar frames si el encoder va atrasado (>1 frame pendiente) |
| Codificación | baja latencia: sin B-frames, CBR con VBV estricto, `async_depth` 1–2 |
| Paquetización | sin re-encolar: el RTP sale con el timestamp del capturador (sistema) |
| Transporte | UDP sin esperas; no usar pausas por RTCP |
| Receptor | jitter buffer dinámico 20–40 ms; `KEY_LOW_LATENCY`; SurfaceView; audio con `LOW_LATENCY` |
| Sync A/V | audio maestro; desvío de video ≤ 1 frame |

### 8.1 Captura y sistemas con portal/compositor rotos

- `xdg-desktop-portal` ≥ 1.20 con el backend hyprland 1.3.12 crashea (SEGV)
  en `CreateSession` (assert de objeto path en el frontal); el screencopy del
  compositor puede no responder (reproducido también con `grim`).
- El emisor no depende del portal: el backend `wlr-screencopy` (protocolo
  vendido en `sender/third_party/`, CC0, generado con `wayland-scanner` en
  build) captura directo contra el compositor, y `x11grab` es el último
  recurso (sesiones X11 o compositores rotos).
- Selección: `LINKY_VIDEO_BACKEND=screencopy|portal|x11` fuerza backend
  (para testing y entornos degradados); por defecto la cascada automática.

## 9. Adaptador AirPlay (aislamiento y licencias)

- `ProtocolAdapter` es la única puerta: `start/stop/onControl/onMedia/onStats`.
- La implementación AirPlay vive en `receiver/airplay/` como **submódulo git**
  apuntando a `jqssun/android-airplay-server` (que integra UxPlay) y se
  instancia tras un muro `AirPlayAdapter` que traduce su API (servicios,
  callbacks) a `ProtocolAdapter`. El núcleo **jamás importa** clases de
  AirPlay.
- **Licencias**: UxPlay y android-airplay-server son GPL-3.0. El APK del
  receptor que incorpora el módulo AirPlay es una obra derivada → la
  **aplicación receptora se publica bajo GPL-3.0** (con [NOTICE], atribución y
  separación clara de lo original). El **emisor Linux es MIT** y no tiene
  ningún vínculo con AirPlay. Sustituir el adaptador por uno permisivo en el
  futuro permite relicenciar el receptor sin rediseñar nada.

## 10. Plan por fases

Véase [`PLAN.md`](PLAN.md). Resumen: F0 inspección+máquina; F1 monorepo+CI;
F2 emisor captura; F3 encoder HW; F4 transporte/control; F5 UI; F6 receptor
núcleo; F7 receptor UI/autorización; F8 adaptador AirPlay; F9 endurecimiento,
SRTP, diagnóstico y pruebas E2E.

## Riesgos principales

| Riesgo | Mitigación |
|---|---|
| Portal ScreenCast bloqueado o sin monitor (headless/TV) | Fallback X11 (x11grab); error claro y estado en UI |
| HW decoder del TV sin HEVC/AV1 | Negociación objetiva (`MediaCodecList`), H.264 como suelo universal |
| Pérdida en Wi-Fi → artefactos | NACK selectivo + PLI; bitrate adaptativo por RTCP RR |
| Repos de AirPlay abandonados | Aislamiento total + pin de commit + flag `airplay.enabled` en CI |
| Latencia del compositor Wayland | dmabuf directo del portal; sin pasos de copia dobles |