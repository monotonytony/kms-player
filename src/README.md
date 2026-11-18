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
| `kms-subd.c` | Long-lived daemon holding the DRM master. Drives the two ARGB planes; protocol on stdin (`sub cue …`, `osd cue …`, `sub clear`, `osd clear`, `off`, `quit`). `off` detaches the video, subtitle and OSD planes; it is what the exit path uses. |
| `kms-keys.c` | Reads every `/dev/input/event*` with one `poll()` loop and prints `key <NAME> <1\|0\|2>` lines on stdout (the table includes `I` for the information panel). **Owns autorepeat**: it drops kernel `value=2` events and synthesises its own (300 ms delay, 60 ms interval), so the behaviour does not depend on the keyboard's `EV_REP` capability or on the desktop's repeat settings. |
| `kms-planes.c` | Dumps the live DRM plane state (zpos, formats, attached framebuffer, position) and every connector's DRM name with the mode and vertical refresh actually scanned out on its CRTC. `--select` prints the three planes the player must use (see "Plane selection"), `--short` is machine-parsable read-back for the banner and the verification, `--check` dumps the topology. This is the only way to prove "the OSD is visible" without a screen. |
| `kms-inject.c` | uinput virtual keyboard for tests. Emits one key press with a monotonic timestamp so latency can be measured. |
| `measure-latency.py` | Measures keyboard → visible-effect latency by watching the OSD plane in `/sys/kernel/debug/dri/0/state`. |
| `measure-cpu.py` | CPU usage of the player process tree over N seconds. |
| `test-seek-scale.py` | Automated check of the cumulative seek scale (see below). |
| `kmsshare.c` | `LD_PRELOAD` shim that hands the DRM master to `kmssink` over a Unix socket (`SCM_RIGHTS`) and neutralises `drmSetMaster`/`drmDropMaster`. Without it, only one process could program planes. |
| `kms-keywait.c` | Legacy watcher kept for compatibility: blocks until ESC or Q. The current player does not use it, but older installations and scripts still reference it. |
| `drm-plane-formats.c` | Minimal diagnostic that prints the fourcc codes each plane accepts. |
| `kms-vt` | Root helper (`sudo -n`, the single NOPASSWD rule): switches/clears a VT and, for `show`, optionally prints a status line after the clear. |
| `kms-play` | Launcher: sets `LD_PRELOAD` and `KMS_SHARE_SOCK` and runs the installed player. |
| `kms-play-next` | Same, but runs everything from this source directory (used for testing before installing). |
| `install.sh` | Copies the compiled binaries and scripts to `/usr/local/bin` after a backup. |
| `kms-renderd` | UPnP/DLNA MediaRenderer: SSDP, description, SOAP and eventing, in the standard library only. Drives the player over its Unix control socket; never touches DRM or GStreamer. See "UPnP/DLNA renderer" below. |
| `kms-renderd.service` | systemd **user** unit for the renderer. |
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
bottom band is `screen × 76`, the startup panel is `content × 202` and the
on-demand panel with the file name is `content × 242` (see "Startup banner and
information panel" for how the content width is measured).

### Plane selection

`plane-select.h` is the single source of truth. It is included by `kms-planes.c`
and `kms-subd.c` (all functions `static`), so there is one implementation, not
two that can drift:

```
kms-planes --select        # video 57 81 NV15 4 / sub 69 ... / osd 75 ...
kms-subd --select          # same output, for a diff in the test suite
```

`kms-play-py` runs `kms-planes --select` once at startup and passes the ids to
`kmssink` (`plane-id=`) and to `kms-subd` (positional arguments plus the CRTC).
It no longer picks a plane itself, so the plane that is printed by hand is
always the plane that is programmed.

Selection, in order:

1. Connector = `KMS_PLAY_CONNECTOR`, else the first `connected` connector with
   a routed CRTC; otherwise `no connected display`.
2. CRTC = the connector's routed CRTC. Only planes whose `possible_crtcs` mask
   contains it are candidates (a multi-head board can no longer be given a
   plane of the other screen).
3. Video = `overlay` (or `primary` if no overlay exists) with NV15/NV12, lowest
   zpos; NV15 preferred. `cursor` planes are never candidates.
4. Sub/OSD = two distinct ARGB8888 overlays above the video plane; the two
   highest zpos are kept (highest = OSD, second = subtitles).
5. `zpos` is read, never forced. Immutable → the current value is the hardware
   order; the player logs `zpos immutable, hw order` and checks the read-back.
6. Degradation: one ARGB → video + OSD, subtitles disabled; none → video only;
   no video plane → error naming what is missing.
7. `KMS_PLAY_PLANES="video=…,sub=…,osd=…"` overrides roles (`none` disables).
   Unknown ids, ids of another CRTC, wrong formats and cursor planes are hard
   errors. `KMS_PLAY_PLANES_TOOL` is the binary path (a different variable, so
   the override string cannot be mistaken for a path).
8. After configuration the player reads `kms-planes --short`: `fb!=0` on the
   video plane, else one retry with the next `--list-video` candidate, then a
   clear log line (two attempts, no loop).

**A blanked screen is woken, not fatal.** Idle blanking deactivates the CRTC
(the connector stays `connected`, `crtc=0`, `dpms=Off`), so `--select` answers
`no connected display`. `select_planes()` recognises that message (and
`has no CRTC routed`), calls `wake_display()` — `kms-vt prepare 4`, because a
VT switch is what makes fbcon reprogram the CRTC — then retries: three
attempts, about a second apart, then the historical fatal message. Any other
selection error is not retried. `wake_display()` records the VT the viewer came
from in `PRE_SELECT_VT` and the desktop session's VT in `PRE_SELECT_DESKTOP`
before switching, so `switch_vt()`/`die()/exit_to_vt()` still restore the
desktop instead of mistaking the player's own VT for it. DRM's DPMS property is
not used: it needs the master the desktop holds, and it cannot recreate the
deactivated CRTC; `/sys/class/drm/*/dpms` is read-only on this kernel.

`kms-planes --check` dumps the topology behind those rules: connectors
(status, encoder, CRTC, mode, vrefresh), CRTCs (mode, active) and, per plane,
the DRM type, the zpos range with its immutable flag, the formats and
`possible_crtcs` decoded to CRTC ids. Measured on the BPI-M7: one CRTC (81),
seven overlay planes, `zpos` mutable `0..7`, planes 39/45/51 ARGB-only below
the video plane, 57 video (NV15), 69 sub, 75 osd; plane 63 is a third ARGB
candidate that stays free.

Limits worth knowing before trusting this on another board: the Rockchip driver
does not expose `CRTC_W`/`CRTC_H` or the plane position (`pos` reads `+0+0`), so
screen coverage is not readable and only `fb!=0` is verified; a driver without
a usable `type` property is treated as `unknown`; and on a single-CRTC board the
"plane of another CRTC" refusal cannot be exercised because every mask is `0x1`.

### Display mode: `kms-subd` is the only caller of `drmModeSetCrtc`

The VT switch lets fbdev emulation reprogram the CRTC (75 Hz on the desktop
becomes 60 Hz on the player's VT) and `kmssink` never mode-sets, so any other
rate has to be programmed by the process holding the DRM master: `kms-subd`.
It serves `<sock>.ctl` next to the fd-passing socket, one request line and one
reply line per connection (`current`, `rates`, `mode W H rate`,
`mode remember W H rate`, `mode restore`, `vblank`). Selection is by resolution
(the active one) then by closest rate within 1 Hz; the mode is programmed at
`fb = cur->buffer_id`, then **read back** with `drmModeGetCrtc`, and a mismatch
reverts to the previous mode before answering `err verify`. `rates` and every
`err` list what the connector actually offers, so the player can explain a
refusal instead of guessing.

`kms-play-py` (`_setup_refresh`, `_apply_refresh`, `_restore_refresh`) applies
the mode after `switch_vt()` and before `PLAYING`, chooses `k` in `2..6` such
that `k x fps` is within 0.5 % of an available mode, and restores the previous
mode before `exit_to_vt()`. `KMS_PLAY_REFRESH=auto|off|<Hz>` and
`KMS_PLAY_STATS=1` are documented in the top-level README; no failure here can
stop playback, the worst case is one log line and the mode already running.

On this board the panel's 119.998/144.002 Hz EDID timings are dropped by the
kernel (mode list capped around 340 MHz TMDS) and a direct `drmModeSetCrtc` with
the EDID timing returns `EINVAL`, so the player keeps 60 Hz and says so. The
path is tested with 75 Hz (a mode that exists): applied, verified by read-back,
measured at 74.98-75.20 Hz from the vblank counter, restored to 59.968 Hz on
exit.

### Startup banner and information panel

Once the decode tier and the video caps are known, `OnScreenDisplay` draws a
four-line panel at `+40+40`, for 8 s, then fades it over 0.8 s (12 renders at
67 ms) and clears the plane. The box is as wide as its content: the widest
line's ink (`get_pixel_extents()[1].width`, `ink.x` included for RTL) plus
`2 * PANEL_MARGIN` (22 px each side), clamped to
`[PANEL_MIN_WIDTH, PANEL_MAX_WIDTH]` = `[240, 880]`. On this board the four
startup lines measure 354 px of ink, so the box is `398x202`; only a line that
had to be shortened reaches the 880 cap (a long file name in the I panel).

```
HARDWARE · zero-copy        <- tier: HARDWARE·zero-copy / SOFTWARE·CPU fallback
H.265 Main 10 · 3840x1600 · 23.976 fps   <- codec+profile, size+rate
Plane 57 · NV15 · DMABuf · zpos 4        <- plane id, fourcc, memory, zpos
HDMI-A-1 · 2560x1440 @ 75 Hz             <- connector, live mode, vrefresh
47.Ronin.2013.MULTI…x265-TAC.mkv         <- only on demand (key I)
```

Every label in that panel comes from the string table (see "Language" below);
with `KMS_PLAY_LANG=en` it reads exactly as shown. The values never change with
the language: a codec name, a DRM fourcc and a connector name are facts.

Sources, all read at the moment the banner is drawn:

* codec and profile: the compressed caps on the decoder's sink pad, found by a
  single walk of the pipeline tree (`video/x-h265, profile=main-10` becomes
  `H.265 Main 10`); ffprobe's stream metadata is the fallback when the decoder
  cannot be located;
* width, height, frame rate: the raw caps that reached the video sink;
* plane format and zpos: `kms-planes --short` for the video plane — the DRM
  fourcc (`NV15`) and the kernel's zpos, not our own bookkeeping. When the caps
  format differs from what the plane scans out (software tier: `I420` through
  videoconvert becomes `NV12`), both are shown: `NV12 (I420 negotiated)`;
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
black band under the text. The width does not change during the fade: the text
and the box are measured once by `_measure_panel()` and every one of the twelve
renders reuses them, so the box cannot tremble while it fades. The surface
cache is keyed on `(width, height)` and is recreated, never resized in place,
when a panel with a different content size is shown.

The background is a translucent black box at `PANEL_BG_ALPHA` (0.72) multiplied
by the fade alpha. `KMS_PLAY_PANEL_BG` (0..1) overrides it at run time; `0`
paints no background at all, leaving white text over the picture, and an
invalid value is ignored with a log line. The bottom band is independent and
keeps its 0.55 background and its full width.

The connector mode line is re-read (`display_info()`) every time the panel is
drawn, so it shows the live CRTC mode instead of the one seen before the VT
switch. Measured: the desktop's 75 Hz becomes 60 Hz as soon as the console VT is
activated, because the kernel's fbdev emulation reprograms the CRTC; `kmssink`
then keeps that mode (`force-modesetting=false`, its default, set explicitly).
The 75 Hz returns on exit. Nothing in the player programs a CRTC mode.

The video plane is verified after the first frames. kmssink only attaches a
framebuffer when a real buffer reaches it, which on 4K HEVC can take several
seconds, so the check polls `kms-planes --short` every 1.5 s for up to 6 s and
logs `first buffer after N s`. Only after that bounded wait, if the plane is
still empty, does it switch to the next `--list-video` candidate (once) and wait
again; a second empty plane is logged and playback continues. The kmssink
element is kept as a direct reference at pipeline build time, because a
`GstPlayBin` has no `GstPipeline.iterate()`; the tree walk is only a fallback.
The timeout callback is guarded, so no exception can escape from it and kill
the player.

The fade is the only animation in the player: `_start_fade()` schedules a
self-removing 67 ms timeout, and after the last render the plane is cleared and
the OSD buffer is blanked, so `kms-shot` reports 0 non-transparent pixels rather
than a stale image. Idle playback runs no timer for the OSD.

### Language

Every string the viewer can read — the panel, the bottom band
(`Pause`, `Décodage logiciel`, `Audio : …`), the track language names and the
displayed error messages — comes from `STRINGS` in `src/kms-play-py`: one
positional table of 45 entries per language, validated at import (a short tuple
or a changed placeholder stops the player with a message rather than shifting
labels on screen). 30 entries ship: `en fr de es pt it nl sv pl ru uk cs ro el
tr ar fa he hi bn ta vi id ms th ja ko` plus `zh-Hans` (CN/SG), `zh-Hant`
(TW/MO) and `zh-HK`.

Selection order: `KMS_PLAY_LANG`, then `LC_MESSAGES`, `LANG`, `LANGUAGE`, then
English. Locale tags are normalised (`fr_FR.UTF-8` → `fr`, `ja_JP.utf8` → `ja`,
`zh_TW` → `zh-Hant`, `zh_HK` → `zh-HK`, `zh_CN` → `zh-Hans`, `pt_BR` → `pt`), and
an unknown language falls back to English:

```
KMS_PLAY_LANG=ko kms-play film.mkv     # Korean panel
LANG=ja_JP.UTF-8 kms-play film.mkv     # Japanese, no explicit variable
LANG=xx_YY kms-play film.mkv           # English
```

Two guards keep the screen honest, both measured rather than assumed:

* a missing key (partial translation) falls back to its English form;
* before a string is drawn, `tr()` counts its unknown glyphs with the
  language's font (`Pango.Layout.get_unknown_glyphs_count()`); if any is
  missing, the English form is used instead and the event is logged once as
  `i18n: ko missing glyphs, falling back to en (osd.pause)`. A partially
  installed font set therefore shows English, never tofu.

The font family is chosen per language — `Noto Sans CJK KR/JP/SC/TC/HK`,
`Noto Sans Thai`, `Devanagari`, `Bengali`, `Tamil`, `Arabic` — because
fontconfig's fallback does not honour the regional CJK variants: measured on
this image, `fc-match :charset=9F8D` (龍) answers `Noto Sans CJK JP`, i.e. a
Traditional character would be drawn with Japanese glyph shapes.

Arabic, Persian and Hebrew read right to left: their panel is flush right, and
the plane line is drawn as one right-aligned block — its translated label by an
RTL layout, its technical values (`57 · NV15 · DMABuf · zpos 4`) by a separate
LTR layout placed to the left of the label. The split is not decoration: Pango
ignores the LRE/PDF embedding marks that would have isolated the values inside
one paragraph, and a single RTL paragraph put the plane number on the far side
of the line (measured with `index_to_pos`). Lines with no label of their own
(codec, screen, file name) are drawn LTR and flush right, so the panel keeps a
single visual edge.

`src/test-i18n.py` renders the panel for all 30 entries offline (cairo surface,
no DRM, no screen) through the player's own `OnScreenDisplay`, reports the
unknown-glyph count per language and writes a PNG for each non-Latin script; it
also checks the detection order. Run it on the target:

```
python3 src/test-i18n.py --out /tmp/i18n-panels
30 languages, 0 failure(s); PNGs in /tmp/i18n-panels
```

### Exit, and why the screen comes back

A plane outlives the process that programmed it. The video plane is a
full-screen overlay: if it is still attached when the player exits, it covers
fbcon and the viewer gets a black screen on the television with no process left
to blame. Nothing in the GStreamer teardown guarantees otherwise, and
`kmssink`'s `restore-crtc` (default **true**, set explicitly in the pipeline
description so a changed default cannot reintroduce the bug) restores the CRTC
mode, not the plane.

So every exit path — `Q`, `ESC`, end of stream, error, `SIGTERM`/`SIGINT` —
runs the same three steps, in this order:

1. `_teardown_pipeline()`: `set_state(NULL)` then `get_state(2 s)`, i.e. the
   transition is **awaited**, not assumed. `kmssink` frees its plane while
   handling it, so releasing the plane earlier races with the sink and can
   re-enable it. The result is logged: `pipeline: NULL in 121 ms`, or
   `pipeline: teardown slow, state=…` when it does not finish in time;
2. `_release_planes()`: `kms-subd` (the DRM master) receives `off` and calls
   `drmModeSetPlane(…, fb_id=0, …)` on the video plane, the subtitle plane and
   the OSD plane. Its teardown does the same again if the player dies abruptly;
3. read-back: `kms-planes --short` is parsed and the release is only considered
   done when no plane has a framebuffer any more (`fb=0`, `pos=+0+0`). Three
   attempts, 200 ms apart, then a logged `planes: WARNING still programmed`.

`run()` repeats steps 1–2 if the main loop ever ends without `stop()`, so a path
that quits the loop directly cannot leave a plane on. A run ends with
`exit, VT 5 | planes released: True`.

What the viewer sees is checked the same way, from the kernel's own state:

```
# player stopped, after ESC, Q, and EOS respectively
grep -E "^plane\[(33|57|75|69)\]" -A4 /sys/kernel/debug/dri/0/state
plane[33]: Cluster0-win0      crtc=video_port0   fb=86            <- fbcon, never touched
plane[57]: Esmart0-win0       crtc=(null)        fb=0   crtc-pos=0x0+0+0   <- video
plane[69]: Esmart2-win0       crtc=(null)        fb=0   crtc-pos=0x0+0+0   <- subtitles
plane[75]: Esmart3-win0       crtc=(null)        fb=0   crtc-pos=0x0+0+0   <- OSD
```

The player's own VT gets a status line, because a cleared VT is
indistinguishable from a crash: `kms-vt show VT TEXT` clears, shows the cursor
and prints one line, and the player passes `playback finished: <file>` (EOS) or
`playback stopped: <file>` (a key, a signal, an error). That line is
**ASCII English whatever the language**: the kernel console font has no
non-Latin coverage, so a translated line would come back as question marks.
The two strings live in the language table like everything else, filled for
`en` only, which makes `tr()` fall back to English for the other 29 entries.
The viewer does not see that line: they are sent back to the desktop, and the
line stays readable in `/dev/vcsa<player VT>` and in the log as the proof that
playback ended cleanly.

#### Where the viewer lands

A console prompt after a film is alarming - it looks like something crashed.
So the player returns to the desktop VT (GNOME runs on `tty2` here), resolved
**once at startup**, in this order:

1. `KMS_PLAY_RETURN_VT=N` when the variable holds a number (pin the target,
   e.g. in a test). `KMS_PLAY_RETURN_VT=none` disables the return entirely and
   keeps the historical behaviour: stay on the player's VT, which is the right
   answer on a machine with no desktop;
2. the VT of the active graphical session of `seat0`, asked from logind
   (`loginctl show-seat seat0 -p ActiveSession`, then
   `loginctl show-session … -p VTNr`), ignored when empty or non-numeric;
3. the VT that was active before the player took the screen
   (`/sys/class/tty/tty0/active` read at startup);
4. `1`.

Step 2 is read **before** the player takes its VT, and also before
`wake_display()` switches VT: once the console has moved, logind reports an
empty `ActiveSession` for the seat and the desktop would be mislabelled as a
plain console. When the screen had to be woken, `PRE_SELECT_VT` supplies
step 3 and `PRE_SELECT_DESKTOP` supplies step 2, so a wake does not change
where the viewer lands - the log still says `previous tty2, taken while waking
the display` and `exit, VT 2 (desktop)`.

At exit, after the planes are released, three guards apply before any
`chvt`:

* the player must still own the screen: if `/sys/class/tty/tty0/active` is no
  longer its own VT, the user changed VT during playback and that choice is
  respected - `exit, VT untouched: user switched to tty1`, no switch;
* the target must exist (`/dev/ttyN`) and must not be the player's VT;
* the result is read back, then logged as `exit, VT 2 (desktop)` or
  `exit, VT 1 (console)`, or `exit, VT 2 (desktop) not confirmed, active ttyN`
  when the switch did not take.

A normal run therefore ends with:

```
[91110.835] VT: previous tty2, returning to 2 (desktop)
[91119.848] pipeline: NULL in 122 ms
[91119.851] planes: released, no framebuffer left on any plane
[91120.081] exit, VT 2 (desktop)
[91120.081] exit: previous tty2 | planes released: True
```

and the console is left with `tty2` active, `gnome-shell` still running, and no
plane programmed.

---

## Behaviour

| Key | Action |
|---|---|
| `ESC` / `Q` | Quit, restore the previous VT |
| `Space` / `PLAYPAUSE` | Play / pause (pause keeps the last frame) |
| `A` | Next audio track (cyclic), OSD `Audio : Français — VFF` |
| `S` | Next subtitle track (cyclic, includes "off"), OSD shows the track language |
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

**Tier 1 — hardware, playbin3.** `playbin3` picks the highest-ranked decoder through
`decodebin3` and `kmssink` takes its DMA-BUFs directly (`video caps: … format=DMA_DRM
memory=DMABuf`). Nothing is converted, nothing goes through system memory. This is the
single route for every codec on this board since 2026-11-18, H.264 included.

**Tier 2 — software, on demand.** If tier 1 posts an error (the common one is
`not-negotiated (-4)`, because a hardware decoder cannot negotiate DMABuf caps with
`kmssink`) or never reaches `PLAYING` within `FALLBACK_START_TIMEOUT` (12 s), the
player rebuilds the same playbin3 route with software decoders:

* flags `0x1057` (`PLAYBIN3_FLAGS | FORCE_SW_DECODERS`) and
  `video-sink="queue ! videoconvert ! kmssink plane-id=57"`, so system-memory frames
  still scan out on the video plane;
* every decoder whose class contains `Hardware` demoted to rank `NONE` — that is what
  actually works here: `decodebin3` has **no** `force-sw-decoders` property (checked with
  `gst-inspect`, GStreamer 1.28), unlike the old `uridecodebin`;
* a `FLUSH | KEY_UNIT` seek back to the previous position, **deferred to two seconds
  after the first software frame**: playbin3 refuses a seek before preroll, and a seek
  sent inside the preroll window froze the pipeline and the main loop (measured). With
  the delay, the same seek answers in ~80 ms;
* the OSD label « Décodage logiciel ».

There is exactly one fallback attempt: a second failure stops playback with
« Lecture impossible » and `decode tier: none (software fallback failed)`. No loop.

The log states which tier was retained, on the first caps event:

```
decode tier: hardware (zero copy, DMA_DRM -> kmssink)
decode tier: software (reason: not-negotiated)
decode tier: software (confirmed, I420 in system memory)
```

### playbin3: the flags, and why each one is there

`PLAYBIN3_FLAGS = 0x57` — `VIDEO (1) | AUDIO (2) | TEXT (4) | SOFT_VOLUME (0x10) |
NATIVE_VIDEO (0x40)`. Measured on the BPI-M7 with the 4K H.264 film, the 4K HEVC 10-bit
sample and the 4K AV1 sample:

| Bit | Effect if wrong |
|---|---|
| `NATIVE_VIDEO` | without it playbin3 inserts its own converter: the sink sees `memory:SystemMemory`, one copy per frame |
| `NATIVE_AUDIO` | must stay **off**: with it the audio branch fails with `not-negotiated` and no audio buffer ever flows |
| `TEXT` | needed for the text streams to appear in `STREAM_COLLECTION`; the text-sink is a `fakesink`, the subtitles are drawn on the "sub" plane |
| `SOFT_VOLUME` | keeps playsink's software volume; the player's own gain element is inside the audio-sink bin |

The audio sink is the other half of the recipe:

```
audioconvert ! volume name=gain ! audioresample ! pipewiresink
```

`audioconvert` **before** `volume`: decodebin3 hands out a non-interleaved F32 layout
that `volume` refuses, and that refusal surfaced as `not-negotiated` at the demuxer,
ending the film on its first audio buffer. With this order the sink sees
`audio/x-raw rate=48000` and buffers flow continuously (counted by a probe on the audio
bin and reported as `audio_buffers=` in the stats line).

This also explains the earlier "playbin3 fails for H.264" measurement: that test had a
plain sink on the audio side, so the failure came from the audio branch, not from the
video decoder. Same decoder, same sink, same zero copy once the audio chain is right.

### Audio track changes: `select-streams`

playbin3 exposes no `current-audio` property and no `n-audio` count. The player reads
the `STREAM_COLLECTION` bus message, keeps the ids per kind (container order), and
switches with `Gst.Event.new_select_streams([video ids] + [first text id] + [audio id])`
sent on the pipeline. The selection **replaces** everything: without the video ids the
picture would be disabled, so nothing is sent until the collection has arrived. The ids
are 96-character container ids sharing a prefix, which differ only in their `/NNN:…`
suffix — the log prints that suffix, and the 4K film's two E-AC-3 tracks read
`002:…` (French) and `003:…` (English).

### What lands where on this board (measured)

| Codec | Decoder | Tier | Caps at the sink | CPU (1080p unless stated) |
|---|---|---|---|---|
| H.265 10-bit (4K) | `v4l2slh265dec` | hardware | `DMA_DRM` / DMABuf | 0.19 core |
| H.265 8-bit (4K) | `v4l2slh265dec` | hardware | `DMA_DRM` / DMABuf, plane `NV12` | — |
| H.265 10-bit HDR10 (4K) | `v4l2slh265dec` | hardware | `DMA_DRM` / DMABuf, plane `NV15` | — |
| AV1 | `v4l2slav1dec` | hardware | `DMA_DRM` / DMABuf | ~0.06 core |
| AV1 10-bit, HDR10 (4K) | `v4l2slav1dec` | hardware | `DMA_DRM` / DMABuf, plane `NV15` | — |
| MPEG-2 | `v4l2slmpeg2dec` | hardware | `DMA_DRM` / DMABuf | ~0.09 core |
| VP8 | `v4l2slvp8dec` | hardware | `DMA_DRM` / DMABuf | ~0.03 core |
| H.264 8-bit | `v4l2slh264dec` | hardware | `DMA_DRM` / DMABuf | 0.12 core (4K60) |
| H.264 High 10 | `avdec_h264` | software | `I420_10LE` / system | ~0.85 core |
| H.264 8-bit, 4K | `v4l2slh264dec` | hardware | `DMA_DRM` / DMABuf | 0.12 core (2160p60) |
| VP9 | `avdec_vp9` | software | `NV12` / system | ~0.31 core (720p) |

**VP8 is hardware too.** `v4l2slvp8dec` decodes a 1920x1080 clip at `DMA_DRM` in
zero copy, 0.03 core once running (0.20 at startup), exactly like
HEVC/AV1/MPEG-2 — tier 1 never falls back and the log confirms
`decode tier: hardware (zero copy, DMA_DRM -> kmssink)`.

**H.264 is hardware too, through playbin3.** The old conclusion — that `v4l2slh264dec`
cannot hand DMABufs to `kmssink` on this board — was only true of the legacy `decodebin2`
inside playbin, and the culprit was not kmssink. Measured on a 3840x2160 60 fps AVC
sample (147 Mbit/s):

| Pipeline | Result |
|---|---|
| `v4l2slh264dec ! kmssink` | hardware, `DMA_DRM` NV12, plane 57 programmed, 0.10 core |
| `v4l2slh264dec ! queue ! kmssink` | same, a queue does not break the negotiation |
| `decodebin ! kmssink`, `decodebin3 ! kmssink`, `uridecodebin ! queue ! kmssink` | hardware, `DMA_DRM` |
| `uridecodebin3 ! queue ! kmssink` | hardware, `DMA_DRM` |
| `playbin` (`video-sink=kmssink`), playbin2 **and** `playbin3` with a plain audio sink | **fails**: `not-negotiated`, decoder says `DMABuf caps negotiated without the mandatory support of VideoMeta`, kmssink's `propose_allocation` is never called |
| `playbin3`, flags `0x57`, audio bin `audioconvert ! volume ! audioresample ! pipewiresink` | **hardware, zero copy**: `video/x-raw format=DMA_DRM drm-format=NV12 3840x2160 24/1`, `memory:DMABuf` |

The decoder and the sink were always compatible. Two things broke the old attempts:
playbin's own uridecodebin still contains the legacy `decodebin2`, which loses the
decoder's allocation query (the decoder treats that as fatal for `DMA_DRM` caps instead
of falling back); and the audio branch of those test pipelines had no `audioconvert`
before `volume`, so the demuxer died of `not-negotiated` on the first audio buffer and
took the video with it. With `decodebin3` (playbin3) and the audio chain in the right
order, the same decoder scans out NV12 DMA-BUF frames on plane 57 with no copy.

