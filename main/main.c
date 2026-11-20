#include <stdio.h>

#include "sdkconfig.h"

#if CONFIG_FACTORY_TEST_MODE
#include "factory_test.h"
#else
#include "watchshell.h"
#endif

void app_main(void)
{
#if CONFIG_FACTORY_TEST_MODE
    factory_test_start();
#else
    watchshell_start();
#endif
}
