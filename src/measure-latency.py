#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

"""measure-latency.py - keyboard to visible effect latency of the KMS player.

Method: the uinput injector (kms-inject) timestamps its write on CLOCK_MONOTONIC.
We then sample /sys/kernel/debug/dri/0/state right after and detect the first
sample where the OSD plane carries a framebuffer. The difference is the
end-to-end latency: uinput -> evdev -> kms-keys -> player -> cairo -> kms-subd
-> SetPlane.

Requires root (reading the debugfs state file) or a kernel built with the DRM
state dump readable by the current user.

Usage: measure-latency.py <KEY> [--hold ms] [--osd-plane 75] [--repetitions n]
"""
import os
import re
import subprocess
import sys
import time

STATE = "/sys/kernel/debug/dri/0/state"
INJECTOR = "/usr/local/src/player-next/kms-inject"
BLOCK_RE = re.compile(r"plane\[(\d+)\]:[^\n]*\n((?:\t[^\n]*\n)+)")


def planes():
    """Parse the DRM state dump into {plane_id: {fb, format, pos}}."""
    with open(STATE) as handle:
        text = handle.read()
    result = {}
    for match in BLOCK_RE.finditer(text):
        plane_id = int(match.group(1))
        body = match.group(2)
        fb_id = int(re.search(r"\tfb=(\d+)", body).group(1))
        fmt = re.search(r"\tformat=(\w+)", body)
        pos = re.search(r"crtc-pos=(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", body)
        result[plane_id] = {
            "fb": fb_id,
            "format": fmt.group(1) if fmt else None,
            "pos": tuple(int(x) for x in pos.groups()) if pos else None,
        }
    return result


def wait_for(predicate, limit=3.0):
    started = time.monotonic()
    while time.monotonic() - started < limit:
        if predicate(planes()):
            return time.monotonic()
        time.sleep(0.001)
    return None


def start_injector(key, hold):
    ts_path = "/tmp/kms-inject.ts"
    if os.path.exists(ts_path):
        os.unlink(ts_path)
    proc = subprocess.Popen([INJECTOR, key, "--delay", "0.6", "--hold", str(hold),
                             "--ts", ts_path],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return proc, ts_path


def one_test(key, hold, osd_plane):
    # Wait for the OSD to be hidden (fb == 0) before measuring.
    ready = wait_for(lambda state: state.get(osd_plane, {}).get("fb", 0) == 0, 2.5)
    if ready is None:
        return None
    time.sleep(0.4)                 # let the 1.2 s display window close

    proc, ts_path = start_injector(key, hold)
    visible_at = wait_for(lambda state: state.get(osd_plane, {}).get("fb", 0) != 0, 3.0)
    proc.wait()
    if not os.path.exists(ts_path):
        return None
    with open(ts_path) as handle:
        fields = handle.read().split()
    injected_at = float(fields[2])
    if visible_at is None:
        return None
    return (visible_at - injected_at) * 1000.0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    key = sys.argv[1]
    hold, repetitions, osd_plane = 60, 3, 75
    for index, argument in enumerate(sys.argv):
        if argument == "--hold":
            hold = int(sys.argv[index + 1])
        elif argument == "--repetitions":
            repetitions = int(sys.argv[index + 1])
        elif argument == "--osd-plane":
            osd_plane = int(sys.argv[index + 1])

    samples = []
    for _ in range(repetitions):
        value = one_test(key, hold, osd_plane)
        if value is None:
            print("%s: no OSD effect detected" % key)
            break
        samples.append(value)
        print("%s: %.1f ms" % (key, value))
        time.sleep(0.3)
    if samples:
        print("--- %s: n=%d  min=%.1f  mean=%.1f  max=%.1f ms"
              % (key, len(samples), min(samples),
                 sum(samples) / len(samples), max(samples)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
