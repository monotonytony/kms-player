# kms-player

A zero-copy video player that draws straight to the display hardware of a Rockchip
RK3588 board through **KMS/DRM**, with subtitles and an on-screen display composited
by the GPU/display controller on **separate hardware planes** — no compositor, no X11,
no Wayland, no window manager.

Target board: **Banana Pi BPI-M7** (RK3588), Armbian, kernel 6.18.
It should work on any RK3588 board exposing NV15 (10-bit) and NV12 (8-bit) overlay
planes, but this is what it was written and measured on.

---

## Why this exists

Playing a 10-bit HEVC movie on a small ARM board usually means choosing between two
bad options:

1. **A full desktop compositor** (GNOME/KDE), which copies every frame through system
   memory and burns CPU for nothing.
2. **A player that draws subtitles itself** (for example GStreamer's `textoverlay`),
   which forces the video through `videoconvert` and destroys performance.

Measurements on this board, 1080p 10-bit, 30 seconds of playback with `textoverlay`
in the pipeline: **11.6 s of CPU time (0.39 cores)**, and in 4K it collapsed to
**3.85 fps**. Both unacceptable.

This player takes a third route. Rockchip's display controller (VOP) can composite
several planes on top of each other. So:

* **Plane 1 (bottom)** — the decoded video, written directly into the plane's
  framebuffer (`NV15` for 10-bit, `NV12` for 8-bit). Zero copy, hardware decoder to screen.
* **Plane 2** — subtitles, as an `AR24` (32-bit ARGB) buffer that is transparent
  everywhere except the glyphs. Rendered by Pango/Cairo **on the CPU**, but only a few
  hundred thousand pixels a second, which costs nothing.
* **Plane 3 (top)** — the OSD: progress bar, volume bar, track labels. Same technique.

The video never leaves the GPU memory path, the overlays never touch the video, and
each layer updates independently. That is what makes the interface feel instant.

---

## Architecture

```
  ┌─────────────────────────────────────────────── display ───────────────────────────────────────────────┐
  │                                                                                                        │
  │   plane zpos 7  AR24   ┌─────────────┐  OSD: progress bar, volume bar, "Français — VFF"                │
  │                        └─────────────┘                                                     kms-subd   │
  │   plane zpos 6  AR24   ┌─────────────┐  subtitles: text rendered by Pango/Cairo into ARGB              │
  │                        └─────────────┘                                                     kms-subd   │
  │   plane zpos 5  NV15   ┌─────────────┐  video: decoded frame, zero copy, BT.2100 PQ                 │
  │                        └─────────────┘                                                     kmssink    │
  └────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### The DRM master problem

KMS allows exactly **one DRM master** at a time. That is a problem here, because three
different things need to draw: the video sink (`kmssink`), the subtitle renderer and
the OSD. The solution is a small daemon, `kms-subd`, which takes the DRM master and
**shares its file descriptor** with the video sink over a Unix socket (`SCM_RIGHTS`).
A `LD_PRELOAD` shim, `libkmsshare.so`, intercepts the sink's `open("/dev/dri/card0")`
and hands it the shared descriptor instead, while neutralising `drmSetMaster` and
`drmDropMaster` so the sink does not fight for mastership.

Result: one master, three writers, no tearing, no extra copies.

### Components

| Component | Language | Role |
|---|---|---|
| `kms-play` | shell | Entry point. Sets `LD_PRELOAD` + socket path, then execs the Python player. |
| `kms-play-py` | Python (GStreamer, Pango, Cairo) | The player itself: picks the video plane, extracts text subtitles, builds the pipeline, renders subtitles and OSD, handles the keyboard. |
| `kms-subd` | C (libdrm) | Holds the DRM master, owns the subtitle and OSD planes, composites ARGB buffers on command. |
| `libkmsshare.so` | C (`LD_PRELOAD`) | Shares the DRM fd with `kmssink` so it can scan out without being master. |
| `kms-keys` | C | Reads every `/dev/input/event*` with `poll()`, prints one line per key event. |
| `kms-planes` | C | Diagnostic: lists every plane, its formats, zpos and alpha capabilities. |
| `kms-inject` | C (`uinput`) | Diagnostic: injects synthetic key events, used to measure input latency. |

---

## Keyboard

| Key | Action |
|---|---|
| `ESC` | Quit immediately, restore the original VT |
| `Q` | Same as `ESC` |
| `Space` | Play / pause |
| `A` | Next audio track (cyclic) — OSD shows `Language — track title` |
| `S` | Next subtitle track (cyclic, includes `Désactivé`) — OSD shows the language |
| `←` / `→` | Seek backwards / forwards, **cumulative ladder**: 1st press 0.5 s, then 1 s, 10 s, 30 s, 60 s, 120 s, then 240 s (max). Resets after 1 s without a press or when the direction changes. |
| `↑` / `↓` | Software volume ±5 % (player gain, 0-100 %), with a discrete bar |
| Media keys | System volume through PipeWire (`wpctl`), with the same bar |
| `Enter` | Toggle the OSD |

The OSD is deliberately sober: a thin bar at the bottom of the screen, small label,
white on translucent black. It never covers the picture and fades out by itself after
about 1.2 s. It is not animated continuously — it is only redrawn when something changes.

---

## Output formats

The player never converts video to force a format. The RK3588 planes accept both
`NV15` (10-bit 4:2:0) and `NV12` (8-bit 4:2:0) natively, so:

* 10-bit sources decode to `NV15` and are scanned out untouched;
* 8-bit sources decode to `NV12` and are scanned out untouched.

Both paths are zero copy. Forcing 10-bit output on an 8-bit source would require a CPU
conversion — exactly the cost this project exists to avoid.

---

## Requirements

* RK3588 board with KMS planes supporting `NV15`/`NV12` plus at least two `AR24` overlay planes
* Armbian (or any distro with a recent kernel exposing the VOP planes)
* GStreamer 1.20+ with `kmssink`, `v4l2` Rockchip decoders
* Python 3 with `python3-gi`, `gir1.2-gst-1.0`, Pango, PangoCairo, Cairo
* `libdrm` development headers to build the C helpers
* Permission to use `/dev/dri/card0` and `/dev/input/event*` as a normal user
* `chvt` allowed through sudo (the player switches VT before taking the DRM master)

## Build

```sh
make            # builds kms-subd, kms-keys, kms-planes, kms-inject, libkmsshare.so
sudo make install
```

## Run

```sh
kms-play /path/to/movie.mkv
```

Logs go to `~/.cache/kms-play/last.log` and record every decision the player makes:
which planes were selected, which subtitle track was used, every seek, every track change.

## Testing

The player is designed to be verified **without a screen**, because that is how it was
developed:

```sh
kms-planes                       # which plane accepts which format
kms-inject --key KEY_RIGHT       # inject a key press
cat /sys/kernel/debug/dri/0/state   # confirm planes are active with the right formats
```

---

## Known pitfalls

These cost real debugging time; do not rediscover them.

* **Switch VT before `drmSetMaster`.** Otherwise you get `EACCES`.
* **Only one DRM master.** Leftover `gst-launch` or key watchers holding the master cause
  `drmModeSetPlane failed: Permission denied (13)`. Kill them before starting.
* **`kmssink` needs a plane that actually accepts the format.** Primary planes on this
  board only do `AR24`; feeding them `NV15` gives `not-negotiated`.
* **`textoverlay` has no `halign` property**, and is far too slow for 4K anyway.
* **PGS (image-based) subtitles cannot be converted to text.** The player selects only
  text subtitle tracks (`subrip`, `ass`, `ssa`, `mov_text`, `webvtt`); PGS tracks are skipped.
* **A suspended board freezes the display pipeline** (1 fps, Cloudflare loops, etc.).
  Sleep must be disabled at the systemd, logind and gsettings levels.
* **`Restart=always` is not enough** for the chromium/CDP side of this machine — see the
  watchdog in the parent project.

---

## Status

Under active development. The three-plane path (video + subtitles + OSD) and the full
keyboard map are being implemented; see the commit history and `docs/` for the current
state. Measurements, not impressions: every feature is validated by reading the DRM
state and the pipeline position, not by looking at the screen.

## License

GNU General Public License v3.0 or later — see [LICENSE](LICENSE).

In short: you may use, study, modify and redistribute this software, but if you
distribute a modified version you must publish its source under the same license.
Running a modified copy privately carries no obligation to publish.
