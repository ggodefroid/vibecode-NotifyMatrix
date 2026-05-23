#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

struct RuntimeIdfmSlot {
  char monitoring_ref[72] = "";
  char line_ref[48] = "";
  char line_code[16] = "";
  char label[16] = "";
  /// idfm | naolib | tul
  char provider[12] = "idfm";
  /// Clé feed Mecatran (Laval / Aléop), ex. tul
  char feed_key[32] = "";
  bool prefer_theoretical = false;
};

/// 0 = QUAI/PCH puis minutes, 1 = minutes uniquement, 2 = QUAI/PCH seulement (sinon --)
enum EtaDisplayMode : uint8_t {
  kEtaDisplayAuto = 0,
  kEtaDisplayMinutesOnly = 1,
  kEtaDisplayLabelsOnly = 2,
};

struct RuntimeConfig {
  bool configured = false;
  char wifi_ssid[33] = "";
  char wifi_password[65] = "";
  char mqtt_host[64] = "";
  uint16_t mqtt_port = 1883;
  char mqtt_user[32] = "";
  char mqtt_password[65] = "";
  char mqtt_topic_notify[64] = "notifymatrix/notify";
  char idfm_api_key[96] = "";
  /// Base URL API temps réel Aléop/Mecatran (Laval).
  char transit_api_base[80] = "https://app.mecatran.com";
  char idfm_destination_filter[48] = "";
  char idfm_monitoring_ref[72] = "";
  char idfm_line_ref[48] = "";
  char idfm_line_code[16] = "";
  char idfm_line_label[16] = "2225";
  char idfm_provider[12] = "idfm";
  char idfm_feed_key[32] = "";
  bool idfm_prefer_theoretical = false;
  uint8_t slot_count = 0;
  RuntimeIdfmSlot slots[16];

  char setup_line1[40] = "";
  char setup_line2[40] = "";
  char setup_line3[40] = "";
  /// Texte centré sur l'animation arc-en-ciel au démarrage (ex. TweakFR). Vide = pas de texte.
  char boot_splash_text[16] = "TweakFR";
  uint16_t boot_selftest_ms = 1200;
  uint8_t color_time_r = 255;
  uint8_t color_time_g = 165;
  uint8_t color_time_b = 0;
  uint8_t color_eta_green_r = 0;
  uint8_t color_eta_green_g = 255;
  uint8_t color_eta_green_b = 0;
  uint8_t color_eta_orange_r = 255;
  uint8_t color_eta_orange_g = 165;
  uint8_t color_eta_orange_b = 0;
  uint8_t color_eta_red_r = 255;
  uint8_t color_eta_red_g = 0;
  uint8_t color_eta_red_b = 0;
  uint8_t color_eta_noinfo_r = 0;
  uint8_t color_eta_noinfo_g = 180;
  uint8_t color_eta_noinfo_b = 200;
  uint8_t color_bus_line_r = 255;
  uint8_t color_bus_line_g = 165;
  uint8_t color_bus_line_b = 0;
  uint8_t color_blink_on_r = 255;
  uint8_t color_blink_on_g = 255;
  uint8_t color_blink_on_b = 255;
  uint8_t color_blink_off_r = 255;
  uint8_t color_blink_off_g = 0;
  uint8_t color_blink_off_b = 0;
  /// 0 = pas d'animation de changement d'heure.
  uint16_t bus_eta_blink_ms = 300;
  uint16_t carousel_hold_ms = 3500;
  uint16_t carousel_hold_no_info_ms = 3000;
  /// Masquer dans le carrousel les lignes sans horaire renvoyé par l'API.
  bool hide_carousel_no_info = true;
  uint8_t eta_threshold_green = 10;
  uint8_t eta_threshold_orange = 7;
  uint8_t eta_display_mode = kEtaDisplayAuto;
  uint8_t minutes_quai_max = 1;
  uint8_t minutes_pch_max = 3;
  /// Texte court affiché (max 5 car. à l'écran) ou ignoré si show_minutes.
  char eta_label_quai[8] = "QUAI";
  char eta_label_pch[8] = "PCH";
  bool eta_quai_show_minutes = false;
  bool eta_pch_show_minutes = false;
  /// Luminosité panneau 0–255 (jour / nuit).
  uint8_t brightness_normal = 28;
  uint8_t brightness_dim = 8;
  /// Heure de début du mode nuit (0–23), ex. 20 = 20h.
  uint8_t dim_hour_start = 20;
  /// Heure de fin du mode nuit (0–23), ex. 7 = 7h.
  uint8_t dim_hour_end = 7;
};

