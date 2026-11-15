#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

# Install the compiled helpers and the launcher into /usr/local/bin.
#
# Run as root from this directory after building. The current binaries and
# scripts are copied to /root/gst-hevc-patch-<timestamp>/ first: the previous
# player is the only way to watch a film on this board, so a failed install
# must always be reversible by hand.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
DEST=/usr/local/bin
STAMP=$(date +%Y%m%d-%H%M%S)
BACKUP=/root/gst-hevc-patch-$STAMP

FILES="kms-subd kms-keys kms-planes kms-inject kms-play-py kms-play kms-play-next"

mkdir -p "$BACKUP"
for f in $FILES; do
    [ -e "$DEST/$f" ] && cp -a "$DEST/$f" "$BACKUP/" || true
done
echo "backup of the previous version: $BACKUP"

for f in $FILES; do
    if [ ! -e "$HERE/$f" ]; then
        echo "missing $HERE/$f - build first" >&2
        exit 1
    fi
    install -m 0755 "$HERE/$f" "$DEST/$f"
done

echo "installed: $FILES"
echo "test with: kms-play-next /path/to/movie.mkv   then   kms-play /path/to/movie.mkv"
