/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT rethink_backlight

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <rethink/backlight.h>

#if IS_ENABLED(CONFIG_RETHINK_ADAPTER)
#include <rethink/adapter.h>
#endif

LOG_MODULE_REGISTER(rethink_backlight, CONFIG_RETHINK_BACKLIGHT_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "Exactly one rethink,backlight node");

#define HAS(prop) DT_INST_NODE_HAS_PROP(0, prop)

// Highest duty cycle at full brightness for "backlight max", to try a keyboard
// out; above the Kconfig range (20) BUCK1 may not keep up
#define SHELL_DUTY_LIMIT 50
#define KCONFIG_DUTY_LIMIT 20

// VLED_BL from VBAT or VSYS leaves BUCK1 alone
#define SWITCH_VIN (HAS(vin_supply) && IS_ENABLED(CONFIG_RETHINK_BACKLIGHT_SUPPLY_BUCK1))

struct backlight_config {
    struct pwm_dt_spec pwm;
    uint32_t power_on_delay_ms;
#if HAS(enable_gpios)
    struct gpio_dt_spec enable;
#endif
#if SWITCH_VIN
    const struct device *vin;
#endif
};

struct backlight_data {
    struct k_mutex lock;
    // From DT and Kconfig; "backlight khz" and "backlight max" change them
    // until reboot
    uint32_t period_ns;
    uint8_t max_duty;
    uint8_t brightness;
    bool powered;
    bool unlocked;
    bool warned;
    bool shut_down;
};

static const struct device *const backlight = DEVICE_DT_INST_GET(0);

// Read again before each power-on, not trusted from boot: the keyboard may
// have been swapped or reseated since
static bool detected(void) {
#if IS_ENABLED(CONFIG_RETHINK_ADAPTER)
    return rethink_adapter_bl_redetect() == RETHINK_BL_PRESENT;
#else
    return false;
#endif
}

static const char *detect_name(void) {
#if IS_ENABLED(CONFIG_RETHINK_ADAPTER)
    return rethink_bl_detect_name(rethink_adapter_bl_detect());
#else
    return "unknown";
#endif
}

static void boost_set(bool on) {
#if IS_ENABLED(CONFIG_RETHINK_ADAPTER)
    int err = rethink_adapter_boost_set(on);
    if (err == -ENODEV && on) {
        LOG_WRN("Adapter missing: BOOST_EN not driven");
    }
#endif
}

static void power_on(const struct device *dev) {
    const struct backlight_config *cfg = dev->config;

#if SWITCH_VIN
    int err = regulator_enable(cfg->vin);
    if (err) {
        LOG_ERR("Failed to enable the VLED_BL supply (%d)", err);
    }
#endif
#if HAS(enable_gpios)
    gpio_pin_set_dt(&cfg->enable, 1);
#endif
    boost_set(true);

    k_msleep(cfg->power_on_delay_ms);
    LOG_INF("Power chain on");
}

static void power_off(const struct device *dev) {
    const struct backlight_config *cfg = dev->config;

    boost_set(false);
#if HAS(enable_gpios)
    gpio_pin_set_dt(&cfg->enable, 0);
#endif
#if SWITCH_VIN
    regulator_disable(cfg->vin);
#endif

    LOG_INF("Power chain off");
}

// Call with data->lock held
static int apply(const struct device *dev) {
    const struct backlight_config *cfg = dev->config;
    struct backlight_data *data = dev->data;
    bool want = data->brightness > 0;

    if (data->shut_down) {
        want = false;
    } else if (want && !IS_ENABLED(CONFIG_RETHINK_BOOST_FITTED)) {
        if (!data->warned) {
            LOG_WRN("Backlight off: boost converter U4 not fitted (RETHINK_BOOST_FITTED=n)");
            data->warned = true;
        }
        want = false;
    } else if (want && !data->powered && !data->unlocked && !detected()) {
        if (!data->warned) {
            LOG_WRN("Backlight locked: backlit keyboard %s.%s", detect_name(),
                    IS_ENABLED(CONFIG_RETHINK_BACKLIGHT_TAP_UNLOCK)
                        ? " Triple-press the pairing button to unlock until reboot."
                        : "");
            data->warned = true;
        }
        want = false;
    }

    if (want && !data->powered) {
        power_on(dev);
        data->powered = true;
    }

    uint32_t pulse =
        want ? (uint32_t)((uint64_t)data->period_ns * data->brightness * data->max_duty / (100 * 100))
             : 0;
    int err = pwm_set_dt(&cfg->pwm, data->period_ns, pulse);
    if (err) {
        LOG_ERR("Failed to set PWM (%d)", err);
    }

    if (!want && data->powered) {
        power_off(dev);
        data->powered = false;
    }

    return err;
}

static int backlight_set_brightness(const struct device *dev, uint32_t led, uint8_t value) {
    struct backlight_data *data = dev->data;

    if (led != 0) {
        return -EINVAL;
    }

    k_mutex_lock(&data->lock, K_FOREVER);
    data->brightness = MIN(value, 100);
    LOG_DBG("Brightness %d%%", data->brightness);
    // Warn again on the next attempt to switch on
    if (data->brightness == 0) {
        data->warned = false;
    }
    int err = apply(dev);
    k_mutex_unlock(&data->lock);

    return err;
}

void rethink_backlight_unlock(void) {
    struct backlight_data *data = backlight->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    if (!data->unlocked) {
        data->unlocked = true;
        LOG_WRN("Backlight unlocked by user (backlit keyboard %s)", detect_name());
        apply(backlight);
    }
    k_mutex_unlock(&data->lock);
}

