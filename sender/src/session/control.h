#pragma once
// Canal de control: TCP + JSON línea. Cliente (emisor) → servidor (receptor).
// Protocolo v1:
//   S→R  {"type":"hello","proto":1,"device":"<name>","senderId":"<sha256>",
//         "codecs":["h264","h265"],"audio":["opus"]}
//   R→S  {"type":"request","req":"<id>"}           (primer uso, aceptar/cancelar)
//   R→S  {"type":"welcome","session":"<uuid>","codec":"h265","audio":"opus",
//         "vport":61034,"vrtcp":61036,"aport":61035,"artcp":61037}
//   R→S  {"type":"denied"}
//   S→R  {"type":"bye"}
#include <functional>
#include <mutex>
#include <string>

#include "common/json.h"

namespace linky {

class ControlClient {
 public:
  struct Welcome {
    std::string session;
    std::string codec;    // h264 | h265 | av1
    std::string audio;    // opus
    int vport = 0, vrtcp = 0, aport = 0, artcp = 0;
  };

  using OnWelcome = std::function<void(const Welcome&)>;
  using OnAuthRequest = std::function<void()>;  // el receptor pide aceptación
  using OnDenied = std::function<void()>;
  using OnError = std::function<void(const std::string&)>;
  using OnClosed = std::function<void()>;

  ~ControlClient();
  // Conecta y envia hello; luego lee respuestas en su hilo.
  bool connect(const std::string& host, int port, const Json& hello);
  void close();
  void set_handlers(OnWelcome w, OnAuthRequest r, OnDenied d, OnError e, OnClosed c) {
    on_welcome_ = std::move(w);
    on_request_ = std::move(r);
    on_denied_ = std::move(d);
    on_error_ = std::move(e);
    on_closed_ = std::move(c);
  }

 private:
  int fd_ = -1;
  std::mutex mu_;
  OnWelcome on_welcome_;
  OnAuthRequest on_request_;
  OnDenied on_denied_;
  OnError on_error_;
  OnClosed on_closed_;
  void read_loop();
  void dispatch(const Json& msg);
};

}  // namespace linky
