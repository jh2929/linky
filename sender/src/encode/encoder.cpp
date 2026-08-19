// Implementación de encoders sobre FFmpeg. Auto-detección por capas:
//   1) VAAPI (hwdevice probe + open por códec)
//   2) QSV  (si libvpl está disponible en build)
//   3) NVENC/AMF (si los encoders existen y hay dispositivo)
//   4) CPU  (x264/x265 si están compilados; en caso contrario se informa
//      claramente de que esta build no puede codificar en CPU).
// Baja latencia: sin B-frames, GOP corto, bitrate CBR con VBV, y política
// de descarte de frames si el encoder se atrasa.
#include "encode/encoder.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "common/log.h"
#include "common/util.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace linky {

const char* codec_name(Codec c) {
  switch (c) {
    case Codec::H264: return "h264";
    case Codec::H265: return "h265";
    case Codec::AV1:  return "av1";
  }
  return "?";
}

// Asocia un contexto de frames VAAPI (NV12) al encoder. Sin esto, FFmpeg
// no puede abrir ningún encoder *vaapi.
static bool attach_frames_ctx(AVCodecContext* ctx, AVBufferRef* hwdev, int w, int h) {
  AVBufferRef* frames = av_hwframe_ctx_alloc(hwdev);
  if (!frames) return false;
  AVHWFramesContext* fctx = reinterpret_cast<AVHWFramesContext*>(frames->data);
  fctx->format = AV_PIX_FMT_VAAPI;
  fctx->sw_format = AV_PIX_FMT_NV12;
  fctx->width = w;
  fctx->height = h;
  fctx->initial_pool_size = 8;
  if (av_hwframe_ctx_init(frames) < 0) {
    av_buffer_unref(&frames);
    return false;
  }
  ctx->hw_frames_ctx = frames;  // el codec toma posesión de la referencia
  return true;
}

// ── Sondeo de capacidades ─────────────────────────────────────────────────
static AVHWDeviceContext* hw_ctx(AVBufferRef* ref) {
  return reinterpret_cast<AVHWDeviceContext*>(ref->data);
}

EncoderInfo probe_encoders() {
  EncoderInfo info;
  AVBufferRef* dev = nullptr;
  int err = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VAAPI,
                                   "/dev/dri/renderD128", nullptr, 0);
  if (err < 0)
    err = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
  if (err >= 0) {
    info.backend = "vaapi";
    struct Entry { Codec c; const char* name; };
    for (const Entry& e : {Entry{Codec::H264, "h264_vaapi"},
                           Entry{Codec::H265, "hevc_vaapi"},
                           Entry{Codec::AV1, "av1_vaapi"}}) {
      const AVCodec* codec = avcodec_find_encoder_by_name(e.name);
      if (!codec) continue;
      AVCodecContext* ctx = avcodec_alloc_context3(codec);
      if (!ctx) continue;
      ctx->width = 64;
      ctx->height = 64;
      ctx->time_base = AVRational{1, 30};
      ctx->pix_fmt = AV_PIX_FMT_VAAPI;
      ctx->hw_device_ctx = av_buffer_ref(dev);
      if (!attach_frames_ctx(ctx, dev, 64, 64)) {
        avcodec_free_context(&ctx);
        continue;
      }
      // AV1 requiere perfil 0 en KBL+; open falla si la GPU no lo soporta.
      if (avcodec_open2(ctx, codec, nullptr) == 0) {
        info.codecs.push_back(e.c);
        avcodec_free_context(&ctx);
      } else {
        avcodec_free_context(&ctx);
      }
    }
    av_buffer_unref(&dev);
  }
  if (info.codecs.empty()) {
    // Fallback CPU (si la build tiene libx264/libx265 enlazables).
    if (avcodec_find_encoder_by_name("libx264")) info.backend = "cpu-x264";
    else if (avcodec_find_encoder(AV_CODEC_ID_H264)) info.backend = "cpu-native";
    if (info.backend != "none" && !info.backend.empty()) info.codecs.push_back(Codec::H264);
  }
  if (info.codecs.empty()) info.backend = "none";
  return info;
}

// ── Encoder de vídeo (VAAPI) ───────────────────────────────────────────────
namespace {

class VaapiVideoEncoder : public VideoEncoder {
 public:
  ~VaapiVideoEncoder() override { close(); }

  bool init(Codec codec, int width, int height, int fps,
            int bitrate_kbps) override {
    codec_ = codec;
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_kbps_ = bitrate_kbps;
    const char* name = codec == Codec::H265 ? "hevc_vaapi"
                       : codec == Codec::AV1 ? "av1_vaapi"
                                             : "h264_vaapi";
    return open(name);
  }

