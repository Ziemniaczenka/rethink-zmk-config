/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// "tp" shell commands: power the pointing stick and talk PS/2 to it by hand.

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <rethink/trackpoint.h>

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#include <zmk/pointing/resolution_multipliers.h>
#endif

// Bytes closer together than this belong to one packet or reply
#define BURST_GAP_MS 3
// The stick acknowledges a byte (0xFA) within about 20 ms
#define ACK_TIMEOUT K_MSEC(25)

// Prints what the stick sends during the window, one line per burst
static int print_incoming(const struct shell *sh, int32_t window_ms) {
    int64_t start = k_uptime_get();
    int64_t last = 0;
    int count = 0;
    uint8_t byte;

    for (;;) {
        int64_t left = start + window_ms - k_uptime_get();

        if (left <= 0 || rethink_tp_read(&byte, K_MSEC(left)) != 0) {
            break;
        }

        int64_t now = k_uptime_get();

        if (count == 0 || now - last > BURST_GAP_MS) {
            shell_fprintf(sh, SHELL_NORMAL, "%s%6d ms:", count ? "\n" : "", (int)(now - start));
        }
        shell_fprintf(sh, SHELL_NORMAL, " %02x", byte);
        last = now;
        count++;
    }

    shell_print(sh, "%s", count ? "" : "(nothing)");
    return count;
}

// Raw access stops pointing, and streaming (f5) so packets don't get in the
// way of the replies
static bool take_raw(const struct shell *sh) {
    if (!rethink_tp_powered()) {
        shell_error(sh, "The stick is off: tp on");
        return false;
    }

    if (rethink_tp_pointing()) {
        uint8_t reply;

        rethink_tp_set_raw();
        rethink_tp_write(0xf5);
        k_msleep(30);
        while (rethink_tp_read(&reply, K_NO_WAIT) == 0) {
        }
        shell_print(sh, "Pointing paused, streaming off (f5). tp on resumes it.");
    }
    return true;
}

static int cmd_on(const struct shell *sh, size_t argc, char **argv) {
    int err = rethink_tp_start();

    if (err) {
        shell_error(sh, "Start failed (%d)%s", err,
                    err == -ENODEV ? ": the adapter did not answer at boot"
                                   : ": no answer from the stick, which is off again");
        return err;
    }

    shell_print(sh, "Pointing (self-test passed, streaming on; the log shows the ID)");
    return 0;
}

static int cmd_off(const struct shell *sh, size_t argc, char **argv) {
    rethink_tp_power_off();
    shell_print(sh, "Off");
    return 0;
}

// Each byte, then its acknowledge; the rest of the reply after the last one
static int cmd_send(const struct shell *sh, size_t argc, char **argv) {
    if (!take_raw(sh)) {
        return -ENODEV;
    }

    rethink_tp_flush();

    for (size_t i = 1; i < argc; i++) {
        char *end;
        unsigned long value = strtoul(argv[i], &end, 16);

        if (*end || value > 0xff) {
            shell_error(sh, "Not a hex byte: %s", argv[i]);
            return -EINVAL;
        }

        int err = rethink_tp_write(value);
        if (err) {
            shell_error(sh, "%02lx: %s (%d)", value,
                        err == -ETIMEDOUT ? "the stick did not clock it in"
                                          : "the stick did not acknowledge it",
                        err);
            return err;
        }

        uint8_t ack;

        if (rethink_tp_read(&ack, ACK_TIMEOUT) != 0) {
            shell_error(sh, "%02lx: no reply", value);
            return -ETIMEDOUT;
        }
        shell_print(sh, "%02lx: %02x%s", value, ack,
                    ack == 0xfa   ? " (ack)"
                    : ack == 0xfe ? " (resend)"
                    : ack == 0xfc ? " (error)"
                                  : "");
    }

    shell_print(sh, "Then:");
    print_incoming(sh, 1000);
    return 0;
}

// While pointing, the packets are shown instead of reported, then pointing
// resumes
static int cmd_read(const struct shell *sh, size_t argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 5;

    if (seconds <= 0 || seconds > 60) {
        shell_error(sh, "1-60 seconds");
        return -EINVAL;
    }
    if (!rethink_tp_powered()) {
        shell_error(sh, "The stick is off: tp on");
        return -ENODEV;
    }

    bool was_pointing = rethink_tp_pointing();

    if (was_pointing) {
        rethink_tp_set_raw();
        shell_print(sh, "Packets shown instead of reported for %d s:", seconds);
    }
    print_incoming(sh, seconds * 1000);
    if (was_pointing) {
        rethink_tp_resume();
        shell_print(sh, "Pointing again");
    }
    return 0;
}

