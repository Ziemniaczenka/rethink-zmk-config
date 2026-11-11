#!/usr/bin/env python3
# Copyright (c) 2026 Ziemniaczenka
# SPDX-License-Identifier: MIT
"""Watch a keyboard's matrix: which keys are pressed, stuck or chattering.

Reads the rethink_scanner firmware's USB log, as matrix_scan.py does, and
draws the adapter's matrix in the terminal: rows SENSE0-7 and the direct keys
(HOTKEY, PWRSWITCH), columns DRV0-17. For a keyboard whose keys misbehave (one
that registers at the lightest touch, one that never does, chatter) and for
checking every key of a new one.

    python scripts/matrix_view.py                        # live
    python scripts/matrix_view.py --shield rethink_t420  # with key names
    python scripts/matrix_view.py --log out/t480_iso_scan.log --shield rethink_t420

Setup as for matrix_scan.py: rethink_scanner with the rethink-usb-shell
snippet, the keyboard connected, USB in, no terminal open on the port. The
zmk log must stay at debug level (not after "log enable inf zmk"). The live
log is also written to out/matrix_view.log, for --log later. The view is 112
characters wide.

--shield takes key names from a shield's matrix transform and base layer; one
with the same matrix will do (the T480's main block matches rethink_t420).
With names, the view also lists the keys not pressed yet, and a press at a
position outside the shield's layout shows as "?". Without, "*" marks a
position that has been pressed.

Colours: green background = pressed now (red background: for longer than
--stuck seconds), red = held that long before, or already held when the log
started, yellow = chatter (a press within --bounce ms of the key's last
release, or shorter than that), green = pressed before, dim = not yet. Live:
c clears the counts, q quits.

The firmware blocks ghost keys (in a diodeless matrix, three pressed corners
of a rectangle make the fourth read pressed), so they don't show here.
"""

import argparse
import os
import re
import shutil
import sys
import time
from pathlib import Path

from matrix_scan import ANSI, COLS, DIRECT_ROW, EVENT, ROWS, Console, find_port, line_name, serial

REPO = Path(__file__).resolve().parent.parent

# Zephyr log time stamp "[00:04:24.070,123]": h, min, s, ms, us
STAMP = re.compile(r"\[(\d+):(\d\d):(\d\d)\.(\d{3}),(\d{3})\]")

CELL = 6  # characters per column
SHOWN_EVENTS = 8

RESET = "\x1b[0m"
STYLE = {
    "stuck_now": "\x1b[97;41m",  # white on red
    "down": "\x1b[30;42m",  # black on green
    "problem": "\x1b[91m",  # red
    "chatter": "\x1b[93m",  # yellow
    "seen": "\x1b[32m",  # green
    "never": "\x1b[2m",  # dim
}
# Without colours (output to a file or pipe): a mark after the name
MARK = {"stuck_now": "#", "down": "#", "problem": "!", "chatter": "!", "seen": "+", "never": " "}


def seconds(line):
    m = STAMP.search(line)
    if not m:
        return None
    h, mi, s, ms, us = map(int, m.groups())
    return h * 3600 + mi * 60 + s + ms / 1e3 + us / 1e6


def clock(t):
    h, rest = divmod(t, 3600)
    mi, s = divmod(rest, 60)
    return f"{int(h):02}:{int(mi):02}:{s:06.3f}"


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def short(binding):
    """'kp LSHFT' -> 'LSHFT', 'kp N1' -> '1', 'kp C_AC_BACK' -> 'BACK', 'mo FN' -> 'FN'."""
    parts = binding.split()
    name = parts[-1] if len(parts) > 1 else parts[0]
    name = re.sub(r"^(C_AC_|C_|K_)", "", name)
    name = re.sub(r"^N(\d)$", r"\1", name)
    return name[: CELL - 1]


def shield_names(shield):
    """{(row, col): name} from a shield's matrix transform and base layer."""
    d = REPO / "boards" / "shields" / shield
    if not d.is_dir():
        sys.exit(f"No shield {shield} in {d.parent}")
    files = sorted(d.glob("*.overlay")) + sorted(d.glob("*.dtsi"))
    dts = "\n".join(strip_comments(p.read_text(encoding="utf-8")) for p in files)
    m = re.search(r'"zmk,matrix-transform".*?map\s*=\s*<(.*?)>', dts, re.S)
    if not m:
        sys.exit(f"No matrix transform in {d}")
    positions = [(int(r), int(c)) for r, c in re.findall(r"RC\(\s*(\d+)\s*,\s*(\d+)\s*\)", m.group(1))]

    keymap = strip_comments((d / f"{shield}.keymap").read_text(encoding="utf-8"))
    m = re.search(r'"zmk,keymap".*?bindings\s*=\s*<(.*?)>', keymap, re.S)
    if not m:
        sys.exit(f"No keymap layer in {shield}.keymap")
    bindings = [b.strip() for b in m.group(1).split("&")[1:]]
    if len(bindings) != len(positions):
        sys.exit(f"{shield}: {len(positions)} positions in the transform, "
                 f"{len(bindings)} bindings in the base layer")
    return {p: short(b) for p, b in zip(positions, bindings)}


