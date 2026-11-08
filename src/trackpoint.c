/*
 * Copyright (c) 2026 Ziemniaczenka
 * SPDX-License-Identifier: MIT
 */

// Pointing stick on the adapter: power, reset, the PS/2 link and pointing.
//
// TP1 (Mezzanine/FFC1) and TP2 (FFC2) both reach the Control Board as FW_CLK
// (P1.16) and FW_DAT (P1.15): open-drain lines with 4.7k pull-ups to TP_3V3,
// through the PCA9306 level shifter for a 5 V stick. The stick drives the
// clock for every bit in both directions, and an interrupt on each falling
// CLK edge reads DATA (stick to host) or sets the next bit (host to stick).
// A byte to the stick starts with the host holding CLK low for over 100 µs,
// then pulling DATA low and releasing CLK.
//
// While pointing, the interrupt also assembles the 3-byte movement packets:
// a packet's bytes follow each other within about a millisecond and packets
// come 10 ms apart, so a pause or a lost byte starts the next packet afresh.
// A thread reports them as relative movement and buttons (input events).

#define DT_DRV_COMPAT rethink_trackpoint

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rethink/adapter.h>
#include <rethink/expander.h>
#include <rethink/trackpoint.h>

LOG_MODULE_REGISTER(rethink_trackpoint, CONFIG_RETHINK_TRACKPOINT_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "Exactly one rethink,trackpoint node");

#define TP2 DT_INST_PROP(0, tp2)
#define NEEDS_PUMP                                                                                 \
    (TP2 ? IS_ENABLED(CONFIG_RETHINK_TP2_SUPPLY_5V) : IS_ENABLED(CONFIG_RETHINK_TP1_SUPPLY_5V))
// TP2 with R23 instead of R22: running unless the reset is driven high
#define RESET_PULLED_DOWN (TP2 && IS_ENABLED(CONFIG_RETHINK_TP2_RESET_PULL_DOWN))

// Within a byte the clock period is 60-100 µs; a longer pause breaks it off
#define GAP_US 250
// Host to stick: CLK held low at least 100 µs to inhibit
#define INHIBIT_US 150
// The stick starts clocking within 15 ms of the request, then 11 clocks
#define TX_TIMEOUT K_MSEC(25)
// A byte from the stick takes at most 11 clocks of 100 µs
#define RX_FINISH_US 1200
// Bytes of one packet arrive about 1 ms apart, packets 10 ms apart
#define PACKET_GAP_US 4000
// The stick answers a byte within 20 ms (0xFA, then any reply bytes)
#define REPLY_TIMEOUT K_MSEC(25)
// Its self-test result follows the reset after about 250-500 ms
#define SELF_TEST_TIMEOUT K_MSEC(1500)

#define PS2_ACK 0xfa
#define PS2_RESEND 0xfe
#define PS2_RESET 0xff
#define PS2_ENABLE 0xf4
#define PS2_SELF_TEST_OK 0xaa
#define TP_READ_ID 0xe1

// First packet byte: buttons, a bit that is always 1, sign bits
#define PKT_BUTTONS 0x07
#define PKT_ALWAYS_1 BIT(3)
#define PKT_X_SIGN BIT(4)
#define PKT_Y_SIGN BIT(5)

static const struct gpio_dt_spec clk = GPIO_DT_SPEC_INST_GET(0, clk_gpios);
static const struct gpio_dt_spec dat = GPIO_DT_SPEC_INST_GET(0, data_gpios);
static const uint8_t reset_pin = DT_INST_GPIO_PIN(0, reset_gpios);
static const struct device *const supply = DEVICE_DT_GET(DT_INST_PHANDLE(0, power_supply));

enum link_state {
    LINK_OFF,
    LINK_RX,      // receiving, or ready to
    LINK_INHIBIT, // the host holds CLK low
    LINK_TX,
};

static volatile enum link_state state;
static struct gpio_callback clk_cb;

