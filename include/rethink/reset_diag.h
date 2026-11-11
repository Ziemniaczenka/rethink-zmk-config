/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/shell/shell.h>

// The last reset's cause and what the firmware recorded just before it (a
// fatal error or a stalled system work queue), for "ble log"
void rethink_reset_diag_print(const struct shell *sh);
