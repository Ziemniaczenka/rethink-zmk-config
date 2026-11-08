/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// &tp: pointing stick on, off, toggle and recalibrate from a key

#define DT_DRV_COMPAT rethink_behavior_trackpoint

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <rethink/trackpoint.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int tp_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    int err = rethink_tp_request(binding->param1);

    return err ? err : ZMK_BEHAVIOR_OPAQUE;
}

static int tp_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api tp_api = {
    .binding_pressed = tp_pressed,
    .binding_released = tp_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tp_api);

#endif
