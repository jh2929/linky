#pragma once
// Paquetización RTP: H.264 (RFC 6184), H.265 (RFC 7798), Opus (RFC 7587).
// AV1 (RFC 9159) queda reservado para cuando exista HW en el emisor.
#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
}

namespace linky {

struct RtpHeader {
  uint16_t seq = 0;
  uint32_t ts = 0;
  uint8_t pt = 96;
  bool marker = false;
  uint32_t ssrc = 0;
};

// RTP básico: cabecera de 12 bytes.
void rtp_build_header(uint8_t* out, const RtpHeader& h);

// Divide un paquete Annex B del encoder en paquetes RTP (payload ≤ mtu).
// `keyframe` indica si el AU empieza en IDR (se anteponen SPS/PPS).
void packetize_h264(const AVPacket* annexb, uint16_t& seq, uint32_t ts,
                    uint32_t ssrc, int mtu, bool keyframe,
                    std::vector<std::vector<uint8_t>>& out);

// Igual para HEVC (VPS/SPS/PPS + FU de RFC 7798).
void packetize_h265(const AVPacket* annexb, uint16_t& seq, uint32_t ts,
                    uint32_t ssrc, int mtu, bool keyframe,
                    std::vector<std::vector<uint8_t>>& out);

// Un frame Opus (20 ms) → un paquete RTP (RFC 7587).
void packetize_opus(const uint8_t* data, int size, uint16_t& seq,
                    uint32_t& ts, uint32_t ssrc,
                    std::vector<std::vector<uint8_t>>& out);

// Extrae de un flujo Annex B: 0=no es keyframe, 1=es IDR/I-frame.
bool annexb_is_keyframe(const AVPacket* pkt);

}  // namespace linky