class Key:
    def __init__(self):
        self.down = False
        self.since = None  # time of the last press or release
        self.released = None  # time of the last release
        self.presses = 0
        self.chatter = 0
        self.longest = 0.0  # longest finished hold
        self.held_at_start = False


class Matrix:
    def __init__(self, names, bounce_s, stuck_s):
        self.names = names
        self.bounce, self.stuck = bounce_s, stuck_s
        self.keys = {}  # (row, col): Key
        self.events = []  # the last few (time, row, col, pressed)
        self.count = 0
        self.now = 0.0  # firmware time of the last time-stamped line

    def feed(self, line):
        """Takes one log line; True if it was a key event."""
        t = seconds(line)
        if t is not None:
            self.now = t
        m = EVENT.search(line)
        if not m:
            return False
        r, c, pressed = int(m.group(1)), int(m.group(2)), m.group(3) == "true"
        k = self.keys.setdefault((r, c), Key())
        t = self.now
        if pressed:
            if k.released is not None and t - k.released < self.bounce:
                k.chatter += 1
            k.presses += 1
            k.down, k.since = True, t
        else:
            if not k.down:
                k.held_at_start = True
            else:
                held = t - k.since
                k.longest = max(k.longest, held)
                if held < self.bounce:
                    k.chatter += 1
            k.down, k.since, k.released = False, t, t
        self.events.append((t, r, c, pressed))
        del self.events[:-SHOWN_EVENTS]
        self.count += 1
        return True

    def clear(self):
        """Counts back to zero; keys held now stay held."""
        self.keys = {p: k for p, k in self.keys.items() if k.down}
        for k in self.keys.values():
            k.presses, k.chatter, k.longest = 0, 0, 0.0
            k.held_at_start, k.released = False, None
        self.events.clear()
        self.count = 0

    def held(self, k, now):
        return now - k.since if k.down else 0.0

    def state(self, k, now):
        if k is None:
            return "never"
        if k.down:
            return "stuck_now" if self.held(k, now) >= self.stuck else "down"
        if k.held_at_start or k.longest >= self.stuck:
            return "problem"
        if k.chatter:
            return "chatter"
        return "seen" if k.presses else "never"

    def label(self, r, c):
        name = self.names.get((r, c))
        return f"{name} ({line_name(r, c)})" if name else line_name(r, c)

    def problems(self, now):
        out = []
        for (r, c), k in sorted(self.keys.items()):
            what = []
            held = self.held(k, now)
            if held >= self.stuck:
                what.append(f"held for {held:.1f} s now")
            elif k.longest >= self.stuck:
                what.append(f"held for {k.longest:.1f} s once")
            if k.held_at_start:
                what.append("already held when the log started")
            if k.chatter:
                what.append(f"{k.chatter} chatter")
            if self.names and (r, c) not in self.names:
                what.append("outside the shield's layout")
            if what:
                out.append(f"{self.label(r, c)}: " + ", ".join(what))
        return out

    def untouched(self):
        return [n for p, n in self.names.items()
                if p not in self.keys or not (self.keys[p].presses or self.keys[p].held_at_start)]


def cell(m, r, c, now, color):
    k = m.keys.get((r, c))
    name = m.names.get((r, c))
    if name is None:
        if k is not None:
            name = "?" if m.names else "*"
        elif r == DIRECT_ROW and c > 1:
            name = ""
        else:
            name = "."
    st = m.state(k, now)
    if color:
        return f"{STYLE[st]}{name:^{CELL - 1}}{RESET} "
    return f"{name[: CELL - 2]:>{CELL - 2}}{MARK[st]} "


def wrap(prefix, words, width, max_lines=3):
    lines, line = [], prefix
    for w in words:
        if len(line) + len(w) + 1 > width:
            lines.append(line)
            if len(lines) == max_lines:
                lines[-1] += " ..."
                return lines
            line = " " * len(prefix)
        line += " " + w
    lines.append(line)
    return lines


