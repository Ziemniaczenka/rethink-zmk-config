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
// come 5-100 ms apart (200-10/s), so a pause starts the next packet afresh.
// A thread reports them as relative movement and buttons (input events). A
// packet with a broken byte is dropped and the thread asks the stick for it
// again (0xFE). The thread also carries out the requests from keys (&tp),
// can drop the report rate to 80/s while the stick is still (fewer force
// measurements) and powers the stick off after a while without use.
//
// With the middle button held, movement scrolls instead (vertical, or
// sideways when that is clearly stronger; in sixteenths of a step with smooth
// scrolling); the middle button clicks only if let go without movement.
//
// The stick calibrates itself: its zero point is set at the self-test, and
// again whenever the force stays constant for 0.53 s (hands off: drift
// correction). E2 51 sets it to the present forces on request.
//
// Settings (report rates on battery and with USB power, the stick's
// sensitivity and the like, scrolling) have Kconfig defaults and are kept in
// flash once changed. The stick's own live in its RAM (E2 80/81 read and
// write a location): they survive a reset command but not a power cycle, so
// each start writes them, after a power-on only those that differ from the
// stick's power-on values (as Linux does).

#define DT_DRV_COMPAT rethink_trackpoint

#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif

#include <dt-bindings/rethink/trackpoint.h>
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
// Bytes of one packet arrive about 1 ms apart. A pause of half the packet
// period, at most this, starts the next packet.
#define PACKET_GAP_US 4000
// After an error, the rest of the packet: the clock quiet this long
#define PACKET_END_US 1000
#define RESEND_ATTEMPTS 3
// The stick answers a byte within 20 ms (0xFA, then any reply bytes)
#define REPLY_MS 25
#define REPLY_TIMEOUT K_MSEC(REPLY_MS)
// Its self-test result follows the reset after about 250-500 ms
#define SELF_TEST_TIMEOUT K_MSEC(1500)
// E2 51 is acknowledged once the recalibration is done, after about 310 ms
#define RECALIBRATE_TIMEOUT K_MSEC(1000)
#define RECALIBRATE_MS 350

#define IDLE_OFF_MS (CONFIG_RETHINK_TRACKPOINT_IDLE_OFF_MIN * 60 * 1000)

#define PS2_ACK 0xfa
#define PS2_RESEND 0xfe
#define PS2_RESET 0xff
#define PS2_ENABLE 0xf4
#define PS2_DISABLE 0xf5
#define PS2_SET_RATE 0xf3
#define PS2_SELF_TEST_OK 0xaa
#define TP_READ_ID 0xe1
#define TP_COMMAND 0xe2
#define TP_RECALIBRATE 0x51
// After TP_COMMAND: read a RAM location, write one, flip bits of a flag byte
#define TP_READ_RAM 0x80
#define TP_WRITE_RAM 0x81
#define TP_TOGGLE 0x47
// The first ID byte: IBM sticks have all the RAM settings below, others
// (ALPS, Elan, NXP) only sensitivity and press-to-select
#define TP_VARIANT_IBM 0x01
// Press-to-select: a bit of flag byte 2C
#define TP_PTS_FLAGS 0x2c
#define TP_PTS_BIT 0x01

// Settings are saved this long after the last change
#define SAVE_DELAY K_SECONDS(3)

// Report rates (F3 XX)
static const uint8_t rates[] = {10, 20, 40, 60, 80, 100, 200};

#define VALID_RATE(r)                                                                              \
    ((r) == 10 || (r) == 20 || (r) == 40 || (r) == 60 || (r) == 80 || (r) == 100 || (r) == 200)
#define RATE CONFIG_RETHINK_TRACKPOINT_SAMPLE_RATE
#define RATE_USB CONFIG_RETHINK_TRACKPOINT_SAMPLE_RATE_USB
BUILD_ASSERT(VALID_RATE(RATE) && VALID_RATE(RATE_USB),
             "PS/2 sample rates: 10, 20, 40, 60, 80, 100, 200");

// Force measurements a second at a report rate (TrackPoint specification
// 2.4.6): never under 80
static int conversions(int rate) { return rate == 60 ? 120 : rate >= 100 ? 100 : 80; }

// Still for this long: IDLE_RATE, the fewest measurements
#define SLOW_IDLE_MS 2000
#define IDLE_RATE 80

// Requests to the thread: TP_OFF, TP_ON, TP_RECAL, a key press after an idle
// power-off, a packet with an error, a new report rate or stick setting
#define REQ_WAKE (TP_PTS_TOG + 1)
#define REQ_RESEND (TP_PTS_TOG + 2)
#define REQ_CONFIG (TP_PTS_TOG + 3)

// First packet byte: buttons, a bit that is always 1, sign bits
#define PKT_BUTTONS 0x07
#define PKT_MIDDLE BIT(2)
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
// Replies to a resend request or a rate change go to rx_queue
static volatile bool commanding;
static uint8_t pkt[3];
static uint8_t pkt_len;
static uint32_t pkt_last;
// The packet had an error: its other bytes are dropped, it is asked for again
static volatile bool pkt_broken;
K_MSGQ_DEFINE(packet_queue, sizeof(pkt), 16, 1);
// A pause this long starts the next packet
static volatile uint32_t packet_gap_us = PACKET_GAP_US;
static uint8_t buttons;
// The stick's report rate while pointing
static uint8_t rate_now;
// Uptime (ms) of the last packet with movement
static uint32_t last_move;

