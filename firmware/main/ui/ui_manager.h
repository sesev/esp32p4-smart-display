#pragma once
#include "cJSON.h"
#include <stdint.h>

/* Build full UI from config root. Call once after display init. */
void ui_manager_init(cJSON *config);

/* Update a tile's state (on/off/value). Called from HA state callback. */
void ui_manager_on_state(const char *entity_id, const char *state,
                          cJSON *attributes);

/* Show WiFi status text in footer. */
void ui_manager_set_status(const char *text, uint32_t colour);

/* Show AP setup screen instead of dashboard. */
void ui_manager_show_ap_screen(const char *ssid, const char *ip);
