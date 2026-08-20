#include "app/sender_app.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "common/log.h"
#include "common/util.h"
#include "rtp/packetizer.h"

namespace linky {

namespace {
uint64_t ntp_ticks_now() {
  struct timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t sec = static_cast<uint64_t>(ts.tv_sec) + 2208988800ULL;  // epoch → ntp
  uint64_t frac = (static_cast<uint64_t>(ts.tv_nsec) << 32) / 1000000000ULL;
  return (sec << 32) | frac;
}
int64_t mono_ns() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
}  // namespace

SenderApp::~SenderApp() { stop(); }

void SenderApp::set_target(const std::string& host, int control_port,
                           const std::string& name, const Json& caps) {
  host_ = host;
  control_port_ = control_port;
  name_ = name;
  caps_ = caps;
}

bool SenderApp::start(const SenderConfig& cfg, Events events) {
  cfg_ = cfg;
  events_ = std::move(events);
  if (host_.empty()) {
    emit(SenderStatus::Failed, "sin destino");
    return false;
  }
  status_ = SenderStatus::Connecting;
  emit(SenderStatus::Connecting, "Conectando a " + host_);

  Json hello;
  hello["type"] = "hello";
  hello["proto"] = "1";
  hello["device"] = cfg_.device_name.empty() ? hostname() : cfg_.device_name;
  hello["senderId"] = sender_id();
  hello["codecs"] = caps_.count("codecs") ? caps_.at("codecs") : "";
  hello["audio"] = caps_.count("audio") ? caps_.at("audio") : "opus";
  hello["vres"] = std::to_string(cfg_.width) + "x" + std::to_string(cfg_.height);
  hello["vrate"] = std::to_string(cfg_.fps);

  client_ = std::make_unique<ControlClient>();
  client_->set_handlers(
      [this](const ControlClient::Welcome& w) { begin_media(w); },
      [this] {
        status_ = SenderStatus::WaitingAcceptance;
        emit(SenderStatus::WaitingAcceptance, "Esperando aceptación del TV");
      },
      [this] {
        status_ = SenderStatus::Idle;
        emit(SenderStatus::Failed, "El TV rechazó la conexión");
      },
      [this](const std::string& e) {
        status_ = SenderStatus::Failed;
        emit(SenderStatus::Failed, e);
      },
      [this] {
        if (status_ == SenderStatus::Streaming) {
          status_ = SenderStatus::Idle;
          emit(SenderStatus::Failed, "Conexión perdida con el TV");
        }
      });
  if (!client_->connect(host_, control_port_, hello)) {
    emit(SenderStatus::Failed, "No se pudo conectar al control del TV");
    return false;
  }
  return true;
}

void SenderApp::stop() {
  if (!running_) {
    if (client_) client_->close();
    return;
  }
  running_ = false;
  if (client_) client_->close();
  if (vcap_) vcap_->stop();
  if (acap_) acap_->stop();
  if (audio_thread_.joinable()) audio_thread_.join();
  if (rtcp_thread_.joinable()) rtcp_thread_.join();
  teardown_transport();
  status_ = SenderStatus::Idle;
}

void SenderApp::emit(SenderStatus st, const std::string& msg) {
  Event e;
  e.status = st;
  e.message = msg;
  SenderStats s;
  s.frames_encoded = static_cast<int>(encoded_.load());
  s.frames_dropped = static_cast<int>(dropped_.load());
  s.nacks = nacks_.load();
  s.plis = plis_.load();
  e.stats = s;
  if (events_) events_(e);
}

// ── Inicio de media (welcome recibido) ─────────────────────────────────────
void SenderApp::begin_media(const ControlClient::Welcome& w) {
  status_ = SenderStatus::Streaming;
  emit(SenderStatus::Streaming, "Transmitiendo a " + name_);

  // Sockets de media
  auto open_udp = [](int& fd, sockaddr_in& to, const std::string& ip, int port) {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &to.sin_addr);
  };
  open_udp(sock_v_, to_v_, host_, w.vport);
  if (cfg_.audio) open_udp(sock_a_, to_a_, host_, w.aport);
  open_udp(sock_vr_, to_vr_, host_, w.vrtcp);
  if (cfg_.audio) open_udp(sock_ar_, to_ar_, host_, w.artcp);

  ssrc_v_ = static_cast<uint32_t>(0x6c696e4b ^ 0x76400000);  // "linkV"
  ssrc_a_ = static_cast<uint32_t>(0x6c696e4b ^ 0x61400000);  // "linkA"
  vts_ = 0;
  ats_ = 0;
  vseq_ = static_cast<uint16_t>(rand() & 0xffff);
  aseq_ = static_cast<uint16_t>(rand() & 0xffff);

