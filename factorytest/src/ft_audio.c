/**
 * @file  ft_audio.c
 * @brief I2S 全双工回声：MAX98357A 功放 + MSM261S4030H0R 数字麦克风
 *
 * 硬件：BCLK=GPIO6, WS=GPIO8, DOUT=GPIO5, DIN(GPIO7)=麦克风数据。
 * 同一控制器同时申请 TX/RX 通道即构成全双工，共用 BCLK/WS。
 * 麦克风 L/R 脚接地 -> 数据在左声道；输出左右声道写相同数据，
 * 避免 MAX98357A SD_MODE 声道模式不确定导致无声。
 */
#include "ft_audio.h"

#include <math.h>

#include "driver/i2s_std.h"
#include "esp_timer.h"

static const char *TAG = "FT_AUDIO";

#define FT_SAMPLE_RATE   16000
#define FT_FRAMES        480

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static bool s_ready;

bool ft_audio_ready(void)
{
    return s_ready;
}

esp_err_t ft_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_I2S_SCK,
            .ws = GPIO_I2S_WS,
            .dout = GPIO_I2S_DIN,
            .din = GPIO_I2S_SD,
            .invert_flags = { false, false, false },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tx init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        goto fail;
    }
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        goto fail;
    }
    s_ready = true;
    ESP_LOGI(TAG, "I2S full-duplex ready: %d Hz, stereo 16-bit", FT_SAMPLE_RATE);
    return ESP_OK;

fail:
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    return err;
}

esp_err_t ft_audio_deinit(void)
{
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    s_ready = false;
    return ESP_OK;
}

static void print_level(int rms, int peak)
{
    const int bars = 24;
    int n = (rms * bars) / 8000;
    if (n > bars) {
        n = bars;
    }
    printf("  mic rms=%5d peak=%5d [", rms, peak);
    for (int i = 0; i < bars; i++) {
        putchar(i < n ? '#' : '.');
    }
    printf("]\n");
}

esp_err_t ft_audio_mic(int seconds)
{
    if (!s_ready && ft_audio_init() != ESP_OK) {
        return ESP_FAIL;
    }
    if (seconds <= 0) {
        seconds = 5;
    }
    int16_t buf[FT_FRAMES * 2];
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    int64_t next_print = esp_timer_get_time();
    while (esp_timer_get_time() < end) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(500));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
            return err;
        }
        if (!got) {
            continue;
        }
        int frames = (int)(got / (2 * sizeof(int16_t)));
        int64_t sum = 0;
        int peak = 0;
        for (int i = 0; i < frames; i++) {
            int v = buf[i * 2];   /* 左声道 = 麦克风 */
            sum += (int64_t)v * v;
            if (v < 0) {
                v = -v;
            }
            if (v > peak) {
                peak = v;
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_print) {
            int rms = frames ? (int)sqrt((double)sum / frames) : 0;
            print_level(rms, peak);
            next_print = now + 500000;
        }
    }
    return ESP_OK;
}

/* 16 点正弦表 = 16 kHz / 1 kHz */
static const int16_t s_sine16[16] = {
    0, 3135, 5793, 7568, 8192, 7568, 5793, 3135,
    0, -3135, -5793, -7568, -8192, -7568, -5793, -3135,
};

esp_err_t ft_audio_tone(int seconds)
{
    if (!s_ready && ft_audio_init() != ESP_OK) {
        return ESP_FAIL;
    }
    if (seconds <= 0) {
        seconds = 3;
    }
    int16_t frame[16 * 2];
    for (int i = 0; i < 16; i++) {
        frame[i * 2] = s_sine16[i];
        frame[i * 2 + 1] = s_sine16[i];
    }
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    while (esp_timer_get_time() < end) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, frame, sizeof(frame), &written, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tone write failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    int16_t silence[16 * 2] = {0};
    size_t written = 0;
    i2s_channel_write(s_tx, silence, sizeof(silence), &written, pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t ft_audio_echo(int seconds, int gain_x10)
{
    if (!s_ready && ft_audio_init() != ESP_OK) {
        return ESP_FAIL;
    }
    if (seconds <= 0) {
        seconds = 10;
    }
    if (gain_x10 <= 0) {
        gain_x10 = CONFIG_FT_ECHO_GAIN_X10;
    }
    if (gain_x10 > 20) {
        gain_x10 = 20;
    }
    int16_t in[FT_FRAMES * 2];
    int16_t out[FT_FRAMES * 2];
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    int64_t next_print = esp_timer_get_time();
    printf("  echo %d s, gain x%.1f (注意声学回授)\n", seconds, gain_x10 / 10.0f);
    while (esp_timer_get_time() < end) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, in, sizeof(in), &got, pdMS_TO_TICKS(500));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            return err;
        }
        if (!got) {
            continue;
        }
        int frames = (int)(got / (2 * sizeof(int16_t)));
        int64_t sum = 0;
        for (int i = 0; i < frames; i++) {
            int32_t v = ((int32_t)in[i * 2] * gain_x10) / 10;
            if (v > 32767) {
                v = 32767;
            }
            if (v < -32768) {
                v = -32768;
            }
            out[i * 2] = (int16_t)v;
            out[i * 2 + 1] = (int16_t)v;
            sum += (int64_t)in[i * 2] * in[i * 2];
        }
        size_t written = 0;
        err = i2s_channel_write(s_tx, out, (size_t)frames * 2 * sizeof(int16_t), &written,
                                pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            return err;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_print) {
            int rms = frames ? (int)sqrt((double)sum / frames) : 0;
            printf("  echo in-rms=%d\n", rms);
            next_print = now + 1000000;
        }
    }
    int16_t silence[FT_FRAMES * 2] = {0};
    size_t written = 0;
    i2s_channel_write(s_tx, silence, sizeof(silence), &written, pdMS_TO_TICKS(100));
    return ESP_OK;
}

