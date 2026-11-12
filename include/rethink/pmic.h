/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

#pragma once

// Stops this module's own accesses to the nPM1300 (the shell's sampling), for
// the quiet time before System OFF: an access right before it let the system
// timer wake the nRF54L again at once (src/power.c)
void rethink_pmic_quiet(void);

// Switches BUCK2 off and on if BUCKSTATUS shows it stuck in PWM mode, for the
// battery check (src/pmic.c)
#if IS_ENABLED(CONFIG_RETHINK_BUCK2_RESTART)
void rethink_pmic_buck2_check(void);
#else
static inline void rethink_pmic_buck2_check(void) {}
#endif