For files with no audio track nothing special is needed any more: playbin3 simply never
creates an audio stream for them and playsink builds no audio sink. Measured with the
seven video-only Jellyfin samples (H.264, HEVC 8-bit, HEVC 10-bit, HEVC HDR10, AV1
10-bit, AV1 HDR10): each plays, seeks, pauses and reaches `end of stream (EOS)` with
`audio_buffers=0` and no error. The HEVC 8-bit sample also carries a `data` stream,
which playbin3 lists in the collection, never links, and never complains about.

HDR10 is a pass-through: the decoder outputs `NV15`, kmssink scans it out, and the
PQ/BT.2020 signal reaches the panel untouched. There is no tone mapping — on an SDR
screen the picture looks washed out, by design rather than by failure.

The software tier remains the answer for H.264 High 10 (no hardware decoder for that
profile) and VP9.

**VP9 has no hardware decoder on this kernel, and that is a kernel problem, not a
GStreamer one.** The rkvdec instance does not register (`missing multi-core support,
ignoring this instance`), so neither `v4l2slvp9dec` nor a mainline VP9 node exists:
`/dev/video2` is `rk3328-vpu-dec`, `/dev/video4` is `rk3588-av1-vpu-dec`. No VP9
decoder can be plugged because there is nothing to plug. Tier 1 already selects
`avdec_vp9` by itself in that case (there is no hardware decoder to outrank it), so the
log says `decode tier: software (reason: no hardware decoder selected, system memory)`
without any fallback.

