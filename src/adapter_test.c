/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// "adapter" shell commands for bringing up an adapter without a keyboard:
// supplies and enables one at a time, expander pins, the INT line, and a test
// for shorts between the lines that run to the keyboard connectors.

#define DT_DRV_COMPAT rethink_adapter_ctrl

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/shell/shell.h>

#include <rethink/adapter.h>

#define EXPANDER DT_INST_PHANDLE(0, expander)
#define BACKLIGHT DT_NODELABEL(rethink_backlight)
#define HAS_BACKLIGHT DT_NODE_HAS_STATUS_OKAY(BACKLIGHT)

BUILD_ASSERT(DT_INST_NODE_HAS_PROP(0, tp_power_supply), "The adapter tests need tp-power-supply");

static const struct device *const expander = DEVICE_DT_GET(EXPANDER);
static const struct i2c_dt_spec expander_i2c = I2C_DT_SPEC_GET(EXPANDER);
static const struct device *const tp_power = DEVICE_DT_GET(DT_INST_PHANDLE(0, tp_power_supply));

// Control Board TP_INT: the expander's open-drain INT, R3 pull-up on the adapter
static const struct gpio_dt_spec int_pin = {DEVICE_DT_GET(DT_NODELABEL(gpio3)), 6, 0};

#if HAS_BACKLIGHT
static const struct gpio_dt_spec vled_switch = GPIO_DT_SPEC_GET(BACKLIGHT, enable_gpios);
static const struct pwm_dt_spec pwm = PWM_DT_SPEC_GET(BACKLIGHT);
#define SWITCH_BUCK1 IS_ENABLED(CONFIG_RETHINK_BACKLIGHT_SUPPLY_BUCK1)
#if SWITCH_BUCK1
static const struct device *const buck1 = DEVICE_DT_GET(DT_PHANDLE(BACKLIGHT, vin_supply));
#endif
static bool vled_on;
#endif

// PCAL6416A registers
#define REG_INPUT 0x00
#define REG_INT_MASK 0x4a

// U1 pins 0-15 are P0_0..P1_7 (ReThink_keyboard docs/pinouts/io_expander.md)
#define PIN_PUMP_EN 0
#define PIN_BOOST_EN 1
#define PIN_HOTKEY 2
#define PIN_PWRSWITCH 3
#define PIN_KBD_BL_DTCT 15

static const char *const pin_names[16] = {
    "PUMP_EN", "BOOST_EN", "HOTKEY",  "PWRSWITCH", "DRV15",      "DRV16",      "DRV17",      "OUT1",
    "TP1_RST", "TP2_RST",  "LEDPWR",  "LEDMUTE",   "LEDMICMUTE", "LEDCPSLOCK", "LEDNUMLOCK", "KBD_BL_DTCT",
};

// Pulled low by a key or a backlit keyboard; read with U1's 100k pull-ups
static const gpio_pin_t input_pins[] = {PIN_HOTKEY, PIN_PWRSWITCH, PIN_KBD_BL_DTCT};
#define INPUT_MASK (BIT(PIN_HOTKEY) | BIT(PIN_PWRSWITCH) | BIT(PIN_KBD_BL_DTCT))

static bool parse_on_off(const struct shell *sh, const char *arg, bool *on) {
    if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0) {
        *on = arg[1] == 'n';
        return true;
    }

    shell_error(sh, "Expected on or off, not %s", arg);
    return false;
}

static bool check_expander(const struct shell *sh) {
    if (!rethink_adapter_ready()) {
        shell_error(sh, "The expander (%s) did not answer at boot", expander->name);
        return false;
    }
    return true;
}

static int int_level(void) {
    gpio_pin_configure_dt(&int_pin, GPIO_INPUT);
    return gpio_pin_get_raw(int_pin.port, int_pin.pin);
}

