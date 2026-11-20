# ESP-Watch 工厂测试组件（随主工程一起构建）
set(srcs
    "src/factory_main.c"
    "src/ft_audio.c"
    "src/ft_bt.c"
    "src/ft_i2c.c"
    "src/ft_keys.c"
    "src/ft_lcd.c"
    "src/ft_lcd_tables.c"
    "src/ft_pmic.c"
    "src/ft_sensors.c"
    "src/ft_system.c"
    "src/ft_touch.c"
    "src/ft_wifi.c"
)

set(reqs
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_ledc
    esp_driver_spi
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_lcd
    console
    nvs_flash
    esp_psram
    spi_flash
    esp_timer
    esp_system
    esp_hw_support
    esp_wifi
    esp_netif
    esp_event
    bt
    log
    heap
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${reqs}
)
