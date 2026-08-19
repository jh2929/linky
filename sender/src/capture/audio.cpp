// Captura de audio del sistema (loopback del sink) mediante libpulse-simple
// cargado en tiempo de ejecución (dlopen). El nombre del monitor se resuelve
// con pactl; si no, no se puede monitorizar y se informa.
// PCM s16le 48 kHz estéreo. No hay dependencia de link: cualquier sistema con
// PipeWire/PulseAudio (libpulse-simple.so.0) funciona.
#include "capture/video.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <dlfcn.h>
extern "C" {
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
}

#include "common/log.h"

namespace linky {

namespace {

std::string resolve_monitor() {
  // pactl list short sinks → "N\tsink_name\t..."
  FILE* p = popen("pactl list short sinks 2>/dev/null", "r");
  if (!p) return "";
  std::string line, best;
  int best_idx = -1;
  char buf[1024];
  while (fgets(buf, sizeof buf, p)) {
    int idx = -1;
    char name[256] = {0};
    if (sscanf(buf, "%d\t%255s", &idx, name) == 2) {
      if (best_idx < 0 || idx < best_idx) {
        best_idx = idx;
        best = name;
      }
    }
  }
  pclose(p);
  if (best.empty()) return "";
  return best + ".monitor";
}

class PulseAudioCapture : public AudioCapture {
 public:
  ~PulseAudioCapture() override { stop(); }

  bool start(std::function<void(const int16_t*, int)> on_pcm) override {
    on_pcm_ = std::move(on_pcm);

    dl_ = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!dl_) {
      LERR("capa", "libpulse-simple.so.0 no disponible (%s)", dlerror());
      return false;
    }
    *(void**)(&fn_new_) = dlsym(dl_, "pa_simple_new");
    *(void**)(&fn_read_) = dlsym(dl_, "pa_simple_read");
    *(void**)(&fn_free_) = dlsym(dl_, "pa_simple_free");
    *(void**)(&fn_strerror_) = dlsym(dl_, "pa_strerror");
    if (!fn_new_ || !fn_read_ || !fn_free_ || !fn_strerror_) {
      LERR("capa", "símbolos libpulse incompletos");
      dlclose(dl_);
      dl_ = nullptr;
      return false;
    }

    std::string source = resolve_monitor();
    if (source.empty()) {
      LERR("capa", "no se encontró ningún sink para monitorizar (¿pactl?)");
      return false;
    }
    LINF("capa", "fuente de audio: %s", source.c_str());

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 48000;
    ss.channels = 2;

    int err = 0;
    simple_ = fn_new_(nullptr, "linky-sender", PA_STREAM_RECORD, source.c_str(),
                      "linky", &ss, nullptr, nullptr, &err);
    if (!simple_) {
      LERR("capa", "pa_simple_new %s: %s", source.c_str(), fn_strerror_(err));
      return false;
    }
    thread_ = std::thread([this] { loop(); });
    return true;
  }

  void stop() override {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    if (simple_) fn_free_(simple_);
    if (dl_) dlclose(dl_);
  }

 private:
  using FnNew = pa_simple* (*)(const char*, const char*, pa_stream_direction_t,
                               const char*, const char*, const pa_sample_spec*,
                               const pa_channel_map*, const pa_buffer_attr*, int*);
  using FnRead = int (*)(pa_simple*, void*, size_t, int*);
  using FnFree = void (*)(pa_simple*);
  using FnStrerror = const char* (*)(int);

  std::function<void(const int16_t*, int)> on_pcm_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  void* dl_ = nullptr;
  pa_simple* simple_ = nullptr;
  FnNew fn_new_ = nullptr;
  FnRead fn_read_ = nullptr;
  FnFree fn_free_ = nullptr;
  FnStrerror fn_strerror_ = nullptr;

  void loop() {
    // 20 ms de PCM s16le 48 kHz estéreo = 960 muestras = 3840 bytes.
    constexpr size_t chunk = 960 * 2 * 2;
    std::unique_ptr<int16_t[]> buf(new int16_t[chunk / 2]);
    while (!stop_) {
      int err = 0;
      if (fn_read_(simple_, buf.get(), chunk, &err) < 0) {
        LWRN("capa", "pa_simple_read: %s", fn_strerror_(err));
        break;
      }
      if (on_pcm_) on_pcm_(buf.get(), chunk / 2);
    }
  }
};

}  // namespace

AudioCapture* create_audio_capture(const std::string& device_hint) {
  (void)device_hint;
  return new PulseAudioCapture;
}

}  // namespace linky