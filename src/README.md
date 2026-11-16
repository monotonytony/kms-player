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
| `kms-keys.c` | Reads every `/dev/input/event*` with one `poll()` loop and prints `key <NAME> <1\|0\|2>` lines on stdout (the table includes `I` for the information panel). **Owns autorepeat**: it drops kernel `value=2` events and synthesises its own (300 ms delay, 60 ms interval), so the behaviour does not depend on the keyboard's `EV_REP` capability or on the desktop's repeat settings. |
| `kms-planes.c` | Dumps the live DRM plane state (zpos, formats, attached framebuffer, position) and every connector's DRM name with the mode and vertical refresh actually scanned out on its CRTC. `--short` is machine-parsable and is what the player uses to find the NV15 plane and to report the plane format in the banner. This is the only way to prove "the OSD is visible" without a screen. |
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
`width * 4`. A `WxH` string is written next to each raw file
(`/dev/shm/kms-{sub,osd}-<uid>.geom`) because the OSD surface changes size: the
bottom band is `screen × 76`, the startup panel is `880 × 202` and the on-demand
panel with the file name is `880 × 242`.

### Startup banner and information panel

Once the decode tier and the video caps are known, `OnScreenDisplay` draws a
four-line panel in the top left corner (`880x202+40+40` on a 2560x1440 screen)
and shows it for 8 s, then fades it over 0.8 s (12 renders at 67 ms) and clears
the plane:

```
MATÉRIEL · zéro copie          <- tier: MATÉRIEL·zero copy / LOGICIEL·CPU fallback
H.265 · Main 10 · 3840x1600 · 23.976 i/s   <- codec+profile, size+rate
Plan 57 · NV15 · DMABuf · zpos 4           <- plane id, fourcc, memory, zpos
HDMI-A-1 · 2560x1440 @ 75 Hz               <- connector, live mode, vrefresh
```

Sources, all read at the moment the banner is drawn:

* codec and profile: the compressed caps on the decoder's sink pad, found by a
  single walk of the pipeline tree (`video/x-h265, profile=main-10` becomes
  `H.265 Main 10`); ffprobe's stream metadata is the fallback when the decoder
  cannot be located;
* width, height, frame rate: the raw caps that reached the video sink;
* plane format and zpos: `kms-planes --short` for the video plane — the DRM
  fourcc (`NV15`) and the kernel's zpos, not our own bookkeeping. When the caps
  format differs from what the plane scans out (software tier: `I420` through
  videoconvert becomes `NV12`), both are shown: `NV12 (négocié I420)`;
* connector, mode, refresh: `kms-planes`, one call at startup. The sysfs `modes`
  file lists supported modes, not the live one, and has no refresh rate, so the
  connector's routed CRTC and `drmModeModeInfo.vrefresh` are used. If no mode is
  routed yet, the preferred mode is printed and flagged `preferred`.

The panel and the bottom band share the OSD plane: the board exposes only two
ARGB planes in total, and the subtitles keep theirs. They cannot be shown at the
same time, and the rules differ by trigger:

* `kind="startup"` (automatic): four lines, superseded by a band render and
  cancelled by any key event — `on_key()` calls `cancel_panel(..., kind="startup")`
  before interpreting the key, so a seek shows its band immediately;
* `kind="manual"` (key `I`): the same four lines plus a fifth line with the base
  name of the file. Only `I` closes it (toggle) or the 8 s timer fades it; while it
  is up, `show()` suppresses the bottom band so seeking, pausing or switching tracks
  does not erase what is being read. The action itself still happens.

The file line is shortened in the middle by `_fit_middle()`, measured with the real
Pango layout and unwrapped (with the drawing width set, Pango reports the wrapped
block width and every long name would look like it fits). The cut is made at a dot,
so the tail keeps whole release tokens and the extension
(`Un.Titre.…x265-TEAM.mkv`); the full name is logged once as
`panel: shortened: <full> -> <shown>`.

The box height follows the line count (`PANEL_TOP + lines*40 + PANEL_BOTTOM`):
202 px for the startup panel, 242 px with the file name, so neither has a stray
black band under the text.

The fade is the only animation in the player: `_start_fade()` schedules a
self-removing 67 ms timeout, and after the last render the plane is cleared and
the OSD buffer is blanked, so `kms-shot` reports 0 non-transparent pixels rather
than a stale image. Idle playback runs no timer for the OSD.

---

## Behaviour

