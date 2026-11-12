/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */
// The nPM1300 PMIC: BUCK1 off at boot, BUCK2 kept out of its stuck state, and
// the "pmic" shell command.
//
// BUCK1 (nPM_out1_3.3V) only feeds the backlight switch Q3. Its VSET1
// resistor starts it at power-on; the board's regulator-boot-off should then
// switch it off, but Zephyr only switches off a buck that reads as on, from
// BUCKSTATUS's power-good bit, and that bit reads 0 for a running BUCK1 (VO1
// at 3.34 V). So BUCK1 stayed on, at the 3.3 V Zephyr sets, and from a cell
// below about 3.4 V it sat in dropout, drawing about 3 mA. An enable and a
// disable leave its reference count as it was and switch it off; the
// backlight (and "adapter vled on") switch it on when they need it.
//
// BUCK2's stuck state: PWM mode with its power good at 0 (BUCKSTATUS 0x20 or
// 0x28 instead of 0x40 or 0x48), about 3 mA more, System OFF included, until
// the input rises above about 3.35 V or USB comes. A change of its voltage
// without headroom brings it: Zephyr's regulator-init-microvolt (3.3 V) at a
// start from 3.2-3.3 V; src/power.c's change to 3.0 V a second later once
// that was gone. Started from 3.2 V without either, BUCK2 isn't in it. The
// nPM1300's anomaly 27 ("Programming BUCK voltage increases current
// consumption", 1 mA) has the same trigger; as PebbleOS found, a buck keeps
// the state until it is switched off and on. Lowering BUCK2 further or
// forcing its hysteretic mode doesn't clear it.
//
// CONFIG_RETHINK_BUCK2_RESTART:
// - At boot, from a cell under CONFIG_RETHINK_BUCK2_LOW_MV, BUCK2 is set to
//   3.0 V and then switched off and on: it starts again at 3.0 V, with
//   headroom. src/power.c keeps it there until the cell is over
//   CONFIG_RETHINK_BUCK2_HIGH_MV, where the change back to 3.3 V has
//   headroom too.
// - At boot and at each battery check (src/power.c, every 10 s, 20 ms after
//   a change), a BUCK2 in PWM mode is switched off and on.
// The two register writes go one after the other; the nRF and the adapter run
// through the gap on the rail's capacitors. If the second write failed, the
// board would stay off until battery and USB are both disconnected.
//
// "pmic" shows the bucks' status and the battery current, now and as last
// read on battery (every 2 s), to see the state after plugging in USB, and
// what was done to BUCK2.

#include <zephyr/device.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <rethink/pmic.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

LOG_MODULE_REGISTER(rethink_pmic, LOG_LEVEL_INF);

static const struct device *const pmic = DEVICE_DT_GET(DT_NODELABEL(npm1300));
static const struct device *const buck1 = DEVICE_DT_GET(DT_NODELABEL(npm1300_buck1));
static const struct device *const buck2 = DEVICE_DT_GET(DT_NODELABEL(npm1300_buck2));

#if IS_ENABLED(CONFIG_SENSOR)
static const struct device *const charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
#endif

#define BUCK_BASE 0x04U
#define BUCK2_ENA_SET 0x02U
#define BUCK2_ENA_CLR 0x03U
#define BUCK_STATUS 0x34U
// BUCKSTATUS [5:4]: BUCK2's mode, 2 = PWM
#define BUCK2_MODE(status) (((status) >> 4) & 3U)
#define BUCK_MODE_PWM 2U

// BUCKSTATUS, or a negative error
static int buck_status(void) {
    uint8_t status;
    int err = mfd_npm13xx_reg_read(pmic, BUCK_BASE, BUCK_STATUS, &status);

    return err ? err : status;
}

#if IS_ENABLED(CONFIG_RETHINK_BUCK2_RESTART)

#if defined(CONFIG_RETHINK_BUCK2_LOW_MV) && CONFIG_RETHINK_BUCK2_LOW_MV > 0 &&                     \
    IS_ENABLED(CONFIG_SENSOR)
#define BOOT_LOW_MV CONFIG_RETHINK_BUCK2_LOW_MV
#else
#define BOOT_LOW_MV 0
#endif

#define BUCK2_LOW_UV 3000000
// Restarts in a row that leave BUCK2 in PWM mode before the checks stop
#define MAX_FAILURES 3

static bool ready;

// What was done at boot; statuses negative for none
static struct {
    int mv;    // battery
    int start; // BUCKSTATUS at boot
    int set;   // 5 ms after setting 3.0 V
    int after; // 5 ms after the restart
    bool lowered;
    bool restarted;
} boot = {.mv = -ENODATA, .start = -ENODATA, .set = -ENODATA, .after = -ENODATA};