def render(m, now, color, title):
    width = max(shutil.get_terminal_size((120, 40)).columns - 1, 60)
    out = [title, ""]
    out.append("    " + "".join(f"{'D' + str(c):^{CELL - 1}} " for c in range(COLS)))
    for r in range(ROWS):
        label = "dir" if r == DIRECT_ROW else f"S{r}"
        out.append(f"{label:<4}" + "".join(cell(m, r, c, now, color) for c in range(COLS)))
    out.append("")
    if color:
        out.append(f"{STYLE['down']} pressed {RESET} {STYLE['stuck_now']} held {m.stuck:g} s+ {RESET} "
                   f"{STYLE['problem']}held {m.stuck:g} s+ before / at start{RESET}  "
                   f"{STYLE['chatter']}chatter{RESET}  {STYLE['seen']}pressed before{RESET}  "
                   f"{STYLE['never']}not yet{RESET}")
    else:
        out.append(f"Marks: # pressed, ! held {m.stuck:g} s+ / at start / chatter, + pressed before")
    out.append("")
    problems = m.problems(now)
    out.append("Problems:" + ("" if problems else " none"))
    out += ["  " + p for p in problems]
    if m.names:
        untouched = m.untouched()
        out.append("")
        out += wrap(f"Not pressed yet ({len(untouched)}):", untouched, width) if untouched \
            else ["Every key of the layout pressed"]
    out.append("")
    out.append(f"Last events ({m.count} in all):")
    for t, r, c, pressed in m.events:
        out.append(f"  {clock(t)}  {'down' if pressed else 'up  '}  {m.label(r, c)}")
    return out


def enable_colors():
    """ANSI colours if stdout is a terminal (on Windows, VT mode switched on)."""
    if not sys.stdout.isatty():
        return False
    if os.name == "nt":
        import ctypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        # ENABLE_VIRTUAL_TERMINAL_PROCESSING
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))
    return True


def replay(args, m):
    with open(args.log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m.feed(ANSI.sub("", line).strip("\r\n "))
    title = f"{args.log}: up to {clock(m.now)}; held times count to the end of the log"
    print("\n".join(render(m, m.now, enable_colors(), title)))


def live(args, m):
    port = find_port(args.port)
    link = serial.Serial(port, 115200, timeout=0.05)
    args.save.parent.mkdir(parents=True, exist_ok=True)
    save = open(args.save, "w", encoding="utf-8")
    color = enable_colors()
    buf = b""
    stamp_pc = time.monotonic()  # PC time when m.now last changed
    drawn, dirty = 0.0, True
    sys.stdout.write("\x1b[?1049h\x1b[?25l")  # own screen, no cursor
    try:
        with Console() as con:
            while True:
                buf += link.read(link.in_waiting or 1)
                *lines, buf = buf.split(b"\n")
                for raw in lines:
                    line = ANSI.sub("", raw.decode("utf-8", "replace")).strip("\r ")
                    if not line:
                        continue
                    save.write(line + "\n")
                    before = m.now
                    dirty |= m.feed(line)
                    if m.now != before:
                        stamp_pc = time.monotonic()
                ch = con.key()
                if ch in ("q", "Q", "\x1b"):
                    break
                if ch in ("c", "C"):
                    m.clear()
                    dirty = True
                t = time.monotonic()
                if dirty or t - drawn > 0.5:
                    now = m.now + (t - stamp_pc)
                    title = f"ReThink matrix on {port}, log to {args.save}. c: clear counts, q: quit"
                    text = "\x1b[K\n".join(render(m, now, color, title))
                    sys.stdout.write(f"\x1b[H{text}\x1b[K\n\x1b[J")
                    sys.stdout.flush()
                    drawn, dirty = t, False
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.write("\x1b[?25h\x1b[?1049l")
        sys.stdout.flush()
        link.close()
        save.close()
    print(f"Log: {args.save} (python scripts/matrix_view.py --log {args.save})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--shield", help="key names from this shield's transform and base layer")
    ap.add_argument("--log", type=Path, help="show a saved log instead of the board")
    ap.add_argument("--port", help="the board's serial port (default: found by ZMK's USB IDs)")
    ap.add_argument("--save", type=Path, default=REPO / "out" / "matrix_view.log",
                    help="where the live log goes (default: out/matrix_view.log)")
    ap.add_argument("--stuck", type=float, default=3.0, help="seconds held that count as stuck (3)")
    ap.add_argument("--bounce", type=float, default=30.0, help="chatter window, ms (30)")
    args = ap.parse_args()

    names = shield_names(args.shield) if args.shield else {}
    m = Matrix(names, args.bounce / 1000.0, args.stuck)
    if args.log:
        replay(args, m)
    else:
        live(args, m)


if __name__ == "__main__":
    main()
