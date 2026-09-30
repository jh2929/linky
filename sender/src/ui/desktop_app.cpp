// Linky Desktop — Aplicación oficial para Linux con interfaz moderna Libadwaita (GTK4).
#include <adwaita.h>
#include <gtk/gtk.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/sender_app.h"
#include "common/log.h"
#include "common/util.h"
#include "discovery/discovery.h"
#include "encode/encoder.h"

using namespace linky;

namespace {

struct AppState {
  AdwApplicationWindow* win = nullptr;
  AdwPreferencesGroup* group_devices = nullptr;
  AdwPreferencesGroup* group_stats = nullptr;
  AdwActionRow* row_stats_info = nullptr;
  AdwActionRow* row_status_badge = nullptr;
  GtkWidget* btn_stop = nullptr;
  GtkWidget* btn_refresh = nullptr;

  AdwComboRow* combo_resolution = nullptr;
  AdwComboRow* combo_fps = nullptr;
  AdwSwitchRow* switch_audio = nullptr;
  GtkScale* scale_bitrate = nullptr;
  GtkLabel* label_bitrate_val = nullptr;

  Discovery disc;
  SenderApp sender;
  std::vector<Device> devices;
  std::mutex mu;
  std::string active_target_name;

  SenderApp::Event last_event;
};

static AppState* g_state = nullptr;

static void update_devices_ui(AppState* s) {
  if (!s || !s->group_devices) return;

  // Limpiar filas anteriores de dispositivos
  GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(s->group_devices));
  std::vector<GtkWidget*> to_remove;
  while (child) {
    if (ADW_IS_ACTION_ROW(child)) {
      to_remove.push_back(child);
    }
    child = gtk_widget_get_next_sibling(child);
  }
  for (auto* w : to_remove) {
    adw_preferences_group_remove(s->group_devices, w);
  }

  std::vector<Device> devs;
  {
    std::lock_guard<std::mutex> lk(s->mu);
    devs = s->devices;
  }

  if (devs.empty()) {
    AdwActionRow* row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Buscando televisores en la red local…");
    adw_action_row_set_subtitle(row, "Asegúrate de que el receptor Linky esté abierto en el Android TV");
    adw_action_row_add_prefix(row, gtk_image_new_from_icon_name("network-wireless-symbolic"));
    adw_preferences_group_add(s->group_devices, GTK_WIDGET(row));
    return;
  }

  for (const auto& d : devs) {
    AdwActionRow* row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), d.name.c_str());
    
    std::string sub = d.host + ":" + std::to_string(d.port);
    if (!d.codecs.empty()) sub += "  (" + d.codecs + ")";
    adw_action_row_set_subtitle(row, sub.c_str());
    adw_action_row_add_prefix(row, gtk_image_new_from_icon_name("tv-symbolic"));

    GtkWidget* btn = gtk_button_new_with_label("Transmitir");
    gtk_widget_add_css_class(btn, "suggested-action");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);

    // Conectar botón
    struct ConnectData {
      AppState* state;
      Device dev;
    };
    auto* cd = new ConnectData{s, d};

    g_signal_connect_data(
        btn, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer user_data) {
          auto* data = static_cast<ConnectData*>(user_data);
          auto* st = data->state;
          Device dev = data->dev;

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

          // Configuración desde la UI
          guint res_idx = adw_combo_row_get_selected(st->combo_resolution);
          if (res_idx == 1) { cfg.width = 1920; cfg.height = 1080; }
          else if (res_idx == 2) { cfg.width = 1280; cfg.height = 720; }
          else { cfg.width = 1920; cfg.height = 1080; } // Auto / Default

          guint fps_idx = adw_combo_row_get_selected(st->combo_fps);
          cfg.fps = (fps_idx == 0) ? 60 : 30;

          cfg.bitrate_kbps = static_cast<int>(gtk_range_get_value(GTK_RANGE(st->scale_bitrate)));
          cfg.audio = adw_switch_row_get_active(st->switch_audio);

          st->active_target_name = dev.name;
          st->sender.set_target(dev.host, dev.port, dev.name, caps);

          adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->row_status_badge), ("Conectando a " + dev.name + "…").c_str());
          gtk_widget_set_visible(GTK_WIDGET(st->group_stats), TRUE);

          st->sender.start(cfg, [st](const SenderApp::Event& ev) {
            {
              std::lock_guard<std::mutex> lk(st->mu);
              st->last_event = ev;
            }
            g_idle_add(+[](gpointer p) -> gboolean {
              auto* s = static_cast<AppState*>(p);
              SenderApp::Event e;
              {
                std::lock_guard<std::mutex> lk(s->mu);
                e = s->last_event;
              }
              const char* status_label = "Inactivo";
              if (e.status == SenderStatus::Connecting) status_label = "Conectando…";
              else if (e.status == SenderStatus::WaitingAcceptance) status_label = "Esperando confirmación en el TV…";
              else if (e.status == SenderStatus::Streaming) status_label = "Transmitiendo en directo";
              else if (e.status == SenderStatus::Failed) status_label = "Error de conexión";

              adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->row_status_badge), status_label);
              if (!e.message.empty()) {
                adw_action_row_set_subtitle(s->row_status_badge, e.message.c_str());
              }

              if (e.status == SenderStatus::Streaming) {
                char buf[256];
                snprintf(buf, sizeof buf, "%.1f FPS  ·  %.0f kbps  ·  Frames: %d  ·  Drops: %d  ·  NACKs: %d",
                         e.stats.fps, e.stats.kbps, e.stats.frames_encoded,
                         e.stats.frames_dropped, e.stats.nacks);
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->row_stats_info), buf);
              } else if (e.status == SenderStatus::Failed || e.status == SenderStatus::Idle) {
                gtk_widget_set_visible(GTK_WIDGET(s->group_stats), FALSE);
              }
              return G_SOURCE_REMOVE;
            }, st);
          });
        }),
        cd, [](gpointer data, GClosure*) { delete static_cast<ConnectData*>(data); },
        G_CONNECT_DEFAULT);

    adw_action_row_add_suffix(row, btn);
    adw_preferences_group_add(s->group_devices, GTK_WIDGET(row));
  }
}

