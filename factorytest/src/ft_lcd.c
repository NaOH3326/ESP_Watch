/**
 * @file  ft_lcd.c
 * @brief ST77916 / NV3030B 4 线 SPI 屏：初始化、帧缓冲、图形与文字
 *
 * 说明：
 *  - 面板只有 MOSI，无 MISO，无法读 ID，测试以目视为主。
 *  - 帧缓冲优先放在 PSRAM；没有 PSRAM 时退化为纯色填充模式。
 *  - 背光 LEDC 输出到 GPIO39，经 Q13 低边开关，高电平点亮。
 */
#include "ft_lcd.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "ft_font5x7.h"

static const char *TAG = "FT_LCD";

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_color_done;
static bool s_bus_ready;
static bool s_backlight_ready;
static ft_lcd_info_t s_info;
static uint16_t *s_fb;
static uint16_t *s_dma_buf;
static size_t s_dma_bytes;

static bool color_done_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp = pdFALSE;
    if (s_color_done) {
        xSemaphoreGiveFromISR(s_color_done, &hp);
    }
    return hp == pdTRUE;
}

static esp_err_t lcd_bus_init(void)
{
    if (s_bus_ready) {
        return ESP_OK;
    }
    spi_bus_config_t buscfg = {
        .mosi_io_num = GPIO_LCD_SDA,
        .miso_io_num = -1,
        .sclk_io_num = GPIO_LCD_SCL,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64 * 1024,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }
    s_bus_ready = true;
    return ESP_OK;
}

static void lcd_hw_reset(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(GPIO_LCD_RST, 1);
    ft_delay_ms(10);
    gpio_set_level(GPIO_LCD_RST, 0);
    ft_delay_ms(30);
    gpio_set_level(GPIO_LCD_RST, 1);
    ft_delay_ms(120);
}

static esp_err_t lcd_io_create(void)
{
    if (s_io) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = GPIO_LCD_CS,
        .dc_gpio_num = GPIO_LCD_DC,
        .spi_mode = s_info.spi_mode,
        .pclk_hz = s_info.spi_hz,
        .trans_queue_depth = 8,
        .on_color_trans_done = color_done_cb,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .flags.sio_mode = 1,
        .flags.lsb_first = 0,
        .flags.cs_high_active = 0,
    };
    esp_err_t err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
    }
    return err;
}

const char *ft_lcd_controller_name(void)
{
    return "GC9307";
}

const ft_lcd_info_t *ft_lcd_info(void)
{
    return &s_info;
}

const uint16_t *ft_lcd_framebuffer(void)
{
    return s_fb;
}

size_t ft_lcd_framebuffer_size(void)
{
    return s_fb ? (size_t)s_info.width * s_info.height * sizeof(uint16_t) : 0;
}

static void lcd_send_table(const ft_lcd_cmd_t *tbl, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const ft_lcd_cmd_t *c = &tbl[i];
        if (c->cmd == 0 && c->len == 0) {
            if (c->delay_ms) {
                ft_delay_ms(c->delay_ms);
            }
            continue;
        }
        esp_err_t err = esp_lcd_panel_io_tx_param(s_io, c->cmd, c->len ? c->data : NULL, c->len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cmd 0x%02X failed: %s", c->cmd, esp_err_to_name(err));
        }
        if (c->delay_ms) {
            ft_delay_ms(c->delay_ms);
        }
    }
}

static uint8_t rotation_bits(uint8_t rotation)
{
    switch (rotation & 0x03) {
    case 1:  return 0x60; /* MX | MV */
    case 2:  return 0xC0; /* MX | MY */
    case 3:  return 0xA0; /* MY | MV */
    default: return 0x00;
    }
}

static uint8_t madctl_base(void)
{
    return 0x48; /* MX + BGR，按 GC9307 厂商初始化 */
}

esp_err_t ft_lcd_set_rotation(uint8_t rotation)
{
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }
    s_info.rotation = rotation & 0x03;
    uint8_t v = (uint8_t)(madctl_base() | rotation_bits(s_info.rotation));
    return esp_lcd_panel_io_tx_param(s_io, 0x36, &v, 1);
}