static void configure_inputs(void) {
    for (size_t i = 0; i < ARRAY_SIZE(input_pins); i++) {
        gpio_pin_configure(expander, input_pins[i], GPIO_INPUT | GPIO_PULL_UP);
    }
    k_busy_wait(1000);
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv) {
    gpio_port_value_t pins;

    shell_print(sh, "TP_3V3 (P3.12): %s", regulator_is_enabled(tp_power) ? "on" : "off");
#if HAS_BACKLIGHT
    shell_print(sh, "VLED_BL: %s by 'adapter vled'", vled_on ? "on" : "off");
#endif
    shell_print(sh, "INT (P3.06): %s", int_level() ? "high" : "low (asserted)");

    if (!check_expander(sh)) {
        return -ENODEV;
    }

    int err = gpio_port_get_raw(expander, &pins);
    if (err) {
        shell_error(sh, "Failed to read the expander (%d)", err);
        return err;
    }

    shell_print(sh, "Expander pin levels:");
    for (size_t i = 0; i < ARRAY_SIZE(pin_names); i++) {
        shell_fprintf(sh, SHELL_NORMAL, "%s%s=%d", i % 8 ? " " : "  ", pin_names[i],
                      (pins >> i) & 1);
        if (i % 8 == 7) {
            shell_fprintf(sh, SHELL_NORMAL, "\n");
        }
    }

    return 0;
}

static int cmd_tp(const struct shell *sh, size_t argc, char **argv) {
    bool on;

    if (!parse_on_off(sh, argv[1], &on)) {
        return -EINVAL;
    }

    // U2's EN must stay within its supply (TP_3V3 + 0.3 V): pump off first
    if (!on && !IS_ENABLED(CONFIG_RETHINK_PUMP_EN_TIED) && device_is_ready(expander)) {
        gpio_pin_configure(expander, PIN_PUMP_EN, GPIO_OUTPUT_LOW);
    }

    // TP_3V3 is off from boot (regulator-boot-off); others may hold it too
    if (on != regulator_is_enabled(tp_power)) {
        int err = on ? regulator_enable(tp_power) : regulator_disable(tp_power);
        if (err) {
            shell_error(sh, "Failed to switch TP_3V3 (%d)", err);
            return err;
        }
    }

    shell_print(sh, "TP_3V3 %s", regulator_is_enabled(tp_power) ? "on" : "off");
    if (!on && IS_ENABLED(CONFIG_RETHINK_EXPANDER_SUPPLY_TP_3V3)) {
        shell_warn(sh, "The expander runs from TP_3V3 (JP10 2-3) and no longer answers");
    }

    return 0;
}

// PUMP_EN and BOOST_EN, unless a closed JP1/JP2 ties them to their supply
static int set_enable(const struct shell *sh, const char *arg, gpio_pin_t pin, bool tied) {
    bool on;

    if (!parse_on_off(sh, arg, &on) || !check_expander(sh)) {
        return -EINVAL;
    }

    if (tied) {
        shell_error(sh, "%s is tied to its supply by a jumper (config/rethink.conf)",
                    pin_names[pin]);
        return -ENOTSUP;
    }

    int err = gpio_pin_configure(expander, pin, on ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW);
    if (err) {
        shell_error(sh, "Failed to set %s (%d)", pin_names[pin], err);
        return err;
    }

    shell_print(sh, "%s %s, pin reads %d", pin_names[pin], on ? "high" : "low",
                gpio_pin_get_raw(expander, pin));
    return 0;
}

static int cmd_pump(const struct shell *sh, size_t argc, char **argv) {
    // LM2775: EN at most VIN + 0.3 V, and VIN is TP_3V3
    if (strcmp(argv[1], "on") == 0 && !regulator_is_enabled(tp_power)) {
        shell_error(sh, "TP_3V3 is off: PUMP_EN above it exceeds U2's rating. adapter tp on first");
        return -EPERM;
    }

    return set_enable(sh, argv[1], PIN_PUMP_EN, IS_ENABLED(CONFIG_RETHINK_PUMP_EN_TIED));
}