static void on_stop_clicked(GtkButton*, gpointer user_data) {
  auto* s = static_cast<AppState*>(user_data);
  s->sender.stop();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->row_status_badge), "Transmisión detenida");
  adw_action_row_set_subtitle(s->row_status_badge, "Listo para nueva conexión");
  gtk_widget_set_visible(GTK_WIDGET(s->group_stats), FALSE);
}

static void on_refresh_clicked(GtkButton*, gpointer user_data) {
  auto* s = static_cast<AppState*>(user_data);
  s->disc.stop();
  s->disc.start([s](const std::vector<Device>&) {
    g_idle_add(+[](gpointer p) -> gboolean {
      auto* st = static_cast<AppState*>(p);
      {
        std::lock_guard<std::mutex> lk(st->mu);
        st->devices = st->disc.devices();
      }
      update_devices_ui(st);
      return G_SOURCE_REMOVE;
    }, s);
  });
}

static void on_bitrate_changed(GtkRange* range, gpointer user_data) {
  auto* s = static_cast<AppState*>(user_data);
  int val = static_cast<int>(gtk_range_get_value(range));
  char buf[32];
  snprintf(buf, sizeof buf, "%d kbps", val);
  gtk_label_set_text(s->label_bitrate_val, buf);
}

static void on_app_activate(GtkApplication* app, gpointer user_data) {
  auto* s = static_cast<AppState*>(user_data);

  s->win = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
  gtk_window_set_title(GTK_WINDOW(s->win), "Linky Stream");
  gtk_window_set_default_size(GTK_WINDOW(s->win), 640, 680);

  // Barra de cabecera Libadwaita
  GtkWidget* header = adw_header_bar_new();
  s->btn_refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_set_tooltip_text(s->btn_refresh, "Buscar televisores");
  g_signal_connect(s->btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), s);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), s->btn_refresh);

  // Página de preferencias
  AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

  // Grupo 1: Estado del Sistema / Transmisión
  s->group_stats = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(s->group_stats, "Transmisión Activa");
  s->row_status_badge = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->row_status_badge), "Listo");
  adw_action_row_set_subtitle(s->row_status_badge, "Selecciona un televisor para iniciar la duplicación");
  adw_preferences_group_add(s->group_stats, GTK_WIDGET(s->row_status_badge));

  s->row_stats_info = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->row_stats_info), "Métricas en tiempo real...");
  adw_action_row_add_prefix(s->row_stats_info, gtk_image_new_from_icon_name("utilities-system-monitor-symbolic"));
  adw_preferences_group_add(s->group_stats, GTK_WIDGET(s->row_stats_info));

  s->btn_stop = gtk_button_new_with_label("Detener Transmisión");
  gtk_widget_add_css_class(s->btn_stop, "destructive-action");
  gtk_widget_set_margin_top(s->btn_stop, 8);
  g_signal_connect(s->btn_stop, "clicked", G_CALLBACK(on_stop_clicked), s);
  adw_preferences_group_add(s->group_stats, s->btn_stop);
  gtk_widget_set_visible(GTK_WIDGET(s->group_stats), FALSE);
  adw_preferences_page_add(page, s->group_stats);

  // Grupo 2: Dispositivos descubiertos
  s->group_devices = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(s->group_devices, "Televisores en la Red Local");
  adw_preferences_group_set_description(s->group_devices, "Descubrimiento automático de receptores Android TV / Google TV");
  adw_preferences_page_add(page, s->group_devices);

  // Grupo 3: Opciones de Transmisión
  AdwPreferencesGroup* group_settings = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group_settings, "Calidad y Parámetros");

  // Selector de resolución
  const char* const res_options[] = {"Pantalla Completa (Auto)", "1080p (1920x1080)", "720p (1280x720)", nullptr};
  GtkStringList* res_list = gtk_string_list_new(res_options);
  s->combo_resolution = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->combo_resolution), "Resolución de Captura");
  adw_combo_row_set_model(s->combo_resolution, G_LIST_MODEL(res_list));
  adw_combo_row_set_selected(s->combo_resolution, 0);
  adw_preferences_group_add(group_settings, GTK_WIDGET(s->combo_resolution));

  // Selector de FPS
  const char* const fps_options[] = {"60 FPS (Ultra Fluido / Juego)", "30 FPS (Estándar / Video)", nullptr};
  GtkStringList* fps_list = gtk_string_list_new(fps_options);
  s->combo_fps = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->combo_fps), "Frecuencia de Cuadros");
  adw_combo_row_set_model(s->combo_fps, G_LIST_MODEL(fps_list));
  adw_combo_row_set_selected(s->combo_fps, 0);
  adw_preferences_group_add(group_settings, GTK_WIDGET(s->combo_fps));

  // Slider de Bitrate
  AdwActionRow* row_bitrate = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row_bitrate), "Tasa de Bits (Bitrate)");
  GtkWidget* bitrate_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  s->scale_bitrate = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 3000, 25000, 1000));
  gtk_widget_set_size_request(GTK_WIDGET(s->scale_bitrate), 180, -1);
  gtk_range_set_value(GTK_RANGE(s->scale_bitrate), 8000);
  s->label_bitrate_val = GTK_LABEL(gtk_label_new("8000 kbps"));
  g_signal_connect(s->scale_bitrate, "value-changed", G_CALLBACK(on_bitrate_changed), s);
  gtk_box_append(GTK_BOX(bitrate_box), GTK_WIDGET(s->scale_bitrate));
  gtk_box_append(GTK_BOX(bitrate_box), GTK_WIDGET(s->label_bitrate_val));
  adw_action_row_add_suffix(row_bitrate, bitrate_box);
  adw_preferences_group_add(group_settings, GTK_WIDGET(row_bitrate));

  // Audio Switch
  s->switch_audio = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->switch_audio), "Transmitir Audio del Sistema");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(s->switch_audio), "Captura de monitor loopback PipeWire en Opus 48 kHz");
  adw_switch_row_set_active(s->switch_audio, TRUE);
  adw_preferences_group_add(group_settings, GTK_WIDGET(s->switch_audio));

  adw_preferences_page_add(page, group_settings);

  // Layout principal
  GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(main_box), header);
  gtk_box_append(GTK_BOX(main_box), GTK_WIDGET(page));
  adw_application_window_set_content(s->win, main_box);

  // Iniciar descubrimiento mDNS
  s->disc.start([s](const std::vector<Device>&) {
    g_idle_add(+[](gpointer p) -> gboolean {
      auto* st = static_cast<AppState*>(p);
      {
        std::lock_guard<std::mutex> lk(st->mu);
        st->devices = st->disc.devices();
      }
      update_devices_ui(st);
      return G_SOURCE_REMOVE;
    }, s);
  });

  update_devices_ui(s);
  gtk_window_present(GTK_WINDOW(s->win));
}

}  // namespace

int main(int argc, char** argv) {
  g_state = new AppState();
  AdwApplication* app = adw_application_new("dev.linky.Sender", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), g_state);

  int status = g_application_run(G_APPLICATION(app), argc, argv);

  g_state->sender.stop();
  g_state->disc.stop();
  g_object_unref(app);
  delete g_state;
  return status;
}