**The H.264 diagnosis that was wrong, and how it was settled.** `v4l2slh264dec` fails
inside playbin (legacy `decodebin2`) in `decide_allocation`: `DMABuf caps negotiated
without the mandatory support of VideoMeta`, then `Failed to negotiate with downstream`.
That message is a plugin check on the allocation query — it is not a kmssink limitation:
kmssink does support `GstVideoMeta` (the installed library imports
`gst_query_add_allocation_meta` and logs `adding GstVideoMeta`), and the decoder's check
passes whenever the sink is its direct peer. Replacing the transport settles it:
`decodebin3` — which is what playbin3 builds — negotiates the same decoder to the same
sink in zero copy. Forcing system-memory caps still fails (`No support for 1920x1080
format UNKNOWN`), which is why an intermediate `videoconvert` is not a workaround either.

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

# a remote stream (UPnP hands out http:// URLs); subtitles are skipped
kms-play http://192.168.10.14:8000/film.mp4

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

## UPnP/DLNA renderer: `kms-renderd`

`kms-renderd` is a Digital Media Renderer (DMR, `MediaRenderer:1`). A phone, a
TV app or Home Assistant lists it in the "play on" menu, sends a URL, and the
film starts on the three planes exactly as if it had been launched from the
console. It renders only: there is no media server, no library browsing and no
transcoding.

