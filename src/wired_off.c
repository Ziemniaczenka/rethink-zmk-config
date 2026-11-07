/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#if IS_ENABLED(CONFIG_RETHINK_ADAPTER)
#include <rethink/adapter.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Framework's gpio-keys entries: mode switch pressed = Bluetooth
#define SWITCH DT_NODELABEL(protocol_switch)
#define BUTTON DT_NODELABEL(pairing_button)

#define DELAY K_MSEC(CONFIG_RETHINK_WIRED_OFF_DELAY_MS)

static const struct gpio_dt_spec mode_switch = GPIO_DT_SPEC_GET(SWITCH, gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON, gpios);

// GPIO outputs keep their level in System OFF, so every LED is switched off
#define LED_SPEC(label) GPIO_DT_SPEC_GET(DT_NODELABEL(label), gpios)

static const struct gpio_dt_spec leds[] = {
    LED_SPEC(led_r),  LED_SPEC(led_g),  LED_SPEC(led_b),  LED_SPEC(led_w1),
    LED_SPEC(led_w2), LED_SPEC(led_w3), LED_SPEC(led_w4),
};

static bool should_be_off(void) {
    return gpio_pin_get_dt(&mode_switch) == 0 && !zmk_usb_is_powered();
}

static void power_off(struct k_work *work) {
    // The switch or USB may have changed while waiting
    if (!should_be_off()) {
        return;
    }

    LOG_INF("Wired mode without USB: powering off. USB or the mode switch wakes it.");

#if IS_ENABLED(CONFIG_RETHINK_BACKLIGHT)
    led_set_brightness(DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)), 0, 0);
#endif
#if IS_ENABLED(CONFIG_RETHINK_ADAPTER)
    rethink_adapter_power_down();
#endif

    // An idle matrix waits on level interrupts, which would let any key press
    // wake the board. Suspending disconnects its pins (I2C for the adapter's,
    // so before the scheduler is locked).
#define SUSPEND_MATRIX(node) pm_device_action_run(DEVICE_DT_GET(node), PM_DEVICE_ACTION_SUSPEND);
    DT_FOREACH_STATUS_OKAY(zmk_input_gpio_matrix, SUSPEND_MATRIX)
    DT_FOREACH_STATUS_OKAY(rethink_matrix, SUSPEND_MATRIX)

    // Nothing may light an LED again from here on
    k_sched_lock();

    for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
        gpio_pin_set_dt(&leds[i], 0);
    }

    // A level interrupt arms the GPIO sense used to wake from System OFF. VBUS
    // wakes the nRF54LM20A without configuration. The pairing button stays
    // inert, like the keys.
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_DISABLE);
    gpio_pin_interrupt_configure_dt(&mode_switch, GPIO_INT_LEVEL_ACTIVE);

    sys_poweroff();
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
