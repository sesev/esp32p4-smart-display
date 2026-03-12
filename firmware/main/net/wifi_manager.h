#pragma once
#include <stdbool.h>

typedef void (*wifi_connected_cb_t)(const char *ip);
typedef void (*wifi_ap_started_cb_t)(const char *ap_ssid, const char *ap_ip);

/* Start WiFi. Tries NVS credentials first, falls back to AP+captive portal. */
void wifi_manager_start(wifi_connected_cb_t  on_connected,
                        wifi_ap_started_cb_t on_ap_started);

/* Save new credentials and reboot into STA mode. */
void wifi_manager_connect(const char *ssid, const char *password);

/* True if currently in AP (setup) mode. */
bool wifi_manager_is_ap_mode(void);
