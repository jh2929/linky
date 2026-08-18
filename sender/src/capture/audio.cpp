// Captura de audio del sistema (loopback del sink) mediante libavdevice
// (device "pulse"). El nombre del monitor se resuelve con pactl; si no,
// se usa el sink por defecto del usuario. PCM s16le 48 kHz estéreo.
#include "capture/video.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include "common/log.h"
#include "common/util.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

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
    char ebuf[AV_ERROR_MAX_STRING_SIZE];
    on_pcm_ = std::move(on_pcm);
    avdevice_register_all();

    std::string source = resolve_monitor();
    if (source.empty()) {
      LERR("capa", "no se encontró ningún sink para monitorizar");
      return false;
    }
    LINF("capa", "fuente de audio: %s", source.c_str());

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "sample_rate", "48000", 0);
    av_dict_set(&opts, "channels", "2", 0);
    av_dict_set(&opts, "sample_format", "s16le", 0);
    av_dict_set(&opts, "frame_size", "960", 0);  // 20 ms

    const AVInputFormat* fmt = av_find_input_format("pulse");
    int r = avformat_open_input(&fmt_ctx_, source.c_str(), fmt, &opts);
    av_dict_free(&opts);
    if (r < 0) {
      LERR("capa", "abrir %s: %s", source.c_str(), fferr(r, ebuf, sizeof(ebuf)));
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

 private:
  std::function<void(const int16_t*, int)> on_pcm_;
  AVFormatContext* fmt_ctx_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  void loop() {
    char ebuf[AV_ERROR_MAX_STRING_SIZE];
    AVPacket* pkt = av_packet_alloc();
    while (!stop_) {
      int r = av_read_frame(fmt_ctx_, pkt);
      if (r < 0) {
        if (r != AVERROR_EOF) LWRN("capa", "av_read_frame: %s", fferr(r, ebuf, sizeof(ebuf)));
        av_packet_unref(pkt);
        continue;
      }
      if (on_pcm_ && pkt->size >= 2)
        on_pcm_(reinterpret_cast<const int16_t*>(pkt->data), pkt->size / 2);
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
  }
};

}  // namespace

AudioCapture* create_audio_capture(const std::string& device_hint) {
  (void)device_hint;
  return new PulseAudioCapture;
}

}  // namespace linky
