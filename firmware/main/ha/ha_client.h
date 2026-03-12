#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Generic state callback — called whenever any subscribed entity changes.
 *   entity_id : HA entity id string
 *   state     : new state string ("on", "21.5", "cloudy", …)
 *   attributes: full attributes object (may be NULL) — do NOT free
 */
typedef void (*ha_state_cb_t)(const char *entity_id,
                               const char *state,
                               cJSON      *attributes);

/**
 * Start MQTT client and connect to broker.
 * broker_url: "mqtt://192.168.1.x:1883" or "mqtts://..." for TLS
 * cb: called on every smarthome/state/+ and smarthome/attr/+ message
 */
void ha_client_start(const char *broker_url, ha_state_cb_t cb);

/** Publish {"action":"toggle"} to smarthome/cmd/<entity_id> */
void ha_toggle(const char *entity_id);

/**
 * Publish light_set command.
 * brightness_pct: 0–100, or -1 to omit
 * rgb: 3-byte {r,g,b} array or NULL to omit
 */
void ha_light_set(const char *entity_id, int brightness_pct,
                  const uint8_t *rgb);

/** Call arbitrary HA service via MQTT command topic */
void ha_call_service(const char *domain, const char *service,
                     const char *entity_id);

bool ha_is_connected(void);
