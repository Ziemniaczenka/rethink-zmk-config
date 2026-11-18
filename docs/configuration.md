# Configuration

How the adapter and Control Board are assembled ([Hardware options](#hardware-options), `config/rethink.conf`) and the firmware's Kconfig options.

## Hardware options

[config/rethink.conf](../config/rethink.conf) describes how the adapter and Control Board are assembled: jumpers, fitted parts, and where the backlight power comes from. ZMK applies it to every shield whose name starts with `rethink_` (it also looks for `<shield prefix>.conf` in `config/`), locally and in CI. The defaults match a stock adapter and Control Board; change the lines that differ on yours.

| Option | Default | Hardware | Effect |
| --- | --- | --- | --- |
| `CONFIG_RETHINK_EXPANDER_SUPPLY_LED_3V3` / `_TP_3V3` | LED_3V3 | Adapter JP10 | With TP_3V3, P3.12 is switched on before the expander is probed and stays on |
| `CONFIG_RETHINK_PUMP_EN_TIED` | n | Adapter JP1 closed | PUMP_EN (P0_0) left as an input |
| `CONFIG_RETHINK_BOOST_FITTED` | y | U4 (n on Mezz_no_BL, FFC1) | n: the backlight is never powered |
| `CONFIG_RETHINK_BOOST_EN_TIED` | n | Adapter JP2 closed | BOOST_EN (P0_1) left as an input |
| `CONFIG_RETHINK_BACKLIGHT_SUPPLY_BUCK1` / `_VBAT` / `_VSYS` | BUCK1 | Control Board R62 / R59 (VBAT) / VSYS on the next revision | BUCK1: the regulator is switched with the backlight |
| `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY` | 10 (range 1-20), 20 on `rethink_e580` | | Duty cycle at 100 % brightness, to stay within the supply's current. 10 % until a keyboard is measured; the E580 takes about 5 mA from the battery per 1 %. `backlight max` changes it until reboot, up to 50 |
| `CONFIG_RETHINK_TP1_SUPPLY_5V` / `_3V3` | 5V | Adapter JP6 (+ JP4, JP5) | Read by the pointing stick driver |
| `CONFIG_RETHINK_TP2_SUPPLY_3V3` / `_5V` | 3V3 | Adapter JP7 (+ JP8, JP9) | Read by the pointing stick driver |
| `CONFIG_RETHINK_TP2_RESET_PULL_UP` / `_DOWN` | pull-up | Adapter R22 / R23 | Read by the pointing stick driver |
| `CONFIG_RETHINK_MCUBOOT_NEEDS_POWER_CYCLE` | y | Factory MCUboot (Framework's daisy branch before 2026-10-16) | The recovery hold power-cycles through the nPM1300; n: soft reset |

The expander pin map, the jumpers and which keyboard uses which pin are in the hardware repository: [docs/pinouts/io_expander.md](https://github.com/Ziemniaczenka/ReThink_Keyboard/blob/main/docs/pinouts/io_expander.md).

What a keyboard has (status LEDs, KBD_BL_DTCT pin, backlight) is set by its shield, not here: shields list their LEDs on `&rethink_adapter` and include `rethink_backlight.dtsi` only when the keyboard is backlit.

## Kconfig options

| Option | Default | Description |
| --- | --- | --- |
| `CONFIG_RETHINK_PAIRING_BUTTON` | y | Pairing button: tap = next profile, hold = clear profile, triple tap = unlock backlight (hardware tester) |
| `CONFIG_RETHINK_PAIRING_BUTTON_HOLD_MS` | 3000 | Hold time to clear (on release) |
| `CONFIG_RETHINK_PAIRING_BUTTON_RECOVERY_MS` | 10000 | Hold time to reboot into the bootloader, 0 = off |
| `CONFIG_RETHINK_POWER` | y | Power off (System OFF with everything off) and its reasons below; `power` shell commands |
| `CONFIG_RETHINK_LOW_BATTERY_OFF_MV` | 3300 | Power off on battery below this voltage, until USB; 0 = never |
| `CONFIG_RETHINK_LOW_BATTERY_SHIP_MODE` | y | That power-off in the nPM1300's ship mode (everything off) instead of System OFF |
| `CONFIG_RETHINK_BUCK2_LOW_MV` | 3550 | BUCK2 at 3.0 V under this battery voltage, out of dropout (about 3 mA); 0 = never switched |
| `CONFIG_RETHINK_BUCK2_HIGH_MV` | 3650 | BUCK2 at 3.3 V again over this battery voltage |
| `CONFIG_RETHINK_RESET_LOOP_OFF` | 3 | Ship mode at a start on battery after this many resets in a row (a start counts until 30 s up); 0 = never |
| `CONFIG_RETHINK_PMIC` | y | BUCK1 off at boot (the board's boot-off doesn't take); `pmic` shell command |
| `CONFIG_RETHINK_BUCK2_RESTART` | y | From a cell under `CONFIG_RETHINK_BUCK2_LOW_MV`, BUCK2 set to 3.0 V at boot and switched off and on; at boot and each battery check, a BUCK2 stuck in PWM mode switched off and on |
| `CONFIG_RETHINK_USB_1500MA` | y | USB input limit 1.5 A when a USB-C source offers 1.5 A or 3 A (the nPM1300's CC detection), else the board's 500 mA |
| `CONFIG_RETHINK_IDLE_SLEEP_MIN` | 30 | Power off after this long unused on battery, until a key, USB or the switch; 0 = never |
| `CONFIG_RETHINK_WIRED_OFF` | y | System OFF in wired mode while USB is unplugged |
| `CONFIG_RETHINK_WIRED_OFF_DELAY_MS` | 3000 | Delay before powering off |
| `CONFIG_RETHINK_PAIRING_BUTTON_TAP_WINDOW_MS` | 400 with `RETHINK_BACKLIGHT_TAP_UNLOCK`, else 0 | Maximum gap between taps; 0: a tap acts at once (no triple tap) |
| `CONFIG_RETHINK_PROFILE_LEDS` | y | Active profile on the white LEDs |
| `CONFIG_RETHINK_PROFILE_LEDS_TIMEOUT_MS` | 3000 | How long the LED stays on |
| `CONFIG_RETHINK_CONNECTED_LED` | y | Blue LED after connecting (replaces framework-cb's `CONFIG_FRAMEWORK_CB_STATUS_LED`, which the shields set to n) |
| `CONFIG_RETHINK_CONNECTED_LED_TIMEOUT_MS` | 3000 | How long the blue LED stays on |
| `CONFIG_RETHINK_ADAPTER` | if `rethink,adapter-ctrl` in DT | Adapter bring-up, status LEDs, BOOST_EN, KBD_BL_DTCT |
| `CONFIG_RETHINK_ADAPTER_LED_SELFTEST` | n | Light each status LED in turn at boot |
| `CONFIG_RETHINK_ADAPTER_TEST` | n | `adapter` shell commands for bring-up (set by the `rethink-usb-shell` snippet) |
| `CONFIG_RETHINK_MATRIX` | if `rethink,matrix` in DT | Matrix with expander columns, direct keys and ghost-key blocking |
| `CONFIG_RETHINK_BACKLIGHT` | if `rethink,backlight` in DT | Backlight power chain, PWM and interlock (use with `CONFIG_ZMK_BACKLIGHT`) |
| `CONFIG_RETHINK_BACKLIGHT_RESTORE` | n | The backlight's last step at each start (ZMK saves it a minute after a change); n: each start as the first (`CONFIG_ZMK_BACKLIGHT_ON_START`, off on the FFC2 shields). Either way it stays off until the settings are loaded |
| `CONFIG_ZMK_IDLE_TIMEOUT` (ZMK's) | 30000 | Milliseconds without a key or the stick before the backlight goes off (`CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE`, set in the FFC2 shields); nothing else uses ZMK's idle state here |
| `CONFIG_RETHINK_BACKLIGHT_TAP_UNLOCK` | y on `rethink_hw_tester`, else n | Triple tap of the pairing button unlocks the backlight until reboot, bypassing KBD_BL_DTCT |
| `CONFIG_RETHINK_TRACKPOINT` | if `rethink,trackpoint` in DT | Pointing stick power, reset, PS/2 link and pointing |
| `CONFIG_RETHINK_TRACKPOINT_IDLE_OFF_MIN` | 10 | Minutes with neither the stick nor a key used before the stick is powered off; 0 = never |
| `CONFIG_RETHINK_TRACKPOINT_GUARD` | y | After a switch-on on battery that reset the board (a mark in flash), the stick stays off at the next boot until USB power or `&tp TP_ON` |
| `CONFIG_RETHINK_TRACKPOINT_SAMPLE_RATE` | 80 | Reports per second on battery (10, 20, 40, 60, 80, 100, 200); `tp set rate` and the keys change it (kept in flash) |
| `CONFIG_RETHINK_TRACKPOINT_SAMPLE_RATE_USB` | 100 | Reports per second while USB powers the keyboard; `tp set rate_usb` |
| `CONFIG_RETHINK_TRACKPOINT_SLOW_IDLE` | n | Experimental: 80 reports/s after 2 s without movement, the set rate from the next movement packet |
| `CONFIG_RETHINK_TRACKPOINT_SENSITIVITY` | 192 | The stick's force scale (×1.5; 128 = ×1.0, its power-on value); `tp set sensitivity` |
| `CONFIG_RETHINK_TRACKPOINT_NEG_INERTIA` | 6 | The stick's negative inertia, 0 off; `tp set inertia` |
| `CONFIG_RETHINK_TRACKPOINT_SPEED` | 150 | The stick's upper plateau speed (about 667 counts/s; power-on 97, 439 counts/s); `tp set speed` |
| `CONFIG_RETHINK_TRACKPOINT_DRIFT_TIME` | 10 | Hands-off time before drift correction, 107 ms steps (1.07 s; power-on 5); `tp set drift_time` |
| `CONFIG_RETHINK_TRACKPOINT_PRESS_TO_SELECT` | n | Pressing the stick clicks; `tp set press_to_select` |
| `CONFIG_RETHINK_TRACKPOINT_SCROLL` | y | Middle button held: the stick scrolls; the middle button clicks only without movement |
| `CONFIG_RETHINK_TRACKPOINT_MIDDLE_SCROLL` | y | Start with the middle button scrolling; n: a plain middle button. `tp set middle_scroll`, Shift+Fn+F8 |
| `CONFIG_RETHINK_TRACKPOINT_SCROLL_DIVISOR` | 20 | Stick counts per wheel step; `tp set scroll_divisor` |
| `CONFIG_RETHINK_TRACKPOINT_SCROLL_H_DEAD_ZONE` | 2 | Sideways counts per report that don't scroll; `tp set scroll_dead_zone` |
| `CONFIG_ZMK_POINTING_SMOOTH_SCROLLING` (ZMK's) | y with scrolling | The wheel in sixteenths of a step for hosts that set the HID Resolution Multiplier |
| `CONFIG_RETHINK_TRACKPOINT_SHELL` | y with a shell | `tp` shell commands |
| `CONFIG_RETHINK_BLE_SHELL` | y with a shell | `ble conn`: the Bluetooth connection interval; `ble log`: the connections since boot and the last reset's cause |
| `CONFIG_RETHINK_RESET_DIAG` | y with a shell | Reset cause, and a record of a fatal error (then an immediate reboot) or a work queue stall for after the reset; `diag crash`, `diag stall` |

Hardware options: see [Hardware options](#hardware-options) above.

framework-cb adds `CONFIG_FRAMEWORK_CB_MODE_SWITCH`, `CONFIG_FRAMEWORK_CB_STATUS_LED`, `CONFIG_FRAMEWORK_CB_FUEL_GAUGE` and `CONFIG_FRAMEWORK_CB_WATCHDOG`. See its [README](https://github.com/Ziemniaczenka/zmk-config-fw-cb).
