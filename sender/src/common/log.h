#pragma once
// Logger mínimo, por módulos, con timestamps y color (solo tty).
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <time.h>

namespace linky::log {

enum class Level { Debug, Info, Warn, Error };

struct Sink {
  Level min = Level::Info;
  std::mutex mu;
};

inline Sink& sink() {
  static Sink s;
  return s;
}

inline const char* name(Level l) {
  switch (l) {
    case Level::Debug: return "DBG";
    case Level::Info:  return "INF";
    case Level::Warn:  return "WRN";
    case Level::Error: return "ERR";
  }
  return "???";
}

template <typename... A>
void write(Level l, const char* mod, const char* fmt, A... args) {
  if (static_cast<int>(l) < static_cast<int>(sink().min)) return;
  std::lock_guard<std::mutex> g(sink().mu);
  struct timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  char t[32];
  snprintf(t, sizeof t, "%02ld:%02ld:%02ld.%03ld", (ts.tv_sec % 86400) / 3600,
           (ts.tv_sec % 3600) / 60, ts.tv_sec % 60, ts.tv_nsec / 1000000);
  fprintf(stderr, "[%s][%s][%s] ", t, name(l), mod);
  fprintf(stderr, fmt, args...);
  fputc('\n', stderr);
}

#define LDBG(mod, ...) ::linky::log::write(::linky::log::Level::Debug, mod, __VA_ARGS__)
#define LINF(mod, ...) ::linky::log::write(::linky::log::Level::Info,  mod, __VA_ARGS__)
#define LWRN(mod, ...) ::linky::log::write(::linky::log::Level::Warn,  mod, __VA_ARGS__)
#define LERR(mod, ...) ::linky::log::write(::linky::log::Level::Error, mod, __VA_ARGS__)

}  // namespace linky::log
