/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#ifndef RETHINK_DT_BINDINGS_TRACKPOINT_H
#define RETHINK_DT_BINDINGS_TRACKPOINT_H

// Parameters of &tp (rethink,behavior-trackpoint)

// Powers the stick off; keys don't wake it
#define TP_OFF 0
// Starts it: supply, self-test, streaming (about a second)
#define TP_ON 1
// TP_OFF while pointing, else TP_ON
#define TP_TOG 2
// The present forces become the zero point (E2 51): hands off the stick
#define TP_RECAL 3
// Report rate one step up or down: 10, 20, 40, 60, 80, 100, 200 a second;
// the battery rate, or the USB one while USB powers the keyboard. Kept in
// flash, like the setting.
#define TP_RATE_UP 4
#define TP_RATE_DN 5
// Press-to-select (clicking by pressing the stick) on or off, kept in flash
#define TP_PTS_TOG 6

#endif
