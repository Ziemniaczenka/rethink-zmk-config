/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#define TIMEOUT K_MSEC(CONFIG_RETHINK_CONNECTED_LED_TIMEOUT_MS)

// Blue part of the RGB LED; framework-cb's status_led.c configures it off at boot
static const struct gpio_dt_spec blue = GPIO_DT_SPEC_GET(DT_NODELABEL(led_b), gpios);

static bool was_connected;

static void off(struct k_work *work) { gpio_pin_set_dt(&blue, 0); }

static K_WORK_DELAYABLE_DEFINE(off_work, off);

// Same test as framework-cb's status LED
static bool connected(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_get_conn_state() == ZMK_USB_CONN_HID) {
        return true;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
    if (zmk_ble_active_profile_is_connected()) {
        return true;
    }
#endif

    return false;
}

static int connected_led_listener(const zmk_event_t *eh) {
    bool now = connected();
    bool profile_changed = false;

#if IS_ENABLED(CONFIG_ZMK_BLE)
    static int profile = -1;
    const struct zmk_ble_active_profile_changed *ev = as_zmk_ble_active_profile_changed(eh);

    if (ev && ev->index != profile) {
        profile_changed = true;
        profile = ev->index;
    }
#endif

    if (now && (!was_connected || profile_changed)) {
        gpio_pin_set_dt(&blue, 1);
        k_work_reschedule(&off_work, TIMEOUT);
    } else if (!now) {
        k_work_cancel_delayable(&off_work);
        gpio_pin_set_dt(&blue, 0);
    }

    was_connected = now;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rethink_connected_led, connected_led_listener);

#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(rethink_connected_led, zmk_ble_active_profile_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(rethink_connected_led, zmk_usb_conn_state_changed);
#endif
