/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// Powering the keyboard off (System OFF, or the nPM1300's ship mode), for
// every reason to:
//
// - Wired mode without USB (src/wired_off.c): USB or the mode switch wakes it.
// - A low battery: on battery, below CONFIG_RETHINK_LOW_BATTERY_OFF_MV in
//   LOW_READINGS readings in a row, CHECK_PERIOD apart. Ship mode
//   (CONFIG_RETHINK_LOW_BATTERY_SHIP_MODE): everything off, BUCK2 included;
//   only USB wakes it, so a stray key can't drain the cell further. The
//   voltage comes from the zmk,battery device (framework-cb's fuel gauge),
//   not the level, which depends on the battery model.
// - Unused: on battery, no input from any device (keys, the stick, the
//   buttons) for CONFIG_RETHINK_IDLE_SLEEP_MIN. A key, USB or the mode
//   switch wakes it; the key that wakes it isn't typed.
//
// The same battery readings keep BUCK2, the 3.3 V rail, out of dropout: at
// 3.0 V under CONFIG_RETHINK_BUCK2_LOW_MV, at 3.3 V again over
// CONFIG_RETHINK_BUCK2_HIGH_MV, both changes with headroom. A change without
// headroom brings a state that draws about 3 mA more until the cell is above
// about 3.35 V or USB is plugged in: src/pmic.c makes the boot's change and
// restarts BUCK2 after it, and each reading here (20 ms after a change)
// restarts a BUCK2 found in that state.
//
// No access to the nPM1300 right before System OFF: a read of BUCK2's voltage
// there let the system timer (GRTC) wake the nRF54L again at once (reset
// cause "clock"), every 3 s in wired mode, though every compare channel was
// disabled. So BUCK2 isn't lowered on the way into System OFF, and a quiet
// time with the battery check and the pmic sampling stopped comes first.
//
// "power" shell commands show the state and try the cut-off and the sleep
// without waiting for them.

#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/regulator.h>
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

#if IS_ENABLED(CONFIG_RETHINK_PMIC)
#include <rethink/pmic.h>
#endif

#if IS_ENABLED(CONFIG_RETHINK_ADAPTER)
#include <rethink/adapter.h>
#endif

#if IS_ENABLED(CONFIG_RETHINK_TRACKPOINT)
#include <rethink/trackpoint.h>
#endif

LOG_MODULE_REGISTER(rethink_power, LOG_LEVEL_INF);

#define LOW_MV CONFIG_RETHINK_LOW_BATTERY_OFF_MV
#define SLEEP_MIN CONFIG_RETHINK_IDLE_SLEEP_MIN
#define BUCK2_LOW_MV CONFIG_RETHINK_BUCK2_LOW_MV
#define BUCK2_HIGH_MV CONFIG_RETHINK_BUCK2_HIGH_MV
// Battery readings, for the cut-off or BUCK2
#define BATTERY_CHECKS (LOW_MV > 0 || BUCK2_LOW_MV > 0)

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

#if BUCK2_LOW_MV > 0
BUILD_ASSERT(BUCK2_HIGH_MV > BUCK2_LOW_MV, "RETHINK_BUCK2_HIGH_MV must be above RETHINK_BUCK2_LOW_MV");

#define BUCK2_LOW_UV 3000000
#define BUCK2_FULL_UV 3300000

static const struct device *const buck2 = DEVICE_DT_GET(DT_NODELABEL(npm1300_buck2));
// The thresholds; "power buck2" changes them until the next reset, 0 0 =
// leave BUCK2 as it is
static atomic_t buck2_low_mv = ATOMIC_INIT(BUCK2_LOW_MV);
static atomic_t buck2_high_mv = ATOMIC_INIT(BUCK2_HIGH_MV);
// BUCK2's setting as last read or set, 0 before the first read: the battery
// check then only talks to the nPM1300 when it changes
static int32_t buck2_uv;

