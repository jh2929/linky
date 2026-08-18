// Captura de vídeo por portal ScreenCast (org.freedesktop.portal.ScreenCast
// vía D-Bus) + flujo PipeWire. Única ruta válida en Wayland.
// Flujo: CreateSession → SelectSources(monitor) → Start → node_id PipeWire
//        → pw_stream con formato BGR0 → callback con los pixels.
//
// El portal responde de forma asíncrona con la señal "Response" sobre el
// objeto request; cada petición lleva un handle_token que se correlaciona
// aquí para restaurar el flujo de la máquina de estados.
#include "capture/video.h"

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

#include "common/log.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace linky {

namespace {

constexpr const char* kService = "org.freedesktop.portal.Desktop";
constexpr const char* kObject = "/org/freedesktop/portal/desktop";
constexpr const char* kIface = "org.freedesktop.portal.ScreenCast";
constexpr const char* kRequestIface = "org.freedesktop.portal.Request";
constexpr const char* kRequestPath = "/org/freedesktop/portal/desktop/request";

struct PwStream {
  pw_thread_loop* loop = nullptr;
  pw_context* ctx = nullptr;
  pw_core* core = nullptr;
  pw_stream* stream = nullptr;
  spa_hook listener{};
  std::atomic<bool> running{false};
  uint32_t node_id = 0;
  uint32_t width = 0, height = 0, stride = 0;
  bool got_format = false;
  enum AVPixelFormat fmt = AV_PIX_FMT_BGR0;
  AVFrame* frame = nullptr;
  std::function<void(AVFrame*)> on_frame;
};

// Llamada desde el hilo de pw (proceso de buffers): copia el plano y
// entrega el marco al consumidor (encoder) en el hilo del emisor.
static void on_process(void* userdata) {
  auto* s = static_cast<PwStream*>(userdata);
  if (!s->running || !s->got_format) return;
  pw_buffer* b = pw_stream_dequeue_buffer(s->stream);
  if (!b) return;
  spa_buffer* buf = b->buffer;
  if (buf && buf->n_datas > 0) {
    spa_data& d = buf->datas[0];
    if (d.chunk && d.chunk->size > 0 && d.data) {
      AVFrame* f = s->frame;
      av_frame_unref(f);
      f->format = s->fmt;
      f->width = static_cast<int>(s->width);
      f->height = static_cast<int>(s->height);
      if (f->linesize[0] != static_cast<int>(s->stride)) {
        av_frame_unref(f);
        f->format = s->fmt;
        f->width = static_cast<int>(s->width);
        f->height = static_cast<int>(s->height);
        f->linesize[0] = static_cast<int>(s->stride);
        if (av_frame_get_buffer(f, 1) < 0) {
          pw_stream_queue_buffer(s->stream, b);
          return;
        }
      }
      av_frame_unref(f);
      f->format = s->fmt;
      f->width = static_cast<int>(s->width);
      f->height = static_cast<int>(s->height);
      f->linesize[0] = static_cast<int>(s->stride);
      if (d.chunk->size <= static_cast<size_t>(s->stride) * s->height)
        memcpy(f->data[0], d.data, d.chunk->size);
      if (s->on_frame) s->on_frame(f);
    }
  }
  pw_stream_queue_buffer(s->stream, b);
}

static void on_param_changed(void* userdata, uint32_t id, const spa_pod* param) {
  auto* s = static_cast<PwStream*>(userdata);
  if (id != SPA_PARAM_Format || !param) return;
  spa_video_info info{};
  if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
  if (info.media_type != SPA_MEDIA_TYPE_video ||
      info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
    return;
  if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;

  switch (info.info.raw.format) {
    case SPA_VIDEO_FORMAT_BGRx:
      s->fmt = AV_PIX_FMT_BGR0;
      break;
    case SPA_VIDEO_FORMAT_RGBA:
      s->fmt = AV_PIX_FMT_RGBA;
      break;
    default:
      LWRN("capv", "formato de video no soportado");
      return;
  }
  s->width = info.info.raw.size.width;
  s->height = info.info.raw.size.height;
  s->stride = s->width * 4;  // BGRx/RGBA: 4 bytes por píxel
  s->got_format = true;
  LINF("capv", "portal: %ux%u stride=%u fmt=%s", s->width, s->height, s->stride,
       av_get_pix_fmt_name(s->fmt));
}

static const pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process = on_process,
};

}  // namespace