```
control point --SSDP / HTTP / SOAP--> kms-renderd --unix socket, JSON--> kms-play-py
                                                                          |
                                                              kms-subd (DRM master)
                                                              GStreamer -> kmssink
```

### Why not Rygel, gmrender-resurrect or upmpdcli

They own their GStreamer pipeline. Using one would drop the zero-copy
NV15/NV12 plane (their sink is not `kmssink` with our plane ids), the two ARGB
planes that draw the subtitles and the OSD, the decode-tier fallback and the
VT/exit handling — i.e. the whole point of this player. A renderer is a state
machine plus two protocols, so it is written here with the Python standard
library only: no new dependency, nothing to install, and the player stays the
only thing that touches DRM.

### What the renderer implements

| Endpoint | Role |
|---|---|
| `/description.xml` | device description: `MediaRenderer:1`, three services, `DMR-1.50` |
| `/avt/scpd.xml`, `/avt/control`, `/avt/event` | AVTransport |
| `/rcs/scpd.xml`, `/rcs/control`, `/rcs/event` | RenderingControl |
| `/cm/scpd.xml`, `/cm/control`, `/cm/event` | ConnectionManager |
| UDP 1900 | SSDP: `M-SEARCH` answers, `NOTIFY ssdp:alive` at start and every `max-age/2`, `ssdp:byebye` on exit |

