/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// Powering the keyboard off (System OFF), for every reason to:
//
// - Wired mode without USB (src/wired_off.c): USB or the mode switch wakes it.
// - A low battery: on battery, below CONFIG_RETHINK_LOW_BATTERY_OFF_MV in
//   LOW_READINGS readings in a row, CHECK_PERIOD apart. Only USB wakes it, so
//   a stray key can't drain the cell further. The voltage comes from the
//   zmk,battery device (framework-cb's fuel gauge), not the level, which
//   depends on the battery model.
// - Unused: on battery, no input from any device (keys, the stick, the
//   buttons) for CONFIG_RETHINK_IDLE_SLEEP_MIN. A key, USB or the mode
//   switch wakes it; the key that wakes it isn't typed.
//
// "power" shell commands show the state and try the cut-off and the sleep
// without waiting for them.

#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/poweroff.h>

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#include <rethink/power.h>

#if IS_ENABLED(CONFIG_RETHINK_ADAPTER)
#include <rethink/adapter.h>
#endif

#if IS_ENABLED(CONFIG_RETHINK_TRACKPOINT)
#include <rethink/trackpoint.h>
#endif

LOG_MODULE_REGISTER(rethink_power, LOG_LEVEL_INF);

// Framework's gpio-keys entries: mode switch pressed = Bluetooth
static const struct gpio_dt_spec mode_switch =
    GPIO_DT_SPEC_GET(DT_NODELABEL(protocol_switch), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_NODELABEL(pairing_button), gpios);

// GPIO outputs keep their level in System OFF, so every LED is switched off
#define LED_SPEC(label) GPIO_DT_SPEC_GET(DT_NODELABEL(label), gpios)

static const struct gpio_dt_spec leds[] = {
    LED_SPEC(led_r),  LED_SPEC(led_g),  LED_SPEC(led_b),  LED_SPEC(led_w1),
    LED_SPEC(led_w2), LED_SPEC(led_w3), LED_SPEC(led_w4),
};

FUNC_NORETURN void rethink_power_off(uint32_t wake) {
#if IS_ENABLED(CONFIG_RETHINK_BACKLIGHT)
    led_set_brightness(DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)), 0, 0);
#endif
#if IS_ENABLED(CONFIG_RETHINK_TRACKPOINT)
    // TP_3V3 (P3.12) would stay on through System OFF
    rethink_tp_power_off();
#endif
#if IS_ENABLED(CONFIG_RETHINK_ADAPTER)
    rethink_adapter_power_down();
#endif

    // An idle matrix waits on level interrupts, which arm the GPIO sense that
    // wakes the board from System OFF on a key press. Suspending disconnects
    // its pins instead (I2C for the adapter's, so before the scheduler is
    // locked).
    if (!(wake & RETHINK_WAKE_KEYS)) {
#define SUSPEND_MATRIX(node) pm_device_action_run(DEVICE_DT_GET(node), PM_DEVICE_ACTION_SUSPEND);
        DT_FOREACH_STATUS_OKAY(zmk_input_gpio_matrix, SUSPEND_MATRIX)
        DT_FOREACH_STATUS_OKAY(rethink_matrix, SUSPEND_MATRIX)
    }

    // Nothing may light an LED again from here on
    k_sched_lock();

    for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
        gpio_pin_set_dt(&leds[i], 0);
    }

    // A level interrupt arms the GPIO sense: the switch wakes the board when
    // it leaves its present position. VBUS wakes the nRF54LM20A without
    // configuration. The pairing button stays inert.
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_DISABLE);
    if (wake & RETHINK_WAKE_SWITCH) {
        gpio_pin_interrupt_configure_dt(&mode_switch, gpio_pin_get_dt(&mode_switch)
                                                          ? GPIO_INT_LEVEL_INACTIVE
                                                          : GPIO_INT_LEVEL_ACTIVE);
    } else {
        gpio_pin_interrupt_configure_dt(&mode_switch, GPIO_INT_DISABLE);
    }

    sys_poweroff();
}

#define LOW_MV CONFIG_RETHINK_LOW_BATTERY_OFF_MV
#define SLEEP_MIN CONFIG_RETHINK_IDLE_SLEEP_MIN

#if LOW_MV > 0
#define CHECK_PERIOD K_SECONDS(10)
#define LOW_READINGS 3

static const struct device *const battery = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
// The cut-off; "power lowbat" changes it until the next reset
static atomic_t low_mv = ATOMIC_INIT(LOW_MV);
// Last reading in mV, -1 for none
static atomic_t battery_mv = ATOMIC_INIT(-1);
static int low_count;

// -ENOTSUP: the battery device gives no voltage (an older framework-cb)
static int read_mv(void) {
    struct sensor_value val;
    int err = sensor_sample_fetch(battery);

    if (!err) {
        err = sensor_channel_get(battery, SENSOR_CHAN_GAUGE_VOLTAGE, &val);
    }
    return err ? err : val.val1 * 1000 + val.val2 / 1000;
}

static void battery_check(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_work, battery_check);

static void battery_check(struct k_work *work) {
    int mv = read_mv();

    if (mv == -ENOTSUP) {
        LOG_WRN("No battery voltage from %s: no low-battery cut-off", battery->name);
        return;
    }
    atomic_set(&battery_mv, mv < 0 ? -1 : mv);

    if (mv < 0 || zmk_usb_is_powered() || mv >= atomic_get(&low_mv)) {
        low_count = 0;
    } else if (++low_count >= LOW_READINGS) {
        LOG_WRN("Battery at %d mV, under %d mV: powering off. USB wakes it.", mv,
                (int)atomic_get(&low_mv));
        rethink_power_off(0);
    } else {
        LOG_WRN("Battery at %d mV, under %d mV (%d of %d readings)", mv,
                (int)atomic_get(&low_mv), low_count, LOW_READINGS);
    }
    k_work_schedule(&battery_work, CHECK_PERIOD);
}
#endif // LOW_MV > 0

