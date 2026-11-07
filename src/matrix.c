/*
 * Copyright (c) 2020-2021 The ZMK Contributors
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// Keyboard matrix for ThinkPad keyboards on the ReThink adapter, based on
// ZMK's input-gpio-matrix (app/module/drivers/input/input_gpio_matrix.c).
// col2row only: columns are driven active one at a time, rows are read.
//
// Expander columns and direct keys go over I2C, which can't run in the GPIO
// interrupt. The interrupt only stops the Control Board columns and schedules
// the scan; the scan releases the expander columns first. While waiting for
// a key, every column is active (expander columns driven low, pull-ups off,
// so nothing draws current) and the rows and INT wake the matrix, also from
// System OFF.
//
// Columns and direct keys that no matrix transform maps are never driven or
// read, so a keyboard without keys on DRV16/17 or without HOTKEY/PWRSWITCH
// saves those I2C transfers on every scan.

#define DT_DRV_COMPAT rethink_matrix

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/util.h>

#include <zmk/debounce.h>

LOG_MODULE_REGISTER(rethink_matrix, CONFIG_RETHINK_MATRIX_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "Exactly one rethink,matrix node");

#define ROWS DT_INST_PROP_LEN(0, row_gpios)
#define GPIO_COLS DT_INST_PROP_LEN(0, col_gpios)
#define EXP_COLS DT_INST_PROP_LEN_OR(0, expander_col_gpios, 0)
#define COLS (GPIO_COLS + EXP_COLS)
#define DIRECT DT_INST_PROP_LEN_OR(0, direct_gpios, 0)
#define HAS_EXPANDER (EXP_COLS + DIRECT > 0)

BUILD_ASSERT(ROWS <= 32, "Rows are tracked in 32-bit masks");
BUILD_ASSERT(COLS <= 32, "Columns are tracked in 32-bit masks");

// Columns and direct keys mapped by an enabled matrix transform. Its entries
// are RC(row, col) = row << 8 | col, shifted by its offsets; direct key i is
// RC(ROWS, i).
#define TF_ROW(node, idx) ((DT_PROP_BY_IDX(node, map, idx) >> 8) - DT_PROP(node, row_offset))
#define TF_COL(node, idx) ((DT_PROP_BY_IDX(node, map, idx) & 0xff) - DT_PROP(node, col_offset))
#define TF_BIT_IF(node, idx, cond) ((cond) && TF_COL(node, idx) >= 0 ? BIT(TF_COL(node, idx)) : 0)
#define TF_COL_BIT(node, prop, idx)                                                                \
    | TF_BIT_IF(node, idx, TF_ROW(node, idx) < ROWS && TF_COL(node, idx) < COLS)
#define TF_DIRECT_BIT(node, prop, idx)                                                             \
    | TF_BIT_IF(node, idx, TF_ROW(node, idx) == ROWS && TF_COL(node, idx) < DIRECT)
#define TF_COLS(node) DT_FOREACH_PROP_ELEM(node, map, TF_COL_BIT)
#define TF_DIRECTS(node) DT_FOREACH_PROP_ELEM(node, map, TF_DIRECT_BIT)

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_matrix_transform)
#define USED_COLS (0 DT_FOREACH_STATUS_OKAY(zmk_matrix_transform, TF_COLS))
#define USED_DIRECT (0 DT_FOREACH_STATUS_OKAY(zmk_matrix_transform, TF_DIRECTS))
#else
// ZMK's default transform maps every position
#define USED_COLS BIT_MASK(COLS)
#define USED_DIRECT BIT_MASK(DIRECT)
#endif

static const uint32_t used_cols = USED_COLS;
BUILD_ASSERT(!HAS_EXPANDER || IS_ENABLED(CONFIG_RETHINK_ADAPTER),
             "Expander columns and direct keys need the adapter (rethink,adapter-ctrl)");
BUILD_ASSERT(!DIRECT || DT_INST_NODE_HAS_PROP(0, int_gpios), "direct-gpios needs int-gpios");

#if HAS_EXPANDER
#include <rethink/adapter.h>
#include <rethink/expander.h>
#endif

#define GHOST_BLOCKING DT_INST_PROP(0, ghost_key_blocking)
#define SETTLE_US DT_INST_PROP(0, settle_us)
#define SCAN_PERIOD_MS DT_INST_PROP(0, debounce_scan_period_ms)

static const struct gpio_dt_spec rows[] = {
    DT_INST_FOREACH_PROP_ELEM_SEP(0, row_gpios, GPIO_DT_SPEC_GET_BY_IDX, (, ))};
static const struct gpio_dt_spec cols[] = {
    DT_INST_FOREACH_PROP_ELEM_SEP(0, col_gpios, GPIO_DT_SPEC_GET_BY_IDX, (, ))};

#define EXP_PIN(node_id, prop, idx) DT_GPIO_PIN_BY_IDX(node_id, prop, idx)

#if EXP_COLS
static const uint8_t exp_cols[] = {
    DT_INST_FOREACH_PROP_ELEM_SEP(0, expander_col_gpios, EXP_PIN, (, ))};
#endif

#if DIRECT
static const uint8_t direct_pins[] = {
    DT_INST_FOREACH_PROP_ELEM_SEP(0, direct_gpios, EXP_PIN, (, ))};
static const struct gpio_dt_spec int_pin = GPIO_DT_SPEC_INST_GET(0, int_gpios);
static struct gpio_callback int_cb;
static struct zmk_debounce_state direct_state[DIRECT];
#endif

static const struct zmk_debounce_config debounce_config = {
    .debounce_press_ms = DT_INST_PROP(0, debounce_press_ms),
    .debounce_release_ms = DT_INST_PROP(0, debounce_release_ms),
};

BUILD_ASSERT(DT_INST_PROP(0, debounce_press_ms) <= DEBOUNCE_COUNTER_MAX);
BUILD_ASSERT(DT_INST_PROP(0, debounce_release_ms) <= DEBOUNCE_COUNTER_MAX);

// Index col * ROWS + row
static struct zmk_debounce_state matrix_state[ROWS * COLS];
static struct gpio_callback row_cbs[ROWS];
static struct k_work_delayable scan_work;
static int64_t scan_time;

#if EXP_COLS
// Mapped expander columns only
static uint16_t exp_col_mask;
// The expander columns are driven low for the wait
static bool exp_cols_waiting;
#endif

#if DIRECT
// Mapped direct keys only
static uint16_t direct_mask;
#endif

static const struct device *matrix_dev(void) { return DEVICE_DT_INST_GET(0); }

static bool expander_ready(void) {
#if HAS_EXPANDER
    return rethink_adapter_ready();
#else
    return false;
#endif
}

#if EXP_COLS
// Bits of active are driven low, the other expander columns rest on the
// pull-up: one write of the direction register
static int exp_cols_drive(uint16_t active) {
    return rethink_exp_update(RETHINK_EXP_CONFIG, exp_col_mask, ~active & exp_col_mask);
}
#endif

// Rows that read active, as a mask
static uint32_t read_rows(void) {
    uint32_t active = 0;

    for (size_t r = 0; r < ROWS; r++) {
        if (gpio_pin_get_dt(&rows[r]) > 0) {
            active |= BIT(r);
        }
    }

    return active;
}

// Unmapped columns stay released, so their keys can't wake the matrix
static void set_gpio_cols(int value) {
    for (size_t c = 0; c < GPIO_COLS; c++) {
        if (used_cols & BIT(c)) {
            gpio_pin_set_dt(&cols[c], value);
        }
    }
}

static void row_interrupts(gpio_flags_t flags) {
    for (size_t r = 0; r < ROWS; r++) {
        gpio_pin_interrupt_configure_dt(&rows[r], flags);
    }
#if DIRECT
    gpio_pin_interrupt_configure_dt(&int_pin,
                                    expander_ready() && direct_mask ? flags : GPIO_INT_DISABLE);
#endif
}

// Interrupt context: Control Board pins only
static void interrupts_disable(void) {
    row_interrupts(GPIO_INT_DISABLE);
    set_gpio_cols(0);
}

static void wait_for_key(void) {
#if HAS_EXPANDER
    if (expander_ready()) {
#if EXP_COLS
        if (exp_col_mask) {
            // Low latch from setup: driving them is the direction change;
            // then the pull-ups go off, so a driven column draws nothing
            exp_cols_drive(exp_col_mask);
            rethink_exp_update(RETHINK_EXP_PULL_EN, exp_col_mask, 0);
            exp_cols_waiting = true;
        }
#endif
#if DIRECT
        if (direct_mask) {
            uint16_t levels;

            // Unmasked again on every wait (the bring-up shell may mask it);
            // the read clears a pending interrupt
            rethink_exp_update(RETHINK_EXP_INT_MASK, direct_mask, 0);
            rethink_exp_read(&levels);
        }
#endif
    }
#endif

    set_gpio_cols(1);
    row_interrupts(GPIO_INT_LEVEL_ACTIVE);
}

static void irq_handler(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins) {
    interrupts_disable();
    scan_time = k_uptime_get();
    k_work_reschedule(&scan_work, K_NO_WAIT);
}

#if GHOST_BLOCKING
// In a diodeless matrix, three pressed corners of a rectangle make the fourth
// read pressed too. Wherever two columns share two or more active rows, the
// keys there keep their debounced state until the rectangle breaks.
static void block_ghosts(uint32_t raw[COLS]) {
    uint32_t ambiguous[COLS] = {0};

    for (size_t a = 0; a < COLS; a++) {
        for (size_t b = a + 1; b < COLS; b++) {
            uint32_t common = raw[a] & raw[b];

            if (common & (common - 1)) {
                ambiguous[a] |= common;
                ambiguous[b] |= common;
            }
        }
    }

    for (size_t c = 0; c < COLS; c++) {
        for (size_t r = 0; r < ROWS; r++) {
            if (!(ambiguous[c] & BIT(r))) {
                continue;
            }

            if (zmk_debounce_is_pressed(&matrix_state[c * ROWS + r])) {
                raw[c] |= BIT(r);
            } else {
                raw[c] &= ~BIT(r);
            }
        }
    }
}
#endif

static void report(uint32_t row, uint32_t col, bool pressed) {
    LOG_DBG("Sending event at %u,%u state %s", row, col, pressed ? "on" : "off");
    input_report_abs(matrix_dev(), INPUT_ABS_X, col, false, K_FOREVER);
    input_report_abs(matrix_dev(), INPUT_ABS_Y, row, false, K_FOREVER);
    input_report_key(matrix_dev(), INPUT_BTN_TOUCH, pressed, true, K_FOREVER);
}

static bool update(struct zmk_debounce_state *state, bool active, uint32_t row, uint32_t col) {
    zmk_debounce_update(state, active, SCAN_PERIOD_MS, &debounce_config);
    if (zmk_debounce_get_changed(state)) {
        report(row, col, zmk_debounce_is_pressed(state));
    }

    return zmk_debounce_is_active(state);
}

static void scan(void) {
    uint32_t raw[COLS] = {0};
    bool exp = expander_ready();
    bool keep_scanning = false;

    ARG_UNUSED(exp);

#if EXP_COLS
    if (exp && exp_cols_waiting) {
        // Pull-ups first, so released columns rest high
        rethink_exp_update(RETHINK_EXP_PULL_EN, exp_col_mask, exp_col_mask);
        exp_cols_drive(0);
        exp_cols_waiting = false;
    }
#endif

    for (size_t c = 0; c < GPIO_COLS; c++) {
        if (!(used_cols & BIT(c))) {
            continue;
        }
        gpio_pin_set_dt(&cols[c], 1);
        k_busy_wait(SETTLE_US);
        raw[c] = read_rows();
        gpio_pin_set_dt(&cols[c], 0);
    }

#if EXP_COLS
    if (exp && exp_col_mask) {
        // Each step releases the previous column and drives the next: one write
        for (size_t k = 0; k < EXP_COLS; k++) {
            if (!(exp_col_mask & BIT(exp_cols[k]))) {
                continue;
            }
            if (exp_cols_drive(BIT(exp_cols[k])) == 0) {
                k_busy_wait(SETTLE_US);
                raw[GPIO_COLS + k] = read_rows();
            }
        }
        exp_cols_drive(0);
    }
#endif

#if GHOST_BLOCKING
    block_ghosts(raw);
#endif

    for (size_t c = 0; c < COLS; c++) {
        for (size_t r = 0; r < ROWS; r++) {
            keep_scanning |= update(&matrix_state[c * ROWS + r], raw[c] & BIT(r), r, c);
        }
    }

#if DIRECT
    uint16_t levels;

    // Also clears the expander's interrupt
    if (exp && direct_mask && rethink_exp_read(&levels) == 0) {
        for (size_t i = 0; i < DIRECT; i++) {
            uint16_t bit = BIT(direct_pins[i]);

            keep_scanning |= update(&direct_state[i], (direct_mask & bit) && !(levels & bit),
                                    ROWS, i);
        }
    }
#endif

    if (keep_scanning) {
        scan_time += SCAN_PERIOD_MS;
        k_work_reschedule(&scan_work, K_TIMEOUT_ABS_MS(scan_time));
    } else {
        wait_for_key();
    }
}

static void scan_work_handler(struct k_work *work) { scan(); }

static void setup_pins(void) {
    for (size_t r = 0; r < ROWS; r++) {
        gpio_pin_configure_dt(&rows[r], GPIO_INPUT);
    }
    for (size_t c = 0; c < GPIO_COLS; c++) {
        gpio_pin_configure_dt(&cols[c], GPIO_OUTPUT_INACTIVE);
    }

#if HAS_EXPANDER
    if (expander_ready()) {
#if EXP_COLS
        for (size_t k = 0; k < EXP_COLS; k++) {
            rethink_exp_configure(exp_cols[k], RETHINK_EXP_IN_PULL_UP);
        }
        // Driving a column is then only the direction change
        rethink_exp_update(RETHINK_EXP_OUTPUT, exp_col_mask, 0);
#endif
#if DIRECT
        for (size_t i = 0; i < DIRECT; i++) {
            rethink_exp_configure(direct_pins[i], RETHINK_EXP_IN_PULL_UP);
        }
#endif
    }
#endif

#if DIRECT
    // Without the adapter nothing pulls INT up: disconnected, not floating
    gpio_pin_configure_dt(&int_pin,
                          expander_ready() && direct_mask ? GPIO_INPUT : GPIO_DISCONNECTED);
#endif
}

#if IS_ENABLED(CONFIG_PM_DEVICE)

// Control Board pins disconnected, expander columns released to their
// pull-ups, direct keys no longer interrupting
static void release_pins(void) {
    for (size_t r = 0; r < ROWS; r++) {
        gpio_pin_configure_dt(&rows[r], GPIO_DISCONNECTED);
    }
    for (size_t c = 0; c < GPIO_COLS; c++) {
        gpio_pin_configure_dt(&cols[c], GPIO_DISCONNECTED);
    }

#if HAS_EXPANDER
    if (expander_ready()) {
#if EXP_COLS
        rethink_exp_update(RETHINK_EXP_PULL_EN, exp_col_mask, exp_col_mask);
        exp_cols_drive(0);
        exp_cols_waiting = false;
#endif
#if DIRECT
        rethink_exp_update(RETHINK_EXP_INT_MASK, direct_mask, direct_mask);
#endif
    }
#endif
}

static int pm_action(const struct device *dev, enum pm_device_action action) {
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        k_work_cancel_delayable(&scan_work);
        row_interrupts(GPIO_INT_DISABLE);
        release_pins();
        return 0;
    case PM_DEVICE_ACTION_RESUME:
        setup_pins();
        scan_time = k_uptime_get();
        // Ends in wait_for_key() once no key is pressed
        scan();
        return 0;
    default:
        return -ENOTSUP;
    }
}

#endif

static int matrix_init(const struct device *dev) {
    k_work_init_delayable(&scan_work, scan_work_handler);

#if EXP_COLS
    for (size_t k = 0; k < EXP_COLS; k++) {
        if (used_cols & BIT(GPIO_COLS + k)) {
            exp_col_mask |= BIT(exp_cols[k]);
        }
    }
#endif

    for (size_t r = 0; r < ROWS; r++) {
        gpio_init_callback(&row_cbs[r], irq_handler, BIT(rows[r].pin));
        int err = gpio_add_callback(rows[r].port, &row_cbs[r]);
        if (err) {
            LOG_ERR("Failed to add the callback for row %u (%d)", r, err);
            return err;
        }
    }

#if DIRECT
    for (size_t i = 0; i < DIRECT; i++) {
        if (USED_DIRECT & BIT(i)) {
            direct_mask |= BIT(direct_pins[i]);
        }
    }

    gpio_init_callback(&int_cb, irq_handler, BIT(int_pin.pin));
    int err = gpio_add_callback(int_pin.port, &int_cb);
    if (err) {
        LOG_ERR("Failed to add the callback for INT (%d)", err);
        return err;
    }
#endif

    // Pins are set up on resume (ZMK resumes the chosen matrix-input)
    pm_device_init_suspended(dev);
#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)
    pm_device_runtime_enable(dev);
#endif

    return 0;
}

PM_DEVICE_DT_INST_DEFINE(0, pm_action);

DEVICE_DT_INST_DEFINE(0, matrix_init, PM_DEVICE_DT_INST_GET(0), NULL, NULL, POST_KERNEL,
                      CONFIG_INPUT_INIT_PRIORITY, NULL);
