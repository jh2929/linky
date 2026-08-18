#pragma once
// Orquestador del emisor: une control + captura + encoder + RTP/RTCP.
// Expone estado y estadísticas mediante un único callback de eventos.
#include <netinet/in.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "capture/video.h"
#include "common/json.h"
#include "discovery/discovery.h"
#include "encode/encoder.h"
#include "rtp/rtcp.h"
#include "session/control.h"

namespace linky {

struct SenderConfig {
  std::string device_name;   // nombre mostrado al receptor
  int width = 1920, height = 1080, fps = 30;
  int bitrate_kbps = 8000;
  bool audio = true;
  int mtu = 1400;
};

struct SenderStats {
  double fps = 0;
  double kbps = 0;
  int frames_encoded = 0;
  int frames_dropped = 0;
  int nacks = 0;
  int plis = 0;
  int rtt_ms = -1;
  double latency_ms = 0;   // estimación captura→socket
};

enum class SenderStatus {
  Idle, Connecting, WaitingAcceptance, Streaming, Failed
};

class SenderApp {
 public:
  struct Event {
    SenderStatus status = SenderStatus::Idle;
    std::string message;
    SenderStats stats;
  };
  using Events = std::function<void(const Event&)>;

  ~SenderApp();
  // Objetivo directo (ip) o descubierto por DNS-SD.
  void set_target(const std::string& host, int control_port,
                  const std::string& name, const Json& caps);
  bool start(const SenderConfig& cfg, Events events);
  void stop();
  SenderStatus status() const { return status_; }
  void cancel_pairing() {
    if (client_) client_->close();
  }

 private:
  SenderConfig cfg_;
  Events events_;
  std::atomic<SenderStatus> status_{SenderStatus::Idle};
  std::string host_, name_;
  int control_port_ = 0;
  Json caps_;
  std::unique_ptr<ControlClient> client_;
  std::unique_ptr<VideoCapture> vcap_;
  std::unique_ptr<AudioCapture> acap_;
  std::unique_ptr<VideoEncoder> venc_;
  std::unique_ptr<AudioEncoder> aenc_;
  std::unique_ptr<RtcpSender> rtcp_;

  std::thread audio_thread_, rtcp_thread_;
  std::atomic<bool> running_{false};
  std::mutex mu_;

  // Transporte
  int sock_v_ = -1, sock_a_ = -1, sock_vr_ = -1, sock_ar_ = -1;
  sockaddr_in to_v_{}, to_a_{}, to_vr_{}, to_ar_{};
  uint32_t ssrc_v_ = 0, ssrc_a_ = 0;
  uint16_t vseq_ = 0, aseq_ = 0;
  uint32_t vts_ = 0, ats_ = 0;
  uint64_t vpkts_ = 0, apkts_ = 0, vbytes_ = 0, abytes_ = 0;
  uint64_t sr_ntp_ = 0;  // ntp ticks en el último SR de video

  // Retransmisión (NACK): anillo de paquetes recientes.
  struct Sent { uint16_t seq; std::vector<uint8_t> data; uint64_t t; };
  std::deque<Sent> nack_ring_;
  std::atomic<int> nacks_{0}, plis_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> encoded_{0};
  int64_t last_frame_ns_ = 0;
  int64_t last_stats_ns_ = 0;
  int64_t last_fps_ns_ = 0;
  double fps_ema_ = 0;
  uint64_t vbytes_since_ = 0;   // bytes enviados desde la última estadística
  int64_t last_send_ns_ = 0;
  Codec codec_ = Codec::H264;

  void emit(SenderStatus st, const std::string& msg);
  void begin_media(const ControlClient::Welcome& w);
  void video_frame_cb(AVFrame* f);
  void audio_pcm_cb(const int16_t* pcm, int samples);
  void rtcp_loop(int vid_rtcp_port, int aud_rtcp_port);
  void send_rtp_packets(int fd, const sockaddr_in& to,
                        std::vector<std::vector<uint8_t>>& pkts);
  void retransmit_nack(const std::vector<uint16_t>& seqs);
  void teardown_transport();
};

}  // namespace linky
