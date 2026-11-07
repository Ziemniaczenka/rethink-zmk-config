/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

// The *-led-gpios properties of rethink,adapter-ctrl
enum rethink_led {
    RETHINK_LED_PWR, // power LED (Mezzanine) or Fn lock (FFC2)
    RETHINK_LED_MUTE,
    RETHINK_LED_MICMUTE,
    RETHINK_LED_CAPSLOCK,
    RETHINK_LED_NUMLOCK,
    RETHINK_LED_COUNT,
};

enum rethink_bl_detect {
    RETHINK_BL_UNKNOWN, // no adapter, or no bl-detect-gpios
    RETHINK_BL_ABSENT,
    RETHINK_BL_PRESENT,
};

// True once the PCAL6416A answered at boot
bool rethink_adapter_ready(void);

// KBD_BL_DTCT as last read: at init, then by rethink_adapter_bl_redetect()
enum rethink_bl_detect rethink_adapter_bl_detect(void);

// Reads KBD_BL_DTCT again (pull-up on for 1 ms), for a keyboard connected or
// swapped since boot. Unknown without the adapter or on an I2C error.
enum rethink_bl_detect rethink_adapter_bl_redetect(void);

const char *rethink_bl_detect_name(enum rethink_bl_detect state);

// -ENODEV without the adapter, -ENOTSUP without boost-en-gpios or with
// CONFIG_RETHINK_BOOST_EN_TIED (the boost then follows VLED_BL)
int rethink_adapter_boost_set(bool on);

// -ENODEV without the adapter, -ENOTSUP if the keyboard has no such LED
int rethink_adapter_led_set(enum rethink_led led, bool on);

const char *rethink_adapter_led_name(enum rethink_led led);

// Pins nothing drives back to their resting levels (idle-gpios, KBD_BL_DTCT),
// status LEDs off. For code that reconfigured them, such as the bring-up tests.
void rethink_adapter_park(void);

// Before System OFF: pins parked as above, pump and boost disabled, TP_3V3
// released. The expander keeps its outputs while the Control Board is off.
void rethink_adapter_power_down(void);
