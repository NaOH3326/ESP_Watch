#pragma once

#include <stdbool.h>

void watch_script_init(void);
bool watch_script_execute(const char *path);
void watch_script_stop(void);
bool watch_script_busy(void);
const char *watch_script_current(void);
