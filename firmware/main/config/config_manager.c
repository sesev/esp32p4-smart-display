#include "config_manager.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG        = "config_mgr";
static const char *CONFIG_PATH = "/spiffs/config.json";

static cJSON *s_config = NULL;

/* ── Default config ─────────────────────────────────────────────────── */
/*
 * MQTT broker URL format:
 *   No auth:    "mqtt://192.168.1.x:1883"
 *   With auth:  "mqtt://username:password@192.168.1.x:1883"
 *   TLS:        "mqtts://192.168.1.x:8883"
 *
 * WiFi credentials are stored in NVS (not here) — configure via the
 * web portal at http://<device-ip>/ after first boot.
 *
 * Entity IDs must match your Home Assistant instance.
 * Find them in HA → Settings → Devices & Services → Entities.
 */

static const char DEFAULT_CONFIG[] =
"{"
  "\"version\":1,"
  "\"mqtt\":{"
    "\"broker\":\"\""
  "},"
  "\"pages\":[{"
    "\"weather\":{"
      "\"indoor_entity\":\"sensor.YOUR_INDOOR_TEMP_ENTITY\","
      "\"outdoor_entity\":\"weather.YOUR_WEATHER_ENTITY\""
    "},"
    "\"tiles\":["
      "{\"entity\":\"light.YOUR_LIGHT_1\",  \"label\":\"Living Room\", \"icon\":\"charge\", \"color\":\"f0c040\"},"
      "{\"entity\":\"light.YOUR_LIGHT_2\",  \"label\":\"Bedroom\",     \"icon\":\"charge\", \"color\":\"f08020\"},"
      "{\"entity\":\"light.YOUR_LIGHT_3\",  \"label\":\"Kitchen\",     \"icon\":\"charge\", \"color\":\"40b0f0\"},"
      "{\"entity\":\"switch.YOUR_SWITCH_1\",\"label\":\"TV\",          \"icon\":\"play\",   \"color\":\"40b0f0\"},"
      "{\"entity\":\"switch.YOUR_SWITCH_2\",\"label\":\"Coffee\",      \"icon\":\"power\",  \"color\":\"c06030\"},"
      "{\"entity\":\"binary_sensor.YOUR_DOOR_SENSOR\",\"label\":\"Front Door\",\"icon\":\"home\",\"color\":\"40c080\"}"
    "]"
  "}]"
"}";

/* ── SPIFFS ─────────────────────────────────────────────────────────── */

static void spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "storage",
        .max_files              = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("storage", &total, &used);
        ESP_LOGI(TAG, "SPIFFS: %u/%u bytes used", used, total);
    }
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static bool write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(data, f);
    fclose(f);
    return true;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void config_manager_init(void)
{
    spiffs_init();

    char *raw = read_file(CONFIG_PATH);
    if (raw) {
        s_config = cJSON_Parse(raw);
        free(raw);
        if (!s_config) {
            ESP_LOGW(TAG, "config.json corrupt — using default");
        }
    }

    if (!s_config) {
        ESP_LOGI(TAG, "Writing default config");
        write_file(CONFIG_PATH, DEFAULT_CONFIG);
        s_config = cJSON_Parse(DEFAULT_CONFIG);
    }

    /* Migrate: add missing "mqtt" key if loaded from old config */
    if (s_config && !cJSON_GetObjectItem(s_config, "mqtt")) {
        cJSON *mqtt = cJSON_CreateObject();
        cJSON_AddStringToObject(mqtt, "broker", "");
        cJSON_AddItemToObject(s_config, "mqtt", mqtt);
        ESP_LOGI(TAG, "Migrated config: added mqtt section");
        char *str = cJSON_PrintUnformatted(s_config);
        if (str) { write_file(CONFIG_PATH, str); free(str); }
    }

    ESP_LOGI(TAG, "Config loaded");
}

cJSON *config_get(void)
{
    return s_config;
}

bool config_save(cJSON *new_root)
{
    char *str = cJSON_PrintUnformatted(new_root);
    if (!str) return false;
    bool ok = write_file(CONFIG_PATH, str);
    free(str);
    if (ok) {
        cJSON_Delete(s_config);
        s_config = new_root;
        ESP_LOGI(TAG, "Config saved");
    }
    return ok;
}

cJSON *config_reload(void)
{
    char *raw = read_file(CONFIG_PATH);
    if (!raw) return s_config;
    cJSON *fresh = cJSON_Parse(raw);
    free(raw);
    if (fresh) {
        cJSON_Delete(s_config);
        s_config = fresh;
    }
    return s_config;
}

/* ── WiFi credentials in NVS ────────────────────────────────────────── */

#define NVS_NS   "wifi_cfg"
#define NVS_SSID "ssid"
#define NVS_PASS "pass"

bool config_mqtt_get(char *broker_out, size_t broker_len)
{
    if (!s_config) return false;
    cJSON *mqtt = cJSON_GetObjectItem(s_config, "mqtt");
    if (!mqtt) return false;
    cJSON *broker = cJSON_GetObjectItem(mqtt, "broker");
    if (!broker || !cJSON_IsString(broker) || !broker->valuestring[0])
        return false;
    strncpy(broker_out, broker->valuestring, broker_len - 1);
    broker_out[broker_len - 1] = '\0';
    return true;
}

bool config_wifi_get(char *ssid_out, size_t ssid_len,
                     char *pass_out, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, NVS_SSID, ssid_out, &ssid_len) == ESP_OK &&
               nvs_get_str(h, NVS_PASS, pass_out, &pass_len) == ESP_OK);
    nvs_close(h);
    return ok;
}

void config_wifi_set(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_SSID, ssid);
    nvs_set_str(h, NVS_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials saved: %s", ssid);
}
