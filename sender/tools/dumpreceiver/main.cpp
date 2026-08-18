// linky-dumpreceiver — receptor de prueba local (sin Android).
// Atiende el canal de control (hello→welcome), recibe RTP vídeo+audio,
// despaquetiza H.264/H.265 y decodifica con FFmpeg para verificar el
// pipeline completo del emisor en loopback. Uso:
//   linky-dumpreceiver [--decode]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "common/json.h"
#include "common/log.h"
#include "common/util.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

using namespace linky;

namespace {

constexpr int kCtrlPort = 61032;
constexpr int kVideoPort = 61034;
constexpr int kVideoRtcpPort = 61036;
constexpr int kAudioPort = 61035;

std::atomic<uint64_t> g_bytes_video{0}, g_bytes_audio{0};
std::atomic<uint64_t> g_pkts_video{0}, g_pkts_audio{0};
std::atomic<int> g_frames{0}, g_key{0};

int64_t mono_now_ms() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

int bind_udp(int port, int type) {
  int fd = socket(AF_INET, type, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
    perror("bind");
    close(fd);
    return -1;
  }
  return fd;
}

bool is_keyframe(const uint8_t* data, int size) {
  if (size < 6) return false;
  // El AU comienza con start code (00 00 00 01): cabecera NAL en data[4].
  // H.264: 1 byte, tipo 5 = IDR. H.265: 2 bytes; 32 = VPS, 33 = SPS
  // (primera NAL del keyframe), 34 = PPS, 19-21 = IDR/CRA.
  const int t264 = data[4] & 0x1f;
  const int t265 = (data[4] >> 1) & 0x3f;
  return t264 == 5 || t265 == 32 || t265 == 33 || (t265 >= 19 && t265 <= 21);
}

// ── Despaquetizador RTP mínimo (H.264/H.265: NAL enteros + FU) ───────────
class Depacketizer {
 public:
  std::function<void(const uint8_t*, int)> on_au;

  void feed(const uint8_t* pkt, int size) {
    if (size < 12) return;
    const uint8_t* p = pkt + 12;
    int len = size - 12;
    if (len < 1) return;
    uint8_t nal = p[0] & 0x1f;
    if (nal == 28) {  // H.264 FU-A
      uint8_t fu = p[1];
      if (fu & 0x80) {
        annexb_.clear();
        push_sc();
        annexb_.push_back(static_cast<uint8_t>(0x60 | (fu & 0x1f)));
        annexb_.insert(annexb_.end(), p + 2, p + len);
      } else {
        annexb_.insert(annexb_.end(), p + 2, p + len);
      }
      if (fu & 0x40) flush();
    } else if (nal == 49) {  // H.265 FU
      uint8_t fu = p[1];
      uint8_t type = static_cast<uint8_t>((fu >> 1) & 0x3f);
      if (fu & 0x80) {
        annexb_.clear();
        push_sc();
        annexb_.push_back(static_cast<uint8_t>((p[0] & 0x81) | ((type & 0x3f) << 1)));
        annexb_.insert(annexb_.end(), p + 2, p + len);
      } else {
        annexb_.insert(annexb_.end(), p + 2, p + len);
      }
      if (fu & 0x40) flush();
    } else if (nal == 32 || nal == 33 || nal == 34 || nal == 39 ||
               (nal <= 21 && nal >= 1)) {
      push_sc();
      annexb_.insert(annexb_.end(), p, p + len);
      flush();
    }
  }

 private:
  std::vector<uint8_t> annexb_;

  void push_sc() { annexb_.insert(annexb_.end(), {0, 0, 0, 1}); }
  void flush() {
    if (annexb_.empty()) return;
    if (on_au) on_au(annexb_.data(), static_cast<int>(annexb_.size()));
    annexb_.clear();
  }
};

}  // namespace

static std::string g_codec = "h264";

static void control_loop() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(kCtrlPort);
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || listen(fd, 4) < 0) {
    LERR("dump", "control bind/listen");
    return;
  }
  LINF("dump", "control en :%d", kCtrlPort);
  for (;;) {
    int c = accept(fd, nullptr, nullptr);
    if (c < 0) break;
    std::string buf;
    LINF("dump", "emisor conectado al control");
    for (;;) {
      char chunk[1024];
      ssize_t n = recv(c, chunk, sizeof chunk, 0);
      if (n <= 0) break;
      buf.append(chunk, static_cast<size_t>(n));
      size_t eol = buf.find('\n');
      if (eol == std::string::npos) continue;
      Json hello;
      json_decode(buf.substr(0, eol), hello);
      std::string codecs = hello["codecs"];
      std::string codec = codecs.find("h265") != std::string::npos ? "h265" : "h264";
      g_codec = codec;
      Json welcome;
      welcome["type"] = "welcome";
      welcome["session"] = uuid4();
      welcome["codec"] = codec;
      welcome["audio"] = "opus";
      welcome["vport"] = std::to_string(kVideoPort);
      welcome["vrtcp"] = std::to_string(kVideoRtcpPort);
      welcome["aport"] = std::to_string(kAudioPort);
      welcome["artcp"] = std::to_string(kVideoRtcpPort);  // compartido
      std::string msg = json_encode(welcome) + "\n";
      send(c, msg.data(), msg.size(), 0);
      LINF("dump", "welcome enviado (códec %s)", codec.c_str());
      buf.erase(0, eol + 1);
    }
    close(c);
  }
}

