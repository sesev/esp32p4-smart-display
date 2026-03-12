#pragma once

#define LOG_LINE_MAX 200

/* Called when a new config was saved via HTTP — implementor rebuilds UI. */
typedef void (*http_config_changed_cb_t)(void);

void http_server_start(http_config_changed_cb_t on_config_changed);
void http_server_log(const char *line);   /* feed log lines from esp_log hook */
