/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// PCAL6416A register access with a cache. Zephyr's PCAL64xxA driver rewrites
// all eight configuration registers on every pin change; here a change costs
// one transaction for the register pair it touches, or none.

#define DT_DRV_COMPAT rethink_adapter_ctrl

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <rethink/expander.h>

static const struct i2c_dt_spec i2c = I2C_DT_SPEC_GET(DT_INST_PHANDLE(0, expander));

#define REG_INPUT 0x00
#define REG_POLARITY 0x04
#define REG_INPUT_LATCH 0x44
#define REG_OUTPUT_PORT_CONFIG 0x4f

// First register of each cached pair; the device steps to the second by itself
static const uint8_t reg_addr[RETHINK_EXP_REG_COUNT] = {
    [RETHINK_EXP_OUTPUT] = 0x02,   [RETHINK_EXP_CONFIG] = 0x06,
    [RETHINK_EXP_PULL_EN] = 0x46,  [RETHINK_EXP_PULL_SEL] = 0x48,
    [RETHINK_EXP_INT_MASK] = 0x4a,
};

// Written in this order: inputs first, so an output left on by an earlier
// firmware is released before anything else changes
static const struct {
    enum rethink_exp_reg reg;
    uint16_t value;
} init_state[] = {
    {RETHINK_EXP_CONFIG, 0xffff},   {RETHINK_EXP_OUTPUT, 0x0000},
    {RETHINK_EXP_PULL_SEL, 0xffff}, {RETHINK_EXP_PULL_EN, 0x0000},
    {RETHINK_EXP_INT_MASK, 0xffff},
};

static uint16_t regs[RETHINK_EXP_REG_COUNT];
static bool ready;
static K_MUTEX_DEFINE(lock);

// One transaction: the low byte, the high byte, or both
static int write_pair(uint8_t addr, uint16_t value, bool low, bool high) {
    uint8_t buf[3];
    size_t len = 0;

    buf[len++] = low ? addr : addr + 1;
    if (low) {
        buf[len++] = value & 0xff;
    }
    if (high) {
        buf[len++] = value >> 8;
    }

    return i2c_write_dt(&i2c, buf, len);
}

static int update_locked(enum rethink_exp_reg reg, uint16_t mask, uint16_t value) {
    uint16_t next = (regs[reg] & ~mask) | (value & mask);
    uint16_t changed = regs[reg] ^ next;

    if (!changed) {
        return 0;
    }

    int err = write_pair(reg_addr[reg], next, changed & 0x00ff, changed & 0xff00);
    if (!err) {
        regs[reg] = next;
    }

    return err;
}

static int read_locked(uint16_t *levels) {
    uint8_t buf[2];

    int err = i2c_burst_read_dt(&i2c, REG_INPUT, buf, sizeof(buf));
    if (!err) {
        *levels = buf[0] | (buf[1] << 8);
    }

    return err;
}

int rethink_exp_init(void) {
    int err = 0;
    uint16_t levels;

    if (!i2c_is_ready_dt(&i2c)) {
        return -ENODEV;
    }

    k_mutex_lock(&lock, K_FOREVER);
    ready = false;

    for (size_t i = 0; i < ARRAY_SIZE(init_state) && !err; i++) {
        enum rethink_exp_reg reg = init_state[i].reg;

        err = write_pair(reg_addr[reg], init_state[i].value, true, true);
        regs[reg] = init_state[i].value;
    }

    // No inversion, no latched inputs, push-pull outputs on both ports
    if (!err) {
        err = write_pair(REG_POLARITY, 0x0000, true, true);
    }
    if (!err) {
        err = write_pair(REG_INPUT_LATCH, 0x0000, true, true);
    }
    if (!err) {
        uint8_t buf[] = {REG_OUTPUT_PORT_CONFIG, 0x00};

        err = i2c_write_dt(&i2c, buf, sizeof(buf));
    }
    if (!err) {
        err = read_locked(&levels);
    }

    ready = !err;
    k_mutex_unlock(&lock);

    return err;
}

int rethink_exp_update(enum rethink_exp_reg reg, uint16_t mask, uint16_t value) {
    if (reg >= RETHINK_EXP_REG_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&lock, K_FOREVER);
    int err = ready ? update_locked(reg, mask, value) : -ENODEV;
    k_mutex_unlock(&lock);

    return err;
}

int rethink_exp_configure(uint8_t pin, enum rethink_exp_mode mode) {
    if (pin > 15) {
        return -EINVAL;
    }

    uint16_t bit = BIT(pin);
    int err;

    k_mutex_lock(&lock, K_FOREVER);
    if (!ready) {
        k_mutex_unlock(&lock);
        return -ENODEV;
    }

    switch (mode) {
    case RETHINK_EXP_OUT_LOW:
    case RETHINK_EXP_OUT_HIGH:
        // Latch first, so the pin starts at its level
        err = update_locked(RETHINK_EXP_OUTPUT, bit, mode == RETHINK_EXP_OUT_HIGH ? bit : 0);
        if (!err) {
            err = update_locked(RETHINK_EXP_CONFIG, bit, 0);
        }
        if (!err) {
            err = update_locked(RETHINK_EXP_PULL_EN, bit, 0);
        }
        break;
    case RETHINK_EXP_IN:
        err = update_locked(RETHINK_EXP_CONFIG, bit, bit);
        if (!err) {
            err = update_locked(RETHINK_EXP_PULL_EN, bit, 0);
        }
        break;
    case RETHINK_EXP_IN_PULL_UP:
    case RETHINK_EXP_IN_PULL_DOWN:
        // Pull first, so the pin rests on it when the driver lets go
        err = update_locked(RETHINK_EXP_PULL_SEL, bit, mode == RETHINK_EXP_IN_PULL_UP ? bit : 0);
        if (!err) {
            err = update_locked(RETHINK_EXP_PULL_EN, bit, bit);
        }
        if (!err) {
            err = update_locked(RETHINK_EXP_CONFIG, bit, bit);
        }
        break;
    default:
        err = -EINVAL;
    }

    k_mutex_unlock(&lock);
    return err;
}

int rethink_exp_read(uint16_t *levels) {
    k_mutex_lock(&lock, K_FOREVER);
    int err = ready ? read_locked(levels) : -ENODEV;
    k_mutex_unlock(&lock);

    return err;
}
