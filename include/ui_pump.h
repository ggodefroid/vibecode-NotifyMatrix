#pragma once

#include <stdint.h>

/// Rafraîchit le clignotement QUAI/PCH (zone bus uniquement). À appeler pendant les attentes réseau.
void ui_pump_bus_eta_blink();

/// Force un redraw au prochain pompage (changement de ligne / texte bus).
void ui_reset_bus_eta_blink();

/// Attente active avec pompage affichage (remplace delay() long côté IDFM).
void idfm_yield_ms(unsigned long ms);

/// Bloque MQTT / allège le Wi-Fi pendant un GET PRIM (évite les échecs TLS).
void idfm_http_busy_begin();
void idfm_http_busy_end();
bool idfm_http_is_busy();

/// Attend un WiFi stable (utilisé par le worker PRIM avant TLS).
bool nm_wifi_wait_stable(uint32_t timeout_ms);

/// Invalide le cache DNS PRIM (après perte WiFi).
void nm_wifi_mark_lost();
