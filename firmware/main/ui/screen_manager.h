#pragma once
#include <stdbool.h>
#include "lvgl.h"

/**
 * Callback invoked when the user wakes the display from the clock screen
 * back to the dashboard. Can be NULL.
 */
typedef void (*screen_wake_cb_t)(void);

/**
 * Initialise screen sleep / wake management.
 *
 * Must be called after bsp_display_start_with_config() and after the first
 * call to ui_manager_init() so that the dashboard screen already exists as
 * the active LVGL screen.
 *
 * @param sleep_min   Inactivity timeout in minutes (e.g. 5).
 * @param wake_cb     Optional callback fired when returning to dashboard.
 */
void screen_manager_init(uint32_t sleep_min, screen_wake_cb_t wake_cb);

/**
 * Return the dashboard screen saved at init time.
 * Use this when you need to rebuild the UI regardless of current sleep state.
 */
lv_obj_t *screen_manager_get_dashboard(void);
