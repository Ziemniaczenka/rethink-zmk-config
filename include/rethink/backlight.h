/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#pragma once

// Allow the backlight power chain without a detected backlit keyboard, until reboot
void rethink_backlight_unlock(void);

// Switches the power chain off for good, ignoring later brightness changes:
// for the power-off, before its quiet time, since switching BUCK1 off is an
// access to the nPM1300 (src/power.c)
void rethink_backlight_shutdown(void);