static int cmd_boost(const struct shell *sh, size_t argc, char **argv) {
    if (!IS_ENABLED(CONFIG_RETHINK_BOOST_FITTED)) {
        shell_error(sh, "U4 is not fitted (CONFIG_RETHINK_BOOST_FITTED=n)");
        return -ENOTSUP;
    }

    int err = set_enable(sh, argv[1], PIN_BOOST_EN, IS_ENABLED(CONFIG_RETHINK_BOOST_EN_TIED));
#if HAS_BACKLIGHT
    if (!err && !vled_on && strcmp(argv[1], "on") == 0) {
        shell_print(sh, "BL_5V also needs VLED_BL: adapter vled on");
    }
#endif
    return err;
}

static int cmd_vled(const struct shell *sh, size_t argc, char **argv) {
#if HAS_BACKLIGHT
    bool on;
    int err = 0;

    if (!parse_on_off(sh, argv[1], &on)) {
        return -EINVAL;
    }

    if (on != vled_on) {
#if SWITCH_BUCK1
        if (on) {
            err = regulator_enable(buck1);
        }
#endif
        if (!err) {
            err = gpio_pin_configure_dt(&vled_switch,
                                        on ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
        }
#if SWITCH_BUCK1
        if (!on && !err) {
            err = regulator_disable(buck1);
        }
#endif
        if (err) {
            shell_error(sh, "Failed to switch VLED_BL (%d)", err);
            return err;
        }
        vled_on = on;
    }

    shell_print(sh, "VLED_BL %s (P3.11%s)", on ? "on" : "off",
                SWITCH_BUCK1 ? " and BUCK1" : "; VBAT or VSYS source");
    return 0;
#else
    shell_error(sh, "This shield has no backlight");
    return -ENOTSUP;
#endif
}

static int cmd_pwm(const struct shell *sh, size_t argc, char **argv) {
#if HAS_BACKLIGHT
    char *end;
    long duty = strtol(argv[1], &end, 10);

    if (*end != '\0' || duty < 0 || duty > 100) {
        shell_error(sh, "Expected a duty cycle of 0-100, not %s", argv[1]);
        return -EINVAL;
    }

    if (duty > CONFIG_RETHINK_BACKLIGHT_MAX_DUTY) {
        shell_warn(sh, "Limited to CONFIG_RETHINK_BACKLIGHT_MAX_DUTY (%d %%)",
                   CONFIG_RETHINK_BACKLIGHT_MAX_DUTY);
        duty = CONFIG_RETHINK_BACKLIGHT_MAX_DUTY;
    }

    int err = pwm_set_pulse_dt(&pwm, (uint32_t)((uint64_t)pwm.period * duty / 100));
    if (err) {
        shell_error(sh, "Failed to set PWM (%d)", err);
        return err;
    }

    shell_print(sh, "PWM on P1.17: %ld %% at %u Hz, averages %ld mV on a multimeter", duty,
                (uint32_t)(NSEC_PER_SEC / pwm.period), 3300 * duty / 100);
    return 0;
#else
    shell_error(sh, "This shield has no backlight");
    return -ENOTSUP;
#endif
}

static int cmd_pin(const struct shell *sh, size_t argc, char **argv) {
    static const struct {
        const char *arg;
        gpio_flags_t flags;
        const char *what;
    } modes[] = {
        {"0", GPIO_OUTPUT_LOW, "driven low"},
        {"1", GPIO_OUTPUT_HIGH, "driven high"},
        {"z", GPIO_INPUT, "released"},
        {"u", GPIO_INPUT | GPIO_PULL_UP, "released with 100k pull-up"},
    };
    size_t pin, mode;

    for (pin = 0; pin < ARRAY_SIZE(pin_names); pin++) {
        if (strcasecmp(argv[1], pin_names[pin]) == 0) {
            break;
        }
    }
    for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
        if (strcmp(argv[2], modes[mode].arg) == 0) {
            break;
        }
    }

    if (pin == ARRAY_SIZE(pin_names) || mode == ARRAY_SIZE(modes)) {
        shell_error(sh, "Usage: adapter pin <name> 0|1|z|u, names as in 'adapter status'");
        return -EINVAL;
    }
    if (pin == PIN_PUMP_EN || pin == PIN_BOOST_EN) {
        shell_error(sh, "Use adapter pump / adapter boost");
        return -EINVAL;
    }
    if (!check_expander(sh)) {
        return -ENODEV;
    }

    int err = gpio_pin_configure(expander, pin, modes[mode].flags);
    if (err) {
        shell_error(sh, "Failed to configure %s (%d)", pin_names[pin], err);
        return err;
    }

    k_busy_wait(1000);
    shell_print(sh, "%s %s, reads %d", pin_names[pin], modes[mode].what,
                gpio_pin_get_raw(expander, pin));
    return 0;
}