static void video_loop(bool decode) {
  int fd = bind_udp(kVideoPort, SOCK_DGRAM);
  int rtcp_fd = bind_udp(kVideoRtcpPort, SOCK_DGRAM);
  Depacketizer dep;
  AVCodecContext* dctx = nullptr;
  sockaddr_in sender_rtcp{};
  bool have_sender_rtcp = false;

  if (decode) {
    const AVCodec* codec =
        avcodec_find_decoder(g_codec == "h265" ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (codec) {
      dctx = avcodec_alloc_context3(codec);
      dctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
      if (avcodec_open2(dctx, codec, nullptr) < 0) {
        avcodec_free_context(&dctx);
        dctx = nullptr;
      }
    }
  }
  dep.on_au = [&](const uint8_t* data, int size) {
    g_frames.fetch_add(1);
    if (is_keyframe(data, size)) g_key.fetch_add(1);
    if (dctx) {
      AVPacket pkt;
      av_init_packet(&pkt);
      pkt.data = const_cast<uint8_t*>(data);
      pkt.size = size;
      if (avcodec_send_packet(dctx, &pkt) == 0) {
        AVFrame* f = av_frame_alloc();
        while (avcodec_receive_frame(dctx, f) == 0) av_frame_unref(f);
        av_frame_free(&f);
      }
    }
  };

  auto send_fb = [&](const std::vector<uint8_t>& msg) {
    if (!have_sender_rtcp) return;
    sendto(rtcp_fd, msg.data(), msg.size(), 0,
           reinterpret_cast<const sockaddr*>(&sender_rtcp), sizeof sender_rtcp);
  };

  int64_t last_report = 0, last_pli = 0;
  int nacks_sent = 0;
  for (;;) {
    pollfd pfd[2] = {{fd, POLLIN, 0}, {rtcp_fd, POLLIN, 0}};
    if (poll(pfd, 2, 100) > 0) {
      if (pfd[1].revents & POLLIN) {  // SR del emisor → aprender su dirección
        uint8_t buf[2048];
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(rtcp_fd, buf, sizeof buf, 0,
                             reinterpret_cast<sockaddr*>(&from), &fl);
        if (n >= 4) {
          sender_rtcp = from;
          have_sender_rtcp = true;
          char ip[64];
          inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
          fprintf(stderr, "[dump] aprendido emisor rtcp %s:%u (n=%zd)\n", ip,
                  ntohs(from.sin_port), n);
        }
      }
      if (pfd[0].revents & POLLIN) {
        uint8_t buf[2048];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n >= 14) {
          uint16_t seq = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
          bool marker = buf[1] & 0x80;
          // Pérdida simulada (1 %) → NACK real, y PLI periódico (7 s)
          if (seq % 100 == 7) {
            if (nacks_sent++ < 3) {
              std::array<uint8_t, 16> nack{};
              nack[0] = 0x81;
              nack[1] = 205;
              nack[2] = 0;
              nack[3] = 2;
              nack[8] = static_cast<uint8_t>(seq >> 8);
              nack[9] = static_cast<uint8_t>(seq & 0xff);
              nack[14] = 0;
              nack[15] = 1;
              send_fb(std::vector<uint8_t>(nack.begin(), nack.end()));
              LINF("dump", "NACK enviado por seq %u", seq);
            }
          } else {
            dep.feed(buf, static_cast<int>(n));
          }
          g_pkts_video.fetch_add(1);
          g_bytes_video.fetch_add(static_cast<uint64_t>(n));
          (void)marker;
        }
      }
    }
    int64_t now = mono_now_ms();
    if (now - last_pli > 7000) {
      last_pli = now;
      std::array<uint8_t, 16> pli{};
      pli[0] = 0x81;
      pli[1] = 206;
      pli[2] = 0;
      pli[3] = 2;
      pli[8] = 1;
      send_fb(std::vector<uint8_t>(pli.begin(), pli.end()));
      LINF("dump", "PLI enviado");
    }
    if (now - last_report >= 2000) {
      last_report = now;
      printf("[dump] video: %d frames/s (%d key), %.0f kbps, %llu pkt/s\n",
             g_frames.exchange(0), g_key.exchange(0),
             g_bytes_video.load() * 8.0 / 2.0 / 1000.0,
             static_cast<unsigned long long>(g_pkts_video.exchange(0)));
      printf("[dump] audio: %.0f kbps, %llu pkt/s\n",
             g_bytes_audio.load() * 8.0 / 2.0 / 1000.0,
             static_cast<unsigned long long>(g_pkts_audio.exchange(0)));
      fflush(stdout);
    }
  }
  if (dctx) avcodec_free_context(&dctx);
}

static void audio_loop() {
  int fd = bind_udp(kAudioPort, SOCK_DGRAM);
  for (;;) {
    uint8_t buf[2048];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n >= 14) {
      g_pkts_audio.fetch_add(1);
      g_bytes_audio.fetch_add(static_cast<uint64_t>(n));
    }
  }
}

int main(int argc, char** argv) {
  bool decode = argc > 1 && strcmp(argv[1], "--decode") == 0;
  log::sink().min = log::Level::Info;
  printf("linky-dumpreceiver — receptor de prueba local\n");
  printf(" control :%d  video :%d (rtcp %d)  audio :%d\n", kCtrlPort, kVideoPort,
         kVideoRtcpPort, kAudioPort);
  printf("%s\n", decode ? "decodificación E2E habilitada"
                        : "solo contaje (usa --decode para E2E)");
  std::thread t1(control_loop);
  std::thread t2(video_loop, decode);
  std::thread t3(audio_loop);
  t1.join();
  t2.join();
  t3.join();
  return 0;
}