// Stick to host
static volatile uint8_t rx_pos;
static uint16_t rx_frame;
static uint32_t rx_last;
static uint32_t rx_start;
K_MSGQ_DEFINE(rx_queue, sizeof(uint8_t), 64, 1);

// Host to stick
static uint8_t tx_pos;
static uint8_t tx_byte;
static uint8_t tx_parity;
static bool tx_ack;
static K_SEM_DEFINE(tx_done, 0, 1);
static K_MUTEX_DEFINE(tx_lock);

// Pointing
static volatile bool pointing;
static uint8_t pkt[3];
static uint8_t pkt_len;
static uint32_t pkt_last;
K_MSGQ_DEFINE(packet_queue, sizeof(pkt), 16, 1);
static uint8_t buttons;
static K_MUTEX_DEFINE(start_lock);

static struct rethink_tp_stats stats;
static bool powered;

// From the interrupt, after each valid byte while pointing
static void packet_byte(uint8_t byte, uint32_t now) {
    if (pkt_len > 0 && k_cyc_to_us_floor32(now - pkt_last) > PACKET_GAP_US) {
        stats.resyncs++;
        pkt_len = 0;
    }
    pkt_last = now;

    if (pkt_len == 0 && !(byte & PKT_ALWAYS_1)) {
        stats.resyncs++;
        return;
    }

    pkt[pkt_len++] = byte;
    if (pkt_len < sizeof(pkt)) {
        return;
    }
    pkt_len = 0;

    if (k_msgq_put(&packet_queue, pkt, K_NO_WAIT)) {
        stats.overruns++;
    } else {
        stats.packets++;
    }
}

// The parity bit that makes the count of ones odd
static int odd_parity_bit(uint8_t byte) { return (__builtin_popcount(byte) & 1) == 0; }

static void rx_edge(int bit, uint32_t now) {
    if (rx_pos > 0 && k_cyc_to_us_floor32(now - rx_last) > GAP_US) {
        stats.gaps++;
        rx_pos = 0;
        // The packet lost a byte
        pkt_len = 0;
    }
    rx_last = now;

    if (rx_pos == 0) {
        rx_frame = 0;
        rx_start = now;
    }
    rx_frame |= (uint16_t)bit << rx_pos;

    if (++rx_pos < 11) {
        return;
    }
    rx_pos = 0;

    // Start bit 0, 8 data bits LSB first, odd parity, stop bit 1
    uint8_t byte = (rx_frame >> 1) & 0xff;

    if ((rx_frame & BIT(0)) || !(rx_frame & BIT(10))) {
        stats.framing++;
        pkt_len = 0;
        return;
    }
    if (((rx_frame >> 9) & 1) != odd_parity_bit(byte)) {
        stats.parity++;
        pkt_len = 0;
        return;
    }

    // Ten periods from the start bit's edge to the stop bit's
    uint16_t period = k_cyc_to_us_floor32(now - rx_start) / 10;

    stats.period_us = period;
    stats.period_min_us = stats.period_min_us ? MIN(stats.period_min_us, period) : period;
    stats.period_max_us = MAX(stats.period_max_us, period);
    stats.bytes++;

    if (pointing) {
        packet_byte(byte, now);
    } else if (k_msgq_put(&rx_queue, &byte, K_NO_WAIT)) {
        stats.overruns++;
    }
}

// The stick reads DATA on the rising edge: set it while CLK is low
static void tx_edge(int bit) {
    tx_pos++;
    if (tx_pos <= 8) {
        gpio_pin_set_raw(dat.port, dat.pin, (tx_byte >> (tx_pos - 1)) & 1);
    } else if (tx_pos == 9) {
        gpio_pin_set_raw(dat.port, dat.pin, tx_parity);
    } else if (tx_pos == 10) {
        // Stop bit: released
        gpio_pin_set_raw(dat.port, dat.pin, 1);
    } else {
        // The stick pulls DATA low to acknowledge
        tx_ack = bit == 0;
        rx_pos = 0;
        state = LINK_RX;
        k_sem_give(&tx_done);
    }
}