  // Encoders
  codec_ = w.codec == "h265" ? Codec::H265
           : w.codec == "av1" ? Codec::AV1
                              : Codec::H264;
  venc_.reset(create_video_encoder());
  if (!venc_ || !venc_->init(codec_, cfg_.width, cfg_.height, cfg_.fps,
                             cfg_.bitrate_kbps)) {
    emit(SenderStatus::Failed, "no se pudo inicializar el encoder de vídeo");
    return;
  }
  if (cfg_.audio) {
    aenc_.reset(create_audio_encoder());
    if (!aenc_ || !aenc_->init()) {
      LWRN("app", "audio deshabilitado (encoder Opus no disponible)");
      cfg_.audio = false;
    }
  }

  rtcp_ = std::make_unique<RtcpSender>();
  rtcp_->init(ssrc_v_, ssrc_a_);

  // Captura de vídeo (hilo del portal)
  VideoSpec vspec{cfg_.width, cfg_.height, cfg_.fps};
  vcap_.reset(create_video_capture());
  if (!vcap_) {
    emit(SenderStatus::Failed, "no hay backend de captura (Wayland ni X11)");
    return;
  }
  if (!vcap_->start(vspec, [this](AVFrame* f) { video_frame_cb(f); })) {
    emit(SenderStatus::Failed, "la captura falló al arrancar");
    return;
  }

  // Audio (hilo propio)
  if (cfg_.audio) {
    acap_.reset(create_audio_capture(""));
    running_ = true;
    audio_thread_ = std::thread([this] {
      acap_->start([this](const int16_t* pcm, int n) { audio_pcm_cb(pcm, n); });
    });
  } else {
    running_ = true;
  }

  // RTCP (hilo propio)
  running_ = true;
  rtcp_thread_ = std::thread([this, &w] { rtcp_loop(w.vrtcp, w.artcp); });
}

void SenderApp::video_frame_cb(AVFrame* f) {
  if (!venc_ || !venc_->ready() || status_ != SenderStatus::Streaming) return;
  int64_t now = mono_ns();
  // Anti-latencia: si el encoder va atrasado, se salta el frame.
  if (last_frame_ns_ > 0 && now - last_frame_ns_ > 1e9 / cfg_.fps * 2) {
    dropped_.fetch_add(1);
    return;
  }
  last_frame_ns_ = now;
  std::vector<AVPacket*> pkts;
  if (!venc_->encode(f, pkts) || pkts.empty()) {
    dropped_.fetch_add(1);
    for (auto* p : pkts) av_packet_free(&p);
    return;
  }
  encoded_.fetch_add(1);
  std::vector<std::vector<uint8_t>> rtp;
  for (AVPacket* p : pkts) {
    bool key = annexb_is_keyframe(p);
    if (codec_ == Codec::H265)
      packetize_h265(p, vseq_, vts_, ssrc_v_, cfg_.mtu, key, rtp);
    else if (codec_ == Codec::AV1) { /* RFC 9159: extensión (fase 9) */ }
    else
      packetize_h264(p, vseq_, vts_, ssrc_v_, cfg_.mtu, key, rtp);
    vbytes_ += static_cast<uint64_t>(p->size);
    av_packet_free(&p);
  }
  vts_ += static_cast<uint32_t>(90000 / cfg_.fps);
  vpkts_ += rtp.size();
  send_rtp_packets(sock_v_, to_v_, rtp);
  for (auto& pkt : rtp) {
    vbytes_since_ += pkt.size() + 28;  // RTP + UDP + IP
    Sent s;
    s.seq = static_cast<uint16_t>((pkt[2] << 8) | pkt[3]);
    s.data = std::move(pkt);
    s.t = static_cast<uint64_t>(now);
    nack_ring_.push_back(std::move(s));
  }
  while (nack_ring_.size() > 400) nack_ring_.pop_front();

  // Estadísticas cada ~500 ms
  if (now - last_stats_ns_ >= 500'000'000) {
    double dt = std::max(1.0, (now - last_stats_ns_) / 1e9);
    SenderStats st;
    st.fps = (last_fps_ns_ > 0)
                 ? fps_ema_ = fps_ema_ * 0.8 + (encoded_.load() - last_frames_) / dt * 0.2
                 : 0.0;
    last_fps_ns_ = now;
    last_frames_ = encoded_.load();
    st.kbps = vbytes_since_ * 8 / dt / 1000.0;
    vbytes_since_ = 0;
    st.frames_encoded = static_cast<int>(encoded_.load());
    st.frames_dropped = static_cast<int>(dropped_.load());
    st.nacks = nacks_.load();
    st.plis = plis_.load();
    last_stats_ns_ = now;
    Event e;
    e.status = SenderStatus::Streaming;
    e.stats = st;
    if (events_) events_(e);
  }
}

