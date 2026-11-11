/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// "ble" shell commands: the connection interval the host chose (how often
// the keyboard may send: a report waits at most one interval), asking the
// host for another, and a history of the connections since boot. The host
// decides the interval; Zephyr asks for CONFIG_BT_PERIPHERAL_PREF_* 5 s
// after connecting.
//
// The history keeps connections, disconnections with their reason,
// encryption, new connection parameters and USB power, so a drop-out on
// battery can be read once USB (and with it the shell) is back. It starts
// with the uptime and the last reset (src/reset_diag.c): a keyboard that
// rebooted shows it. Each event is also logged ("rethink_ble", info level),
// from the system work queue: the Bluetooth callbacks run on a small stack.

#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/spinlock.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_RETHINK_RESET_DIAG)
#include <rethink/reset_diag.h>
#endif

LOG_MODULE_REGISTER(rethink_ble, LOG_LEVEL_INF);

enum event_type {
    EV_CONNECTED,
    EV_CONNECT_FAILED,
    EV_DISCONNECTED,
    EV_SECURITY,
    EV_SECURITY_FAILED,
    EV_PARAMS,
    EV_USB,
};

struct event {
    uint32_t ms;
    uint8_t type;
    // Bonded profile, -1 for none
    int8_t profile;
    bool active;
    // Disconnect reason, error, security level or USB state
    uint8_t code;
    // Connection interval (1.25 ms units), peripheral latency, supervision
    // timeout (10 ms units)
    uint16_t interval;
    uint16_t latency;
    uint16_t timeout;
    bt_addr_le_t addr;
};

#define EVENTS 48

static struct event events[EVENTS];
// Events recorded; the last EVENTS are kept
static uint32_t recorded;
// Events logged so far
static uint32_t logged;
static struct k_spinlock lock;

static const char *reason_str(uint8_t reason) {
    switch (reason) {
    case BT_HCI_ERR_AUTH_FAIL:
        return "authentication failed";
    case BT_HCI_ERR_PIN_OR_KEY_MISSING:
        return "key missing";
    case BT_HCI_ERR_CONN_TIMEOUT:
        return "supervision timeout: radio contact lost";
    case BT_HCI_ERR_REMOTE_USER_TERM_CONN:
        return "ended by the host";
    case BT_HCI_ERR_REMOTE_LOW_RESOURCES:
        return "host low on resources";
    case BT_HCI_ERR_REMOTE_POWER_OFF:
        return "host powering off or sleeping";
    case BT_HCI_ERR_LOCALHOST_TERM_CONN:
        return "ended by the keyboard";
    case BT_HCI_ERR_UNSUPP_REMOTE_FEATURE:
        return "unsupported feature";
    case BT_HCI_ERR_LL_RESP_TIMEOUT:
        return "link layer response timeout";
    case BT_HCI_ERR_INSTANT_PASSED:
        return "instant passed";
    case BT_HCI_ERR_UNACCEPT_CONN_PARAM:
        return "unacceptable parameters";
    case BT_HCI_ERR_ADV_TIMEOUT:
        return "advertising timed out";
    case BT_HCI_ERR_TERM_DUE_TO_MIC_FAIL:
        return "encryption check (MIC) failed";
    case BT_HCI_ERR_CONN_FAIL_TO_ESTAB:
        return "failed to be established";
    default:
        return "";
    }
}

static const char *security_err_str(uint8_t err) {
    switch (err) {
    case BT_SECURITY_ERR_AUTH_FAIL:
        return "authentication failed";
    case BT_SECURITY_ERR_PIN_OR_KEY_MISSING:
        return "key missing: one side lost the pairing";
    case BT_SECURITY_ERR_AUTH_REQUIREMENT:
        return "authentication requirements not met";
    case BT_SECURITY_ERR_PAIR_NOT_ALLOWED:
        return "pairing not allowed";
    case BT_SECURITY_ERR_KEY_REJECTED:
        return "key rejected";
    default:
        return "";
    }
}

