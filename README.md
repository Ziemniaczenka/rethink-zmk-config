# ReThink ZMK config

ZMK firmware for the [ReThink Keyboard](https://github.com/Ziemniaczenka/ReThink_Keyboard): ThinkPad keyboard modules driven by the Framework Wireless Touchpad Keyboard Control Board (nRF54LM20A + nPM1300) through the ReThink adapter.

> Work in progress. So far only the tester shields exist: `rethink_bare_tester` and `rethink_hw_tester`.

## Roadmap

- [x] Container build (local and CI), flashing over USB, bare Control Board tester
- [x] Adapter control, backlight power chain and interlock, hardware options (`config/rethink.conf`)
- [ ] Power: off in wired mode without USB, fuel gauge fixes (framework-cb)
- [ ] Control Board LEDs: RGB as charging indicator, battery level and charging animation on the white LEDs
- [ ] Key matrix: open-drain columns for diodeless ThinkPad matrices, ghost-key blocking, expander columns DRV15-17 with fewer I2C transfers (patched PCAL64xxA driver), HOTKEY/PWRSWITCH through the expander interrupt, matrix tester, transform generator
- [ ] TrackPoint: power and reset sequencing, PS/2 driver
- [ ] Shields: `rethink_t420`, `rethink_t430`, `rethink_t480`, `rethink_l14`
- [ ] Backlight on the adapter: current (`CONFIG_RETHINK_BACKLIGHT_MAX_DUTY`), PWM frequency
- [ ] Fuel gauge model of the real battery

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

No Zephyr SDK, Python or west on the host. Builds run in the image from [docker/Dockerfile](docker/Dockerfile) (ZMK's `zmk-build-arm:4.4-branch` with Zephyr SDK 1.0.1, plus Zephyr 4.5's Python requirements and `imgtool`).

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
| `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY` | 60 (BUCK1), 100 (VBAT, VSYS) | | Duty cycle at 100 % brightness, to stay within the supply's current. Not measured yet. |
| `CONFIG_RETHINK_TP1_SUPPLY_5V` / `_3V3` | 5V | Adapter JP6 (+ JP4, JP5) | For the TrackPoint support (not implemented yet) |
| `CONFIG_RETHINK_TP2_SUPPLY_3V3` / `_5V` | 3V3 | Adapter JP7 (+ JP8, JP9) | For the TrackPoint support |
| `CONFIG_RETHINK_TP2_RESET_PULL_UP` / `_DOWN` | pull-up | Adapter R22 / R23 | For the TrackPoint support |
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
- **RGB LED:** blue while connected to a host.
- **Power off in wired mode:** with the switch on wired and no USB power (running from the battery), the board switches all LEDs off and enters System OFF after 3 s. Only plugging in USB or moving the switch to Bluetooth wakes it; the pairing button and the keys do nothing while it is off.
- **P3.11** (Framework's backlight PWM): disabled. On ReThink it enables the adapter's backlight boost converter.
- **Name:** shows up as "ReThink Tester" over USB and Bluetooth.

### `rethink_hw_tester`

Tests the ReThink adapter. Everything from `rethink_bare_tester`, plus the following:

- **Adapter detection:** probes the PCAL6416A at 0x20 on `i2c22` (first switching on TP_3V3 if the expander runs from it, JP10 2-3). If it doesn't answer, it logs `ReThink adapter not found` and carries on without it.
- **Status LEDs** *(needs adapter)*: all five, as on an FFC2 keyboard with a numpad. At boot, Fn lock/power, mute, mic mute, Caps Lock and Num Lock light one after another, then all together, then go off. After that, Caps Lock and Num Lock follow the host's lock state. A lit LED pulls its expander pin low; an unlit one rests on the expander's pull-up.
- **Idle expander pins** *(needs adapter)*: every pin nothing drives rests on the expander's pull-up (`idle-gpios` in `rethink_adapter.dtsi`), also in System OFF, since a floating input draws up to 80 µA from LED_3V3.
- **Backlight power chain:** nPM1300 BUCK1 (if it is the VLED_BL source) → P3.11 (VLED_BL to the adapter) → BOOST_EN (expander P0_1) → PWM 4096 Hz on P1.17, duty cycle = brightness × `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY`. The chain is on only while the brightness is above 0.
- **Backlight interlock:** at boot, KBD_BL_DTCT (expander P1_7) is read once with the pull-up enabled; a backlit keyboard pulls it low. The pull-up then stays on unless the pin is low. Without a detected backlit keyboard (or without the adapter), the backlight stays locked and logs `Backlight locked`. Triple-press the pairing button to unlock it until reboot.
- **Keymap:** row 0 (KSI0) × KSO0..4 are backlight toggle, down (20 % steps), up, Caps Lock and Num Lock. All other positions are as in `rethink_bare_tester`. The backlight starts on at 40 %.
- **Name:** "ReThink HW Test" (BLE names are limited to 16 characters).

Test on a bare Control Board (build with `-Snippet rethink-usb-log` to watch the log):

1. Boot. The log shows `ReThink adapter not found`, `Ready: VLED_BL from BUCK1, max duty 60%`, then `Backlight locked: backlit keyboard unknown`.
2. Triple-press the pairing button. The log shows `Backlight unlocked`, `Adapter missing: BOOST_EN not driven` and `Power chain on`. VLED_BL is now on the 6-pin connector, and P1.17 (KSO17 on the header) carries 40 % × 60 % = 24 % PWM, which reads about 0.8 V on a multimeter.
3. Short KSI0 to KSO1 (down) or KSO2 (up) to change the duty cycle; KSO0 toggles the backlight. Off logs `Power chain off`, and VLED_BL and PWM drop to 0.

#### Adapter bring-up shell

Build with `.\build.ps1 -Shield rethink_hw_tester -Snippet rethink-usb-shell` and open the board's USB serial port in a terminal program. The `adapter` commands switch the adapter's supplies one at a time, for an adapter without a keyboard. They bypass the backlight interlock, so don't use them with a keyboard connected. The full procedure, with expected readings, is [docs/adapter_bringup.md](https://github.com/Ziemniaczenka/ReThink_Keyboard/blob/main/docs/adapter_bringup.md) in the hardware repository.

| Command | Does |
| --- | --- |
| `adapter status` | TP_3V3, VLED_BL, INT and the level of every expander pin |
| `adapter tp on\|off` | TP_3V3 (P3.12), off from boot. `off` sets PUMP_EN low first |
| `adapter pump on\|off` | PUMP_EN: charge pump TP_5V. `on` is refused while TP_3V3 is off: the LM2775's EN must stay within 0.3 V of its supply |
| `adapter vled on\|off` | VLED_BL to the adapter: BUCK1 (if it is the source) and P3.11 |
| `adapter boost on\|off` | BOOST_EN: backlight boost BL_5V (needs VLED_BL) |
| `adapter pwm <0-100>` | Duty cycle on P1.17, limited to `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY` |
| `adapter pin <name> 0\|1\|z\|u` | An expander pin driven low or high, released, or released with its 100 kΩ pull-up. Names as in `adapter status` |
| `adapter in` | HOTKEY, PWRSWITCH and KBD_BL_DTCT, read with pull-ups |
| `adapter int [s]` | Waits for INT (P3.06) while an input is shorted to GND, then checks that reading the inputs releases it |
| `adapter shorts` | Drives the 40 lines to the keyboard connectors low one at a time and reports lines that follow (shorted) or are low at rest. The matrix is suspended meanwhile |

Zephyr's `i2c` and `gpio` shells are included too, e.g. `i2c scan i2c@c8000` (only 0x20 should answer).

## Repository layout

```text
.
├── build.ps1 / flash.ps1        Host scripts (Windows)
├── scripts/build.sh             Build steps, shared by build.ps1, dev container and CI
├── docker/                      Build image
├── .devcontainer/               VS Code dev container
├── .github/workflows/build.yml  CI, matrix in build.yaml
├── config/west.yml              West manifest (pinned)
├── config/rethink.conf          Hardware options (jumpers, fitted parts)
├── zephyr/module.yml            This repository as a Zephyr module
├── CMakeLists.txt, Kconfig      Module sources and options (CONFIG_RETHINK_*)
├── src/                         Pairing button, profile LEDs, adapter control and its
│                                bring-up commands, backlight, wired power-off
├── include/rethink/             Public API (adapter.h, backlight.h)
├── dts/bindings/                rethink,adapter-ctrl and rethink,backlight
├── boards/shields/              Shields; rethink_common/ holds the shared dtsi files
│                                (Control Board, adapter, backlight, header matrix)
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
| `CONFIG_RETHINK_ADAPTER` | if `rethink,adapter-ctrl` in DT | Adapter bring-up, status LEDs, BOOST_EN, KBD_BL_DTCT |
| `CONFIG_RETHINK_ADAPTER_LED_SELFTEST` | n | Light each status LED in turn at boot |
| `CONFIG_RETHINK_ADAPTER_TEST` | n | `adapter` shell commands for bring-up (set by the `rethink-usb-shell` snippet) |
| `CONFIG_RETHINK_BACKLIGHT` | if `rethink,backlight` in DT | Backlight power chain, PWM and interlock (use with `CONFIG_ZMK_BACKLIGHT`) |

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