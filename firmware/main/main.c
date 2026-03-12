#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "config/config_manager.h"
#include "net/wifi_manager.h"
#include "ui/ui_manager.h"
#include "ui/screen_manager.h"
#include "server/http_server.h"
#include "esp_sntp.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "concept";

#define C_GREEN  0x40c080
#define C_YELLOW 0xf0c040

/* ── Demo config (hardcoded — MQTT/HA disabled for UI development) ── */
static const char *DEMO_CONFIG_JSON =
"{"
  "\"pages\":[{"
    "\"weather\":{"
      "\"indoor_entity\":\"sensor.indoor_temp\","
      "\"outdoor_entity\":\"weather.home\""
    "},"
    "\"tiles\":["
      "{\"entity\":\"light.living_room\", \"label\":\"Living Room\", \"icon\":\"charge\", \"color\":\"f0c040\"},"
      "{\"entity\":\"light.bedroom\",     \"label\":\"Bedroom\",     \"icon\":\"charge\", \"color\":\"f08020\"},"
      "{\"entity\":\"light.kitchen\",     \"label\":\"Kitchen\",     \"icon\":\"charge\", \"color\":\"40b0f0\"},"
      "{\"entity\":\"switch.tv\",         \"label\":\"TV\",          \"icon\":\"play\",   \"color\":\"40b0f0\"},"
      "{\"entity\":\"switch.coffee\",     \"label\":\"Coffee\",      \"icon\":\"power\",  \"color\":\"c06030\"},"
      "{\"entity\":\"binary_sensor.door\",\"label\":\"Front Door\",  \"icon\":\"home\",   \"color\":\"40c080\"}"
    "]"
  "}]"
"}";

/* ── Log capture → HTTP server ──────────────────────────────────────── */

static int log_to_server(const char *fmt, va_list args)
{
    static char buf[LOG_LINE_MAX];
    va_list args2;
    va_copy(args2, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
    http_server_log(buf);
    int r = vprintf(fmt, args2);
    va_end(args2);
    return r;
}

/* ── WiFi callbacks ─────────────────────────────────────────────────── */

static void on_config_changed(void)
{
    /* Config portal saved — rebuild UI (MQTT still disabled) */
    ESP_LOGI(TAG, "Config changed — rebuilding UI");
    if (bsp_display_lock(500)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_clean(scr);
        cJSON *demo = cJSON_Parse(DEMO_CONFIG_JSON);
        ui_manager_init(demo);
        cJSON_Delete(demo);
        screen_manager_init(5, NULL);
        bsp_display_unlock();
    }
}

static void on_wifi_connected(const char *ip)
{
    char buf[48];
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  %s", ip);
    ui_manager_set_status(buf, C_GREEN);

    static bool s_sntp_started = false;
    if (!s_sntp_started) {
        setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        s_sntp_started = true;
    }

    http_server_start(on_config_changed);
    /* MQTT/HA disabled — uncomment when re-enabling:
    static char broker[128];
    if (config_mqtt_get(broker, sizeof(broker)))
        ha_client_start(broker, on_ha_state);
    */
}

static void on_ap_started(const char *ssid, const char *ip)
{
    ui_manager_show_ap_screen(ssid, ip);
    http_server_start(on_config_changed);
}

/* ── Entry point ───────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "Smart Home starting");

    nvs_flash_init();
    config_manager_init();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
            .sw_rotate   = false,
        },
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    /* Build UI from hardcoded demo config */
    bsp_display_lock(0);
    cJSON *demo = cJSON_Parse(DEMO_CONFIG_JSON);
    ui_manager_init(demo);
    cJSON_Delete(demo);
    screen_manager_init(5, NULL);
    bsp_display_unlock();

    esp_log_set_vprintf(log_to_server);

    wifi_manager_start(on_wifi_connected, on_ap_started);
}
