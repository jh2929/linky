#pragma once
// Serialización/deserialización JSON mínima para el canal de control.
// Solo se necesitan mapas planos de strings/enteros/bool (protocolo v1).
#include <map>
#include <string>
#include <vector>

namespace linky {

using Json = std::map<std::string, std::string>;

// Codifica un mapa plano a JSON estricto.
std::string json_encode(const Json& m);

// Decodifica un objeto JSON plano. Devuelve false si el formato es inválido
// (bom de robustez: el emisor ignora mensajes malformados).
bool json_decode(const std::string& s, Json& out);

}  // namespace linky
