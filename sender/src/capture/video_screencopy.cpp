// Captura Wayland directa vía zwlr_screencopy_manager_v1 (wlr-screencopy).
// Evita el portal ScreenCast: funciona con compositores wlroots
// (Hyprland, Sway, …) sin diálogos ni dependencias de xdg-desktop-portal.
//
// Flujo por frame (protocolo v1/v3):
//   1. zwlr_screencopy_manager_v1.capture_output -> objeto frame
//   2. evento "buffer"  -> geometría y formato (sin haber hecho copy)
//   3. crear wl_buffer sobre pool wl_shm, frame.buffer(buf), frame.copy()
//   4. eventos "damage" y "ready"/"failed" -> copiar píxeles a BGR0
//   5. destruir el objeto frame y el wl_buffer; repetir
//
// El pool wl_shm se reutiliza entre frames (geometría estable en la práctica);
// si cambia, se recrea.

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

extern "C" {
#include <libavutil/time.h>  // FFmpeg 8 no la envuelve en extern "C"
}
#include <wayland-client.h>

#include "capture/video.h"
#include "common/log.h"
#include "common/util.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

namespace {

constexpr const char* kScreencopyV1 = "zwlr_screencopy_manager_v1";

struct WlGlobals {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_shm* shm = nullptr;
  zwlr_screencopy_manager_v1* mgr = nullptr;
  wl_output* output = nullptr;
};

struct FrameState {
  WlGlobals* wl = nullptr;
  wl_shm_pool* pool = nullptr;
  uint8_t* mem = nullptr;
  size_t pool_size = 0;
  uint32_t format = 0;
  int width = 0;
  int height = 0;
  int stride = 0;
  bool ready = false;
  bool failed = false;
};

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* iface, uint32_t version) {
  auto* wl = static_cast<WlGlobals*>(data);
  const std::string id(iface);
  if (id == wl_shm_interface.name) {
    wl->shm = static_cast<wl_shm*>(
        wl_registry_bind(registry, name, &wl_shm_interface, 1));
  } else if (id == kScreencopyV1) {
    const uint32_t v = std::min(version, 3u);
    wl->mgr = static_cast<zwlr_screencopy_manager_v1*>(
        wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, v));
  } else if (id == wl_output_interface.name && !wl->output) {
    wl->output = static_cast<wl_output*>(
        wl_registry_bind(registry, name, &wl_output_interface, 1));
  }
}

void registry_remove(void*, wl_registry*, uint32_t) {}

void frame_buffer(void* data, zwlr_screencopy_frame_v1* frame, uint32_t format,
                  uint32_t width, uint32_t height, uint32_t stride) {
  auto* fs = static_cast<FrameState*>(data);
  // Algunos compositores anuncian formato 0 (inválido); XRGB8888 es el
  // formato wl_shm más compatible y el que wlroots garantiza.
  if (format == 0) format = WL_SHM_FORMAT_XRGB8888;
  fs->format = format;
  fs->width = static_cast<int>(width);
  fs->height = static_cast<int>(height);
  fs->stride = static_cast<int>(stride);
  (void)frame;
}

void frame_flags(void* data, zwlr_screencopy_frame_v1* frame, uint32_t flags) {
  (void)data;
  (void)frame;
  (void)flags;
}

void frame_ready(void* data, zwlr_screencopy_frame_v1* frame, uint32_t,
                 uint32_t, uint32_t) {
  auto* fs = static_cast<FrameState*>(data);
  fs->ready = true;
  LINF("caps", "screencopy: ready");
  (void)frame;
}

void frame_failed(void* data, zwlr_screencopy_frame_v1* frame) {
  auto* fs = static_cast<FrameState*>(data);
  fs->failed = true;
  LINF("caps", "screencopy: failed!");
  (void)frame;
}

void frame_damage(void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t,
                  uint32_t) {}

// El compositor anuncia también la ruta DMABUF; nosotros solo usamos wl_shm.
void frame_linux_dmabuf(void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t,
                        uint32_t) {}

void frame_buffer_done(void*, zwlr_screencopy_frame_v1*) {}

constexpr zwlr_screencopy_frame_v1_listener kFrameListener = {
    frame_buffer, frame_flags, frame_ready, frame_failed, frame_damage,
    frame_linux_dmabuf, frame_buffer_done};

}  // namespace

