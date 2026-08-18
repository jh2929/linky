// Linky Sender — punto de entrada. Modos:
//   linky-sender --probe                        sondeo de capacidades HW
//   linky-sender --list                         lista receptores DNS-SD
//   linky-sender --connect <nombre|ip> [opts]   transmisión CLI
//   linky-sender (sin args)                     interfaz GTK4 (si compilada)
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "app/sender_app.h"
#include "common/log.h"
#include "common/util.h"
#include "discovery/discovery.h"
#include "encode/encoder.h"

using namespace linky;

namespace {
std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit = true; }
}  // namespace

static void run_probe() {
  EncoderInfo info = probe_encoders();
  printf("Encoders disponibles (backend: %s)\n", info.backend.c_str());
  if (info.codecs.empty()) {
    printf("  ninguna aceleración disponible en esta máquina\n");
    return;
  }
  for (Codec c : info.codecs) printf("  - %s (hardware)\n", codec_name(c));

  printf("Captura de vídeo: %s\n",
         getenv("WAYLAND_DISPLAY") ? "Wayland (portal ScreenCast + PipeWire)"
                                   : (getenv("DISPLAY") ? "X11 (x11grab)"
                                                        : "sin sesión gráfica"));
  FILE* p = popen("pactl list short sinks 2>/dev/null", "r");
  if (p) {
    printf("Audio (monitor del sistema):\n");
    char line[512];
    while (fgets(line, sizeof line, p)) printf("  - %s", line);
    pclose(p);
  }
}

static Device pick_device(const std::vector<Device>& devices,
                          const std::string& target) {
  for (const auto& d : devices)
    if (d.name == target || d.host == target) return d;
  return {};
}

static int run_cli(const std::string& connect_to, SenderConfig cfg) {
  Discovery disc;
  Device target;
  bool have_target = false;

  // IP directa (override de diagnóstico; no se pide al usuario)
  if (connect_to.find('.') != std::string::npos) {
    target.name = connect_to;
    target.host = connect_to;
    target.port = 61032;
    have_target = true;
  }

  if (!have_target) {
    disc.start(nullptr);
    for (int i = 0; i < 24 && !have_target; ++i) {  // hasta ~6 s
      usleep(250'000);
      target = pick_device(disc.devices(), connect_to);
      if (!target.host.empty()) have_target = true;
    }
    if (!have_target) {
      std::cerr << "receptor '" << connect_to
                << "' no encontrado en la red (_linky._tcp)\n";
      return 1;
    }
  }

  EncoderInfo info = probe_encoders();
  if (info.codecs.empty()) {
    std::cerr << "este sistema no puede codificar vídeo (sin HW ni CPU)\n";
    return 1;
  }
  Json caps;
  std::string codec_str;
  for (Codec c : info.codecs) {
    if (!codec_str.empty()) codec_str += ",";
    codec_str += codec_name(c);
  }
  caps["codecs"] = codec_str;
  caps["audio"] = "opus";

  printf("Destino: %s (%s:%d)  códecs: %s\n", target.name.c_str(),
         target.host.c_str(), target.port, codec_str.c_str());

  SenderApp app;
  app.set_target(target.host, target.port, target.name, caps);
  app.start(cfg, [](const SenderApp::Event& e) {
    const char* st = e.status == SenderStatus::Connecting
                         ? "conectando"
                         : e.status == SenderStatus::WaitingAcceptance
                               ? "esperando aceptación"
                               : e.status == SenderStatus::Streaming
                                     ? "transmitiendo"
                                     : e.status == SenderStatus::Failed
                                           ? "error"
                                           : "inactivo";
    if (!e.message.empty()) printf("[%s] %s\n", st, e.message.c_str());
    if (e.status == SenderStatus::Streaming)
      printf("[stats] %.1f fps  %.0f kbps  frames=%d  drop=%d  nack=%d  pli=%d\n",
             e.stats.fps, e.stats.kbps, e.stats.frames_encoded,
             e.stats.frames_dropped, e.stats.nacks, e.stats.plis);
    fflush(stdout);
  });

  while (!g_quit) usleep(200'000);
  printf("\ndeteniendo…\n");
  app.stop();
  return 0;
}

int main(int argc, char** argv) {
  std::string connect_to;
  SenderConfig cfg;
  cfg.device_name = hostname();
  bool probe = false, list_only = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "falta valor para " << name << "\n";
        exit(2);
      }
      return argv[++i];
    };
    if (a == "--probe") probe = true;
    else if (a == "--list") list_only = true;
    else if (a == "--connect") connect_to = next("--connect");
    else if (a == "--name") cfg.device_name = next("--name");
    else if (a == "--width") cfg.width = std::stoi(next("--width"));
    else if (a == "--height") cfg.height = std::stoi(next("--height"));
    else if (a == "--fps") cfg.fps = std::stoi(next("--fps"));
    else if (a == "--bitrate") cfg.bitrate_kbps = std::stoi(next("--bitrate"));
    else if (a == "--no-audio") cfg.audio = false;
    else {
      std::cerr << "uso: linky-sender [--probe|--list|--connect <nombre|ip> "
                   "[--name X] [--width W] [--height H] [--fps N] "
                   "[--bitrate KBPS] [--no-audio]]\n";
      return 2;
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (probe) {
    run_probe();
    return 0;
  }

  if (list_only) {
    Discovery disc;
    disc.start(nullptr);
    printf("Buscando receptores Linky (_linky._tcp)…\n");
    sleep(3);
    auto devs = disc.devices();
    if (devs.empty()) {
      printf("  (ningún receptor encontrado)\n");
    } else {
      int i = 1;
      for (const auto& d : devs)
        printf("%d. %s (modelo=%s, códecs=%s, audio=%s, ip=%s:%d)\n", i++,
               d.name.c_str(), d.model.empty() ? "?" : d.model.c_str(),
               d.codecs.empty() ? "?" : d.codecs.c_str(),
               d.audio.empty() ? "?" : d.audio.c_str(), d.host.c_str(), d.port);
    }
    disc.stop();
    return 0;
  }

#if LINKY_HAS_GTK
  if (connect_to.empty()) {
    extern int gtk_run();  // ui/gtk_app.cpp
    return gtk_run();
  }
#endif

  return run_cli(connect_to, cfg);
}