static void describe(const struct event *ev, char *buf, size_t len) {
    size_t pos = 0;

    buf[0] = '\0';
    if (ev->type == EV_USB) {
#if IS_ENABLED(CONFIG_ZMK_USB)
        static const char *const states[] = {
            [ZMK_USB_CONN_NONE] = "unplugged",
            [ZMK_USB_CONN_POWERED] = "powered",
            [ZMK_USB_CONN_HID] = "connected to a host",
        };

        snprintk(buf, len, "USB %s", ev->code < ARRAY_SIZE(states) ? states[ev->code] : "?");
#endif
        return;
    }

    char addr[BT_ADDR_STR_LEN];

    bt_addr_to_str(&ev->addr.a, addr, sizeof(addr));
    if (ev->profile >= 0) {
        pos = snprintk(buf, len, "%s profile %d%s: ", addr, ev->profile,
                       ev->active ? " (active)" : "");
    } else {
        pos = snprintk(buf, len, "%s not bonded: ", addr);
    }
    buf += MIN(pos, len);
    len -= MIN(pos, len);

    switch (ev->type) {
    case EV_CONNECTED:
        snprintk(buf, len, "connected, interval %u.%02u ms", ev->interval * 125 / 100,
                 ev->interval * 125 % 100);
        break;
    case EV_CONNECT_FAILED:
        snprintk(buf, len, "connection failed, 0x%02x %s", ev->code, reason_str(ev->code));
        break;
    case EV_DISCONNECTED:
        snprintk(buf, len, "disconnected, 0x%02x %s", ev->code, reason_str(ev->code));
        break;
    case EV_SECURITY:
        snprintk(buf, len, "security level %u", ev->code);
        break;
    case EV_SECURITY_FAILED:
        snprintk(buf, len, "security failed, error %u %s", ev->code, security_err_str(ev->code));
        break;
    case EV_PARAMS:
        snprintk(buf, len, "interval %u.%02u ms, latency %u, timeout %u ms",
                 ev->interval * 125 / 100, ev->interval * 125 % 100, ev->latency,
                 ev->timeout * 10);
        break;
    }
}

// The events recorded since the last run, to the log
static void log_events(struct k_work *work) {
    char buf[112];

    for (;;) {
        struct event ev;
        bool more = false;

        K_SPINLOCK(&lock) {
            if (recorded - logged > EVENTS) {
                logged = recorded - EVENTS;
            }
            if (logged < recorded) {
                ev = events[logged % EVENTS];
                logged++;
                more = true;
            }
        }
        if (!more) {
            return;
        }
        describe(&ev, buf, sizeof(buf));
        LOG_INF("%s", buf);
    }
}

static K_WORK_DEFINE(log_work, log_events);

static void record(struct event *ev, struct bt_conn *conn) {
    ev->ms = k_uptime_get_32();
    ev->profile = -1;

    if (conn) {
        struct bt_conn_info info;

        bt_addr_le_copy(&ev->addr, bt_conn_get_dst(conn));

        int profile = zmk_ble_profile_index(&ev->addr);

        if (profile >= 0) {
            ev->profile = profile;
            ev->active = profile == zmk_ble_active_profile_index();
        }
        if (bt_conn_get_info(conn, &info) == 0) {
            ev->interval = info.le.interval_us / 1250;
            ev->latency = info.le.latency;
            ev->timeout = info.le.timeout;
        }
    }

    K_SPINLOCK(&lock) {
        events[recorded % EVENTS] = *ev;
        recorded++;
    }
    k_work_submit(&log_work);
}

static void on_connected(struct bt_conn *conn, uint8_t err) {
    struct event ev = {.type = err ? EV_CONNECT_FAILED : EV_CONNECTED, .code = err};

    record(&ev, conn);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
    struct event ev = {.type = EV_DISCONNECTED, .code = reason};

    record(&ev, conn);
}

#if IS_ENABLED(CONFIG_BT_SMP)
static void on_security_changed(struct bt_conn *conn, bt_security_t level,
                                enum bt_security_err err) {
    struct event ev = {.type = err ? EV_SECURITY_FAILED : EV_SECURITY, .code = err ? err : level};

    record(&ev, conn);
}
#endif

static void on_params_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                              uint16_t timeout) {
    struct event ev = {.type = EV_PARAMS};

    record(&ev, conn);
}

BT_CONN_CB_DEFINE(rethink_ble_conn_cb) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
#if IS_ENABLED(CONFIG_BT_SMP)
    .security_changed = on_security_changed,
#endif
    .le_param_updated = on_params_updated,
};

