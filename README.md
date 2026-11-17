# ReThink ZMK config

ZMK firmware for the [ReThink Keyboard](https://github.com/Ziemniaczenka/ReThink_Keyboard): ThinkPad keyboard modules driven by the Framework Wireless Touchpad Keyboard Control Board (nRF54LM20A + nPM1300) through the ReThink adapter.

> Work in progress. Shields so far: the testers `rethink_bare_tester` and `rethink_hw_tester`, the matrix scanner `rethink_scanner`, and the keyboards `rethink_l420`, `rethink_t420`, `rethink_t480` and `rethink_e580`.

> [!IMPORTANT]
> Most of this repository is vibe-coded and may not follow best practices. I test it on my hardware before pushing commits but use it at your own risk.

## Documentation

| Document | Contents |
| --- | --- |
| [Shields](docs/shields.md) | The testers, the matrix scanner and the keyboards: matrix, keymap, LEDs, buttons; the adapter bring-up shell; mapping a keyboard |
| [Pointing stick](docs/pointing-stick.md) | Power, the PS/2 link, pointing, scrolling, report rate, settings, keys, `tp` commands, the stick's own settings |
| [Power](docs/power.md) | Power off (wired mode, low battery, unused, resets in a row), the nPM1300 on the Control Board, the stick's switch-on surge |
| [Configuration](docs/configuration.md) | Hardware options (`config/rethink.conf`: jumpers, fitted parts) and Kconfig options |
| [Diagnostics](docs/diagnostics.md) | `ble conn`, `ble log`, fatal error and stall records, `diag` |
| [Roadmap](docs/roadmap.md) | Status, measurements and what comes next |

## Software stack

All pinned by commit in [config/west.yml](config/west.yml):

| Project | Source | Why |
| --- | --- | --- |
| ZMK | [zmkfirmware/zmk#3387](https://github.com/zmkfirmware/zmk/pull/3387) (`petejohanson/zmk`) | Draft port of ZMK to Zephyr 4.4; matrix scanning uses Zephyr's input API |
| Zephyr | [FrameworkComputer/zephyr#2](https://github.com/FrameworkComputer/zephyr/pull/2) | Zephyr 4.5 with the `framework_wireless_tp_kb` board |
| framework-cb | [Ziemniaczenka/zmk-config-fw-cb](https://github.com/Ziemniaczenka/zmk-config-fw-cb), fork of [Shaedil/zmk-config-fw-cb](https://github.com/Shaedil/zmk-config-fw-cb) | ZMK board variant `framework_wireless_tp_kb/nrf54lm20a/cpuapp/zmk`: USB HID on Zephyr's `device_next` stack, USB/Bluetooth mode switch, charger, fuel gauge, watchdog, MCUboot signing, and the patches that let ZMK #3387 build on Zephyr 4.5 |

The patches from framework-cb (its README says what each one does) and from `patches/` here are applied to the ZMK and Zephyr checkouts at build time. Nothing is edited by hand. The patches here:

| Patch | What it does |
| --- | --- |
| `zmk/0001-pointing-smooth-scrolling-for-hosts-without-the-multiplier.patch` | Smooth scrolling (`CONFIG_ZMK_POINTING_SMOOTH_SCROLLING`): the input listener divides the wheel's sixteenths by what the host's Resolution Multiplier leaves to do, but reported the undivided value; it now reports the divided one. And the multiplier now starts at 1, not 16, until the host sets it, so a host without Resolution Multiplier support (which never sets it) gets whole steps instead of 16 times the scrolling |
| `zmk/0002-pointing-resolution-multiplier-back-to-1-when-the-host-goes.patch` | The multiplier a host set stayed for its Bluetooth profile or USB after it disconnected; a host back from sleep that didn't set it again then scrolled 16 times too far until a reboot. Now reset to 1 when the Bluetooth connection drops or USB is unplugged; a host with Resolution Multiplier support sets it again when it connects |
| `zmk/0003-hog-queue-mouse-reports-without-waiting.patch` | ZMK's pointing input listener sends a mouse report per input event from the system work queue, and each waited up to 100 ms for room in the Bluetooth report queue. While the link stalls, the queue stays full and the work queue ran nothing else until the link dropped, long enough for the watchdog to reset the board. Now queued without waiting, the oldest report dropped when the queue is full |

## Requirements (Windows)

- Docker Desktop
- For flashing over USB: `mcumgr`, installed with [Go](https://go.dev/dl/):
  ```powershell
  go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
  ```
- For the matrix scripts only: Python 3 with pyserial (`python -m pip install pyserial`)

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

The output is `out\<shield>[-<snippet>].signed.bin`, signed for Framework's MCUboot, and its `.elf` beside it, also kept as `out\elf\<artifact>-<end of code>.elf` (the last 10 per artifact) for looking up the addresses of a reset record.

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
| `rethink-usb-shell` | Same port, as a Zephyr shell with the log | As `rethink-usb-log`, plus the `adapter` bring-up commands, the `log` commands (filters, see below) and the `i2c`, `gpio` and `regulator` shells. See [Adapter bring-up shell](docs/shields.md#adapter-bring-up-shell). |
| `framework-cb-uart-log` | Header UART: TX P2.02, RX P2.00, 115200 baud | Keeps the UART receiver on (battery drain). |
| `rethink-profiling` | (no log of its own) | For power profiling from a bench supply into the battery input: charging off, so USB plugged in for the shell doesn't charge into the supply; low-battery cut-off at 3.0 V instead of 3.3 V, under the EFM32 kit's 3.24-3.27 V (less under a running stick) and still there to try ship mode on a bench supply. With the shell: `-Snippet rethink-usb-shell,rethink-profiling` (several snippets separated by commas). With a pointing stick, put a reservoir capacitor across the battery input at the board, 1000 µF for the T420 on the EFM32 kit ([switch-on surge](docs/power.md#pointing-stick-switch-on-surge)). |

All of them log at ZMK debug level. The USB driver logs only warnings (at info level it logged every receive buffer). In the shell, `log enable inf zmk` hides ZMK's debug lines (keys, HID reports, the connection details) and keeps the rest, such as `rethink_ble`'s connection events (`ble log`); `log enable dbg zmk` brings them back, `log disable <module>` silences one (`rethink_trackpoint`, `fuel_gauge`, ...) and `log status` lists them.

## Repository layout

```text
.
├── build.ps1 / flash.ps1        Host scripts (Windows)
├── scripts/build.sh             Build steps, shared by build.ps1, dev container and CI
├── scripts/matrix_scan.py       Maps a keyboard's matrix (rethink_scanner)
├── scripts/matrix_view.py       Shows the matrix live: stuck keys, chatter, keys not pressed yet
├── docker/                      Build image
├── .devcontainer/               VS Code dev container
├── .github/workflows/build.yml  CI, matrix in build.yaml
├── config/west.yml              West manifest (pinned)
├── config/rethink.conf          Hardware options (jumpers, fitted parts)
├── zephyr/module.yml            This repository as a Zephyr module
├── CMakeLists.txt, Kconfig      Module sources and options (CONFIG_RETHINK_*)
├── src/                         Pairing button, profile and blue LEDs, adapter control, expander
│                                registers, matrix, bring-up commands, backlight,
│                                pointing stick and its key behavior, power off (wired,
│                                low battery, unused),
│                                ble and reset diagnostics shells
├── include/rethink/             Public API (adapter.h, expander.h, backlight.h, trackpoint.h,
│                                reset_diag.h)
├── include/dt-bindings/rethink/ Keymap constants (trackpoint.h: TP_OFF, TP_ON, TP_TOG, TP_RECAL,
│                                TP_RATE_UP, TP_RATE_DN, TP_PTS_TOG, TP_SCROLL_TOG)
├── dts/bindings/                rethink,adapter-ctrl, rethink,pcal6416a, rethink,matrix,
│                                rethink,backlight, rethink,trackpoint and
│                                rethink,behavior-trackpoint
├── boards/shields/              Shields; rethink_common/ holds the shared dtsi files
│                                (Control Board, adapter, its matrix and full layout, pointing stick,
│                                backlight, header matrix) and the shields' Kconfig defaults
├── docs/                        Documentation (see Documentation above)
├── patches/zmk/                 Patches to ZMK, applied at build time (see Software stack)
└── snippets/                    rethink-usb-log (USB CDC ACM logging),
                                 rethink-usb-shell (shell and log on the same port),
                                 rethink-profiling (charging off, low-battery cut-off at 3.0 V)
```

## Changing framework-cb

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
