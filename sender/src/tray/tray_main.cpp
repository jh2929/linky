// linky-tray — agente de sistema para compartir pantalla con el TV.
//
// Modos:
//   linky-tray --toggle        abre el menú desplegable (o lo cierra si ya
//                              está abierto) con los televisores disponibles.
//   linky-tray --waybar        imprime JSON de estado para un módulo waybar.
//   linky-tray --stop          detiene la transmisión en curso.
//   linky-tray (otro)          igual que --toggle (por defecto).
//
// El menú descubre receptores Linky por DNS-SD (_linky._tcp) y al elegir uno
// lanza `linky-stream` (headless, pantalla completa) como proceso propio.
// El estado de la sesión se guarda en /tmp/linky-tray.json para que waybar
// muestre el icono correspondiente.
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <gtk/gtk.h>

#include "common/log.h"
#include "common/util.h"
#include "discovery/discovery.h"

using namespace linky;

namespace {

constexpr const char* kStatePath = "/tmp/linky-tray.json";
constexpr const char* kLockPath = "/tmp/linky-tray-popup.lock";

// ---------- estado compartido (archivo JSON mínimo) ----------

struct State {
  bool active = false;
  std::string name;
  std::string ip;
  pid_t pid = 0;
  pid_t popup_pid = 0;
};

std::string state_json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

void state_write(const State& st) {
  char buf[1024];
  int n = snprintf(
      buf, sizeof buf,
      "{\"active\":%s,\"name\":\"%s\",\"ip\":\"%s\",\"pid\":%d,\"popup_pid\":%d}\n",
      st.active ? "true" : "false", state_json_escape(st.name).c_str(),
      state_json_escape(st.ip).c_str(), static_cast<int>(st.pid),
      static_cast<int>(st.popup_pid));
  FILE* f = fopen(kStatePath, "w");
  if (f) {
    fwrite(buf, 1, static_cast<size_t>(n), f);
    fclose(f);
  }
}

State state_read() {
  State st;
  FILE* f = fopen(kStatePath, "r");
  if (!f) return st;
  char line[1024];
  if (fgets(line, sizeof line, f)) {
    char name[256] = {0}, ip[64] = {0};
    int active = 0;
    if (sscanf(line, "{\"active\":%d,\"name\":\"%255[^\"]\",\"ip\":\"%63[^\"]\","
                     "\"pid\":%d,\"popup_pid\":%d}",
               &active, name, ip, &st.pid, &st.popup_pid) == 5) {
      st.active = active != 0;
      st.name = name;
      st.ip = ip;
    }
  }
  fclose(f);
  return st;
}

bool process_alive(pid_t pid) {
  if (pid <= 0) return false;
  return kill(pid, 0) == 0 || errno == EPERM;
}

void refresh_waybar() {
  // Waybar re-ejecuta los módulos con `signal: 12` (RTMIN+12).
  system("pkill -RTMIN+12 -x waybar 2>/dev/null");
}

void stream_stop() {
  State st = state_read();
  if (st.active && st.pid > 0) kill(st.pid, SIGTERM);
  st.active = false;
  st.pid = 0;
  st.name.clear();
  st.ip.clear();
  state_write(st);
  refresh_waybar();
}

void stream_start(const Device& dev) {
  int fps = 60;
  int bitrate = 12000;
  if (const char* e = getenv("LINKY_TRAY_FPS")) fps = atoi(e);
  if (const char* e = getenv("LINKY_TRAY_BITRATE")) bitrate = atoi(e);

  // Resolver el binario: LINKY_STREAM_BIN -> mismo directorio que el tray
  // (por si vive en ~/.local/bin fuera del PATH del sesión) -> PATH.
  std::string stream_bin;
  if (const char* b = getenv("LINKY_STREAM_BIN")) {
    stream_bin = b;
  } else {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
      exe[n] = '\0';
      std::string p(exe);
      std::string::size_type slash = p.find_last_of('/');
      stream_bin = (slash == std::string::npos) ? "linky-stream"
                                                : p.substr(0, slash + 1) + "linky-stream";
    }
  }
  if (stream_bin.empty()) stream_bin = "linky-stream";

  // Los std::string deben vivir hasta que g_spawn_async copia argv.
  std::string fps_s = std::to_string(fps);
  std::string bit_s = std::to_string(bitrate);
  gchar* argv[] = {const_cast<gchar*>(stream_bin.c_str()),
                   const_cast<gchar*>("--connect"),
                   const_cast<gchar*>(dev.host.c_str()),
                   const_cast<gchar*>("--fps"),
                   const_cast<gchar*>(fps_s.c_str()),
                   const_cast<gchar*>("--bitrate"),
                   const_cast<gchar*>(bit_s.c_str()),
                   nullptr};