class PortalVideoCapture : public VideoCapture {
 public:
  ~PortalVideoCapture() override { stop(); }

  bool start(const VideoSpec& spec,
             std::function<void(AVFrame*)> on_frame) override {
    spec_ = spec;
    on_frame_ = std::move(on_frame);
    if (!dbus_connect()) return false;
    thread_ = std::thread([this] { run_loop(); });
    // El portal responde con señales punto a punto; hay que iniciar el
    // flujo tras arrancar el hilo que itera el contexto de la conexión.
    create_session();
    return true;
  }

  void stop() override {
    if (!thread_.joinable()) return;
    g_main_loop_quit(loop_);
    thread_.join();
    teardown_pw();
    if (sub_id_) {
      g_dbus_connection_signal_unsubscribe(conn_, sub_id_);
      sub_id_ = 0;
    }
    g_object_unref(conn_);
    g_main_loop_unref(loop_);
    conn_ = nullptr;
    loop_ = nullptr;
  }

  bool ready() const override { return pw_.got_format; }
  const char* backend_name() const override { return "wayland-portal"; }

 private:
  VideoSpec spec_;
  std::function<void(AVFrame*)> on_frame_;
  GDBusConnection* conn_ = nullptr;
  GMainLoop* loop_ = nullptr;
  guint sub_id_ = 0;
  PwStream pw_;
  std::thread thread_;
  std::string session_handle_;

  // ── D-Bus ───────────────────────────────────────────────────────────────
  bool dbus_connect() {
    const char* addr = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    std::string fallback;
    if (!addr || !*addr) {
      const char* rt = g_getenv("XDG_RUNTIME_DIR");
      fallback = std::string(rt ? rt : "/run/user/1000") + "/bus";
      addr = fallback.c_str();
    }
    GError* err = nullptr;
    conn_ = g_dbus_connection_new_for_address_sync(
        addr, static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &err);
    if (!conn_) {
      LERR("capv", "d-bus: %s", err ? err->message : "sin dirección de bus");
      return false;
    }
    // Las señal callbacks se despachan en el contexto por defecto del hilo
    // que creó la conexión; run_loop() itera ESE contexto desde su hilo.
    loop_ = g_main_loop_new(g_main_context_default(), FALSE);

    // Todas las respuestas del portal llegan como señal "Response" en el
    // objeto request; el handle_token distingue la petición.
    sub_id_ = g_dbus_connection_signal_subscribe(
        conn_, nullptr, kRequestIface, "Response", kRequestPath, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, on_response_signal, this, nullptr);
    return sub_id_ != 0;
  }

  static void on_response_signal(GDBusConnection*, const gchar*, const gchar*,
                                 const gchar*, const gchar*, GVariant* params,
                                 gpointer userdata) {
    auto* self = static_cast<PortalVideoCapture*>(userdata);
    guint32 status = 0;
    GVariant* dict_v = nullptr;
    g_variant_get(params, "(u@a{sv})", &status, &dict_v);
    GVariantDict dict;
    g_variant_dict_init(&dict, dict_v);
    g_variant_unref(dict_v);
    const char* token = nullptr;
    g_variant_dict_lookup(&dict, "handle_token", "s", &token);
    LINF("capv", "portal respuesta '%s': status=%u", token ? token : "?", status);
    if (!token || status != 0) return;
    std::string t = token;
    if (t == "linky-create") {
      const char* h = nullptr;
      if (g_variant_dict_lookup(&dict, "session_handle", "s", &h)) {
        self->session_handle_ = h;
        self->select_sources();
      }
    } else if (t == "linky-select") {
      self->start_portal();
    } else if (t == "linky-start") {
      GVariant* streams = nullptr;
      if (g_variant_dict_lookup(&dict, "streams", "@a(ua{sv})", &streams) && streams) {
        GVariantIter iter;
        g_variant_iter_init(&iter, streams);
        if (g_variant_iter_n_children(&iter) > 0) {
          GVariant* tuple = g_variant_iter_next_value(&iter);
          guint node = 0;
          g_variant_get(tuple, "(u@a{sv})", &node, nullptr);
          g_variant_unref(tuple);
          g_variant_unref(streams);
          self->pw_.node_id = node;
          LINF("capv", "portal: nodo PipeWire %u", node);
          self->connect_pw();
        }
      }
    }
  }