esp_err_t ft_lcd_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    if (!s_backlight_ready) {
        ledc_timer_config_t tcfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = false,
        };
        esp_err_t err = ledc_timer_config(&tcfg);
        if (err != ESP_OK) {
            return err;
        }
        ledc_channel_config_t ccfg = {
            .gpio_num = GPIO_LCD_BLK,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags.output_invert = 0,
            .deconfigure = false,
        };
        err = ledc_channel_config(&ccfg);
        if (err != ESP_OK) {
            return err;
        }
        s_backlight_ready = true;
    }
    uint32_t duty = (uint32_t)percent * 1023 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    s_info.brightness = percent;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 窗口 / 像素发送                                                     */
/* ------------------------------------------------------------------ */
/* GC9307 按大端接收 RGB565，而 ESP32 帧缓冲按小端存储；
 * 写入/发送前交换高低字节，否则 R/G/B 会整体轮换（红变蓝、绿变红、蓝变绿）。 */
static inline uint16_t lcd_color(uint16_t rgb565)
{
    return (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
}

static void lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t xs = (uint16_t)(x + s_info.x_offset);
    uint16_t ys = (uint16_t)(y + s_info.y_offset);
    uint16_t xe = (uint16_t)(xs + w - 1);
    uint16_t ye = (uint16_t)(ys + h - 1);
    uint8_t caset[4] = { (uint8_t)(xs >> 8), (uint8_t)xs, (uint8_t)(xe >> 8), (uint8_t)xe };
    uint8_t raset[4] = { (uint8_t)(ys >> 8), (uint8_t)ys, (uint8_t)(ye >> 8), (uint8_t)ye };
    esp_lcd_panel_io_tx_param(s_io, 0x2A, caset, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, raset, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2C, NULL, 0);
}