AVTransport: `SetAVTransportURI`, `SetNextAVTransportURI` (stored, not
chained), `Play`, `Pause`, `Stop`, `Seek` (`REL_TIME`/`ABS_TIME`), `Next`,
`Previous`, `GetTransportInfo`, `GetPositionInfo`, `GetMediaInfo`,
`GetDeviceCapabilities`, `GetTransportSettings`,
`GetCurrentTransportActions`.
RenderingControl: `GetVolume`, `SetVolume`, `GetMute`, `SetMute`,
`ListPresets`, `SelectPreset`.
ConnectionManager: `GetProtocolInfo`, `GetCurrentConnectionIDs`,
`GetCurrentConnectionInfo`.

Failures use the UPnP fault form with `701` (transition not available), `402`
(invalid args), `710` (seek mode not supported), `718` (invalid InstanceID),
`401` (invalid action) and `717` (play speed not supported).

Eventing answers `SUBSCRIBE` with a SID and a `Timeout` and accepts renewal
and `UNSUBSCRIBE`; it does not send `NOTIFY` state events yet, so clients fall
back to polling — BubbleUPnP and VLC are content with that, and
`GetTransportInfo`/`GetPositionInfo` are always live.

### Lifecycle

1. Idle: SSDP answers, HTTP serves, no player process exists.
2. `SetAVTransportURI` stores the URL (schemes `http`, `https`, `upnp`,
   `rtsp`; `file://` is refused, a control point must not name a local path)
   and reports `STOPPED`. A URI arriving while a film plays stops the player
   first.
