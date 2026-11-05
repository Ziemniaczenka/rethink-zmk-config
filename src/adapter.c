/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT rethink_adapter_ctrl

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rethink/adapter.h>

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#endif

LOG_MODULE_REGISTER(rethink_adapter, CONFIG_RETHINK_ADAPTER_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "Exactly one rethink,adapter-ctrl node");

#define EXPANDER DT_INST_PHANDLE(0, expander)

BUILD_ASSERT(DT_PROP(EXPANDER, zephyr_deferred_init),
             "The expander needs zephyr,deferred-init: it may be missing or unpowered at boot");

static const struct device *const expander = DEVICE_DT_GET(EXPANDER);

// JP10 2-3: U1 and its I2C pull-ups run from TP_3V3
#define EXPANDER_ON_TP_POWER IS_ENABLED(CONFIG_RETHINK_EXPANDER_SUPPLY_TP_3V3)

BUILD_ASSERT(!EXPANDER_ON_TP_POWER || DT_INST_NODE_HAS_PROP(0, tp_power_supply),
             "RETHINK_EXPANDER_SUPPLY_TP_3V3 needs tp-power-supply");

#if EXPANDER_ON_TP_POWER
static const struct device *const tp_power = DEVICE_DT_GET(DT_INST_PHANDLE(0, tp_power_supply));
#endif

// A closed JP1/JP2 ties the enable to its supply: leave the pin an input
#define HAS_PUMP_EN                                                                                \
    (DT_INST_NODE_HAS_PROP(0, pump_en_gpios) && !IS_ENABLED(CONFIG_RETHINK_PUMP_EN_TIED))
#define HAS_BOOST_EN                                                                               \
    (DT_INST_NODE_HAS_PROP(0, boost_en_gpios) && !IS_ENABLED(CONFIG_RETHINK_BOOST_EN_TIED))

#if HAS_PUMP_EN
static const struct gpio_dt_spec pump_en = GPIO_DT_SPEC_INST_GET(0, pump_en_gpios);
#endif

#if HAS_BOOST_EN
static const struct gpio_dt_spec boost_en = GPIO_DT_SPEC_INST_GET(0, boost_en_gpios);
#endif

#if DT_INST_NODE_HAS_PROP(0, bl_detect_gpios)
static const struct gpio_dt_spec bl_detect = GPIO_DT_SPEC_INST_GET(0, bl_detect_gpios);
#endif

// Absent LEDs keep a NULL port
#define LED_SPEC(prop) GPIO_DT_SPEC_INST_GET_OR(0, prop, {0})

static const struct gpio_dt_spec leds[RETHINK_LED_COUNT] = {
    [RETHINK_LED_PWR] = LED_SPEC(pwr_led_gpios),
    [RETHINK_LED_MUTE] = LED_SPEC(mute_led_gpios),
    [RETHINK_LED_MICMUTE] = LED_SPEC(micmute_led_gpios),
    [RETHINK_LED_CAPSLOCK] = LED_SPEC(capslock_led_gpios),
    [RETHINK_LED_NUMLOCK] = LED_SPEC(numlock_led_gpios),
};

static const char *const led_names[] = {"PWR/FnLock", "Mute", "MicMute", "CapsLock", "NumLock"};

static const char *const bl_detect_names[] = {"unknown", "not detected", "detected"};

static bool ready;
static enum rethink_bl_detect bl_state = RETHINK_BL_UNKNOWN;

bool rethink_adapter_ready(void) { return ready; }

enum rethink_bl_detect rethink_adapter_bl_detect(void) { return bl_state; }

const char *rethink_bl_detect_name(enum rethink_bl_detect state) {
    return state < ARRAY_SIZE(bl_detect_names) ? bl_detect_names[state] : "?";
}

const char *rethink_adapter_led_name(enum rethink_led led) {
    return led < RETHINK_LED_COUNT ? led_names[led] : "?";
}

int rethink_adapter_boost_set(bool on) {
#if HAS_BOOST_EN
    if (!ready) {
        return -ENODEV;
    }

    int err = gpio_pin_set_dt(&boost_en, on);
    if (err) {
        LOG_ERR("Failed to set BOOST_EN (%d)", err);
    } else {
        LOG_INF("BOOST_EN %s", on ? "on" : "off");
    }

    return err;
#else
    return -ENOTSUP;
#endif
}

#if DT_INST_NODE_HAS_PROP(0, bl_detect_gpios)
// Pull-up on, sample, pull-up off: keeps the pin passive afterwards
static enum rethink_bl_detect sample_bl_detect(void) {
    int err = gpio_pin_configure(bl_detect.port, bl_detect.pin, GPIO_INPUT | GPIO_PULL_UP);
    if (err) {
        LOG_ERR("Failed to configure KBD_BL_DTCT (%d)", err);
        return RETHINK_BL_UNKNOWN;
    }

    k_busy_wait(1000);
    int level = gpio_pin_get_raw(bl_detect.port, bl_detect.pin);
    gpio_pin_configure(bl_detect.port, bl_detect.pin, GPIO_INPUT);

    if (level < 0) {
        LOG_ERR("Failed to read KBD_BL_DTCT (%d)", level);
        return RETHINK_BL_UNKNOWN;
    }

