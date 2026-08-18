# Linky

Transmisión de pantalla local (cualquier PC Linux → tu TV Android), sin nube ni cuentas.

```
┌─────────────┐  RTP/UDP:61034/35  ┌──────────────────┐
│  Emisor      │  RTCP:61036        │  Receptor TV      │
│  (Linux)     │◄──────────────────►│  (Android TV)     │
│  captura→HW  │  TCP/JSON:61032    │  MediaCodec       │
│  encode→RTP  │  hello/welcome     │  SurfaceView      │
└─────────────┘  NSD _linky._tcp    └──────────────────┘
```

- **Emisor** `sender/` (MIT): captura Wayland screencopy / portal / X11, encode H.264/HEVC por VAAPI, RTP+RTCP, descubrimiento por avahi/mDNS.
- **Receptor** `receiver/` (GPL-3.0): Android TV, control TCP/JSON, RTP/RTCP con NACK/PLI, MediaCodec → Surface, audio Opus → AudioTrack, auth por aceptación manual, anuncio `_linky._tcp` por NSD. Arquitectura de protocolos abierta vía `ProtocolAdapter` (AirPlay queda aislado en un módulo aparte, no implementado aún).

## Descargar sin compilar

Releases: https://github.com/jh2929/linky/releases

| Artefacto | Para | Instalación |
|---|---|---|
| `linky-receiver.apk` | Android TV | `adb install linky-receiver.apk`, o bájalo en el navegador del TV y ábrelo (permite sideload) |
| `linky-sender-linux-x86_64.tar.gz` | PC Linux x86_64 (glibc) | descomprimir y ejecutar `./linky-sender` |

## Probar (TV + PC en la misma red)

1. Instala el APK en el TV y ábrelo (pantalla negra "Esperando emisor…").
2. En el PC:
   ```sh
   tar xzf linky-sender-linux-x86_64.tar.gz
   ./linky-sender --fps 30 --bitrate 6000 --width 1920 --height 1080
   ```
   (en Wayland añade `LINKY_VIDEO_BACKEND=screencopy`; si no hay portal, usa `LINKY_VIDEO_BACKEND=x11` en X11/Xwayland).
3. El emisor detecta el TV por mDNS y conecta. Primera vez, el TV pregunta *"el emisor «X» quiere transmitir"* → **Aceptar** (queda en confianza).

### Sin TV a mano: probar el emisor contra el dumpreceiver

```sh
./linky-sender --connect 127.0.0.1 --width 1366 --height 768 --no-audio &
./linky-dumpreceiver --decode
```
Debe salir: frames, keyframes y 0 errores.

## Construir

**Emisor (Arch/CachyOS):**
```sh
sudo pacman -S cmake ffmpeg pipewire avahi wayland wayland-utils glib2 openssl
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cp build/linky-sender build/linky-dumpreceiver /usr/local/bin/
```

**Receptor / CI:** el workflow de GitHub Actions compila el APK (JDK 17 + SDK) y publica el artefacto `linky-receiver-apk`.

## Documentación técnica

- `docs/PLAN.md` — fases, estado, criterios verificados.
- `docs/ARCHITECTURE.md` — diseño del transporte, backends de captura (incluye §8.1 fallos de portal/screencopy del entorno), Roadmap.

## Licencias

- Emisor: MIT (`sender/LICENSE`).
- Receptor: GPL-3.0 (compatibilidad con UxPlay/AirPlay) (`receiver/LICENSE`).

## Roadmap

- AirPlay dentro de un adaptador aislado (submódulo UxPlay/AirPlayServer).
- Configuración de descodificación en TV, latencia ajustable.
- Emisor: audio AAC/Opus-AAC fallback, AV1 (HW), multi-monitor.