3. `Play` starts `kms-play` with the URL as its only argument. The player
   switches to VT4, builds the three planes and plays; the renderer reports
   `TRANSITIONING` until the bus confirms `PLAYING`.
4. `Pause`, `Seek`, `SetVolume` and `SetMute` are forwarded to the player's
   Unix control socket. State and position are polled there every 0.5 s, so
   `GetPositionInfo` is live to half a second.
5. `Stop`, end of stream or a decode error: the player tears its planes down
   and exits to the desktop VT; the renderer reports `STOPPED` and stays in
   the idle state. A player killed from outside is detected the same way.

### The control socket (player side)

`kms-play-py` listens on a Unix socket, by default
`/run/user/<uid>/kms-play.sock` (override: `KMS_PLAY_CTL_SOCK`), mode `0600`,
created at start and unlinked at exit. One JSON request per line, one JSON
reply per line:

```json
{"cmd": "status"}
{"ok": true, "state": "playing", "position": 12.4, "duration": 1452.0,
 "volume": 100, "muted": false, "audio_tracks": [{"index": 0, "label": "English"}],
 "current_audio": 0, "uri": "http://…", "tier": 1,
 "video": {"width": 3840, "height": 2160, "drm_format": "NV15", "memory": "DMABuf"}}
```

Commands: `status`, `play`, `pause`, `resume`, `stop`, `seek`
(`{"position": seconds}`), `volume` (`{"volume": 0..100}`), `audio_track`
(`{"index": n}`), `quit`. Every request is marshalled onto the GLib main
loop, which owns GStreamer; the socket thread never calls the pipeline. The
socket is also usable by hand:

```sh
printf '{"cmd":"status"}\n' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/kms-play.sock
```

### Security model

UPnP has **no authentication and no transport security**: anything that can
reach the HTTP port or the SSDP socket can start a playback and seek. The
daemon bounds the surface instead of pretending to secure it:

* **Source filtering.** SSDP `M-SEARCH` and every HTTP method are accepted
  only from private, link-local and loopback addresses (`10/8`, `172.16/12`,
  `192.168/16`, `169.254/16`, `127/8`, `fe80::/10`, `fc00::/7`, `::1`),
  IPv4-mapped IPv6 included. Anything else is refused and logged.
* **Bounded input.** HTTP bodies are capped at 64 KiB, chunked requests are
  refused, and a request with a bad `Content-Length` is a 400.
* **Hardened XML.** `xml.etree` only; any document containing `DOCTYPE` or
  `ENTITY` is rejected before parsing, so no external entity is resolved and
  no entity expansion can be used as a bomb.
* **No writes, no shell.** No file is written on behalf of a request and no
  shell command is built from a received string: the player is started with an
  argv list, the URI is one element of it.
* **One player.** A single process at a time, owned by the renderer; its
  stdin/stdout/stderr are not connected to the network.
* **Timeouts and isolation.** Every socket has a timeout and every request a
  `try/except`: a malformed request is answered, never fatal.
* **The port must never be forwarded on the internet.** There is nothing to
  gain and everything to lose: a remote attacker could start playback on the
  screen. If the renderer must be reachable from another VLAN, put an
  authenticated reverse proxy in front of it or restrict it with firewall
  rules; do not expose it.

### Install

`kms-renderd` is a user service: the player must run as the desktop user,
because root's `XDG_RUNTIME_DIR` and PipeWire session are not the ones the
audio sink needs.

```sh
sudo install -m 0755 src/kms-renderd /usr/local/bin/kms-renderd
mkdir -p ~/.config/systemd/user
cp src/kms-renderd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now kms-renderd.service
# optional: start at boot without a login session
loginctl enable-linger armbian
```

Options: `--http-port` (default 8200), `--friendly-name`, `--player` (default
`/usr/local/bin/kms-play`), `--control-socket` (default
`/run/user/<uid>/kms-play.sock`), `--uuid`, `--log-level`. The environment
variables `KMS_RENDERD_HTTP_PORT`, `KMS_RENDERD_NAME`, `KMS_RENDERD_PLAYER`
and `KMS_RENDERD_CONTROL_SOCKET` do the same. The UDN is stable: it is cached
in `~/.cache/kms-renderd/uuid`, so clients keep recognising the renderer
across restarts.

The player's own `install.sh` still provides `/usr/local/sbin/kms-vt` and its
`sudo -n` rule; the renderer needs nothing else.

### Verifying it from another machine

```sh
# 1. SSDP: the renderer must answer with a LOCATION on the board's address
python3 - <<'EOF'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
s.sendto(b'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n'
         b'MAN: "ssdp:discover"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n',
         ("239.255.255.250", 1900))
print(s.recvfrom(4096)[0].decode())
EOF

# 2. description and SCPDs
curl -s http://192.168.10.14:8200/description.xml | head -30

# 3. a full playback, with curl
curl -s -X POST http://192.168.10.14:8200/avt/control \
  -H 'Content-Type: text/xml; charset="utf-8"' \
  -H 'SOAPAction: "urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI"' \
  --data '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:SetAVTransportURI xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID><CurrentURI>http://192.168.10.14:8000/film.mp4</CurrentURI><CurrentURIMetaData></CurrentURIMetaData></u:SetAVTransportURI></s:Body></s:Envelope>'
```

Then `Play`, and watch `~/.cache/kms-play/last.log`: it must show
`decode tier: hardware`, `video caps: … memory=DMABuf` and the plane ids.
`kms-planes --short` must show `fb=0` on every plane after `Stop`, and the
active VT must be back on the desktop (`cat /sys/class/tty/tty0/active`).

Measured on the BPI-M7 (19/11/2026), 4K HEVC 10-bit and 4K H.264, both served
over HTTP with Range support:

| Check | Result |
|---|---|
| M-SEARCH / `description.xml` / three SCPDs | answered, valid XML |
| `SetAVTransportURI` + `Play` | `STOPPED` → `TRANSITIONING` → `PLAYING` |
| decode tier / caps | `hardware`, `format=DMA_DRM memory=DMABuf 3840x2160` |
| `Pause` | position identical 2 s apart |
| `Seek` 10 s | applied in 16 ms, position 0:00:12 afterwards |
| `SetVolume 42` / `GetVolume` | 42, software gain applied to the pipeline |
| `Stop` | planes `fb=0`, VT back to `tty2`, no player process |
| dead URI | `STOPPED`, no zombie player, daemon alive |
| body > 64 KiB / invalid XML / DOCTYPE | 413 / 400 / 400, daemon alive |

### Known limits

* **No authentication**: see the security model; private networks only.
* **No eventing NOTIFY**: subscriptions exist, state is polled by clients.
* **No media server**: the renderer cannot browse a library; the control point
  supplies the URL.
* **No transcoding**: `SinkProtocolInfo` lists what GStreamer can decode. A
  server that only offers an unsupported codec will fail, as it would from
  the console.
