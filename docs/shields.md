# Shields

What each shield does and how to use it. Build any of them with `.\build.ps1 -Shield <shield>` ([Building](../README.md#building)).

## `rethink_bare_tester`

Tests a bare Control Board; no adapter needed.

- **Matrix:** 8x15 on the header pins the adapter uses: KSI0-7 (P0.00-P0.07) by KSO0-14 (P1.00, P1.01, P1.18, P1.19, P1.04-P1.14). Columns are driven low and rows pulled up, as on the ThinkPad matrices. Short a KSI pin to a KSO pin to press a key. Each position sends a different key (row r, column c = position r*15 + c, see the keymap). KSO15/16 (P1.15/P1.16, TrackPoint) and KSO17 (P1.17, backlight PWM) are left out.
- **Pairing button** (P3.10, outside the matrix): tap for the next Bluetooth profile (at once; on `rethink_hw_tester` 0.4 s after the tap, to tell it apart from the triple tap that unlocks the backlight there); hold for 3 s, then release, to clear the active profile. Both are ignored while the mode switch is on wired. Hold for 10 s to reboot into MCUboot recovery (keep holding until the green LED lights); holding it while plugging in USB also enters recovery.
- **USB/Bluetooth switch** (P3.05, outside the matrix): Bluetooth sends over Bluetooth, falling back to USB when no host is connected. Wired turns Bluetooth off and sends over USB.
- **White LEDs 1-4:** show the active Bluetooth profile (4 profiles) for 3 s after boot and after each change, blinking while the profile is unpaired.
- **RGB LED:** blue for 3 s when a host connects (USB, or the active Bluetooth profile) and after switching to a connected profile.
- **Power off:** in wired mode without USB, on a low battery and when unused; see [Power off](power.md#power-off).
- **P3.11** (Framework's backlight PWM): disabled. On ReThink it enables the adapter's backlight boost converter.
- **Name:** shows up as "ReThink Tester" over USB and Bluetooth.

## `rethink_hw_tester`

Tests the ReThink adapter. Everything from `rethink_bare_tester` except its matrix, plus the following:

- **Matrix:** the adapter's whole matrix (`rethink_adapter_matrix.dtsi`, driver `rethink,matrix`, with every position in `rethink_adapter_full_layout.dtsi`): SENSE0-7 by DRV0-14 on the Control Board and DRV15-17 on the expander, plus HOTKEY and PWRSWITCH as direct keys in row 8. Row r, column c = position r*18 + c; HOTKEY and PWRSWITCH are 144 and 145. Columns are open-drain and ghost keys are blocked, as diodeless ThinkPad matrices need. Every position sends a different key (see the keymap), and the log shows `position: N` for each press. Without a keyboard, short a SENSE pin to a DRV pin, e.g. on the Mezzanine connector SENSE0 (pin 5) to DRV15 (pin 30) for position 15, or HOTKEY (pin 1) to GND (pin 34) for 144. Without the adapter, the Control Board part still works on the header.
- **Adapter detection:** probes the PCAL6416A at 0x20 on `i2c22` (first switching on TP_3V3 if the expander runs from it, JP10 2-3). If it doesn't answer, it logs `ReThink adapter not found` and carries on without it.
- **Status LEDs** *(needs adapter)*: all five, as on an FFC2 keyboard with a numpad. At boot, Fn lock/power, mute, mic mute, Caps Lock and Num Lock light one after another, then all together, then go off. After that, Caps Lock and Num Lock follow the host's lock state. A lit LED pulls its expander pin low; an unlit one rests on the expander's pull-up.
- **Idle expander pins** *(needs adapter)*: every pin nothing drives rests on the expander's pull-up (`idle-gpios` in `rethink_adapter.dtsi`), also in System OFF, since a floating input draws up to 80 µA from LED_3V3. TP1/TP2_RST rest on the pull-down instead: R14/R22 go to the pointing stick supplies, which float while off.
- **Backlight power chain:** nPM1300 BUCK1 (if it is the VLED_BL source) → P3.11 (VLED_BL to the adapter) → BOOST_EN (expander P0_1) → PWM 4096 Hz on P1.17, duty cycle = brightness × `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY`. The chain is on only while the brightness is above 0.
- **Backlight interlock:** KBD_BL_DTCT (expander P1_7) is read with the pull-up enabled at boot and again each time the backlight switches on; a backlit keyboard pulls it low. The pull-up then stays on unless the pin is low. Without a detected backlit keyboard (or without the adapter), the backlight stays locked and logs `Backlight locked`. Triple-press the pairing button to unlock it until reboot (this tester only, `CONFIG_RETHINK_BACKLIGHT_TAP_UNLOCK`: keyboards unlock only for a detected backlit keyboard).
- **Keymap:** row 0 (KSI0) × KSO0..4 are backlight toggle, down (20 % steps), up, Caps Lock and Num Lock. The backlight starts on at 40 %.
- **Name:** "ReThink HW Test" (BLE names are limited to 16 characters).

Test on a bare Control Board (build with `-Snippet rethink-usb-log` to watch the log):

1. Boot. The log shows `ReThink adapter not found`, `Ready: VLED_BL from BUCK1, max duty 10%`, then `Backlight locked: backlit keyboard unknown`.
2. Triple-press the pairing button. The log shows `Backlight unlocked`, `Adapter missing: BOOST_EN not driven` and `Power chain on`. VLED_BL is now on the 6-pin connector, and P1.17 (KSO17 on the header) carries 40 % × 10 % = 4 % PWM, which reads about 0.13 V on a multimeter.
3. Short KSI0 to KSO1 (down) or KSO2 (up) to change the duty cycle; KSO0 toggles the backlight. Off logs `Power chain off`, and VLED_BL and PWM drop to 0.

### Adapter bring-up shell

Build with `.\build.ps1 -Shield rethink_hw_tester -Snippet rethink-usb-shell` and open the board's USB serial port in a terminal program. The `adapter` commands switch the adapter's supplies one at a time, for an adapter without a keyboard. They bypass the backlight interlock, so don't use them with a keyboard connected, except `status`, `in`, `int` and `shorts`, which only read pins or pull single lines low. The full procedure, with expected readings, is [docs/adapter_bringup.md](https://github.com/Ziemniaczenka/ReThink_Keyboard/blob/main/docs/adapter_bringup.md) in the hardware repository.

| Command | Does |
| --- | --- |
| `adapter status` | TP_3V3, VLED_BL, INT and the level of every expander pin |
| `adapter tp on\|off` | TP_3V3 (P3.12), off from boot. `off` switches the charge pump off first (PUMP_EN low) |
| `adapter pump on\|off` | PUMP_EN: charge pump TP_5V. `on` is refused while TP_3V3 is off: the LM2775's EN must stay within 0.3 V of its supply |
| `adapter vled on\|off` | VLED_BL to the adapter: BUCK1 (if it is the source) and P3.11 |
| `adapter boost on\|off` | BOOST_EN: backlight boost BL_5V (needs VLED_BL) |
| `adapter pwm <0-100>` | Duty cycle on P1.17, limited to `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY` |
| `adapter pin <name> 0\|1\|z\|u\|d` | An expander pin driven low or high, released, or released with its 100 kΩ pull-up or pull-down. Names as in `adapter status` |
| `adapter in` | HOTKEY, PWRSWITCH and KBD_BL_DTCT, read with pull-ups |
| `adapter int [s]` | Waits for INT (P3.06) while an input is shorted to GND, then checks that reading the inputs releases it |
| `adapter shorts` | Drives the 40 lines to the keyboard connectors low one at a time and reports lines that follow (shorted) or are low at rest. The matrix is suspended meanwhile |

Zephyr's `i2c` and `gpio` shells are included too, e.g. `i2c scan i2c@c8000` (only 0x20 should answer). The `gpio` shell covers the Control Board ports only: the expander has no Zephyr GPIO driver (see `include/rethink/expander.h`).

## `rethink_scanner`

Maps a keyboard's matrix: which SENSE and DRV lines each key connects. Build it with the `rethink-usb-shell` snippet.

- **Matrix:** the adapter's whole matrix, as in `rethink_hw_tester`. No key sends anything, so presses don't type into the host; the log shows `Row: r, col: c` for each.
- **Nothing powered for the keyboard:** no backlight (BUCK1 stays off from boot), no status LEDs, TP_3V3 and the charge pump off. Of the `adapter` commands, use only `status`, `in`, `int` and `shorts` with a keyboard connected.
- **Name:** "ReThink Scanner".

Mapping a keyboard:

1. With all power removed, connect the keyboard (checks: [First keyboard](https://github.com/Ziemniaczenka/ReThink_Keyboard/blob/main/docs/adapter_bringup.md#5-first-keyboard) in the bring-up procedure), then plug in USB.
2. Optional: write a layout file. One line per physical row, keys left to right, as ZMK key codes; a key without one gets a label in brackets. For example:
   ```text
   ESC F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 F11 F12
   GRAVE N1 N2 N3 N4 N5 N6 N7 N8 N9 N0 MINUS EQUAL BSPC
   [FN] LCTRL LGUI LALT SPACE RALT
   ```
3. Close any terminal on the board's serial port and run one of:
   ```powershell
   python scripts\matrix_scan.py --layout l420.txt   # asks for each key by name
   python scripts\matrix_scan.py --name l420         # press the keys row by row, Enter ends a row
   ```
   The port is found by ZMK's USB IDs (otherwise `--port COMx`). On the PC's keyboard, Enter skips the key asked for, `u` undoes the last one and `q` finishes. After the layout, the script takes keys the layout missed as extras.
4. The script prints which SENSE and DRV lines the keyboard uses and writes to `out\`: `<name>_transform.dtsi` (matrix transform in physical order), `<name>.keymap` (keymap skeleton: `&kp` for key codes, `&none` for labels), `<name>_scan.json` and the raw log.

Watching the matrix, for a keyboard whose keys misbehave or to check every key of a new one: with the same setup, `python scripts\matrix_view.py` draws the matrix in the terminal (112 characters wide) and marks each position: pressed now, held for more than 3 s now or before (a key that registers at the lightest touch, a short), already held when the log started, chatter (a press within 30 ms of the last release, or shorter), pressed before, not yet. Below it, the problems in words and the last events. `--shield rethink_t420` takes key names from a shield with the same matrix and lists the keys not pressed yet; `--log out\t480_iso_scan.log` shows a saved log (the live one goes to `out\matrix_view.log`). The firmware blocks ghost keys, so they don't show.

A keyboard shield includes `rethink_adapter_matrix.dtsi` without the full layout and defines its own physical layout with that transform, as `rethink_l420` does: ZMK sizes the keymap for the largest physical layout enabled. The matrix drives and reads only the columns and direct keys that an enabled transform maps, so a keyboard without keys on DRV16/17 or without HOTKEY/PWRSWITCH saves those I2C transfers on every scan.

## `rethink_l420`

ThinkPad L420 keyboard on FFC1 (30-pin matrix cable in J4). Mapped with `rethink_scanner` on two Norwegian L420 keyboards, and in use for typing.

- **Everything from `rethink_bare_tester` except its matrix:** pairing button, USB/Bluetooth switch, profile LEDs, RGB LED, power off in wired mode.
- **Matrix:** the adapter's matrix with the L420's 85 keys (ISO) as the physical layout `l420_layout`. Keys at other positions are ignored.
- **Keymap:** each key sends the standard key code of its position, so the host's keyboard layout (e.g. Norwegian, Swedish, Danish) gives the legends. Fn (bottom left) does what the keycaps show, where a standard key code exists:

  | Fn + | Sends |
  | --- | --- |
  | Esc, F1 | Break, SysRq (Alt+PrtSc) |
  | F2, F4, F7 | Lock (Win+L), sleep, display (Win+P) |
  | F8 | TrackPoint on/off; with Ctrl: recalibrate it (hands off the stick); with Alt: press-to-select (clicking by pressing the stick) on/off; with Shift: middle-button scrolling on/off |
  | F9, F10, F11 | Scroll Lock, NmLk (embedded keypad on/off), Pause |
  | Insert, Delete, Home | PrtSc, brightness down, brightness up |
  | PgUp, PgDn | Browser back, forward |
  | Up, Down, Left, Right | Stop, play/pause, previous, next |

  Battery (F3), wireless (F5), camera (F6), hibernate (F12) and zoom (Space) have no standard key code. The embedded keypad sends the digits on 7-9, U-O, J-L and M, and `/ * - +` on 0, P, `;` and `/` (UK ISO legends), independent of the host's Num Lock.
- **Not on this keyboard:** backlight (BUCK1 stays off from boot) and status LEDs; the expander's LED pins rest on their pull-ups.
- **TrackPoint** (8-pin cable in J5; its buttons go to the stick, not the matrix): a mouse, see [Pointing stick](pointing-stick.md).
- **Name:** "ReThink L420".

Build with `-Snippet rethink-usb-log` to see `position: N` for each key in the log.

## `rethink_t420`

ThinkPad T420 keyboard on the Mezzanine connector (2x20 pins). Mapped with `rethink_scanner` on a Norwegian (ISO) and an ANSI T420 keyboard.

- **Everything from `rethink_bare_tester` except its matrix**, as `rethink_l420`.
- **Matrix:** the physical layout `t420_layout`: the keyboard, the buttons above it, Fn on HOTKEY and the power button on PWRSWITCH; 96 keys on ISO, 95 on ANSI. The two differ in three positions: ANSI has `\` above Enter (BSLH), ISO has `#` left of Enter and `\` right of LShift (NUHS, NUBS); Enter is the same. The layout holds all 97, so one firmware runs either keyboard.
- **Buttons above the keyboard:** mute, volume down, volume up, mic mute (sends F20, which Linux treats as mic mute). ThinkVantage and power send nothing. The keys left and right of Up are browser back and forward.
- **Keymap:** standard key codes by position, as `rethink_l420`. Fn does what the keycaps show:

  | Fn + | Sends |
  | --- | --- |
  | PrtSc, ScrLk, Pause | SysRq (Alt+PrtSc), NmLk (embedded keypad on/off), Break |
  | F2, F4, F7 | Lock (Win+L), sleep, display (Win+P) |
  | F8 | TrackPoint on/off; with Ctrl: recalibrate it (hands off the stick); with Alt: press-to-select (clicking by pressing the stick) on/off; with Shift: middle-button scrolling on/off |
  | Home, End | Brightness up, down |
  | Up, Down, Left, Right | Stop, play/pause, previous, next |

  Battery (F3), wireless (F5), camera (F6), hibernate (F12), ThinkLight (PgUp) and zoom (Space) have no standard key code. The embedded keypad is the same as on `rethink_l420`.
- **Caps Lock LED** (Mezzanine 21) follows the host. The power, mute and mic mute LEDs rest on their pull-ups.
- **Not on this keyboard:** backlight. KBDID0-2 (Mezzanine 25, 27, 29) meet the adapter's backlight PWM, GND and BL_5V, so keep JP3 open; P1.17 stays unused.
- **TrackPoint** (Mezzanine 37-40): a mouse, see [Pointing stick](pointing-stick.md).
- **Name:** "ReThink T420".

## `rethink_t480`

ThinkPad T480 keyboard on FFC2: the 36-pin matrix cable and the 12-pin cable (TrackPoint, backlight). Mapped with `rethink_scanner` on a UK ISO T480 keyboard. Tried on a T480 (01AX516): typing, Caps Lock LED, the stick (4.5 mA with it, 1.6 mA without).

- **Everything from `rethink_bare_tester` except its matrix**, as `rethink_l420`.
- **Matrix:** the physical layout `rethink_t480_layout`, 86 positions: every scanned key is at its T420 position. The scanned keyboard is faulty (U, I, O, P never register, J at the lightest touch), so those five are the T420's positions, as is the ANSI keyboard's `\` above Enter. Fn is on HOTKEY. Cable pin 36 is KBD_ID, on DRV16, which the matrix leaves alone.
- **Keymap:** standard key codes by position, as `rethink_l420`; F1-F12 send F1-F12. Fn does what the keycaps show:

  | Fn + | Sends |
  | --- | --- |
  | F1-F6 | Mute, volume down, volume up, mic mute (F20), brightness down, up |
  | F7, F9 | Display (Win+P), settings (Win+I) |
  | F8 | TrackPoint on/off; with Ctrl: recalibrate it (hands off the stick); with Shift: middle-button scrolling on/off (airplane mode has no key code) |
  | 4, K, P, B, S | Sleep, ScrLk, Pause, Break, SysRq |
  | PrtSc | Snipping tool (Win+Shift+S) |

  Fn lock (Fn+Esc), Bluetooth (F10), keyboard settings (F11), the star key (F12) and the backlight (Fn+Space) are left out for now.
- **Caps Lock LED** follows the host. The Fn lock, mute and mic mute LEDs rest on their pull-ups.
- **Not yet:** the backlight (BUCK1 stays off). KBD_BL_DTCT is read (`bl-detect-gpios`) all the same: a backlit keyboard grounds it, and it then rests on a pull-down; on its pull-up it drew about 35 µA on the E580, System OFF included.
- **TrackPoint:** TP2 on the 12-pin cable, at 3.3 V (adapter JP7, JP8, JP9 at 1-2, `CONFIG_RETHINK_TP2_SUPPLY_3V3`), a mouse; its buttons come over the matrix cable (pins 33-35) to the adapter and on to the stick. This generation's sticks (T480, E580) have no press-to-select, so Alt+Fn+F8 isn't bound to it. See [Pointing stick](pointing-stick.md).
- **Name:** "ReThink T480".

## `rethink_e580`

ThinkPad E580 keyboard with numpad on FFC2: the 40-pin matrix cable and the 12-pin cable. Mapped with `rethink_scanner` on a US ANSI E580 keyboard. Tried on an E580 (01YN773): typing, the lock LEDs, the stick (2.1 mA with it, 1.5 mA without).

- **As `rethink_t480`,** with the numpad: the physical layout `rethink_e580_layout`, 107 positions. The main block is at the T420's positions, the ISO keyboard's `#` and `\` right of LShift included (unscanned); the numpad on DRV16/17 and SENSE2/3 x DRV9/11/12.
- **Numpad:** keypad codes, so the host's Num Lock picks digits or navigation; its top row sends keypad `=`, `(` and `)` (Linux and Android take those two, Windows ignores them) and Backspace. Fn+keypad `=` opens the calculator and Fn+Backspace sleeps, as their keycaps show.
- **Caps Lock and Num Lock LEDs** follow the host.
- **Name:** "ReThink E580".
