/*
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Framework's gpio-keys entry for the pairing button (P3.10, active low)
#define BUTTON DT_NODELABEL(pairing_button)

static void hold(struct k_work *work) {
    LOG_INF("Pairing button held: clearing profile %d", zmk_ble_active_profile_index());
    zmk_ble_clear_bonds();
}

static void tap(struct k_work *work) {
    LOG_INF("Pairing button tapped: next profile");
    zmk_ble_prof_next();
}

static K_WORK_DELAYABLE_DEFINE(hold_work, hold);
static K_WORK_DEFINE(tap_work, tap);

static void button_changed(struct input_event *evt, void *user_data) {
    if (evt->type != INPUT_EV_KEY || evt->code != DT_PROP(BUTTON, zephyr_code)) {
        return;
    }

    if (evt->value) {
        k_work_schedule(&hold_work, K_MSEC(CONFIG_RETHINK_PAIRING_BUTTON_HOLD_MS));
        return;
    }

    // Released before the hold fired
    if (k_work_delayable_is_pending(&hold_work)) {
        k_work_cancel_delayable(&hold_work);
        k_work_submit(&tap_work);
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(BUTTON)), button_changed, NULL);
