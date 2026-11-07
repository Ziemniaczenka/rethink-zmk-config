#!/usr/bin/env python3
# Copyright (c) 2026 Ziemniaczenka
# SPDX-License-Identifier: MIT
"""Map a keyboard's matrix with the rethink_scanner firmware.

Prompts for each key, reads the row and column of the press from the board's
USB log, and writes a matrix transform and a keymap skeleton for a shield.

Setup: flash rethink_scanner (rethink-usb-shell snippet), connect the keyboard,
plug in USB, and close any terminal on the board's serial port. Then:

    python scripts/matrix_scan.py --layout my_keyboard.txt
    python scripts/matrix_scan.py --name l420

Layout file: one line per physical row, keys left to right, separated by
spaces. A key is a ZMK key code (ESC, F1, LCTRL, ...) or a label in brackets
for a key without one ([FN], [TP_LEFT]); the keymap gets &none for those.
Lines starting with # are comments. Without a layout file, press the keys row
by row, left to right, and end each row with Enter; they get labels like
[r2k5].

The scanned keyboard sends nothing to the host. Control the script from the
PC's keyboard: Enter skips the prompted key (without a layout: next row),
u undoes the last key, q finishes. Results so far are written either way.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Needs pyserial: python -m pip install pyserial")

if os.name == "nt":
    import msvcrt
else:
    import select
    import termios
    import tty

ZMK_VID, ZMK_PID = 0x1D50, 0x615E

# rethink_adapter_matrix.dtsi: rows SENSE0-7 plus the direct keys, columns DRV0-17
ROWS, COLS = 9, 18
DIRECT_ROW = 8
DIRECT = {0: "HOTKEY", 1: "PWRSWITCH"}

# ZMK physical_layouts.c: "Row: 3, col: 15, position: 69, pressed: true" at
# debug level, and its warning for a press outside the transform
EVENT = re.compile(r"[Rr]ow: (\d+), col: (\d+)(?:, position: \d+)?, pressed: (true|false)")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def line_name(r, c):
    if r == DIRECT_ROW:
        return DIRECT.get(c, f"direct key {c}")
    return f"SENSE{r} x DRV{c}"


def find_port(port):
    if port:
        return port
    ports = list_ports.comports()
    zmk = [p for p in ports if p.vid == ZMK_VID and p.pid == ZMK_PID]
    if len(zmk) == 1:
        return zmk[0].device
    listing = "\n".join(f"  {p.device}  {p.description} [{p.hwid}]" for p in ports) or "  (none)"
    reason = "Several ZMK serial ports" if zmk else "No ZMK serial port"
    sys.exit(f"{reason}; pass --port. Serial ports:\n{listing}")


class Link:
    """Key presses from the scanner's log; every line also goes to a log file."""

    def __init__(self, port, log_path):
        # pyserial sets DTR on open; the shell sends nothing before it
        self.port = serial.Serial(port, 115200, timeout=0.05)
        self.log = open(log_path, "w", encoding="utf-8")
        self.buf = b""

    def close(self):
        self.port.close()
        self.log.close()

    def presses(self):
        """Presses received since the last call, as (row, col)."""
        self.buf += self.port.read(self.port.in_waiting or 1)
        *lines, self.buf = self.buf.split(b"\n")
        found = []
        for raw in lines:
            line = ANSI.sub("", raw.decode("utf-8", "replace")).strip("\r ")
            if not line:
                continue
            self.log.write(line + "\n")
            m = EVENT.search(line)
            if m and m.group(3) == "true":
                found.append((int(m.group(1)), int(m.group(2))))
        return found

    def drain(self):
        for _ in range(10):
            self.presses()


class Console:
    """Single keys from the PC's keyboard, without waiting for Enter."""

    def __enter__(self):
        if os.name != "nt" and sys.stdin.isatty():
            self.fd = sys.stdin.fileno()
            self.saved = termios.tcgetattr(self.fd)
            tty.setcbreak(self.fd)
        return self

    def __exit__(self, *exc):
        if getattr(self, "saved", None):
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)

    def key(self):
        if os.name == "nt":
            if not msvcrt.kbhit():
                return None
            ch = msvcrt.getwch()
            if ch in ("\x00", "\xe0"):  # arrow and function keys: ignored
                msvcrt.getwch()
                return None
            return ch
        if select.select([sys.stdin], [], [], 0)[0]:
            return sys.stdin.read(1)
        return None


