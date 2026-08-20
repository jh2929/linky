#include "rtp/packetizer.h"

#include <algorithm>
#include <cstring>

namespace linky {

namespace {
constexpr int kRtpFixed = 12;

void rtp_append(std::vector<std::vector<uint8_t>>& out, const RtpHeader& h,
                const uint8_t* payload, int size) {
  std::vector<uint8_t> pkt(static_cast<size_t>(kRtpFixed + size));
  rtp_build_header(pkt.data(), h);
  if (size > 0) memcpy(pkt.data() + kRtpFixed, payload, static_cast<size_t>(size));
  out.push_back(std::move(pkt));
}

// NAL de Annex B: devuelve puntero+longitud y avanza.
// Solo 00 00 00 01 / 00 00 01 delimitan NAL; 00 00 03 (emulación de
// inicio de código) NO es frontera — sin esto, los SPS/PPS/VPS/IDR con
// bytes < 0x04 quedaban truncados y el decodificador los rechazaba.
const uint8_t* next_nal(const uint8_t* data, size_t size, size_t& offset,
                        size_t& nal_len, int& nal_type) {
  while (offset + 3 < size) {
    if (data[offset] == 0 && data[offset + 1] == 0) {
      size_t code = (data[offset + 2] == 1) ? 3 : (data[offset + 2] == 0 && offset + 3 < size && data[offset + 3] == 1 ? 4 : 0);
      if (code) {
        const uint8_t* start = data + offset + code;
        size_t end = offset + code;
        while (end + 2 < size) {
          if (data[end] == 0 && data[end + 1] == 0) {
            if (data[end + 2] == 1) break;  // start code siguiente
            if (data[end + 2] == 0 && end + 3 < size && data[end + 3] == 1) break;
            if (data[end + 2] == 3) {        // 00 00 03: emulación, seguir
              end += 3;
              continue;
            }
          }
          ++end;
        }
        if (end + 2 >= size) end = size;
        nal_len = end - (offset + code);
        if (nal_len < 2) {
          offset = end;
          continue;
        }
        nal_type = start[0] & 0x1f;
        offset = end;
        return start;
      }
    }
    ++offset;
  }
  return nullptr;
}

bool nal_is_vcl(int type) { return (type >= 1 && type <= 5) || type == 20 || type == 21; }
}  // namespace

void rtp_build_header(uint8_t* out, const RtpHeader& h) {
  out[0] = 0x80;
  out[1] = static_cast<uint8_t>((h.marker ? 0x80 : 0) | (h.pt & 0x7f));
  out[2] = static_cast<uint8_t>(h.seq >> 8);
  out[3] = static_cast<uint8_t>(h.seq & 0xff);
  out[4] = static_cast<uint8_t>(h.ts >> 24);
  out[5] = static_cast<uint8_t>(h.ts >> 16);
  out[6] = static_cast<uint8_t>(h.ts >> 8);
  out[7] = static_cast<uint8_t>(h.ts);
  out[8] = static_cast<uint8_t>(h.ssrc >> 24);
  out[9] = static_cast<uint8_t>(h.ssrc >> 16);
  out[10] = static_cast<uint8_t>(h.ssrc >> 8);
  out[11] = static_cast<uint8_t>(h.ssrc);
}

bool annexb_is_keyframe(const AVPacket* pkt) {
  if (!pkt || pkt->size < 6) return false;
  // Recorre todas las NAL del paquete: el IDR puede no ser la primera
  // (VAAPI h264 emite SPS+PPS+SEI+IDR en el primer paquete; comprobar solo
  // data[4] lo perdía y el receptor nunca recibía SPS/PPS → PPS no existe).
  size_t offset = 0, nal_len = 0;
  int nal_type = 0;
  const uint8_t* n =
      next_nal(pkt->data, static_cast<size_t>(pkt->size), offset, nal_len, nal_type);
  while (n) {
    // h264: 5 = IDR (SPS 7 / PPS 8 también valen: preceden al IDR).
    // h265: 32 = VPS, 33 = SPS, 19-21 = IDR_W_RADL/IDR_N_LP/CRA.
    const int t264 = n[0] & 0x1f;
    const int t265 = (n[0] >> 1) & 0x3f;
    if (t264 == 5 || t264 == 7 || t264 == 8 || t265 == 32 || t265 == 33 ||
        (t265 >= 19 && t265 <= 21))
      return true;
    n = next_nal(pkt->data, static_cast<size_t>(pkt->size), offset, nal_len, nal_type);
  }
  return false;
}

void packetize_h264(const AVPacket* annexb, uint16_t& seq, uint32_t ts,
                    uint32_t ssrc, int mtu, bool keyframe,
                    std::vector<std::vector<uint8_t>>& out) {
  RtpHeader h;
  h.ts = ts;
  h.ssrc = ssrc;
  h.pt = 96;
  size_t offset = 0, nal_len = 0;
  int nal_type = 0;
  std::vector<std::pair<const uint8_t*, size_t>> nals;
  const uint8_t* n = next_nal(annexb->data, static_cast<size_t>(annexb->size), offset, nal_len, nal_type);
  while (n) {
    if (nal_type <= 5 && nal_type >= 1)
      nals.emplace_back(n, nal_len);  // VCL
    n = next_nal(annexb->data, static_cast<size_t>(annexb->size), offset, nal_len, nal_type);
  }

  bool first = true;
  // SPS/PPS solo en keyframe, antes del IDR.
  if (keyframe) {
    size_t o2 = 0, l2 = 0;
    int t2 = 0;
    const uint8_t* p = next_nal(annexb->data, static_cast<size_t>(annexb->size), o2, l2, t2);
    while (p) {
      if (t2 == 7 || t2 == 8) {  // SPS, PPS
        h.seq = seq++;
        h.marker = false;
        rtp_append(out, h, p, static_cast<int>(l2));
      }
      p = next_nal(annexb->data, static_cast<size_t>(annexb->size), o2, l2, t2);
    }
  }

  for (const auto& [data, len] : nals) {
    h.seq = seq++;
    if (len <= static_cast<size_t>(mtu)) {
      h.marker = false;
      rtp_append(out, h, data, static_cast<int>(len));
      continue;
    }
    // FU-A (RFC 6184 5.8): 1 byte indicador (tipo 28) + 1 byte cabecera FU.
    // El 1er fragmento lleva la cabecera NAL completa en el payload: el
    // receptor ensambla start code + payload sin reconstruir cabeceras.
    const int payload = mtu - 2;
    size_t pos = 0;
    int idx = 0;
    while (pos < len) {
      size_t chunk = std::min<size_t>(payload, len - pos);
      bool last = pos + chunk >= len;
      uint8_t fu[2] = {static_cast<uint8_t>((data[0] & 0x60) | 28),
                       static_cast<uint8_t>((idx == 0 ? 0x80 : 0) | (last ? 0x40 : 0) | (data[0] & 0x1f))};
      h.seq = seq++;
      h.marker = false;
      std::vector<uint8_t> pkt(static_cast<size_t>(kRtpFixed + 2 + chunk));
      rtp_build_header(pkt.data(), h);
      memcpy(pkt.data() + kRtpFixed, fu, 2);
      memcpy(pkt.data() + kRtpFixed + 2, data + pos, chunk);
      out.push_back(std::move(pkt));
      pos += chunk;
      ++idx;
    }
  }
  if (!nals.empty()) out.back()[1] |= 0x80;  // marker en el último paquete del AU
}

void packetize_h265(const AVPacket* annexb, uint16_t& seq, uint32_t ts,
                    uint32_t ssrc, int mtu, bool keyframe,
                    std::vector<std::vector<uint8_t>>& out) {
  RtpHeader h;
  h.ts = ts;
  h.ssrc = ssrc;
  h.pt = 97;
  size_t offset = 0, nal_len = 0;
  int nal_type = 0;
  const uint8_t* n = next_nal(annexb->data, static_cast<size_t>(annexb->size), offset, nal_len, nal_type);
  while (n) {
    if (nal_type >= 0 && nal_type <= 21) {  // VCL + SPS/PPS/VPS (32-34) incluidos aquí
      if (nal_type >= 32 && nal_type <= 34) {
        h.seq = seq++;
        h.marker = false;
        rtp_append(out, h, n, static_cast<int>(nal_len));
        n = next_nal(annexb->data, static_cast<size_t>(annexb->size), offset, nal_len, nal_type);
        continue;
      }
      if (nal_len <= static_cast<size_t>(mtu)) {
        h.seq = seq++;
        h.marker = false;
        rtp_append(out, h, n, static_cast<int>(nal_len));
      } else {
        // FU (RFC 7798, mismo layout compacto que FU-A): indicador tipo 49
        // (bits 1..6) + cabecera S|E|tipo; el 1er fragmento lleva la cabecera
        // NAL completa en el payload (el receptor no reconstruye cabeceras).
        const int payload = mtu - 2;
        size_t pos = 0;
        int idx = 0;
        while (pos < nal_len) {
          size_t chunk = std::min<size_t>(payload, nal_len - pos);
          bool last = pos + chunk >= nal_len;
          uint8_t fu[2] = {0x62,  // 49<<1: FU H.265, F=0, capa 0
                           static_cast<uint8_t>((idx == 0 ? 0x80 : 0) | (last ? 0x40 : 0) | ((n[0] >> 1) & 0x3f))};
          h.seq = seq++;
          h.marker = false;
          std::vector<uint8_t> pkt(static_cast<size_t>(kRtpFixed + 2 + chunk));
          rtp_build_header(pkt.data(), h);
          memcpy(pkt.data() + kRtpFixed, fu, 2);
          memcpy(pkt.data() + kRtpFixed + 2, n + pos, chunk);
          out.push_back(std::move(pkt));
          pos += chunk;
          ++idx;
        }
      }
    }
    n = next_nal(annexb->data, static_cast<size_t>(annexb->size), offset, nal_len, nal_type);
  }
  if (out.size() > 0) {
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
      if (!(*it).empty()) {
        (*it)[1] |= 0x80;
        break;
      }
    }
  }
}

void packetize_opus(const uint8_t* data, int size, uint16_t& seq,
                    uint32_t& ts, uint32_t ssrc,
                    std::vector<std::vector<uint8_t>>& out) {
  RtpHeader h;
  h.seq = seq++;
  h.ts = ts;
  h.ssrc = ssrc;
  h.pt = 111;
  h.marker = false;
  rtp_append(out, h, data, size);
  ts += 960;  // 20 ms @48 kHz
}

}  // namespace linky
