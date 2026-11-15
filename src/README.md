# kms-player — three-plane KMS player for Rockchip RK3588

A full-screen video player that draws straight to the display controller through
KMS/DRM, with subtitles and an on-screen display composited by the VOP on
separate hardware planes. No compositor, no X11, no Wayland, no window manager.

Target board: **Banana Pi BPI-M7** (RK3588), Armbian, kernel 6.18, kernel module
`rockchipdrm`. It should work on any RK3588 board whose VOP exposes an NV15
(10-bit) and an NV12 (8-bit) video plane plus two ARGB overlay planes, but this is
what it was written and measured on.

---

## Architecture

```
 screen
 ┌───────────────────────────────────────────────────────────────────────┐
 │ plane zpos 7  AR24   OSD: progress bar, volume bar, "Français — VFF"  │  kms-subd
 │ plane zpos 6  AR24   subtitles, Pango/Cairo, transparent except text  │  kms-subd
 │ plane zpos 5  NV15   video, decoder DMA-BUF, zero copy                │  kmssink
 └───────────────────────────────────────────────────────────────────────┘
```

The video never leaves the GPU/decoder memory path. Only two small ARGB bands are
written by the CPU, and each layer updates independently — which is what makes the
interface feel instant.

### The DRM master problem, and why the fd is shared

KMS allows exactly one DRM master per device. Three things need to draw: the video
sink (`kmssink`), the subtitle plane and the OSD plane. The solution:

1. `kms-subd` starts first, takes the DRM master, and owns the plane state.
2. It listens on a unix socket and sends its DRM file descriptor to whoever
   connects, using `SCM_RIGHTS`.
3. `libkmsshare.so` is `LD_PRELOAD`ed into the player; it intercepts the player's
   `open("/dev/dri/card*")` and returns the received fd, and neutralises
   `drmSetMaster`/`drmDropMaster` so the pipeline cannot drop the master.

Both processes then share one file description, hence one master. `libkmsshare.so`
is pre-existing infrastructure, not part of this source tree.

**The VT must be switched before `drmSetMaster`** — the VT that currently owns the
console keeps the master locked, and the ioctl fails with `EACCES`. `kms-play-py`
does the switch (`sudo -n /usr/bin/chvt`, the only sudo rule granted) and restores
the original VT on exit.

---

## Components

| File | Role |
|---|---|
| `kms-play-py` | The player: probes the container, picks the video plane, starts the helpers, builds the GStreamer pipeline, renders subtitles and OSD with Pango/Cairo, handles the keyboard. |
| `kms-subd.c` | Long-lived daemon holding the DRM master. Drives the two ARGB planes; protocol on stdin (`sub cue …`, `osd cue …`, `sub clear`, `osd clear`, `quit`). |
| `kms-keys.c` | Reads every `/dev/input/event*` with one `poll()` loop and prints `key <NAME> <1\|0\|2>` lines on stdout. **Owns autorepeat**: it drops kernel `value=2` events and synthesises its own (300 ms delay, 60 ms interval), so the behaviour does not depend on the keyboard's `EV_REP` capability or on the desktop's repeat settings. |
| `kms-planes.c` | Dumps the live DRM plane state (zpos, formats, attached framebuffer, position). `--short` is machine-parsable and is what the player uses to find the NV15 plane. This is the only way to prove "the OSD is visible" without a screen. |
| `kms-inject.c` | uinput virtual keyboard for tests. Emits one key press with a monotonic timestamp so latency can be measured. |
| `measure-latency.py` | Measures keyboard → visible-effect latency by watching the OSD plane in `/sys/kernel/debug/dri/0/state`. |
| `measure-cpu.py` | CPU usage of the player process tree over N seconds. |
| `test-seek-scale.py` | Automated check of the cumulative seek scale (see below). |
| `kmsshare.c` | `LD_PRELOAD` shim that hands the DRM master to `kmssink` over a Unix socket (`SCM_RIGHTS`) and neutralises `drmSetMaster`/`drmDropMaster`. Without it, only one process could program planes. |
| `kms-keywait.c` | Legacy watcher kept for compatibility: blocks until ESC or Q. The current player does not use it, but older installations and scripts still reference it. |
| `drm-plane-formats.c` | Minimal diagnostic that prints the fourcc codes each plane accepts. |
| `kms-play` | Launcher: sets `LD_PRELOAD` and `KMS_SHARE_SOCK` and runs the installed player. |
| `kms-play-next` | Same, but runs everything from this source directory (used for testing before installing). |
| `install.sh` | Copies the compiled binaries and scripts to `/usr/local/bin` after a backup. |
| `Makefile` | Builds every C component and the shim, and checks that licence headers are present. |