def wait(link, con):
    """('press', r, c), ('enter',), ('undo',) or ('quit',)."""
    while True:
        ch = con.key()
        if ch in ("\r", "\n"):
            return ("enter",)
        if ch in ("u", "U", "\b"):
            return ("undo",)
        if ch in ("q", "Q", "\x1b"):
            return ("quit",)
        presses = link.presses()
        if presses:
            return ("press",) + presses[0]


class Scan:
    def __init__(self):
        self.keys = []  # {"name", "layout_row", "row", "col"}
        self.skipped = []

    def owner(self, r, c):
        return next((k["name"] for k in self.keys if (k["row"], k["col"]) == (r, c)), None)

    def add(self, name, layout_row, r, c):
        self.keys.append({"name": name, "layout_row": layout_row, "row": r, "col": c})
        print(f"row {r}, col {c} ({line_name(r, c)})")

    def take(self, ev, name, layout_row):
        """Record a press; False if the position already has a key."""
        _, r, c = ev
        other = self.owner(r, c)
        if other:
            print(f"row {r}, col {c} is already {other}, ignored", flush=True)
            return False
        self.add(name, layout_row, r, c)
        return True


def read_layout(path):
    rows = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            rows.append(line.split())
    if not rows:
        sys.exit(f"{path}: no keys")
    return rows


def scan_layout(layout, link, con, scan):
    """Prompt for every key of the layout. False if the user quit."""
    flat = [(i, name) for i, row in enumerate(layout) for name in row]
    done = []  # per prompted key: "key" or "skip", for undo
    i = 0
    while i < len(flat):
        layout_row, name = flat[i]
        print(f"[{i + 1}/{len(flat)}] {name}", end=": ", flush=True)
        while True:
            ev = wait(link, con)
            if ev[0] == "press":
                if scan.take(ev, name, layout_row):
                    done.append("key")
                    i += 1
                    break
                print(f"{name} (press it again, or Enter to skip)", end=": ", flush=True)
            elif ev[0] == "enter":
                print("skipped")
                scan.skipped.append(name)
                done.append("skip")
                i += 1
                break
            elif ev[0] == "undo":
                if done:
                    if done.pop() == "key":
                        undone = scan.keys.pop()["name"]
                    else:
                        undone = scan.skipped.pop()
                    print(f"undid {undone}")
                    i -= 1
                else:
                    print("nothing to undo")
                break
            else:
                print("quit")
                return False
    return True


def scan_free(link, con, scan, first_row, label, one_row):
    """Keys in any number, named by label(row, index); Enter ends a row."""
    row = first_row
    count = 0
    if one_row:
        print("Press each key the layout missed, one at a time. Enter or q: finish.")
    else:
        print(f"Row {row + 1}: press the keys left to right, then Enter. q: finish.")
    while True:
        ev = wait(link, con)
        if ev[0] == "press":
            name = label(row, count)
            print(f"  {name}", end=": ")
            if scan.take(ev, name, row):
                count += 1
        elif ev[0] == "enter":
            if one_row:
                return
            if count:
                row += 1
                count = 0
                print(f"Row {row + 1}: press the keys left to right, then Enter. q: finish.")
        elif ev[0] == "undo":
            if scan.keys and scan.keys[-1]["layout_row"] >= first_row:
                k = scan.keys.pop()
                print(f"  undid {k['name']}")
                row = k["layout_row"]
                count = sum(1 for x in scan.keys if x["layout_row"] == row)
            else:
                print("  nothing to undo")
        else:
            return


def rows_of(scan):
    rows = {}
    for k in scan.keys:
        rows.setdefault(k["layout_row"], []).append(k)
    return [rows[r] for r in sorted(rows)]


def binding(name):
    return "&none" if name.startswith("[") else f"&kp {name}"


def table(rows, cell, indent):
    """Rows of cells, aligned in columns, each row under a comment with the key names."""
    cells = [[cell(k) for k in row] for row in rows]
    width = max(len(c) for row in cells for c in row)
    out = []
    for row, row_cells in zip(rows, cells):
        out.append(indent + "// " + " ".join(k["name"] for k in row))
        out.append(indent + " ".join(c.ljust(width) for c in row_cells).rstrip())
    return "\n".join(out)