// A setting by its name or the start of one: -1 none, -2 more than one
static int find_setting(const char *arg) {
    size_t len = strlen(arg);
    int found = -1;

    for (int s = 0; s < RETHINK_TP_SETTINGS; s++) {
        const char *name = rethink_tp_setting_info(s)->name;

        if (strcmp(name, arg) == 0) {
            return s;
        }
        if (strncmp(name, arg, len) == 0) {
            if (found >= 0) {
                return -2;
            }
            found = s;
        }
    }
    return found;
}

static void print_setting(const struct shell *sh, int s) {
    const struct rethink_tp_setting_info *info = rethink_tp_setting_info(s);
    int value = rethink_tp_setting_get(s);

    shell_print(sh, "%-16s %3d%c  %s (%d-%d, default %d)", info->name, value,
                value != info->def ? '*' : ' ', info->help, info->min, info->max, info->def);
}

// The stick's settings are written to it at once while pointing
static int cmd_set(const struct shell *sh, size_t argc, char **argv) {
    if (argc == 1) {
        for (int s = 0; s < RETHINK_TP_SETTINGS; s++) {
            print_setting(sh, s);
        }
        shell_print(sh, "*: changed, kept in flash. \"tp set defaults\" puts all back.");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "defaults") == 0) {
        rethink_tp_settings_reset();
        shell_print(sh, "All settings at their defaults");
        return 0;
    }

    int s = find_setting(argv[1]);

    if (s < 0) {
        shell_error(sh, "%s: %s", argv[1],
                    s == -2 ? "more than one setting starts so" : "no such setting (tp set)");
        return -EINVAL;
    }
    if (argc > 2) {
        const struct rethink_tp_setting_info *info = rethink_tp_setting_info(s);
        char *end;
        long value = strtol(argv[2], &end, 0);

        if (*end || rethink_tp_setting_set(s, value)) {
            shell_error(sh, "%s: %d-%d, %s", info->name, info->min, info->max, info->help);
            return -EINVAL;
        }
    }
    print_setting(sh, s);
    return 0;
}

static int cmd_stat(const struct shell *sh, size_t argc, char **argv) {
    struct rethink_tp_stats s;

    rethink_tp_stats_get(&s);

    shell_print(sh, "Stick: %s", !rethink_tp_powered() ? "off"
                                 : rethink_tp_pointing() ? "pointing"
                                                         : "on, raw");
    shell_print(sh, "Received: %u bytes; errors: %u framing, %u parity, %u broken off, %u dropped",
                s.bytes, s.framing, s.parity, s.gaps, s.overruns);
    shell_print(sh, "Packets: %u reported, %u resyncs; with an error: %u sent again, %u lost",
                s.packets, s.resyncs, s.resent, s.lost);

    int now = rethink_tp_rate_now();

    if (now) {
        shell_print(sh, "Rate: %d/s set, %d/s now; %u changes of rate or settings",
                    rethink_tp_setting_get(RETHINK_TP_RATE), now, s.rate_changes);
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
    // 16 once a host with smooth scrolling has set it, 1 otherwise
    struct zmk_pointing_resolution_multipliers m =
        zmk_pointing_resolution_multipliers_get_current_profile();

    shell_print(sh, "Wheel resolution multiplier (this host): %d, sideways %d", m.wheel + 1,
                m.hor_wheel + 1);
#endif
    shell_print(sh, "Sent: %u bytes; %u not clocked in, %u not acknowledged", s.tx_bytes,
                s.tx_timeouts, s.tx_nacks);
    if (s.period_us) {
        shell_print(sh, "Clock period: %u us last (%u kHz), %u-%u us", s.period_us,
                    1000 / s.period_us, s.period_min_us, s.period_max_us);
    }

    if (argc > 1) {
        rethink_tp_stats_reset();
        shell_print(sh, "Counters cleared");
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    tp_cmds,
    SHELL_CMD(on, NULL, "Power (or reset) the stick and start pointing", cmd_on),
    SHELL_CMD(off, NULL, "Power the stick off", cmd_off),
    SHELL_CMD_ARG(send, NULL,
                  "<hex> [hex...]: bytes to the stick, e.g. e1 (ID), f4 (stream). Pauses pointing",
                  cmd_send, 2, 15),
    SHELL_CMD_ARG(read, NULL,
                  "[seconds]: show what the stick sends (default 5); while pointing, the packets",
                  cmd_read, 1, 1),
    SHELL_CMD_ARG(set, NULL,
                  "[name [value]] | defaults: rate, the stick's settings, scrolling; saved",
                  cmd_set, 1, 2),
    SHELL_CMD_ARG(stat, NULL, "[clear]: link counters, rate, clock period", cmd_stat, 1, 1),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(tp, &tp_cmds, "Pointing stick (PS/2) by hand", NULL);