static esp_err_t lcd_write_pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *data)
{
    if (!s_io || !data || !w || !h) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The framebuffer normally lives in PSRAM.  SPI DMA must not read it
     * directly: CPU writes may still be in cache while DMA reads the stale
     * PSRAM contents (the screen then stays black).  Copy each chunk into
     * an internal DMA-capable bounce buffer first. */
    size_t chunk_limit = s_dma_bytes ? s_dma_bytes : (32 * 1024);
    uint32_t rows_per_chunk = (uint32_t)(chunk_limit / ((size_t)w * 2));
    if (rows_per_chunk < 1) {
        rows_per_chunk = 1;
    }
    if (rows_per_chunk > h) {
        rows_per_chunk = h;
    }
    for (uint32_t done = 0; done < h; done += rows_per_chunk) {
        uint32_t rows = h - done;
        if (rows > rows_per_chunk) {
            rows = rows_per_chunk;
        }
        lcd_set_window(x, (uint16_t)(y + done), w, (uint16_t)rows);
        while (s_color_done && xSemaphoreTake(s_color_done, 0) == pdTRUE) {
            /* drain stale completion */
        }
        const uint16_t *src = data + (size_t)done * w;
        size_t bytes = (size_t)w * rows * 2;
        if (s_dma_buf && bytes <= s_dma_bytes) {
            memcpy(s_dma_buf, src, bytes);
            src = s_dma_buf;
        }
        esp_err_t err = esp_lcd_panel_io_tx_color(s_io, -1, src, bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tx_color failed: %s", esp_err_to_name(err));
            return err;
        }
        if (s_color_done && xSemaphoreTake(s_color_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "color transfer timeout");
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 帧缓冲                                                              */
/* ------------------------------------------------------------------ */
static void fb_release(void)
{
    if (s_fb) {
        heap_caps_free(s_fb);
        s_fb = NULL;
    }
    s_info.have_fb = false;
}

static void fb_alloc(void)
{
    fb_release();
    size_t bytes = (size_t)s_info.width * s_info.height * 2;
    if (!bytes) {
        return;
    }
    /* 240x284 帧缓冲会占 136 KB；WatchShell 需要把内部 RAM 留给
     * Wi-Fi/TLS/音频队列，因此大缓冲优先放 PSRAM，小屏仍走内部 RAM。 */
    if (bytes <= 32 * 1024) {
        s_fb = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_fb) {
        s_fb = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_fb) {
        s_fb = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_fb) {
        memset(s_fb, 0, bytes);
        s_info.have_fb = true;
        ESP_LOGI(TAG, "framebuffer %ux%u (%u bytes)", s_info.width, s_info.height, (unsigned)bytes);
    } else {
        ESP_LOGW(TAG, "no framebuffer (%u bytes), fallback to solid fills only", (unsigned)bytes);
    }
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */
static void set_default_geometry(void)
{
    s_info.width = CONFIG_FT_LCD_WIDTH;
    s_info.height = CONFIG_FT_LCD_HEIGHT;
    s_info.x_offset = CONFIG_FT_LCD_X_OFFSET;
    s_info.y_offset = CONFIG_FT_LCD_Y_OFFSET;
}

esp_err_t ft_lcd_reinit(void)
{
    esp_err_t err = lcd_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_color_done) {
        s_color_done = xSemaphoreCreateBinary();
        if (!s_color_done) {
            return ESP_ERR_NO_MEM;
        }
    }
    err = lcd_io_create();
    if (err != ESP_OK) {
        return err;
    }

    fb_release();
    lcd_hw_reset();
    lcd_send_table(ft_lcd_gc9307_init, ft_lcd_gc9307_init_len);
    ft_lcd_set_rotation(s_info.rotation);
    esp_lcd_panel_io_tx_param(s_io, 0x29, NULL, 0);
    ft_delay_ms(50);

    fb_alloc();
    if (!s_dma_buf) {
        static const size_t dma_sizes[] = {32 * 1024, 16 * 1024, 8 * 1024};
        for (size_t i = 0; i < sizeof(dma_sizes) / sizeof(dma_sizes[0]); i++) {
            s_dma_buf = heap_caps_malloc(dma_sizes[i], MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (s_dma_buf) {
                s_dma_bytes = dma_sizes[i];
                break;
            }
        }
        if (!s_dma_buf) {
            ESP_LOGW(TAG, "no internal DMA bounce buffer; PSRAM framebuffer may stay black");
        }
    }
    s_info.initialized = true;
    ft_lcd_backlight(s_info.brightness ? s_info.brightness : 70);

    ESP_LOGI(TAG, "LCD %s %ux%u offset(%u,%u) %s dma=%u",
             ft_lcd_controller_name(),
             s_info.width, s_info.height, s_info.x_offset, s_info.y_offset,
             s_info.have_fb ? "fb" : "no-fb", (unsigned)s_dma_bytes);
    return ESP_OK;
}

esp_err_t ft_lcd_init(void)
{
    s_info.rotation = 0;
    s_info.spi_mode = 0;
    s_info.spi_hz = CONFIG_FT_LCD_SPI_MHZ * 1000 * 1000;
    s_info.brightness = CONFIG_FT_LCD_MAX_BRIGHTNESS > 100 ? 100 : CONFIG_FT_LCD_MAX_BRIGHTNESS;
    set_default_geometry();
    return ft_lcd_reinit();
}

esp_err_t ft_lcd_set_resolution(uint16_t width, uint16_t height, uint16_t x_offset, uint16_t y_offset)
{
    if (!width || !height) {
        return ESP_ERR_INVALID_ARG;
    }
    s_info.width = width;
    s_info.height = height;
    s_info.x_offset = x_offset;
    s_info.y_offset = y_offset;
    return ft_lcd_reinit();
}

esp_err_t ft_lcd_set_spi(uint8_t mode, uint32_t mhz)
{
    if (mode > 3 || mhz < 1 || mhz > 80) {
        return ESP_ERR_INVALID_ARG;
    }
    s_info.spi_mode = mode;
    s_info.spi_hz = mhz * 1000 * 1000;
    return ft_lcd_reinit();
}



/* ------------------------------------------------------------------ */
/* 填充 / 刷新                                                         */
/* ------------------------------------------------------------------ */
esp_err_t ft_lcd_fill(uint16_t color)
{
    if (!s_info.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_info.have_fb) {
        size_t n = (size_t)s_info.width * s_info.height;
        for (size_t i = 0; i < n; i++) {
            s_fb[i] = lcd_color(color);
        }
        return ft_lcd_flush();
    }
    /* 无帧缓冲：用 16 行缓冲逐块发送 */
    uint16_t rows = 16;
    size_t bytes = (size_t)s_info.width * rows * 2;
    uint16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < (size_t)s_info.width * rows; i++) {
        buf[i] = lcd_color(color);
    }
    esp_err_t err = ESP_OK;
    for (uint16_t y = 0; y < s_info.height; y += rows) {
        uint16_t rh = (uint16_t)((y + rows <= s_info.height) ? rows : (s_info.height - y));
        err = lcd_write_pixels(0, y, s_info.width, rh, buf);
        if (err != ESP_OK) {
            break;
        }
    }
    heap_caps_free(buf);
    return err;
}

esp_err_t ft_lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (!s_info.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_info.have_fb) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (x >= s_info.width || y >= s_info.height) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x + w > s_info.width) w = s_info.width - x;
    if (y + h > s_info.height) h = s_info.height - y;
    for (uint16_t row = 0; row < h; row++) {
        uint16_t *p = s_fb + (size_t)(y + row) * s_info.width + x;
        for (uint16_t col = 0; col < w; col++) {
            p[col] = lcd_color(color);
        }
    }
    return ESP_OK;
}

esp_err_t ft_lcd_flush_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!s_info.initialized || !s_info.have_fb) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x >= s_info.width || y >= s_info.height) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x + w > s_info.width) w = s_info.width - x;
    if (y + h > s_info.height) h = s_info.height - y;
    return lcd_write_pixels(x, y, w, h, s_fb + (size_t)y * s_info.width + x);
}