// mv: the battery reading that called for it, for the log. True if it wrote
// a new setting.
static bool buck2_set(int32_t uv, int mv) {
    if (buck2_uv == 0 && regulator_get_voltage(buck2, &buck2_uv) != 0) {
        buck2_uv = 0;
    }
    if (buck2_uv == uv) {
        return false;
    }

    int err = regulator_set_voltage(buck2, uv, uv);

    if (err) {
        LOG_WRN("BUCK2 not set to %d mV (%d)", uv / 1000, err);
        buck2_uv = 0;
    } else {
        buck2_uv = uv;
        LOG_INF("Battery at %d mV: BUCK2 at %d mV", mv, uv / 1000);
    }
    return true;
}

static bool buck2_follow(int mv) {
    int low = atomic_get(&buck2_low_mv);

    if (low == 0) {
        return false;
    }
    if (mv < low) {
        return buck2_set(BUCK2_LOW_UV, mv);
    }
    if (mv > atomic_get(&buck2_high_mv)) {
        return buck2_set(BUCK2_FULL_UV, mv);
    }
    return false;
}
#endif // BUCK2_LOW_MV > 0

static void battery_stop(void);

FUNC_NORETURN void rethink_power_off(uint32_t wake) {
    // Quiet time: the last access to the nPM1300 well before System OFF
    battery_stop();
#if IS_ENABLED(CONFIG_RETHINK_PMIC)
    rethink_pmic_quiet();
#endif
    k_msleep(200);

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

#if IS_ENABLED(CONFIG_RETHINK_LOW_BATTERY_SHIP_MODE)
    // Cuts every rail at once, the nRF's too; USB power starts the nPM1300
    // again. If it returns, System OFF follows.
    if (wake & RETHINK_POWER_SHIP) {
        int err = regulator_parent_ship_mode(DEVICE_DT_GET(DT_NODELABEL(npm1300_regulators)));

        k_msleep(100);
        LOG_WRN("Ship mode didn't cut the power (%d): System OFF instead", err);
    }
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

#if BATTERY_CHECKS
#define CHECK_PERIOD K_SECONDS(10)
// The fuel gauge has no reading in the first second or so after boot
#define FIRST_READING_MS 15000

static const struct device *const battery = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
// Last reading in mV, -1 for none
static atomic_t battery_mv = ATOMIC_INIT(-1);

#if LOW_MV > 0
#define LOW_READINGS 3

// The cut-off; "power lowbat" changes it until the next reset
static atomic_t low_mv = ATOMIC_INIT(LOW_MV);
static int low_count;
#endif

// -ENOTSUP: the battery device gives no voltage (an older framework-cb);
// -EAGAIN: no reading (yet), or no cell
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
        LOG_WRN("No battery voltage from %s: no low-battery cut-off, BUCK2 left as it is",
                battery->name);
        return;
    }
    atomic_set(&battery_mv, mv < 0 ? -1 : mv);
    if (mv < 0) {
        k_work_schedule(&battery_work,
                        k_uptime_get() < FIRST_READING_MS ? K_SECONDS(1) : CHECK_PERIOD);
        return;
    }

#if BUCK2_LOW_MV > 0
    if (buck2_follow(mv)) {
        // For the check below to see the new setting's state
        k_msleep(20);
    }
#endif
#if IS_ENABLED(CONFIG_RETHINK_PMIC)
    rethink_pmic_buck2_check();
#endif

#if LOW_MV > 0
    if (zmk_usb_is_powered() || mv >= atomic_get(&low_mv)) {
        low_count = 0;
    } else if (++low_count >= LOW_READINGS) {
        LOG_WRN("Battery at %d mV, under %d mV: powering off. USB wakes it.", mv,
                (int)atomic_get(&low_mv));
        rethink_power_off(RETHINK_POWER_SHIP);
    } else {
        LOG_WRN("Battery at %d mV, under %d mV (%d of %d readings)", mv,
                (int)atomic_get(&low_mv), low_count, LOW_READINGS);
    }
#endif
    k_work_schedule(&battery_work, CHECK_PERIOD);
}