#define SCROLL IS_ENABLED(CONFIG_RETHINK_TRACKPOINT_SCROLL)

static const struct rethink_tp_setting_info infos[] = {
    [RETHINK_TP_RATE] = {"rate", RATE, 10, 200,
                         "reports per second on battery: 10, 20, 40, 60, 80, 100, 200"},
    [RETHINK_TP_RATE_USB] = {"rate_usb", RATE_USB, 10, 200, "reports per second with USB power"},
    [RETHINK_TP_SENSITIVITY] = {"sensitivity", CONFIG_RETHINK_TRACKPOINT_SENSITIVITY, 0, 255,
                                "force scale, 128 = x1.0"},
    [RETHINK_TP_INERTIA] = {"inertia", CONFIG_RETHINK_TRACKPOINT_NEG_INERTIA, 0, 255,
                            "negative inertia: sharper starts and stops; 0 off"},
    [RETHINK_TP_SPEED] = {"speed", CONFIG_RETHINK_TRACKPOINT_SPEED, 0, 255,
                          "upper plateau: about 22 + 4.3 x this counts/s"},
    [RETHINK_TP_DRIFT_TIME] = {"drift_time", CONFIG_RETHINK_TRACKPOINT_DRIFT_TIME, 1, 255,
                               "hands off this long (107 ms steps): new zero point"},
    [RETHINK_TP_PRESS_TO_SELECT] = {"press_to_select",
                                    IS_ENABLED(CONFIG_RETHINK_TRACKPOINT_PRESS_TO_SELECT), 0, 1,
                                    "pressing the stick clicks"},
    [RETHINK_TP_RESET_MS] = {"reset_ms", DT_INST_PROP(0, reset_ms), 1, 1000,
                             "reset held after power-on, ms (specification: 600)"},
#if SCROLL
    [RETHINK_TP_SCROLL_DIVISOR] = {"scroll_divisor", CONFIG_RETHINK_TRACKPOINT_SCROLL_DIVISOR, 1,
                                   255, "stick counts per wheel step"},
    [RETHINK_TP_SCROLL_DEAD_ZONE] = {"scroll_dead_zone",
                                     CONFIG_RETHINK_TRACKPOINT_SCROLL_H_DEAD_ZONE, 0, 127,
                                     "sideways counts per report that don't scroll"},
#endif
};
BUILD_ASSERT(ARRAY_SIZE(infos) == RETHINK_TP_SETTINGS);

static atomic_t values[] = {
    [RETHINK_TP_RATE] = ATOMIC_INIT(RATE),
    [RETHINK_TP_RATE_USB] = ATOMIC_INIT(RATE_USB),
    [RETHINK_TP_SENSITIVITY] = ATOMIC_INIT(CONFIG_RETHINK_TRACKPOINT_SENSITIVITY),
    [RETHINK_TP_INERTIA] = ATOMIC_INIT(CONFIG_RETHINK_TRACKPOINT_NEG_INERTIA),
    [RETHINK_TP_SPEED] = ATOMIC_INIT(CONFIG_RETHINK_TRACKPOINT_SPEED),
    [RETHINK_TP_DRIFT_TIME] = ATOMIC_INIT(CONFIG_RETHINK_TRACKPOINT_DRIFT_TIME),
    [RETHINK_TP_PRESS_TO_SELECT] =
        ATOMIC_INIT(IS_ENABLED(CONFIG_RETHINK_TRACKPOINT_PRESS_TO_SELECT)),
    [RETHINK_TP_RESET_MS] = ATOMIC_INIT(DT_INST_PROP(0, reset_ms)),
#if SCROLL
    [RETHINK_TP_SCROLL_DIVISOR] = ATOMIC_INIT(CONFIG_RETHINK_TRACKPOINT_SCROLL_DIVISOR),
    [RETHINK_TP_SCROLL_DEAD_ZONE] = ATOMIC_INIT(CONFIG_RETHINK_TRACKPOINT_SCROLL_H_DEAD_ZONE),
#endif
};
BUILD_ASSERT(ARRAY_SIZE(values) == RETHINK_TP_SETTINGS);

static int setting(enum rethink_tp_setting s) { return atomic_get(&values[s]); }

// The stick's settings in its RAM, with its power-on values
static const struct {
    uint8_t loc;
    uint8_t power_on;
    enum rethink_tp_setting setting;
} stick_ram[] = {
    {0x4a, 0x80, RETHINK_TP_SENSITIVITY},
    {0x4d, 0x06, RETHINK_TP_INERTIA},
    {0x60, 0x61, RETHINK_TP_SPEED},
    {0x5f, 0x05, RETHINK_TP_DRIFT_TIME},
};
// Stick settings changed since they were last written
static atomic_t stick_changed;
// The first ID byte; 0 for a plain PS/2 mouse
static uint8_t variant;
// Wheel units per step: sixteenths with smooth scrolling, which the input
// listener divides back into whole steps for a host that doesn't set the HID
// Resolution Multiplier
#define WHEEL_UNITS (IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING) ? 16 : 1)
// Counts while the middle button is held: more than this, and letting it go
// doesn't click
#define CLICK_SLOP 4
static struct {
    bool held;
    int moved;
    // Counts times WHEEL_UNITS not yet a wheel unit
    int wheel;
    int hwheel;
} scroll;
// Held while the stick is started, recalibrated or powered off
static K_MUTEX_DEFINE(start_lock);