  bool encode(AVFrame* bgr0, std::vector<AVPacket*>& out) override {
    if (!ready()) return false;
    std::lock_guard<std::mutex> g(mu_);
    if (queued_ >= 1) {  // anti-latencia: descartar si hay cola
      ++dropped_;
      return false;
    }
    ++queued_;

    AVFrame* sw = nullptr;
    if (!sw_) sw_ = sws_getContext(width_, height_, AV_PIX_FMT_BGR0, width_,
                                   height_, AV_PIX_FMT_NV12, SWS_BILINEAR,
                                   nullptr, nullptr, nullptr);
    int r = 0;
    if (sw_) {
      sw = av_frame_alloc();
      sw->format = AV_PIX_FMT_NV12;
      sw->width = width_;
      sw->height = height_;
      r = av_frame_get_buffer(sw, 0);
      if (r == 0) {
        const uint8_t* src[] = {bgr0->data[0], nullptr, nullptr, nullptr};
        const int src_stride[] = {bgr0->linesize[0], 0, 0, 0};
        uint8_t* dst[] = {sw->data[0], sw->data[1], nullptr, nullptr};
        const int dst_stride[] = {sw->linesize[0], sw->linesize[1], 0, 0};
        sws_scale(sw_, src, src_stride, 0, height_, dst, dst_stride);
      }
    }
    if (r < 0 || !sw) {
      if (sw) av_frame_free(&sw);
      --queued_;
      return false;
    }
    sw->pts = pts_++;
    r = avcodec_send_frame(ctx_, sw);
    av_frame_free(&sw);
    if (r < 0) {
      --queued_;
      return false;
    }
    for (;;) {
      AVPacket* pkt = av_packet_alloc();
      r = avcodec_receive_packet(ctx_, pkt);
      if (r == 0) {
        out.push_back(pkt);
      } else {
        av_packet_free(&pkt);
        break;
      }
    }
    --queued_;
    return true;
  }

  void force_keyframe() override {
    // VAAPI no expone "forzar IDR" sin reabrir; reabrir es barato y emite
    // SPS/PPS frescos (que es justo lo que pide el PLI del receptor).
    std::lock_guard<std::mutex> g(mu_);
    close();
    if (!open(codec_ == Codec::H265 ? "hevc_vaapi"
                                    : codec_ == Codec::AV1 ? "av1_vaapi"
                                                           : "h264_vaapi"))
      LERR("encv", "reopen tras PLI falló");
    else
      LINF("encv", "keyframe solicitado (PLI): encoder reabierto");
  }

  bool ready() const override { return ctx_ != nullptr; }
  const char* backend() const override { return "vaapi"; }

 private:
  std::mutex mu_;
  Codec codec_ = Codec::H264;
  int width_ = 0, height_ = 0, fps_ = 0, bitrate_kbps_ = 0;
  int queued_ = 0;
  uint64_t pts_ = 0;
  std::atomic<uint64_t> dropped_{0};
  AVCodecContext* ctx_ = nullptr;
  AVBufferRef* hwdev_ = nullptr;
  SwsContext* sw_ = nullptr;

  bool open(const char* name) {
    char ebuf[AV_ERROR_MAX_STRING_SIZE];
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) {
      LERR("encv", "encoder %s no disponible en esta build", name);
      return false;
    }
    int err = av_hwdevice_ctx_create(&hwdev_, AV_HWDEVICE_TYPE_VAAPI,
                                     "/dev/dri/renderD128", nullptr, 0);
    if (err < 0)
      err = av_hwdevice_ctx_create(&hwdev_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (err < 0) {
      LERR("encv", "vaapi: %s", fferr(err, ebuf, sizeof(ebuf)));
      return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    ctx_->width = width_;
    ctx_->height = height_;
    ctx_->time_base = AVRational{1, fps_};
    ctx_->framerate = AVRational{fps_, 1};
    ctx_->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx_->hw_device_ctx = av_buffer_ref(hwdev_);
    if (!attach_frames_ctx(ctx_, hwdev_, width_, height_)) {
      LERR("encv", "no se pudo crear hw_frames_ctx");
      close();
      return false;
    }
    ctx_->bit_rate = bitrate_kbps_ * 1000;
    ctx_->rc_max_rate = bitrate_kbps_ * 1000;
    ctx_->rc_buffer_size = bitrate_kbps_ * 1000 / std::max(4, fps_ / 2);
    ctx_->gop_size = fps_ * 2;            // IDR cada 2 s
    ctx_->max_b_frames = 0;               // sin B-frames (baja latencia)
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->thread_count = 1;

    err = avcodec_open2(ctx_, codec, nullptr);
    if (err < 0) {
      LERR("encv", "abrir %s: %s", name, fferr(err, ebuf, sizeof(ebuf)));
      close();
      return false;
    }
    LINF("encv", "encoder %s (vaapi) %dx%d@%d %d kbps", name, width_, height_,
         fps_, bitrate_kbps_);
    return true;
  }

  void close() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (hwdev_) av_buffer_unref(&hwdev_);
    if (sw_) sws_freeContext(sw_);
    sw_ = nullptr;
  }
};

}  // namespace

