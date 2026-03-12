#include "http_server.h"
#include "config/config_manager.h"
#include "net/wifi_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "http_srv";

/* ── Log ring buffer ────────────────────────────────────────────────── */
#define LOG_RING_SIZE 128

static char  s_log_ring[LOG_RING_SIZE][LOG_LINE_MAX];
static int   s_log_head  = 0;
static int   s_log_count = 0;

void http_server_log(const char *line)
{
    strncpy(s_log_ring[s_log_head], line, LOG_LINE_MAX - 1);
    s_log_ring[s_log_head][LOG_LINE_MAX - 1] = '\0';
    s_log_head = (s_log_head + 1) % LOG_RING_SIZE;
    if (s_log_count < LOG_RING_SIZE) s_log_count++;
}

/* ── Config-changed callback ────────────────────────────────────────── */
static http_config_changed_cb_t s_config_cb = NULL;

/* ── HTML ───────────────────────────────────────────────────────────── */

static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>SmartHome Setup</title>"
"<style>"
"body{background:#0d1117;color:#e6edf3;font-family:monospace;margin:0;padding:16px}"
"h2{color:#40c080;margin-bottom:8px}"
"h3{color:#8b949e;margin:16px 0 8px}"
"textarea{width:100%;height:320px;background:#161b22;color:#e6edf3;"
"border:1px solid #30363d;border-radius:8px;padding:8px;"
"font-family:monospace;font-size:13px;box-sizing:border-box}"
"button{background:#40c080;color:#0d1117;border:none;padding:10px 24px;"
"border-radius:6px;cursor:pointer;font-size:14px;font-weight:bold;margin-top:8px}"
"button:hover{background:#50d090}"
"button.warn{background:#f0c040}"
"button.info{background:#40b0f0}"
"#log{width:100%;height:260px;background:#161b22;color:#8b949e;"
"border:1px solid #30363d;border-radius:8px;padding:8px;overflow-y:auto;"
"font-size:12px;white-space:pre;box-sizing:border-box}"
"#status{margin-top:8px;min-height:20px;font-size:13px}"
".ok{color:#40c080}.err{color:#e05050}"
".card{background:#161b22;border:1px solid #30363d;border-radius:12px;"
"padding:16px;margin-bottom:16px}"
"input{background:#0d1117;color:#e6edf3;border:1px solid #30363d;"
"border-radius:6px;padding:8px;width:100%;box-sizing:border-box;margin-bottom:8px}"
"</style></head><body>"
"<h2>SmartHome Dashboard</h2>"
"<div class='card'>"
"<h3>Dashboard Config (JSON)</h3>"
"<textarea id='cfg'></textarea><br>"
"<button onclick='saveConfig()'>Save &amp; Reload UI</button>"
"<div id='status'></div>"
"</div>"
"<div class='card'>"
"<h3>MQTT Broker</h3>"
"<input id='mqttbroker' placeholder='mqtt://192.168.1.x:1883' />"
"<button onclick='saveMQTT()'>Save &amp; Connect</button>"
"<div id='mqttstatus'></div>"
"</div>"
"<div class='card'>"
"<h3>WiFi Setup</h3>"
"<input id='ssid' placeholder='Network name (SSID)' />"
"<input id='pass' placeholder='Password' type='password' />"
"<button class='warn' onclick='saveWifi()'>Save &amp; Reboot</button>"
"<button class='info' onclick='scanWifi()' style='margin-left:8px'>Scan networks</button>"
"<div id='networks' style='margin-top:8px'></div>"
"</div>"
"<div class='card'>"
"<h3>Live Logs</h3>"
"<div id='log'></div>"
"<div style='margin-top:8px;display:flex;gap:8px;flex-wrap:wrap'>"
"<button class='info' onclick='pollLogs()'>Refresh</button>"
"<button class='info' onclick='copyLogs()'>Copy all</button>"
"<button class='info' onclick='saveLogs()'>Save .txt</button>"
"<button onclick='document.getElementById(\"log\").textContent=\"\"'>Clear</button>"
"</div>"
"</div>"
"<script>"
"async function loadConfig(){"
"  try{"
"    const r=await fetch('/config');"
"    const j=await r.json();"
"    document.getElementById('cfg').value=JSON.stringify(j,null,2);"
"  }catch(e){setStatus('Load failed: '+e,'err')}"
"}"
"function setStatus(msg,cls){"
"  const el=document.getElementById('status');"
"  el.textContent=msg; el.className=cls||'';"
"}"
"async function saveConfig(){"
"  const txt=document.getElementById('cfg').value;"
"  try{JSON.parse(txt);}catch(e){setStatus('Invalid JSON: '+e,'err');return;}"
"  try{"
"    const r=await fetch('/config',{method:'POST',"
"      headers:{'Content-Type':'application/json'},body:txt});"
"    r.ok?setStatus('Saved! UI reloading...','ok'):setStatus('Error '+r.status,'err');"
"  }catch(e){setStatus('Failed: '+e,'err')}"
"}"
"async function saveWifi(){"
"  const s=document.getElementById('ssid').value.trim();"
"  const p=document.getElementById('pass').value;"
"  if(!s){alert('Enter SSID');return;}"
"  if(!confirm('Save WiFi and reboot?'))return;"
"  await fetch('/wifi',{method:'POST',"
"    headers:{'Content-Type':'application/json'},"
"    body:JSON.stringify({ssid:s,password:p})});"
"  setStatus('Rebooting...','ok');"
"}"
"async function scanWifi(){"
"  setStatus('Scanning...','');"
"  try{"
"    const r=await fetch('/wifi/scan');"
"    const j=await r.json();"
"    const el=document.getElementById('networks');"
"    el.innerHTML=j.map(n=>"
"      '<a href=\"#\" onclick=\"document.getElementById(\\x27ssid\\x27).value=\\x27'"
"      +n.ssid+'\\x27;return false\" style=\"color:#40b0f0;display:block\">'"
"      +n.ssid+' ('+n.rssi+'dBm)</a>').join('');"
"    setStatus('','');"
"  }catch(e){setStatus('Scan failed','err')}"
"}"
"async function pollLogs(){"
"  const r=await fetch('/logs');"
"  const t=await r.text();"
"  const el=document.getElementById('log');"
"  el.textContent=t;"
"  el.scrollTop=el.scrollHeight;"
"}"
"async function copyLogs(){"
"  const t=document.getElementById('log').textContent;"
"  try{await navigator.clipboard.writeText(t);setStatus('Logs copied!','ok');}catch(e){"
"    const ta=document.createElement('textarea');"
"    ta.value=t;document.body.appendChild(ta);ta.select();"
"    document.execCommand('copy');document.body.removeChild(ta);"
"    setStatus('Logs copied (fallback)','ok');}"
"}"
"function saveLogs(){"
"  const t=document.getElementById('log').textContent;"
"  const ts=new Date().toISOString().replace(/[:.]/g,'-').slice(0,19);"
"  const a=document.createElement('a');"
"  a.href=URL.createObjectURL(new Blob([t],{type:'text/plain'}));"
"  a.download='smarthome-log-'+ts+'.txt';"
"  a.click();URL.revokeObjectURL(a.href);"
"}"
"async function saveMQTT(){"
"  const broker=document.getElementById('mqttbroker').value.trim();"
"  if(!broker){document.getElementById('mqttstatus').textContent='Enter broker URL';return;}"
"  const r=await fetch('/config');"
"  const cfg=await r.json();"
"  if(!cfg.mqtt)cfg.mqtt={};"
"  cfg.mqtt.broker=broker;"
"  const r2=await fetch('/config',{method:'POST',"
"    headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});"
"  document.getElementById('mqttstatus').textContent=r2.ok?'Saved — connecting to broker':'Error '+r2.status;"
"}"
"async function loadMQTT(){"
"  try{"
"    const r=await fetch('/config');"
"    const j=await r.json();"
"    if(j.mqtt&&j.mqtt.broker)document.getElementById('mqttbroker').value=j.mqtt.broker;"
"  }catch(e){}"
"}"
"loadConfig();loadMQTT();"
"pollLogs();"
"setInterval(pollLogs,3000);"
"</script></body></html>";