static struct rethink_tp_stats stats;
static bool powered;
// Uptime (ms) when the last reset ended (pin released, or reset command
// acknowledged); the self-test result follows it
static int64_t reset_end;

static atomic_t requests;
static struct k_poll_signal request_signal = K_POLL_SIGNAL_INITIALIZER(request_signal);
// Uptime (ms) of the last packet or key press
static atomic_t last_use;
// Powered off for lack of use: the next key press starts the stick
static atomic_t idle_off;

static void post_request(int req) {
    atomic_or(&requests, BIT(req));
    k_poll_signal_raise(&request_signal, 0);
}

// From the interrupt, after each valid byte while pointing
static void packet_byte(uint8_t byte, uint32_t now) {
    if (k_cyc_to_us_floor32(now - pkt_last) > packet_gap_us) {
        // The next packet
        if (pkt_len > 0) {
            stats.resyncs++;
        }
        pkt_len = 0;
        pkt_broken = false;
    }
    pkt_last = now;

    if (pkt_broken) {
        return;
    }
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

// A byte with an error or broken off: while pointing, the thread asks for its
// packet again
static void rx_error(uint32_t when) {
    if (!pointing || commanding) {
        return;
    }

    pkt_len = 0;
    pkt_last = when;
    if (!pkt_broken) {
        pkt_broken = true;
        post_request(REQ_RESEND);
    }
}

// A byte still unfinished after RX_FINISH_US lost clock edges. Without this
// its loss would show only with the next packet, too late to ask again.
static void byte_timeout(struct k_timer *timer) {
    unsigned int key = irq_lock();

    if (rx_pos > 0) {
        stats.gaps++;
        rx_pos = 0;
        rx_error(rx_last);
    }
    irq_unlock(key);
}

static K_TIMER_DEFINE(byte_timer, byte_timeout, NULL);

static void rx_edge(int bit, uint32_t now) {
    if (rx_pos > 0 && k_cyc_to_us_floor32(now - rx_last) > GAP_US) {
        stats.gaps++;
        rx_pos = 0;
        rx_error(rx_last);
    }
    rx_last = now;

    if (rx_pos == 0) {
        rx_frame = 0;
        rx_start = now;
        k_timer_start(&byte_timer, K_USEC(RX_FINISH_US), K_NO_WAIT);
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
        rx_error(now);
        return;
    }
    if (((rx_frame >> 9) & 1) != odd_parity_bit(byte)) {
        stats.parity++;
        rx_error(now);
        return;
    }

    // Ten periods from the start bit's edge to the stop bit's
    uint16_t period = k_cyc_to_us_floor32(now - rx_start) / 10;

    stats.period_us = period;
    stats.period_min_us = stats.period_min_us ? MIN(stats.period_min_us, period) : period;
    stats.period_max_us = MAX(stats.period_max_us, period);
    stats.bytes++;

    if (pointing && !commanding) {
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
    k_timer_stop(&byte_timer);
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
    k_msleep(setting(RETHINK_TP_RESET_MS));

    // Listening before the reset ends: the self-test result follows it
    err = link_start();
    if (!err) {
        err = reset_set(false);
    }
    if (err) {
        rethink_tp_power_off();
        return err;
    }
    reset_end = k_uptime_get();

    LOG_INF("Pointing stick on (%s, %s)", TP2 ? "TP2" : "TP1", NEEDS_PUMP ? "5 V" : "3.3 V");
    return 0;
}

void rethink_tp_power_off(void) {
    atomic_clear(&idle_off);

    k_mutex_lock(&start_lock, K_FOREVER);
    if (powered) {
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
    k_mutex_unlock(&start_lock);
}

bool rethink_tp_powered(void) { return powered; }

bool rethink_tp_pointing(void) { return pointing; }

static const struct device *tp_dev(void) { return DEVICE_DT_INST_GET(0); }

// Buttons still held go up, so ZMK doesn't keep them pressed
static void release_buttons(void) {
    scroll.held = false;
    for (int i = 0; i < 3; i++) {
        if (buttons & BIT(i)) {
            buttons &= ~BIT(i);
            input_report_key(tp_dev(), INPUT_BTN_0 + i, 0, buttons == 0, K_FOREVER);
        }
    }
}

void rethink_tp_set_raw(void) {
    k_mutex_lock(&start_lock, K_FOREVER);
    pointing = false;
    k_msgq_purge(&packet_queue);
    k_msgq_purge(&rx_queue);
    release_buttons();
    k_mutex_unlock(&start_lock);
}

// PS/2 bit 0 left, 1 right, 2 middle: ZMK's mouse buttons 1, 2, 3
// The input events of one packet; the last one syncs
struct events {
    struct {
        uint8_t type;
        uint16_t code;
        int32_t value;
    } ev[5];
    int n;
};

static void add_event(struct events *e, uint8_t type, uint16_t code, int32_t value) {
    e->ev[e->n].type = type;
    e->ev[e->n].code = code;
    e->ev[e->n].value = value;
    e->n++;
}

static void send_events(struct events *e) {
    for (int i = 0; i < e->n; i++) {
        input_report(tp_dev(), e->ev[i].type, e->ev[i].code, e->ev[i].value, i == e->n - 1,
                     K_FOREVER);
    }
    e->n = 0;
}

#if SCROLL
// Counts to wheel units (WHEEL_UNITS per divisor counts), the remainder kept
// for the next report
static int steps(int *acc, int counts) {
    int divisor = setting(RETHINK_TP_SCROLL_DIVISOR);
    int n;

    *acc += counts * WHEEL_UNITS;
    n = *acc / divisor;
    *acc -= n * divisor;
    return n;
}

// The stronger axis of the report scrolls; sideways only past the dead zone,
// so a push up that leans a little doesn't scroll sideways. PS/2 and HID
// both count up and right as positive here.
static void scroll_by(struct events *e, int dx, int dy) {
    int dead = setting(RETHINK_TP_SCROLL_DEAD_ZONE);
    int n = 0;

    scroll.moved += abs(dx) + abs(dy);
    if (abs(dy) >= abs(dx)) {
        n = steps(&scroll.wheel, dy);
        if (n) {
            add_event(e, INPUT_EV_REL, INPUT_REL_WHEEL, n);
        }
    } else if (abs(dx) > dead) {
        n = steps(&scroll.hwheel, dx > 0 ? dx - dead : dx + dead);
        if (n) {
            add_event(e, INPUT_EV_REL, INPUT_REL_HWHEEL, n);
        }
    }
}
#endif

static void report(const uint8_t p[3]) {
    // PS/2 counts up as positive
    int dx = p[1] - ((p[0] & PKT_X_SIGN) ? 256 : 0);
    int dy = p[2] - ((p[0] & PKT_Y_SIGN) ? 256 : 0);
    uint8_t pressed = p[0] & PKT_BUTTONS;
    bool click = false;
    struct events e = {.n = 0};

    if (dx || dy) {
        last_move = k_uptime_get_32();
    }

#if SCROLL
    if (pressed & PKT_MIDDLE) {
        if (!scroll.held) {
            scroll.held = true;
            scroll.moved = scroll.wheel = scroll.hwheel = 0;
        }
        scroll_by(&e, dx, dy);
        dx = dy = 0;
    } else if (scroll.held) {
        scroll.held = false;
        click = scroll.moved <= CLICK_SLOP;
    }
    pressed &= ~PKT_MIDDLE;
#endif

    if (dx) {
        add_event(&e, INPUT_EV_REL, INPUT_REL_X, dx);
    }
    if (dy) {
        // HID counts down as positive
        add_event(&e, INPUT_EV_REL, INPUT_REL_Y, -dy);
    }
    for (int i = 0; i < 3; i++) {
        if ((pressed ^ buttons) & BIT(i)) {
            buttons ^= BIT(i);
            add_event(&e, INPUT_EV_KEY, INPUT_BTN_0 + i, (buttons & BIT(i)) != 0);
        }
    }
    send_events(&e);

    if (click) {
        input_report_key(tp_dev(), INPUT_BTN_2, 1, true, K_FOREVER);
        input_report_key(tp_dev(), INPUT_BTN_2, 0, true, K_FOREVER);
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

// A command byte and its acknowledge. 0xFE from the stick asks for the byte
// again; a reply that doesn't come in time (lost to a receive error) is asked
// for again with 0xFE.
static int command_wait(uint8_t cmd, k_timeout_t ack_timeout) {
    uint8_t out = cmd;
    k_timeout_t timeout = ack_timeout;

    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t reply;

        if (rethink_tp_write(out)) {
            continue;
        }
        if (rethink_tp_read(&reply, timeout) != 0) {
            out = PS2_RESEND;
            timeout = REPLY_TIMEOUT;
            continue;
        }
        if (reply == PS2_ACK) {
            return 0;
        }
        if (reply != PS2_RESEND) {
            LOG_WRN("0x%02x answered with 0x%02x", cmd, reply);
            return -EIO;
        }
        out = cmd;
    }

    return -ETIMEDOUT;
}

static int command(uint8_t cmd) { return command_wait(cmd, REPLY_TIMEOUT); }

static void note_use(void) { atomic_set(&last_use, k_uptime_get_32()); }

// Streaming on (F4) or off (F5), sent again until acknowledged: both may be
// repeated. Packet bytes may come before the acknowledge of F5 (a packet
// already on its way: dropped) and instead of a lost one of F4 (streaming
// is on).
static int stream_set(bool on) {
    for (int attempt = 0; attempt < 3; attempt++) {
        int64_t end = k_uptime_get() + REPLY_MS;
        uint8_t byte;

        if (rethink_tp_write(on ? PS2_ENABLE : PS2_DISABLE)) {
            continue;
        }
        while (rethink_tp_read(&byte, K_MSEC(MAX(end - k_uptime_get(), 0))) == 0) {
            if (byte == PS2_ACK || (on && byte != PS2_RESEND)) {
                return 0;
            }
            if (byte == PS2_RESEND) {
                break;
            }
        }
    }
    return -EIO;
}

// Command bytes, each acknowledged
static int commands(const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int err = command(bytes[i]);

        if (err) {
            return err;
        }
    }
    return 0;
}

static int ram_write(uint8_t loc, uint8_t value) {
    const uint8_t bytes[] = {TP_COMMAND, TP_WRITE_RAM, loc, value};

    return commands(bytes, sizeof(bytes));
}

static int ram_read(uint8_t loc, uint8_t *value) {
    const uint8_t bytes[] = {TP_COMMAND, TP_READ_RAM, loc};
    int err = commands(bytes, sizeof(bytes));

    return err ? err : read_reply(value, 1, REPLY_TIMEOUT);
}

// Press-to-select is a flag bit, flipped by E2 47 2C 01: read first
static int pts_write(bool on) {
    const uint8_t toggle[] = {TP_COMMAND, TP_TOGGLE, TP_PTS_FLAGS, TP_PTS_BIT};
    uint8_t flags;
    int err = ram_read(TP_PTS_FLAGS, &flags);

    if (err || !(flags & TP_PTS_BIT) == !on) {
        return err;
    }
    return commands(toggle, sizeof(toggle));
}

// The stick's settings to its RAM, with streaming off. Right after a power-on
// only those that differ from its power-on values, which differ from stick to
// stick for some makers.
static int stick_settings_write(bool power_on) {
    atomic_clear(&stick_changed);
    if (variant == 0) {
        return 0;
    }

    for (size_t i = 0; i < ARRAY_SIZE(stick_ram); i++) {
        int value = setting(stick_ram[i].setting);

        if ((power_on && value == stick_ram[i].power_on) ||
            (variant != TP_VARIANT_IBM && stick_ram[i].setting != RETHINK_TP_SENSITIVITY)) {
            continue;
        }
        int err = ram_write(stick_ram[i].loc, value);

        if (err) {
            return err;
        }
    }

    bool pts = setting(RETHINK_TP_PRESS_TO_SELECT);

    return power_on && !pts ? 0 : pts_write(pts);
}

static void note_rate(uint8_t rate) {
    rate_now = rate;
    packet_gap_us = MIN(PACKET_GAP_US, 500000 / rate);
}

// The rate setting in effect: with USB power the stick's current costs no
// battery
static enum rethink_tp_setting rate_setting(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        return RETHINK_TP_RATE_USB;
    }
#endif
    return RETHINK_TP_RATE;
}

// A rate that measures the force more than 80 times a second drops to 80/s
// while the stick is still
static bool slow_idle(void) {
    return IS_ENABLED(CONFIG_RETHINK_TRACKPOINT_SLOW_IDLE) &&
           conversions(setting(rate_setting())) > conversions(IDLE_RATE);
}

static uint32_t still_ms(void) { return k_uptime_get_32() - last_move; }

// The chosen rate, or IDLE_RATE once the stick is still
static uint8_t rate_wanted(void) {
    return slow_idle() && still_ms() >= SLOW_IDLE_MS ? IDLE_RATE : setting(rate_setting());
}

static int start_locked(void) {
    int64_t begin = k_uptime_get();
    uint8_t reply[2];
    int err = 0;

    rethink_tp_set_raw();

    // A powered stick gets the reset command; one that doesn't answer it (E2
    // 44 powered it down, say) a power cycle, which resets it with its pin
    if (powered) {
        if (command(PS2_RESET) == 0) {
            reset_end = k_uptime_get();
        } else {
            LOG_INF("No answer to the reset command: power cycle");
            rethink_tp_power_off();
        }
    }
    // Its RAM settings at their power-on values; a reset command keeps them
    bool power_on = !powered;

    if (!powered) {
        err = rethink_tp_power_on();
    }
    if (err == -ENODEV) {
        return err;
    }
    if (!err) {
        err = read_reply(reply, sizeof(reply), SELF_TEST_TIMEOUT);
    }
    int32_t self_test_ms = k_uptime_get() - reset_end;

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
        variant = reply[0];
    } else {
        LOG_INF("No TrackPoint ID: used as a plain PS/2 mouse");
        rethink_tp_flush();
        variant = 0;
    }

    err = stick_settings_write(power_on);
    if (err) {
        LOG_WRN("Failed to write the stick's settings (%d)", err);
        rethink_tp_flush();
    }

    // Still until its first movement packet. The self-test set 100/s.
    last_move = k_uptime_get_32() - SLOW_IDLE_MS;
    uint8_t rate = rate_wanted();

    if (rate != 100 && (command(PS2_SET_RATE) || command(rate))) {
        LOG_WRN("Failed to set %d reports/s", rate);
        rate = 100;
    }
    note_rate(rate);

    err = stream_set(true);
    if (err) {
        LOG_ERR("Failed to turn streaming on (%d), powering the stick off", err);
        rethink_tp_power_off();
        return err;
    }

    pkt_len = 0;
    pointing = true;
    note_use();

    stats.start_ms = k_uptime_get() - begin;
    stats.reset_ms = power_on ? setting(RETHINK_TP_RESET_MS) : 0;
    stats.self_test_ms = self_test_ms;
    LOG_INF("Pointing, %u ms after %s (self-test %u ms after the reset)", stats.start_ms,
            power_on ? "power-on" : "the reset command", stats.self_test_ms);
    return 0;
}

int rethink_tp_start(void) {
    atomic_clear(&idle_off);

    k_mutex_lock(&start_lock, K_FOREVER);
    int err = start_locked();
    k_mutex_unlock(&start_lock);

    return err;
}

void rethink_tp_resume(void) {
    k_mutex_lock(&start_lock, K_FOREVER);
    if (powered && !pointing) {
        rethink_tp_flush();
        pkt_len = 0;
        pkt_broken = false;
        pointing = true;
        note_use();
    }
    k_mutex_unlock(&start_lock);
}

// Streaming off, so replies don't mix with packets; packet bytes already on
// their way are dropped with the acknowledge
static int pause_streaming(void) {
    rethink_tp_set_raw();
    int err = rethink_tp_write(PS2_DISABLE);

    k_sleep(REPLY_TIMEOUT);
    rethink_tp_flush();
    return err;
}

int rethink_tp_recalibrate(void) {
    int err = -ENODEV;

    k_mutex_lock(&start_lock, K_FOREVER);
    if (pointing) {
        err = pause_streaming();
        if (!err) {
            err = command(TP_COMMAND);
        }
        int64_t sent = k_uptime_get();
        int32_t ack_ms = 0;

        if (!err) {
            err = command_wait(TP_RECALIBRATE, RECALIBRATE_TIMEOUT);
            ack_ms = k_uptime_get() - sent;
            // Nothing may be sent during the cycle, should the stick acknowledge early
            k_sleep(K_MSEC(MAX(RECALIBRATE_MS - ack_ms, 0)));
        }
        if (err) {
            LOG_WRN("Recalibration failed (%d)", err);
        } else {
            LOG_INF("Recalibrated (acknowledged after %d ms): the present forces are the zero "
                    "point",
                    ack_ms);
        }

        if (stream_set(true) == 0) {
            pkt_len = 0;
            pointing = true;
        } else {
            LOG_WRN("Streaming didn't resume, restarting the stick");
            start_locked();
        }
    } else {
        LOG_INF("Not recalibrated: the stick is off");
    }
    k_mutex_unlock(&start_lock);

    return err;
}

int rethink_tp_rate_now(void) { return pointing ? rate_now : 0; }

const struct rethink_tp_setting_info *rethink_tp_setting_info(enum rethink_tp_setting s) {
    return s < RETHINK_TP_SETTINGS ? &infos[s] : NULL;
}

int rethink_tp_setting_get(enum rethink_tp_setting s) {
    return s < RETHINK_TP_SETTINGS ? setting(s) : 0;
}

static bool setting_valid(enum rethink_tp_setting s, int value) {
    if (s >= RETHINK_TP_SETTINGS || value < infos[s].min || value > infos[s].max) {
        return false;
    }
    if (s != RETHINK_TP_RATE && s != RETHINK_TP_RATE_USB) {
        return true;
    }
    for (size_t i = 0; i < ARRAY_SIZE(rates); i++) {
        if (rates[i] == value) {
            return true;
        }
    }
    return false;
}

static bool stick_setting(enum rethink_tp_setting s) {
    return s >= RETHINK_TP_SENSITIVITY && s <= RETHINK_TP_PRESS_TO_SELECT;
}

#if IS_ENABLED(CONFIG_SETTINGS)
#define SETTINGS_ROOT "rethink/tp"

// Settings not saved yet, one bit each
static atomic_t unsaved;

// A setting at its default is deleted, so a new Kconfig default applies
static void save(struct k_work *work) {
    atomic_val_t mask = atomic_clear(&unsaved);

    for (int s = 0; s < RETHINK_TP_SETTINGS; s++) {
        if (!(mask & BIT(s))) {
            continue;
        }

        char key[48];
        int32_t value = setting(s);
        int err;

        snprintk(key, sizeof(key), SETTINGS_ROOT "/%s", infos[s].name);
        if (value == infos[s].def) {
            err = settings_delete(key);
        } else {
            err = settings_save_one(key, &value, sizeof(value));
        }
        if (err && err != -ENOENT) {
            LOG_ERR("Failed to save %s (%d)", key, err);
        }
    }
}

static K_WORK_DELAYABLE_DEFINE(save_work, save);

static void save_later(atomic_val_t mask) {
    atomic_or(&unsaved, mask);
    k_work_reschedule(&save_work, SAVE_DELAY);
}

// Settings loaded at boot (main(), before the stick starts)
static K_SEM_DEFINE(settings_loaded, 0, 1);

static int tp_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                           void *cb_arg) {
    for (int s = 0; s < RETHINK_TP_SETTINGS; s++) {
        const char *next;
        int32_t value;

        if (!settings_name_steq(name, infos[s].name, &next) || next) {
            continue;
        }
        if (len != sizeof(value) || read_cb(cb_arg, &value, sizeof(value)) != sizeof(value)) {
            return -EINVAL;
        }
        if (setting_valid(s, value)) {
            atomic_set(&values[s], value);
        } else {
            LOG_WRN("Saved %s %d out of range: default %d", infos[s].name, value, infos[s].def);
        }
        return 0;
    }
    return -ENOENT;
}