| Key | Action |
|---|---|
| `ESC` / `Q` | Quit, restore the previous VT |
| `Space` / `PLAYPAUSE` | Play / pause (pause keeps the last frame) |
| `A` | Next audio track (cyclic), OSD `Audio : Français — VFF` |
| `S` | Next subtitle track (cyclic, includes `Désactivé`), OSD shows the language |
| `I` | Information panel (four lines + file name); a second `I` hides it, other keys leave it up |
| `←` / `→` | Cumulative seek, see below |
| `↑` / `↓` | Software gain ±2 % (starts at 100 %, ceiling 100 %; `KMS_PLAY_VOLUME_STEP` overrides) |
| `VOLUMEUP` / `VOLUMEDOWN` / `MUTE` | PipeWire sink volume (`wpctl`), OSD |
| `Enter` | Toggle the OSD |
| `NEXTSONG` / `PREVIOUSSONG` | Fixed ±60 s jump |

### Cumulative seek scale

Not a hold acceleration. Each accepted press jumps the current rung and advances
one rung, in the direction pressed:

| Press | 1 | 2 | 3 | 4 | 5 | 6+ |
|---|---|---|---|---|---|---|
| Jump | 1 s | 10 s | 30 s | 60 s | 120 s | 240 s |

* The index returns to 1 s after **1 s** without a press.
* The index returns to 1 s as soon as the **direction changes**.
* Autorepeat counts as successive presses (a held arrow walks the scale), with a
  **120 ms debounce** so one physical press cannot consume two rungs.
* The target is clamped to `[0, duration - 0.25 s]`; seeking to the very end would
  raise EOS and stop playback.

### Two independent volumes

* `↑`/`↓` → GStreamer `volume` element (software gain), 0-100 %, starts at 100 %.
* Multimedia keys → PipeWire sink volume, shared with the rest of the session.

They are intentionally separate: the first one is the player's own level, the
second one is the system level.

---

## Decode tiers

The player decodes in two passes, and only the first one is zero copy.

**Tier 1 — hardware, unchanged.** `playbin` picks the highest-ranked decoder and
`kmssink` takes its DMA-BUFs directly (`video caps: … format=DMA_DRM
memory=DMABuf`). Nothing is converted, nothing goes through system memory. This is
the path for everything the kernel really decodes.

**Tier 2 — software, on demand.** If tier 1 posts an error (the common one is
`not-negotiated (-4)`, because a hardware decoder cannot negotiate DMABuf caps with
`kmssink`) or never reaches `PLAYING` within `FALLBACK_START_TIMEOUT` (12 s), the
player rebuilds a `playbin` with software decoders and restarts at the position it
had reached:

* `video-sink="videoconvert ! kmssink plane-id=57"`, so system-memory frames still
  scan out on the video plane;
* `GST_PLAY_FLAG_FORCE_SW_DECODERS` and `force-sw-decoders` on `uridecodebin`;
* every decoder whose class contains `Hardware` demoted to rank `NONE` — on this
  GStreamer the first two were not enough, `v4l2slh264dec` (rank 257) was still
  selected inside `decodebin`, and rank is what auto-plugging obeys;
* a `FLUSH | KEY_UNIT` seek to the previous position, and the OSD label
  « Décodage logiciel ».

There is exactly one fallback attempt: a second failure stops playback with
« Lecture impossible » and `decode tier: none (software fallback failed)`. No loop.

The log states which tier was retained, on the first caps event:

```
decode tier: hardware (zero copy, DMA_DRM -> kmssink)
decode tier: software (reason: not-negotiated)
decode tier: software (confirmed, I420 in system memory)
```

### What lands where on this board (measured)

| Codec | Decoder | Tier | Caps at the sink | CPU, 1080p |
|---|---|---|---|---|
| H.265 10-bit (4K) | `v4l2slh265dec` | hardware | `DMA_DRM` / DMABuf | 0.19 core |
| AV1 | `v4l2slav1dec` | hardware | `DMA_DRM` / DMABuf | ~0.06 core |
| MPEG-2 | `v4l2slmpeg2dec` | hardware | `DMA_DRM` / DMABuf | ~0.09 core |
| VP8 | `v4l2slvp8dec` | hardware | `DMA_DRM` / DMABuf | ~0.03 core |
| H.264 8-bit | `avdec_h264` | software | `I420` / system | ~0.57 core |
| H.264 High 10 | `avdec_h264` | software | `I420_10LE` / system | ~0.85 core |
| H.264 8-bit, 4K | `avdec_h264` | software | `I420` / system | ~1.4 core (2160p30) |
| VP9 | `avdec_vp9` | software | `NV12` / system | ~0.31 core (720p) |

