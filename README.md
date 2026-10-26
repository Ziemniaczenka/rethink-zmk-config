# ReThink ZMK config

ZMK firmware for the [ReThink Keyboard](https://github.com/Ziemniaczenka/ReThink_Keyboard): ThinkPad keyboard modules driven by the Framework Wireless Touchpad Keyboard Control Board (nRF54LM20A + nPM1300) through the ReThink adapter.

> Work in progress. Only `rethink_bare_tester` exists so far.

## Software stack

All pinned by commit in [config/west.yml](config/west.yml):

| Project | Source | Why |
| --- | --- | --- |
| ZMK | [zmkfirmware/zmk#3387](https://github.com/zmkfirmware/zmk/pull/3387) (`petejohanson/zmk`) | Draft port of ZMK to Zephyr 4.4; matrix scanning uses Zephyr's input API |
| Zephyr | [FrameworkComputer/zephyr#2](https://github.com/FrameworkComputer/zephyr/pull/2) | Zephyr 4.5 with the `framework_wireless_tp_kb` board |
| framework-cb | [Ziemniaczenka/zmk-config-fw-cb](https://github.com/Ziemniaczenka/zmk-config-fw-cb), fork of [Shaedil/zmk-config-fw-cb](https://github.com/Shaedil/zmk-config-fw-cb) | ZMK board variant `framework_wireless_tp_kb/nrf54lm20a/cpuapp/zmk`: USB HID on Zephyr's `device_next` stack, USB/Bluetooth mode switch, charger, fuel gauge, watchdog, MCUboot signing, and the patches that let ZMK #3387 build on Zephyr 4.5 |

The patches from framework-cb (and from `patches/` here, if any) are applied to the ZMK and Zephyr checkouts at build time. Nothing is edited by hand.

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
.\build.ps1 -CmakeArgs "-DCONFIG_ZMK_SLEEP=y" # extra CMake arguments
.\build.ps1 -Snippet rethink-usb-log -Flash   # build, then flash (see Flashing)
```

The output is `out\<shield>[-<snippet>].signed.bin`, signed for Framework's MCUboot.

How it works:

- `build.ps1` builds the Docker image if needed (it is tagged by a hash of `docker/`) and runs [scripts/build.sh](scripts/build.sh) in it.
- The west workspace (ZMK, Zephyr, modules, build directories, about 4 GB) lives in the Docker volume `rethink-zmk-ws`. The first build fills it. This repository is mounted at `/repo`.
- `scripts/build.sh` runs `west update` whenever `config/west.yml` has changed, applies the patches, and calls `west build`.

To remove the workspace: `docker volume rm rethink-zmk-ws`.

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

1. Hold the pairing button while plugging in USB. The green LED lights and a USB serial port appears (`USB\VID_32AC&PID_0034`).
2. Flash:

```powershell
.\flash.ps1                                                     # usage and the images in out\
.\flash.ps1 -Shield rethink_bare_tester                         # out\rethink_bare_tester.signed.bin
.\flash.ps1 -Shield rethink_bare_tester -Snippet rethink-usb-log
.\flash.ps1 -File path\to\image.signed.bin
.\flash.ps1 -Shield rethink_bare_tester -Port COM7              # explicit port
.\build.ps1 -Snippet rethink-usb-log -Flash                     # build, then flash that image
```

The port is found automatically by its USB ID. If no board is in recovery mode, or more than one is, the script lists the serial ports and exits; pass `-Port` in that case.

MCUboot only starts reading from the port after the host sets DTR/RTS, which `mcumgr` doesn't do on Windows. `flash.ps1` opens the port once with DTR/RTS set before uploading. If you call `mcumgr` directly and it hangs at 0%, this is the cause.

Images are signed with MCUboot's public development key (`root-ed25519.pem`), which Framework's bootloader accepts. Anyone can sign images with it.

## Logging

| Snippet | Output | Notes |
| --- | --- | --- |
| `rethink-usb-log` | Second USB serial port next to the keyboard | Disables ZMK Studio (same CDC ACM port). Messages from before the port is opened may be lost. |
| `framework-cb-uart-log` | Header UART: TX P2.02, RX P2.00, 115200 baud | Keeps the UART receiver on (battery drain). |

Both log at ZMK debug level.

## Shields

### `rethink_bare_tester`

Tests a bare Control Board; no adapter needed.

- **Matrix:** 8x15 on the header pins the adapter uses: KSI0-7 (P0.00-P0.07) by KSO0-14 (P1.00, P1.01, P1.18, P1.19, P1.04-P1.14). Columns are driven low and rows pulled up, as on the ThinkPad matrices. Short a KSI pin to a KSO pin to press a key. Each position sends a different key (row r, column c = position r*15 + c, see the keymap). KSO15/16 (P1.15/P1.16, TrackPoint) and KSO17 (P1.17, backlight PWM) are left out.
- **Pairing button** (P3.10, outside the matrix): tap for the next Bluetooth profile, hold for 3 s to clear the active profile. Holding it while plugging in USB enters MCUboot recovery.
- **USB/Bluetooth switch** (P3.05, outside the matrix): Bluetooth sends over Bluetooth, falling back to USB when no host is connected. Wired turns Bluetooth off and sends over USB.
- **White LEDs 1-4:** show the active Bluetooth profile (4 profiles) for 3 s after boot and after each change, blinking while the profile is unpaired.
- **RGB LED:** blue while connected to a host.
- **P3.11** (Framework's backlight PWM): disabled. On ReThink it enables the adapter's backlight boost converter.
- **Name:** shows up as "ReThink Tester" over USB and Bluetooth.

## Repository layout

```text
.
├── build.ps1 / flash.ps1        Host scripts (Windows)
├── scripts/build.sh             Build steps, shared by build.ps1, dev container and CI
├── docker/                      Build image
├── .devcontainer/               VS Code dev container
├── .github/workflows/build.yml  CI, matrix in build.yaml
├── config/west.yml              West manifest (pinned)
├── zephyr/module.yml            This repository as a Zephyr module
├── CMakeLists.txt, Kconfig      Module sources and options (CONFIG_RETHINK_*)
├── src/                         pairing_button.c, profile_leds.c
├── boards/shields/              Shields
└── snippets/rethink-usb-log/    USB CDC ACM logging
```

## Kconfig options

| Option | Default | Description |
| --- | --- | --- |
| `CONFIG_RETHINK_PAIRING_BUTTON` | y | Pairing button: tap = next profile, hold = clear profile |
| `CONFIG_RETHINK_PAIRING_BUTTON_HOLD_MS` | 3000 | Hold time to clear |
| `CONFIG_RETHINK_PROFILE_LEDS` | y | Active profile on the white LEDs |
| `CONFIG_RETHINK_PROFILE_LEDS_TIMEOUT_MS` | 3000 | How long the LED stays on |

framework-cb adds `CONFIG_FRAMEWORK_CB_MODE_SWITCH`, `CONFIG_FRAMEWORK_CB_STATUS_LED`, `CONFIG_FRAMEWORK_CB_FUEL_GAUGE` and `CONFIG_FRAMEWORK_CB_WATCHDOG`. See its [README](https://github.com/Ziemniaczenka/zmk-config-fw-cb).

## Updating pinned revisions

1. Change the revision in `config/west.yml`. When changing the Zephyr revision, also update `ZEPHYR_REV` in `docker/Dockerfile`.
2. Run `.\build.ps1 -Pristine`. The workspace updates automatically. If a patch no longer applies, the build stops with an error.
