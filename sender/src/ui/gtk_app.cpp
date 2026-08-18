// Interfaz GTK4 mínima del emisor: lista de receptores (DNS-SD), botón de
// transmisión, estado y estadísticas en vivo. Sin adornos: el foco está en
// el motor. El modo CLI cubre automatización (--connect).
#include <gtk/gtk.h>

#include <mutex>
#include <string>
#include <vector>

#include "app/sender_app.h"
#include "common/util.h"
#include "discovery/discovery.h"
#include "encode/encoder.h"

using namespace linky;

namespace {

struct Ui {
  GtkWidget* win = nullptr;
  GtkWidget* list = nullptr;
  GtkWidget* status = nullptr;
  GtkWidget* stats = nullptr;
  GtkWidget* connect_btn = nullptr;
  GtkListStore* store = nullptr;
  Discovery disc;
  SenderApp app;
  std::vector<Device> devices;
  std::mutex mu;
  bool busy = false;
  std::string last_err;
  SenderApp::Event appEvent;
  std::function<void(SenderApp::Event)> appEventDelegate;
};

constexpr int kColName = 0, kColModel = 1, kColHost = 2, kColPort = 3;

static void refresh_list(Ui* ui) {
  GtkTreeIter it;
  auto devs = ui->devices;
  gtk_list_store_clear(ui->store);
  for (const auto& d : devs) {
    gtk_list_store_append(ui->store, &it);
    gtk_list_store_set(ui->store, &it, kColName, d.name.c_str(), kColModel,
                       d.model.c_str(), kColHost, d.host.c_str(), kColPort, d.port,
                       -1);
  }
}

static gboolean on_disc_change(gpointer data) {
  auto* ui = static_cast<Ui*>(data);
  {
    std::lock_guard<std::mutex> g(ui->mu);
    ui->devices = ui->disc.devices();
  }
  refresh_list(ui);
  return G_SOURCE_REMOVE;
}

static gboolean on_event(gpointer data) {
  auto* ui = static_cast<Ui*>(data);
  SenderApp::Event ev;
  {
    std::lock_guard<std::mutex> g(ui->mu);
    ev = ui->appEvent;
  }
  const char* st = ev.status == SenderStatus::Connecting
                       ? "Conectando…"
                       : ev.status == SenderStatus::WaitingAcceptance
                             ? "Esperando aceptación en el TV…"
                             : ev.status == SenderStatus::Streaming
                                   ? "Transmitiendo…"
                                   : ev.status == SenderStatus::Failed
                                         ? "Error"
                                         : "Inactivo";
  gtk_label_set_text(GTK_LABEL(ui->status), st);
  if (ev.status == SenderStatus::Streaming) {
    char b[256];
    snprintf(b, sizeof b, "%.1f fps   %.0f kbps   frames=%d   drop=%d   nack=%d   pli=%d",
             ev.stats.fps, ev.stats.kbps, ev.stats.frames_encoded,
             ev.stats.frames_dropped, ev.stats.nacks, ev.stats.plis);
    gtk_label_set_text(GTK_LABEL(ui->stats), b);
    gtk_widget_set_sensitive(GTK_WIDGET(ui->connect_btn), FALSE);
  } else if (ev.status == SenderStatus::Failed || ev.status == SenderStatus::Idle) {
    gtk_widget_set_sensitive(GTK_WIDGET(ui->connect_btn), TRUE);
  }
  return G_SOURCE_REMOVE;
}

static void on_connect(GtkButton*, gpointer data) {
  auto* ui = static_cast<Ui*>(data);
  GtkTreeIter it;
  GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->list));
  if (!gtk_tree_selection_get_selected(sel, nullptr, &it)) {
    gtk_label_set_text(GTK_LABEL(ui->status), "Selecciona un receptor");
    return;
  }
  gchar *name = nullptr, *host = nullptr;
  gint port = 0;
  gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &it, kColName, &name, kColHost,
                     &host, kColPort, &port, -1);
  Device d;
  d.name = name ? name : "";
  d.host = host ? host : "";
  d.port = port;
  g_free(name);
  g_free(host);

  EncoderInfo info = probe_encoders();
  Json caps;
  std::string cs;
  for (Codec c : info.codecs) {
    if (!cs.empty()) cs += ",";
    cs += codec_name(c);
  }
  caps["codecs"] = cs;
  caps["audio"] = "opus";

  SenderConfig cfg;
  cfg.device_name = hostname();
  ui->app.set_target(d.host, d.port, d.name, caps);
  ui->appEventDelegate = [ui](SenderApp::Event e) {
    {
      std::lock_guard<std::mutex> g(ui->mu);
      ui->appEvent = e;
    }
    g_idle_add(on_event, ui);
  };
  ui->app.start(cfg, ui->appEventDelegate);
  if (ui->app.status() == SenderStatus::Failed)
    gtk_label_set_text(GTK_LABEL(ui->status), "No se pudo conectar");
}

}  // namespace

int gtk_run() {
  gtk_init();
  GMainLoop* main_loop = g_main_loop_new(nullptr, FALSE);
  auto* ui = new Ui;
  ui->win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(ui->win), "Linky Sender");
  gtk_window_set_default_size(GTK_WINDOW(ui->win), 560, 460);
  g_signal_connect(ui->win, "destroy", G_CALLBACK(g_main_loop_quit), main_loop);

  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_window_set_child(GTK_WINDOW(ui->win), box);

  GtkWidget* head = gtk_label_new("Receptores Linky en la red");
  gtk_widget_add_css_class(head, "title");
  gtk_box_append(GTK_BOX(box), head);

  // Lista de dispositivos
  ui->store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
  GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->store));
  for (int i = 0; i < 4; ++i) {
    GtkTreeViewColumn* col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col,
                                   i == 0 ? "Nombre" : i == 1 ? "Modelo" : i == 2 ? "IP" : "Puerto");
    GtkCellRenderer* ren = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, ren, TRUE);
    gtk_tree_view_column_add_attribute(col, ren, "text", i);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
  }
  gtk_widget_set_size_request(view, -1, 220);
  GtkWidget* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
  gtk_box_append(GTK_BOX(box), scroll);
  ui->list = view;

  ui->connect_btn = gtk_button_new_with_label("Transmitir pantalla");
  g_signal_connect(ui->connect_btn, "clicked", G_CALLBACK(on_connect), ui);
  gtk_box_append(GTK_BOX(box), ui->connect_btn);

  ui->status = gtk_label_new("Buscando receptores…");
  gtk_box_append(GTK_BOX(box), ui->status);
  ui->stats = gtk_label_new("");
  gtk_box_append(GTK_BOX(box), ui->stats);

  ui->disc.start([ui](const std::vector<Device>&) {
    g_idle_add(on_disc_change, ui);
  });

  gtk_window_present(GTK_WINDOW(ui->win));
  g_main_loop_run(main_loop);
  ui->app.stop();
  ui->disc.stop();
  g_main_loop_unref(main_loop);
  delete ui;
  return 0;
}