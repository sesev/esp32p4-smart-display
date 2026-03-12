#include "ha_client.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ha_client";

/* ── MQTT Statestream topics ────────────────────────────────────────── */
/* HA publishes: homeassistant/<domain>/<object_id>/state|attributes    */
#define SS_BASE       "homeassistant"
#define SS_STATE_SUB  "homeassistant/+/+/state"
#define SS_ATTR_SUB   "homeassistant/+/+/attributes"
#define T_CMD_PFX     "smarthome/cmd/"

/* ── Attribute cache ────────────────────────────────────────────────── */
#define ATTR_CACHE_SIZE 48
typedef struct {
    char   entity_id[80];   /* "light.living_room" */
    cJSON *attrs;
} attr_cache_t;

static attr_cache_t s_attr_cache[ATTR_CACHE_SIZE];
static int          s_attr_cache_count = 0;

static esp_mqtt_client_handle_t s_client   = NULL;
static ha_state_cb_t            s_state_cb = NULL;
static bool                     s_connected = false;

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Build entity_id from statestream topic.
 * "homeassistant/light/my_lamp/state" → "light.my_lamp"
 * Returns false if topic format is unexpected.                         */
static bool entity_id_from_topic(const char *topic, char *out, size_t out_sz)
{
    /* Skip base "homeassistant/" */
    const char *p = topic;
    const char *base = SS_BASE "/";
    size_t base_len = strlen(base);
    if (strncmp(p, base, base_len) != 0) return false;
    p += base_len;

    /* Find domain segment (up to next '/') */
    const char *slash1 = strchr(p, '/');
    if (!slash1) return false;
    size_t domain_len = (size_t)(slash1 - p);

    /* Find object_id segment (up to next '/') */
    const char *obj_start = slash1 + 1;
    const char *slash2 = strchr(obj_start, '/');
    if (!slash2) return false;
    size_t obj_len = (size_t)(slash2 - obj_start);

    if (domain_len + 1 + obj_len >= out_sz) return false;
    memcpy(out, p, domain_len);
    out[domain_len] = '.';
    memcpy(out + domain_len + 1, obj_start, obj_len);
    out[domain_len + 1 + obj_len] = '\0';
    return true;
}

static cJSON *attr_cache_get(const char *entity_id)
{
    for (int i = 0; i < s_attr_cache_count; i++)
        if (strcmp(s_attr_cache[i].entity_id, entity_id) == 0)
            return s_attr_cache[i].attrs;
    return NULL;
}

static void attr_cache_set(const char *entity_id, cJSON *attrs)
{
    for (int i = 0; i < s_attr_cache_count; i++) {
        if (strcmp(s_attr_cache[i].entity_id, entity_id) == 0) {
            cJSON_Delete(s_attr_cache[i].attrs);
            s_attr_cache[i].attrs = attrs;
            return;
        }
    }
    if (s_attr_cache_count >= ATTR_CACHE_SIZE) return;
    attr_cache_t *e = &s_attr_cache[s_attr_cache_count++];
    size_t elen = strlen(entity_id);
    if (elen >= sizeof(e->entity_id)) elen = sizeof(e->entity_id) - 1;
    memcpy(e->entity_id, entity_id, elen);
    e->entity_id[elen] = '\0';
    e->attrs = attrs;
}

/* ── MQTT event handler ─────────────────────────────────────────────── */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        s_connected = true;
        esp_mqtt_client_subscribe(s_client, SS_STATE_SUB, 0);
        esp_mqtt_client_subscribe(s_client, SS_ATTR_SUB,  0);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA: {
        if (!ev->topic || !ev->data || ev->data_len <= 0) break;

        /* Copy topic (not null-terminated by default) */
        char topic[160] = {0};
        int tlen = ev->topic_len < (int)sizeof(topic) - 1
                   ? ev->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, ev->topic, tlen);

        char entity_id[80] = {0};
        if (!entity_id_from_topic(topic, entity_id, sizeof(entity_id))) break;

        /* Is this a /state or /attributes message? */
        bool is_attr = (strstr(topic, "/attributes") != NULL);

        if (is_attr) {
            /* Attributes payload can be large — allocate from heap */
            char *abuf = malloc(ev->data_len + 1);
            if (!abuf) break;
            memcpy(abuf, ev->data, ev->data_len);
            abuf[ev->data_len] = '\0';
            cJSON *attrs = cJSON_Parse(abuf);
            free(abuf);
            if (attrs) attr_cache_set(entity_id, attrs);
        } else {
            /* State: short string, stack is fine */
            char state[64] = {0};
            int dlen = ev->data_len < (int)sizeof(state) - 1
                       ? ev->data_len : (int)sizeof(state) - 1;
            memcpy(state, ev->data, dlen);

            cJSON *attrs = attr_cache_get(entity_id);
            if (s_state_cb) s_state_cb(entity_id, state, attrs);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error");
        break;

    default: break;
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

void ha_client_start(const char *broker_url, ha_state_cb_t cb)
{
    s_state_cb = cb;

    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri           = broker_url,
        .session.keepalive            = 15,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms           = 10000,
        .buffer.size                  = 8192,   /* attributes can be large */
        .buffer.out_size              = 1024,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "connecting to %s", broker_url);
}

/* ── Command publishing ──────────────────────────────────────────────── */

static void publish_cmd(const char *entity_id, const char *payload)
{
    if (!s_client || !s_connected) {
        ESP_LOGW(TAG, "cmd skipped — not connected");
        return;
    }
    char topic[128];
    snprintf(topic, sizeof(topic), T_CMD_PFX "%s", entity_id);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
}

void ha_toggle(const char *entity_id)
{
    publish_cmd(entity_id, "{\"action\":\"toggle\"}");
    ESP_LOGI(TAG, "toggle: %s", entity_id);
}

void ha_light_set(const char *entity_id, int brightness_pct,
                  const uint8_t *rgb)
{
    char payload[128];
    int n = snprintf(payload, sizeof(payload), "{\"action\":\"light_set\"");
    if (brightness_pct >= 0)
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"brightness_pct\":%d", brightness_pct);
    if (rgb)
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"rgb\":[%d,%d,%d]", rgb[0], rgb[1], rgb[2]);
    snprintf(payload + n, sizeof(payload) - n, "}");
    publish_cmd(entity_id, payload);
    ESP_LOGI(TAG, "light_set: %s bri=%d", entity_id, brightness_pct);
}

void ha_call_service(const char *domain, const char *service,
                     const char *entity_id)
{
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"action\":\"call_service\",\"domain\":\"%s\","
             "\"service\":\"%s\"}", domain, service);
    publish_cmd(entity_id, payload);
}

bool ha_is_connected(void)
{
    return s_connected;
}