static int tp_settings_commit(void) {
    k_sem_give(&settings_loaded);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rethink_tp, SETTINGS_ROOT, NULL, tp_settings_set,
                               tp_settings_commit, NULL);
#else
static void save_later(atomic_val_t mask) {}
#endif

int rethink_tp_setting_set(enum rethink_tp_setting s, int value) {
    if (!setting_valid(s, value)) {
        return -EINVAL;
    }
    if (atomic_set(&values[s], value) == value) {
        return 0;
    }

    save_later(BIT(s));
    if (s == RETHINK_TP_RATE || s == RETHINK_TP_RATE_USB) {
        LOG_INF("%d reports/s %s%s", value, s == RETHINK_TP_RATE ? "on battery" : "with USB power",
                slow_idle() ? ", 80 while still" : "");
    } else {
        LOG_INF("%s %d", infos[s].name, value);
    }
    if (stick_setting(s)) {
        atomic_set(&stick_changed, 1);
    }
    if (s == RETHINK_TP_RATE || s == RETHINK_TP_RATE_USB || stick_setting(s)) {
        post_request(REQ_CONFIG);
    }
    return 0;
}

void rethink_tp_settings_reset(void) {
    for (int s = 0; s < RETHINK_TP_SETTINGS; s++) {
        rethink_tp_setting_set(s, infos[s].def);
    }
    // Including values saved under an older default
    save_later(BIT_MASK(RETHINK_TP_SETTINGS));
}