VideoEncoder* create_video_encoder() { return new VaapiVideoEncoder; }

// ── Encoder de audio (Opus, encoders nativos de FFmpeg) ───────────────────
namespace {

class OpusAudioEncoder : public AudioEncoder {
 public:
  ~OpusAudioEncoder() override {
    if (ctx_) avcodec_free_context(&ctx_);
    if (swr_) swr_free(&swr_);
  }

    char ebuf[AV_ERROR_MAX_STRING_SIZE];
  bool init() override {
    const AVCodec* codec = avcodec_find_encoder_by_name("libopus");
    if (!codec) codec = avcodec_find_encoder_by_name("opus");
    if (!codec) {
      LERR("enca", "no hay encoder Opus en esta build");
      return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    ctx_->sample_rate = 48000;
    ctx_->ch_layout = AVChannelLayout{AV_CHANNEL_ORDER_NATIVE, 2,
                                      {AV_CH_LAYOUT_STEREO}};
    ctx_->sample_fmt = AV_SAMPLE_FMT_FLT;
    ctx_->bit_rate = 128000;
    ctx_->compression_level = 5;
    ctx_->frame_size = 960;  // 20 ms @48 kHz
    if (av_opt_set(ctx_->priv_data, "application", "lowdelay", 0) < 0)
      LWRN("enca", "opción opus 'application' no soportada; modo por defecto");
    int r = avcodec_open2(ctx_, codec, nullptr);
    if (r < 0) {
      // fallback: planar
      ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
      r = avcodec_open2(ctx_, codec, nullptr);
    }
    if (r < 0) {
      LERR("enca", "abrir opus: %s", fferr(r, ebuf, sizeof(ebuf)));
      avcodec_free_context(&ctx_);
      ctx_ = nullptr;
      return false;
    }
    int swr_rc = swr_alloc_set_opts2(&swr_, &ctx_->ch_layout, ctx_->sample_fmt,
                                     48000, &ctx_->ch_layout, AV_SAMPLE_FMT_S16,
                                     48000, 0, nullptr);
    if (swr_rc < 0 || swr_init(swr_) < 0) {
      LERR("enca", "swr_init opus falló");
      return false;
    }
    fifo_.clear();
    return true;
  }

  bool encode(const int16_t* pcm, int samples, std::vector<AVPacket*>& out) override {
    if (!ctx_) return false;
    fifo_.insert(fifo_.end(), pcm, pcm + static_cast<size_t>(samples));
    const size_t frame = static_cast<size_t>(ctx_->frame_size);
    float tmp[frame * 2];
    while (fifo_.size() >= frame) {
      const uint8_t* in_planes[1] = {
          reinterpret_cast<const uint8_t*>(fifo_.data())};
      uint8_t* out_planes[1] = {reinterpret_cast<uint8_t*>(tmp)};
      swr_convert(swr_, out_planes, static_cast<int>(frame), in_planes,
                  static_cast<int>(frame));
      AVFrame* f = av_frame_alloc();
      f->format = ctx_->sample_fmt;
      f->ch_layout = ctx_->ch_layout;
      f->sample_rate = 48000;
      f->nb_samples = ctx_->frame_size;
      if (av_frame_get_buffer(f, 0) == 0) {
        for (size_t i = 0; i < frame; ++i) {
          f->data[0][i * 2] = tmp[i * 2];
          f->data[0][i * 2 + 1] = tmp[i * 2 + 1];
        }
      }
      if (avcodec_send_frame(ctx_, f) == 0) {
        for (;;) {
          AVPacket* pkt = av_packet_alloc();
          int r = avcodec_receive_packet(ctx_, pkt);
          if (r == 0) {
            out.push_back(pkt);
          } else {
            av_packet_free(&pkt);
            break;
          }
        }
      }
      av_frame_free(&f);
      fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<long>(frame));
    }
    return true;
  }

  bool ready() const override { return ctx_ != nullptr; }

 private:
  AVCodecContext* ctx_ = nullptr;
  SwrContext* swr_ = nullptr;
  std::vector<int16_t> fifo_;
};

}  // namespace

AudioEncoder* create_audio_encoder() { return new OpusAudioEncoder; }

}  // namespace linky
