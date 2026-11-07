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
#include <rethink/expander.h>

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#endif

LOG_MODULE_REGISTER(rethink_adapter, CONFIG_RETHINK_ADAPTER_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "Exactly one rethink,adapter-ctrl node");

#define EXPANDER DT_INST_PHANDLE(0, expander)

// JP10 2-3: U1 and its I2C pull-ups run from TP_3V3
#define EXPANDER_ON_TP_POWER IS_ENABLED(CONFIG_RETHINK_EXPANDER_SUPPLY_TP_3V3)

BUILD_ASSERT(!EXPANDER_ON_TP_POWER || DT_INST_NODE_HAS_PROP(0, tp_power_supply),
             "RETHINK_EXPANDER_SUPPLY_TP_3V3 needs tp-power-supply");

#if EXPANDER_ON_TP_POWER
static const struct device *const tp_power = DEVICE_DT_GET(DT_INST_PHANDLE(0, tp_power_supply));
#endif

// U1 pin numbers from the GPIO specifiers; levels are physical, flags only
// give the idle pulls
#define NO_PIN 0xff
#define PIN_OR_NONE(prop)                                                                          \
    COND_CODE_1(DT_INST_NODE_HAS_PROP(0, prop), (DT_INST_GPIO_PIN(0, prop)), (NO_PIN))

// A closed JP1/JP2 ties the enable to its supply: leave the pin an input
#define HAS_PUMP_EN                                                                                \
    (DT_INST_NODE_HAS_PROP(0, pump_en_gpios) && !IS_ENABLED(CONFIG_RETHINK_PUMP_EN_TIED))
#define HAS_BOOST_EN                                                                               \
    (DT_INST_NODE_HAS_PROP(0, boost_en_gpios) && !IS_ENABLED(CONFIG_RETHINK_BOOST_EN_TIED))

static const uint8_t pump_en = PIN_OR_NONE(pump_en_gpios);
static const uint8_t boost_en = PIN_OR_NONE(boost_en_gpios);
static const uint8_t bl_detect = PIN_OR_NONE(bl_detect_gpios);

#define IDLE_MODE(flags)                                                                           \
    (((flags) & GPIO_PULL_DOWN)                                                                    \
         ? RETHINK_EXP_IN_PULL_DOWN                                                                \
         : (((flags) & GPIO_PULL_UP) ? RETHINK_EXP_IN_PULL_UP : RETHINK_EXP_IN))

#define IDLE_PIN(node_id, prop, idx)                                                               \
    {DT_GPIO_PIN_BY_IDX(node_id, prop, idx), IDLE_MODE(DT_GPIO_FLAGS_BY_IDX(node_id, prop, idx))},

static const struct {
    uint8_t pin;
    uint8_t mode;
} idle_pins[] = {
#if DT_INST_NODE_HAS_PROP(0, idle_gpios)
    DT_INST_FOREACH_PROP_ELEM(0, idle_gpios, IDLE_PIN)
#endif
};

static const uint8_t leds[RETHINK_LED_COUNT] = {
    [RETHINK_LED_PWR] = PIN_OR_NONE(pwr_led_gpios),
    [RETHINK_LED_MUTE] = PIN_OR_NONE(mute_led_gpios),
    [RETHINK_LED_MICMUTE] = PIN_OR_NONE(micmute_led_gpios),
    [RETHINK_LED_CAPSLOCK] = PIN_OR_NONE(capslock_led_gpios),
    [RETHINK_LED_NUMLOCK] = PIN_OR_NONE(numlock_led_gpios),
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
    if (!HAS_BOOST_EN) {
        return -ENOTSUP;
    }
    if (!ready) {
        return -ENODEV;
    }

    int err = rethink_exp_configure(boost_en, on ? RETHINK_EXP_OUT_HIGH : RETHINK_EXP_OUT_LOW);
    if (err) {
        LOG_ERR("Failed to set BOOST_EN (%d)", err);
    } else {
        LOG_INF("BOOST_EN %s", on ? "on" : "off");
    }

    return err;
}

// Sampled with the pull-up; park_pins() then sets its resting state
static enum rethink_bl_detect sample_bl_detect(void) {
    uint16_t levels;

    int err = rethink_exp_configure(bl_detect, RETHINK_EXP_IN_PULL_UP);
    if (!err) {
        k_busy_wait(1000);
        err = rethink_exp_read(&levels);
    }
    if (err) {
        LOG_ERR("Failed to read KBD_BL_DTCT (%d)", err);
        return RETHINK_BL_UNKNOWN;
    }

    return levels & BIT(bl_detect) ? RETHINK_BL_ABSENT : RETHINK_BL_PRESENT;
}