**VP8 is hardware too.** `v4l2slvp8dec` decodes a 1920x1080 clip at `DMA_DRM` in
zero copy, 0.03 core once running (0.20 at startup), exactly like
HEVC/AV1/MPEG-2 — tier 1 never falls back and the log confirms
`decode tier: hardware (zero copy, DMA_DRM -> kmssink)`.

**H.264 4K in software is sustainable, but only just.** A 3840x2160 10 s clip at
30 fps played from the first frame to EOS in 9.97 s of wall time, i.e. real time,
with a steady 1.3-1.7 core (average ~1.4). The ceiling was measured separately
(`avdec_h264 ! videoconvert ! fakesink sync=false`): 300 frames in 3.10 s, so
~97 fps at 5.1 cores. At 30 fps that leaves roughly a factor of three of headroom,
which a real 4K film — more entropy, higher bitrate, 60 fps or 10-bit H.264 — will
consume. 4K H.264 therefore still ends up in the software tier by design (the
hardware decoder cannot negotiate with `kmssink`), and it is the one case where the
fallback costs most of a core.

**VP9 has no hardware decoder on this kernel, and that is a kernel problem, not a
GStreamer one.** The rkvdec instance does not register (`missing multi-core support,
ignoring this instance`), so neither `v4l2slvp9dec` nor a mainline VP9 node exists:
`/dev/video2` is `rk3328-vpu-dec`, `/dev/video4` is `rk3588-av1-vpu-dec`. No VP9
decoder can be plugged because there is nothing to plug. Tier 1 already selects
`avdec_vp9` by itself in that case (there is no hardware decoder to outrank it), so the
log says `decode tier: software (reason: no hardware decoder selected, system memory)`
without any fallback.

**Why H.264 hardware does not work (measured, not guessed).** `v4l2slh264dec` fails in
`decide_allocation`: `DMABuf caps negotiated without the mandatory support of
VideoMeta`, then `Failed to negotiate with downstream`. Forcing system-memory caps
fails earlier (`No support for 1920x1080 format UNKNOWN`), and inserting `v4l2convert`
— which does fix the chain as far as `fakesink` — still fails once `kmssink` is the
sink. Making H.264 hardware work would need a `kmssink` that advertises
`GstVideoMeta` on its `DMA_DRM` pad, i.e. a change in the plugin, not in the player.
Hence the software tier.

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
   the OSD is displayed. The same output ends with one `connector` line per
   connector (`connector HDMI-A-1 connected crtc=81 mode=2560x1440 vrefresh=75`)
   and one `crtc` line per CRTC: that is where the banner's screen line comes
   from, and it is also the fastest way to confirm the mode the VOP is driving.
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
7. **Startup banner and I panel**: wait for `/dev/shm/kms-osd-<uid>.geom` to read
   `880x202` (the startup panel is up), then `kms-shot osd /tmp/banner.png`. It
   must show four text lines. Ten seconds later the geom is back to `Wx76` and
   `kms-shot osd` prints `0 non-transparent pixels`. Pressing a key while the
   startup panel is up must log `panel: hidden (startup, key …)` and show the
   bottom band instead. Pressing `I` must log `panel: shown (manual)` and produce
   a `880x242` capture with a fifth line (the file name); a second `I` must give
   `0 non-transparent pixels`, and arrows, `A`, `S`, `Q` must leave it up.

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
* `gst-libav` (`avdec_h264`, `avdec_h265`, `avdec_vp9`, `avdec_mpeg2video`) for the
  software tier. No package is installed by the player; it only uses what is there.

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

## Audio switch resilience

`current-audio` makes playbin rebuild the whole audio branch (decoder, resample,
sink). On this board that reconfiguration can post **ERROR** or even **EOS** from
the audio side while the video keeps running perfectly. Ending the film there is
wrong, so `recover_audio_switch()` catches these messages when they arrive within
`AUDIO_SWITCH_GRACE` (3 s) of a switch: it restores the previous track, re-anchors
the pipeline with a seek, shows "Piste audio indisponible" and keeps playing. Two
attempts maximum, then the message is treated normally.

A file with a single audio track is never reconfigured at all — pressing `A` just
recalls the label.

