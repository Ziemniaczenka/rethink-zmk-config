# ReThink ZMK config

ZMK firmware for the [ReThink Keyboard](https://github.com/Ziemniaczenka/ReThink_Keyboard): ThinkPad keyboard modules driven by the Framework Wireless Touchpad Keyboard Control Board (nRF54LM20A + nPM1300) through the ReThink adapter.

> Work in progress. Shields so far: the testers `rethink_bare_tester` and `rethink_hw_tester`, the matrix scanner `rethink_scanner`, and the keyboards `rethink_l420` and `rethink_t420`.

## Roadmap

- [x] Container build (local and CI), flashing over USB, bare Control Board tester
- [x] Adapter control, backlight power chain and interlock, hardware options (`config/rethink.conf`)
- [x] Power off in wired mode without USB; charger current fix (framework-cb)
- [x] Adapter bring-up: `rethink-usb-shell` commands, idle expander pins on pulls (System OFF at the bare board's current)
- [x] Key matrix: expander register access without Zephyr's PCAL64xxA driver (a column step is one 1-byte I2C write), open-drain columns for diodeless ThinkPad matrices, ghost-key blocking, expander columns DRV15-17, HOTKEY/PWRSWITCH through the expander interrupt. Tested with shorts on the connectors
- [x] Matrix scanner: `rethink_scanner` and `scripts/matrix_scan.py`, which asks for each key, reads its row and column from the USB log, and writes the matrix transform and a keymap. First keyboard mapped: L420 (FFC1)
- [x] Shield `rethink_l420` (FFC1): keymap from the scan, ISO, Fn layer per the keycaps; TrackPoint comes later
- [x] Blue LED only for 3 s after connecting, not the whole time
- [x] Matrix scans only the columns and direct keys the layout maps (the L420 skips DRV16/17 and HOTKEY/PWRSWITCH: two I2C transfers per scan instead of five)

Next, in this order:

- [ ] Shield `rethink_t420` (Mezzanine): keymap from the scan, buttons above the keyboard, Fn layer per the keycaps, Caps Lock LED. Built, not yet tested on the keyboard
- [ ] TrackPoint, TP1 on the T420 and L420 (see [Pointing stick](#pointing-stick-in-progress)):
  - [x] Power, reset and the bit-banged PS/2 link, `tp` shell commands. Tested on the T420: ID 01/0e, 14.7 kHz, no bit errors on USB, one byte in ~1500 broken off with Bluetooth
  - [x] Pointing: start at boot (self-test, ID, streaming on), packets to ZMK's pointing input over USB and Bluetooth, the stick's buttons. Built, not yet tested
  - [ ] Mouse buttons on keys for the T420 (its buttons are on the palmrest), e.g. on a layer while the stick moves
  - [ ] If Bluetooth costs more packets than it does now: receive through UARTE20 instead (a PS/2 byte is UART 8O1 at the stick's clock rate) and keep bit-banging for sending
  - [ ] Power: the stick off after inactivity and on with the next key, its current measured
  - [ ] Settings: sensitivity, press-to-select, scrolling with the middle button
  - TP2 (FFC2) comes with `rethink_t480`
- [ ] Battery: fuel gauge model of the real cell (the "Example" model reads 50 % at 4155 mV), charge-current reading (fuel gauge +525 mA, meter 440 mA), discharge limit (200 mA, 1 A with the backlight), USB 1.5 A, low-battery cutoff
- [ ] Idle current on Bluetooth. Measured on battery with a multimeter:

  | Setup | Connected | Advertising |
  | --- | --- | --- |
  | `rethink_l420`, adapter and keyboard | 1.58 mA | |
  | `rethink_l420`, adapter, no keyboard | 1.58 mA | 1.7 mA |
  | `rethink_bare_tester`, bare Control Board | 1.6 mA | 1.7 mA |
  | System OFF (wired mode, no USB) | 6-8 µA | |
  | A key held (1 kHz scan with I2C, before the column skip above) | +1.2 mA | |

  So a floor of about 1.5 mA comes from the Control Board and firmware, not the adapter or the matrix, and the radio adds little. In the USB log no connection-parameter update followed in 4 minutes (ZMK logs `interval N latency N` on one), so the host kept its own parameters. A tuned ZMK keyboard idles at tens to a few hundred µA, so about 1 mA is to be found. Next: a trace with a power profiler. Predictions, by what the trace would show:
  - **Flat floor between radio events:** something stays on. Candidates: the USB controller and its regulator (`vregusb`, enabled in the board DT) without VBUS, the 32 MHz crystal held by a driver, or the CPU not sleeping. Check: a plain Zephyr BLE sample (`peripheral_hids`) on the same board. At ~1.5 mA too, it's the board or Zephyr; lower, it's ZMK or these modules.
  - **Spikes every 4 s:** calibration of the RC 32 kHz clock (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC_CALIBRATION`). The board DT sets up the 32 kHz crystal (`lfxo`); if it's fitted, use it, which also narrows the radio's receive windows.
  - **Radio events every 7.5-30 ms while connected:** the host ignores ZMK's request for latency 30; request again or accept other parameters.
  - **Advertising:** ZMK advertises every 30-60 ms for as long as no host is connected. Slow down after about 30 s (ZMK patch).
  - Then `CONFIG_ZMK_SLEEP`: System OFF after some minutes untouched on Bluetooth too, waking on a key (rows and INT as wake sources).
- [ ] Backlight on a real keyboard (`rethink_t430`): current on BUCK1 (50 mA works, ~106 mA doesn't), `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY` (capped at 10 % until then), PWM frequency
- [ ] Control Board LEDs: the blue LED sometimes misbehaves (`CONFIG_RETHINK_CONNECTED_LED`, to be described); PWM brightness control (RGB especially); RGB as charging indicator; battery level, charging and backlight-level animations on the white LEDs
- [ ] Shields: `rethink_t480`, `rethink_l14`
- [ ] Self-test after programming, for user-built boards: check the adapter (I2C) and other parts at boot and report a fault without a serial log, e.g. a sequence of red blinks
- [ ] ZMK Studio: change the keymap at runtime without rebuilding (swap Fn and LCtrl, Fn-layer and unused keys) and pick ISO or ANSI, which Studio offers when a shield defines both physical layouts. Needs key positions in each physical layout, an ANSI transform per keyboard, a `&studio_unlock` key (ThinkVantage on the T420), spare layers, and Studio over USB (a CDC ACM port on the device_next stack, which the log and shell snippets use now) and Bluetooth

## Software stack

All pinned by commit in [config/west.yml](config/west.yml):

| Project | Source | Why |
| --- | --- | --- |
| ZMK | [zmkfirmware/zmk#3387](https://github.com/zmkfirmware/zmk/pull/3387) (`petejohanson/zmk`) | Draft port of ZMK to Zephyr 4.4; matrix scanning uses Zephyr's input API |
| Zephyr | [FrameworkComputer/zephyr#2](https://github.com/FrameworkComputer/zephyr/pull/2) | Zephyr 4.5 with the `framework_wireless_tp_kb` board |
| framework-cb | [Ziemniaczenka/zmk-config-fw-cb](https://github.com/Ziemniaczenka/zmk-config-fw-cb), fork of [Shaedil/zmk-config-fw-cb](https://github.com/Shaedil/zmk-config-fw-cb) | ZMK board variant `framework_wireless_tp_kb/nrf54lm20a/cpuapp/zmk`: USB HID on Zephyr's `device_next` stack, USB/Bluetooth mode switch, charger, fuel gauge, watchdog, MCUboot signing, and the patches that let ZMK #3387 build on Zephyr 4.5 |

The patches from framework-cb (its README says what each one does) and from `patches/` here, if any, are applied to the ZMK and Zephyr checkouts at build time. Nothing is edited by hand.

## Requirements (Windows)

- Docker Desktop
- For flashing over USB: `mcumgr`, installed with [Go](https://go.dev/dl/):
  ```powershell
  go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
  ```
- For the matrix scanner only: Python 3 with pyserial (`python -m pip install pyserial`)

No Zephyr SDK or west on the host. Builds run in the image from [docker/Dockerfile](docker/Dockerfile) (ZMK's `zmk-build-arm:4.4-branch` with Zephyr SDK 1.0.1, plus Zephyr 4.5's Python requirements and `imgtool`).

## Building

```powershell
.\build.ps1                                   # default shield: rethink_bare_tester
.\build.ps1 -Snippet rethink-usb-log          # with logs on a USB serial port
.\build.ps1 -Snippet framework-cb-uart-log    # with logs on the header UART
.\build.ps1 -Shield <shield> -Pristine        # clean build directory
.\build.ps1 -Update                           # west update after editing config/west.yml
.\build.ps1 -Menuconfig                       # menuconfig of the last build
.\build.ps1 -Shell                            # bash inside the build container
.\build.ps1 -RebuildImage                     # rebuild the Docker image
.\build.ps1 -FrameworkCb ..\zmk-config-fw-cb  # build with a local framework-cb checkout
.\build.ps1 -CmakeArgs "-DCONFIG_ZMK_SLEEP=y" # extra CMake arguments
.\build.ps1 -Snippet rethink-usb-log -Flash   # build, then flash (see Flashing)
```

The output is `out\<shield>[-<snippet>].signed.bin`, signed for Framework's MCUboot.

How it works:

- `build.ps1` builds the Docker image if needed (it is tagged by a hash of `docker/`) and runs [scripts/build.sh](scripts/build.sh) in it.
- The west workspace (ZMK, Zephyr, modules, build directories, about 4 GB) lives in the Docker volume `rethink-zmk-ws`. The first build fills it. This repository is mounted at `/repo`.
- `scripts/build.sh` runs `west update` whenever `config/west.yml` has changed, applies the patches, and calls `west build`.

To remove the workspace: `docker volume rm rethink-zmk-ws`.

To browse the ZMK and Zephyr sources from Windows (for reading: `scripts/build.sh` resets the ZMK and Zephyr checkouts whenever it re-applies the patches), open the volume through Docker Desktop's WSL share, e.g. with *File > Add Folder to Workspace* in VS Code:

```text
\\wsl.localhost\docker-desktop\mnt\docker-desktop-disk\data\docker\volumes\rethink-zmk-ws\_data
```

It contains `zmk/`, `zephyr/`, `modules/` (framework-cb, HALs) and `build/<artifact>/` (generated `zephyr.dts`, `.config`, `compile_commands.json`). The path is the one used by current Docker Desktop releases with the WSL 2 backend. The dev container below shows the same files at `/zmk-ws`.

### Dev container

[.devcontainer/devcontainer.json](.devcontainer/devcontainer.json) opens this repository in the same image, with the same `rethink-zmk-ws` volume at `/zmk-ws`. In VS Code: *Dev Containers: Reopen in Container*, then:

```bash
scripts/build.sh                                  # SHIELD, SNIPPET, BOARD, CMAKE_ARGS from env
SNIPPET=rethink-usb-log scripts/build.sh --pristine
```

ZMK and Zephyr sources are under `/zmk-ws` inside the container.

### GitHub Actions

[.github/workflows/build.yml](.github/workflows/build.yml) builds the same Docker image and runs the same `scripts/build.sh` for each entry in [build.yaml](build.yaml). The signed images are uploaded as artifacts.

## Flashing

The board ships with MCUboot with USB serial recovery:

1. Hold the pairing button while plugging in USB. With a battery connected, plugging in does not reset the board: instead hold the pairing button for 10 s while the firmware runs and keep holding it. The firmware has the nPM1300 power-cycle the board, because the factory MCUboot ignores the button after a software reset (fixed in Framework's `daisy` branch on 2026-10-16, commit `c4a29622f1`, but not in the bootloader shipped on the boards). The green LED lights and a USB serial port appears (`USB\VID_32AC&PID_0034`).
2. Flash:

```powershell
.\flash.ps1                                                     # usage and the images in out\
.\flash.ps1 -Shield rethink_bare_tester                         # out\rethink_bare_tester.signed.bin
.\flash.ps1 -Shield rethink_bare_tester -Snippet rethink-usb-log
.\flash.ps1 -File path\to\image.signed.bin
.\flash.ps1 -Shield rethink_bare_tester -Port COM7              # explicit port
.\build.ps1 -Snippet rethink-usb-log -Flash                     # build, then flash that image
.\flash.ps1 -Reset                                              # leave the bootloader without flashing
```

The factory MCUboot can only be left by a reset command over the port (`-Reset`) or by removing power: the pairing button and the mode switch do nothing in recovery. Framework's `daisy` branch can leave recovery when a switch moves (`BOOT_SERIAL_EXIT_GPIO`, 2026-10-16), but the keyboard board doesn't enable it yet.

`flash.ps1` sends 1024-byte SMP packets (`-Mtu 1024`, about 4 KB/s). mcumgr's default packet size gives about 1 KB/s; use `-Mtu ''` to fall back to it.

The port is found automatically by its USB ID. If no board is in recovery mode, or more than one is, the script lists the serial ports and exits; pass `-Port` in that case.

MCUboot only starts reading from the port after the host sets DTR/RTS, which `mcumgr` doesn't do on Windows. `flash.ps1` opens the port once with DTR/RTS set before uploading. If you call `mcumgr` directly and it hangs at 0%, this is the cause.

Images are signed with MCUboot's public development key (`root-ed25519.pem`), which Framework's bootloader accepts. Anyone can sign images with it.

## Logging

| Snippet | Output | Notes |
| --- | --- | --- |
| `rethink-usb-log` | Second USB serial port next to the keyboard | Disables ZMK Studio (same CDC ACM port). Messages from before the port is opened may be lost. |
| `rethink-usb-shell` | Same port, as a Zephyr shell with the log | As `rethink-usb-log`, plus the `adapter` bring-up commands and the `i2c` and `gpio` shells. See [Adapter bring-up shell](#adapter-bring-up-shell). |
| `framework-cb-uart-log` | Header UART: TX P2.02, RX P2.00, 115200 baud | Keeps the UART receiver on (battery drain). |

All of them log at ZMK debug level.

## Hardware options

[config/rethink.conf](config/rethink.conf) describes how the adapter and Control Board are assembled: jumpers, fitted parts, and where the backlight power comes from. ZMK applies it to every shield whose name starts with `rethink_` (it also looks for `<shield prefix>.conf` in `config/`), locally and in CI. The defaults match a stock adapter and Control Board; change the lines that differ on yours.

| Option | Default | Hardware | Effect |
| --- | --- | --- | --- |
| `CONFIG_RETHINK_EXPANDER_SUPPLY_LED_3V3` / `_TP_3V3` | LED_3V3 | Adapter JP10 | With TP_3V3, P3.12 is switched on before the expander is probed and stays on |
| `CONFIG_RETHINK_PUMP_EN_TIED` | n | Adapter JP1 closed | PUMP_EN (P0_0) left as an input |
| `CONFIG_RETHINK_BOOST_FITTED` | y | U4 (n on Mezz_no_BL, FFC1) | n: the backlight is never powered |
| `CONFIG_RETHINK_BOOST_EN_TIED` | n | Adapter JP2 closed | BOOST_EN (P0_1) left as an input |
| `CONFIG_RETHINK_BACKLIGHT_SUPPLY_BUCK1` / `_VBAT` / `_VSYS` | BUCK1 | Control Board R62 / R59 (VBAT) / VSYS on the next revision | BUCK1: the regulator is switched with the backlight |
| `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY` | 10 (range 1-10) | | Duty cycle at 100 % brightness, to stay within the supply's current. Capped at 10 % until real keyboards are measured |
| `CONFIG_RETHINK_TP1_SUPPLY_5V` / `_3V3` | 5V | Adapter JP6 (+ JP4, JP5) | Read by the pointing stick driver |
| `CONFIG_RETHINK_TP2_SUPPLY_3V3` / `_5V` | 3V3 | Adapter JP7 (+ JP8, JP9) | Read by the pointing stick driver |
| `CONFIG_RETHINK_TP2_RESET_PULL_UP` / `_DOWN` | pull-up | Adapter R22 / R23 | Read by the pointing stick driver |
| `CONFIG_RETHINK_MCUBOOT_NEEDS_POWER_CYCLE` | y | Factory MCUboot (Framework's daisy branch before 2026-10-16) | The recovery hold power-cycles through the nPM1300; n: soft reset |

The expander pin map, the jumpers and which keyboard uses which pin are in the hardware repository: [docs/pinouts/io_expander.md](https://github.com/Ziemniaczenka/ReThink_Keyboard/blob/main/docs/pinouts/io_expander.md).

What a keyboard has (status LEDs, KBD_BL_DTCT pin, backlight) is set by its shield, not here: shields list their LEDs on `&rethink_adapter` and include `rethink_backlight.dtsi` only when the keyboard is backlit.

## Shields

### `rethink_bare_tester`

Tests a bare Control Board; no adapter needed.

- **Matrix:** 8x15 on the header pins the adapter uses: KSI0-7 (P0.00-P0.07) by KSO0-14 (P1.00, P1.01, P1.18, P1.19, P1.04-P1.14). Columns are driven low and rows pulled up, as on the ThinkPad matrices. Short a KSI pin to a KSO pin to press a key. Each position sends a different key (row r, column c = position r*15 + c, see the keymap). KSO15/16 (P1.15/P1.16, TrackPoint) and KSO17 (P1.17, backlight PWM) are left out.
- **Pairing button** (P3.10, outside the matrix): tap for the next Bluetooth profile (acts 0.4 s after the tap, to tell it apart from a triple tap); hold for 3 s, then release, to clear the active profile. Both are ignored while the mode switch is on wired. Hold for 10 s to reboot into MCUboot recovery (keep holding until the green LED lights); holding it while plugging in USB also enters recovery.
- **USB/Bluetooth switch** (P3.05, outside the matrix): Bluetooth sends over Bluetooth, falling back to USB when no host is connected. Wired turns Bluetooth off and sends over USB.
- **White LEDs 1-4:** show the active Bluetooth profile (4 profiles) for 3 s after boot and after each change, blinking while the profile is unpaired.
- **RGB LED:** blue for 3 s when a host connects (USB, or the active Bluetooth profile) and after switching to a connected profile.
- **Power off in wired mode:** with the switch on wired and no USB power (running from the battery), the board switches all LEDs off and enters System OFF after 3 s. Only plugging in USB or moving the switch to Bluetooth wakes it; the pairing button and the keys do nothing while it is off.
- **P3.11** (Framework's backlight PWM): disabled. On ReThink it enables the adapter's backlight boost converter.
- **Name:** shows up as "ReThink Tester" over USB and Bluetooth.

### `rethink_hw_tester`

Tests the ReThink adapter. Everything from `rethink_bare_tester` except its matrix, plus the following:

- **Matrix:** the adapter's whole matrix (`rethink_adapter_matrix.dtsi`, driver `rethink,matrix`, with every position in `rethink_adapter_full_layout.dtsi`): SENSE0-7 by DRV0-14 on the Control Board and DRV15-17 on the expander, plus HOTKEY and PWRSWITCH as direct keys in row 8. Row r, column c = position r*18 + c; HOTKEY and PWRSWITCH are 144 and 145. Columns are open-drain and ghost keys are blocked, as diodeless ThinkPad matrices need. Every position sends a different key (see the keymap), and the log shows `position: N` for each press. Without a keyboard, short a SENSE pin to a DRV pin, e.g. on the Mezzanine connector SENSE0 (pin 5) to DRV15 (pin 30) for position 15, or HOTKEY (pin 1) to GND (pin 34) for 144. Without the adapter, the Control Board part still works on the header.
- **Adapter detection:** probes the PCAL6416A at 0x20 on `i2c22` (first switching on TP_3V3 if the expander runs from it, JP10 2-3). If it doesn't answer, it logs `ReThink adapter not found` and carries on without it.
- **Status LEDs** *(needs adapter)*: all five, as on an FFC2 keyboard with a numpad. At boot, Fn lock/power, mute, mic mute, Caps Lock and Num Lock light one after another, then all together, then go off. After that, Caps Lock and Num Lock follow the host's lock state. A lit LED pulls its expander pin low; an unlit one rests on the expander's pull-up.
- **Idle expander pins** *(needs adapter)*: every pin nothing drives rests on the expander's pull-up (`idle-gpios` in `rethink_adapter.dtsi`), also in System OFF, since a floating input draws up to 80 µA from LED_3V3. TP1/TP2_RST rest on the pull-down instead: R14/R22 go to the pointing stick supplies, which float while off.
- **Backlight power chain:** nPM1300 BUCK1 (if it is the VLED_BL source) → P3.11 (VLED_BL to the adapter) → BOOST_EN (expander P0_1) → PWM 4096 Hz on P1.17, duty cycle = brightness × `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY`. The chain is on only while the brightness is above 0.
- **Backlight interlock:** KBD_BL_DTCT (expander P1_7) is read with the pull-up enabled at boot and again each time the backlight switches on; a backlit keyboard pulls it low. The pull-up then stays on unless the pin is low. Without a detected backlit keyboard (or without the adapter), the backlight stays locked and logs `Backlight locked`. Triple-press the pairing button to unlock it until reboot.
- **Keymap:** row 0 (KSI0) × KSO0..4 are backlight toggle, down (20 % steps), up, Caps Lock and Num Lock. The backlight starts on at 40 %.
- **Name:** "ReThink HW Test" (BLE names are limited to 16 characters).

Test on a bare Control Board (build with `-Snippet rethink-usb-log` to watch the log):

1. Boot. The log shows `ReThink adapter not found`, `Ready: VLED_BL from BUCK1, max duty 10%`, then `Backlight locked: backlit keyboard unknown`.
2. Triple-press the pairing button. The log shows `Backlight unlocked`, `Adapter missing: BOOST_EN not driven` and `Power chain on`. VLED_BL is now on the 6-pin connector, and P1.17 (KSO17 on the header) carries 40 % × 10 % = 4 % PWM, which reads about 0.13 V on a multimeter.
3. Short KSI0 to KSO1 (down) or KSO2 (up) to change the duty cycle; KSO0 toggles the backlight. Off logs `Power chain off`, and VLED_BL and PWM drop to 0.

#### Adapter bring-up shell

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

### `rethink_scanner`

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

A keyboard shield includes `rethink_adapter_matrix.dtsi` without the full layout and defines its own physical layout with that transform, as `rethink_l420` does: ZMK sizes the keymap for the largest physical layout enabled. The matrix drives and reads only the columns and direct keys that an enabled transform maps, so a keyboard without keys on DRV16/17 or without HOTKEY/PWRSWITCH saves those I2C transfers on every scan.

### `rethink_l420`

ThinkPad L420 keyboard on FFC1 (30-pin matrix cable in J4). Mapped with `rethink_scanner` on two Norwegian L420 keyboards, and in use for typing.

- **Everything from `rethink_bare_tester` except its matrix:** pairing button, USB/Bluetooth switch, profile LEDs, RGB LED, power off in wired mode.
- **Matrix:** the adapter's matrix with the L420's 85 keys (ISO) as the physical layout `l420_layout`. Keys at other positions are ignored.
- **Keymap:** each key sends the standard key code of its position, so the host's keyboard layout (e.g. Norwegian, Swedish, Danish) gives the legends. Fn (bottom left) does what the keycaps show, where a standard key code exists:

  | Fn + | Sends |
  | --- | --- |
  | Esc, F1 | Break, SysRq (Alt+PrtSc) |
  | F2, F4, F7 | Lock (Win+L), sleep, display (Win+P) |
  | F9, F10, F11 | Scroll Lock, NmLk (embedded keypad on/off), Pause |
  | Insert, Delete, Home | PrtSc, brightness down, brightness up |
  | PgUp, PgDn | Browser back, forward |
  | Up, Down, Left, Right | Stop, play/pause, previous, next |

  Battery (F3), wireless (F5), camera (F6), pointing device (F8), hibernate (F12) and zoom (Space) have no standard key code. The embedded keypad sends the digits on 7-9, U-O, J-L and M, and `/ * - +` on 0, P, `;` and `/` (UK ISO legends), independent of the host's Num Lock.
- **Not on this keyboard:** backlight (BUCK1 stays off from boot) and status LEDs; the expander's LED pins rest on their pull-ups.
- **TrackPoint** (8-pin cable in J5; its buttons go to the stick, not the matrix): a mouse, see [Pointing stick](#pointing-stick-in-progress).
- **Name:** "ReThink L420".

Build with `-Snippet rethink-usb-log` to see `position: N` for each key in the log.

### `rethink_t420`

ThinkPad T420 keyboard on the Mezzanine connector (2x20 pins). Mapped with `rethink_scanner` on a Norwegian T420 keyboard.

- **Everything from `rethink_bare_tester` except its matrix**, as `rethink_l420`.
- **Matrix:** the T420's 96 keys (ISO) as the physical layout `t420_layout`: the keyboard, the buttons above it, Fn on HOTKEY and the power button on PWRSWITCH.
- **Buttons above the keyboard:** mute, volume down, volume up, mic mute (sends F20, which Linux treats as mic mute). ThinkVantage and power send nothing. The keys left and right of Up are browser back and forward.
- **Keymap:** standard key codes by position, as `rethink_l420`. Fn does what the keycaps show:

  | Fn + | Sends |
  | --- | --- |
  | PrtSc, ScrLk, Pause | SysRq (Alt+PrtSc), NmLk (embedded keypad on/off), Break |
  | F2, F4, F7 | Lock (Win+L), sleep, display (Win+P) |
  | Home, End | Brightness up, down |
  | Up, Down, Left, Right | Stop, play/pause, previous, next |

  Battery (F3), wireless (F5), camera (F6), pointing device (F8), hibernate (F12), ThinkLight (PgUp) and zoom (Space) have no standard key code. The embedded keypad is the same as on `rethink_l420`.
- **Caps Lock LED** (Mezzanine 21) follows the host. The power, mute and mic mute LEDs rest on their pull-ups.
- **Not on this keyboard:** backlight. KBDID0-2 (Mezzanine 25, 27, 29) meet the adapter's backlight PWM, GND and BL_5V, so keep JP3 open; P1.17 stays unused.
- **TrackPoint** (Mezzanine 37-40): a mouse, see [Pointing stick](#pointing-stick-in-progress).
- **Name:** "ReThink T420".

### Pointing stick (in progress)

`rethink_trackpoint.dtsi` adds the TP1 stick (Mezzanine 37-40, FFC1 J5) to a keyboard shield; `rethink_t420` and `rethink_l420` include it. The driver (`rethink,trackpoint`, `src/trackpoint.c`) starts the stick after boot and reports it to ZMK as a mouse (`CONFIG_ZMK_POINTING`, on for shields with the stick), over USB and Bluetooth. Without an answer from a stick (none connected) it powers it off again. Wired power-off switches it off before System OFF. Sensitivity and the stick's own power saving come later; it stays powered while the keyboard runs.

- **Power:** TP_3V3 (P3.12), then the charge pump for a 5 V stick (`CONFIG_RETHINK_TP1_SUPPLY_5V`, JP6 1-2), all with the reset held. The reset (TP1_RST, active high, R14 pull-up to the stick's supply) is released 600 ms later, the power-on reset time of the TrackPoint specification.
- **PS/2 link:** FW_CLK (P1.16) and FW_DAT (P1.15), through the PCA9306 for a 5 V stick. The stick clocks every bit (a T420 stick at about 14.7 kHz); an interrupt on each falling clock edge reads DATA or sets the next bit.
- **Start:** self-test result `aa 00`, TrackPoint ID (`e1`, logged), streaming on (`f4`).
- **Pointing:** 3-byte packets every 10 ms while the stick moves, assembled in the interrupt. A pause, or a byte lost to an error, starts the next packet afresh. Movement goes out as relative X/Y, the packet's left, right and middle buttons as mouse buttons 1-3. A T420 keyboard has no buttons of its own (they are on the palmrest); pressing the stick down clicks if its press-to-select is on.
- **Bluetooth:** the keyboard now also describes a mouse. If the pointer doesn't move over Bluetooth, remove the keyboard on the computer and pair it again.

Build a keyboard with the shell, e.g. `.\build.ps1 -Shield rethink_t420 -Snippet rethink-usb-shell`:

| Command | Does |
| --- | --- |
| `tp on` | Powers the stick (or resets it with `ff`) and starts pointing |
| `tp off` | Powers it off |
| `tp send <hex> [hex...]` | Sends bytes, showing each acknowledge (`fa`), then the reply. Pauses pointing and turns streaming off first (`f5`) |
| `tp read [s]` | Shows what the stick sends, one line per packet (default 5 s). Pauses pointing like `send` |
| `tp stat [clear]` | Bytes and errors both ways, packets reported, the stick's clock period |

Bytes to try: `e1` asks for the TrackPoint ID (`fa`, then the maker: 01 IBM/Lenovo, 02 ALPS, 03 Elan, 04 NXP, and its firmware), `f2` the PS/2 device ID (`fa 00`), `ff` resets (`fa aa 00`), `f4` starts streaming (3-byte packets while the stick moves, see `tp read`), `f5` stops it.

Measured on a T420 stick (ID 01, firmware 0e): no bit errors over USB; with Bluetooth connected, one byte in about 1500 broken off (an interrupt delayed by more than a clock period), which costs that one packet.

## Repository layout

```text
.
├── build.ps1 / flash.ps1        Host scripts (Windows)
├── scripts/build.sh             Build steps, shared by build.ps1, dev container and CI
├── scripts/matrix_scan.py       Maps a keyboard's matrix (rethink_scanner)
├── docker/                      Build image
├── .devcontainer/               VS Code dev container
├── .github/workflows/build.yml  CI, matrix in build.yaml
├── config/west.yml              West manifest (pinned)
├── config/rethink.conf          Hardware options (jumpers, fitted parts)
├── zephyr/module.yml            This repository as a Zephyr module
├── CMakeLists.txt, Kconfig      Module sources and options (CONFIG_RETHINK_*)
├── src/                         Pairing button, profile and blue LEDs, adapter control, expander
│                                registers, matrix, bring-up commands, backlight,
│                                pointing stick, wired power-off
├── include/rethink/             Public API (adapter.h, expander.h, backlight.h, trackpoint.h)
├── dts/bindings/                rethink,adapter-ctrl, rethink,pcal6416a, rethink,matrix,
│                                rethink,backlight and rethink,trackpoint
├── boards/shields/              Shields; rethink_common/ holds the shared dtsi files
│                                (Control Board, adapter, its matrix and full layout, pointing stick,
│                                backlight, header matrix) and the shields' Kconfig defaults
└── snippets/                    rethink-usb-log (USB CDC ACM logging),
                                 rethink-usb-shell (shell and log on the same port)
```

## Kconfig options

| Option | Default | Description |
| --- | --- | --- |
| `CONFIG_RETHINK_PAIRING_BUTTON` | y | Pairing button: tap = next profile, hold = clear profile, triple tap = unlock backlight |
| `CONFIG_RETHINK_PAIRING_BUTTON_HOLD_MS` | 3000 | Hold time to clear (on release) |
| `CONFIG_RETHINK_PAIRING_BUTTON_RECOVERY_MS` | 10000 | Hold time to reboot into the bootloader, 0 = off |
| `CONFIG_RETHINK_WIRED_OFF` | y | System OFF in wired mode while USB is unplugged |
| `CONFIG_RETHINK_WIRED_OFF_DELAY_MS` | 3000 | Delay before powering off |
| `CONFIG_RETHINK_PAIRING_BUTTON_TAP_WINDOW_MS` | 400 | Maximum gap between taps |
| `CONFIG_RETHINK_PROFILE_LEDS` | y | Active profile on the white LEDs |
| `CONFIG_RETHINK_PROFILE_LEDS_TIMEOUT_MS` | 3000 | How long the LED stays on |
| `CONFIG_RETHINK_CONNECTED_LED` | y | Blue LED after connecting (replaces framework-cb's `CONFIG_FRAMEWORK_CB_STATUS_LED`, which the shields set to n) |
| `CONFIG_RETHINK_CONNECTED_LED_TIMEOUT_MS` | 3000 | How long the blue LED stays on |
| `CONFIG_RETHINK_ADAPTER` | if `rethink,adapter-ctrl` in DT | Adapter bring-up, status LEDs, BOOST_EN, KBD_BL_DTCT |
| `CONFIG_RETHINK_ADAPTER_LED_SELFTEST` | n | Light each status LED in turn at boot |
| `CONFIG_RETHINK_ADAPTER_TEST` | n | `adapter` shell commands for bring-up (set by the `rethink-usb-shell` snippet) |
| `CONFIG_RETHINK_MATRIX` | if `rethink,matrix` in DT | Matrix with expander columns, direct keys and ghost-key blocking |
| `CONFIG_RETHINK_BACKLIGHT` | if `rethink,backlight` in DT | Backlight power chain, PWM and interlock (use with `CONFIG_ZMK_BACKLIGHT`) |
| `CONFIG_RETHINK_TRACKPOINT` | if `rethink,trackpoint` in DT | Pointing stick power, reset, PS/2 link and pointing |
| `CONFIG_RETHINK_TRACKPOINT_SHELL` | y with a shell | `tp` shell commands |

Hardware options: see [Hardware options](#hardware-options).

framework-cb adds `CONFIG_FRAMEWORK_CB_MODE_SWITCH`, `CONFIG_FRAMEWORK_CB_STATUS_LED`, `CONFIG_FRAMEWORK_CB_FUEL_GAUGE` and `CONFIG_FRAMEWORK_CB_WATCHDOG`. See its [README](https://github.com/Ziemniaczenka/zmk-config-fw-cb).

### Changing framework-cb

framework-cb is pinned to a commit of [the fork](https://github.com/Ziemniaczenka/zmk-config-fw-cb). To try changes before pushing them, build with a local checkout mounted over the pinned one:

```powershell
.\build.ps1 -Shield rethink_hw_tester -Snippet rethink-usb-log -FrameworkCb ..\zmk-config-fw-cb
```

`west update` is refused while it is mounted, since it would reset the checkout. Once the change is pushed to the fork, update its revision in `config/west.yml`.

## Updating pinned revisions

1. Change the revision in `config/west.yml`. When changing the Zephyr revision, also update `ZEPHYR_REV` in `docker/Dockerfile`.
2. Run `.\build.ps1 -Pristine`. The workspace updates automatically. If a patch no longer applies, the build stops with an error.

## License

ReThink ZMK config by Ziemniaczenka is licensed under the [MIT License](LICENSE). The ReThink hardware (adapter PCB, case, documentation) is in the [ReThink_Keyboard](https://github.com/Ziemniaczenka/ReThink_Keyboard) repository under CC BY-SA 4.0.

Firmware built from this repository also includes code that is not covered by the above license. These projects are not stored here; west downloads them at build time:

| Component | License |
| --- | --- |
| [ZMK](https://github.com/zmkfirmware/zmk) | MIT |
| [Zephyr](https://github.com/FrameworkComputer/zephyr), including the `framework_wireless_tp_kb` board by Framework Computer Inc | Apache-2.0 |
| [framework-cb](https://github.com/Ziemniaczenka/zmk-config-fw-cb) (by Shaedil) and the patches it applies to ZMK and Zephyr | MIT |
| `mcuboot-dev-ed25519.pem` in framework-cb, MCUboot's public development signing key from [mcu-tools/mcuboot](https://github.com/mcu-tools/mcuboot) | Apache-2.0 |
| nRF Fuel Gauge library and example battery model in framework-cb (`third_party/nrf_fuel_gauge/`) | Nordic 5-Clause; may only be used with Nordic Semiconductor devices |
| Zephyr modules such as `hal_nordic`, `mbedtls`, `cmsis` | See each module |

## Disclaimers

> Framework Control Board was provided pre-release by Framework Computer Inc. through Control Board Developer Program.

> ThinkPad and TrackPoint are registered trademarks of Lenovo. ReThink is an independent open-source project and is not affiliated with, endorsed by, or certified by Lenovo Group Limited.