def write_outputs(scan, name, layout_path, out_dir):
    rows = rows_of(scan)
    label = re.sub(r"[^a-z0-9_]", "_", name.lower())
    source = f" from {Path(layout_path).name}" if layout_path else ""
    header = (f"// Generated by scripts/matrix_scan.py{source}.\n"
              f"// Rows 0-7: SENSE0-7, row 8: HOTKEY (col 0), PWRSWITCH (col 1). Columns: DRV0-17.\n")

    transform = (header + "\n#include <dt-bindings/zmk/matrix_transform.h>\n\n/ {\n"
                 f"    {label}_transform: {label}_transform {{\n"
                 '        compatible = "zmk,matrix-transform";\n'
                 f"        rows = <{ROWS}>;\n        columns = <{COLS}>;\n        map = <\n"
                 + table(rows, lambda k: f"RC({k['row']},{k['col']})", " " * 12)
                 + "\n        >;\n    };\n};\n")

    keymap = (header + "\n#include <behaviors.dtsi>\n#include <dt-bindings/zmk/keys.h>\n\n/ {\n"
              "    keymap {\n        compatible = \"zmk,keymap\";\n\n        default_layer {\n"
              "            bindings = <\n"
              + table(rows, lambda k: binding(k["name"]), " " * 16)
              + "\n            >;\n        };\n    };\n};\n")

    data = {"name": name, "layout": str(layout_path) if layout_path else None,
            "keys": scan.keys, "skipped": scan.skipped}

    files = {f"{label}_transform.dtsi": transform, f"{label}.keymap": keymap,
             f"{label}_scan.json": json.dumps(data, indent=2) + "\n"}
    for fname, text in files.items():
        (out_dir / fname).write_text(text, encoding="utf-8", newline="\n")
    return [out_dir / f for f in files]


def summary(scan, layout):
    if layout:
        listed = sum(1 for k in scan.keys if k["layout_row"] < len(layout))
        extra = len(scan.keys) - listed
        print(f"\n{listed} of {sum(len(r) for r in layout)} layout keys mapped"
              + (f", {extra} extra" if extra else ""))
    else:
        print(f"\n{len(scan.keys)} keys mapped")
    if scan.skipped:
        print("Skipped: " + " ".join(scan.skipped))
    sense = sorted({k["row"] for k in scan.keys if k["row"] != DIRECT_ROW})
    drv = sorted({k["col"] for k in scan.keys if k["row"] != DIRECT_ROW})
    direct = sorted({k["col"] for k in scan.keys if k["row"] == DIRECT_ROW})
    print("Rows used:    " + (" ".join(f"SENSE{r}" for r in sense) or "-"))
    print("Columns used: " + (" ".join(f"DRV{c}" for c in drv) or "-"))
    unused = [f"DRV{c}" for c in range(COLS) if c not in drv]
    if unused:
        print("Columns unused: " + " ".join(unused))
    if direct:
        print("Direct keys:  " + " ".join(DIRECT.get(c, str(c)) for c in direct))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--layout", type=Path, help="keys per physical row (see the script's header)")
    ap.add_argument("--name", help="output file names (default: the layout file's name)")
    ap.add_argument("--port", help="the board's serial port (default: found by ZMK's USB IDs)")
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "out",
                    help="output directory (default: out/)")
    args = ap.parse_args()

    layout = read_layout(args.layout) if args.layout else None
    name = args.name or (args.layout.stem if args.layout else "keyboard")
    args.out.mkdir(parents=True, exist_ok=True)
    label = re.sub(r"[^a-z0-9_]", "_", name.lower())

    port = find_port(args.port)
    try:
        link = Link(port, args.out / f"{label}_scan.log")
    except serial.SerialException as e:
        sys.exit(f"Cannot open {port}: {e}\nClose any terminal on it first.")

    print(f"Reading {port}. Press keys on the scanned keyboard; on this PC: Enter = "
          f"{'skip' if layout else 'next row'}, u = undo, q = finish.\n")
    scan = Scan()
    try:
        with Console() as con:
            link.drain()
            if layout:
                if scan_layout(layout, link, con, scan):
                    scan_free(link, con, scan, len(layout),
                              lambda row, i: f"[extra{i + 1}]", one_row=True)
            else:
                scan_free(link, con, scan, 0, lambda row, i: f"[r{row + 1}k{i + 1}]",
                          one_row=False)
    except KeyboardInterrupt:
        print("\nInterrupted")
    except serial.SerialException as e:
        print(f"\nLost {port}: {e}")
    finally:
        link.close()

    if not scan.keys:
        print("No keys mapped; nothing written. The raw log is in " + str(args.out / f"{label}_scan.log"))
        return
    summary(scan, layout)
    for f in write_outputs(scan, name, args.layout, args.out):
        print(f"Wrote {f}")
    print(f"Raw log: {args.out / (label + '_scan.log')}")


if __name__ == "__main__":
    main()