  gchar** envp = g_get_environ();
  GPid child = 0;
  gboolean ok = g_spawn_async(
      nullptr, argv, envp,
      static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                               G_SPAWN_STDOUT_TO_DEV_NULL |
                               G_SPAWN_STDERR_TO_DEV_NULL),
      nullptr, nullptr, &child, nullptr);
  g_strfreev(envp);
  if (!ok) {
    LERR("tray", "no se pudo lanzar %s", stream_bin.c_str());
    return;
  }
  g_child_watch_add(child, +[](GPid pid, int /*status*/, gpointer) {
    g_spawn_close_pid(pid);
  }, nullptr);

  State st;
  st.active = true;
  st.name = dev.name;
  st.ip = dev.host;
  st.pid = static_cast<pid_t>(child);
  st.popup_pid = getpid();
  state_write(st);
  refresh_waybar();
  LINF("tray", "transmitiendo a %s (%s) pid=%d", dev.name.c_str(),
       dev.host.c_str(), static_cast<int>(child));

  // Si el stream muere a los pocos segundos (captura/control falla) se
  // deshace el estado para que waybar vuelva a "idle".
  g_timeout_add(2000, +[](gpointer data) -> gboolean {
    pid_t pid = static_cast<pid_t>(*static_cast<pid_t*>(data));
    delete static_cast<pid_t*>(data);
    State st = state_read();
    if (st.active && st.pid == pid && !process_alive(pid)) {
      LERR("tray", "linky-stream (%d) murió al arrancar; reseteando estado", pid);
      st.active = false;
      st.pid = 0;
      st.name.clear();
      st.ip.clear();
      state_write(st);
      refresh_waybar();
    }
    return G_SOURCE_REMOVE;
  }, new pid_t(static_cast<pid_t>(child)));
}

// ---------- modo --waybar ----------

int waybar_mode() {
  State st = state_read();
  if (st.active && !process_alive(st.pid)) {
    // El stream murió sin actualizar el estado: sanear y mostrar idle.
    st.active = false;
    st.pid = 0;
    st.name.clear();
    st.ip.clear();
    state_write(st);
  }
  const char* icon = "\xEF\x87\x90";  // f047 TV (Font Awesome / Nerd)
  if (st.active) {
    printf("{\"text\":\"%s \",\"class\":\"streaming\","
           "\"tooltip\":\"Linky \\u2192 %s (%s) \\u2014 clic para detener\"}\n",
           icon, st.name.c_str(), st.ip.c_str());
  } else {
    printf("{\"text\":\"%s\",\"class\":\"idle\","
           "\"tooltip\":\"Linky \\u2014 compartir pantalla con el TV\"}\n",
           icon);
  }
  return 0;
}

// ---------- modo --toggle / menú GTK ----------

struct Popup {
  GtkWidget* window = nullptr;
  GtkWidget* list = nullptr;
  GtkWidget* empty = nullptr;
  GtkWidget* active_box = nullptr;
  GtkWidget* active_label = nullptr;
  GtkWidget* spin = nullptr;
  Discovery disc;
  std::vector<Device> last;
};

Popup* g_popup = nullptr;

static void popup_update_active(Popup* p) {
  State st = state_read();
  bool running = st.active && process_alive(st.pid);
  gtk_widget_set_visible(p->active_box, running);
  if (running) {
    char label[256];
    snprintf(label, sizeof label, "● Transmitiendo a %s (%s)", st.name.c_str(),
             st.ip.c_str());
    gtk_label_set_text(GTK_LABEL(p->active_label), label);
  }
}

static void popup_close(Popup* p);

