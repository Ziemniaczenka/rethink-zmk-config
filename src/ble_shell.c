/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// "ble" shell commands: the connection interval the host chose (how often
// the keyboard may send: a report waits at most one interval), and asking
// the host for another. The host decides; Zephyr asks for
// CONFIG_BT_PERIPHERAL_PREF_* 5 s after connecting.

#include <stdlib.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

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

SHELL_STATIC_SUBCMD_SET_CREATE(
    ble_cmds,
    SHELL_CMD_ARG(conn, NULL,
                  "[min [max [latency]]]: connection interval; with numbers (1.25 ms units, "
                  "e.g. 6 = 7.5 ms), ask the host for another",
                  cmd_conn, 1, 3),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ble, &ble_cmds, "Bluetooth connection", NULL);
