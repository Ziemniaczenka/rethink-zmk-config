/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

// What may wake the board besides USB power, which always does
#define RETHINK_WAKE_KEYS BIT(0)   // a key press: the matrix stays armed
#define RETHINK_WAKE_SWITCH BIT(1) // the mode switch moved from where it is

// Switches the stick, the adapter, the backlight and the LEDs off and enters
// System OFF. Waking reboots the board.
FUNC_NORETURN void rethink_power_off(uint32_t wake);