Self-test of that path on real hardware:

```sh
KMS_PLAY_AUDIO_SELFTEST=1 kms-play film.mkv    # then press A
```

It pretends the switch failed as soon as it succeeded; the log must show
`audio switch failed, restoring track N` and the video must keep advancing
(`plane[57] fb=` changes).

The previous run's log is kept as `last.log.prev` (rotated at startup): a crash
or a failed switch leaves no other trace.

## Decode tier self-tests

Two environment variables exercise the fallback path. Both are read at startup
and are inactive by default; nothing changes in normal use.

`KMS_PLAY_TIER_WATCHDOG_MS` shortens the watchdog that gives tier 1 before it is
declared stalled (default 12000 ms). It is the only way to hit that branch
quickly, because the usual fallback is a negotiation error, not a stall:

```sh
KMS_PLAY_TIER_WATCHDOG_MS=2000 kms-play /tmp/t-trunc.mkv   # head -c 200000 of a clip
```

The log must show `state ready, pending playing`, then
`decode tier: software (reason: tier 1 did not reach PLAYING in 2 s ...)`, then
either playback (`decode tier: software (confirmed, ...)`) or, if the software
tier fails too, `decode tier: none (software fallback failed)`.

`KMS_PLAY_FORCE_SOFT_AT=<seconds>` drops to the software tier once playback
reaches that position. Real H.264 failures happen at 0 s, so this is what
validates the resume seek:

```sh
KMS_PLAY_FORCE_SOFT_AT=5 kms-play clip.webm   # a hardware codec, e.g. VP8/HEVC
```

The log must show `test hook: forcing software tier at 0:05`, `fallback:
rebuilding playbin with software decoders at 0:05`, then `fallback: seek to
0:05 -> ok`, then `decode tier: software (confirmed, ...)`, and the video plane
must keep changing framebuffers (`plane[57] fb=`) after the switch.

## Keyboard layout

evdev gives **positions**, not characters: `KEY_A` is the second key of the home row
whatever the language (`KEY_*` codes follow the US physical layout). On an AZERTY
keyboard the key labelled `A` is at the US `Q` position and reports `KEY_Q`; reading the
US name literally made `A` do nothing and `Q` quit the film.

`kms-keys` therefore prints the raw evdev code as a fourth field:

```
key <NAME> <1|0|2> <code>
```

and `kms-play-py` translates it through the XKB layout declared in
`/etc/default/keyboard` (`XKBLAYOUT`), using `libxkbcommon` through `ctypes` — no extra
package, since any Wayland system already ships that library:

* the layout is resolved once at startup: `KMS_PLAY_KEYMAP`, then `XKBLAYOUT`, then
  `localectl status`, then `us`;
* only single-letter tokens are translated (`A`, `S`, `Q`); arrows, space, `ENTER`,
  media keys and `ESC` are position-independent;
* if the keymap cannot be compiled, the raw US name is used and the reason is logged —
  a broken layout must not make the player unusable.

Check it without a screen: `/usr/local/bin/kms-keys` prints the stream, and
`kms-inject A` (which emits evdev code 30, the US `A` position) must appear as `key Q`
under an AZERTY layout.

## Looking at the overlays: `kms-shot`

Nobody in this pipeline sits in front of the screen, but the overlays do not need an
eye: the player renders them itself into a cairo ARGB surface and writes that surface
to `/dev/shm/kms-osd-<uid>.raw` (bottom band 2560x76, or information panel 880x202 /
880x242) and
`/dev/shm/kms-sub-<uid>.raw` (band 2560x220) before handing the file name to
`kms-subd`. The matching `.geom` file holds `WxH`, which is how `kms-shot` knows
which rectangle a dump contains.

```sh
kms-shot osd /tmp/osd.png      # progress bar, volume bar, track label, info panel
kms-shot sub /tmp/sub.png      # current subtitle cue
kms-shot osd /tmp/x.png --size 880x242   # manual override if the geom file is gone
```

It prints `WxH, N non-transparent pixels, size from geom|--size|inferred`; `N = 0`
means the overlay was not showing, not that the render is broken. To inspect glyph
quality, crop to the non-transparent bounding box and scale up — a 3x crop shows the
anti-aliased outline clearly.

The **video plane is out of reach**: its framebuffer is an `NV15` DMABUF allocated by
the VPU, and the kernel does not expose it for reading. Picture quality stays a human
question.