static gboolean popup_on_device_cb(void* data) {
  // Se invoca desde el hilo de GTK (g_timeout_add). Refresca la lista con la
  // foto actual de discovery.devices() (copia segura).
  Popup* p = static_cast<Popup*>(data);
  if (!p->window) return G_SOURCE_REMOVE;
  std::vector<Device> devs = p->disc.devices();
  if (devs == p->last) return G_SOURCE_CONTINUE;
  p->last = devs;

  gtk_widget_set_visible(p->spin, false);
  gtk_widget_set_visible(p->empty, devs.empty());

  GtkListBox* list = GTK_LIST_BOX(p->list);
  gtk_list_box_remove_all(list);
  for (const Device& d : devs) {
    GtkWidget* row = gtk_list_box_row_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* title = gtk_label_new(d.name.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_add_css_class(title, "tv-name");
    char sub[256];
    snprintf(sub, sizeof sub, "%s  ·  %s%s%s", d.host.c_str(),
             d.codecs.empty() ? "?" : d.codecs.c_str(),
             d.audio.empty() ? "" : "  ·  ", d.audio.empty() ? "" : d.audio.c_str());
    GtkWidget* sublabel = gtk_label_new(sub);
    gtk_label_set_xalign(GTK_LABEL(sublabel), 0.0);
    gtk_widget_add_css_class(sublabel, "tv-sub");
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), sublabel);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), true);
    auto* dev_copy = new Device(d);
    g_object_set_data_full(G_OBJECT(row), "device", dev_copy,
                           +[](gpointer data) { delete static_cast<Device*>(data); });
    gtk_list_box_append(list, row);

    // El clic en la fila lanza el stream (GTK_SELECTION_NONE no emite
    // "activate" de forma fiable, así que se usa un gesture explícito).
    GtkGesture* tap = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(tap), GDK_BUTTON_PRIMARY);
    g_signal_connect(tap, "released", G_CALLBACK(+[](GtkGestureClick* g, int, double,
                                                     double, gpointer data) {
      Popup* pp = static_cast<Popup*>(data);
      GtkWidget* row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
      auto* d = static_cast<Device*>(g_object_get_data(G_OBJECT(row), "device"));
      if (!d) return;
      LINF("tray", "clic en %s (%s)", d->name.c_str(), d->host.c_str());
      stream_start(*d);
      popup_close(pp);
    }), p);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(tap));
    g_signal_connect(row, "activate", G_CALLBACK(+[](GtkListBoxRow* row,
                                                     gpointer data) {
      Popup* pp = static_cast<Popup*>(data);
      auto* d = static_cast<Device*>(g_object_get_data(G_OBJECT(row), "device"));
      if (!d) return;
      stream_start(*d);
      popup_close(pp);
    }), p);
  }
  popup_update_active(p);
  return G_SOURCE_CONTINUE;
}

static gboolean popup_tick(gpointer data) {
  Popup* p = static_cast<Popup*>(data);
  if (!p->window) return G_SOURCE_REMOVE;
  popup_update_active(p);
  return G_SOURCE_CONTINUE;
}

static void popup_close(Popup* p) {
  if (p->window) {
    gtk_window_destroy(GTK_WINDOW(p->window));
    p->window = nullptr;
  }
}