    return level == 0 ? RETHINK_BL_PRESENT : RETHINK_BL_ABSENT;
}
#endif

// Sink when lit, high-impedance otherwise. Physical levels, flags ignored.
static int led_apply(const struct gpio_dt_spec *spec, bool on) {
    return gpio_pin_configure(spec->port, spec->pin, on ? GPIO_OUTPUT_LOW : GPIO_INPUT);
}

int rethink_adapter_led_set(enum rethink_led led, bool on) {
    if (!ready) {
        return -ENODEV;
    }

    if (led >= RETHINK_LED_COUNT || leds[led].port == NULL) {
        return -ENOTSUP;
    }

    int err = led_apply(&leds[led], on);
    if (err) {
        LOG_ERR("Failed to set %s LED (%d)", led_names[led], err);
    }

    return err;
}

#if IS_ENABLED(CONFIG_RETHINK_ADAPTER_LED_SELFTEST)

#define SELFTEST_STEP K_MSEC(300)

static void selftest(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(selftest_work, selftest);
static size_t selftest_step;

// Lights each LED alone, then all of them, then turns them off. Steps for
// LEDs the keyboard does not have only show as log lines.
static void selftest(struct k_work *work) {
    size_t count = RETHINK_LED_COUNT;

    for (size_t i = 0; i < count; i++) {
        if (leds[i].port != NULL) {
            rethink_adapter_led_set(i, selftest_step == count || i == selftest_step);
        }
    }

    if (selftest_step < count) {
        LOG_INF("LED self-test: %s%s", led_names[selftest_step],
                leds[selftest_step].port ? "" : " (not on this keyboard)");
    } else if (selftest_step == count) {
        LOG_INF("LED self-test: all");
    }

    if (selftest_step++ <= count) {
        k_work_schedule(&selftest_work, SELFTEST_STEP);
    }
}

#endif

void rethink_adapter_power_down(void) {
    if (!ready) {
        return;
    }

#if IS_ENABLED(CONFIG_RETHINK_ADAPTER_LED_SELFTEST)
    k_work_cancel_delayable(&selftest_work);
#endif

    for (size_t i = 0; i < RETHINK_LED_COUNT; i++) {
        if (leds[i].port != NULL) {
            led_apply(&leds[i], false);
        }
    }

#if HAS_PUMP_EN
    gpio_pin_set_dt(&pump_en, 0);
#endif
#if HAS_BOOST_EN
    gpio_pin_set_dt(&boost_en, 0);
#endif
#if EXPANDER_ON_TP_POWER
    regulator_disable(tp_power);
#endif
}

static int adapter_init(void) {
    int err;

#if EXPANDER_ON_TP_POWER
    err = regulator_enable(tp_power);
    if (err) {
        LOG_ERR("Failed to enable TP_3V3 (%d)", err);
        return 0;
    }
    k_msleep(DT_INST_PROP(0, power_on_delay_ms));
#endif

    err = device_init(expander);
    if (err) {
        LOG_WRN("ReThink adapter not found: %s did not answer (%d). Running without it.",
                expander->name, err);
#if EXPANDER_ON_TP_POWER
        regulator_disable(tp_power);
#endif
        return 0;
    }

#if HAS_PUMP_EN
    err = gpio_pin_configure_dt(&pump_en, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure PUMP_EN (%d)", err);
        return 0;
    }
#endif

#if HAS_BOOST_EN
    err = gpio_pin_configure_dt(&boost_en, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure BOOST_EN (%d)", err);
        return 0;
    }
#endif

#if DT_INST_NODE_HAS_PROP(0, bl_detect_gpios)
    bl_state = sample_bl_detect();
#endif

    for (size_t i = 0; i < RETHINK_LED_COUNT; i++) {
        if (leds[i].port == NULL) {
            continue;
        }

        err = led_apply(&leds[i], false);
        if (err) {
            LOG_ERR("Failed to configure %s LED (%d)", led_names[i], err);
            return 0;
        }
    }

    ready = true;
    LOG_INF("ReThink adapter found (%s), backlit keyboard %s", expander->name,
            bl_detect_names[bl_state]);

#if IS_ENABLED(CONFIG_RETHINK_ADAPTER_LED_SELFTEST)
    k_work_schedule(&selftest_work, K_MSEC(500));
#endif

    return 0;
}

// After I2C, the expander driver and regulator-fixed; before rethink,backlight
SYS_INIT(adapter_init, POST_KERNEL, CONFIG_RETHINK_ADAPTER_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

// HID LED report bits
#define HID_INDICATOR_NUM_LOCK BIT(0)
#define HID_INDICATOR_CAPS_LOCK BIT(1)

static int indicators_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);

    if (ev && ready) {
        rethink_adapter_led_set(RETHINK_LED_CAPSLOCK, ev->indicators & HID_INDICATOR_CAPS_LOCK);
        rethink_adapter_led_set(RETHINK_LED_NUMLOCK, ev->indicators & HID_INDICATOR_NUM_LOCK);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rethink_adapter_indicators, indicators_listener);
ZMK_SUBSCRIPTION(rethink_adapter_indicators, zmk_hid_indicators_changed);

#endif
