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

// Stops pointing: from now on bytes from the stick queue for rethink_tp_read()
void rethink_tp_set_raw(void);

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
    uint32_t tx_bytes;  // sent and acknowledged
    uint32_t tx_timeouts;
    uint32_t tx_nacks;
    uint16_t period_us; // clock period of the last received byte
    uint16_t period_min_us;
    uint16_t period_max_us;
};

void rethink_tp_stats_get(struct rethink_tp_stats *stats);
void rethink_tp_stats_reset(void);
