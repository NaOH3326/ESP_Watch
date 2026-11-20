#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 扫描周围 AP；max_ap 为最多显示条数（默认 10）。 */
esp_err_t ft_wifi_scan(int max_ap);

/** 连接指定 AP 并等待 DHCP；password 可为 NULL/空串。 */
esp_err_t ft_wifi_connect(const char *ssid, const char *password, int timeout_ms);

/** 打印 STA MAC / 当前连接 / IP。 */
void ft_wifi_status(void);

#ifdef __cplusplus
}
#endif