### Subtitle and OSD planes

Both are `ARGB8888` dumb buffers with **pre-multiplied** alpha, blended by the VOP
(`pixel blend mode = Pre-multiplied`, plane `alpha = 65535`). This is what cairo
writes, so the glyph edges blend correctly; using straight alpha would produce
halos. Two buffers per plane are alternated so the buffer being scanned out is
never modified.

Cairo surfaces are written to `/dev/shm/kms-{sub,osd}-<uid>.raw` (tmpfs, no eMMC
wear) and `kms-subd` copies them row by row — the kernel pitch can be larger than
`width * 4`.

---

## Behaviour

| Key | Action |
|---|---|
| `ESC` / `Q` | Quit, restore the previous VT |
| `Space` / `PLAYPAUSE` | Play / pause (pause keeps the last frame) |
| `A` | Next audio track (cyclic), OSD `Audio : Français — VFF` |
| `S` | Next subtitle track (cyclic, includes `Désactivé`), OSD shows the language |
| `←` / `→` | Cumulative seek, see below |
| `↑` / `↓` | Software gain ±5 % (starts at 85 %, ceiling 100 %) |
| `VOLUMEUP` / `VOLUMEDOWN` / `MUTE` | PipeWire sink volume (`wpctl`), OSD |
| `Enter` | Toggle the OSD |
| `NEXTSONG` / `PREVIOUSSONG` | Fixed ±60 s jump |

### Cumulative seek scale

Not a hold acceleration. Each accepted press jumps the current rung and advances
one rung, in the direction pressed:

| Press | 1 | 2 | 3 | 4 | 5 | 6 | 7+ |
|---|---|---|---|---|---|---|---|
| Jump | 0.5 s | 1 s | 10 s | 30 s | 60 s | 120 s | 240 s |

* The index returns to 0.5 s after **1 s** without a press.
* The index returns to 0.5 s as soon as the **direction changes**.
* Autorepeat counts as successive presses (a held arrow walks the scale), with a
  **120 ms debounce** so one physical press cannot consume two rungs.
* The target is clamped to `[0, duration - 0.25 s]`; seeking to the very end would
  raise EOS and stop playback.

### Two independent volumes

* `↑`/`↓` → GStreamer `volume` element (software gain), 0-100 %, starts at 85 %.
* Multimedia keys → PipeWire sink volume, shared with the rest of the session.

They are intentionally separate: the first one is the player's own level, the
second one is the system level.

---

## Build

Dependencies (Debian/Ubuntu naming):

```
build-essential libdrm-dev libinput-dev
python3-gi gir1.2-gstreamer-1.0 gir1.2-pango-1.0 python3-cairo
gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
gstreamer1.0-rockchip  (kmssink)   ffmpeg   wireplumber (wpctl)
```

Build everything with a single command:

```sh
make            # all C helpers + libkmsshare.so, then a syntax check on kms-play-py
make check      # verify that every source file carries an SPDX licence header
```

`make` probes `pkg-config` for libdrm and falls back to `-I/usr/include/libdrm`
on images that ship libdrm without a usable `.pc` file — a real case on Armbian.

If you prefer to compile by hand, the equivalents are:

```sh
cc -O2 -Wall -Wextra -o kms-subd   kms-subd.c   $(pkg-config --cflags --libs libdrm)
cc -O2 -Wall -Wextra -o kms-planes kms-planes.c $(pkg-config --cflags --libs libdrm)
cc -O2 -Wall -Wextra -o kms-keys   kms-keys.c
cc -O2 -Wall -Wextra -o kms-inject kms-inject.c
cc -O2 -Wall -Wextra -fPIC -shared -o libkmsshare.so kmsshare.c -ldl
```

`kms-play-py` is Python 3 with PyGObject and needs no build step.

## Run

```sh
# installed version
kms-play /path/to/movie.mkv

# test the sources in this directory without installing
./kms-play-next /path/to/movie.mkv
```

The player must run as the desktop user (`armbian`), **never as root**: root's
`XDG_RUNTIME_DIR` and PipeWire session are not the ones the audio sink needs.

## Install

```sh
sudo make install          # builds, then runs install.sh and installs libkmsshare.so
```