static void clk_falling(const struct device *port, struct gpio_callback *cb,
                        gpio_port_pins_t pins) {
    // DATA first: it is valid only while CLK is low
    int bit = gpio_pin_get_raw(dat.port, dat.pin);
    uint32_t now = k_cycle_get_32();

    switch (state) {
    case LINK_RX:
        rx_edge(bit, now);
        break;
    case LINK_TX:
        tx_edge(bit);
        break;
    default:
        // Our own inhibit edge, or the link is off
        break;
    }
}

// Both lines released (pulled up), falling CLK edges interrupting
static int link_start(void) {
    int err = gpio_pin_configure_dt(&clk, GPIO_INPUT | GPIO_OUTPUT_HIGH);
    if (!err) {
        err = gpio_pin_configure_dt(&dat, GPIO_INPUT | GPIO_OUTPUT_HIGH);
    }
    if (err) {
        return err;
    }

    rx_pos = 0;
    k_msgq_purge(&rx_queue);
    state = LINK_RX;

    return gpio_pin_interrupt_configure_dt(&clk, GPIO_INT_EDGE_FALLING);
}

// R6/R7 go to TP_3V3: nothing may drive or pull the lines while it is off
static void link_stop(void) {
    state = LINK_OFF;
    gpio_pin_interrupt_configure_dt(&clk, GPIO_INT_DISABLE);
    gpio_pin_configure_dt(&clk, GPIO_DISCONNECTED);
    gpio_pin_configure_dt(&dat, GPIO_DISCONNECTED);
}

static int reset_set(bool asserted) {
    enum rethink_exp_mode mode;

    if (RESET_PULLED_DOWN) {
        mode = asserted ? RETHINK_EXP_OUT_HIGH : RETHINK_EXP_OUT_LOW;
    } else {
        // Released, R14/R22 pull it up to the stick's supply
        mode = asserted ? RETHINK_EXP_IN : RETHINK_EXP_OUT_LOW;
    }

    int err = rethink_exp_configure(reset_pin, mode);
    if (err) {
        LOG_ERR("Failed to %s the reset (%d)", asserted ? "assert" : "release", err);
    }

    return err;
}

int rethink_tp_power_on(void) {
    if (powered) {
        return 0;
    }
    if (!rethink_adapter_ready()) {
        return -ENODEV;
    }

    int err = reset_set(true);
    if (err) {
        return err;
    }

    err = regulator_enable(supply);
    if (err) {
        LOG_ERR("Failed to enable TP_3V3 (%d)", err);
        rethink_exp_configure(reset_pin, RETHINK_EXP_IN_PULL_DOWN);
        return err;
    }
    powered = true;

    if (NEEDS_PUMP) {
        // TP_3V3 first: U2's EN may not exceed its supply
        k_msleep(1);
        err = rethink_adapter_pump_set(true);
        if (err && err != -ENOTSUP) {
            rethink_tp_power_off();
            return err;
        }
    }

    memset(&stats, 0, sizeof(stats));
    k_msleep(DT_INST_PROP(0, reset_ms));

    // Listening before the reset ends: the self-test result follows it
    err = link_start();
    if (!err) {
        err = reset_set(false);
    }
    if (err) {
        rethink_tp_power_off();
        return err;
    }

    LOG_INF("Pointing stick on (%s, %s)", TP2 ? "TP2" : "TP1", NEEDS_PUMP ? "5 V" : "3.3 V");
    return 0;
}

void rethink_tp_power_off(void) {
    if (!powered) {
        return;
    }

    rethink_tp_set_raw();
    link_stop();
    if (NEEDS_PUMP) {
        rethink_adapter_pump_set(false);
    }
    regulator_disable(supply);
    // Back to idle-gpios: R14/R22's supply is gone
    rethink_exp_configure(reset_pin, RETHINK_EXP_IN_PULL_DOWN);
    powered = false;

    LOG_INF("Pointing stick off");
}