// One step along rates[] for the rate in effect, stopping at either end
static int rate_step(int step) {
    enum rethink_tp_setting s = rate_setting();
    int rate = setting(s);

    for (int i = 0; i < (int)ARRAY_SIZE(rates); i++) {
        if (rates[i] == rate) {
            return rethink_tp_setting_set(s, rates[CLAMP(i + step, 0, (int)ARRAY_SIZE(rates) - 1)]);
        }
    }
    return -EINVAL;
}

int rethink_tp_request(uint32_t cmd) {
    switch (cmd) {
    case TP_TOG:
        cmd = pointing ? TP_OFF : TP_ON;
        break;
    case TP_RATE_UP:
        return rate_step(1);
    case TP_RATE_DN:
        return rate_step(-1);
    case TP_PTS_TOG:
        return rethink_tp_setting_set(RETHINK_TP_PRESS_TO_SELECT,
                                      !setting(RETHINK_TP_PRESS_TO_SELECT));
    case TP_OFF:
    case TP_ON:
    case TP_RECAL:
    case REQ_WAKE:
        break;
    default:
        return -EINVAL;
    }

    post_request(cmd);
    return 0;
}

// Packets assembled so far, in order
static void report_queued(void) {
    uint8_t p[3];

    while (k_msgq_get(&packet_queue, p, K_NO_WAIT) == 0) {
        if (pointing) {
            report(p);
            note_use();
        }
    }
}