static int cmd_in(const struct shell *sh, size_t argc, char **argv) {
    if (!check_expander(sh)) {
        return -ENODEV;
    }

    configure_inputs();
    for (size_t i = 0; i < ARRAY_SIZE(input_pins); i++) {
        int level = gpio_pin_get_raw(expander, input_pins[i]);
        shell_print(sh, "%s %s", pin_names[input_pins[i]],
                    level < 0 ? "read failed" : level ? "high" : "low (pulled to GND)");
    }

    return 0;
}

static void wait_for_int(const struct shell *sh, long seconds) {
    uint8_t inputs[2];

    if (int_level() == 0) {
        shell_error(sh, "INT is low with nothing changed: shorted to GND, or R3 missing");
        return;
    }

    shell_print(sh, "Within %ld s, short HOTKEY, PWRSWITCH or KBD_BL_DTCT to GND", seconds);

    int64_t start = k_uptime_get();
    while (gpio_pin_get_raw(int_pin.port, int_pin.pin) != 0) {
        if (k_uptime_get() - start > seconds * MSEC_PER_SEC) {
            shell_error(sh, "INT stayed high: check P3.06 - adapter J1 pin 2 - U1 pin 22");
            return;
        }
        k_msleep(5);
    }

    int elapsed = (int)(k_uptime_get() - start);
    int err = i2c_burst_read_dt(&expander_i2c, REG_INPUT, inputs, sizeof(inputs));

    k_busy_wait(100);
    shell_print(sh, "INT went low after %d ms. Inputs P0 0x%02x P1 0x%02x; INT %s", elapsed,
                inputs[0], inputs[1],
                err                                           ? "not cleared (read failed)"
                : gpio_pin_get_raw(int_pin.port, int_pin.pin) ? "released after reading them"
                                                              : "still low after reading them");
}

// The expander pulls INT low when an unmasked input changes, until its inputs are read
static int cmd_int(const struct shell *sh, size_t argc, char **argv) {
    long seconds = argc > 1 ? strtol(argv[1], NULL, 10) : 15;
    uint8_t inputs[2];
    const uint8_t unmasked[2] = {(uint8_t)~INPUT_MASK, (uint8_t)~(INPUT_MASK >> 8)};
    const uint8_t masked[2] = {0xff, 0xff};

    if (!check_expander(sh)) {
        return -ENODEV;
    }

    configure_inputs();

    // Reading the inputs clears a pending interrupt
    int err = i2c_burst_read_dt(&expander_i2c, REG_INPUT, inputs, sizeof(inputs));
    if (!err) {
        err = i2c_burst_write_dt(&expander_i2c, REG_INT_MASK, unmasked, sizeof(unmasked));
    }
    if (err) {
        shell_error(sh, "Failed to set up the expander interrupt (%d)", err);
        return err;
    }

    wait_for_int(sh, CLAMP(seconds, 1, 60));
    i2c_burst_write_dt(&expander_i2c, REG_INT_MASK, masked, sizeof(masked));
    return 0;
}

// Lines from the Control Board and the expander to the keyboard connectors
// (ReThink_keyboard docs/pinouts). Not tested: the I2C pins (in use by the
// TWIM), PWM on P1.17 (PWM22), PUMP_EN and BOOST_EN (they switch supplies).
enum { PORT_P0, PORT_P1, PORT_P3, PORT_EXP, PORT_COUNT };

static const struct device *const ports[PORT_COUNT] = {
    DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    DEVICE_DT_GET(DT_NODELABEL(gpio1)),
    DEVICE_DT_GET(DT_NODELABEL(gpio3)),
    DEVICE_DT_GET(EXPANDER),
};

