/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// What the last reset left behind: its cause (hwinfo, read and cleared at
// boot) and a record written just before it.
//
// - A fatal error (fault, assert, stack overflow) records the thread, its
//   pc and lr and the code addresses on its stack, then reboots at once.
//   Without this Zephyr halts, and the watchdog resets the board 8 s later.
// - A stall: the system work queue, which feeds the watchdog, ran nothing
//   for STALL_MS. A timer interrupt records what the work queue waits on,
//   the thread running instead and the code addresses on both stacks; the
//   watchdog resets the board a few seconds later. A watchdog reset without
//   a record means the timer interrupt didn't run: the system timer stopped
//   (a lost GRTC compare event, DevZone 129122), or interrupts took all
//   the time.
//
// The record is kept in the last 256 bytes of RAM, which the linker leaves
// unused (it rounds RAM down to whole kilobytes; MCUboot too) and a reset
// doesn't clear. The addresses map to functions with the .elf of the build
// that wrote the record (addr2line). The record carries that build's end of
// code (__text_region_end), and scripts/build.sh keeps each .elf as
// out/elf/<artifact>-<that address>.elf.

#include <stddef.h>
#include <string.h>

#include <cmsis_core.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <rethink/reset_diag.h>

LOG_MODULE_REGISTER(rethink_diag, LOG_LEVEL_INF);

#define RAM_NODE DT_NODELABEL(cpuapp_sram)
#define RECORD_SIZE 256
#define RECORD_ADDR (DT_REG_ADDR(RAM_NODE) + DT_REG_SIZE(RAM_NODE) - RECORD_SIZE)
// The RAM the linker uses
#define LINKED_RAM_END (CONFIG_SRAM_BASE_ADDRESS + CONFIG_SRAM_SIZE * 1024)
BUILD_ASSERT(RECORD_ADDR >= LINKED_RAM_END, "The record must lie past the linked RAM");

#define MAGIC 0x52544447
#define TRACE_LEN 10
// Without the work queue this long: a stall. The watchdog (8 s, fed by the
// work queue every 2 s) resets the board from 6 s on.
#define STALL_MS 4000

enum kind {
    KIND_NONE,
    KIND_FATAL,
    KIND_STALL,
};

struct trace {
    char name[16];
    uint32_t pc;
    uint32_t lr;
    // Code addresses on the stack, nearest first: return addresses mostly
    uint32_t addr[TRACE_LEN];
};

struct record {
    uint32_t magic;
    // The build that wrote it: its end of code
    uint32_t build;
    uint32_t kind;
    // Fatal: K_ERR_*; stall: ms without the work queue
    uint32_t reason;
    uint32_t uptime_ms;
    // Fatal: 1 if in an interrupt
    uint32_t in_isr;
    // Stall: the work queue's thread state and what it waits on
    uint32_t wq_state;
    uint32_t wq_pended_on;
    // Fatal: the thread at fault; stall: the thread running
    struct trace running;
    // Stall: the system work queue, unless it was the one running
    struct trace wq;
    uint32_t check;
};
BUILD_ASSERT(sizeof(struct record) <= RECORD_SIZE);

static struct record *const saved = (struct record *)RECORD_ADDR;
// The record from before this boot
static struct record last;
// hwinfo RESET_* flags
static uint32_t reset_cause;

static uint32_t checksum(const struct record *r) {
    const uint32_t *w = (const uint32_t *)r;
    uint32_t sum = 0x5a5a5a5a;

    for (size_t i = 0; i < offsetof(struct record, check) / sizeof(uint32_t); i++) {
        sum = ((sum << 1) | (sum >> 31)) ^ w[i];
    }
    return sum;
}

static uint32_t build_stamp(void) { return (uintptr_t)__text_region_end; }

static void save(const struct record *r) {
    *saved = *r;
    saved->build = build_stamp();
    saved->check = checksum(saved);
}

static bool code_addr(uint32_t v) {
    // Thumb: odd
    return (v & 1) && v >= (uintptr_t)__text_region_start && v < (uintptr_t)__text_region_end;
}

static void note_name(struct trace *t, struct k_thread *thread) {
    const char *name = k_thread_name_get(thread);

    if (name && name[0]) {
        strncpy(t->name, name, sizeof(t->name) - 1);
    } else {
        snprintk(t->name, sizeof(t->name), "%p", thread);
    }
}

// The end of the thread's stack if sp lies in it, else 0
static uintptr_t stack_end(const struct k_thread *thread, uintptr_t sp) {
    uintptr_t start = thread->stack_info.start;
    uintptr_t end = start + thread->stack_info.size;

    return sp >= start && sp < end ? end : 0;
}

