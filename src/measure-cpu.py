#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

"""measure-cpu.py - CPU usage of the player process tree over a period.

Why: the player must stay under one core while decoding 4K 10-bit video with
the OSD active. This tool samples utime+stime from /proc/<pid>/stat for the
target process and all its descendants (kms-subd, kms-keys, gst children) and
reports the average number of cores used.

Usage: measure-cpu.py [seconds] [--pattern kms-play-py]
"""
import os
import sys
import time

HZ = os.sysconf("SC_CLK_TCK")


def all_pids():
    return [int(entry) for entry in os.listdir("/proc") if entry.isdigit()]


def process_tree(root):
    """Return root plus every descendant, by walking the ppid links once."""
    children = {}
    for pid in all_pids():
        try:
            with open("/proc/%d/stat" % pid) as handle:
                fields = handle.read().rsplit(") ", 1)[1].split()
            parent = int(fields[1])
            children.setdefault(parent, []).append(pid)
        except OSError:
            continue
    seen, stack = set(), [root]
    while stack:
        pid = stack.pop()
        if pid in seen:
            continue
        seen.add(pid)
        stack.extend(children.get(pid, []))
    return seen


def cpu_ticks(pid):
    """utime + stime in clock ticks, or 0 if the process vanished."""
    try:
        with open("/proc/%d/stat" % pid) as handle:
            fields = handle.read().rsplit(") ", 1)[1].split()
        return int(fields[11]) + int(fields[12])
    except OSError:
        return 0


def find_process(pattern):
    out = os.popen("pgrep -f %s" % pattern).read().split()
    return int(out[0]) if out else None


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    pattern = "kms-play-py"
    if "--pattern" in sys.argv:
        pattern = sys.argv[sys.argv.index("--pattern") + 1]
    root = find_process(pattern)
    if not root:
        print("no process matching", pattern)
        return 1
    pids_a = process_tree(root)
    started = time.monotonic()
    baseline = {pid: cpu_ticks(pid) for pid in pids_a}
    time.sleep(duration)
    elapsed = time.monotonic() - started
    pids_b = process_tree(root)
    total = 0
    detail = []
    for pid in sorted(pids_b):
        now = cpu_ticks(pid)
        delta = now - baseline.get(pid, 0)
        if delta:
            try:
                with open("/proc/%d/comm" % pid) as handle:
                    name = handle.read().strip()
            except OSError:
                name = "?"
            detail.append((pid, name, delta / HZ / elapsed))
            total += delta
    print("duration %.1f s, %d processes, total %.2f core" % (elapsed, len(pids_b),
                                                              total / HZ / elapsed))
    for pid, name, cores in detail:
        print("  %6d %-16s %.3f core" % (pid, name, cores))
    return 0


if __name__ == "__main__":
    sys.exit(main())