namespace linky {

class ScreencopyVideoCapture final : public VideoCapture {
 public:
  ~ScreencopyVideoCapture() override { stop(); }

  bool start(const VideoSpec& spec,
             std::function<void(AVFrame*)> on_frame) override {
    spec_ = spec;
    on_frame_ = std::move(on_frame);
    // Todo el ciclo de vida de Wayland (conexión, dispatch, teardown) vive
    // en el hilo de captura: evita uso de proxies entre hilos.
    thread_ = std::thread([this] { run_loop(); });
    return true;
  }

  void stop() override {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

  bool ready() const override { return running_.load() && connected_.load(); }
  const char* backend_name() const override { return "wayland-screencopy"; }

 private:
  VideoSpec spec_;
  std::function<void(AVFrame*)> on_frame_;
  WlGlobals wl_{};
  FrameState* frame_state_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::thread thread_;

  void run_loop() {
    running_ = true;
    wl_.display = wl_display_connect(nullptr);
    if (!wl_.display) {
      LERR("caps", "wayland: no hay compositor");
      running_ = false;
      return;
    }
    wl_.registry = wl_display_get_registry(wl_.display);
    static const wl_registry_listener kReg = {registry_global, registry_remove};
    wl_registry_add_listener(wl_.registry, &kReg, &wl_);
    wl_display_roundtrip(wl_.display);
    if (!wl_.mgr || !wl_.output) {
      LERR("caps", "wayland: compositor sin zwlr_screencopy_manager_v1 o sin salida");
      cleanup();
      return;
    }
    connected_.store(true);
    LINF("caps", "wayland: capturando salida via zwlr_screencopy");
    while (running_) {
      if (!capture_one()) {
        if (!running_) break;
        LERR("caps", "wayland: captura fallida, reintentando");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
    }
    cleanup();
  }

  // Solo se llama desde el hilo de captura (same thread que creó los proxies).
  void cleanup() {
    connected_.store(false);
    if (frame_state_) {
      if (frame_state_->mem) munmap(frame_state_->mem, frame_state_->pool_size);
      if (frame_state_->pool) wl_shm_pool_destroy(frame_state_->pool);
      delete frame_state_;
      frame_state_ = nullptr;
    }
    if (wl_.mgr) zwlr_screencopy_manager_v1_destroy(wl_.mgr);
    if (wl_.output) wl_output_destroy(wl_.output);
    if (wl_.shm) wl_shm_destroy(wl_.shm);
    if (wl_.registry) wl_registry_destroy(wl_.registry);
    if (wl_.display) {
      wl_display_disconnect(wl_.display);
      wl_.display = nullptr;
    }
    running_ = false;
  }

  // Espera eventos hasta que la geometría esté disponible (evento "buffer").
  bool wait_geometry(FrameState& fs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (running_ && fs.width == 0) {
      if (!dispatch_once()) return false;
      if (std::chrono::steady_clock::now() > deadline) {
        LERR("caps", "wayland: el compositor no envió la geometría (¿captura rota?)");
        return false;
      }
    }
    return fs.width != 0;
  }

  // Espera el resultado de la copia ("ready" o "failed").
  bool wait_ready(FrameState& fs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (running_ && !fs.ready && !fs.failed) {
      if (!dispatch_once()) return false;
      if (std::chrono::steady_clock::now() > deadline) {
        LERR("caps", "wayland: el compositor no completó la copia");
        return false;
      }
    }
    return fs.ready;
  }

  // Poll sobre el fd de Wayland con timeout de 50 ms (permite cancelar).
  bool dispatch_once() {
    while (wl_display_prepare_read(wl_.display) != 0) {
      if (wl_display_dispatch_pending(wl_.display) < 0) return false;
    }
    pollfd pfd{};
    pfd.fd = wl_display_get_fd(wl_.display);
    pfd.events = POLLIN;
    const int pr = poll(&pfd, 1, 50);
    if (pr > 0) {
      if (wl_display_read_events(wl_.display) < 0) return false;
    } else {
      wl_display_cancel_read(wl_.display);
    }
    if (wl_display_dispatch_pending(wl_.display) < 0) return false;
    return true;
  }

  bool ensure_pool(FrameState& fs, int stride, int height) {
    const size_t need = static_cast<size_t>(stride) * height;
    if (fs.pool && fs.pool_size >= need) return true;
    if (fs.mem) munmap(fs.mem, fs.pool_size);
    if (fs.pool) wl_shm_pool_destroy(fs.pool);
    const int fd = static_cast<int>(syscall(SYS_memfd_create, "linky-sc", MFD_CLOEXEC));
    if (fd < 0) return false;
    if (ftruncate(fd, static_cast<off_t>(need)) != 0) {
      close(fd);
      return false;
    }
    fs.pool = wl_shm_create_pool(wl_.shm, fd, static_cast<int32_t>(need));
    fs.mem = static_cast<uint8_t*>(mmap(nullptr, need, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, 0));
    close(fd);
    if (fs.pool == nullptr || fs.mem == MAP_FAILED || fs.mem == nullptr) {
      fs.mem = nullptr;
      fs.pool = nullptr;
      return false;
    }
    fs.pool_size = need;
    return true;
  }

  bool capture_one() {
    auto* frame = zwlr_screencopy_manager_v1_capture_output(wl_.mgr, 0, wl_.output);
    // Sin flush la petición nunca sale del cliente y "buffer" no llega.
    if (wl_display_flush(wl_.display) < 0) {
      LERR("caps", "wayland: flush falló");
      zwlr_screencopy_frame_v1_destroy(frame);
      return false;
    }
    FrameState fs;
    fs.wl = &wl_;
    zwlr_screencopy_frame_v1_add_listener(frame, &kFrameListener, &fs);
    if (!frame_state_) frame_state_ = new FrameState();
    fs.pool = frame_state_->pool;
    fs.mem = frame_state_->mem;
    fs.pool_size = frame_state_->pool_size;

    if (!wait_geometry(fs)) {
      zwlr_screencopy_frame_v1_destroy(frame);
      return false;
    }
    if (!ensure_pool(fs, fs.stride, fs.height)) {
      zwlr_screencopy_frame_v1_destroy(frame);
      LERR("caps", "wayland: no hay memoria para el buffer de captura");
      return false;
    }
    auto* buffer = wl_shm_pool_create_buffer(fs.pool, 0, fs.width, fs.height,
                                             fs.stride, fs.format);
    if (!buffer) {
      zwlr_screencopy_frame_v1_destroy(frame);
      return false;
    }
    zwlr_screencopy_frame_v1_copy(frame, buffer);
    if (wl_display_flush(wl_.display) < 0) {
      wl_buffer_destroy(buffer);
      zwlr_screencopy_frame_v1_destroy(frame);
      LERR("caps", "wayland: flush copy falló");
      return false;
    }

    if (!wait_ready(fs)) {
      wl_buffer_destroy(buffer);
      zwlr_screencopy_frame_v1_destroy(frame);
      return false;
    }
    if (fs.failed) {
      wl_buffer_destroy(buffer);
      zwlr_screencopy_frame_v1_destroy(frame);
      return false;
    }

    deliver_frame(fs);

    wl_buffer_destroy(buffer);
    zwlr_screencopy_frame_v1_destroy(frame);
    return true;
  }

  void deliver_frame(const FrameState& fs) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
      LERR("caps", "wayland: sin memoria para AVFrame");
      return;
    }
    frame->format = AV_PIX_FMT_BGR0;
    frame->width = fs.width;
    frame->height = fs.height;
    frame->linesize[0] = fs.stride;
    frame->pts = av_gettime();
    if (av_frame_get_buffer(frame, 32) != 0) {
      av_frame_free(&frame);
      return;
    }

    // wl_shm ARGB8888/XRGB8888 es BGRA/BGRX en memoria (little endian);
    // BGR0 = B,G,R,X -> copia directa de 4 bytes por píxel.
    const uint8_t* src = fs.mem;
    uint8_t* dst = frame->data[0];
    for (int y = 0; y < fs.height; ++y) {
      std::memcpy(dst + y * static_cast<size_t>(fs.stride),
                  src + y * static_cast<size_t>(fs.stride),
                  static_cast<size_t>(fs.width) * 4);
    }

    if (on_frame_) on_frame_(frame);
    av_frame_free(&frame);
  }
};

VideoCapture* create_screencopy_capture() {
  return new ScreencopyVideoCapture;
}

}  // namespace linky