// pc and lr from the exception frame at sp, and the code addresses above it
// up to end
static void note_frame(struct trace *t, uintptr_t sp, uintptr_t end) {
    // r0-r3, r12, lr, pc, xpsr
    const uint32_t *frame = (const uint32_t *)sp;
    int n = 0;

    if (!sp || sp % 4 || sp + 8 * sizeof(uint32_t) > end) {
        return;
    }
    t->lr = frame[5];
    t->pc = frame[6];
    for (const uint32_t *p = frame + 8; (uintptr_t)p < end && n < TRACE_LEN; p++) {
        if (code_addr(*p)) {
            t->addr[n++] = *p;
        }
    }
}

// A thread stopped with its exception frame at sp
static void note_thread(struct trace *t, struct k_thread *thread, uintptr_t sp) {
    note_name(t, thread);
    note_frame(t, sp, stack_end(thread, sp));
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    struct record r = {
        .magic = MAGIC,
        .kind = KIND_FATAL,
        .reason = reason,
        .uptime_ms = k_uptime_get_32(),
    };

    struct k_thread *thread = k_current_get();
    uintptr_t sp = (uintptr_t)esf;
    uintptr_t end = stack_end(thread, sp);

    note_name(&r.running, thread);
    // Not on the thread's stack: on the interrupt stack
    if (!end && sp >= CONFIG_SRAM_BASE_ADDRESS && sp < LINKED_RAM_END) {
        r.in_isr = 1;
        end = MIN(sp + 1024, LINKED_RAM_END);
    }
    note_frame(&r.running, sp, end);
    save(&r);

    // No LOG_PANIC: the logs can't leave over USB from here
    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}

static atomic_t beat;