bool rethink_tp_powered(void) { return powered; }

bool rethink_tp_pointing(void) { return pointing; }

static const struct device *tp_dev(void) { return DEVICE_DT_INST_GET(0); }

// Buttons still held go up, so ZMK doesn't keep them pressed
static void release_buttons(void) {
    for (int i = 0; i < 3; i++) {
        if (buttons & BIT(i)) {
            buttons &= ~BIT(i);
            input_report_key(tp_dev(), INPUT_BTN_0 + i, 0, buttons == 0, K_FOREVER);
        }
    }
}

void rethink_tp_set_raw(void) {
    pointing = false;
    k_msgq_purge(&packet_queue);
    k_msgq_purge(&rx_queue);
    release_buttons();
}

// PS/2 bit 0 left, 1 right, 2 middle: ZMK's mouse buttons 1, 2, 3
static void report(const uint8_t p[3]) {
    int dx = p[1] - ((p[0] & PKT_X_SIGN) ? 256 : 0);
    // PS/2 counts up as positive, HID down
    int dy = -(p[2] - ((p[0] & PKT_Y_SIGN) ? 256 : 0));
    uint8_t changed = (p[0] & PKT_BUTTONS) ^ buttons;

    if (dx) {
        input_report_rel(tp_dev(), INPUT_REL_X, dx, !dy && !changed, K_FOREVER);
    }
    if (dy) {
        input_report_rel(tp_dev(), INPUT_REL_Y, dy, !changed, K_FOREVER);
    }
    for (int i = 0; i < 3; i++) {
        if (changed & BIT(i)) {
            changed &= ~BIT(i);
            buttons ^= BIT(i);
            input_report_key(tp_dev(), INPUT_BTN_0 + i, buttons & BIT(i), !changed, K_FOREVER);
        }
    }
}

// Reply bytes after an acknowledge
static int read_reply(uint8_t *buf, size_t len, k_timeout_t first) {
    for (size_t i = 0; i < len; i++) {
        if (rethink_tp_read(&buf[i], i ? REPLY_TIMEOUT : first) != 0) {
            return -ETIMEDOUT;
        }
    }
    return 0;
}

// A command byte and its acknowledge; 0xFE asks for the byte again
static int command(uint8_t cmd) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t reply;
        int err = rethink_tp_write(cmd);

        if (err) {
            continue;
        }
        if (rethink_tp_read(&reply, REPLY_TIMEOUT) != 0) {
            return -ETIMEDOUT;
        }
        if (reply == PS2_ACK) {
            return 0;
        }
        if (reply != PS2_RESEND) {
            LOG_WRN("0x%02x answered with 0x%02x", cmd, reply);
            return -EIO;
        }
    }

    return -EIO;
}

static int start_locked(void) {
    uint8_t reply[2];
    int err;

    rethink_tp_set_raw();

    // A powered stick gets the reset command instead of a power cycle
    err = powered ? command(PS2_RESET) : rethink_tp_power_on();
    if (err == -ENODEV) {
        return err;
    }
    if (!err) {
        err = read_reply(reply, sizeof(reply), SELF_TEST_TIMEOUT);
    }
    if (!err && (reply[0] != PS2_SELF_TEST_OK || reply[1] != 0x00)) {
        LOG_ERR("Self-test failed: %02x %02x", reply[0], reply[1]);
        err = -EIO;
    }
    if (err) {
        LOG_WRN("No pointing stick answered (%d), powering it off", err);
        rethink_tp_power_off();
        return err;
    }

    // TrackPoints answer with their maker and firmware; other PS/2 mice don't
    if (command(TP_READ_ID) == 0 && read_reply(reply, sizeof(reply), REPLY_TIMEOUT) == 0) {
        LOG_INF("TrackPoint ID %02x, firmware %02x", reply[0], reply[1]);
    } else {
        LOG_INF("No TrackPoint ID: used as a plain PS/2 mouse");
        rethink_tp_flush();
    }

    err = command(PS2_ENABLE);
    if (err) {
        LOG_ERR("Failed to turn streaming on (%d), powering the stick off", err);
        rethink_tp_power_off();
        return err;
    }

    pkt_len = 0;
    pointing = true;
    LOG_INF("Pointing");
    return 0;
}