void SenderApp::audio_pcm_cb(const int16_t* pcm, int samples) {
  if (!aenc_ || !aenc_->ready()) return;
  std::vector<AVPacket*> pkts;
  aenc_->encode(pcm, samples, pkts);
  for (AVPacket* p : pkts) {
    std::vector<std::vector<uint8_t>> rtp;
    packetize_opus(p->data, p->size, aseq_, ats_, ssrc_a_, rtp);
    abytes_ += static_cast<uint64_t>(p->size);
    av_packet_free(&p);
    send_rtp_packets(sock_a_, to_a_, rtp);
  }
}

void SenderApp::send_rtp_packets(int fd, const sockaddr_in& to,
                                 std::vector<std::vector<uint8_t>>& pkts) {
  for (auto& pkt : pkts)
    sendto(fd, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr*>(&to),
           sizeof to);
}

void SenderApp::retransmit_nack(const std::vector<uint16_t>& seqs) {
  for (uint16_t s : seqs) {
    for (auto it = nack_ring_.rbegin(); it != nack_ring_.rend(); ++it) {
      if (it->seq == s) {
        sendto(sock_v_, it->data.data(), it->data.size(), 0,
               reinterpret_cast<const sockaddr*>(&to_v_), sizeof to_v_);
        nacks_.fetch_add(1);
        break;
      }
    }
  }
}

void SenderApp::rtcp_loop(int vid_rtcp_port, int aud_rtcp_port) {
  (void)vid_rtcp_port;
  (void)aud_rtcp_port;
  if (sock_vr_ < 0) return;
  pollfd pfd[2];
  pfd[0].fd = sock_vr_;
  pfd[0].events = POLLIN;
  pfd[1].fd = sock_ar_;
  pfd[1].events = POLLIN;
  uint64_t next_sr = 0;
  while (running_) {
    int rc = poll(pfd, 2, 100);
    if (rc > 0) {
      uint8_t buf[4096];
      // RTCP de retorno (NACK/PLI) puede llegar por cualquiera de los dos.
      for (int i = 0; i < 2; ++i) {
        const int fd = pfd[i].fd;
        if (fd < 0 || !(pfd[i].revents & POLLIN)) continue;
        sockaddr_in from{};
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0,
                             reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0) continue;
        RtcpPeer peer;
        rtcp_->parse(buf, static_cast<size_t>(n), peer,
                     [this](bool pli, const std::vector<uint16_t>& seqs) {
                       if (pli) {
                         plis_.fetch_add(1);
                         if (venc_) venc_->force_keyframe();
                       } else if (!seqs.empty()) {
                         retransmit_nack(seqs);
                       }
                     });
      }
    }
    uint64_t now = static_cast<uint64_t>(mono_ns());
    if (now >= next_sr) {
      next_sr = now + 500'000'000;  // SR cada 500 ms
      uint64_t ntp = ntp_ticks_now();
      if (sock_vr_ >= 0) {
        auto sr = rtcp_->build_sr(ssrc_v_, vts_, ntp,
                                  static_cast<uint32_t>(vpkts_),
                                  static_cast<uint32_t>(vbytes_));
        sendto(sock_vr_, sr.data(), sr.size(), 0,
               reinterpret_cast<const sockaddr*>(&to_vr_), sizeof to_vr_);
      }
      if (sock_ar_ >= 0 && cfg_.audio) {
        auto sr = rtcp_->build_sr(ssrc_a_, ats_, ntp,
                                  static_cast<uint32_t>(apkts_),
                                  static_cast<uint32_t>(abytes_));
        sendto(sock_ar_, sr.data(), sr.size(), 0,
               reinterpret_cast<const sockaddr*>(&to_ar_), sizeof to_ar_);
      }
    }
  }
}

void SenderApp::teardown_transport() {
  for (int fd : {sock_v_, sock_a_, sock_vr_, sock_ar_})
    if (fd >= 0) close(fd);
  sock_v_ = sock_a_ = sock_vr_ = sock_ar_ = -1;
  vcap_.reset();
  acap_.reset();
  venc_.reset();
  aenc_.reset();
  rtcp_.reset();
  nack_ring_.clear();
}

}  // namespace linky
