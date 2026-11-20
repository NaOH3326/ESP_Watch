#include "watch_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../main/board_pins.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "WATCH_AUDIO";
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static bool s_ready;
static int s_volume = 60;
static SemaphoreHandle_t s_lock;

static void apply_gain(int16_t *samples, int n)
{
    int v = s_volume;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    for (int i = 0; i < n; i++) {
        samples[i] = (int16_t)((int32_t)samples[i] * v / 100);
    }
}

esp_err_t watch_audio_init(void)
{
    if (s_ready) return ESP_OK;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WS_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_I2S_SCK,
            .ws = GPIO_I2S_WS,
            .dout = GPIO_I2S_DIN,
            .din = GPIO_I2S_SD,
            .invert_flags = { false, false, false },
        },
    };
    if ((err = i2s_channel_init_std_mode(s_tx, &cfg)) != ESP_OK ||
        (err = i2s_channel_init_std_mode(s_rx, &cfg)) != ESP_OK ||
        (err = i2s_channel_enable(s_tx)) != ESP_OK ||
        (err = i2s_channel_enable(s_rx)) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init: %s", esp_err_to_name(err));
        watch_audio_deinit();
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "I2S %d Hz stereo ready", WS_AUDIO_RATE);
    return ESP_OK;
}

void watch_audio_deinit(void)
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
}

bool watch_audio_ready(void) { return s_ready; }

void watch_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
}

int watch_audio_volume(void) { return s_volume; }

static esp_err_t write_stereo(const int16_t *mono, int frames)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    int16_t buf[480 * 2];
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > 480) n = 480;
        for (int i = 0; i < n; i++) {
            buf[i * 2] = mono[done + i];
            buf[i * 2 + 1] = mono[done + i];
        }
        apply_gain(buf, n * 2);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, buf, (size_t)n * 2 * sizeof(int16_t),
                                          &written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) return err;
        done += n;
    }
    return ESP_OK;
}

esp_err_t watch_audio_read(int16_t *mono, int frames, int timeout_ms)
{
    if (!s_ready || !mono || frames <= 0) return ESP_ERR_INVALID_ARG;
    int16_t buf[480 * 2];
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > 480) n = 480;
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, buf, (size_t)n * 2 * sizeof(int16_t),
                                         &got, pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK) return err;
        int got_frames = (int)(got / (2 * sizeof(int16_t)));
        /* The MEMS mic is wired to the left channel on this board, but some
         * boards/assembly variants put it on the right channel.  Pick the
         * channel with more energy per DMA block instead of assuming left;
         * with a silent right channel this still selects left. */
        int64_t left_energy = 0;
        int64_t right_energy = 0;
        for (int i = 0; i < got_frames; i++) {
            int32_t l = buf[i * 2];
            int32_t r = buf[i * 2 + 1];
            left_energy += l < 0 ? -l : l;
            right_energy += r < 0 ? -r : r;
        }
        bool use_right = right_energy > left_energy;
        for (int i = 0; i < got_frames; i++) {
            mono[done + i] = buf[i * 2 + (use_right ? 1 : 0)];
        }
        done += got_frames;
        if (got_frames == 0) break;
    }
    return done == frames ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t write_resampled(const int16_t *mono, int frames, int rate)
{
    if (rate == WS_AUDIO_RATE || rate <= 0) {
        return write_stereo(mono, frames);
    }
    int out_frames = (int)((int64_t)frames * WS_AUDIO_RATE / rate);
    if (out_frames <= 0) return ESP_OK;
    int16_t *out = malloc((size_t)out_frames * sizeof(int16_t));
    if (!out) return ESP_ERR_NO_MEM;
    for (int i = 0; i < out_frames; i++) {
        float pos = (float)i * rate / WS_AUDIO_RATE;
        int i0 = (int)pos;
        int i1 = i0 + 1 < frames ? i0 + 1 : frames - 1;
        float f = pos - i0;
        out[i] = (int16_t)(mono[i0] * (1.0f - f) + mono[i1] * f);
    }
    esp_err_t err = write_stereo(out, out_frames);
    free(out);
    return err;
}

esp_err_t watch_audio_write(const int16_t *mono, int frames, int rate)
{
    if (!s_ready) {
        esp_err_t err = watch_audio_init();
        if (err != ESP_OK) return err;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = write_resampled(mono, frames, rate);
    if (s_lock) xSemaphoreGive(s_lock);
    return err;
}

esp_err_t watch_audio_tone(int freq_hz, int ms)
{
    if (freq_hz <= 0 || ms <= 0) return ESP_ERR_INVALID_ARG;
    int total = WS_AUDIO_RATE * ms / 1000;
    int16_t block[480];
    int phase = 0;
    while (total > 0) {
        int n = total > 480 ? 480 : total;
        for (int i = 0; i < n; i++) {
            float a = 2.0f * 3.14159265f * freq_hz * phase / WS_AUDIO_RATE;
            block[i] = (int16_t)(sinf(a) * 9000.0f);
            phase++;
        }
        esp_err_t err = watch_audio_write(block, n, WS_AUDIO_RATE);
        if (err != ESP_OK) return err;
        total -= n;
    }
    return ESP_OK;
}

void watch_audio_silence(int ms)
{
    int16_t zero[480] = {0};
    int total = WS_AUDIO_RATE * ms / 1000;
    while (total > 0) {
        int n = total > 480 ? 480 : total;
        watch_audio_write(zero, n, WS_AUDIO_RATE);
        total -= n;
    }
}