// Sink when lit. Unlit, the pull-up holds the pin at U1's supply, which is
// also the LEDs' supply: no current through the LED, and the input doesn't
// sit ~1.2 V below VDD(P), where U1 draws extra current (datasheet 9.1).
// park_pins() leaves the latch low and the pull-up on, so switching is a
// direction change: one 1-byte write.
static int led_apply(uint8_t pin, bool on) {
    return rethink_exp_update(RETHINK_EXP_CONFIG, BIT(pin), on ? 0 : BIT(pin));
}

// Every pin nothing drives gets a defined level. A floating input draws up
// to 80 µA (U1 datasheet, ΔIDD); an adapter without a keyboard drew 0.5 mA
// from LED_3V3, which stays on in System OFF. A pull-up costs current only
// while something holds its pin low: KBD_BL_DTCT keeps its pull-up unless a
// backlit keyboard grounds it. Status LEDs go off.
static int park_pins(void) {
    int err = 0;

    for (size_t i = 0; i < ARRAY_SIZE(idle_pins) && !err; i++) {
        err = rethink_exp_configure(idle_pins[i].pin, idle_pins[i].mode);
        if (err) {
            LOG_ERR("Failed to park expander pin %u (%d)", idle_pins[i].pin, err);
        }
    }

    if (!err && bl_detect != NO_PIN) {
        err = rethink_exp_configure(bl_detect, bl_state == RETHINK_BL_PRESENT
                                                   ? RETHINK_EXP_IN
                                                   : RETHINK_EXP_IN_PULL_UP);
        if (err) {
            LOG_ERR("Failed to configure KBD_BL_DTCT (%d)", err);
        }
    }

    for (size_t i = 0; i < RETHINK_LED_COUNT && !err; i++) {
        if (leds[i] == NO_PIN) {
            continue;
        }

        err = rethink_exp_update(RETHINK_EXP_OUTPUT, BIT(leds[i]), 0);
        if (!err) {
            err = rethink_exp_configure(leds[i], RETHINK_EXP_IN_PULL_UP);
        }
        if (err) {
            LOG_ERR("Failed to configure %s LED (%d)", led_names[i], err);
        }
    }

    return err;
}

void rethink_adapter_park(void) {
    if (ready) {
        park_pins();
    }
}

int rethink_adapter_led_set(enum rethink_led led, bool on) {
    if (!ready) {
        return -ENODEV;
    }

    if (led >= RETHINK_LED_COUNT || leds[led] == NO_PIN) {
        return -ENOTSUP;
    }

    int err = led_apply(leds[led], on);
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
        if (leds[i] != NO_PIN) {
            rethink_adapter_led_set(i, selftest_step == count || i == selftest_step);
        }
    }

    if (selftest_step < count) {
        LOG_INF("LED self-test: %s%s", led_names[selftest_step],
                leds[selftest_step] != NO_PIN ? "" : " (not on this keyboard)");
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

    park_pins();

    if (HAS_PUMP_EN) {
        rethink_exp_configure(pump_en, RETHINK_EXP_OUT_LOW);
    }
    if (HAS_BOOST_EN) {
        rethink_exp_configure(boost_en, RETHINK_EXP_OUT_LOW);
    }
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

    err = rethink_exp_init();
    if (err) {
        LOG_WRN("ReThink adapter not found: %s did not answer (%d). Running without it.",
                DT_NODE_FULL_NAME(EXPANDER), err);
#if EXPANDER_ON_TP_POWER
        regulator_disable(tp_power);
#endif
        return 0;
    }

    if (HAS_PUMP_EN) {
        err = rethink_exp_configure(pump_en, RETHINK_EXP_OUT_LOW);
        if (err) {
            LOG_ERR("Failed to configure PUMP_EN (%d)", err);
            return 0;
        }
    }

    if (HAS_BOOST_EN) {
        err = rethink_exp_configure(boost_en, RETHINK_EXP_OUT_LOW);
        if (err) {
            LOG_ERR("Failed to configure BOOST_EN (%d)", err);
            return 0;
        }
    }

    if (bl_detect != NO_PIN) {
        bl_state = sample_bl_detect();
    }

    if (park_pins()) {
        return 0;
    }

    ready = true;
    LOG_INF("ReThink adapter found (%s), backlit keyboard %s", DT_NODE_FULL_NAME(EXPANDER),
            bl_detect_names[bl_state]);

#if IS_ENABLED(CONFIG_RETHINK_ADAPTER_LED_SELFTEST)
    k_work_schedule(&selftest_work, K_MSEC(500));
#endif

    return 0;
}

// After I2C and regulator-fixed; before rethink,backlight
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