  void send_request(const char* method, GVariant* args) {
    GError* err = nullptr;
    GVariant* out = g_dbus_connection_call_sync(conn_, kService, kObject, kIface,
                                                method, args, nullptr,
                                                G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &err);
    if (out) g_variant_unref(out);
    if (err) {
      LERR("capv", "%s: %s", method, err->message);
      g_error_free(err);
    }
  }

  void create_session() {
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "handle_token",
                          g_variant_new_string("linky-create"));
    g_variant_builder_add(&opts, "{sv}", "session_handle_token",
                          g_variant_new_string("linky-session"));
    GVariant* options = g_variant_builder_end(&opts);
    send_request("CreateSession", g_variant_new("(@a{sv})", options));
  }

  void select_sources() {
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "handle_token",
                          g_variant_new_string("linky-select"));
    g_variant_builder_add(&opts, "{sv}", "types", g_variant_new_uint32(1));
    g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts, "{sv}", "cursor_mode", g_variant_new_uint32(2));
    GVariant* options = g_variant_builder_end(&opts);
    send_request("SelectSources", g_variant_new("(@s@a{sv})", session_handle_.c_str(), options));
  }

  void start_portal() {
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "handle_token",
                          g_variant_new_string("linky-start"));
    g_variant_builder_add(&opts, "{sv}", "parent_window", g_variant_new_string(""));
    GVariant* options = g_variant_builder_end(&opts);
    send_request("Start", g_variant_new("(@s@a{sv})", session_handle_.c_str(), options));
  }

  // ── PipeWire ────────────────────────────────────────────────────────────
  void connect_pw() {
    pw_.loop = pw_thread_loop_new("linky-cap", nullptr);
    pw_.ctx = pw_context_new(pw_thread_loop_get_loop(pw_.loop), nullptr, 0);
    pw_.core = pw_context_connect(pw_.ctx, nullptr, 0);
    if (!pw_.core) {
      LERR("capv", "pw_context_connect falló");
      return;
    }
    pw_.stream = pw_stream_new(
        pw_.core, "linky-capture",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                          "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (!pw_.stream) {
      LERR("capv", "pw_stream_new falló");
      return;
    }
    pw_stream_add_listener(pw_.stream, &pw_.listener, &kStreamEvents, &pw_);
    pw_.frame = av_frame_alloc();
    pw_.on_frame = on_frame_;
    pw_.running = true;

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const spa_pod* params[1];
    params[0] = reinterpret_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_Rectangle(SPA_RECTANGLE(static_cast<uint32_t>(spec_.width),
                                        static_cast<uint32_t>(spec_.height))),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_Fraction(SPA_FRACTION(static_cast<uint32_t>(spec_.fps ? spec_.fps : 30), 1))));

    pw_thread_loop_lock(pw_.loop);
    int r = pw_stream_connect(pw_.stream, PW_DIRECTION_INPUT, pw_.node_id,
                              static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                           PW_STREAM_FLAG_MAP_BUFFERS),
                              params, 1);
    if (r < 0) LERR("capv", "pw_stream_connect: %s", spa_strerror(r));
    pw_thread_loop_start(pw_.loop);
    pw_thread_loop_unlock(pw_.loop);
  }

  void teardown_pw() {
    if (pw_.loop) pw_thread_loop_stop(pw_.loop);
    if (pw_.stream) {
      pw_stream_disconnect(pw_.stream);
      pw_stream_destroy(pw_.stream);
    }
    if (pw_.core) pw_core_disconnect(pw_.core);
    if (pw_.ctx) pw_context_destroy(pw_.ctx);
    if (pw_.loop) pw_thread_loop_destroy(pw_.loop);
    if (pw_.frame) av_frame_free(&pw_.frame);
    pw_.stream = nullptr;
    pw_.core = nullptr;
    pw_.ctx = nullptr;
    pw_.loop = nullptr;
    pw_.frame = nullptr;
    pw_.on_frame = nullptr;
    pw_.running = false;
    pw_.got_format = false;
    pw_.node_id = 0;
  }

  void run_loop() { g_main_loop_run(loop_); }
};

// ── Factory ───────────────────────────────────────────────────────────────
// La decisión Wayland/X11 está centralizada en video_x11.cpp (un solo
// punto de elección); aquí solo se expone la implementación del portal.
VideoCapture* create_portal_capture() { return new PortalVideoCapture; }

}  // namespace linky
