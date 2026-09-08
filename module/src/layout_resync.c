/* ZMK binding for the layout resync module. Task 4 implements this. */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(layout_resync, CONFIG_ZMK_LOG_LEVEL);

static int layout_resync_init(void) {
    LOG_DBG("layout resync present");
    return 0;
}

SYS_INIT(layout_resync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