static void battery_stop(void) { k_work_cancel_delayable(&battery_work); }
#else
static void battery_stop(void) {}
#endif // BATTERY_CHECKS

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

#if SLEEP_MIN > 0 || BATTERY_CHECKS
static int usb_listener(const zmk_event_t *eh) {
#if SLEEP_MIN > 0
    // Unplugging USB starts the time unused afresh
    note_use();
    k_work_reschedule(&sleep_work, K_MSEC(sleep_ms()));
#endif
#if BATTERY_CHECKS
    // Without USB, BUCK2 runs from the cell: its own voltage soon, not the
    // one measured while charging
    k_work_reschedule(&battery_work, K_SECONDS(2));
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rethink_power, usb_listener);
ZMK_SUBSCRIPTION(rethink_power, zmk_usb_conn_state_changed);
#endif

static int power_init(void) {
#if BATTERY_CHECKS
    // Early, for BUCK2's voltage after a reset
    k_work_schedule(&battery_work, K_SECONDS(1));
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
#if BATTERY_CHECKS
    int mv = atomic_get(&battery_mv);

    if (mv < 0) {
        shell_print(sh, "Battery: no reading");
    } else {
        shell_print(sh, "Battery: %d mV", mv);
    }
#endif
#if LOW_MV > 0
    shell_print(sh, "Off on battery under %d mV%s", (int)atomic_get(&low_mv),
                IS_ENABLED(CONFIG_RETHINK_LOW_BATTERY_SHIP_MODE) ? " (ship mode)" : "");
#endif
#if BUCK2_LOW_MV > 0
    int32_t uv;

    if (regulator_get_voltage(buck2, &uv) == 0) {
        if (atomic_get(&buck2_low_mv)) {
            shell_print(sh, "BUCK2: %d mV (3000 under %d mV, 3300 over %d mV)", uv / 1000,
                        (int)atomic_get(&buck2_low_mv), (int)atomic_get(&buck2_high_mv));
        } else {
            shell_print(sh, "BUCK2: %d mV (not switched)", uv / 1000);
        }
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

#if BUCK2_LOW_MV > 0
static int cmd_buck2(const struct shell *sh, size_t argc, char **argv) {
    long low = strtol(argv[1], NULL, 0);
    long high = strtol(argv[2], NULL, 0);

    if (!(low == 0 && high == 0) && (low < 3000 || high > 4500 || high <= low)) {
        shell_error(sh, "Expected <down> <up> in mV, 3000-4500, up above down; 0 0 = off");
        return -EINVAL;
    }
    atomic_set(&buck2_low_mv, low);
    atomic_set(&buck2_high_mv, high);
    k_work_reschedule(&battery_work, K_NO_WAIT);
    if (low) {
        shell_print(sh, "BUCK2 at 3.0 V under %ld mV, 3.3 V over %ld mV until the next reset", low,
                    high);
    } else {
        shell_print(sh, "BUCK2 left as it is until the next reset");
    }
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
    SHELL_CMD(stat, NULL, "USB power, battery, cut-off, BUCK2, time unused", cmd_stat),
#if LOW_MV > 0
    SHELL_CMD_ARG(lowbat, NULL, "<mV>: low-battery cut-off until the next reset", cmd_lowbat, 2,
                  0),
#endif
#if BUCK2_LOW_MV > 0
    SHELL_CMD_ARG(buck2, NULL,
                  "<down mV> <up mV>: BUCK2 at 3.0 V under down, 3.3 V over up, until the next "
                  "reset; 0 0: leave it",
                  cmd_buck2, 3, 0),
#endif
#if SLEEP_MIN > 0
    SHELL_CMD_ARG(sleep, NULL, "<min>: sleep after this long unused until the next reset",
                  cmd_sleep, 2, 0),
#endif
    SHELL_CMD(off, NULL, "Sleep now, as when unused", cmd_off), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(power, &power_cmds, "Power off: low battery, sleep when unused", NULL);
#endif // CONFIG_SHELL
