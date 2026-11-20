/**
 * @file  ft_system.c
 * @brief 系统信息、Flash、PSRAM、USB-Serial-JTAG
 */
#include "ft_system.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH:return "power-glitch";
    case ESP_RST_CPU_LOCKUP:return "cpu-lockup";
    default:                return "unknown";
    }
}

static const char *chip_model_str(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    default:           return "unknown";
    }
}

void ft_flash_print_info(void)
{
    uint32_t id = 0;
    uint32_t size = 0;
    if (ft_flash_check(&id, &size)) {
        printf("  flash: id=0x%06X (W25Q128 expect 0xEF4018) size=%u MB\n",
               (unsigned)id, (unsigned)(size / (1024 * 1024)));
    } else {
        printf("  flash: read id failed\n");
    }
}

bool ft_flash_check(uint32_t *id, uint32_t *size)
{
    uint32_t local_id = 0;
    uint32_t local_size = 0;
    esp_flash_t *chip = esp_flash_default_chip;
    if (esp_flash_read_id(chip, &local_id) != ESP_OK) {
        return false;
    }
    if (esp_flash_get_size(chip, &local_size) != ESP_OK) {
        return false;
    }
    if (id) {
        *id = local_id;
    }
    if (size) {
        *size = local_size;
    }
    return true;
}

bool ft_usb_connected(void)
{
    return usb_serial_jtag_is_connected();
}

void ft_system_print_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    printf("  chip: %s rev v%d.%d, %d core(s), features=0x%x\n",
           chip_model_str(info.model), info.revision / 100, info.revision % 100,
           info.cores, (unsigned)info.features);
    printf("  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("  IDF: %s\n", esp_get_idf_version());
    printf("  reset reason: %s\n", reset_reason_str(esp_reset_reason()));
    printf("  heap internal: %u free / %u total\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    printf("  heap PSRAM:    %u free / %u total\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    printf("  PSRAM size: %u bytes\n", (unsigned)esp_psram_get_size());
    ft_flash_print_info();
    ft_usb_print_info();
}

void ft_usb_print_info(void)
{
    printf("  USB-Serial-JTAG (GPIO19/20): %s\n",
           usb_serial_jtag_is_connected() ? "connected" : "not connected");
}
