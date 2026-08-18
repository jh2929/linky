#pragma once
// Utilidades: identidad del emisor, hashing y máscara de UUID de sesión.
#include <cstdint>
#include <string>

namespace linky {

// id estable del emisor: SHA-256(hostname|mac) hex. Se usa para autorización.
std::string sender_id();

// SHA-256 hex de una cadena.
std::string sha256_hex(const std::string& data);

// UUID v4 aleatorio (sesión).
std::string uuid4();

// Nombre de host de la máquina.
std::string hostname();

}  // namespace linky
