/**
 * @file  ft_bt.c
 * @brief BLE 工厂测试：NimBLE 扫描，打印地址/RSSI/广播名
 */
#include "ft_bt.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

static SemaphoreHandle_t s_bt_done;
static int s_bt_scan_ms;
static int s_bt_reports;

static void bt_on_reset(int reason)
{
    printf("  BLE reset, reason=%d\n", reason);
}

static void format_addr(const uint8_t val[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             val[5], val[4], val[3], val[2], val[1], val[0]);
}

static int bt_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        char addr[18];
        format_addr(event->disc.addr.val, addr);
        struct ble_hs_adv_fields fields;
        int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        if (rc == 0 && fields.name && fields.name_len) {
            printf("  %s  RSSI=%4d  type=%u  %.*s\n",
                   addr, event->disc.rssi, event->disc.event_type,
                   (int)fields.name_len, (const char *)fields.name);
        } else {
            printf("  %s  RSSI=%4d  type=%u  -\n",
                   addr, event->disc.rssi, event->disc.event_type);
        }
        s_bt_reports++;
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_bt_done) {
            xSemaphoreGive(s_bt_done);
        }
        return 0;
    default:
        return 0;
    }
}

static void bt_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        printf("  ble_hs_util_ensure_addr rc=%d\n", rc);
        if (s_bt_done) {
            xSemaphoreGive(s_bt_done);
        }
        return;
    }

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        printf("  ble_hs_id_infer_auto rc=%d\n", rc);
        if (s_bt_done) {
            xSemaphoreGive(s_bt_done);
        }
        return;
    }

    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 1;
    params.passive = 0;
    rc = ble_gap_disc(own_addr_type, s_bt_scan_ms, &params, bt_gap_event, NULL);
    if (rc != 0) {
        printf("  ble_gap_disc rc=%d\n", rc);
        if (s_bt_done) {
            xSemaphoreGive(s_bt_done);
        }
    }
}

static void bt_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ft_bt_scan(int seconds)
{
    if (seconds <= 0) {
        seconds = 5;
    }
    if (seconds > 60) {
        seconds = 60;
    }

    if (s_bt_done) {
        vSemaphoreDelete(s_bt_done);
        s_bt_done = NULL;
    }
    s_bt_done = xSemaphoreCreateBinary();
    if (!s_bt_done) {
        return ESP_ERR_NO_MEM;
    }
    s_bt_scan_ms = seconds * 1000;
    s_bt_reports = 0;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        printf("  nimble_port_init failed: %s\n", esp_err_to_name(err));
        vSemaphoreDelete(s_bt_done);
        s_bt_done = NULL;
        return err;
    }

    ble_hs_cfg.reset_cb = bt_on_reset;
    ble_hs_cfg.sync_cb = bt_on_sync;
    nimble_port_freertos_init(bt_host_task);

    printf("  BLE scan %d s (ESP32-S3 只有 BLE，无经典蓝牙)...\n", seconds);
    if (xSemaphoreTake(s_bt_done, pdMS_TO_TICKS(s_bt_scan_ms + 5000)) != pdTRUE) {
        printf("  BLE scan timeout, cancelling\n");
        ble_gap_disc_cancel();
    }

    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    } else {
        printf("  nimble_port_stop rc=%d\n", rc);
    }

    vSemaphoreDelete(s_bt_done);
    s_bt_done = NULL;
    printf("  BLE scan done: %d report(s)\n", s_bt_reports);
    return ESP_OK;
}