/// Load config from NVS (or leave defaults). Call once at boot.
void runtime_config_init();

const RuntimeConfig& runtime_config_get();

bool runtime_config_is_configured();

/// Parse JSON object from the web UI POST body.
bool runtime_config_apply_json(JsonObjectConst root, RuntimeConfig& cfg);

/// Persist config and mark as configured. Does not reboot.
bool runtime_config_save(const RuntimeConfig& cfg);

/// Erase NVS config (returns to setup portal on next boot).
void runtime_config_clear();

const char* cfg_wifi_ssid();
const char* cfg_wifi_password();
const char* cfg_mqtt_host();
uint16_t cfg_mqtt_port();
const char* cfg_mqtt_user();
const char* cfg_mqtt_password();
const char* cfg_mqtt_topic_notify();
const char* cfg_idfm_api_key();
const char* cfg_transit_api_base();
const char* cfg_idfm_destination_filter();
const char* cfg_idfm_default_line_label();

const char* cfg_setup_line1();
const char* cfg_setup_line2();
const char* cfg_setup_line3();
const char* cfg_boot_splash_text();
uint16_t cfg_boot_selftest_ms();
uint8_t cfg_color_time_r();
uint8_t cfg_color_time_g();
uint8_t cfg_color_time_b();
uint8_t cfg_color_eta_green_r();
uint8_t cfg_color_eta_green_g();
uint8_t cfg_color_eta_green_b();
uint8_t cfg_color_eta_orange_r();
uint8_t cfg_color_eta_orange_g();
uint8_t cfg_color_eta_orange_b();
uint8_t cfg_color_eta_red_r();
uint8_t cfg_color_eta_red_g();
uint8_t cfg_color_eta_red_b();
uint8_t cfg_color_eta_noinfo_r();
uint8_t cfg_color_eta_noinfo_g();
uint8_t cfg_color_eta_noinfo_b();
uint8_t cfg_color_bus_line_r();
uint8_t cfg_color_bus_line_g();
uint8_t cfg_color_bus_line_b();
uint8_t cfg_color_blink_on_r();
uint8_t cfg_color_blink_on_g();
uint8_t cfg_color_blink_on_b();
uint8_t cfg_color_blink_off_r();
uint8_t cfg_color_blink_off_g();
uint8_t cfg_color_blink_off_b();
uint16_t cfg_bus_eta_blink_ms();
uint16_t cfg_carousel_hold_ms();
uint16_t cfg_carousel_hold_no_info_ms();
bool cfg_hide_carousel_no_info();
uint8_t cfg_eta_threshold_green();
uint8_t cfg_eta_threshold_orange();
uint8_t cfg_eta_display_mode();
uint8_t cfg_minutes_quai_max();
uint8_t cfg_minutes_pch_max();
const char* cfg_eta_label_quai();
const char* cfg_eta_label_pch();
bool cfg_eta_quai_show_minutes();
bool cfg_eta_pch_show_minutes();
uint8_t cfg_brightness_normal();
uint8_t cfg_brightness_dim();
uint8_t cfg_dim_hour_start();
uint8_t cfg_dim_hour_end();
bool cfg_brightness_is_dim_hour(int hour);

/// true si le libellé QUAI/PCH affiché doit clignoter (selon minutes et config).
bool cfg_bus_eta_should_blink(const char* bus_text, int16_t eta_minutes);

/// Format minutes d'attente pour la zone bus (libellés / Nm selon la config).
void cfg_format_bus_eta_text(int minutes,
                             bool prefer_theoretical,
                             const char* departure_iso,
                             char* out,
                             size_t out_size);
