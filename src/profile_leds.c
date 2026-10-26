/*
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define BLINK_PERIOD K_MSEC(250)
#define TIMEOUT_MS CONFIG_RETHINK_PROFILE_LEDS_TIMEOUT_MS

// White LEDs 1-4, active low. Driven as GPIOs; PWM20 is left uninitialized.
static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(led_w1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(led_w2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(led_w3), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(led_w4), gpios),
};

static uint8_t profile;
static bool blink_on;
static int64_t off_at;

static void show(bool on) {
    for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
        // Profiles past the fourth light all LEDs
        bool lit = on && (profile >= ARRAY_SIZE(leds) || i == profile);
        gpio_pin_set_dt(&leds[i], lit);
    }
}

static void update(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(update_work, update);

static void update(struct k_work *work) {
    if (k_uptime_get() >= off_at) {
        show(false);
        return;
    }

    if (zmk_ble_profile_is_open(profile)) {
        blink_on = !blink_on;
        show(blink_on);
        k_work_schedule(&update_work, BLINK_PERIOD);
    } else {
        show(true);
        k_work_schedule(&update_work, K_TIMEOUT_ABS_MS(off_at));
    }
}

static void start(uint8_t index) {
    profile = index;
    blink_on = false;
    off_at = k_uptime_get() + TIMEOUT_MS;
    k_work_reschedule(&update_work, K_NO_WAIT);
}

static int profile_leds_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *ev = as_zmk_ble_active_profile_changed(eh);

    if (ev) {
        start(ev->index);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rethink_profile_leds, profile_leds_listener);
ZMK_SUBSCRIPTION(rethink_profile_leds, zmk_ble_active_profile_changed);

static int profile_leds_init(void) {
    for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
        if (!gpio_is_ready_dt(&leds[i])) {
            return -ENODEV;
        }

        int err = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
        if (err) {
            LOG_ERR("Failed to configure white LED %d (%d)", i + 1, err);
            return err;
        }
    }

    return 0;
}

SYS_INIT(profile_leds_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_SETTINGS)
// Runs after settings_load(), once the stored active profile is known
static int profile_leds_commit(void) {
    start(zmk_ble_active_profile_index());
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rethink_profile_leds, "rethink_leds", NULL, NULL,
                               profile_leds_commit, NULL);
#endif
