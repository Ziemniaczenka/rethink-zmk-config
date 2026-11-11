/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// Wired mode without USB: off after CONFIG_RETHINK_WIRED_OFF_DELAY_MS, until
// USB or the mode switch (src/power.c)

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#include <rethink/power.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Framework's gpio-keys entry: mode switch pressed = Bluetooth
#define SWITCH DT_NODELABEL(protocol_switch)

#define DELAY K_MSEC(CONFIG_RETHINK_WIRED_OFF_DELAY_MS)

static const struct gpio_dt_spec mode_switch = GPIO_DT_SPEC_GET(SWITCH, gpios);

static bool should_be_off(void) {
    return gpio_pin_get_dt(&mode_switch) == 0 && !zmk_usb_is_powered();
}

static void power_off(struct k_work *work) {
    // The switch or USB may have changed while waiting
    if (!should_be_off()) {
        return;
    }

    LOG_INF("Wired mode without USB: powering off. USB or the mode switch wakes it.");
    // Keys and the pairing button stay inert, so stray presses don't drain
    // the battery
    rethink_power_off(RETHINK_WAKE_SWITCH);
}

static K_WORK_DELAYABLE_DEFINE(off_work, power_off);

static void evaluate(void) {
    if (should_be_off()) {
        // Keeps a pending countdown running
        k_work_schedule(&off_work, DELAY);
    } else {
        k_work_cancel_delayable(&off_work);
    }
}

static int usb_listener(const zmk_event_t *eh) {
    evaluate();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rethink_wired_off, usb_listener);
ZMK_SUBSCRIPTION(rethink_wired_off, zmk_usb_conn_state_changed);

static void switch_moved(struct input_event *evt, void *user_data) {
    if (evt->type == INPUT_EV_KEY && evt->code == DT_PROP(SWITCH, zephyr_code)) {
        evaluate();
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(SWITCH)), switch_moved, NULL);

// USB reports VBUS shortly after boot; the delay covers it
static int wired_off_init(void) {
    evaluate();
    return 0;
}

SYS_INIT(wired_off_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