// 0xFE, which the stick doesn't acknowledge: it sends its last packet again
static bool resend_once(void) {
    uint8_t p[3];

    if (rethink_tp_write(PS2_RESEND) || read_reply(p, 1, REPLY_TIMEOUT)) {
        return false;
    }
    // 0xFE back: our byte arrived broken
    if (p[0] == PS2_RESEND || !(p[0] & PKT_ALWAYS_1) || read_reply(&p[1], 2, REPLY_TIMEOUT)) {
        rethink_tp_flush();
        return false;
    }

    report(p);
    note_use();
    return true;
}

// Until the clock has been quiet for PACKET_END_US: the packet on its way
// is complete
static void wait_packet_end(void) {
    while (k_cyc_to_us_floor32(k_cycle_get_32() - rx_last) < PACKET_END_US) {
        k_sleep(K_USEC(PACKET_END_US / 4));
    }
}

// A packet with an error is asked for again before the stick's next one
// (TrackPoint specification 2.4.2). Otherwise a lost packet could take a
// button change with it.
static void resend_locked(void) {
    // The packets before it go out first
    report_queued();

    wait_packet_end();
    if (!pkt_broken) {
        // The next packet came first: asking now would bring that one again
        stats.lost++;
        return;
    }

    commanding = true;
    rethink_tp_flush();

    bool resent = false;

    for (int attempt = 0; attempt < RESEND_ATTEMPTS && !resent; attempt++) {
        resent = resend_once();
    }
    if (resent) {
        stats.resent++;
    } else {
        stats.lost++;
    }

    pkt_len = 0;
    pkt_broken = false;
    commanding = false;
}