// The checks since boot (rethink_pmic_buck2_check)
static struct {
    uint32_t count;
    int64_t uptime_ms;
    int before;
    int after;
    int failures;
} restarts = {.before = -ENODATA, .after = -ENODATA};

static bool stuck(int status) { return status >= 0 && BUCK2_MODE(status) == BUCK_MODE_PWM; }

// Off and on; BUCKSTATUS 5 ms later, or a negative error
static int buck2_restart(void) {
    // Nothing else may run between the two writes
    int prio = k_thread_priority_get(k_current_get());

    k_thread_priority_set(k_current_get(), K_HIGHEST_THREAD_PRIO);
    int err = mfd_npm13xx_reg_write(pmic, BUCK_BASE, BUCK2_ENA_CLR, 1U);
    int on = mfd_npm13xx_reg_write(pmic, BUCK_BASE, BUCK2_ENA_SET, 1U);

    if (on) {
        on = mfd_npm13xx_reg_write(pmic, BUCK_BASE, BUCK2_ENA_SET, 1U);
    }
    k_thread_priority_set(k_current_get(), prio);

    if (err || on) {
        return err ? err : on;
    }
    k_msleep(5);
    return buck_status();
}

#if BOOT_LOW_MV > 0
// The charger's own measurement: the fuel gauge has none yet
static int battery_mv(void) {
    struct sensor_value val;
    int err = device_is_ready(charger) ? sensor_sample_fetch(charger) : -ENODEV;

    if (!err) {
        err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &val);
    }
    return err ? err : val.val1 * 1000 + val.val2 / 1000;
}
#endif

static void buck2_boot(void) {
    boot.start = buck_status();

#if BOOT_LOW_MV > 0
    boot.mv = battery_mv();
    if (boot.mv > 0 && boot.mv < BOOT_LOW_MV) {
        int err = regulator_set_voltage(buck2, BUCK2_LOW_UV, BUCK2_LOW_UV);

        if (err) {
            LOG_WRN("BUCK2 not set to 3.0 V (%d)", err);
        } else {
            boot.lowered = true;
            k_msleep(5);
            boot.set = buck_status();
        }
    }
#endif

    if (boot.lowered || stuck(boot.start)) {
        boot.after = buck2_restart();
        boot.restarted = true;
    }
    // Negative statuses (none, or an error) show as 0xffffff..
    LOG_INF("BUCK2 at boot: battery %d mV, BUCKSTATUS 0x%02x; set to 3.0 V: %s, 0x%02x; "
            "restarted: %s, 0x%02x",
            boot.mv, boot.start, boot.lowered ? "yes" : "no", boot.set,
            boot.restarted ? "yes" : "no", boot.after);
}

void rethink_pmic_buck2_check(void) {
    if (!ready || restarts.failures >= MAX_FAILURES) {
        return;
    }

    int status = buck_status();

    if (!stuck(status)) {
        return;
    }

    int after = buck2_restart();

    restarts.count++;
    restarts.uptime_ms = k_uptime_get();
    restarts.before = status;
    restarts.after = after;
    LOG_WRN("BUCK2 in PWM mode (BUCKSTATUS 0x%02x): restarted, now 0x%02x", status, after);

    if (after < 0 || stuck(after)) {
        if (++restarts.failures >= MAX_FAILURES) {
            LOG_ERR("BUCK2 still in PWM mode after %d restarts: no more", MAX_FAILURES);
        }
    } else {
        restarts.failures = 0;
    }
}
#endif // CONFIG_RETHINK_BUCK2_RESTART

static int pmic_init(void) {
    if (!device_is_ready(buck1) || !device_is_ready(buck2)) {
        LOG_WRN("Bucks not ready: left as they are");
        return 0;
    }

#if IS_ENABLED(CONFIG_RETHINK_BUCK2_RESTART)
    buck2_boot();
    ready = true;
#endif

    int err = regulator_enable(buck1);

    if (!err) {
        err = regulator_disable(buck1);
    }
    if (err) {
        LOG_WRN("BUCK1 not switched off (%d)", err);
    }
    return 0;
}

// After the regulator and charger drivers (POST_KERNEL)
SYS_INIT(pmic_init, APPLICATION, 0);

#if IS_ENABLED(CONFIG_SHELL) && IS_ENABLED(CONFIG_SENSOR)

#define SAMPLE_PERIOD K_SECONDS(2)

struct reading {
    int64_t uptime_ms;
    int status; // BUCKSTATUS, or a negative error
    int32_t ua; // battery current, the charger driver's last measurement
    bool current_ok;
};

