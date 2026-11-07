/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/*
 * The adapter's PCAL6416A (U1), register by register. Each call writes only
 * the register bytes it changes, one I2C transaction at most per register
 * pair: a matrix column step or a status LED is a single 1-byte write. The
 * calls are thread-safe; while the expander is not initialized they return
 * -ENODEV. Pins 0-15 are P0_0..P1_7.
 */

// Cached register pairs, bit n = pin n
enum rethink_exp_reg {
    RETHINK_EXP_OUTPUT,   // output latch: 1 = high
    RETHINK_EXP_CONFIG,   // direction: 1 = input, 0 = output
    RETHINK_EXP_PULL_EN,  // 1 = pull resistor connected
    RETHINK_EXP_PULL_SEL, // 1 = pull-up, 0 = pull-down
    RETHINK_EXP_INT_MASK, // 1 = no interrupt from this input
    RETHINK_EXP_REG_COUNT,
};

enum rethink_exp_mode {
    RETHINK_EXP_OUT_LOW,
    RETHINK_EXP_OUT_HIGH,
    RETHINK_EXP_IN,
    RETHINK_EXP_IN_PULL_UP,
    RETHINK_EXP_IN_PULL_DOWN,
};

// Probes U1 and writes a known state to every register, since U1 keeps its
// state across Control Board resets: all inputs without pulls, output latches
// low, interrupts masked, pending interrupt cleared.
int rethink_exp_init(void);

// Bits of mask take the values in value. Nothing is written if they already do.
int rethink_exp_update(enum rethink_exp_reg reg, uint16_t mask, uint16_t value);

// Output modes drop the pull, input modes set it; latch, direction and pull
// change in the order that avoids glitches.
int rethink_exp_configure(uint8_t pin, enum rethink_exp_mode mode);

// Pin levels (input port registers). Reading clears a pending interrupt.
int rethink_exp_read(uint16_t *levels);
