#pragma once

#include <cstdint>

/// First-boot: Wi‑Fi AP + captive DNS + web configuration server (blocks normal app).
void config_portal_begin(class Hub75MatrixPortal* hub);

/// Normal operation: HTTP config UI on the panel LAN IP (port 80).
void config_web_begin_sta();

/// Handle HTTP (and DNS in AP mode). Call every loop().
void config_web_loop();

/// True only during first-boot AP setup (main app paused).
bool config_portal_active();

/// HTTP server running (AP or STA).
bool config_web_active();

/// Draw AP SSID / password hint on the LED matrix (setup mode only).
void config_portal_draw_matrix();