static struct reading on_battery = {.status = -ENODATA};

static void read_now(struct reading *r) {
    struct sensor_value val;

    r->uptime_ms = k_uptime_get();
    r->status = buck_status();
    // No fetch: the fuel gauge thread fetches, this reads its last result
    r->current_ok = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &val) == 0;
    r->ua = r->current_ok ? (int32_t)sensor_value_to_micro(&val) : 0;
}

static void sample(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sample_work, sample);

static bool usb_powered(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static void sample(struct k_work *work) {
    if (!usb_powered()) {
        read_now(&on_battery);
    }
    k_work_schedule(&sample_work, SAMPLE_PERIOD);
}

static int sample_init(void) {
    if (device_is_ready(pmic)) {
        k_work_schedule(&sample_work, SAMPLE_PERIOD);
    }
    return 0;
}

SYS_INIT(sample_init, APPLICATION, 1);

void rethink_pmic_quiet(void) { k_work_cancel_delayable(&sample_work); }

static const char *const modes[] = {"auto", "hysteretic", "PWM", "?"};

static void print(const struct shell *sh, const char *what, const struct reading *r) {
    if (r->status < 0) {
        shell_print(sh, "%s: no reading (%d)", what, r->status);
        return;
    }

    uint8_t s = r->status;

    // Nordic's map: [1:0] BUCK1MODE, [2] BUCK1PWRGOOD, [3] BUCK1PWMOK, then
    // the same for BUCK2 in [7:4]
    shell_print(sh, "%s (%lld s): BUCKSTATUS 0x%02x", what, r->uptime_ms / 1000, s);
    for (int b = 0; b < 2; b++) {
        uint8_t f = s >> (4 * b);

        shell_print(sh, "  BUCK%d: mode %s, power good %d, PWM %d", b + 1, modes[f & 3],
                    (f >> 2) & 1, (f >> 3) & 1);
    }
    if (r->current_ok) {
        int32_t ua = r->ua < 0 ? -r->ua : r->ua;

        shell_print(sh, "  Battery current: %s%d.%d mA (negative: discharging)",
                    r->ua < 0 ? "-" : "", ua / 1000, ua % 1000 / 100);
    } else {
        shell_print(sh, "  Battery current: no reading");
    }
}

#if IS_ENABLED(CONFIG_RETHINK_BUCK2_RESTART)
// A BUCKSTATUS as text: "0x48", or "-" for none
static const char *status_str(int status, char *buf, size_t len) {
    if (status < 0) {
        return "-";
    }
    snprintk(buf, len, "0x%02x", (uint8_t)status);
    return buf;
}

static void print_buck2(const struct shell *sh) {
    char a[8], b[8], c[8];

    shell_print(sh, "BUCK2 at boot: battery %d mV, BUCKSTATUS %s", boot.mv,
                status_str(boot.start, a, sizeof(a)));
    if (boot.lowered) {
        shell_print(sh, "  set to 3.0 V: %s; restarted: %s", status_str(boot.set, b, sizeof(b)),
                    status_str(boot.after, c, sizeof(c)));
    } else if (boot.restarted) {
        shell_print(sh, "  in PWM mode, restarted: %s", status_str(boot.after, c, sizeof(c)));
    } else {
        shell_print(sh, "  left as it was");
    }
    if (restarts.count == 0) {
        shell_print(sh, "BUCK2 restarts since: none");
    } else {
        shell_print(sh, "BUCK2 restarts since: %u, last at %lld s: %s before, %s after%s",
                    restarts.count, restarts.uptime_ms / 1000,
                    status_str(restarts.before, a, sizeof(a)),
                    status_str(restarts.after, b, sizeof(b)),
                    restarts.failures >= MAX_FAILURES ? " (stopped: it stays in PWM mode)" : "");
    }
}
#endif

static int cmd_pmic(const struct shell *sh, size_t argc, char **argv) {
    struct reading now;

    if (!device_is_ready(pmic)) {
        shell_error(sh, "nPM1300 not ready");
        return -ENODEV;
    }
    read_now(&now);
    print(sh, "Now", &now);
    print(sh, "Last on battery", &on_battery);
#if IS_ENABLED(CONFIG_RETHINK_BUCK2_RESTART)
    print_buck2(sh);
#endif
    return 0;
}

SHELL_CMD_REGISTER(pmic, NULL,
                   "nPM1300 bucks (mode, power good, PWM) and battery current, now and as last "
                   "read on battery (every 2 s); BUCK2 at boot and its restarts",
                   cmd_pmic);

#else
void rethink_pmic_quiet(void) {}
#endif // CONFIG_SHELL && CONFIG_SENSOR