void rethink_backlight_shutdown(void) {
    struct backlight_data *data = backlight->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->shut_down = true;
    apply(backlight);
    k_mutex_unlock(&data->lock);
}

static int backlight_init(const struct device *dev) {
    const struct backlight_config *cfg = dev->config;
    struct backlight_data *data = dev->data;
    int err;

    k_mutex_init(&data->lock);
    data->period_ns = cfg->pwm.period;
    data->max_duty = CONFIG_RETHINK_BACKLIGHT_MAX_DUTY;

    if (!pwm_is_ready_dt(&cfg->pwm)) {
        LOG_ERR("PWM not ready");
        return -ENODEV;
    }

#if HAS(enable_gpios)
    err = gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure the VLED_BL switch (%d)", err);
        return err;
    }
#endif

    err = pwm_set_pulse_dt(&cfg->pwm, 0);
    if (err) {
        LOG_ERR("Failed to stop PWM (%d)", err);
    }

    LOG_INF("Ready: VLED_BL from %s, PWM %u Hz, max duty %d%%, backlit keyboard %s",
            IS_ENABLED(CONFIG_RETHINK_BACKLIGHT_SUPPLY_BUCK1)  ? "BUCK1"
            : IS_ENABLED(CONFIG_RETHINK_BACKLIGHT_SUPPLY_VBAT) ? "VBAT"
                                                               : "VSYS",
            (uint32_t)(NSEC_PER_SEC / data->period_ns), data->max_duty, detect_name());
    return 0;
}

#if IS_ENABLED(CONFIG_SHELL)
// The value of a "<min-max>" argument, or a negative error
static long arg_in(const struct shell *sh, const char *arg, long min, long max) {
    int err = 0;
    long val = shell_strtol(arg, 10, &err);

    if (err || val < min || val > max) {
        shell_error(sh, "Expected %ld-%ld", min, max);
        return -EINVAL;
    }
    return val;
}

static void print_state(const struct shell *sh, const struct backlight_data *data) {
    uint32_t duty_permille = (uint32_t)data->brightness * data->max_duty / 10;

    shell_print(sh, "Brightness %d%%: duty %u.%u%% (max %d%%), PWM %u Hz, power chain %s",
                data->brightness, data->powered ? duty_permille / 10 : 0,
                data->powered ? duty_permille % 10 : 0, data->max_duty,
                (uint32_t)(NSEC_PER_SEC / data->period_ns), data->powered ? "on" : "off");
    shell_print(sh, "Backlit keyboard %s%s", detect_name(),
                data->unlocked ? ", unlocked by user" : "");
}

static int cmd_backlight(const struct shell *sh, size_t argc, char **argv) {
    struct backlight_data *data = backlight->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    print_state(sh, data);
    k_mutex_unlock(&data->lock);
    return 0;
}

static int cmd_khz(const struct shell *sh, size_t argc, char **argv) {
    struct backlight_data *data = backlight->data;
    long khz = arg_in(sh, argv[1], 1, 100);

    if (khz < 0) {
        return khz;
    }
    k_mutex_lock(&data->lock, K_FOREVER);
    data->period_ns = NSEC_PER_SEC / (khz * 1000);
    int err = apply(backlight);
    print_state(sh, data);
    k_mutex_unlock(&data->lock);
    return err;
}

static int cmd_max(const struct shell *sh, size_t argc, char **argv) {
    struct backlight_data *data = backlight->data;
    long max = arg_in(sh, argv[1], 1, SHELL_DUTY_LIMIT);

    if (max < 0) {
        return max;
    }
    if (max > KCONFIG_DUTY_LIMIT) {
        shell_warn(sh, "Above %d%%: BUCK1 (200 mA) may not keep up; watch VLED_BL and BL_5V",
                   KCONFIG_DUTY_LIMIT);
    }
    k_mutex_lock(&data->lock, K_FOREVER);
    data->max_duty = max;
    int err = apply(backlight);
    print_state(sh, data);
    k_mutex_unlock(&data->lock);
    return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    backlight_cmds,
    SHELL_CMD_ARG(khz, NULL, "<1-100>: PWM frequency in kHz, until reboot", cmd_khz, 2, 0),
    SHELL_CMD_ARG(max, NULL,
                  "<1-" STRINGIFY(SHELL_DUTY_LIMIT) ">: duty cycle at full brightness, %, until reboot",
                  cmd_max, 2, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(backlight, &backlight_cmds,
                   "Keyboard backlight: brightness, duty cycle, PWM frequency, interlock",
                   cmd_backlight);
#endif

static DEVICE_API(led, backlight_api) = {
    .set_brightness = backlight_set_brightness,
};

static const struct backlight_config config = {
    .pwm = PWM_DT_SPEC_INST_GET(0),
    .power_on_delay_ms = DT_INST_PROP(0, power_on_delay_ms),
#if HAS(enable_gpios)
    .enable = GPIO_DT_SPEC_INST_GET(0, enable_gpios),
#endif
#if SWITCH_VIN
    .vin = DEVICE_DT_GET(DT_INST_PHANDLE(0, vin_supply)),
#endif
};

static struct backlight_data data;

// After rethink,adapter-ctrl, which samples KBD_BL_DTCT
DEVICE_DT_INST_DEFINE(0, backlight_init, NULL, &data, &config, POST_KERNEL,
                      CONFIG_RETHINK_BACKLIGHT_INIT_PRIORITY, &backlight_api);
