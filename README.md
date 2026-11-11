# ReThink ZMK config

ZMK firmware for the [ReThink Keyboard](https://github.com/Ziemniaczenka/ReThink_Keyboard): ThinkPad keyboard modules driven by the Framework Wireless Touchpad Keyboard Control Board (nRF54LM20A + nPM1300) through the ReThink adapter.

> Work in progress. Shields so far: the testers `rethink_bare_tester` and `rethink_hw_tester`, the matrix scanner `rethink_scanner`, and the keyboards `rethink_l420`, `rethink_t420`, `rethink_t480` and `rethink_e580`.

> [!IMPORTANT]
> Most of this repository is vibe-coded and may not follow best practices. I test it on my hardware before pushing commits but use it at your own risk.



## Roadmap

- [x] Container build (local and CI), flashing over USB, bare Control Board tester
- [x] Adapter control, backlight power chain and interlock, hardware options (`config/rethink.conf`)
- [x] Power off in wired mode without USB; charger current fix (framework-cb)
- [x] Adapter bring-up: `rethink-usb-shell` commands, idle expander pins on pulls (System OFF at the bare board's current)
- [x] Key matrix: expander register access without Zephyr's PCAL64xxA driver (a column step is one 1-byte I2C write), open-drain columns for diodeless ThinkPad matrices, ghost-key blocking, expander columns DRV15-17, HOTKEY/PWRSWITCH through the expander interrupt. Tested with shorts on the connectors
- [x] Matrix scanner: `rethink_scanner` and `scripts/matrix_scan.py`, which asks for each key, reads its row and column from the USB log, and writes the matrix transform and a keymap. First keyboard mapped: L420 (FFC1). `scripts/matrix_view.py` shows the matrix live, for faulty keys
- [x] Shield `rethink_l420` (FFC1): keymap from the scan, ISO, Fn layer per the keycaps; TrackPoint comes later
- [x] Blue LED only for 3 s after connecting, not the whole time
- [x] Matrix scans only the columns and direct keys the layout maps (the L420 skips DRV16/17 and HOTKEY/PWRSWITCH: two I2C transfers per scan instead of five)

Next, in this order:

- [x] Shield `rethink_t420` (Mezzanine): keymap from the scan, buttons above the keyboard, Fn layer per the keycaps, Caps Lock LED
- [x] T420 ANSI keyboard: scanned; differs from ISO in three positions, now in the same layout (`rethink_t420` runs either).
- [ ] TrackPoint, TP1 on the T420 and L420 (see [Pointing stick](#pointing-stick-in-progress)):
  - [x] Power, reset and the bit-banged PS/2 link, `tp` shell commands. Tested on the T420: ID 01/0e, 14.7 kHz, no bit errors on USB, one byte in ~1500 broken off with Bluetooth
  - [x] Pointing: start at boot (self-test, ID, streaming on), packets to ZMK's pointing input. Tested on the T420: movement and its three buttons over USB and Bluetooth
  - [x] Reliable reception. With pointing in the shell build (debug logs over USB), wired: 2505 bytes without an error; over Bluetooth: 107 errors in 913 bytes (46 framing, 3 parity, 58 broken off; earlier 4 % in 4444). An interrupt delayed by more than one clock period (68 µs) misses a bit, and the Bluetooth controller's upper layer shared the GPIO interrupt's priority 1. With GPIOTE20 at priority 0 (`rethink_trackpoint.dtsi`): wired 13466 bytes without an error, Bluetooth 12 errors in 2673 bytes (0.45 %: 1 framing, 10 parity, 1 broken off), so now mostly a bit read late rather than an edge missed. A packet with an error is now asked for again (`fe`, TrackPoint specification 2.4.2), so a lost packet doesn't take a button change with it; `tp stat` counts them as sent again or lost. Tested over Bluetooth: 12 errors in 3729 bytes, 11 packets sent again, none lost. A command's reply lost the same way is asked for again too. If errors grow, receive through UARTE20 instead, where the hardware samples the bits: a PS/2 byte is UART 8O1 at the stick's clock (14.4 kBd is within 2 % of this stick's 14.7 kHz; the nRF54L has odd parity). Sending stays bit-banged: rare, and the stick clocks it. Also drop packets with the overflow bits set
  - [x] Keys: Fn+F8 on/off, Ctrl+Fn+F8 recalibrate (`&tp`). Tested on the T420: off at once, on in 0.87 s; recalibrating does nothing while the stick is off. Report rate up and down (`&tp TP_RATE_UP`, `TP_RATE_DN`): tested, not bound in the keymaps. Off after 10 minutes unused: tested, the next key press starts it again, in about 0.3 s with the reset held 20 ms (0.87 s with 600 ms)
  - [x] Scrolling: middle button held while the stick moves sends the wheel instead of movement, vertical, and horizontal only past a dead zone. A middle click only if the button comes up without movement (see Scrolling in [Pointing stick](#pointing-stick-in-progress)). Tested on the T420: both axes; the defaults (20 counts per wheel step, sideways dead zone 2 counts per report) are fine, `tp set` tunes them
  - [x] Smooth scrolling (`CONFIG_ZMK_POINTING_SMOOTH_SCROLLING`, the HID Resolution Multiplier): the wheel in sixteenths of a step, for hosts that set the multiplier; whole steps for others. Tested on the T420. Bug found: after the laptop slept, 16 times the scrolling (600 000 instead of 30 000 px/s) until the keyboard restarted: the multiplier the host had set stayed after it disconnected. Now reset when the host goes (`patches/zmk/0002`). Tested: the log shows the reset when the laptop disconnected, and the laptop set 16 again when it came back
  - [x] Sensitivity and smoothing: the stick's own settings (sensitivity, negative inertia, plateau speed, drift time, press-to-select; see the table in [Pointing stick](#pointing-stick-in-progress)), the report rate and the scrolling numbers as settings: Kconfig defaults, `tp set`, kept in flash, the stick's written after each start since a power cycle clears them. Defaults sensitivity 192 (×1.5) and speed 150 (about 667 counts/s), press-to-select on Alt+Fn+F8. Tested on the T420: sensitivity 192 and speed 150, Alt+Fn+F8, press-to-select kept over a power cycle, the scroll divisor default (20). Scroll speed is `scroll_divisor`; the sixteenths of a step are fixed by ZMK's HID descriptor (a finer multiplier would need a descriptor patch and new pairings, and the 80 reports/s limit smoothness more). ZMK's input processors (scaling) only if the stick's own sensitivity isn't enough
  - [x] Middle button: a switch between scrolling and a plain middle button, for programs that use it themselves (CAD pans or orbits with the middle button held while the pointer moves). A setting kept in flash (`tp set middle_scroll`) and `&tp TP_SCROLL_TOG` on Shift+Fn+F8, beside the other stick keys on Fn+F8 (on/off, Ctrl recalibrate, Alt press-to-select); Shift alone reaching the host does nothing. Read each time the middle button goes down, so switching while it is held changes nothing until it comes up. Untested
  - [ ] Power, see below
  - TP2 (FFC2): `rethink_t480` and `rethink_e580` (the `tp2` property and TP2_RST on `&trackpoint`), untested
- [ ] Pointing stick power. The stick path: the stick at 5 V, the charge pump doubling from 3.3 V (input current about twice the output), R14 and the PCA9306. The TrackPoint specification: the controller should average under 6 mA (25 mA peak); the sensor bridge is at least 350 Ω (up to 14.3 mA at 5 V) and is powered for each conversion (an op-amp power-up delay starts each one); the stick converts 80-120 times a second whatever the report rate, and sends packets only when something changes. Measured on a T420 over Bluetooth, on battery (shell build, USB unplugged), the stick still:

  | Stick | Battery current | Stick path |
  | --- | --- | --- |
  | Off (Fn+F8 or `tp off`) | 1.56 mA | |
  | On, 100 reports/s | 20.6 mA | 19 mA |
  | 80 reports/s (`f3 50`) | 19.1 mA | 17.5 mA |
  | Sampling off (`e2 47 20 10`) | 13.8 mA | 12.2 mA |
  | Powered down (`e2 44`) | 5.8 mA | 4.2 mA |

  That is the Norwegian ISO T420 keyboard (FRU 45N2161, model C9-90N0). The US ANSI one (FRU 45N2241, model C989) draws 25.5 mA with its stick on and 1.5 mA off, with the firmware of 2026-11-08 as with today's (2026-11-11), so its stick path costs 24 mA: sticks differ by several mA. Other keyboards, stick on (off: 1.5 mA on all): L420 45N2373 33 mA, L420 45N2362 18 mA; the 3.3 V sticks on TP2 far less, T480 01AX516 4.5 mA, E580 01YN773 2.1 mA. All figures, by FRU and with the firmware commit: [Keyboard power](https://github.com/Ziemniaczenka/ReThink_Keyboard/blob/main/docs/keyboard_power.md) in the hardware repository. Earlier: 23.6 mA while it moves, 21.5 mA while typing. So the force measurement costs about 7 mA of the 19 (1.5 mA of it saved at 80/s), the stick's controller awake about 8 mA, and pump, R14, shifter and the stick asleep about 4 mA; the Caps Lock LED adds 1.9 mA (3.5 mA with the stick off). Report rates, battery current with the stick still: 10/s 20.3 mA, 20/s 20.3, 40/s 19.1, 60/s 21.7, 80/s 19.1, 100/s 20.5, 200/s 20.5 (its packets still 10 ms apart). So 80/s is the cheapest, and 10-20/s cost more despite the specification's 80 measurements a second. Done: 80 reports/s on battery and 100/s with USB power (`rate_usb`, where the battery doesn't pay for it), off after 10 minutes unused, Fn+F8 by hand, rate keys. A start in about 0.3 s instead of 0.87: the specification's 600 ms of reset after power-on aren't needed; with 20 ms the T420 stick answered its self-test 255 ms after the reset, as with 600 ms, over ten starts (`tp set reset_ms`, default 20; 50 ms would cost 30 ms more). Experimental, off: 80/s while still and the set rate from the first movement packet (`CONFIG_RETHINK_TRACKPOINT_SLOW_IDLE`; tested at 100/s: back to 19.1 mA 2 s after the stick stops). 100/s felt better to point with than 80/s; 80/s kept on battery for the 1.4 mA. Left:
  - Sampling off while still: 14.1 mA with streaming on (5 mA less than 80/s). The buttons still report, but the stick doesn't notice a touch, so something else has to switch sampling on again within milliseconds. Maybe: sampling on for a moment every 100 ms or so to look for a touch, if the stick keeps its zero point through it. It survives `ff` (`tp on`)
  - `e2 44` powers down only the stick; switching the whole path off (1.56 mA) is better for the idle state
  - Sampling off and power-down (`e2 47 20 10`, `e2 44`) on the other sticks (L420, T480, E580): what they save, and whether the buttons still report and wake them. After the profiling
  - Later, if a stick turns out too slow even at sensitivity 255 (the T480's feels less sensitive at 192): a scale factor on the stick's output in the firmware. Not now
  - Running the stick at 3.3 V (JP6, JP4, JP5 to 2-3: no pump, no level shifter; `CONFIG_RETHINK_TP1_SUPPLY_3V3`): the specification asks for 5 V ± 10 %, so not for the T420 stick; for 3.3 V sticks (T480 class, TP2)
  - R14 (10 kΩ to the stick's 5 V) carries 0.5 mA while the reset is driven low, about 1 mA from the battery through the pump: a larger value (e.g. 100 kΩ) on the next adapter revision; there a 5 V boost converter (about 90 % efficient) instead of the charge pump (5 V from 6.6 V doubled, 76 %) would save about 15 % of the stick's share
- [ ] Bluetooth drop-outs: a few times after USB was unplugged (on battery), the keyboard disconnected and reconnected (blue and white LEDs lighting, as on each connection, and the stick starting again), stopping after a few rounds or once USB was back; never seen with USB in. Not noticed before the settings commit (334e64f). `ble log` after one: `last reset: watchdog`, so the firmware hung: the system work queue, which feeds the watchdog, ran nothing for 8 s; Zephyr also halts there after a fatal error. Now recorded for after the reset (`src/reset_diag.c`, `ble log`): a fatal error with its thread and code addresses, after which the board reboots at once, or a stall of the work queue with what it waits on and what ran instead; both tested (`diag crash`, `diag stall`). Found so far, 2026-11-10:
  - The Bluetooth RX work queue (connection callbacks) had used all of its 1200 bytes of stack (`kernel thread stacks`); one byte more is a stack overflow, a fatal error, and before the records a halt until the watchdog. Now 2048, of which it used 1200 (`rethink_common/Kconfig.defconfig`)
  - Twice after unplugging USB, without a reboot: connections timed out (0x08) 6-37 s later, sometimes both hosts at once, and a fresh connection timed out before its encryption. On the RC 32 kHz clock (framework-cb's choice) the system timer (GRTC), which also times the Bluetooth link, keeps time from the low-power RC while the chip sleeps, and on USB the chip hardly sleeps. Now on the 32 kHz crystal X1 (`rethink_cb.dtsi`), with the board's external capacitors only. First test: no drop-out in 4 minutes on battery (before: 6-37 s after unplugging); to watch
  - The stall recorded before one of those timeouts, decoded with that build's `.elf`: the system work queue sat in ZMK's `zmk_hog_send_mouse_report`, called by the pointing input listener (`input_sync_cb`). That sends one report per stick event and waits up to 100 ms on each while the Bluetooth report queue is full; with 80-100 events a second it never catches up. So a stalled link keeps the work queue, and with it the watchdog feed, busy until the link drops: 4.5 s here, and past about 6 s the watchdog resets the board. Now `patches/zmk/0003`: mouse reports are queued without waiting, the oldest dropped when the queue is full (as ZMK did after the wait; the newest carries the buttons), so a stalled link costs a reconnect, not a reboot. Tested only on a slow link (40 ms, the longest the laptop granted with latency 30): the pointer lags and nothing stalls, but the old build survived that too. Records now carry their build, and every build's `.elf` is kept (`out/elf/`), since a record from an earlier build decodes only with its own
  - Not changed yet: a race in nrfx that can lose a GRTC compare event and so stop the system timer for good (DevZone 129122, nRF54L15; the same code is here). That shows as a watchdog reset with nothing recorded; then patch nrfx (`build.sh` patches only `zmk` and `zephyr` so far)
- [ ] Bluetooth stall while pointing (seen once, 2026-11-10, USB in): about 0.6 s after a stick start, both hosts' connections stopped for 4 s and timed out together (0x08), while the stick still sent 100 reports a second and ZMK's report queues filled; reconnected at once. The start itself was as the others. Likewise once on 2026-11-08: a timeout about 4 s after `tp on`. Both hosts at once points at the keyboard's radio rather than the hosts
- [ ] Battery: fuel gauge model of the real cell (the "Example" model reads 50 % at 4155 mV), charge-current reading (fuel gauge +525 mA, meter 440 mA), discharge limit (200 mA, 1 A with the backlight), USB 1.5 A. Make the cut-off and the level agree (hosts show the fuel gauge's level), so the keyboard doesn't show 15 % and then switch off: e.g. a model of the cell whose 0 % falls at the cut-off voltage, cutting off at 0 % with the voltage as a floor, or the reported level rescaled so the cut-off reads 0 %. Done, tested on the T420 (2026-11-11): off below 3.3 V on battery, and asleep after 30 minutes unused on battery (see Power off in `rethink_bare_tester`); the voltage from framework-cb's fuel gauge (its `GAUGE_VOLTAGE` channel)
- [ ] Idle current on Bluetooth. Measured on battery with a multimeter:

  | Setup | Connected | Advertising |
  | --- | --- | --- |
  | `rethink_l420`, adapter and keyboard | 1.58 mA | |
  | `rethink_l420`, adapter, no keyboard | 1.58 mA | 1.7 mA |
  | `rethink_bare_tester`, bare Control Board | 1.6 mA | 1.7 mA |
  | System OFF (wired mode, no USB) | 6-8 µA | |
  | A key held (1 kHz scan with I2C, before the column skip above) | +1.2 mA | |
  | Caps Lock LED on (`rethink_t420`) | +1.9 mA | |

  So a floor of about 1.5 mA comes from the Control Board and firmware, not the adapter or the matrix, and the radio adds little. In the USB log no connection-parameter update followed in 4 minutes (ZMK logs `interval N latency N` on one), so the host kept its own parameters. `ble conn` (2026-11-10): both hosts chose 15 ms with latency 30, the longest of Zephyr's request, so the latency is honoured; asked for 7.5 ms, a host took it. A tuned ZMK keyboard idles at tens to a few hundred µA, so about 1 mA is to be found. Next: a trace with a power profiler (an EFM32 Gecko kit's energy monitor, its 3.3 V into the battery input; snippet `rethink-profiling`, with charging and the low-battery cut-off off). The floor on a build without the shell; the shell (`-Snippet rethink-usb-shell,rethink-profiling`) for `ble conn` and `tp` between traces, with USB plugged in only while typing commands. Also to measure there: the 7.5 ms interval. With `ble conn 6` the pointer moved visibly smoother than at 15 ms, where the stick's 80 reports a second beat against 66 intervals (USB is smoother still); if it costs little and hosts accept it, it becomes the default (`CONFIG_BT_PERIPHERAL_PREF_MAX_INT=6`). Predictions, by what the trace would show:
  - **Flat floor between radio events:** something stays on. Candidates: the USB controller and its regulator (`vregusb`, enabled in the board DT) without VBUS, the 32 MHz crystal held by a driver, or the CPU not sleeping. Check: a plain Zephyr BLE sample (`peripheral_hids`) on the same board. At ~1.5 mA too, it's the board or Zephyr; lower, it's ZMK or these modules.
  - **Spikes every 4 s:** calibration of the RC 32 kHz clock (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC_CALIBRATION`). Now on the 32 kHz crystal X1 (fitted, Bluetooth drop-outs above), which needs no calibration and narrows the radio's receive windows (100 ppm declared instead of 250).
  - **Radio events every 15 ms while idle and connected:** the keyboard doesn't skip events, though the hosts granted latency 30 (`ble conn`); with it, an idle keyboard need only answer every 31st (465 ms).
  - **Advertising:** ZMK advertises every 30-60 ms for as long as no host is connected. Slow down after about 30 s (ZMK patch).
  - Then System OFF after some minutes untouched on Bluetooth too, waking on a key: done, 30 minutes on battery (`CONFIG_RETHINK_IDLE_SLEEP_MIN`), with the idle matrix's level interrupts as wake sources. ZMK's own `CONFIG_ZMK_SLEEP` would leave the stick powered.
- [ ] Backlight on a real keyboard (`rethink_t430`): current on BUCK1 (50 mA works, ~106 mA doesn't), `CONFIG_RETHINK_BACKLIGHT_MAX_DUTY` (capped at 10 % until then), PWM frequency
- [ ] Control Board LEDs: the blue LED sometimes misbehaves (`CONFIG_RETHINK_CONNECTED_LED`, to be described); PWM brightness control (RGB especially); RGB as charging indicator; battery level, charging and backlight-level animations on the white LEDs; a waking and connecting animation (from sleep the keyboard takes about 2 s to reconnect, and keys pressed meanwhile are lost)
- [ ] Shields on FFC2 (2026-11-11): `rethink_t480` from a UK ISO T480 and `rethink_e580` from a US ANSI E580 with numpad. Tried on a T480 (01AX516) and an E580 (01YN773): typing, the lock LEDs, the stick on TP2 at 3.3 V (2.9 and 0.6 mA, against 17.5 mA and more for the 5 V sticks; over Bluetooth 30 and 44 bytes in about 8600 had an error, as on the T420, all packets sent again, none lost). Neither stick has press-to-select (newer generations dropped it), so Alt+Fn+F8 isn't bound on these two; keypad ( and ) work on Linux and Android, not on Windows. Both are on the T420's matrix, the E580's numpad on DRV16/17 and SENSE2/3 x DRV9/11/12. On the scanned T480 J registers at the lightest touch and U, I, O, P never, though their lines work for other keys, so that keyboard is faulty and those positions come from the T420. To test: every key, every connector pin on the way. Later: Fn lock (Fn+Esc and its LED), the backlight, `rethink_l14`
- [ ] Bluetooth: the T420 build doesn't pair with a Samsung TV (Windows and Android pair fine). Not looked into: the TV, ZMK, or the mouse in the HID descriptor since pointing
- [ ] Self-test after programming, for user-built boards: check the adapter (I2C) and other parts at boot and report a fault without a serial log, e.g. a sequence of red blinks
- [ ] To look into later: pinch zoom with the stick, as on a touchpad (smooth, without reflowing the page). As far as known, a mouse can't send it: Windows (Precision Touchpad) and Linux (libinput) make pinch gestures only from touchpads and touchscreens, two contacts moving apart; mice zoom with Ctrl and the wheel, or through their maker's software. The keyboard would have to report a touchpad (a HID digitizer) besides the mouse
- [ ] ZMK Studio: change the keymap at runtime without rebuilding (swap Fn and LCtrl, Fn-layer and unused keys) and pick ISO or ANSI, which Studio offers when a shield defines both physical layouts. Needs key positions in each physical layout, an ANSI transform per keyboard (the T420 has one layout for both now, which ISO and ANSI layouts would map onto), a `&studio_unlock` key (ThinkVantage on the T420), spare layers, and Studio over USB (a CDC ACM port on the device_next stack, which the log and shell snippets use now) and Bluetooth. Settings beyond the keymap, such as the sleep and stick-off times, the stick's sensitivity and speed, and when Caps Lock and the other LEDs light up (more as other keyboards are tested): upstream Studio edits only the keymap; other settings need a custom Studio RPC subsystem, which so far only an unofficial ZMK fork offers (cormoran's `v0.3+custom-studio-protocol`, used by `zmk-module-runtime-input-processor`); to follow. Until then they are Kconfig options, and the stick's sensitivity and speed are saved from the shell (`tp set`)
- [ ] Test suite for adapters and keyboards in numbers (for selling them): partly automated checks with a pass/fail report per unit. Adapter: supplies, expander, every connector line (`adapter shorts`), the status LEDs, the stick ports, from the bring-up shell. Keyboard: every key of its shield's layout once, no key stuck or chattering (`scripts/matrix_view.py`), the lock LEDs, the stick (self-test, movement, buttons), its current. A PC script would drive the board's shell and ask the tester for the manual steps

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
| `rethink-usb-shell` | Same port, as a Zephyr shell with the log | As `rethink-usb-log`, plus the `adapter` bring-up commands, the `log` commands (filters, see below) and the `i2c` and `gpio` shells. See [Adapter bring-up shell](#adapter-bring-up-shell). |
| `framework-cb-uart-log` | Header UART: TX P2.02, RX P2.00, 115200 baud | Keeps the UART receiver on (battery drain). |
| `rethink-profiling` | (no log of its own) | For power profiling from a bench supply into the battery input: charging off, so USB plugged in for the shell doesn't charge into the supply, and the low-battery cut-off off, since the supply sits at 3.3 V. With the shell: `-Snippet rethink-usb-shell,rethink-profiling` (several snippets separated by commas). |

All of them log at ZMK debug level. The USB driver logs only warnings (at info level it logged every receive buffer). In the shell, `log enable inf zmk` hides ZMK's debug lines (keys, HID reports, the connection details) and keeps the rest, such as `rethink_ble`'s connection events (`ble log`); `log enable dbg zmk` brings them back, `log disable <module>` silences one (`rethink_trackpoint`, `fuel_gauge`, ...) and `log status` lists them.

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
- **Pairing button** (P3.10, outside the matrix): tap for the next Bluetooth profile (at once; on `rethink_hw_tester` 0.4 s after the tap, to tell it apart from the triple tap that unlocks the backlight there); hold for 3 s, then release, to clear the active profile. Both are ignored while the mode switch is on wired. Hold for 10 s to reboot into MCUboot recovery (keep holding until the green LED lights); holding it while plugging in USB also enters recovery.
- **USB/Bluetooth switch** (P3.05, outside the matrix): Bluetooth sends over Bluetooth, falling back to USB when no host is connected. Wired turns Bluetooth off and sends over USB.
- **White LEDs 1-4:** show the active Bluetooth profile (4 profiles) for 3 s after boot and after each change, blinking while the profile is unpaired.
- **RGB LED:** blue for 3 s when a host connects (USB, or the active Bluetooth profile) and after switching to a connected profile.
- **Power off** (`src/power.c`): the stick, the adapter, the backlight and all LEDs off, then System OFF (about 7 µA). Plugging in USB always wakes the board, which then boots afresh. Three reasons:
  - **Wired mode without USB:** with the switch on wired and no USB power, after 3 s. Moving the switch to Bluetooth wakes it too; the pairing button and the keys do nothing.
  - **Low battery:** on battery, under 3.3 V in three readings 10 s apart (`CONFIG_RETHINK_LOW_BATTERY_OFF_MV`). Only USB wakes it, so a stray key can't drain the cell further. The voltage comes from framework-cb's fuel gauge rather than its level, which depends on the battery model.
  - **Unused:** on battery, no input from the keys, the stick or the buttons for 30 minutes (`CONFIG_RETHINK_IDLE_SLEEP_MIN`). A key, USB or moving the mode switch wakes it; the key that wakes it isn't typed. Keys on `rethink,matrix` keyboards wake it, since its idle matrix waits on level interrupts, which arm the GPIO sense.

  In the shell: `power stat` (USB power, battery voltage and cut-off, time unused), `power lowbat <mV>` and `power sleep <min>` (until the next reset, to try both without draining the cell or waiting), `power off` (sleep now).
- **P3.11** (Framework's backlight PWM): disabled. On ReThink it enables the adapter's backlight boost converter.
- **Name:** shows up as "ReThink Tester" over USB and Bluetooth.

### `rethink_hw_tester`

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

Watching the matrix, for a keyboard whose keys misbehave or to check every key of a new one: with the same setup, `python scripts\matrix_view.py` draws the matrix in the terminal (112 characters wide) and marks each position: pressed now, held for more than 3 s now or before (a key that registers at the lightest touch, a short), already held when the log started, chatter (a press within 30 ms of the last release, or shorter), pressed before, not yet. Below it, the problems in words and the last events. `--shield rethink_t420` takes key names from a shield with the same matrix and lists the keys not pressed yet; `--log out\t480_iso_scan.log` shows a saved log (the live one goes to `out\matrix_view.log`). The firmware blocks ghost keys, so they don't show.

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
  | F8 | TrackPoint on/off; with Ctrl: recalibrate it (hands off the stick); with Alt: press-to-select (clicking by pressing the stick) on/off; with Shift: middle-button scrolling on/off |
  | F9, F10, F11 | Scroll Lock, NmLk (embedded keypad on/off), Pause |
  | Insert, Delete, Home | PrtSc, brightness down, brightness up |
  | PgUp, PgDn | Browser back, forward |
  | Up, Down, Left, Right | Stop, play/pause, previous, next |

  Battery (F3), wireless (F5), camera (F6), hibernate (F12) and zoom (Space) have no standard key code. The embedded keypad sends the digits on 7-9, U-O, J-L and M, and `/ * - +` on 0, P, `;` and `/` (UK ISO legends), independent of the host's Num Lock.
- **Not on this keyboard:** backlight (BUCK1 stays off from boot) and status LEDs; the expander's LED pins rest on their pull-ups.
- **TrackPoint** (8-pin cable in J5; its buttons go to the stick, not the matrix): a mouse, see [Pointing stick](#pointing-stick-in-progress).
- **Name:** "ReThink L420".

Build with `-Snippet rethink-usb-log` to see `position: N` for each key in the log.

### `rethink_t420`

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
- **TrackPoint** (Mezzanine 37-40): a mouse, see [Pointing stick](#pointing-stick-in-progress).
- **Name:** "ReThink T420".

### `rethink_t480`

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
- **Not yet:** the backlight (BUCK1 stays off).
- **TrackPoint:** TP2 on the 12-pin cable, at 3.3 V (adapter JP7, JP8, JP9 at 1-2, `CONFIG_RETHINK_TP2_SUPPLY_3V3`), a mouse; its buttons come over the matrix cable (pins 33-35) to the adapter and on to the stick. This generation's sticks (T480, E580) have no press-to-select, so Alt+Fn+F8 isn't bound to it. See [Pointing stick](#pointing-stick-in-progress).
- **Name:** "ReThink T480".

### `rethink_e580`

ThinkPad E580 keyboard with numpad on FFC2: the 40-pin matrix cable and the 12-pin cable. Mapped with `rethink_scanner` on a US ANSI E580 keyboard. Tried on an E580 (01YN773): typing, the lock LEDs, the stick (2.1 mA with it, 1.5 mA without).

- **As `rethink_t480`,** with the numpad: the physical layout `rethink_e580_layout`, 107 positions. The main block is at the T420's positions, the ISO keyboard's `#` and `\` right of LShift included (unscanned); the numpad on DRV16/17 and SENSE2/3 x DRV9/11/12.
- **Numpad:** keypad codes, so the host's Num Lock picks digits or navigation; its top row sends keypad `=`, `(` and `)` (Linux and Android take those two, Windows ignores them) and Backspace. Fn+keypad `=` opens the calculator and Fn+Backspace sleeps, as their keycaps show.
- **Caps Lock and Num Lock LEDs** follow the host.
- **Name:** "ReThink E580".

### Pointing stick (in progress)

`rethink_trackpoint.dtsi` adds the TP1 stick (Mezzanine 37-40, FFC1 J5) to a keyboard shield; `rethink_t420` and `rethink_l420` include it. The driver (`rethink,trackpoint`, `src/trackpoint.c`) starts the stick after boot and reports it to ZMK as a mouse (`CONFIG_ZMK_POINTING`, on for shields with the stick), over USB and Bluetooth. Without an answer from a stick (none connected) it powers it off again. Wired power-off switches it off before System OFF.

- **Power:** TP_3V3 (P3.12), then the charge pump for a 5 V stick (`CONFIG_RETHINK_TP1_SUPPLY_5V`, JP6 1-2), all with the reset held. The reset (TP1_RST, active high, R14 pull-up to the stick's supply) is released 20 ms later (`tp set reset_ms`, DT `reset-ms`). The TrackPoint specification gives 600 ms as its power-on reset time, but the stick only needs its supply up: the T420 stick's self-test result follows 255 ms after the reset either way. `tp stat` shows the last start's times.
- **PS/2 link:** FW_CLK (P1.16) and FW_DAT (P1.15), through the PCA9306 for a 5 V stick. The stick clocks every bit (a T420 stick at about 14.7 kHz); an interrupt on each falling clock edge reads DATA or sets the next bit. The interrupt (GPIOTE20) runs at priority 0, beside the Bluetooth controller's lower layer, since at priority 1 the controller's upper layer delayed it by more than a clock period. A packet with a broken byte (framing, parity, or unfinished 1.2 ms after its start) is dropped, and the thread asks the stick to send it again (`fe`) before its next packet; so is a command's reply that doesn't come in time.
- **Start:** self-test result `aa 00`, TrackPoint ID (`e1`, logged), the stick's settings (see Settings), the report rate (`f3`), streaming on (`f4`).
- **Pointing:** 3-byte packets every 10 ms while the stick moves, assembled in the interrupt. A pause, or a byte lost to an error, starts the next packet afresh. Movement goes out as relative X/Y, the packet's left, right and middle buttons as mouse buttons 1-3 (the middle one: see Scrolling). The buttons are wired to the stick: the T420's under the space bar, the L420's through J5. Press-to-select (clicking by pressing the stick) is off on the T420 stick.
- **Scrolling** (`CONFIG_RETHINK_TRACKPOINT_SCROLL`): while the middle button is held, movement turns into the wheel; Shift+Fn+F8 or `tp set middle_scroll 0` makes it a plain middle button again, also while held (for programs that pan with it), and back. Each report's stronger axis scrolls: vertically, or sideways only past a dead zone (`CONFIG_RETHINK_TRACKPOINT_SCROLL_H_DEAD_ZONE`, 2 counts per report, and only the excess), so a push up that leans a little doesn't scroll sideways. 20 stick counts make one wheel step (`CONFIG_RETHINK_TRACKPOINT_SCROLL_DIVISOR`; usually three lines), sent in sixteenths (smooth scrolling, `CONFIG_ZMK_POINTING_SMOOTH_SCROLLING`, on with scrolling) to a host that sets the HID Resolution Multiplier to 16, and in whole steps to others. Letting the middle button go clicks it only if the stick moved no more than 4 counts meanwhile, so the click comes on release. Middle-button drags are gone with it.
- **Report rate:** 80/s on battery (`CONFIG_RETHINK_TRACKPOINT_SAMPLE_RATE`), the cheapest measured (roadmap: Pointing stick power), and 100/s while USB powers the keyboard (`CONFIG_RETHINK_TRACKPOINT_SAMPLE_RATE_USB`), which points more smoothly; the rate changes as USB power comes and goes. The stick measures its force 80 times a second at 10-40 and 80/s, 120 at 60/s, 100 at 100 and 200/s (TrackPoint specification 2.4.6); at 200/s the T420 stick's packets still came 10 ms apart. Experimental (`CONFIG_RETHINK_TRACKPOINT_SLOW_IDLE`, off): after 2 s without movement the stick reports at 80/s until its next movement packet, which brings the set rate back: streaming off, `f3`, streaming on, about 10 ms without packets; for 60, 100 and 200/s. `tp stat` counts the changes.
- **Settings** (`tp set`): the report rates (battery, USB power), the stick's sensitivity, negative inertia, upper plateau speed, drift time and press-to-select (see the table below), the reset hold after power-on, middle-button scrolling on or off and its numbers. Kconfig gives the defaults (`CONFIG_RETHINK_TRACKPOINT_*`); a changed setting is saved to flash 3 s after the last change and kept over resets and new firmware; one set back to its default is deleted from flash, so a new Kconfig default applies. The stick's own live in its RAM, which a power cycle clears: each start writes them, after a power-on only those that differ from the stick's power-on values (as Linux does). While pointing a change is written at once, with streaming off around it. Sticks other than IBM's (ID 01, the T420's) take only sensitivity and press-to-select.
- **Keys:** `&tp TP_TOG` switches the stick on and off (`TP_ON`, `TP_OFF`), `&tp TP_RECAL` recalibrates it, `&tp TP_RATE_UP` and `TP_RATE_DN` step the report rate in effect (battery or USB power) through 10, 20, 40, 60, 80, 100 and 200/s, kept like `tp set rate`, `&tp TP_PTS_TOG` switches press-to-select, kept like `tp set press_to_select`, `&tp TP_SCROLL_TOG` middle-button scrolling, kept like `tp set middle_scroll` (`dt-bindings/rethink/trackpoint.h`). Both keyboards have on/off, recalibrate, press-to-select and middle-button scrolling on Fn+F8, Ctrl+Fn+F8, Alt+Fn+F8 and Shift+Fn+F8 (Alt alone reaches the host: on Windows it may open a menu bar); the rate keys aren't bound. The stick's thread carries them out: starting takes about a second, recalibrating about 350 ms. The buttons go off with the stick.
- **Unused:** after `CONFIG_RETHINK_TRACKPOINT_IDLE_OFF_MIN` (10) minutes with neither the stick nor a key used, the stick is powered off; the next key press starts it again (`tp stat`: the last start's time). A stick switched off with Fn+F8 stays off until Fn+F8.
- **Calibration** is the stick's own (TrackPoint specification, section 2.2.3): the self-test sets the zero point, so the stick must not be touched in the second after power-on. Drift correction then resets it whenever the force stays within ±0.8 g for about half a second (hands off; `e2 81 5f` sets the time in 107 ms steps, default 5). A wrong zero, from a finger on the stick at power-on, so corrects itself once the stick is let go. `e2 51` (Ctrl+Fn+F8) sets the zero point to the present forces at once. The specification has the stick acknowledge only after the recalibration (about 310 ms); the T420 stick acknowledged after 2 ms, so the driver waits 350 ms either way before the next command.
- **Bluetooth:** the keyboard now also describes a mouse. If the pointer doesn't move over Bluetooth, remove the keyboard on the computer and pair it again.

Build a keyboard with the shell, e.g. `.\build.ps1 -Shield rethink_t420 -Snippet rethink-usb-shell`:

| Command | Does |
| --- | --- |
| `tp on` | Powers the stick (or resets it with `ff`, or power cycles it if that gets no answer, as after `e2 44`) and starts pointing |
| `tp off` | Powers it off |
| `tp send <hex> [hex...]` | Sends bytes, showing each acknowledge (`fa`), then the reply. Pauses pointing and turns streaming off first (`f5`) |
| `tp read [s]` | Shows what the stick sends, one line per packet with its time in ms (default 5 s). While pointing: the packets instead of reporting them, then pointing again |
| `tp set [name [value]]` | Lists the settings with their defaults and ranges (`*`: changed, in flash), shows one, or sets one; a name may be shortened while unique, values may be hex (`0x80`). `tp set defaults` puts all back and deletes them from flash |
| `tp stat [clear]` | Bytes and errors both ways, packets reported, sent again and lost, the report rate now and as set (battery, USB power) and its changes, the last start's times (reset held, self-test), the wheel resolution multiplier the present host has set (16: smooth scrolling, 1: whole steps), the stick's clock period |

`ble conn` shows each Bluetooth connection's interval (how often the host lets the keyboard send: a report waits at most one interval), peripheral latency and timeout; `ble conn <min> [max [latency]]` asks the host for another interval, in 1.25 ms units (6 = 7.5 ms, the shortest). (latency + 1) × interval × 2 must stay under the 4 s supervision timeout, so intervals over 40 ms need a lower latency, e.g. `ble conn 400 400 0`. The host decides; Zephyr asks for 7.5-15 ms, latency 30, 5 s after connecting (`CONFIG_BT_PERIPHERAL_PREF_*`). A browser's polling-rate test counts reports a second, which is the stick's report rate as long as the interval is shorter. Measured: the hosts chose 15 ms (66 a second), so the stick's 80 reports a second (12.5 ms apart) sometimes share an interval; `ble conn 6` got 7.5 ms from a host.

`ble log` lists what happened to the connections since boot, with times: each connection (address, profile, interval), disconnection with its reason (0x08 supervision timeout: radio contact lost; 0x13 ended by the host; 0x15 host powering off or sleeping; 0x3e failed to be established), encryption (security level, or the error), new connection parameters, and USB unplugged, powered or connected. It starts with the uptime and the cause of the last reset (watchdog, software, power-on or brown-out, USB or a key waking it from System OFF, ...), so a keyboard that rebooted shows it, and what was recorded just before (`CONFIG_RETHINK_RESET_DIAG`, kept in the last 256 bytes of RAM, which the linker leaves unused and a reset keeps):

- **Fatal error** (fault, assert, stack overflow): the reason, the thread (or an interrupt), its pc and lr and the code addresses on its stack. The board then reboots at once (`software`); Zephyr would otherwise halt until the watchdog reset it 8 s later.
- **Stall:** the system work queue, which feeds the watchdog, ran nothing for 4 s. A timer interrupt records the work queue's state and the object it waits on, its code addresses, and the thread running instead with its pc, lr and code addresses; the watchdog resets the board a few seconds later. A watchdog reset with nothing recorded means the timer interrupt didn't run: the system timer stopped, or interrupts took all the time.

Look the addresses up with the `.elf` of the build that wrote the record: the record carries that build's end of code, and `ble log` names the file, `out\elf\<artifact>-<that address>.elf`. E.g. in the build container (`.\build.ps1 -Shell`): `/opt/zephyr-sdk-*/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-addr2line -f -p -e out/elf/rethink_t420-rethink-usb-shell-0007a2c4.elf 0x0002a1b3`. Builds without the shell keep no record, but a fatal error reboots them at once (`CONFIG_RESET_ON_FATAL_ERROR`, set in `rethink_common/Kconfig.defconfig`) instead of halting until the watchdog. `diag crash` and `diag stall` test both records. `kernel thread stacks` shows how much of each thread's stack was ever used. `ble log clear` empties the event list. The same events appear in the log as `rethink_ble`.

Bytes to try: `e1` asks for the TrackPoint ID (`fa`, then the maker: 01 IBM/Lenovo, 02 ALPS, 03 Elan, 04 NXP, and its firmware), `f2` the PS/2 device ID (`fa 00`), `ff` resets (`fa aa 00`), `f4` starts streaming (3-byte packets while the stick moves, see `tp read`), `f5` stops it.

The stick's own settings, from the [TrackPoint System Version 4.0 Engineering Specification](https://web.mit.edu/bbaren/Public/ykt3eext.pdf) (YKT3E; a T420 stick answers `e1` with 01 0e, the ID of that version). `e2 80 XX` reads a RAM location, `e2 81 XX YY` writes YY to it, `e2 47 XX MM` flips the bits MM of flag byte XX. The values survive `ff` but not a power cycle, so the driver writes them after each start (Settings above: `tp set` names in the table).

| Setting (`tp set` name) | Command | Default (hex) | Effect |
| --- | --- | --- | --- |
| Sensitivity (`sensitivity`) | `e2 81 4a XX` | 80 (×1.0) | Force scale before the transfer function, ×0 to ×1.99 |
| Negative inertia (`inertia`) | `e2 81 4d XX` | 06 | Boosts changes of force, damps the start of a movement; 0 off |
| Upper plateau speed (`speed`) | `e2 81 60 XX` | 61 | Speed under a firm push, about 22 + 4.3 × XX counts/s (439 at the default) |
| Sample rate (`rate`, `rate_usb`) | `f3 XX` | 64 (100/s) | Reports per second: 0a, 14, 28, 3c, 50, 64, c8 (10-200); force measured 80/s at 10-40 and 80, 120/s at 60, 100/s at 100 and 200. See Report rate above |
| Drift time (`drift_time`) | `e2 81 5f XX` | 05 | Hands-off time before drift correction, in 107 ms steps |
| Drift correction off | `e2 47 23 80` | on | |
| Press-to-select (`press_to_select`) | `e2 47 2c 01` | off | Clicks by pressing the stick; thresholds and timing at `5c`, `5a`, `5e`, `5d`, `57`, `58`, `59` |
| Middle button blocked | `e2 47 23 01` | no | |
| Axes | `e2 81 2c XX` | 00 | Bits 3-6: invert X, Y, Z, swap X and Y |
| Sampling off | `e2 47 20 10` | on | No force conversions, no movement; the buttons still report. 5 mA less from the battery than 80/s on the T420 |
| Recalibrate | `e2 51` | | Zero point to the present forces; acknowledged after about 310 ms (the T420 stick: 2 ms) |
| Power down | `e2 44` | | Until a hardware reset |
| Others | `e2 46`, `e2 25`, `e2 7f` | | ROM version, self-test result bits, power-on reset |

Measured on a T420 stick (ID 01, firmware 0e), pointing in the shell build: over USB 2505 bytes without an error, over Bluetooth 107 errors in 913 bytes (46 framing, 3 parity, 58 broken off). So the Bluetooth controller delayed the interrupt. At priority 0: 12 errors in 2673 bytes over Bluetooth, none in 13466 over USB. With packets asked for again: 12 errors in 3729 bytes, 11 packets sent again, none lost. The stick costs about 17.5 mA while on at 80/s (roadmap: Pointing stick power).

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
├── patches/zmk/                 Patches to ZMK, applied at build time (see Software stack)
└── snippets/                    rethink-usb-log (USB CDC ACM logging),
                                 rethink-usb-shell (shell and log on the same port),
                                 rethink-profiling (charging and low-battery cut-off off)
```

## Kconfig options

| Option | Default | Description |
| --- | --- | --- |
| `CONFIG_RETHINK_PAIRING_BUTTON` | y | Pairing button: tap = next profile, hold = clear profile, triple tap = unlock backlight (hardware tester) |
| `CONFIG_RETHINK_PAIRING_BUTTON_HOLD_MS` | 3000 | Hold time to clear (on release) |
| `CONFIG_RETHINK_PAIRING_BUTTON_RECOVERY_MS` | 10000 | Hold time to reboot into the bootloader, 0 = off |
| `CONFIG_RETHINK_POWER` | y | Power off (System OFF with everything off) and its reasons below; `power` shell commands |
| `CONFIG_RETHINK_LOW_BATTERY_OFF_MV` | 3300 | Power off on battery below this voltage, until USB; 0 = never |
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
| `CONFIG_RETHINK_BACKLIGHT_TAP_UNLOCK` | y on `rethink_hw_tester`, else n | Triple tap of the pairing button unlocks the backlight until reboot, bypassing KBD_BL_DTCT |
| `CONFIG_RETHINK_TRACKPOINT` | if `rethink,trackpoint` in DT | Pointing stick power, reset, PS/2 link and pointing |
| `CONFIG_RETHINK_TRACKPOINT_IDLE_OFF_MIN` | 10 | Minutes with neither the stick nor a key used before the stick is powered off; 0 = never |
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