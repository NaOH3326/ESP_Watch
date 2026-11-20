#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the WatchShell UI.  Must be called from app_main(). */
esp_err_t watchshell_start(void);

#ifdef __cplusplus
}
#endif
