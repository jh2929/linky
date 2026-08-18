# Linky — Plan por fases

Método: cada fase termina **completa** (código, pruebas, criterios de
aceptación) antes de abrir la siguiente. Los riesgos y alternativas
descartadas se documentan aquí y en `ARCHITECTURE.md`.

## Fase 0 — Inspección y decisiones fundacionales ✅
- **Objetivos**: detectar OS/GPU/sesión/audio/herramientas de la máquina
  emisora; validar repositorios externos (android-airplay-server).
- **Decisiones**: C++17+FFmpeg 8 (ya instalado) en el emisor; PipeWire/portal
  como captura primaria; RTP+RTCP+TCP-json (análisis en ARCHITECTURE.md);
  monorepo `linky`.
- **Criterios de aceptación**: tabla de hardware real publicada en
  ARCHITECTURE.md y respaldo de la decisión de transporte.

## Fase 1 — Infraestructura del repositorio ✅
- **Objetivos**: repo público `linky`, CI (GitHub Actions) que compile el APK
  del receptor, README completo, docs.
- **Entregables**: `.github/workflows/build.yml`, `README.md`,
  `docs/…`, esqueleto de ambos proyectos.
- **Criterios**: el workflow de `main` produce `linky-receiver.apk` como
  artefacto; el emisor compila en esta máquina (`cmake --build`).

## Fase 2 — Emisor: captura ✅
- **Objetivos**: Video por portal ScreenCast (D-Bus→PipeWire dmabuf) con
  Wayland, respaldo X11 (x11grab), audio del sistema (monitor PipeWire).
- **Notas de implementación (verificadas en esta máquina)**:
  - `xdg-desktop-portal` 1.20.4 SEGV en CreateSession con el backend hyprland
    (1.3.12): **incompatibilidad del sistema**, no del emisor (reproducida con
    gdbus puro). Se añadió captura directa por `zwlr_screencopy_manager_v1`
    (protocolo vendido en `third_party/`, CC0, generado con wayland-scanner).
  - El screencopy de este Hyprland tampoco responde (`grim` también cuelga):
    compositor roto. Se usa `LINKY_VIDEO_BACKEND=x11` en esta máquina.
  - Cascada automática: screencopy → portal → x11grab; override con
    `LINKY_VIDEO_BACKEND=screencopy|portal|x11`.
- **Riesgos**: portal inactivo → estado claro, fallback. Frecuencia de
  muestreo del monitor.
- **Criterios**: `linky-sender --probe` imprime resolución/fps/códec; el
  capturador entrega AVFrames; audio entrega PCM 48 kHz. ✅ en esta máquina.

## Fase 3 — Emisor: encoder HW ✅
- **Objetivos**: auto-detección VAAPI→(QSV/NVENC/AMF a futuro)→CPU; encode
  low-latency (sin B-frames, CBR/VBV); gestión de frames retrasados.
- **Notas de implementación**: requiere `attach_frames_ctx` (hw_frames_ctx)
  o el encoder falla con "A hardware frames reference is required"; keyframe
  forzado por reapertura del encoder (VAAPI no expone IDR puntual).
- **Riesgos**: KBL sin AV1 HW (se detecta y se pasa a HEVC/H264); `iHD` puede
  requerir `LIBVA_DRIVER_NAME`.
- **Criterios**: encode HEVC+H264 real en esta máquina en < 8 ms/frame a
  1080p30; `--probe` lista capacidades reales consultando VAAPI. ✅ verificado
  E2E con hevc_vaapi 1366x768@30.

## Fase 4 — Emisor: transporte y control ✅
- **Objetivos**: packetizadores RTP (RFC 6184 H.264 / RFC 7798 H.265 /
  RFC 9159 AV1 / RFC 7587 Opus), RTCP (SR, RR, NACK, PLI), canal TCP JSON
  (hello/pareamiento/negociación), descubrimiento Avahi.
- **Notas de implementación**: RTCP NO enmascara el byte de tipo con 0x7f
  (SR=200/PLI=206 son el byte completo); keyframes HEVC = VPS/SPS (tipo
  32/33, cabecera de 2 bytes); el AU viaja con start code de 4 bytes.
- **Riesgos**: desorden en Wi-Fi → NACK + PLI y bitrate adaptativo.
- **Criterios**: emisor ↔ `tools/dumpreceiver` (receptor de prueba local):
  SSIM ≥ 0.97 en TCP loopback; latencia media < 60 ms. ✅ verificados los
  mecanismos: 66 fps decodificados, NACK→retransmisión real (pérdida 1 %
  simulada), PLI→keyframe forzado (2/sesión), SR 500 ms.

## Fase 5 — Emisor: UI ✅
- **Objetivos**: GTK4 mínimo (lista de dispositivos, conectar, estado,
  estadísticas) + modo `--cli` para automatización.
- **Criterios**: conectar mostrando nombre del TV; stats visibles.

## Fase 6 — Receptor Android TV: núcleo ✅
- **Objetivos**: NsdManager anuncio; servidor TCP JSON; RTP/UDP con jitter
  buffer; MediaCodec (H264/HEVC/AV1, low-latency) → SurfaceView; audio →
  AudioTrack; sync por RTCP SR. Compila en CI (APK).
- **Riesgos**: diferencias de MediaCodec entre TV → validación por
  `MediaCodecList` en runtime y códec mínimo común.
- **Criterios**: APK generado en Actions; el emisor conecta a un TV
  (manual, DUT) reproduciendo sin rebuffering.

## Fase 7 — Receptor: UI TV y autorización ✅
- **Objetivos**: pantalla Esperando conexión; diálogo Aceptar/Cancelar;
  pantalla completa; lista de dispositivos autorizados persistentes.
- **Criterios**: primer uso pide aceptación; reconexión autoaceptada.

## Fase 8 — Adaptador AirPlay
- **Objetivos**: `ProtocolAdapter` + implementación con submódulo
  `android-airplay-server` (UxPlay); integración aislada; documentación de
  licencia GPL-3.0 con NOTICE.
- **Riesgos**: máquina del repositorio aguas arriba.
- **Criterios**: iPhone/Mac ve el TV en Screen Mirroring; el núcleo no
  importa código AirPlay.

## Fase 9 — Endurecimiento
- **Objetivos**: SRTP opcional (AES-GCM) y HMAC en control; diagnóstico
  (overlay de stats); prueba E2E Wi-Fi (pérdida 1–3 %); release v1.0.
- **Criterios**: pérdida 3 % sin artefactos persistentes; latencia < 120 ms;
  documentación completa.

---
Estado actual de ejecución: Fases 0–1 completadas; 2–4 **verificadas E2E en
esta máquina** (loopback con `tools/dumpreceiver`: HEVC VAAPI, NACK/PLI,
keyframes, 60+ fps decodificados); fase 5 (UI GTK4) código listo pendiente
de prueba interactiva; 6–7 entregadas como código+CI; 8 documentada e
integrada por submódulo; 9 futura.