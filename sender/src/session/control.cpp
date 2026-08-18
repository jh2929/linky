#include "session/control.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <thread>

#include "common/log.h"

namespace linky {

ControlClient::~ControlClient() { close(); }

bool ControlClient::connect(const std::string& host, int port, const Json& hello) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    hostent* he = gethostbyname(host.c_str());
    if (!he) {
      ::close(fd);
      return false;
    }
    memcpy(&addr.sin_addr, he->h_addr, sizeof addr.sin_addr);
  }
  // Sin SO_RCVTIMEO: la desconexión la decide shutdown() (close() en stop()).
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  std::string msg = json_encode(hello) + "\n";
  ssize_t sent = send(fd_, msg.data(), msg.size(), 0);
  if (sent < 0) {
    close();
    return false;
  }
  std::thread(&ControlClient::read_loop, this).detach();
  return true;
}

void ControlClient::close() {
  std::lock_guard<std::mutex> g(mu_);
  if (fd_ >= 0) {
    shutdown(fd_, SHUT_RDWR);  // despierta el recv bloqueado del hilo lector
    ::close(fd_);
    fd_ = -1;
  }
}

void ControlClient::read_loop() {
  std::string buf;
  char chunk[1024];
  for (;;) {
    {
      std::lock_guard<std::mutex> g(mu_);
      if (fd_ < 0) return;
    }
    ssize_t n = recv(fd_, chunk, sizeof chunk, 0);
    if (n <= 0) {
      LINF("ctrl", "control cerrado por el receptor (errno=%d, n=%ld)", errno,
           static_cast<long>(n));
      if (on_closed_) on_closed_();
      return;
    }
    buf.append(chunk, static_cast<size_t>(n));
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, pos);
      buf.erase(0, pos + 1);
      if (line.empty()) continue;
      Json msg;
      if (json_decode(line, msg)) dispatch(msg);
    }
  }
}

void ControlClient::dispatch(const Json& msg) {
  auto it = msg.find("type");
  if (it == msg.end()) return;
  const std::string& t = it->second;
  if (t == "welcome") {
    Welcome w;
    auto get = [&](const char* k) -> std::string {
      auto f = msg.find(k);
      return f == msg.end() ? "" : f->second;
    };
    w.session = get("session");
    w.codec = get("codec");
    w.audio = get("audio");
    w.vport = std::stoi(get("vport"));
    w.vrtcp = std::stoi(get("vrtcp"));
    w.aport = std::stoi(get("aport"));
    w.artcp = std::stoi(get("artcp"));
    if (on_welcome_) on_welcome_(w);
  } else if (t == "request") {
    if (on_request_) on_request_();
  } else if (t == "denied") {
    if (on_denied_) on_denied_();
  } else if (t == "error") {
    if (on_error_) {
      auto f = msg.find("message");
      on_error_(f == msg.end() ? "error desconocido" : f->second);
    }
  }
}

}  // namespace linky
