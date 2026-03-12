#pragma once
#include "cJSON.h"
#include <stdbool.h>

/* Initialise SPIFFS and load config. Must be called once before anything else. */
void     config_manager_init(void);

/* Returns the live config root — do NOT cJSON_Delete() this pointer. */
cJSON   *config_get(void);

/* Save a new config (replaces current). Takes ownership — do NOT free after call. */
bool     config_save(cJSON *new_root);

/* Reload from SPIFFS (e.g. after external write). Returns new root. */
cJSON   *config_reload(void);

/* MQTT broker URL stored in JSON config — e.g. "mqtt://192.168.1.x:1883" */
bool     config_mqtt_get(char *broker_out, size_t broker_len);

/* WiFi credentials stored in NVS */
bool     config_wifi_get(char *ssid_out, size_t ssid_len,
                         char *pass_out, size_t pass_len);
void     config_wifi_set(const char *ssid, const char *pass);
