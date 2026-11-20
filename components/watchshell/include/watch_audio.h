#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WS_AUDIO_RATE 16000

esp_err_t watch_audio_init(void);
void watch_audio_deinit(void);
bool watch_audio_ready(void);

void watch_audio_set_volume(int percent);
int watch_audio_volume(void);

/* Read mono PCM, selecting the I2S channel with the stronger mic signal. */
esp_err_t watch_audio_read(int16_t *mono, int frames, int timeout_ms);
/* Write mono PCM; `rate` may be 16k or 24k (linear resampled to 16k). */
esp_err_t watch_audio_write(const int16_t *mono, int frames, int rate);
esp_err_t watch_audio_tone(int freq_hz, int ms);
void watch_audio_silence(int ms);