static void resend_packet(void) {
    k_mutex_lock(&start_lock, K_FOREVER);
    if (pointing) {
        resend_locked();
    }
    k_mutex_unlock(&start_lock);
}


// A new report rate or stick settings while pointing, with streaming off
// around them (specification 2.7): about 10 ms without packets for a rate. A
// failure restarts the stick.
static void reconfigure_locked(uint8_t rate, bool stick) {
    wait_packet_end();
    commanding = true;
    report_queued();
    rethink_tp_flush();

    const uint8_t set_rate[] = {PS2_SET_RATE, rate};
    int err = stream_set(false);

    if (!err && rate != rate_now) {
        err = commands(set_rate, sizeof(set_rate));
    }
    if (!err && stick) {
        err = stick_settings_write(false);
    }
    if (!err) {
        err = stream_set(true);
    }

    pkt_len = 0;
    pkt_broken = false;
    commanding = false;

    if (err) {
        LOG_WRN("Failed to change the rate or settings (%d), restarting the stick", err);
        start_locked();
        return;
    }
    note_rate(rate);
    stats.rate_changes++;
    LOG_DBG("%d reports/s%s", rate, stick ? ", stick settings written" : "");
}

static bool reconfigure_due(void) {
    return pointing && (rate_wanted() != rate_now || atomic_get(&stick_changed));
}