#if SLEEP_MIN > 0
// "power sleep" changes it until the next reset
static atomic_t sleep_min = ATOMIC_INIT(SLEEP_MIN);
// Uptime (ms) of the last input, or of USB power going
static atomic_t last_use;

static void note_use(void) { atomic_set(&last_use, k_uptime_get_32()); }

static uint32_t unused_ms(void) { return k_uptime_get_32() - (uint32_t)atomic_get(&last_use); }

static uint32_t sleep_ms(void) { return (uint32_t)atomic_get(&sleep_min) * 60U * 1000U; }

static void sleep_check(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_check);

// Waits for the rest of the time unused; on USB power the board stays on
static void sleep_check(struct k_work *work) {
    uint32_t unused = unused_ms();

    if (sleep_ms() == 0) {
        return;
    }
    if (zmk_usb_is_powered()) {
        k_work_schedule(&sleep_work, K_MSEC(sleep_ms()));
        return;
    }
    if (unused >= sleep_ms()) {
        LOG_INF("Unused for %d min: sleeping. A key, USB or the mode switch wakes it.",
                (int)atomic_get(&sleep_min));
        rethink_power_off(RETHINK_WAKE_KEYS | RETHINK_WAKE_SWITCH);
    }
    k_work_schedule(&sleep_work, K_MSEC(sleep_ms() - unused));
}

// Any input device: the matrix, the stick, the buttons and the switch
static void input_used(struct input_event *evt, void *user_data) { note_use(); }

INPUT_CALLBACK_DEFINE(NULL, input_used, NULL);
#endif // SLEEP_MIN > 0

#if SLEEP_MIN > 0
// Unplugging USB starts the time unused afresh
static int usb_listener(const zmk_event_t *eh) {
    note_use();
    k_work_reschedule(&sleep_work, K_MSEC(sleep_ms()));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rethink_power, usb_listener);
ZMK_SUBSCRIPTION(rethink_power, zmk_usb_conn_state_changed);
#endif

static int power_init(void) {
#if LOW_MV > 0
    k_work_schedule(&battery_work, CHECK_PERIOD);
#endif
#if SLEEP_MIN > 0
    note_use();
    k_work_schedule(&sleep_work, K_MSEC(sleep_ms()));
#endif
    return 0;
}

SYS_INIT(power_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_SHELL)
static int cmd_stat(const struct shell *sh, size_t argc, char **argv) {
    shell_print(sh, "USB power: %s", zmk_usb_is_powered() ? "yes" : "no");
#if LOW_MV > 0
    int mv = atomic_get(&battery_mv);

    if (mv < 0) {
        shell_print(sh, "Battery: no reading");
    } else {
        shell_print(sh, "Battery: %d mV; off on battery under %d mV", mv,
                    (int)atomic_get(&low_mv));
    }
#endif
#if SLEEP_MIN > 0
    if (atomic_get(&sleep_min)) {
        shell_print(sh, "Unused for %u s; sleeps on battery after %d min", unused_ms() / 1000,
                    (int)atomic_get(&sleep_min));
    } else {
        shell_print(sh, "Sleep when unused: off");
    }
#endif
    return 0;
}

#if LOW_MV > 0
static int cmd_lowbat(const struct shell *sh, size_t argc, char **argv) {
    long mv = strtol(argv[1], NULL, 0);

    if (mv < 0 || mv > 4500) {
        shell_error(sh, "Expected 0-4500 mV");
        return -EINVAL;
    }
    atomic_set(&low_mv, mv);
    low_count = 0;
    shell_print(sh, "Off on battery under %ld mV until the next reset (%d readings, 10 s apart)",
                mv, LOW_READINGS);
    return 0;
}
#endif

#if SLEEP_MIN > 0
static int cmd_sleep(const struct shell *sh, size_t argc, char **argv) {
    long min = strtol(argv[1], NULL, 0);

    if (min < 0 || min > 1440) {
        shell_error(sh, "Expected 0-1440 minutes, 0 = never");
        return -EINVAL;
    }
    atomic_set(&sleep_min, min);
    note_use();
    k_work_reschedule(&sleep_work, K_MSEC(min ? sleep_ms() : 0));
    shell_print(sh, "Sleeps on battery after %ld min unused until the next reset", min);
    return 0;
}
#endif

static int cmd_off(const struct shell *sh, size_t argc, char **argv) {
    shell_print(sh, "Sleeping: a key, USB or the mode switch wakes it%s",
                zmk_usb_is_powered() ? " (with USB in, it may wake at once)" : "");
    k_msleep(100);
    rethink_power_off(RETHINK_WAKE_KEYS | RETHINK_WAKE_SWITCH);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    power_cmds,
    SHELL_CMD(stat, NULL, "USB power, battery voltage and cut-off, time unused", cmd_stat),
#if LOW_MV > 0
    SHELL_CMD_ARG(lowbat, NULL, "<mV>: low-battery cut-off until the next reset", cmd_lowbat, 2,
                  0),
#endif
#if SLEEP_MIN > 0
    SHELL_CMD_ARG(sleep, NULL, "<min>: sleep after this long unused until the next reset",
                  cmd_sleep, 2, 0),
#endif
    SHELL_CMD(off, NULL, "Sleep now, as when unused", cmd_off), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(power, &power_cmds, "Power off: low battery, sleep when unused", NULL);
#endif // CONFIG_SHELL
