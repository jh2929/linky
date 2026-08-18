#pragma once
// Utilidades: identidad del emisor, hashing y máscara de UUID de sesión.
#include <cstdint>
#include <cstdio>
#include <string>

extern "C" {
#include <libavutil/error.h>
}

namespace linky {

// id estable del emisor: SHA-256(hostname|mac) hex. Se usa para autorización.
std::string sender_id();

// SHA-256 hex de una cadena.
std::string sha256_hex(const std::string& data);

// UUID v4 aleatorio (sesión).
std::string uuid4();

// Nombre de host de la máquina.
std::string hostname();

// Texto de un código de error FFmpeg (portable; av_err2str es compound literal
// y GCC >= 13 lo rechaza como error al tomar su dirección).
inline const char* fferr(int r, char* buf, size_t n) {
  av_strerror(r, buf, static_cast<unsigned>(n));
  return buf;
}

}  // namespace linky
