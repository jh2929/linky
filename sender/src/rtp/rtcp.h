#pragma once
// RTCP (RFC 3550): emisor envía SR (vídeo y audio) para sincronía A/V;
// recibe RR, NACK (transport-wide, RFC 4585/8108 simple) y PLI.
#include <cstdint>
#include <functional>

namespace linky {

struct RtcpPeer {
  uint32_t rtp_ssrc_video = 0;
  uint32_t rtp_ssrc_audio = 0;
  uint32_t sr_ntp_sec = 0;
  uint32_t sr_ntp_frac = 0;
  uint64_t sr_ntp_ticks = 0;  // ntp en ticks de 1<<32 por sec
  int rtt_ms = -1;
  uint32_t lost_packets = 0;
  uint32_t received_rtp = 0;
  uint32_t frac_lost = 0;
  uint64_t last_sr_sent = 0;
  bool has_sr = false;
};

class RtcpSender {
 public:
  using OnFeedback = std::function<void(bool is_pli, const std::vector<uint16_t>& nack_seqs)>;

  // Configura los SSRC y el socket RTCP del receptor (un solo socket mixto).
  void init(uint32_t video_ssrc, uint32_t audio_ssrc);

  // Construye un SR para un stream (90 kHz video / 48k audio).
  // rtp_ts_secs: timestamp RTP del paquete más reciente; ntp en ticks.
  std::vector<uint8_t> build_sr(uint32_t rtp_ssrc, uint64_t rtp_ts,
                                uint64_t ntp_ticks, uint32_t packet_count,
                                uint32_t octet_count);

  // Analiza un paquete RTCP recibido y extrae feedback.
  void parse(const uint8_t* data, size_t size, RtcpPeer& peer, OnFeedback cb);

 private:
  uint32_t video_ssrc_ = 0;
  uint32_t audio_ssrc_ = 0;
};

}  // namespace linky