struct line {
    const char *name;
    uint8_t port;
    uint8_t pin;
    // Why it may read low without a fault
    const char *pulled_by;
};

static const struct line lines[] = {
    {"SENSE0", PORT_P0, 0},  {"SENSE1", PORT_P0, 1},  {"SENSE2", PORT_P0, 2},
    {"SENSE3", PORT_P0, 3},  {"SENSE4", PORT_P0, 4},  {"SENSE5", PORT_P0, 5},
    {"SENSE6", PORT_P0, 6},  {"SENSE7", PORT_P0, 7},  {"DRV0", PORT_P1, 0},
    {"DRV1", PORT_P1, 1},    {"DRV2", PORT_P1, 18},   {"DRV3", PORT_P1, 19},
    {"DRV4", PORT_P1, 4},    {"DRV5", PORT_P1, 5},    {"DRV6", PORT_P1, 6},
    {"DRV7", PORT_P1, 7},    {"DRV8", PORT_P1, 8},    {"DRV9", PORT_P1, 9},
    {"DRV10", PORT_P1, 10},  {"DRV11", PORT_P1, 11},  {"DRV12", PORT_P1, 12},
    {"DRV13", PORT_P1, 13},  {"DRV14", PORT_P1, 14},
    {"FW_DAT", PORT_P1, 15, "R7 to TP_3V3, low while TP_3V3 is off"},
    {"FW_CLK", PORT_P1, 16, "R6 to TP_3V3, low while TP_3V3 is off"},
    {"INT", PORT_P3, 6},     {"HOTKEY", PORT_EXP, 2}, {"PWRSWITCH", PORT_EXP, 3},
    {"DRV15", PORT_EXP, 4},  {"DRV16", PORT_EXP, 5},  {"DRV17", PORT_EXP, 6},
    {"OUT1", PORT_EXP, 7},
    {"TP1_RST", PORT_EXP, 8, "R14 to TP1_PWR, low while it is off (TP_5V by default)"},
    {"TP2_RST", PORT_EXP, 9, "R22 to TP2_PWR, low while it is off"},
    {"LEDPWR", PORT_EXP, 10},     {"LEDMUTE", PORT_EXP, 11},    {"LEDMICMUTE", PORT_EXP, 12},
    {"LEDCPSLOCK", PORT_EXP, 13}, {"LEDNUMLOCK", PORT_EXP, 14}, {"KBD_BL_DTCT", PORT_EXP, 15},
};

BUILD_ASSERT(ARRAY_SIZE(lines) <= 64, "Lines are tracked in 64-bit masks");

static int line_configure(const struct line *line, gpio_flags_t flags) {
    return gpio_pin_configure(ports[line->port], line->pin, flags);
}

// Bit i set: lines[i] reads low
static int read_low(uint64_t *low) {
    gpio_port_value_t values[PORT_COUNT];

    for (size_t p = 0; p < PORT_COUNT; p++) {
        int err = gpio_port_get_raw(ports[p], &values[p]);
        if (err) {
            return err;
        }
    }

    *low = 0;
    for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
        if (!(values[lines[i].port] & BIT(lines[i].pin))) {
            *low |= BIT64(i);
        }
    }

    return 0;
}

// The matrix scans the SENSE and DRV lines; suspending disconnects them
static void matrices(enum pm_device_action action) {
#define MATRIX_ACTION(node) pm_device_action_run(DEVICE_DT_GET(node), action);
    DT_FOREACH_STATUS_OKAY(zmk_input_gpio_matrix, MATRIX_ACTION)
#undef MATRIX_ACTION
}

