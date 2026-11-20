/**
 * @file  ft_audio.h
 * @brief I2S 全双工音频测试：麦克风 / 单音 / 回声
 */
#pragma once

#include "ft_board.h"

esp_err_t ft_audio_init(void);
esp_err_t ft_audio_deinit(void);
bool ft_audio_ready(void);

esp_err_t ft_audio_mic(int seconds);
esp_err_t ft_audio_tone(int seconds);
esp_err_t ft_audio_echo(int seconds, int gain_x10);