esp_err_t ft_lcd_flush(void)
{
    return ft_lcd_flush_region(0, 0, s_info.width, s_info.height);
}

/* ------------------------------------------------------------------ */
/* 绘图原语                                                            */
/* ------------------------------------------------------------------ */
void ft_lcd_draw_pixel(int x, int y, uint16_t color)
{
    if (!s_info.have_fb || x < 0 || y < 0 || x >= s_info.width || y >= s_info.height) {
        return;
    }
    s_fb[(size_t)y * s_info.width + x] = lcd_color(color);
}

void ft_lcd_draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!s_info.have_fb) {
        return;
    }
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    while (1) {
        ft_lcd_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void ft_lcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (!s_info.have_fb || !w || !h) {
        return;
    }
    int x1 = x + w - 1;
    int y1 = y + h - 1;
    ft_lcd_draw_line(x, y, x1, y, color);
    ft_lcd_draw_line(x, y1, x1, y1, color);
    ft_lcd_draw_line(x, y, x, y1, color);
    ft_lcd_draw_line(x1, y, x1, y1, color);
}

void ft_lcd_draw_text(uint16_t x, uint16_t y, const char *text, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if (!s_info.have_fb || !text || !scale) {
        return;
    }
    int cx = x;
    int cy = y;
    for (const char *p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n') {
            cx = x;
            cy += (FT_FONT_HEIGHT + 1) * scale;
            continue;
        }
        if (ch < FT_FONT_FIRST_CHAR || ch > FT_FONT_LAST_CHAR) {
            ch = ' ';
        }
        const unsigned char *glyph = &ft_font5x7[(ch - FT_FONT_FIRST_CHAR) * FT_FONT_WIDTH];
        for (int col = 0; col < FT_FONT_WIDTH; col++) {
            unsigned char bits = glyph[col];
            for (int row = 0; row < 7; row++) {
                uint16_t color = (bits & (1u << row)) ? fg : bg;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        ft_lcd_draw_pixel(cx + col * scale + sx, cy + row * scale + sy, color);
                    }
                }
            }
            /* 字形底部留空行，保持 5x7 + 1 行间距 */
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    ft_lcd_draw_pixel(cx + col * scale + sx, cy + 7 * scale + sy, bg);
                }
            }
        }
        /* 字符右侧 1 列背景 */
        for (int sy = 0; sy < FT_FONT_HEIGHT * scale; sy++) {
            ft_lcd_draw_pixel(cx + FT_FONT_WIDTH * scale, cy + sy, bg);
        }
        cx += (FT_FONT_WIDTH + 1) * scale;
        if (cx + FT_FONT_WIDTH * scale > s_info.width) {
            cx = x;
            cy += (FT_FONT_HEIGHT + 1) * scale;
        }
    }
}


/* ------------------------------------------------------------------ */
/* 演示图案                                                            */
/* ------------------------------------------------------------------ */
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_CYAN    0x07FF
#define C_MAGENTA 0xF81F
#define C_YELLOW  0xFFE0
#define C_GRAY    0x8410

