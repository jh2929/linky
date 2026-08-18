// Captura de vídeo por X11 (x11grab de FFmpeg). Solo se usa cuando no hay
// Wayland disponible (sesión X11 pura). Entrega AVFrames en BGR0 para
// mantener idéntico el contrato con las rutas de Wayland.
#include "capture/video.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "common/log.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace linky {

namespace {

class X11VideoCapture : public VideoCapture {
 public:
  ~X11VideoCapture() override { stop(); }

  bool start(const VideoSpec& spec,
             std::function<void(AVFrame*)> on_frame) override {
    spec_ = spec;
    on_frame_ = std::move(on_frame);
    const char* display = getenv("DISPLAY");
    if (!display || !*display) {
      LERR("capx", "sin DISPLAY");
      return false;
    }
    avdevice_register_all();

    AVDictionary* opts = nullptr;
    std::string fps_s = std::to_string(spec_.fps ? spec_.fps : 30);
    std::string size_s = std::to_string(spec_.width) + "x" + std::to_string(spec_.height);
    av_dict_set(&opts, "framerate", fps_s.c_str(), 0);
    av_dict_set(&opts, "video_size", size_s.c_str(), 0);
    av_dict_set(&opts, "grab_x", "0", 0);
    av_dict_set(&opts, "grab_y", "0", 0);
    av_dict_set(&opts, "draw_mouse", "0", 0);

    const AVInputFormat* fmt = av_find_input_format("x11grab");
    int r = avformat_open_input(&fmt_ctx_, display, fmt, &opts);
    av_dict_free(&opts);
    if (r < 0) {
      LERR("capx", "x11grab abrir %s: %s", display, av_err2str(r));
      return false;
    }
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
      LERR("capx", "sin info de stream");
      return false;
    }
    thread_ = std::thread([this] { loop(); });
    return true;
  }

  void stop() override {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
  }

  bool ready() const override { return running_.load(); }
  const char* backend_name() const override { return "x11-x11grab"; }

 private:
  VideoSpec spec_;
  std::function<void(AVFrame*)> on_frame_;
  AVFormatContext* fmt_ctx_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};

  void loop() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* raw = av_frame_alloc();

    while (!stop_) {
      int r = av_read_frame(fmt_ctx_, pkt);
      if (r < 0) {
        if (r != AVERROR_EOF) LWRN("capx", "av_read_frame: %s", av_err2str(r));
        continue;
      }
      if (pkt->size == 0) continue;
      if (raw->buf) av_frame_unref(raw);
      raw->format = AV_PIX_FMT_BGR0;
      raw->width = spec_.width;
      raw->height = spec_.height;
      if (av_frame_get_buffer(raw, 1) == 0) {
        const int size =
            av_image_get_buffer_size(AV_PIX_FMT_BGR0, spec_.width, spec_.height, 1);
        memcpy(raw->data[0], pkt->data,
               static_cast<size_t>(std::min(pkt->size, size)));
        running_.store(true);
        if (on_frame_) on_frame_(raw);
      }
      av_packet_unref(pkt);
    }
    av_frame_free(&raw);
    av_packet_free(&pkt);
  }
};

// Capturador compuesto: intenta backends en orden hasta que uno arranca.
// Wayland: screencopy (wlr) → portal ScreenCast → x11grab (si hay Xwayland).
// X11: x11grab. Nunca deja de haber una captura si el entorno lo permite.
class FallbackVideoCapture final : public VideoCapture {
 public:
  FallbackVideoCapture() = default;
  // Modo forzado (LINKY_VIDEO_BACKEND): un único candidato precreado.
  FallbackVideoCapture(const char* name, VideoCapture* cap) : forced_(name, cap) {}
  ~FallbackVideoCapture() override { stop(); }

  bool start(const VideoSpec& spec,
             std::function<void(AVFrame*)> on_frame) override {
    spec_ = spec;
    on_frame_ = std::move(on_frame);

    std::vector<std::pair<const char*, VideoCapture*>> candidates;
    if (forced_.second) {
      candidates.push_back(forced_);
    } else {
      const char* wl = getenv("WAYLAND_DISPLAY");
      if (wl && *wl) {
        candidates.emplace_back("wayland-screencopy", create_screencopy_capture());
        candidates.emplace_back("wayland-portal", create_portal_capture());
      }
      if (getenv("DISPLAY"))
        candidates.emplace_back("x11-x11grab", new X11VideoCapture);
    }
    if (candidates.empty()) {
      LERR("capf", "sin backends de captura disponibles");
      return false;
    }

    for (auto& [name, candidate] : candidates) {
      if (!candidate) continue;
      if (candidate->start(spec_, on_frame_)) {
        LINF("capf", "backend de captura: %s", name);
        active_.reset(candidate);
        return true;
      }
      LERR("capf", "backend %s no disponible, probando siguiente", name);
      delete candidate;
    }
    return false;
  }

  void stop() override {
    if (active_) active_->stop();
  }

  bool ready() const override { return active_ && active_->ready(); }
  const char* backend_name() const override {
    return active_ ? active_->backend_name() : "ninguno";
  }

 private:
  VideoSpec spec_;
  std::function<void(AVFrame*)> on_frame_;
  std::pair<const char*, VideoCapture*> forced_{nullptr, nullptr};
  std::unique_ptr<VideoCapture> active_;
};

}  // namespace

VideoCapture* create_video_capture() {
  // LINKY_VIDEO_BACKEND fuerza un backend (screencopy|portal|x11) para
  // testing y para entornos con captura rota (p. ej. grim también cuelga).
  if (const char* forced = getenv("LINKY_VIDEO_BACKEND")) {
    if (strcmp(forced, "screencopy") == 0 && getenv("WAYLAND_DISPLAY")) {
      VideoCapture* c = create_screencopy_capture();
      if (c) return new FallbackVideoCapture("screencopy", c);
    } else if (strcmp(forced, "portal") == 0) {
      return new FallbackVideoCapture("portal", create_portal_capture());
    } else if (strcmp(forced, "x11") == 0 && getenv("DISPLAY")) {
      return new FallbackVideoCapture("x11", new X11VideoCapture);
    }
    LERR("capf", "LINKY_VIDEO_BACKEND inválido o backend no disponible: %s", forced);
  }
  const char* wl = getenv("WAYLAND_DISPLAY");
  const char* session = getenv("XDG_SESSION_TYPE");
  const bool wayland = (wl && *wl) || (session && strcmp(session, "wayland") == 0);
  if (!wayland && !getenv("DISPLAY")) return nullptr;
  return new FallbackVideoCapture;
}

}  // namespace linky