// Every line gets a pull-up; each one in turn is driven low. Another line that
// follows it is shorted to it, one already low at rest to GND or a pull-down.
static int cmd_shorts(const struct shell *sh, size_t argc, char **argv) {
    static uint64_t reported[ARRAY_SIZE(lines)];
    uint64_t idle, low;
    int problems = 0;
    int err = 0;

    if (!check_expander(sh)) {
        return -ENODEV;
    }

    memset(reported, 0, sizeof(reported));
    matrices(PM_DEVICE_ACTION_SUSPEND);

    for (size_t i = 0; i < ARRAY_SIZE(lines) && !err; i++) {
        err = line_configure(&lines[i], GPIO_INPUT | GPIO_PULL_UP);
    }
    k_msleep(5);
    if (!err) {
        err = read_low(&idle);
    }

    for (size_t i = 0; i < ARRAY_SIZE(lines) && !err; i++) {
        if (idle & BIT64(i)) {
            if (lines[i].pulled_by) {
                shell_warn(sh, "%s low at rest: %s", lines[i].name, lines[i].pulled_by);
            } else {
                shell_error(sh,
                            "%s low at rest: shorted to GND, or to PUMP_EN/BOOST_EN (driven "
                            "low)? If it reads high after 'adapter boost on', it is bridged "
                            "to BOOST_EN",
                            lines[i].name);
                problems++;
            }
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(lines) && !err; i++) {
        err = line_configure(&lines[i], GPIO_OUTPUT_LOW);
        if (err) {
            break;
        }
        k_busy_wait(500);
        err = read_low(&low);
        line_configure(&lines[i], GPIO_INPUT | GPIO_PULL_UP);
        k_busy_wait(1000);
        if (err) {
            break;
        }

        if (!(low & BIT64(i))) {
            shell_error(sh, "%s stays high when driven low: shorted to a supply?", lines[i].name);
            problems++;
        }

        uint64_t followed = low & ~idle & ~BIT64(i);
        for (size_t j = 0; j < ARRAY_SIZE(lines); j++) {
            size_t a = MIN(i, j), b = MAX(i, j);

            if ((followed & BIT64(j)) && !(reported[a] & BIT64(b))) {
                reported[a] |= BIT64(b);
                shell_error(sh, "%s shorted to %s", lines[a].name, lines[b].name);
                problems++;
            }
        }
    }

    // Back to how the firmware had them; the matrix sets up its own lines
    for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
        bool control_board = lines[i].port == PORT_P0 || lines[i].port == PORT_P1;

        line_configure(&lines[i], control_board ? GPIO_DISCONNECTED : GPIO_INPUT);
    }
    rethink_adapter_park();
    matrices(PM_DEVICE_ACTION_RESUME);

    if (err) {
        shell_error(sh, "Test aborted (%d)", err);
        return err;
    }

    if (problems) {
        shell_error(sh, "%d problem(s) on %d lines", problems, (int)ARRAY_SIZE(lines));
    } else {
        shell_print(sh, "No shorts between the %d lines", (int)ARRAY_SIZE(lines));
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    adapter_cmds, SHELL_CMD(status, NULL, "Supplies, INT and expander pin levels", cmd_status),
    SHELL_CMD_ARG(tp, NULL, "on|off: TP_3V3 (P3.12)", cmd_tp, 2, 0),
    SHELL_CMD_ARG(pump, NULL, "on|off: PUMP_EN, charge pump TP_5V", cmd_pump, 2, 0),
    SHELL_CMD_ARG(vled, NULL, "on|off: VLED_BL to the adapter (BUCK1, P3.11)", cmd_vled, 2, 0),
    SHELL_CMD_ARG(boost, NULL, "on|off: BOOST_EN, backlight boost BL_5V", cmd_boost, 2, 0),
    SHELL_CMD_ARG(pwm, NULL, "<0-100>: backlight PWM duty cycle on P1.17", cmd_pwm, 2, 0),
    SHELL_CMD_ARG(pin, NULL,
                  "<name> 0|1|z|u: expander pin low, high, released, released with pull-up",
                  cmd_pin, 3, 0),
    SHELL_CMD(in, NULL, "Read HOTKEY, PWRSWITCH and KBD_BL_DTCT with pull-ups", cmd_in),
    SHELL_CMD_ARG(int, NULL, "[seconds]: wait for INT while an input is shorted to GND", cmd_int,
                  1, 1),
    SHELL_CMD(shorts, NULL, "Test the connector lines for shorts", cmd_shorts),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(adapter, &adapter_cmds, "ReThink adapter bring-up", NULL);
