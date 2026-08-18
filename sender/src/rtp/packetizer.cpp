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
const uint8_t* next_nal(const uint8_t* data, size_t size, size_t& offset,
                        size_t& nal_len, int& nal_type) {
  while (offset + 3 < size) {
    if (data[offset] == 0 && data[offset + 1] == 0) {
      size_t code = (data[offset + 2] == 1) ? 3 : (data[offset + 2] == 0 && offset + 3 < size && data[offset + 3] == 1 ? 4 : 0);
      if (code) {
        const uint8_t* start = data + offset + code;
        size_t end = offset + code;
        while (end + 2 < size &&
               !(data[end] == 0 && data[end + 1] == 0)) {
          end += (data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1) ? 2 : 1;
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
  // NAL con start code de 4 bytes (00 00 00 01): cabecera en data[4].
  // h264 (1 byte): tipo 5 = IDR. h265 (2 bytes): 32 = VPS, 33 = SPS
  // (primera NAL del keyframe), 19-21 = IDR_W_RADL/IDR_N_LP/CRA.
  const int t264 = pkt->data[4] & 0x1f;
  const int t265 = (pkt->data[4] >> 1) & 0x3f;
  return t264 == 5 || t265 == 32 || t265 == 33 || (t265 >= 19 && t265 <= 21);
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
    // FU-A (RFC 6184 5.8): 1 byte FU indicator + 1 byte FU header
    const int payload = mtu - 2;
    size_t pos = 1;
    int idx = 0;
    while (pos < len) {
      size_t chunk = std::min<size_t>(payload, len - pos);
      bool last = pos + chunk >= len;
      uint8_t fu[2] = {static_cast<uint8_t>(0x60 | (data[0] & 0x1f)),
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
        // FU (RFC 7798): 2 bytes de cabecera FU
        const int payload = mtu - 2;
        size_t pos = 1;
        int idx = 0;
        while (pos < nal_len) {
          size_t chunk = std::min<size_t>(payload, nal_len - pos);
          bool last = pos + chunk >= nal_len;
          uint8_t fu2[2] = {0x80 | 49, static_cast<uint8_t>((idx == 0 ? 0x80 : 0) | (last ? 0x40 : 0) | (n[0] & 0x3f))};
          h.seq = seq++;
          h.marker = false;
          std::vector<uint8_t> pkt(static_cast<size_t>(kRtpFixed + 2 + chunk));
          rtp_build_header(pkt.data(), h);
          memcpy(pkt.data() + kRtpFixed, fu2, 2);
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
