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
| `kms-vt` | shell | Prepares the console VT: switch, clear, hide cursor; restores it on exit. The only command the player runs through sudo. |
| `kms-planes` | C | Diagnostic: lists every plane (formats, zpos, alpha, attached framebuffer) and every connector with its live mode and refresh rate. |
| `kms-inject` | C (`uinput`) | Diagnostic: injects synthetic key events, used to measure input latency. |

## Plane selection

The three planes are chosen by **one** algorithm, in `src/plane-select.h`, which is
compiled into both `kms-planes` and `kms-subd` (a header, so there is only one
implementation in the tree). `kms-play-py` never selects a plane itself: it runs

```
$ kms-planes --select
video 57 81 NV15 4
sub   69 81 ARGB8888 6
osd   75 81 ARGB8888 7
```

and hands those ids to `kmssink` (`plane-id=57`) and to `kms-subd`
(`kms-subd <sock> 57 69 75 81`). `sub none` / `osd none` mean the board cannot
provide that layer; playback continues, degraded, and the log says why. Nothing
but those three lines is written to stdout; diagnostics go to stderr.

Rules (single screen: the goal is to never programme another CRTC's plane):

1. **Connector**: `KMS_PLAY_CONNECTOR` if set, otherwise the first connected
   connector with a routed CRTC. None found → explicit `no connected display`.
2. **CRTC**: the one the connector is routed to. Only planes whose
   `possible_crtcs` mask contains that CRTC are candidates.
3. **Video**: an `overlay` plane (`primary` only if no overlay exists) that
   accepts NV15 or NV12, lowest zpos. The chosen fourcc (NV15 preferred) is
   logged.
4. **Sub / OSD**: two distinct `ARGB8888` overlay planes of the same CRTC, with
   a zpos above the video plane. The two highest-zpos candidates are used:
   highest = OSD, second = subtitles (this leaves lower overlays free and keeps
   the long-tested 69/75 pair on the BPI-M7).
5. **zpos is read, never forced.** If the property is immutable the current
   value is the hardware order; the player logs `zpos immutable, hw order` and
   verifies the plane by reading it back after configuration.
6. **Degradation**: one ARGB overlay → video + OSD, subtitles disabled;
   none → video only; no video plane → a clear error naming what is missing.
7. **Overrides**: `KMS_PLAY_PLANES="video=57,osd=75,sub=69"` short-circuits the
   selection (`none` disables a text plane). An id that does not exist, does not
   belong to the CRTC, is a cursor plane, or has the wrong format is a hard
   error.
8. **Read-back**: 1.5 s after the first frames, the player reads
   `kms-planes --short`; the video plane must have `fb!=0`. If not, it logs the
   failure and tries the next `kms-planes --list-video` candidate once (two
   attempts total, never a loop), after which it gives up with a log line.

`KMS_PLAY_PLANES_TOOL` overrides the path to the binary (used by
`kms-play-next`). `KMS_PLAY_PLANES` is the override string above, not a path.

`kms-planes --check` dumps the whole topology this algorithm works on: per
connector (status, encoder, routed CRTC, mode, vrefresh), per CRTC (id, mode,
active) and per plane (DRM type, zpos range and immutability, formats,
`possible_crtcs` decoded to CRTC ids).

### Measured on the BPI-M7 (RK3588, kernel 6.18.44)

HDMI-A-1, 2560x1440 @ 75 Hz, encoder 82, **one** CRTC (81, mask `0x1`), seven
planes, all reported `type=overlay` with a mutable `zpos` range `0..7`:

| plane | zpos | NV15/NV12 | ARGB8888 | selection |
|---|---|---|---|---|
| 39 | 1 | no | yes | — (below the video plane) |
| 45 | 2 | no | yes | — |
| 51 | 3 | no | yes | — |
| 57 | 4 | yes | yes | **video**, NV15 |
| 63 | 5 | yes | yes | — (third ARGB candidate) |
| 69 | 6 | yes | yes | **sub** |
| 75 | 7 | yes | yes | **osd** |

What is *still* not portable, even with this work:

- this Rockchip driver exposes neither `CRTC_W`/`CRTC_H` nor the plane position
  through the legacy `GETPLANE` ioctl (`pos` reads back as `+0+0`), so "covers
  the screen" cannot be proven by reading the kernel; the read-back proves
  `fb!=0`, and full-screen coverage relies on `kmssink` programming the plane
  that way;
- a driver that hides the plane `type` property, or reports a wrong one, is
  treated as `unknown` and falls through to the format/zpos rules;
- a board with no free overlay at all (all planes in use by fbcon) can only run
  the video plane, and the subtitles/OSD are lost — the log says so;
- the wrong-CRTC refusal is implemented but cannot be exercised on a
  single-CRTC board like this one: every plane's `possible_crtcs` is `0x1`.

---

## Keyboard

| Key | Action |
|---|---|
| `ESC` | Quit immediately, restore the original VT |
| `Q` | Same as `ESC` |
| `Space` | Play / pause |
| `A` | Next audio track (cyclic) — OSD shows `Language — track title` |
| `S` | Next subtitle track (cyclic, includes `Désactivé`) — OSD shows the language |
| `I` | Information panel in the top left corner, with the file name; a second `I` hides it |
| `←` / `→` | Seek backwards / forwards, **cumulative ladder**: 1st press 1 s, then 10 s, 30 s, 60 s, 120 s, then 240 s (max). Resets after 1 s without a press or when the direction changes. |
| `↑` / `↓` | Software volume ±2 % (player gain, 0-100 %, starts at 100 %; `KMS_PLAY_VOLUME_STEP=1` for 1 %), with a discrete bar |
| Media keys | System volume through PipeWire (`wpctl`), with the same bar |
| `Enter` | Toggle the OSD |

The OSD is deliberately sober: a thin bar at the bottom of the screen, small label,
white on translucent black. It never covers the picture and fades out by itself after
about 1.2 s. It is not animated continuously — it is only redrawn when something changes.

**Letters follow the keyboard layout, not the US position.** The kernel reports key
*positions*: on an AZERTY keyboard the key labelled `A` sits where a US keyboard has
`Q` and reports `KEY_Q`. The player therefore compiles the XKB layout declared in
`/etc/default/keyboard` (`XKBLAYOUT`, via `libxkbcommon`) and translates each keycode,
so `A`, `S` and `Q` mean the letters actually printed on the keycaps. `ESC`, the arrows,
space and the media keys are position-independent and are never translated. Override
with `KMS_PLAY_KEYMAP=us|fr|…` if the declared layout is wrong.

---

## Output formats

The player never converts video to force a format. The RK3588 planes accept both
`NV15` (10-bit 4:2:0) and `NV12` (8-bit 4:2:0) natively, so:

* 10-bit sources decode to `NV15` and are scanned out untouched;
* 8-bit sources decode to `NV12` and are scanned out untouched.

Both paths are zero copy. Forcing 10-bit output on an 8-bit source would require a CPU
conversion — exactly the cost this project exists to avoid.

---

## Codecs: hardware first, software as a fallback

Every file is first played on the hardware path: `playbin` → decoder → `kmssink`, with
the decoder's DMA-BUFs scanned out without a single copy. If that fails — an error, or
no `PLAYING` state within 12 s — the player rebuilds the pipeline once with software
decoders (`videoconvert ! kmssink`, hardware decoders demoted to rank `NONE`), seeks back
to the current position, shows « Décodage logiciel » and keeps going. One attempt only:
a second failure stops cleanly with a clear message.

What that means in practice, measured on the board at 1080p unless stated:

| Codec | Tier | CPU |
|---|---|---|
| H.265 10-bit, 4K | hardware, zero copy | 0.19 core |
| AV1 | hardware, zero copy | ~0.06 core |
| MPEG-2 | hardware, zero copy | ~0.09 core |
| VP8 | `v4l2slvp8dec` present, not tested | — |
| H.264 8-bit | software | ~0.57 core |
| H.264 High 10 | software | ~0.85 core |
| VP9 | software (720p) | ~0.31 core |

**VP9 has no hardware decoder on this kernel** — the `rkvdec` instance does not register
(`missing multi-core support`), so the only V4L2 nodes are `rk3328-vpu-dec`, `vepu121`
and `rk3588-av1-vpu-dec`. That is a kernel limitation, not a GStreamer one: there is no
VP9 decoder element to plug. Playback uses `avdec_vp9` automatically.

**H.264 hardware decoding is blocked by a plugin limitation.** `v4l2slh264dec` refuses to
negotiate its DMABuf caps with `kmssink` (`DMABuf caps negotiated without the mandatory
support of VideoMeta`), and forcing system memory is not supported by that decoder. The
software tier is the answer; details and the failed workarounds are in
[`src/README.md`](src/README.md#decode-tiers).

## Startup banner and information panel

When playback is confirmed — the decode tier is decided and the video caps are
negotiated — the OSD plane shows a four-line panel in the top left corner, for
8 seconds, then fades out over 0.8 s and disappears. The labels are translated
(30 languages, see [`src/README.md`](src/README.md#language)); this is the
English wording:

```
HARDWARE · zero-copy
H.265 Main 10 · 3840x1600 · 23.976 fps
Plane 57 · NV15 · DMABuf · zpos 4
HDMI-A-1 · 2560x1440 @ 75 Hz
```

| Line | Values come from |
|---|---|
| decode tier | the caps probe that decided the tier (`DMABuf` = hardware) |
| codec, profile | the decoder's input caps (`video/x-h265, profile=main-10`) |
| resolution, frame rate | the raw caps that reached the sink |
| plane, format, memory, zpos | `kms-planes --short`, i.e. the kernel's live plane state |
| connector, mode, refresh | `kms-planes`, from the CRTC's live `drmModeModeInfo` |

Pressing **I** shows the same panel at any time, with a fifth line holding the file
name (base name only). A long name is shortened in the middle, keeping the extension
and the last release tokens: `Un.Titre.…x265-TEAM.mkv`. A second **I** hides the
panel at once; otherwise it fades after the same 8 s. Other keys do **not** close
it — seeking, pausing, changing tracks and reading it at the same time is the point
— so the bottom band stays suppressed while the panel is up.

The box hugs its content: its width is the widest line's real ink plus 22 px of
margin on each side, capped at 880 px (`PANEL_MAX_WIDTH`). A short panel is around
400 px wide, and only a line that had to be shortened reaches the 880 cap. The
height still follows the line count (202 px for the four startup lines, 242 px with
the file name). The black background is translucent (0.72) and configurable:

```bash
KMS_PLAY_PANEL_BG=0 kms-play film.mkv    # no box at all, white text on the picture
KMS_PLAY_PANEL_BG=1 kms-play film.mkv    # fully opaque box
```

`KMS_PLAY_PANEL_BG` accepts 0..1; an invalid value is ignored with a log line and
the default is used. The subtitle band is not affected: it keeps its own width and
its 0.55 background.

In the software tier the first line says `SOFTWARE · CPU fallback`, and the plane
line also shows the negotiated format when it differs from what the plane scans out,
e.g. `Plane 57 · NV12 (I420 negotiated) · system · zpos 4`. The startup panel is
cancelled by the first key, before that key is interpreted, so the usual band appears
immediately; the I panel ignores every key except I.

Every label in the panel, in the bottom band (`Pause`, `Décodage logiciel`,
`Audio : …`) and in the displayed error messages comes from one string table, in
the language chosen by `KMS_PLAY_LANG` or the locale — English when none is
recognised. The values never change with the language: a codec name, a DRM fourcc
and a connector name are facts. A string whose glyphs the language's font cannot
draw is replaced by its English form rather than shown as boxes, and the event is
logged.

```bash
KMS_PLAY_LANG=ko kms-play film.mkv     # Korean
LANG=ja_JP.UTF-8 kms-play film.mkv     # Japanese, from the locale
```

The fade is the only animation in the player: 12 bounded renders, then the plane is
cleared and the buffer blanked. The panel and the bottom band are two different
rectangles on the same OSD plane (there are only two ARGB planes in total), so the
player writes the surface geometry next to the buffer (`/dev/shm/kms-osd-<uid>.geom`,
`WxH`) for `kms-shot`. Both dimensions follow the content: width = widest ink + 44 px
(capped at 880), height = 202 px for the four startup lines, 242 px with the file
name.

---

## After playback: the screen comes back

A DRM plane outlives the process that programmed it, so a player that simply
exits can leave its full-screen video plane scanning out over the console —
which is exactly what a black television screen with no process running looks
like. On every exit path (`Q`, `ESC`, end of stream, error, `SIGTERM`) the
player therefore brings the pipeline to `NULL` and **waits** for it, asks
`kms-subd` (the DRM master) to detach the video, subtitle and OSD planes, and
then reads the result back with `kms-planes`:

```
pipeline: NULL in 121 ms
planes: released, no framebuffer left on any plane
exit, VT 5 | planes released: True
```

The kernel's own view, after a stop:

```
plane[33]: Cluster0-win0   crtc=video_port0   fb=86                     (fbcon)
plane[57]: Esmart0-win0    crtc=(null)  fb=0  crtc-pos=0x0+0+0          (video)
plane[69]: Esmart2-win0    crtc=(null)  fb=0  crtc-pos=0x0+0+0          (subtitles)
plane[75]: Esmart3-win0    crtc=(null)  fb=0  crtc-pos=0x0+0+0          (OSD)
```

The player then **returns to the desktop**: the VT of the active graphical
session on `seat0` (from logind), else the VT that was active before playback,
else `1`; the switch only happens if the player still owns the screen, so a VT
change made during playback is never stolen. `KMS_PLAY_RETURN_VT=2` pins the
target, and `KMS_PLAY_RETURN_VT=none` keeps the player on its own VT for a
machine without a desktop. What it lands on is verified, not assumed:

```
VT: previous tty2, returning to 2 (desktop)
exit, VT 2 (desktop)
```

The player's VT itself is left with one line, `playback finished: <file>` or
`playback stopped: <file>` (invisible once the desktop is back, but readable in
`/dev/vcsa<player VT>` and in the log). That line is ASCII English on purpose:
the kernel console font has no non-Latin glyphs.

---

## Requirements

* RK3588 board with KMS planes supporting `NV15`/`NV12` plus at least two `AR24` overlay planes
* Armbian (or any distro with a recent kernel exposing the VOP planes)
* GStreamer 1.20+ with `kmssink`, `v4l2` Rockchip decoders
* `gst-libav` for the software tier (`avdec_h264`, `avdec_h265`, `avdec_vp9`, `avdec_mpeg2video`) — already installed, nothing to add
* Python 3 with `python3-gi`, `gir1.2-gst-1.0`, Pango, PangoCairo, Cairo
* `libdrm` development headers to build the C helpers
* Permission to use `/dev/dri/card0` and `/dev/input/event*` as a normal user
* `kms-vt` allowed through a `NOPASSWD` sudo rule (`/etc/sudoers.d/kms-play`) — the
  player switches VT before taking the DRM master, and only root may write to a tty
* `getty` masked on the player's VT (`systemctl mask getty@ttyN autovt@ttyN`) —
  `install.sh` does it for the VT it installs

## Build

```sh
make            # builds kms-subd, kms-keys, kms-planes, kms-inject, libkmsshare.so
sudo make install
```

## Run

```sh
kms-play /path/to/movie.mkv
```

Playback happens on **VT4**: the console is switched there, cleared and the cursor hidden,
so the picture sits on plain black. `ESC`/`Q` gives the console back and returns to the VT
you came from (never to VT4, which has no getty — you would land on a blank screen).

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
* **The login prompt shows up in the letterbox.** The video plane does not fill the
  screen when the film's aspect ratio differs from the mode's: on 2560x1440 a 2.39:1
  film scans out at `crtc-pos=2560x1066+0+187`, so 187 px of the console stay visible
  above and below the picture. Two things keep that area black:
  * masking **both** `getty@ttyN` **and** `autovt@ttyN`. Masking `getty@ttyN` alone is
    not enough: systemd-logind starts `autovt@ttyN` directly on every VT switch, and
    that instance is not covered by the alias mask;
  * clearing the VT itself (`kms-vt prepare`). A text VT keeps whatever was last
    written to it, so a VT that once showed a banner still displays it — hiding the
    cursor and clearing is what makes it actually black. Verify without a screen:
    `/dev/vcsaN` is the VT's character map, it must contain no character other than
    space while a film plays.
* **`Restart=always` is not enough** for the chromium/CDP side of this machine — see the
  watchdog in the parent project.

---

## Status

Working and installed on the board. The three-plane path, the full keyboard map, the
cumulative seek ladder and the two independent volumes are implemented and measured.

Verified from the hardware, not from impressions:

| What | Measurement |
|---|---|
| Video plane | `plane[57] format=NV15 allocated by = vqueue:src` — decoder buffer, zero copy |
| Overlay planes | `plane[69]` subtitles, `plane[75]` OSD, both `AR24`, no overlap with the glyph area |
| Keypress → visible change | 64-131 ms, typical 65-85 ms |
| CPU, 4K 10-bit, subtitles + OSD active | 0.19 core |
| CPU, 1080p 10-bit | 0.08 core |
| CPU, H.264 8-bit (software tier) | 0.57 core |
| CPU, H.264 High 10 (software tier) | 0.85 core |
| ESC → original VT | 352 ms (dominated by `chvt`) |

The cumulative seek scale is checked by `src/test-seek-scale.py`, which injects real
uinput key presses and asserts the progression 1 / 10 / 30 / 60 / 120 / 240 s,
the reset after one second of inactivity and the reset on a direction change.

**Overlays are verifiable without a screen.** The player writes the OSD and the subtitle
bands as plain cairo ARGB buffers in `/dev/shm` before handing them to `kms-subd`, so
`kms-shot` reads them back as a PNG:

```sh
kms-shot osd /tmp/osd.png     # progress bar, volume, track label, or the info panel
kms-shot sub /tmp/sub.png     # the current subtitle cue
kms-shot osd /tmp/osd.png --size 640x242   # override the recorded geometry
```

It prints the surface size (and where that size came from), plus the number of
non-transparent pixels, which distinguishes "drawn" from "not showing at the time of
the dump". That is how the subtitle rendering was checked rather than guessed: white
bold glyphs with a black outline, laid out at the real position of the pipeline. The
OSD is no longer always the same band (the information panel is a content-sized box in
the top left corner, up to 880 px wide), so the player writes `WxH` to `/dev/shm/kms-osd-<uid>.geom`
alongside the buffer and `kms-shot` reads it back; `--size` is the manual override.

**What still needs a human**: the picture itself. The video plane scans out an `NV15`
buffer allocated by the VPU as a DMABUF, which is not readable from user space —
banding, HDR tone mapping, motion and colour have to be judged by eye.

Detailed component documentation, build instructions and pitfalls live in
[`src/README.md`](src/README.md).

## License

GNU General Public License v3.0 or later — see [LICENSE](LICENSE).

Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

In short: you may use, study, modify and redistribute this software, but if you
distribute a modified version you must publish its source under the same license.
Running a modified copy privately carries no obligation to publish.