static void reconfigure(void) {
    if (!reconfigure_due()) {
        return;
    }

    k_mutex_lock(&start_lock, K_FOREVER);
    if (reconfigure_due()) {
        reconfigure_locked(rate_wanted(), atomic_get(&stick_changed));
    }
    k_mutex_unlock(&start_lock);
}

static void carry_out(atomic_val_t req) {
    if (req & BIT(REQ_RESEND)) {
        resend_packet();
    }
    if (req & BIT(TP_OFF)) {
        rethink_tp_power_off();
    }
    // A wake-up only if no key switched the stick off in the meantime
    if ((req & BIT(TP_ON)) || ((req & BIT(REQ_WAKE)) && atomic_get(&idle_off))) {
        if (!pointing) {
            rethink_tp_start();
        }
    }
    if (req & BIT(TP_RECAL)) {
        rethink_tp_recalibrate();
    }
}

// Milliseconds until the idle power-off is due (0: now), -1 for none
static int32_t idle_left_ms(void) {
    if (IDLE_OFF_MS == 0 || !pointing) {
        return -1;
    }

    int32_t unused = k_uptime_get_32() - (uint32_t)atomic_get(&last_use);

    return MAX(IDLE_OFF_MS - unused, 0);
}

// Milliseconds until the rate drops for the stick being still, -1 for never
static int32_t slow_left_ms(void) {
    if (!pointing || !slow_idle() || rate_now == IDLE_RATE) {
        return -1;
    }

    return MAX(SLOW_IDLE_MS - (int32_t)still_ms(), 0);
}

static k_timeout_t next_timeout(void) {
    int32_t idle = idle_left_ms();
    int32_t slow = slow_left_ms();
    int32_t ms = idle < 0 ? slow : slow < 0 ? idle : MIN(idle, slow);

    return ms < 0 ? K_FOREVER : K_MSEC(ms);
}

// Starts the stick after boot (the adapter is up by then), then reports
// packets, carries out requests, sets the rate for moving or still and
// powers the stick off when unused
static void tp_thread(void *p1, void *p2, void *p3) {
    struct k_poll_event events[] = {
        K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &request_signal),
        K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
                                 &packet_queue),
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    // The saved settings first; a timeout in case nothing loads them
    if (k_sem_take(&settings_loaded, K_SECONDS(2))) {
        LOG_WRN("Settings not loaded: defaults");
    }
#endif
    rethink_tp_start();

    for (;;) {
        k_poll(events, ARRAY_SIZE(events), next_timeout());

        if (events[0].state == K_POLL_STATE_SIGNALED) {
            k_poll_signal_reset(&request_signal);
            carry_out(atomic_clear(&requests));
        }
        events[0].state = K_POLL_STATE_NOT_READY;
        events[1].state = K_POLL_STATE_NOT_READY;

        report_queued();
        reconfigure();

        if (idle_left_ms() == 0) {
            LOG_INF("Unused for %d min: pointing stick off until a key is pressed",
                    CONFIG_RETHINK_TRACKPOINT_IDLE_OFF_MIN);
            rethink_tp_power_off();
            atomic_set(&idle_off, 1);
        }
    }
}

#if IDLE_OFF_MS > 0 || IS_ENABLED(CONFIG_ZMK_USB)
// Key presses count as use, and wake a stick powered off for lack of it.
// USB power coming or going may change the rate.
static int tp_listener(const zmk_event_t *eh) {
#if IDLE_OFF_MS > 0
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev && ev->state) {
        note_use();
        if (atomic_get(&idle_off)) {
            rethink_tp_request(REQ_WAKE);
        }
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        post_request(REQ_CONFIG);
    }
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rethink_trackpoint, tp_listener);
#if IDLE_OFF_MS > 0
ZMK_SUBSCRIPTION(rethink_trackpoint, zmk_position_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(rethink_trackpoint, zmk_usb_conn_state_changed);
#endif
#endif

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
