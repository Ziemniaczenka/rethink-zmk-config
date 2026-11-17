/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

// What may wake the board besides USB power, which always does
#define RETHINK_WAKE_KEYS BIT(0)   // a key press: the matrix stays armed
#define RETHINK_WAKE_SWITCH BIT(1) // the mode switch moved from where it is
// The nPM1300's ship mode instead of System OFF: everything off, only USB
// wakes it (CONFIG_RETHINK_LOW_BATTERY_SHIP_MODE, else System OFF)
#define RETHINK_POWER_SHIP BIT(2)

// Switches the stick, the adapter, the backlight and the LEDs off and enters
// System OFF (or ship mode). Waking reboots the board.
FUNC_NORETURN void rethink_power_off(uint32_t wake);

// Wired mode without USB power: the board powers off shortly
// (src/wired_off.c, CONFIG_RETHINK_WIRED_OFF)
#if IS_ENABLED(CONFIG_RETHINK_WIRED_OFF)
bool rethink_wired_off_pending(void);
#else
static inline bool rethink_wired_off_pending(void) { return false; }
#endif