* **Range support is required** for MP4 files whose `moov` atom is at the end
  (most non-faststart files): qtdemux seeks to the tail to read the index.
  Measured failure with `python3 -m http.server`, which ignores `Range`:
  `no 'moov' atom within the first 10 MB`. Real DLNA servers (Jellyfin,
  MiniDLNA, Plex) support ranges.
* **Embedded subtitles of a remote stream are skipped**, not extracted: a
  second download for a side file would stall the film. Local files keep the
  full subtitle path; remote streams log `subtitles: N embedded track(s)
  ignored for a remote stream`.
* **`NextAVTransportURI` is stored but not chained** at end of stream; playback
  stops and the renderer reports `STOPPED`.
* **One player, one transport.** A second `SetAVTransportURI` stops the first.
* **`Seek` supports `REL_TIME` and `ABS_TIME` only**; `TRACK_NR` is refused
  with `710`. The target is clamped to `duration - 0.25 s`, so seeking to the
  very end stops playback rather than hanging on a frozen frame.
* **Volume is the player's software gain** (0–100 %, 0 = silence), independent
  from the PipeWire sink volume used by the multimedia keys; mute remembers
  the previous level.
* **Position resolution is 0.5 s** (monitor poll), which is finer than the
  `H:MM:SS` UPnP time format can express.
* **SSDP needs UDP 1900**; if another SSDP daemon already owns it without
  `SO_REUSEPORT`, the renderer logs the bind failure and HTTP still works.

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
   a content-sized `Wx202` (398x202 for the English startup lines; the log line
   `panel: shown (startup) …, box WxH (ink N)` gives both numbers), then
   `kms-shot osd /tmp/banner.png`. It must show four text lines. Ten seconds later
   the geom is back to `Wx76` and `kms-shot osd` prints `0 non-transparent
   pixels`. Pressing a key while the startup panel is up must log `panel: hidden
   (startup, key …)` and show the bottom band instead. Pressing `I` must log
   `panel: shown (manual)` and produce a `Wx242` capture with a fifth line (the
   file name) — 880 wide when the name had to be shortened, less otherwise; a
   second `I` must give `0 non-transparent pixels`, and arrows, `A`, `S`, `Q`
   must leave it up.
8. **Exit and plane release** (the black-screen regression): end one playback
   with `ESC`, one with `Q`, one with a short clip that reaches EOS (all three,
   plus a `SIGTERM`), then check the kernel's own plane state:

   ```sh
   sudo grep -E "^plane\[(33|57|75|69)\]" -A4 /sys/kernel/debug/dri/0/state
   ```

   Planes 57 (video), 69 (subtitles) and 75 (OSD) must read `crtc=(null)`,
   `fb=0`, `crtc-pos=0x0+0+0`; only plane 33 (fbcon) keeps
   `crtc=video_port0`. The log must show `pipeline: NULL in … ms`, then
   `planes: released, no framebuffer left on any plane`, then
   `exit, VT … | planes released: True`, and the VT the viewer lands on must
   carry one ASCII status line (`playback finished: …` or
   `playback stopped: …`) instead of an empty screen.
9. **Return to the desktop**: after the same three endings (and once with
   `SIGTERM`), check what the viewer is left with:

   ```sh
   cat /sys/class/tty/tty0/active      # tty2 here (the GNOME VT), not tty4
   pgrep -a gnome-shell                # still alive
   kms-planes --short | grep -E '^plane (57|69|75)'   # fb=0, pos=+0+0
   grep -E '^\[[0-9.]+\] (VT:|exit)' ~/.cache/kms-play/last.log
   ```

   `exit, VT 2 (desktop)` must be logged. Then two counter-tests:
   `sudo -n kms-vt switch 1` **during** playback, then quit -> the log must say
   `exit, VT untouched: user switched to tty1` and `tty0/active` must still be
   `tty1`; and `KMS_PLAY_RETURN_VT=none` -> `exit, VT 4 (console, return
   disabled)` with the console left on the player's VT.

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

A track change reconfigures the audio branch: `current-audio` on playbin,
`select-streams` on playbin3. On this board that reconfiguration can post **ERROR**
or even **EOS** from the audio side while the video keeps running perfectly. Ending
the film there is wrong, so `recover_audio_switch()` catches these messages when they
arrive within `AUDIO_SWITCH_GRACE` (3 s) of a switch: it restores the previous track
(another `select-streams` with the previous audio id on playbin3), re-anchors the
pipeline with a flush seek, shows "Piste audio indisponible" and keeps playing. Two
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
validates the resume seek. It has to fire before the watchdog disarms itself, i.e.
within the first 12 s of playback; 5 works, 20 does not:

```sh
KMS_PLAY_FORCE_SOFT_AT=5 kms-play clip.webm   # a hardware codec, e.g. VP8/HEVC
```

The log must show `test hook: forcing software tier at 0:05`, `fallback:
rebuilding playbin with software decoders at 0:05`, `fallback: resume at 0:05,
seek deferred to the first frame`, then `decode tier: software (confirmed, ...)`,
then `fallback: seek to 0:05 -> ok` about two seconds later, and the video plane
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
to `/dev/shm/kms-osd-<uid>.raw` (bottom band 2560x76, or information panel: content
width up to 880, 202 or 242 high) and
`/dev/shm/kms-sub-<uid>.raw` (band 2560x220) before handing the file name to
`kms-subd`. The matching `.geom` file holds `WxH`, which is how `kms-shot` knows
which rectangle a dump contains.

```sh
kms-shot osd /tmp/osd.png      # progress bar, volume bar, track label, info panel
kms-shot sub /tmp/sub.png      # current subtitle cue
kms-shot osd /tmp/x.png --size 640x242   # manual override if the geom file is gone
```

It prints `WxH, N non-transparent pixels, size from geom|--size|inferred`; `N = 0`
means the overlay was not showing, not that the render is broken. To inspect glyph
quality, crop to the non-transparent bounding box and scale up — a 3x crop shows the
anti-aliased outline clearly.

The **video plane is out of reach**: its framebuffer is an `NV15` DMABUF allocated by
the VPU, and the kernel does not expose it for reading. Picture quality stays a human
question.
