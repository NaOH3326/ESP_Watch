/**
 * @file  factory_test.h
 * @brief 工厂测试入口（组件可由 CONFIG_FACTORY_TEST_MODE 关闭）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化所有测试外设并启动交互式控制台（UART0 / USB-Serial-JTAG）
 *
 * 该函数不会返回：内部会启动 esp_console REPL 任务。
 */
void factory_test_start(void);

#ifdef __cplusplus
}
#endif