static gboolean on_key_pressed(GtkEventController*, guint keyval, guint,
                               GdkModifierType, gpointer data) {
  if (keyval == GDK_KEY_Escape) {
    popup_close(static_cast<Popup*>(data));
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static gboolean close_via_idle(gpointer data) {
  popup_close(static_cast<Popup*>(data));
  return G_SOURCE_REMOVE;
}

static void on_sigterm(int) {
  // Cerrar la ventana desde el hilo principal de GTK (safe).
  if (g_popup) g_idle_add(close_via_idle, g_popup);
}

// Estética Omarchy (tema Vantablack): negro, grises, acento monocromo.
static void popup_apply_css() {
  static const char* css =
      "window.linky-popup {"
      "  background-color: #0a0a0a;"
      "  border: 1px solid #262626;"
      "  border-radius: 14px;"
      "}"
      ".linky-head { padding: 14px 16px 10px 16px; }"
      ".linky-icon { color: #8d8d8d; }"
      ".linky-head label { color: #ffffff; font-size: 13px; font-weight: 600; }"
      ".linky-popup list { background-color: #0a0a0a; }"
      ".linky-popup row { border-radius: 8px; }"
      ".linky-popup row:hover { background-color: #141414; }"
      ".linky-popup row:focus { outline-width: 1px; outline-color: #8d8d8d; }"
      ".tv-name { color: #ffffff; font-size: 14px; }"
      ".tv-sub { color: #8d8d8d; font-size: 11px; }"
      ".linky-empty { color: #8d8d8d; font-size: 12px; }"
      ".linky-active {"
      "  background-color: #101010;"
      "  border-top: 1px solid #262626;"
      "  padding: 12px 16px;"
      "}"
      ".linky-active label { color: #ffffff; font-size: 12px; }"
      ".linky-active button {"
      "  background-color: #1a1a1a;"
      "  color: #ffffff;"
      "  border: 1px solid #3a3a3a;"
      "  border-radius: 8px;"
      "  padding: 4px 12px;"
      "  font-size: 12px;"
      "}"
      ".linky-active button:hover { background-color: #242424; }"
      "scrollbar { background-color: transparent; }"
      "scrollbar slider { background-color: #2a2a2a; border-radius: 4px; }"
      "spinner { color: #8d8d8d; }";

  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

int popup_run() {
  Popup p;
  g_popup = &p;

  // Lock: solo una instancia del menú a la vez.
  int lfd = open(kLockPath, O_CREAT | O_RDWR, 0600);
  if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB) != 0) {
    if (lfd >= 0) close(lfd);
    LINF("tray", "el menú ya está abierto");
    return 0;
  }
  signal(SIGTERM, on_sigterm);

  popup_apply_css();

  State st = state_read();
  st.popup_pid = getpid();
  state_write(st);

  p.window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(p.window), "linky-tray");
  gtk_window_set_default_size(GTK_WINDOW(p.window), 360, 420);
  gtk_window_set_decorated(GTK_WINDOW(p.window), false);
  gtk_widget_add_css_class(p.window, "linky-popup");

  GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(p.window), root);
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

  // Cabecera
  GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(head, "linky-head");
  GtkWidget* hicon = gtk_label_new("\xEF\x87\x90");
  gtk_widget_add_css_class(hicon, "linky-icon");
  GtkWidget* htitle = gtk_label_new("Linky · pantalla completa");
  gtk_label_set_xalign(GTK_LABEL(htitle), 0.0);
  gtk_widget_set_hexpand(htitle, true);
  p.spin = gtk_spinner_new();
  gtk_spinner_start(GTK_SPINNER(p.spin));
  gtk_box_append(GTK_BOX(head), hicon);
  gtk_box_append(GTK_BOX(head), htitle);
  gtk_box_append(GTK_BOX(head), p.spin);
  gtk_box_append(GTK_BOX(root), head);

  // Lista
  GtkWidget* scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  p.list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(p.list), GTK_SELECTION_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), p.list);
  gtk_widget_set_vexpand(scroller, true);
  gtk_box_append(GTK_BOX(root), scroller);

  p.empty = gtk_label_new("Ningún TV encontrado.\nEnciende la app Linky en el TV.");
  gtk_label_set_wrap(GTK_LABEL(p.empty), true);
  gtk_widget_set_margin_top(p.empty, 24);
  gtk_widget_set_margin_bottom(p.empty, 24);
  gtk_widget_add_css_class(p.empty, "linky-empty");
  gtk_box_append(GTK_BOX(root), p.empty);
  gtk_widget_set_visible(p.empty, false);

  // Bloque de sesión activa
  p.active_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(p.active_box, "linky-active");
  p.active_label = gtk_label_new("");
  gtk_widget_set_hexpand(p.active_label, true);
  GtkWidget* stop = gtk_button_new_with_label("Detener");
  gtk_widget_add_css_class(stop, "destructive-action");
  g_signal_connect(stop, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                     Popup* pp = static_cast<Popup*>(data);
                     stream_stop();
                     popup_close(pp);
                   }),
                   &p);
  gtk_box_append(GTK_BOX(p.active_box), p.active_label);
  gtk_box_append(GTK_BOX(p.active_box), stop);
  gtk_box_append(GTK_BOX(root), p.active_box);
  gtk_widget_set_visible(p.active_box, false);

  // Esc cierra
  GtkEventController* key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), &p);
  gtk_widget_add_controller(p.window, key);

  g_signal_connect(p.window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
                     g_main_loop_quit(static_cast<GMainLoop*>(data));
                   }), loop);

  // La posición del menú la fija el compositor: una regla de Hyprland coloca
  // la ventana (class "linky-tray") abajo de la barra superior.
  gtk_widget_set_halign(p.window, GTK_ALIGN_FILL);

  // Descubrimiento DNS-SD: avahi navega en su hilo; aquí solo se lee la
  // foto actual cada segundo (copy bajo el lock del hilo de Avahi).
  p.disc.start(nullptr);
  g_timeout_add(700, popup_on_device_cb, &p);
  g_timeout_add(1000, popup_tick, &p);

  LINF("tray", "menú abierto");
  gtk_widget_show(p.window);
  g_main_loop_run(loop);

  p.disc.stop();
  st = state_read();
  st.popup_pid = 0;
  state_write(st);
  g_popup = nullptr;
  close(lfd);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Log de diagnóstico (los lanzadores waybar no capturan stderr).
  const char* logpath = getenv("LINKY_TRAY_LOG");
  if (logpath) {
    int fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      close(fd);
    }
  }

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--waybar") return waybar_mode();
    if (a == "--stop") {
      stream_stop();
      return 0;
    }
    if (a == "--toggle") break;
  }

  // ID de app: define la clase de ventana en Wayland ("linky-tray") para que
  // las reglas de Hyprland (flotante + posición) hagan match.
  g_set_prgname("linky-tray");
  gtk_init();
  return popup_run();
}