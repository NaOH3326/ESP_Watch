/**
 * @file  ft_wifi.c
 * @brief Wi-Fi 工厂测试：扫描 AP、连接并获取 IP、打印状态
 */
#include "ft_wifi.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

static const char *TAG = "FT_WIFI";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static bool s_wifi_ready;
static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta_netif;
static esp_netif_ip_info_t s_last_ip;
static uint8_t s_disconnect_reason;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        s_disconnect_reason = disc ? disc->reason : 0;
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        if (ev) {
            s_last_ip = ev->ip_info;
        }
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_init_once(void)
{
    if (s_wifi_ready) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    s_wifi_ready = true;
    ESP_LOGI(TAG, "STA started");
    return ESP_OK;
}

static int ssid_len(const uint8_t ssid[33])
{
    size_t n = strnlen((const char *)ssid, 32);
    return (int)n;
}

esp_err_t ft_wifi_scan(int max_ap)
{
    if (max_ap <= 0) {
        max_ap = 10;
    }
    if (max_ap > 50) {
        max_ap = 50;
    }

    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        printf("  wifi init failed: %s\n", esp_err_to_name(err));
        return err;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    printf("  scanning 2.4 GHz...\n");
    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        printf("  scan failed: %s\n", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    uint16_t want = (ap_num < max_ap) ? ap_num : (uint16_t)max_ap;
    printf("  found %u AP(s), showing %u\n", ap_num, want);
    if (want == 0) {
        esp_wifi_clear_ap_list();
        return ESP_OK;
    }

    wifi_ap_record_t *list = calloc(want, sizeof(*list));
    if (!list) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&want, list);
    if (err == ESP_OK) {
        printf("  #   RSSI  ch  auth  SSID                              BSSID\n");
        for (uint16_t i = 0; i < want; i++) {
            const wifi_ap_record_t *ap = &list[i];
            printf("  %2u  %4d  %2u  %4d  %-32.*s  %02X:%02X:%02X:%02X:%02X:%02X\n",
                   (unsigned)(i + 1), ap->rssi, ap->primary, (int)ap->authmode,
                   ssid_len(ap->ssid), (const char *)ap->ssid,
                   ap->bssid[0], ap->bssid[1], ap->bssid[2],
                   ap->bssid[3], ap->bssid[4], ap->bssid[5]);
        }
    } else {
        printf("  get records failed: %s\n", esp_err_to_name(err));
    }

    free(list);
    esp_wifi_clear_ap_list();
    return err;
}


esp_err_t ft_wifi_connect(const char *ssid, const char *password, int timeout_ms)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 15000;
    }

    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        printf("  wifi init failed: %s\n", esp_err_to_name(err));
        return err;
    }
    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_disconnect_reason = 0;

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        printf("  set config failed: %s\n", esp_err_to_name(err));
        return err;
    }

    esp_wifi_disconnect();
    printf("  connecting to \"%s\" ...\n", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        printf("  connect call failed: %s\n", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        printf("  connected: " IPSTR "\n", IP2STR(&s_last_ip.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            printf("  RSSI=%d dBm  ch=%u  auth=%d\n",
                   ap.rssi, ap.primary, (int)ap.authmode);
        }
        esp_wifi_disconnect();
        return ESP_OK;
    }

    printf("  connect failed (reason=%u)\n", s_disconnect_reason);
    esp_wifi_disconnect();
    return ESP_FAIL;
}


void ft_wifi_status(void)
{
    if (!s_wifi_ready) {
        printf("  wifi not initialized (run `wifi scan` first)\n");
        return;
    }

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        printf("  MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        printf("  MAC unavailable\n");
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("  connected to %.*s  RSSI=%d dBm  ch=%u\n",
               ssid_len(ap.ssid), (const char *)ap.ssid, ap.rssi, ap.primary);
    } else {
        printf("  not connected\n");
    }

    if (s_sta_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr) {
            printf("  IP " IPSTR "  GW " IPSTR "\n",
                   IP2STR(&ip.ip), IP2STR(&ip.gw));
        }
    }
}

