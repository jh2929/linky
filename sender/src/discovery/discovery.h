#pragma once
// Descubrimiento DNS-SD: navega el servicio "_linky._tcp" anunciado por los
// receptores. API asíncrona: el callback se invoca desde el hilo de Avahi.
#include <functional>
#include <string>
#include <vector>

namespace linky {

struct Device {
  std::string name;
  std::string host;      // dirección IP
  int port = 0;          // puerto de control TCP
  std::string model;     // TXT: "model"
  std::string codecs;    // TXT: "codecs" (lista separada por comas)
  std::string audio;     // TXT: "audio"
  std::string apiver;    // TXT: "apiver"

  bool operator==(const Device& o) const {
    return name == o.name && host == o.host && port == o.port &&
           model == o.model && codecs == o.codecs && audio == o.audio &&
           apiver == o.apiver;
  }
};

class Discovery {
 public:
  using Callback = std::function<void(const std::vector<Device>&)>;

  // Inicia la navegación. `cb` recibe la lista completa en cada cambio.
  // Devuelve false si avahi-client no está disponible.
  bool start(Callback cb);
  void stop();

  // Lista actual (copia tomada bajo el lock del hilo de Avahi).
  std::vector<Device> devices() const;

  // Opaco: definido en discovery.cpp. Público porque los callbacks de Avahi
  // (funciones libres) necesitan nombrar el tipo.
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace linky