static const char CAPTIVE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta http-equiv='refresh' content='0;url=http://192.168.4.1/'>"
"</head><body><a href='http://192.168.4.1/'>Open Setup</a></body></html>";

/* ── Handlers ───────────────────────────────────────────────────────── */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_captive(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CAPTIVE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_config_get(httpd_req_t *req)
{
    cJSON *cfg = config_get();
    char  *str = cJSON_Print(cfg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return ESP_OK;
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad size");
        return ESP_FAIL;
    }
    char *buf = malloc(content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int recv = httpd_req_recv(req, buf, content_len);
    buf[recv > 0 ? recv : 0] = '\0';

    cJSON *new_cfg = cJSON_Parse(buf);
    free(buf);
    if (!new_cfg) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    if (!config_save(new_cfg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "Config updated via HTTP");
    if (s_config_cb) s_config_cb();
    return ESP_OK;
}

static esp_err_t handle_wifi_post(httpd_req_t *req)
{
    char buf[256] = {0};
    httpd_req_recv(req, buf, sizeof(buf) - 1);
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *ssid_j = cJSON_GetObjectItem(j, "ssid");
    cJSON *pass_j = cJSON_GetObjectItem(j, "password");
    if (!ssid_j || !cJSON_IsString(ssid_j)) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }
    char ssid[64], pass[64] = {0};
    strncpy(ssid, ssid_j->valuestring, sizeof(ssid) - 1);
    if (pass_j && cJSON_IsString(pass_j))
        strncpy(pass, pass_j->valuestring, sizeof(pass) - 1);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    wifi_manager_connect(ssid, pass);
    return ESP_OK;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    /* Simple scan — blocks briefly */
    esp_wifi_scan_start(NULL, true);
    uint16_t count = 20;
    wifi_ap_record_t records[20];
    esp_wifi_scan_get_ap_records(&count, records);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char *)records[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
        cJSON_AddItemToArray(arr, ap);
    }
    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return ESP_OK;
}

static esp_err_t handle_logs_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    int start = (s_log_count < LOG_RING_SIZE) ? 0 : s_log_head;
    for (int i = 0; i < s_log_count; i++) {
        int idx = (start + i) % LOG_RING_SIZE;
        httpd_resp_sendstr_chunk(req, s_log_ring[idx]);
        httpd_resp_sendstr_chunk(req, "\n");
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── OTA handler ────────────────────────────────────────────────────── */

static esp_err_t handle_ota_post(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int total = req->content_len, received = 0;
    ESP_LOGI(TAG, "OTA: receiving %d bytes to %s", total, update_part->label);

    while (received < total) {
        int chunk = httpd_req_recv(req, buf, MIN(4096, total - received));
        if (chunk <= 0) {
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota_handle, buf, chunk) != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }
        received += chunk;
    }
    free(buf);

    if (esp_ota_end(ota_handle) != ESP_OK ||
        esp_ota_set_boot_partition(update_part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end/boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: success — rebooting");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"OTA complete, rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ── Start ──────────────────────────────────────────────────────────── */

void http_server_start(http_config_changed_cb_t on_config_changed)
{
    s_config_cb = on_config_changed;

    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 13;   /* browsers open multiple keep-alive sockets */
    config.stack_size       = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    const httpd_uri_t uris[] = {
        { .uri="/",              .method=HTTP_GET,  .handler=handle_root       },
        { .uri="/config",       .method=HTTP_GET,  .handler=handle_config_get },
        { .uri="/config",       .method=HTTP_POST, .handler=handle_config_post },
        { .uri="/wifi",         .method=HTTP_POST, .handler=handle_wifi_post  },
        { .uri="/wifi/scan",    .method=HTTP_GET,  .handler=handle_wifi_scan  },
        { .uri="/logs",         .method=HTTP_GET,  .handler=handle_logs_get   },
        { .uri="/ota",          .method=HTTP_POST, .handler=handle_ota_post },
        { .uri="/generate_204", .method=HTTP_GET,  .handler=handle_captive    },
        { .uri="/hotspot-detect.html", .method=HTTP_GET, .handler=handle_captive },
    };
    for (int i = 0; i < sizeof(uris)/sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server started");
}
