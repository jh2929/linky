#include "common/util.h"

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstdio>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <unistd.h>

namespace linky {

std::string hostname() {
  char buf[256] = {0};
  if (gethostname(buf, sizeof buf - 1) != 0) return std::string("linux");
  return buf;
}

std::string sha256_hex(const std::string& data) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash.data());
  char out[SHA256_DIGEST_LENGTH * 2 + 1];
  for (size_t i = 0; i < hash.size(); ++i) snprintf(out + i * 2, 3, "%02x", hash[i]);
  return out;
}

// MAC del primer adaptador Ethernet/Wi-Fi que no sea loopback.
static std::string primary_mac() {
  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) != 0) return "";
  std::string mac;
  for (ifaddrs* p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_PACKET) continue;
    if ((p->ifa_flags & IFF_LOOPBACK) || (p->ifa_flags & IFF_UP) == 0) continue;
    auto* sa = reinterpret_cast<sockaddr_ll*>(p->ifa_addr);
    char buf[64] = {0};
    for (int i = 0; i < sa->sll_halen && i < 6; ++i) {
      snprintf(buf + i * 2, 3, "%02x", sa->sll_addr[i]);
    }
    mac = buf;
    break;
  }
  freeifaddrs(ifa);
  return mac;
}

std::string sender_id() { return sha256_hex(hostname() + "|" + primary_mac()); }

std::string uuid4() {
  std::array<unsigned char, 16> b{};
  RAND_bytes(b.data(), static_cast<int>(b.size()));
  b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x40);
  b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80);
  char out[37];
  snprintf(out, sizeof out,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
           b[11], b[12], b[13], b[14], b[15]);
  return out;
}

}  // namespace linky