int rethink_tp_start(void) {
    k_mutex_lock(&start_lock, K_FOREVER);
    int err = start_locked();
    k_mutex_unlock(&start_lock);

    return err;
}

// Starts the stick after boot (the adapter is up by then), then reports packets
static void tp_thread(void *p1, void *p2, void *p3) {
    uint8_t p[3];

    rethink_tp_start();

    for (;;) {
        k_msgq_get(&packet_queue, p, K_FOREVER);
        if (pointing) {
            report(p);
        }
    }
}

K_THREAD_DEFINE(rethink_tp_thread, CONFIG_RETHINK_TRACKPOINT_THREAD_STACK_SIZE, tp_thread, NULL,
                NULL, NULL, CONFIG_RETHINK_TRACKPOINT_THREAD_PRIORITY, 0, 0);

int rethink_tp_write(uint8_t byte) {
    if (state == LINK_OFF) {
        return -ENODEV;
    }

    k_mutex_lock(&tx_lock, K_FOREVER);

    // Let a byte the stick is sending finish
    for (int waited = 0; rx_pos != 0 && waited < RX_FINISH_US; waited += 50) {
        k_busy_wait(50);
    }

    // Inhibit; the ISR ignores this falling edge
    state = LINK_INHIBIT;
    gpio_pin_set_raw(clk.port, clk.pin, 0);
    k_busy_wait(INHIBIT_US);

    tx_byte = byte;
    tx_parity = odd_parity_bit(byte);
    tx_pos = 0;
    tx_ack = false;
    k_sem_reset(&tx_done);

    // Request to send: DATA low (the start bit), then CLK released
    gpio_pin_set_raw(dat.port, dat.pin, 0);
    state = LINK_TX;
    gpio_pin_set_raw(clk.port, clk.pin, 1);

    int err = k_sem_take(&tx_done, TX_TIMEOUT);
    if (err) {
        state = LINK_INHIBIT;
        gpio_pin_set_raw(dat.port, dat.pin, 1);
        rx_pos = 0;
        state = LINK_RX;
        stats.tx_timeouts++;
        err = -ETIMEDOUT;
    } else if (!tx_ack) {
        stats.tx_nacks++;
        err = -EIO;
    } else {
        stats.tx_bytes++;
    }

    k_mutex_unlock(&tx_lock);
    return err;
}

int rethink_tp_read(uint8_t *byte, k_timeout_t timeout) {
    return k_msgq_get(&rx_queue, byte, timeout);
}

void rethink_tp_flush(void) { k_msgq_purge(&rx_queue); }

void rethink_tp_stats_get(struct rethink_tp_stats *out) {
    unsigned int key = irq_lock();

    *out = stats;
    irq_unlock(key);
}

void rethink_tp_stats_reset(void) {
    unsigned int key = irq_lock();

    memset(&stats, 0, sizeof(stats));
    irq_unlock(key);
}

static int tp_init(const struct device *dev) {
    if (!gpio_is_ready_dt(&clk) || !gpio_is_ready_dt(&dat)) {
        return -ENODEV;
    }

    gpio_init_callback(&clk_cb, clk_falling, BIT(clk.pin));
    int err = gpio_add_callback(clk.port, &clk_cb);
    if (err) {
        LOG_ERR("Failed to add the CLK callback (%d)", err);
        return err;
    }

    link_stop();
    return 0;
}

// After the adapter, which owns the expander
DEVICE_DT_INST_DEFINE(0, tp_init, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_RETHINK_TRACKPOINT_INIT_PRIORITY, NULL);
