#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

"""test-seek-scale.py - verify the cumulative seek scale of a running player.

The player must already be playing (kms-play or kms-play-next). This script
injects real key events through kms-inject, then reads the player log and
checks, for every accepted press, that the applied step is the expected rung of
the scale:

    1, 10, 30, 60, 120, 240 s, then 240 s (clamped)

It also checks the two reset rules: 1.5 s of silence returns to 1 s, and a
change of direction returns to 1 s.

Every step is validated twice: from the "seek: rung n/6 = X s" line (what the
player decided) and from the difference between consecutive target positions
(what the pipeline was actually asked to do). The second one is the real
proof, because it is measured on the resulting positions.

Usage: test-seek-scale.py [--log PATH] [--inject PATH] [--pause SECONDS]
Exit code 0 when every row matches.
"""
import argparse
import os
import re
import subprocess
import sys
import time

DEFAULT_INJECT = "/usr/local/bin/kms-inject"


def default_log():
    """Player log of the user actually running the player.

    The test often runs as root (it needs /dev/uinput), while the player runs as
    the desktop user. ~ would then resolve to /root and read a stale file left
    by an older root-run test, so every candidate is considered and the most
    recently written one wins."""
    candidates = [os.path.expanduser("~/.cache/kms-play/last.log"),
                  "/home/armbian/.cache/kms-play/last.log"]
    existing = [path for path in candidates if os.path.exists(path)]
    if not existing:
        return candidates[0]
    return max(existing, key=os.path.getmtime)


RUNG_RE = re.compile(r"\[([0-9.]+)\] seek: rung (\d+)/(\d+) = ([0-9.]+) s")
SEEK_RE = re.compile(r"\[([0-9.]+)\] seek [a-z]+ (-?[0-9.]+)s -> "
                     r"(?:(\d+):)?(\d+):(\d+) \(")


def log_size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def read_new_lines(path, offset):
    with open(path, "rb") as handle:
        handle.seek(offset)
        return handle.read().decode("utf-8", "replace").splitlines()


def press(inject, key, delay=0.15, hold=40):
    """One discrete tap: press, hold 40 ms, release. Short enough that the
    player's own autorepeat (300 ms delay) never kicks in."""
    subprocess.run([inject, key, "--delay", str(delay), "--hold", str(hold)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=10, check=False)


def taps(inject, key, count, spacing=450, delay=0.15, hold=40):
    """N taps emitted from ONE uinput device, `spacing` ms apart (press to press).

    One device per tap costs about 0.8 s (node creation, udev, settle), so a
    loop of press() calls spaces the presses further apart than the player's
    1 s scale-reset window. The scale would then restart on every press and a
    correct player would look broken."""
    subprocess.run([inject, key, "--taps", str(count),
                    "--tap-interval", str(spacing),
                    "--delay", str(delay), "--hold", str(hold)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=30, check=False)


def parse(lines):
    """Pair every rung decision with the seek line that follows it."""
    events = []
    pending_rung = None
    for line in lines:
        match = RUNG_RE.search(line)
        if match:
            pending_rung = (float(match.group(4)), int(match.group(2)))
            continue
        match = SEEK_RE.search(line)
        if match:
            hours = int(match.group(3) or 0)
            target = hours * 3600 + int(match.group(4)) * 60 + int(match.group(5))
            delta = float(match.group(2))
            events.append({
                "rung": pending_rung[0] if pending_rung else None,
                "rung_index": pending_rung[1] if pending_rung else None,
                "delta": delta,
                "target": target,
            })
            pending_rung = None
    return events


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", default=None)
    parser.add_argument("--inject", default=DEFAULT_INJECT)
    parser.add_argument("--pause", type=float, default=1.5)
    args = parser.parse_args()
    if not args.log:
        args.log = default_log()

    if not os.path.exists(args.log):
        print("no player log at %s - is the player running?" % args.log)
        return 2

    expected = [1.0, 10.0, 30.0, 60.0, 120.0, 240.0, 240.0, 1.0, -1.0]
    labels = ["press 1", "press 2", "press 3", "press 4", "press 5", "press 6",
              "press 7 (clamped)", "press 8 (after %.1fs pause)" % args.pause,
              "press 9 (LEFT)"]

    print("reading log %s" % args.log)
    offset = log_size(args.log)
    print("injecting: 7 x RIGHT, %.1fs pause, RIGHT, LEFT" % args.pause)
    taps(args.inject, "RIGHT", 7)
    time.sleep(args.pause)
    press(args.inject, "RIGHT")
    time.sleep(0.45)
    press(args.inject, "LEFT")
    time.sleep(0.8)

    events = parse(read_new_lines(args.log, offset))
    if len(events) < len(expected):
        print("only %d seek events observed, expected %d" % (len(events), len(expected)))

    print("")
    print("%-28s %-9s %-9s %-10s %-14s %s"
          % ("press", "rung", "step", "target", "d target", "verdict"))
    print("(d target is keyframe-aligned: seeks use KEY_UNIT, so it can differ "
          "from step by a few seconds)")
    previous_target = None
    failures = 0
    for index, expected_step in enumerate(expected):
        label = labels[index]
        if index >= len(events):
            print("%-28s %s" % (label, "MISSING"))
            failures += 1
            continue
        event = events[index]
        jump = None if previous_target is None else event["target"] - previous_target
        previous_target = event["target"]
        step_ok = (event["rung"] is not None and
                   abs(event["rung"] - abs(expected_step)) < 1e-6 and
                   abs(event["delta"] - expected_step) < 1e-6)
        # The step itself must be exact; the difference between consecutive
        # target positions can drift by up to one GOP because every seek snaps
        # to a keyframe (KEY_UNIT). 10 s covers the GOP of these test files
        # with HDR/slow-encode chapters; the step itself must still be exact.
        jump_ok = (jump is None and index == 0) or (jump is not None and
                                                   abs(jump - expected_step) <= 10)
        ok = step_ok and jump_ok
        if not ok:
            failures += 1
        print("%-28s %-9s %-9s %-10s %-14s %s"
              % (label,
                 "%.1f" % event["rung"] if event["rung"] is not None else "?",
                 "%+.1f" % event["delta"],
                 "%d s" % event["target"],
                 "%+d" % jump if jump is not None else "-",
                 "OK" if ok else "FAIL"))

    print("")
    if failures:
        print("FAIL: %d unexpected result(s)" % failures)
        return 1
    print("PASS: 6 rungs of 1/10/30/60/120/240 s, reset after %.1fs, "
          "reset on direction change" % args.pause)
    return 0


if __name__ == "__main__":
    sys.exit(main())
