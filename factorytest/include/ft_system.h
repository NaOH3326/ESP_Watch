/**
 * @file  ft_system.h
 * @brief 芯片 / Flash / PSRAM / USB 信息与测试
 */
#pragma once

#include "ft_board.h"

void ft_system_print_info(void);
void ft_flash_print_info(void);
void ft_usb_print_info(void);
bool ft_flash_check(uint32_t *id, uint32_t *size);
bool ft_usb_connected(void);