`install.sh` backs the previous binaries up to `/root/gst-hevc-patch-<date>/`
first: on this board the player is the only way to watch a film, so every
install must be reversible by hand.

---

## Testing without a screen

The whole point of `kms-planes`, `kms-inject` and the log file is that every
visual claim can be checked from an SSH session.

1. **Is the player running?** Tail `~/.cache/kms-play/last.log`: every decision
   (streams, format, seeks, cues, OSD renders) is logged with a monotonic stamp.
2. **Are the planes right?**

   ```sh
   kms-planes --short
   ```

   The video plane must show `nv15=1` (or `nv12=1`) with a `fb=` and a DMABuf
   modifier; the OSD plane must show `fmt=AR24` with a non-zero `fb` only while
   the OSD is displayed.
3. **Zero copy proof**: grep the log for `video caps:` — it must say
   `memory=DMABuf`. `memory=system` means a conversion was inserted, which is a
   failure (the plane accepts NV15 and NV12 natively).
4. **Keyboard latency**:

   ```sh
   sudo python3 measure-latency.py RIGHT --repetitions 5
   ```

   (reads `/sys/kernel/debug/dri/0/state`, hence root).
5. **Seek scale**:

   ```sh
   python3 test-seek-scale.py
   ```

   Injects 7 RIGHT presses, waits 1.5 s, presses RIGHT, then LEFT, and validates
   each jump against the table above, both from the player's decision and from the
   difference between consecutive target positions.
6. **CPU**: `python3 measure-cpu.py 30` while a 4K file plays. Must stay under one
   core.

## Hardware and software prerequisites

* RK3588 with `rockchipdrm`: one CRTC, at least three planes — one accepting NV15
  and/or NV12, two accepting ARGB8888 with an `alpha` and a `pixel blend mode`
  property.
* A keyboard or remote exposing `KEY_LEFT`, `KEY_RIGHT`, `KEY_UP`, `KEY_DOWN`,
  `KEY_ENTER`, `KEY_SPACE`, `KEY_A`, `KEY_S`, `KEY_ESC` (the exact whitelist is in
  `kms-keys.c`).
* `video` group membership for `/dev/dri/*`, `input` group for `/dev/input/*`.
* `sudo` rule allowing only `/usr/bin/chvt` (for the VT switch). No other sudo
  rights are needed or granted.
* H.265 10-bit decoding through the vendor GStreamer plugin; the player never
  converts to force a format.

## Known pitfalls

* **VT switch before `drmSetMaster`** — otherwise `EACCES`. If a previous run
  crashed, the VT may be left on 3/4; `sudo chvt 2` brings the desktop back.
* **One DRM master only** — a second player is refused by the lock file
  `/tmp/kms-play-<uid>.lock`. A stale `kms-subd` is killed at startup, because it
  would still hold the master.
* **Children must not inherit `LD_PRELOAD`** — `kms-subd`, `kms-keys` and the
  pipeline are started with `LD_PRELOAD` and `KMS_SHARE_SOCK` removed from the
  environment, otherwise they would try to steal the shared fd.
* **Plane alpha is per-pixel, not per-plane** — the plane `alpha` property stays
  at 65535; transparency comes from cairo's ARGB buffer. Setting the plane alpha
  to 0 makes the whole overlay invisible even if the buffer is fine.
* **`drmModeAddFB` with depth 32, bpp 32** exposes the dumb buffer as ARGB8888;
  depth 24 would give XRGB8888 and drop the alpha channel.
* **Pitch padding** — the kernel may return `pitch > width * 4`; always copy row
  by row.
* **Kernel autorepeat is ignored on purpose** (see `kms-keys.c`). If you change
  `REPEAT_INTERVAL_MS` or `REPEAT_DELAY_MS`, re-check the 120 ms seek debounce in
  `kms-play-py`: an interval close to the debounce makes the accepted rate jitter.
* **Image-based subtitles** (PGS, DVD) are filtered out: only `subrip`, `ass`,
  `ssa`, `mov_text` and `webvtt` are rendered.
* **OSD rate limit** — renders are capped at ~10 Hz; faster key bursts update the
  label but not the plane every time. This is deliberate, not a lost event.

## License

This project is distributed under the terms of the **GNU General Public License,
version 3 or (at your option) any later version** (`GPL-3.0-or-later`).
Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>.

The complete license text is in the `LICENSE` file of the repository. Every
source file carries an SPDX header with the same information.
