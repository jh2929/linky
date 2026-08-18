#include "discovery/discovery.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/thread-watch.h>
#include <avahi-common/timeval.h>

#include <algorithm>
#include <cstring>

#include "common/log.h"

namespace linky {

struct Discovery::Impl {
  AvahiThreadedPoll* poll = nullptr;
  AvahiClient* client = nullptr;
  AvahiServiceBrowser* browser = nullptr;
  Callback cb;
  std::vector<Device> devices;
};

std::vector<Device> Discovery::devices() const {
  if (!impl_) return {};
  return impl_->devices;
}

static void resolve_cb(AvahiServiceResolver* r, AvahiIfIndex, AvahiProtocol,
                       AvahiResolverEvent event, const char* name, const char*,
                       const char*, const char* host, const AvahiAddress* addr,
                       uint16_t port, AvahiStringList* txt, AvahiLookupResultFlags,
                       void* userdata) {
  auto* d = static_cast<Discovery::Impl*>(userdata);
  if (event == AVAHI_RESOLVER_FAILURE) return;

  Device dev;
  dev.name = name;
  dev.port = port;
  char ip[AVAHI_ADDRESS_STR_MAX];
  avahi_address_snprint(ip, sizeof ip, addr);
  dev.host = ip;
  for (AvahiStringList* p = txt; p; p = avahi_string_list_get_next(p)) {
    char* key = nullptr;
    char* value = nullptr;
    size_t size = 0;
    if (avahi_string_list_get_pair(p, &key, &value, &size) < 0) continue;
    std::string k = key ? key : "";
    std::string v = value ? std::string(value, size) : "";
    free(key);
    free(value);
    if (k == "model") dev.model = v;
    else if (k == "codecs") dev.codecs = v;
    else if (k == "audio") dev.audio = v;
    else if (k == "apiver") dev.apiver = v;
  }

  bool exists = false;
  for (auto& e : d->devices) {
    if (e.host == dev.host && e.port == dev.port) {
      e = dev;
      exists = true;
      break;
    }
  }
  if (!exists) d->devices.push_back(dev);
  if (d->cb) d->cb(d->devices);

  avahi_service_resolver_free(r);
}

static void browser_cb(AvahiServiceBrowser*, AvahiIfIndex, AvahiProtocol,
                       AvahiBrowserEvent event, const char* name, const char* type,
                       const char* domain, AvahiLookupResultFlags, void* userdata) {
  auto* d = static_cast<Discovery::Impl*>(userdata);
  switch (event) {
    case AVAHI_BROWSER_NEW: {
      AvahiServiceResolver* r = avahi_service_resolver_new(
          d->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, name, type, domain,
          AVAHI_PROTO_UNSPEC, AVAHI_LOOKUP_USE_MULTICAST, resolve_cb, d);
      break;
    }
    case AVAHI_BROWSER_REMOVE: {
      d->devices.erase(
          std::remove_if(d->devices.begin(), d->devices.end(),
                         [&](const Device& dev) { return dev.name == name; }),
          d->devices.end());
      if (d->cb) d->cb(d->devices);
      break;
    }
    case AVAHI_BROWSER_FAILURE:
      LERR("disc", "avahi browse failure: %s",
           avahi_strerror(avahi_client_errno(d->client)));
      break;
    default:
      break;
  }
}

static void client_cb(AvahiClient* c, AvahiClientState state, void* userdata) {
  auto* d = static_cast<Discovery::Impl*>(userdata);
  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      if (!d->browser) {
        d->browser = avahi_service_browser_new(c, AVAHI_IF_UNSPEC,
                                               AVAHI_PROTO_UNSPEC, "_linky._tcp",
                                               "local", AVAHI_LOOKUP_USE_MULTICAST,
                                               browser_cb, d);
        if (!d->browser)
          LERR("disc", "no se pudo crear el browser: %s", avahi_strerror(avahi_client_errno(c)));
      }
      break;
    case AVAHI_CLIENT_FAILURE:
      if (d->poll) avahi_threaded_poll_stop(d->poll);
      break;
    default:
      break;
  }
}

bool Discovery::start(Callback cb) {
  stop();
  impl_ = new Impl;
  impl_->cb = std::move(cb);
  impl_->poll = avahi_threaded_poll_new();
  if (!impl_->poll) {
    LERR("disc", "avahi_threaded_poll_new falló");
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  int error = 0;
  impl_->client = avahi_client_new(avahi_threaded_poll_get(impl_->poll),
                                   AVAHI_CLIENT_NO_FAIL, client_cb, impl_, &error);
  if (!impl_->client) {
    LERR("disc", "avahi_client_new falló: %s", avahi_strerror(error));
    avahi_threaded_poll_free(impl_->poll);
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  avahi_threaded_poll_start(impl_->poll);
  return true;
}

void Discovery::stop() {
  if (!impl_) return;
  if (impl_->poll) avahi_threaded_poll_stop(impl_->poll);
  if (impl_->browser) avahi_service_browser_free(impl_->browser);
  if (impl_->client) avahi_client_free(impl_->client);
  if (impl_->poll) avahi_threaded_poll_free(impl_->poll);
  delete impl_;
  impl_ = nullptr;
}

}  // namespace linky