#if IS_ENABLED(CONFIG_ZMK_USB)
static int usb_listener(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *usb = as_zmk_usb_conn_state_changed(eh);
    static int last = -1;

    if (usb && usb->conn_state != last) {
        struct event ev = {.type = EV_USB, .code = usb->conn_state};

        last = usb->conn_state;
        record(&ev, NULL);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rethink_ble_log, usb_listener);
ZMK_SUBSCRIPTION(rethink_ble_log, zmk_usb_conn_state_changed);
#endif

struct conn_args {
    const struct shell *sh;
    // NULL: only show
    const struct bt_le_conn_param *param;
    int count;
};

static void conn_cb(struct bt_conn *conn, void *data) {
    struct conn_args *args = data;
    struct bt_conn_info info;
    char addr[BT_ADDR_LE_STR_LEN];

    if (bt_conn_get_info(conn, &info) || info.state != BT_CONN_STATE_CONNECTED) {
        return;
    }
    args->count++;

    uint32_t us = info.le.interval_us;

    bt_addr_le_to_str(info.le.dst, addr, sizeof(addr));
    shell_print(args->sh, "%s: interval %u.%02u ms (%u/s), peripheral latency %u, timeout %u ms",
                addr, us / 1000, us % 1000 / 10, 1000000 / us, info.le.latency,
                info.le.timeout * 10);

    if (args->param) {
        int err = bt_conn_le_param_update(conn, args->param);

        if (err) {
            shell_error(args->sh, "  Request failed (%d)", err);
        } else {
            shell_print(args->sh, "  Asked for %u-%u (x 1.25 ms), latency %u: \"ble conn\" shows "
                                  "what the host chose",
                        args->param->interval_min, args->param->interval_max,
                        args->param->latency);
        }
    }
}

static int cmd_conn(const struct shell *sh, size_t argc, char **argv) {
    struct bt_le_conn_param param = {
        .latency = CONFIG_BT_PERIPHERAL_PREF_LATENCY,
        .timeout = CONFIG_BT_PERIPHERAL_PREF_TIMEOUT,
    };
    struct conn_args args = {.sh = sh};

    if (argc > 1) {
        param.interval_min = strtoul(argv[1], NULL, 0);
        param.interval_max = argc > 2 ? strtoul(argv[2], NULL, 0) : param.interval_min;
        if (argc > 3) {
            param.latency = strtoul(argv[3], NULL, 0);
        }
        if (param.interval_min < 6 || param.interval_min > param.interval_max ||
            param.interval_max > 3200) {
            shell_error(sh, "Intervals in 1.25 ms units, 6-3200 (7.5 ms-4 s), min <= max");
            return -EINVAL;
        }
        args.param = &param;
    }

    bt_conn_foreach(BT_CONN_TYPE_LE, conn_cb, &args);
    if (!args.count) {
        shell_print(sh, "Not connected over Bluetooth");
    }
    return 0;
}

static int cmd_log(const struct shell *sh, size_t argc, char **argv) {
    char buf[112];
    uint32_t total;

    if (argc > 1) {
        if (strcmp(argv[1], "clear")) {
            shell_error(sh, "Expected clear, not %s", argv[1]);
            return -EINVAL;
        }
        K_SPINLOCK(&lock) {
            recorded = 0;
            logged = 0;
        }
        return 0;
    }

    uint32_t up = k_uptime_get_32();

    shell_print(sh, "Up %u.%03u s", up / 1000, up % 1000);
#if IS_ENABLED(CONFIG_RETHINK_RESET_DIAG)
    rethink_reset_diag_print(sh);
#endif

    K_SPINLOCK(&lock) { total = recorded; }
    if (!total) {
        shell_print(sh, "No events");
        return 0;
    }

    uint32_t first = total > EVENTS ? total - EVENTS : 0;

    if (first) {
        shell_print(sh, "(%u earlier events not kept)", first);
    }
    for (uint32_t i = first; i < total; i++) {
        struct event ev;

        K_SPINLOCK(&lock) { ev = events[i % EVENTS]; }
        describe(&ev, buf, sizeof(buf));
        shell_print(sh, "%6u.%03u s  %s", ev.ms / 1000, ev.ms % 1000, buf);
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    ble_cmds,
    SHELL_CMD_ARG(conn, NULL,
                  "[min [max [latency]]]: connection interval; with numbers (1.25 ms units, "
                  "e.g. 6 = 7.5 ms), ask the host for another",
                  cmd_conn, 1, 3),
    SHELL_CMD_ARG(log, NULL,
                  "[clear]: connections, disconnections (reason), encryption, parameters and "
                  "USB power since boot; the last reset's cause and record",
                  cmd_log, 1, 1),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ble, &ble_cmds, "Bluetooth connection", NULL);
