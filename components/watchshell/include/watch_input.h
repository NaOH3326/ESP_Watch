#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "watch_types.h"

esp_err_t watch_input_init(void);
/* Fast boot: bring up keys/PMIC first, then touch after the first frame. */
esp_err_t watch_input_init_keys(void);
esp_err_t watch_input_init_touch(void);
bool watch_input_poll(ws_event_t *out);
bool watch_input_key_down(ws_key_t key);
