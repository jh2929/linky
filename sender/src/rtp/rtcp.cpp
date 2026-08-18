#include "rtp/rtcp.h"

#include <cstring>

namespace linky {

void RtcpSender::init(uint32_t video_ssrc, uint32_t audio_ssrc) {
  video_ssrc_ = video_ssrc;
  audio_ssrc_ = audio_ssrc;
}

std::vector<uint8_t> RtcpSender::build_sr(uint32_t rtp_ssrc, uint64_t rtp_ts,
                                          uint64_t ntp_ticks,
                                          uint32_t packet_count,
                                          uint32_t octet_count) {
  // Cabecera compuesta: SR (RFC 3550 6.4.1)
  uint8_t buf[28] = {0};
  buf[0] = 0x80;         // version 2, RC=0
  buf[1] = 200;          // SR
  buf[2] = 0;
  buf[3] = 6;            // 6 words = 24 bytes payload
  auto put32 = [&](size_t off, uint32_t v) {
    buf[off] = static_cast<uint8_t>(v >> 24);
    buf[off + 1] = static_cast<uint8_t>(v >> 16);
    buf[off + 2] = static_cast<uint8_t>(v >> 8);
    buf[off + 3] = static_cast<uint8_t>(v);
  };
  put32(4, rtp_ssrc);
  put32(8, static_cast<uint32_t>(ntp_ticks >> 32));
  put32(12, static_cast<uint32_t>(ntp_ticks));
  put32(16, static_cast<uint32_t>(rtp_ts));
  put32(20, packet_count);
  put32(24, octet_count);
  return std::vector<uint8_t>(buf, buf + 28);
}

void RtcpSender::parse(const uint8_t* data, size_t size, RtcpPeer& peer,
                       OnFeedback cb) {
  size_t off = 0;
  while (off + 4 <= size) {
    // RTCP: el byte de tipo es completo (SR=200, RR=201, NACK=205, PLI=206);
    // NO enmascarar con 0x7f (eso es RTP con marker bit).
    uint8_t pt = data[off + 1];
    size_t len_words = static_cast<size_t>((data[off + 2] << 8) | data[off + 3]);
    size_t len = (len_words + 1) * 4;
    if (off + len > size) break;

    if (pt == 200) {  // SR del receptor (rare) — ignorar cuerpo
    } else if (pt == 201) {  // RR
      const uint8_t* p = data + off + 4;
      uint32_t ssrc = (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
      p += 4;
      uint32_t frac = p[0];
      uint32_t cum = ((static_cast<uint32_t>(p[1]) << 16) | (p[2] << 8) | p[3]);
      p += 4;
      uint32_t ext = (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
      p += 4;
      uint32_t lsr = (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
      p += 4;
      uint32_t dlsr = (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
      (void)ssrc;
      (void)ext;
      peer.frac_lost = frac;
      peer.lost_packets = cum;
      // RTT estimado si lsr/dlsr válidos
      if (lsr && dlsr && peer.last_sr_sent) {
        uint64_t now = 0;  // aprox: el emisor mide con su reloj; dlsr viene en 1/65536 s
        (void)now;
      }
      (void)lsr;
      (void)dlsr;
    } else if (pt == 205) {  // transporte de feedback genérico (NACK)
      const uint8_t* p = data + off + 8;
      if (len >= 12) {
        uint16_t pid = (static_cast<uint16_t>(p[0]) << 8) | p[1];
        uint16_t blp = (static_cast<uint16_t>(p[2]) << 8) | p[3];
        std::vector<uint16_t> seqs;
        seqs.push_back(pid);
        for (int i = 0; i < 16; ++i)
          if (blp & (1u << i)) seqs.push_back(static_cast<uint16_t>(pid + i + 1));
        if (cb) cb(false, seqs);
      }
    } else if (pt == 206 && len >= 12 && data[off + 8] == 1) {  // PLI (PSFB)
      if (cb) cb(true, {});
    }
    off += len;
  }
}

}  // namespace linky
