#pragma once

#include <stdbool.h>

typedef enum {
    WS_POWEROFF_ACTIVE = 0,   /* user requested: next boot is normal */
    WS_POWEROFF_IDLE,         /* idle timeout: next boot is fast */
    WS_POWEROFF_LOW_BATTERY,  /* warning finished: next boot is normal */
} ws_poweroff_reason_t;

/* Consume the one-shot fast-boot hint left by an idle shutdown. */
bool watch_power_consume_fast_boot(void);

void watch_power_init(void);
void watch_power_note_activity(void);
void watch_power_poll(void);

int watch_power_idle_seconds(void);
void watch_power_set_idle_seconds(int seconds);

bool watch_power_low_batt_active(void);
int watch_power_low_batt_mv(void);
int watch_power_low_batt_percent(void);
int watch_power_low_batt_countdown(void);

void watch_power_off(ws_poweroff_reason_t reason);
