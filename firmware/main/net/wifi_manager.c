#include "wifi_manager.h"
#include "config/config_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_manager";

#define MAX_RETRIES   10
#define AP_SSID_PFX   "SmartHome-"

static wifi_connected_cb_t  s_on_connected  = NULL;
static wifi_ap_started_cb_t s_on_ap_started = NULL;
static int                  s_retries       = 0;
static bool                 s_ap_mode       = false;
static esp_netif_t         *s_sta_netif     = NULL;
static esp_netif_t         *s_ap_netif      = NULL;
static TaskHandle_t         s_dns_task      = NULL;

/* ── Captive-portal DNS server ──────────────────────────────────────── */
/*  Replies to every DNS query with 192.168.4.1 so browsers open portal */

#define DNS_PORT 53

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[256];
    while (1) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&src, &src_len);
        if (len < 12) continue;

        /* Build minimal DNS response: copy header, set QR+AA, answer with 192.168.4.1 */
        uint8_t resp[512];
        memcpy(resp, buf, len);
        resp[2] = 0x81; /* QR=1, OPCODE=0, AA=1 */
        resp[3] = 0x80; /* RA=1                  */
        resp[6] = 0x00; resp[7] = 0x01; /* ANCOUNT = 1 */
        resp[8] = resp[9] = resp[10] = resp[11] = 0;

        int rlen = len;
        /* Answer: name ptr → question, type A, class IN, TTL 60, rdlength 4, 192.168.4.1 */
        resp[rlen++] = 0xc0; resp[rlen++] = 0x0c; /* name ptr */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; /* type A   */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; /* class IN */
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 60;   /* TTL      */
        resp[rlen++] = 0x00; resp[rlen++] = 0x04; /* rdlength */
        resp[rlen++] = 192; resp[rlen++] = 168;
        resp[rlen++] = 4;   resp[rlen++] = 1;

        sendto(sock, resp, rlen, 0, (struct sockaddr *)&src, src_len);
    }
}

/* ── AP mode ────────────────────────────────────────────────────────── */

static void start_ap(void)
{
    s_ap_mode = true;

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X",
             AP_SSID_PFX, mac[4], mac[5]);

    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        }
    };
    memcpy(ap_cfg.ap.ssid, ap_ssid, strlen(ap_ssid));
    ap_cfg.ap.ssid_len = strlen(ap_ssid);

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    xTaskCreate(dns_server_task, "dns_srv", 3072, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "AP mode: SSID=%s  IP=192.168.4.1", ap_ssid);
    if (s_on_ap_started) s_on_ap_started(ap_ssid, "192.168.4.1");
}

/* ── STA event handler ──────────────────────────────────────────────── */

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting...");
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < MAX_RETRIES) {
            s_retries++;
            ESP_LOGI(TAG, "Reconnecting (%d/%d)...", s_retries, MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA failed — switching to AP mode");
            esp_wifi_stop();
            if (s_sta_netif) {
                esp_netif_destroy(s_sta_netif);
                s_sta_netif = NULL;
            }
            start_ap();
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ip);
        s_retries = 0;
        if (s_on_connected) s_on_connected(ip);
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

void wifi_manager_start(wifi_connected_cb_t  on_connected,
                        wifi_ap_started_cb_t on_ap_started)
{
    s_on_connected  = on_connected;
    s_on_ap_started = on_ap_started;

    esp_netif_init();
    esp_event_loop_create_default();

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        event_handler, NULL, NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    char ssid[64] = {0}, pass[64] = {0};
    if (config_wifi_get(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0]) {
        ESP_LOGI(TAG, "Connecting to saved SSID: %s", ssid);
        s_sta_netif = esp_netif_create_default_wifi_sta();

        wifi_config_t wifi_cfg = {0};
        memcpy(wifi_cfg.sta.ssid,     ssid, strnlen(ssid, sizeof(wifi_cfg.sta.ssid) - 1));
        memcpy(wifi_cfg.sta.password, pass, strnlen(pass, sizeof(wifi_cfg.sta.password) - 1));

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_start();
    } else {
        ESP_LOGW(TAG, "No saved WiFi — starting AP setup mode");
        start_ap();
    }
}

static void reboot_timer_cb(TimerHandle_t t)
{
    esp_restart();
}

void wifi_manager_connect(const char *ssid, const char *password)
{
    config_wifi_set(ssid, password);
    ESP_LOGI(TAG, "Credentials saved, rebooting in 1s...");
    TimerHandle_t t = xTimerCreate("reboot", pdMS_TO_TICKS(1000),
                                   pdFALSE, NULL, reboot_timer_cb);
    if (t) xTimerStart(t, 0);
}

bool wifi_manager_is_ap_mode(void)
{
    return s_ap_mode;
}
