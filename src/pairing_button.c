/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <zmk/ble.h>

#if IS_ENABLED(CONFIG_RETHINK_BACKLIGHT)
#include <rethink/backlight.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Framework's gpio-keys entry for the pairing button (P3.10, active low)
#define BUTTON DT_NODELABEL(pairing_button)

#define TAP_WINDOW K_MSEC(CONFIG_RETHINK_PAIRING_BUTTON_TAP_WINDOW_MS)
#define HOLD_TIME K_MSEC(CONFIG_RETHINK_PAIRING_BUTTON_HOLD_MS)
#define RECOVERY_TIME                                                                              \
    K_MSEC(CONFIG_RETHINK_PAIRING_BUTTON_RECOVERY_MS - CONFIG_RETHINK_PAIRING_BUTTON_HOLD_MS)

BUILD_ASSERT(CONFIG_RETHINK_PAIRING_BUTTON_RECOVERY_MS == 0 ||
                 CONFIG_RETHINK_PAIRING_BUTTON_RECOVERY_MS > CONFIG_RETHINK_PAIRING_BUTTON_HOLD_MS,
             "The recovery hold must be longer than the clear hold");

static unsigned int taps;
static bool held;

// Profile actions are meaningless while the mode switch has Bluetooth off
static bool ble_on(const char *action) {
    if (!zmk_ble_is_enabled()) {
        LOG_INF("Pairing button: %s ignored, Bluetooth is off (mode switch)", action);
        return false;
    }

    return true;
}

// The factory MCUboot (Framework's daisy branch before c4a29622f1) only
// checks its button after a power-on or pin reset, so a soft reset boots the
// application again. A PMIC reset power-cycles the nRF instead.
#define PMIC DT_NODELABEL(npm1300)
#define USE_PMIC_RESET (DT_NODE_HAS_STATUS_OKAY(PMIC) && IS_ENABLED(CONFIG_MFD_NPM13XX))

static void recovery(struct k_work *work) {
    // Still held at reset, so MCUboot enters USB serial recovery
    LOG_WRN("Pairing button held: rebooting into the bootloader");
    k_msleep(100);

#if USE_PMIC_RESET
    int err = mfd_npm13xx_reset(DEVICE_DT_GET(PMIC));

    if (err == 0) {
        // Power is cut within a few ms
        k_msleep(500);
    }
    LOG_ERR("nPM1300 reset did not power-cycle the board (%d), soft reset instead", err);
    k_msleep(100);
#endif

    sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(recovery_work, recovery);

static void hold(struct k_work *work) {
    taps = 0;
    held = true;
    LOG_INF("Pairing button held: release to clear profile %d%s", zmk_ble_active_profile_index(),
            CONFIG_RETHINK_PAIRING_BUTTON_RECOVERY_MS ? ", keep holding to reboot into the bootloader"
                                                      : "");

    if (CONFIG_RETHINK_PAIRING_BUTTON_RECOVERY_MS) {
        k_work_schedule(&recovery_work, RECOVERY_TIME);
    }
}

// Runs once no further tap arrived within the tap window
static void taps_done(struct k_work *work) {
    unsigned int count = taps;

    taps = 0;

    switch (count) {
    case 1:
        if (ble_on("next profile")) {
            LOG_INF("Pairing button tapped: next profile");
            zmk_ble_prof_next();
        }
        break;
#if IS_ENABLED(CONFIG_RETHINK_BACKLIGHT)
    case 3:
        LOG_INF("Pairing button triple-tapped: unlocking backlight");
        rethink_backlight_unlock();
        break;
#endif
    default:
        LOG_INF("Pairing button tapped %u times: ignored", count);
        break;
    }
}

static K_WORK_DELAYABLE_DEFINE(hold_work, hold);
static K_WORK_DELAYABLE_DEFINE(taps_work, taps_done);

static void button_changed(struct input_event *evt, void *user_data) {
    if (evt->type != INPUT_EV_KEY || evt->code != DT_PROP(BUTTON, zephyr_code)) {
        return;
    }

    if (evt->value) {
        k_work_cancel_delayable(&taps_work);
        held = false;
        k_work_schedule(&hold_work, HOLD_TIME);
        return;
    }

    // Released after the hold time, before the reboot
    if (held) {
        held = false;
        k_work_cancel_delayable(&recovery_work);
        if (ble_on("clear profile")) {
            LOG_INF("Pairing button released: clearing profile %d",
                    zmk_ble_active_profile_index());
            zmk_ble_clear_bonds();
        }
        return;
    }

    // Released before the hold time
    if (k_work_delayable_is_pending(&hold_work)) {
        k_work_cancel_delayable(&hold_work);
        taps++;
        k_work_reschedule(&taps_work, TAP_WINDOW);
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(BUTTON)), button_changed, NULL);
