# Linky

> **Transmisión de pantalla local de ultra baja latencia (< 100 ms) desde Linux a Android TV, con soporte integrado para Apple AirPlay.** 100 % privado, sin servidores externos, sin cuentas y sin depender de Internet.

```
┌───────────────────────────┐         RTP/UDP:61034 (Video)         ┌───────────────────────────┐
│       Emisor Linux        │         RTP/UDP:61035 (Audio)         │    Receptor Android TV    │
│  - Captura Wayland / X11  │◄─────────────────────────────────────►│  - MediaCodec HW (Low Lat)│
│  - Encode HW (VAAPI/FFmpeg│         RTCP/UDP:61036 (Sync/NACK)    │  - SurfaceView Fullscreen │
│  - App Oficial Libadwaita │                                       │  - UI Minimalista 10-Foot │
│  - linky-stream CLI       │         TCP/JSON:61032 (Control)      │  - Soporte Apple AirPlay  │
│  - linky-tray (SystemTray)│◄─────────────────────────────────────►│  - Anuncio mDNS (_linky)  │
└───────────────────────────┘          mDNS (_linky._tcp)           └───────────────────────────┘
```

---

## ✨ Características Principales

* ⚡ **Ultra Baja Latencia (< 100 ms):** Pipeline optimizado con codificación por hardware en Linux (`VA-API` / `FFmpeg`) y decodificación hardware con `MediaCodec` `KEY_LOW_LATENCY` en Android TV.
* 🖥️ **Aplicación Oficial para Linux (`linky-desktop`):** Interfaz gráfica moderna desarrollada con **Libadwaita** y **GTK4**. Descubrimiento automático de televisores por mDNS, selección de monitores, presets de FPS (30/60) y slider de bitrate dinámico.
* 📺 **Experiencia TV Minimalista (10-Foot UI):**
  * Diseño estético monocromo y sobrio en escala de grises y negro profundo.
  * Navegación 100 % adaptada a mando a distancia (D-Pad) con foco de alto contraste.
  * Cuadro modal de pareamiento (*"Aceptar y Recordar"*) con un solo clic.
  * HUD de rendimiento en pantalla (FPS, Bitrate, Códec) alternable con el botón `[OK]` o `[Abajo]` del control remoto.
* 🍎 **Compatibilidad Mixta Apple AirPlay:** Módulo AirPlay integrado y aislado para duplicar pantalla directamente desde iPhone, iPad o Mac.
* 🛡️ **Privacidad Absoluta:** No envía telemetría ni datos a la nube. Todo el tráfico de vídeo y audio viaja en tu red local (LAN).

---

## 📦 Descargas e Instalación

Los paquetes precompilados de cada versión están disponibles en [Releases de GitHub](https://github.com/jh2929/linky/releases).

| Formato | Plataforma | Instalación rápida |
|---|---|---|
| **`linky-receiver.apk`** | Android TV / Google TV | `adb install -r linky-receiver.apk` *(o abrir desde el navegador del TV / pendrive)* |
| **`linky-sender-x86_64.AppImage`** | Linux (Universal) | `chmod +x linky-sender-x86_64.AppImage && ./linky-sender-x86_64.AppImage` |
| **`linky-sender_amd64.deb`** | Debian / Ubuntu / Mint / Pop!_OS | `sudo apt install ./linky-sender_amd64.deb` |
| **`linky-sender.x86_64.rpm`** | Fedora / RHEL / openSUSE | `sudo dnf install ./linky-sender.x86_64.rpm` |
| **`linky-sender-linux-x86_64.tar.gz`** | Arch / Tarball Genérico | Descomprimir y ejecutar `./linky-desktop` o `./linky-stream` |

---

## 🚀 Guía de Uso

### 1. En el Televisor (Android TV / Google TV)
1. Instala y abre la app **Linky** en tu televisor.
2. Verás la pantalla de espera minimalista mostrando el nombre de tu TV y su **Dirección IP local** (ej. `192.168.1.45`).
3. El televisor queda listo automáticamente a la espera de transmisiones.

### 2. Desde tu PC Linux
* **Modo Gráfico (Recomendado):**
  Abre **Linky Stream** desde tu lanzador de aplicaciones (o ejecuta `linky-desktop`).
  Tu televisor aparecerá automáticamente en la lista de dispositivos descubiertos. Ajusta los FPS o Bitrate deseados y pulsa **"Transmitir"**.
* **Modo Terminal / CLI:**
  ```bash
  # Conexión automática por nombre descubierto:
  linky-stream

  # O especificando resolución y destino directo:
  linky-stream --connect 192.168.1.45 --fps 60 --bitrate 8000
  ```

### 3. Primera Conexión (Autorización)
La primera vez que un ordenador transmite, el televisor mostrará el diálogo:
> *«El equipo "PC-Linux" quiere transmitir su pantalla»*
> Presiona **[Aceptar y Recordar]** con el mando a distancia del TV. En futuras ocasiones se conectará automáticamente.

### 4. Modo Apple AirPlay
En la pantalla principal del televisor, selecciona el botón **[AirPlay (Apple)]**. El televisor quedará visible para el menú "Duplicar Pantalla" de tu iPhone, iPad o Mac.

---

## 🛠️ Compilación desde el Código Fuente

### Requisitos en Linux (Arch / Ubuntu / Fedora)
* **Compilador:** C++17, CMake 3.20+
* **Librerías:** `ffmpeg` (libavcodec, libavformat, libswscale), `pipewire`, `avahi-client`, `wayland-client`, `gtk4`, `libadwaita-1`, `openssl`.

```bash
# Clonar repositorio con submódulos
git clone --recursive https://github.com/jh2929/linky.git
cd linky

# Compilar Emisor Linux
cmake -B sender/build -S sender -DCMAKE_BUILD_TYPE=Release
cmake --build sender/build -j$(nproc)

# Ejecutables resultantes en sender/build/:
# - linky-desktop (App gráfica oficial Libadwaita)
# - linky-stream (Motor CLI headless)
# - linky-tray (Bandeja del sistema)
# - linky-dumpreceiver (Receptor de pruebas de loopback local)
```

### Compilar Receptor Android TV
Requiere Android Studio o JDK 17 + Android SDK / NDK:
```bash
cd receiver
./gradlew assembleDebug

# APK generado en:
# receiver/app/build/outputs/apk/debug/app-debug.apk
```

---

## 📐 Arquitectura y Documentación Técnica

* [`docs/PIPELINE_FLOW.md`](docs/PIPELINE_FLOW.md) — Flujo E2E detallado (Handshake → RTP → MediaCodec → Renderizado) y diagnóstico de capas.
* [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — Diseño de transporte RTP/RTCP, análisis de latencia y capas de captura.
* [`docs/preview_tv_ui.html`](docs/preview_tv_ui.html) — Vista previa interactiva de la interfaz del televisor.

---

## ⚖️ Licencias

* **Emisor Linux (`sender/`):** Licencia MIT ([`sender/LICENSE`](sender/LICENSE)).
* **Receptor Android (`receiver/`):** Licencia GPL-3.0 por integración de submódulo AirPlay ([`receiver/LICENSE`](receiver/LICENSE)).
