#include "common/json.h"

#include <cctype>
#include <cstdio>

namespace linky {

std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string json_encode(const Json& m) {
  std::string out = "{";
  bool first = true;
  for (const auto& [k, v] : m) {
    if (!first) out += ",";
    first = false;
    out += '"' + json_escape(k) + "\":\"" + json_escape(v) + '"';
  }
  out += "}";
  return out;
}

namespace {
void skip_ws(const char*& p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
}

bool parse_string(const char*& p, std::string& out) {
  if (*p != '"') return false;
  ++p;
  out.clear();
  while (*p && *p != '"') {
    if (*p == '\\') {
      ++p;
      switch (*p) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            ++p;
            code <<= 4;
            if (*p >= '0' && *p <= '9') code |= *p - '0';
            else if (*p >= 'a' && *p <= 'f') code |= *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') code |= *p - 'A' + 10;
            else return false;
          }
          out += static_cast<char>(code);
          break;
        }
        default: return false;
      }
      ++p;
    } else {
      out += *p++;
    }
  }
  return *p == '"' && (++p, true);
}
}  // namespace

bool json_decode(const std::string& s, Json& out) {
  const char* p = s.c_str();
  skip_ws(p);
  if (*p != '{') return false;
  ++p;
  out.clear();
  for (;;) {
    skip_ws(p);
    if (*p == '}') { ++p; return true; }
    std::string key, val;
    if (!parse_string(p, key)) return false;
    skip_ws(p);
    if (*p != ':') return false;
    ++p;
    skip_ws(p);
    if (*p == '"') {
      if (!parse_string(p, val)) return false;
    } else {
      const char* start = p;
      while (*p && *p != ',' && *p != '}') ++p;
      val.assign(start, p);
    }
    out[key] = val;
    skip_ws(p);
    if (*p == ',') { ++p; continue; }
    if (*p == '}') { ++p; return true; }
    return false;
  }
}

}  // namespace linky
