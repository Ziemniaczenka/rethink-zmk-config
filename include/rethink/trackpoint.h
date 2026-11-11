/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

// Pointing stick on the adapter (rethink,trackpoint): power, reset, the PS/2
// link and pointing. The stick either streams movement to ZMK (pointing, set
// up by rethink_tp_start()) or the link is raw: bytes from the stick are
// queued for rethink_tp_read(). Raw access is for one thread at a time.

// Powers the stick if needed (otherwise resets it with 0xFF), waits for its
// self-test result, reads its TrackPoint ID and turns streaming on: movement
// and buttons then go to ZMK's pointing input. Runs at boot. Without an
// answer the stick is powered off again and -ETIMEDOUT returned; -ENODEV
// without the adapter.
int rethink_tp_start(void);

// Supply on (TP_3V3, and the charge pump for a 5 V stick) with the reset
// held, then the reset released after reset-ms. The stick then runs its
// self-test and sends 0xAA 0x00. The link is raw. -ENODEV without the adapter.
int rethink_tp_power_on(void);

// Lines released, supply off, reset back on its idle pull-down. Also before
// System OFF: the Control Board keeps TP_3V3 on otherwise.
void rethink_tp_power_off(void);

bool rethink_tp_powered(void);

// True while movement goes to ZMK
bool rethink_tp_pointing(void);

// TP_OFF, TP_ON, TP_TOG, TP_RECAL, TP_RATE_UP, TP_RATE_DN, TP_PTS_TOG or
// TP_SCROLL_TOG (dt-bindings/rethink/trackpoint.h), carried out by the
// stick's thread; TP_TOG, the new rate, press-to-select and middle-button
// scrolling (settings, so saved) are decided now. Returns at once, -EINVAL for
// anything else. The thread also powers the stick off once neither it nor a
// key was used for CONFIG_RETHINK_TRACKPOINT_IDLE_OFF_MIN; the next key press
// starts it again.
int rethink_tp_request(uint32_t cmd);

// Settings. Kconfig gives the defaults; a changed one is kept in flash
// (saved a few seconds after the last change). The thread writes the stick's
// own to it after each start, and while pointing as they change.
enum rethink_tp_setting {
    // Reports per second (PS/2 sample rate): 10, 20, 40, 60, 80, 100 or 200,
    // on battery and with USB power. With CONFIG_RETHINK_TRACKPOINT_SLOW_IDLE
    // 80/s while the stick is still.
    RETHINK_TP_RATE,
    RETHINK_TP_RATE_USB,
    // The stick's: force scale (128 = x1.0)
    RETHINK_TP_SENSITIVITY,
    // The stick's, IBM sticks only: negative inertia, upper plateau speed,
    // drift correction time
    RETHINK_TP_INERTIA,
    RETHINK_TP_SPEED,
    RETHINK_TP_DRIFT_TIME,
    // The stick's: clicking by pressing the stick (0 or 1)
    RETHINK_TP_PRESS_TO_SELECT,
    // Milliseconds the reset is held after power-on
    RETHINK_TP_RESET_MS,
#if IS_ENABLED(CONFIG_RETHINK_TRACKPOINT_SCROLL)
    // The middle button held scrolls (1) or is a plain middle button (0);
    // scrolling: stick counts per wheel step, sideways counts per report
    // that don't scroll
    RETHINK_TP_MIDDLE_SCROLL,
    RETHINK_TP_SCROLL_DIVISOR,
    RETHINK_TP_SCROLL_DEAD_ZONE,
#endif
    RETHINK_TP_SETTINGS,
};

struct rethink_tp_setting_info {
    const char *name;
    int def;
    int min;
    int max;
    const char *help;
};

// Name, default, range and description; NULL past the last setting
const struct rethink_tp_setting_info *rethink_tp_setting_info(enum rethink_tp_setting setting);

int rethink_tp_setting_get(enum rethink_tp_setting setting);

// Takes effect at once (the stick's while pointing: the thread pauses
// streaming for it). -EINVAL out of range.
int rethink_tp_setting_set(enum rethink_tp_setting setting, int value);

// Every setting back to its default; the saved values are deleted
void rethink_tp_settings_reset(void);

// The stick's present report rate, 0 when not pointing
int rethink_tp_rate_now(void);

// While pointing: streaming off, the present X, Y and Z forces become the
// zero point (E2 51, about 310 ms), streaming on. -ENODEV when not pointing.
int rethink_tp_recalibrate(void);

// Stops pointing: from now on bytes from the stick queue for rethink_tp_read()
void rethink_tp_set_raw(void);

// Pointing again after rethink_tp_set_raw(), for a stick still streaming
void rethink_tp_resume(void);

// One byte to the stick: inhibit, request to send, 8 data bits, odd parity,
// stop, and the stick's acknowledge bit. Its reply (0xFA, ...) arrives
// through rethink_tp_read() while raw. -ETIMEDOUT if the stick doesn't clock
// the byte in, -EIO if it doesn't acknowledge it, -ENODEV while powered off.
int rethink_tp_write(uint8_t byte);

// Next byte from the stick while raw. An error if none arrives in time.
int rethink_tp_read(uint8_t *byte, k_timeout_t timeout);

// Drops the bytes queued so far
void rethink_tp_flush(void);

struct rethink_tp_stats {
    uint32_t bytes;     // received and valid
    uint32_t framing;   // start bit 1 or stop bit 0
    uint32_t parity;    // even parity
    uint32_t gaps;      // byte broken off: no clock edge for too long
    uint32_t overruns;  // queue full, byte or packet dropped
    uint32_t packets;   // movement packets while pointing
    uint32_t resyncs;   // packets dropped to find the next packet start
    uint32_t resent;    // packets with an error, sent again on request (0xFE)
    uint32_t lost;      // packets with an error that didn't come again
    // Report rate or stick settings changed while pointing
    uint32_t rate_changes;
    uint32_t tx_bytes;  // sent and acknowledged
    uint32_t tx_timeouts;
    uint32_t tx_nacks;
    uint16_t period_us; // clock period of the last received byte
    uint16_t period_min_us;
    uint16_t period_max_us;
    // The last start: from power-on or the reset command to pointing, the
    // reset held after power-on (0 for a reset command), from the reset to
    // the self-test result
    uint16_t start_ms;
    uint16_t reset_ms;
    uint16_t self_test_ms;
};

void rethink_tp_stats_get(struct rethink_tp_stats *stats);
void rethink_tp_stats_reset(void);
