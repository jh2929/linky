#pragma once
// Capa de captura de vídeo y audio. El núcleo solo conoce estas interfaces;
// las implementaciones (portal Wayland, X11, PipeWire) son reemplazables.
#include <functional>
#include <string>

extern "C" {
#include <libavutil/frame.h>
}

namespace linky {

struct VideoSpec {
  int width = 0;
  int height = 0;
  int fps = 0;
};

// Frames en BGR0 (el portal entrega DMABUF; swscale se encarga de convertir).
class VideoCapture {
 public:
  virtual ~VideoCapture() = default;
  virtual bool start(const VideoSpec& spec,
                     std::function<void(AVFrame*)> on_frame) = 0;
  virtual void stop() = 0;
  virtual bool ready() const = 0;
  virtual const char* backend_name() const = 0;
};

// PCM entrelazado 48 kHz, s16, 2 canales.
class AudioCapture {
 public:
  virtual ~AudioCapture() = default;
  virtual bool start(std::function<void(const int16_t*, int samples)> on_pcm) = 0;
  virtual void stop() = 0;
};

// Crea el mejor capturador de vídeo disponible para la sesión actual.
// Wayland → screencopy (wlr) → portal ScreenCast + PipeWire; X11 → x11grab.
VideoCapture* create_video_capture();

// Captura Wayland directa (zwlr_screencopy_manager_v1); nullptr si el
// compositor no lo implementa. Definida en video_screencopy.cpp.
VideoCapture* create_screencopy_capture();

// Captura vía portal ScreenCast (video_portal.cpp); nullptr si no hay bus.
VideoCapture* create_portal_capture();

// Crea la captura de audio del sistema (monitor de salida vía PipeWire).
AudioCapture* create_audio_capture(const std::string& device_hint);

}  // namespace linky