void ft_lcd_show_bars(void)
{
    static const uint16_t colors[6] = { C_RED, C_GREEN, C_BLUE, C_CYAN, C_MAGENTA, C_YELLOW };
    static const char *names[6] = { "RED", "GREEN", "BLUE", "CYAN", "MAGENTA", "YELLOW" };

    if (!s_info.initialized) {
        return;
    }
    if (!s_info.have_fb) {
        for (int i = 0; i < 6; i++) {
            ft_lcd_fill(colors[i]);
            ft_delay_ms(600);
        }
        return;
    }
    ft_lcd_fill_rect(0, 0, s_info.width, s_info.height, C_BLACK);
    uint16_t bw = s_info.width / 6;
    for (int i = 0; i < 6; i++) {
        uint16_t x = (uint16_t)(i * bw);
        uint16_t w = (i == 5) ? (uint16_t)(s_info.width - x) : bw;
        ft_lcd_fill_rect(x, 0, w, (uint16_t)(s_info.height - 40), colors[i]);
        ft_lcd_draw_text((uint16_t)(x + 4), (uint16_t)(s_info.height - 32), names[i], C_WHITE, C_BLACK, 1);
    }
    ft_lcd_flush();
}

void ft_lcd_show_gradient(void)
{
    if (!s_info.initialized || !s_info.have_fb) {
        return;
    }
    for (uint16_t y = 0; y < s_info.height; y++) {
        for (uint16_t x = 0; x < s_info.width; x++) {
            uint8_t r = (uint8_t)(x * 255 / (s_info.width ? s_info.width - 1 : 1));
            uint8_t g = (uint8_t)(y * 255 / (s_info.height ? s_info.height - 1 : 1));
            uint8_t b = (uint8_t)(255 - r);
            s_fb[(size_t)y * s_info.width + x] = lcd_color(ft_rgb565(r, g, b));
        }
    }
    ft_lcd_flush();
}

void ft_lcd_show_grid(void)
{
    if (!s_info.initialized || !s_info.have_fb) {
        return;
    }
    ft_lcd_fill_rect(0, 0, s_info.width, s_info.height, C_BLACK);
    for (uint16_t x = 0; x < s_info.width; x += 20) {
        ft_lcd_draw_line(x, 0, x, s_info.height - 1, C_GRAY);
    }
    for (uint16_t y = 0; y < s_info.height; y += 20) {
        ft_lcd_draw_line(0, y, s_info.width - 1, y, C_GRAY);
    }
    ft_lcd_draw_rect(0, 0, s_info.width, s_info.height, C_WHITE);
    ft_lcd_draw_text(8, 8, "TOUCH TEST AREA", C_WHITE, C_BLACK, 2);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %ux%u", ft_lcd_controller_name(), s_info.width, s_info.height);
    ft_lcd_draw_text(8, 32, buf, C_CYAN, C_BLACK, 1);
    ft_lcd_flush();
}

void ft_lcd_show_pattern(void)
{
    if (!s_info.initialized || !s_info.have_fb) {
        return;
    }
    for (uint16_t y = 0; y < s_info.height; y += 20) {
        for (uint16_t x = 0; x < s_info.width; x += 20) {
            uint16_t color = (((x / 20) + (y / 20)) & 1) ? C_WHITE : C_BLACK;
            ft_lcd_fill_rect(x, y, 20, 20, color);
        }
    }
    ft_lcd_flush();
}

void ft_lcd_banner(const char *line1, const char *line2)
{
    if (!s_info.initialized) {
        return;
    }
    if (!s_info.have_fb) {
        ft_lcd_fill(C_BLACK);
        return;
    }
    ft_lcd_fill_rect(0, 0, s_info.width, s_info.height, C_BLACK);
    uint8_t scale = (s_info.width >= 300) ? 3 : 2;
    if (line1) {
        int tw = (int)strlen(line1) * (FT_FONT_WIDTH + 1) * scale;
        int x = (s_info.width > tw) ? (s_info.width - tw) / 2 : 4;
        ft_lcd_draw_text((uint16_t)x, (uint16_t)(s_info.height / 3), line1, C_WHITE, C_BLACK, scale);
    }
    if (line2) {
        int tw = (int)strlen(line2) * (FT_FONT_WIDTH + 1) * scale;
        int x = (s_info.width > tw) ? (s_info.width - tw) / 2 : 4;
        ft_lcd_draw_text((uint16_t)x, (uint16_t)(s_info.height / 3 + 40), line2, C_CYAN, C_BLACK, scale);
    }
    ft_lcd_flush();
}

