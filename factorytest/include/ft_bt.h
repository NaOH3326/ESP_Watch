#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** BLE 扫描 seconds 秒，打印地址/RSSI/广播名。 */
esp_err_t ft_bt_scan(int seconds);

#ifdef __cplusplus
}
#endif
