#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool connected;
    bool ap_mode;
    char ip[16];
    char ssid[33];
    int rssi;
} ws_net_status_t;

esp_err_t watch_net_init(void);
bool watch_net_connected(void);
bool watch_net_ap_mode(void);
const char *watch_net_ip(void);
int watch_net_rssi(void);
void watch_net_status(ws_net_status_t *out);

/* Start/refresh the configuration + upload web portal. */
esp_err_t watch_net_start_portal(void);
esp_err_t watch_net_restart_sta(void);