static void heartbeat(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(beat_work, heartbeat);

static void heartbeat(struct k_work *work) {
    atomic_set(&beat, k_uptime_get_32());
    k_work_schedule(&beat_work, K_SECONDS(1));
}

// In the timer interrupt: the thread it interrupted is k_current_get(), its
// exception frame at PSP
static void check(struct k_timer *timer) {
    static bool recorded;
    uint32_t now = k_uptime_get_32();
    uint32_t silent = now - (uint32_t)atomic_get(&beat);

    if (silent < STALL_MS) {
        recorded = false;
        return;
    }
    if (recorded) {
        return;
    }
    recorded = true;

    struct k_thread *wq = k_work_queue_thread_get(&k_sys_work_q);
    struct k_thread *running = k_current_get();
    struct record r = {
        .magic = MAGIC,
        .kind = KIND_STALL,
        .reason = silent,
        .uptime_ms = now,
        .wq_state = wq->base.thread_state,
        .wq_pended_on = (uintptr_t)wq->base.pended_on,
    };

    note_thread(&r.running, running, __get_PSP());
    if (running != wq) {
        note_thread(&r.wq, wq, wq->callee_saved.psp);
    }
    save(&r);
    LOG_ERR("System work queue stalled for %u ms (recorded for after the reset)", silent);
}

static K_TIMER_DEFINE(check_timer, check, NULL);

static const struct {
    uint32_t flag;
    const char *name;
} causes[] = {
    {RESET_PIN, "reset pin"},
    {RESET_SOFTWARE, "software (a reboot, or after a fatal error)"},
    {RESET_BROWNOUT, "brown-out"},
    // On the nRF54L, Zephyr's POR is VBUS waking the chip from System OFF;
    // a real power-on records nothing (cause_str)
    {RESET_POR, "USB woke it from System OFF"},
    {RESET_WATCHDOG, "watchdog: the system work queue stopped"},
    {RESET_DEBUG, "debugger"},
    {RESET_SECURITY, "security"},
    // A GPIO sense (LPCOMP and NFC, the other sources, aren't used)
    {RESET_LOW_POWER_WAKE, "a key or the mode switch woke it from System OFF"},
    {RESET_CPU_LOCKUP, "CPU lockup"},
    {RESET_CLOCK, "clock"},
    {RESET_HARDWARE, "hardware"},
};

static void cause_str(char *buf, size_t len) {
    uint32_t named = 0;
    size_t pos = 0;

    // The nRF54L records no reason for these
    if (!reset_cause) {
        snprintk(buf, len, "power-on or brown-out (no reason recorded)");
        return;
    }

    buf[0] = '\0';
    for (size_t i = 0; i < ARRAY_SIZE(causes) && pos < len; i++) {
        if (reset_cause & causes[i].flag) {
            named |= causes[i].flag;
            pos += snprintk(buf + pos, len - pos, "%s%s", pos ? ", " : "", causes[i].name);
        }
    }
    if ((reset_cause & ~named) && pos < len) {
        snprintk(buf + pos, len - pos, "%sflags 0x%x", pos ? ", " : "", reset_cause & ~named);
    }
}

static const char *fatal_str(uint32_t reason) {
    switch (reason) {
    case K_ERR_CPU_EXCEPTION:
        return "CPU exception (fault)";
    case K_ERR_SPURIOUS_IRQ:
        return "spurious interrupt";
    case K_ERR_STACK_CHK_FAIL:
        return "stack overflow";
    case K_ERR_KERNEL_OOPS:
        return "oops (assert)";
    case K_ERR_KERNEL_PANIC:
        return "panic";
    default:
        return "ARM fault";
    }
}

static void print_trace(const struct shell *sh, const struct trace *t) {
    char buf[TRACE_LEN * 11 + 1];
    size_t pos = 0;

    buf[0] = '\0';
    for (int i = 0; i < TRACE_LEN && t->addr[i]; i++) {
        pos += snprintk(buf + pos, sizeof(buf) - pos, " %08x", t->addr[i]);
    }
    shell_print(sh, "  %s: pc %08x lr %08x; on its stack:%s", t->name, t->pc, t->lr,
                pos ? buf : " nothing");
}

void rethink_reset_diag_print(const struct shell *sh) {
    char buf[112];

    cause_str(buf, sizeof(buf));
    shell_print(sh, "Last reset: %s", buf);

    switch (last.kind) {
    case KIND_FATAL:
        shell_print(sh, "Before it, at %u.%03u s: fatal error %u, %s, %s", last.uptime_ms / 1000,
                    last.uptime_ms % 1000, last.reason, fatal_str(last.reason),
                    last.in_isr ? "in an interrupt during" : "in");
        print_trace(sh, &last.running);
        break;
    case KIND_STALL:
        // The work queue's trace stays empty when it was the one running
        if (!last.wq.name[0]) {
            shell_print(sh,
                        "Before it, at %u.%03u s: the system work queue was busy for %u ms "
                        "with one item:",
                        last.uptime_ms / 1000, last.uptime_ms % 1000, last.reason);
            print_trace(sh, &last.running);
            break;
        }
        shell_print(sh,
                    "Before it, at %u.%03u s: the system work queue had run nothing for %u ms; "
                    "state 0x%02x, waiting on %08x:",
                    last.uptime_ms / 1000, last.uptime_ms % 1000, last.reason, last.wq_state,
                    last.wq_pended_on);
        print_trace(sh, &last.wq);
        shell_print(sh, "  Running instead:");
        print_trace(sh, &last.running);
        break;
    default:
        if (reset_cause & RESET_WATCHDOG) {
            // The check runs on a kernel timer
            shell_print(sh, "Nothing recorded before it: the system timer stopped, or "
                            "interrupts took all the time");
        }
        return;
    }
    shell_print(sh, "Addresses: addr2line with out/elf/<build>-%08x.elf%s (README: ble log)",
                last.build, last.build == build_stamp() ? "" : ", an earlier build");
}

static int reset_diag_init(void) {
    char buf[112];

#if IS_ENABLED(CONFIG_HWINFO)
    if (hwinfo_get_reset_cause(&reset_cause) == 0) {
        hwinfo_clear_reset_cause();
    }
#endif
    if (saved->magic == MAGIC && saved->check == checksum(saved)) {
        last = *saved;
    }
    memset(saved, 0, sizeof(*saved));

    cause_str(buf, sizeof(buf));
    LOG_INF("Reset cause: %s%s", buf,
            last.kind == KIND_FATAL   ? "; a fatal error was recorded (ble log)"
            : last.kind == KIND_STALL ? "; a stall was recorded (ble log)"
                                      : "");

    atomic_set(&beat, k_uptime_get_32());
    k_work_schedule(&beat_work, K_SECONDS(1));
    k_timer_start(&check_timer, K_MSEC(500), K_MSEC(500));
    return 0;
}

SYS_INIT(reset_diag_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

// Tests of both records

static int cmd_crash(const struct shell *sh, size_t argc, char **argv) {
    shell_print(sh, "Panic: a fatal error record, then a reboot");
    k_msleep(100);
    k_panic();
    return 0;
}

// Busy, so the work queue (cooperative) runs nothing else
static void stall(struct k_work *work) { k_busy_wait(10 * USEC_PER_SEC); }

static K_WORK_DEFINE(stall_work, stall);

static int cmd_stall(const struct shell *sh, size_t argc, char **argv) {
    shell_print(sh, "Blocking the system work queue: a stall record after 4 s, the watchdog "
                    "reset within 8 s");
    k_work_submit(&stall_work);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(diag_cmds,
                               SHELL_CMD(crash, NULL, "Panic: tests the fatal error record",
                                         cmd_crash),
                               SHELL_CMD(stall, NULL,
                                         "Block the system work queue: tests the stall record "
                                         "and the watchdog",
                                         cmd_stall),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(diag, &diag_cmds, "Tests of the reset records shown by \"ble log\"", NULL);
