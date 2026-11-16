#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

"""test-i18n.py - render the information panel in all 30 languages offline.

For every entry in STRINGS this checks three things and prints one line:

    1. the panel composes - no missing key, no placeholder mismatch (the
       player validates the tables at import, this exercises them);
    2. every visible string draws with zero unknown glyphs at the panel font,
       i.e. no tofu can reach the screen;
    3. for non-Latin scripts, a PNG of the real panel composition, written to
       --out (default /tmp/i18n-panels), so the result can be looked at.

No DRM, no VT, no player: a cairo ImageSurface and PangoCairo are enough, and
the panel is drawn by the player's own renderer (OnScreenDisplay), not by a
copy of it. The only substitution is the OSD dump path, redirected to the
output directory so a test cannot overwrite /dev/shm.

Usage:
    python3 src/test-i18n.py [--out DIR] [--quiet]

Exit code 0 only if all 30 entries pass.
"""

import importlib.util
import os
from importlib.machinery import SourceFileLoader
import sys

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kms-play-py")

# Scripts that need a dedicated font family: their PNG is worth looking at.
NON_LATIN = ("ru", "uk", "el", "ar", "fa", "he", "hi", "bn", "ta", "th",
             "ja", "ko", "zh-Hans", "zh-Hant", "zh-HK")

# Values used to fill the placeholders. One dict for all keys: format()
# ignores the ones a template does not use.
SAMPLE = {
    "plane": "57", "fmt": "NV15", "width": 3840, "height": 1600, "hz": 75,
    "sign": "+", "value": "10",
}

PANEL_VALUES = {
    "hardware": True,
    "codec": "H.265", "profile": "Main 10",
    "definition": "3840x1600", "rate": "23.976 fps",
    "plane": 57, "fmt": "NV15", "caps_fmt": "DMA_DRM",
    "dmabuf": True, "zpos": 4,
    "connector": "HDMI-A-1", "width": 2560, "height": 1440, "vrefresh": 75,
}

FILENAME = "Un.Titre.De.Film.2026.2160p.x265-TEAM.mkv"


class FakeSubdaemon:
    """Collects the osd commands the panel would send to kms-subd."""

    def __init__(self):
        self.commands = []

    class _Stdin:
        def __init__(self, sink):
            self.sink = sink

        def write(self, data):
            self.sink.commands.append(data.strip())

        def flush(self):
            pass

    @property
    def stdin(self):
        return self._Stdin(self)


def load_player():
    """Import kms-play-py as a module.

    It is a script with no .py extension, so the loader has to be named
    explicitly: spec_from_file_location() returns no loader for it."""
    loader = SourceFileLoader("kms_play_py", SRC)
    spec = importlib.util.spec_from_loader("kms_play_py", loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules["kms_play_py"] = module
    loader.exec_module(module)
    return module


def main():
    out_dir = "/tmp/i18n-panels"
    quiet = "--quiet" in sys.argv
    if "--out" in sys.argv:
        out_dir = sys.argv[sys.argv.index("--out") + 1]
    os.makedirs(out_dir, exist_ok=True)

    mod = load_player()
    import cairo
    from gi.repository import Pango, PangoCairo

    # The player's log goes to a file and to stderr; capture it here so the
    # report stays readable, and keep the i18n lines for the report.
    log_lines = []
    mod.log = lambda *args: log_lines.append(" ".join(str(a) for a in args))

    # Panel dumps land in the output directory, never in /dev/shm.
    mod.RAW_OSD = os.path.join(out_dir, "panel.raw")
    mod.RAW_OSD_GEOM = os.path.join(out_dir, "panel.geom")

    # One measuring layout for the glyph counts.
    measure = PangoCairo.create_layout(
        cairo.Context(cairo.ImageSurface(cairo.FORMAT_ARGB32, 1, 1)))
    measure.set_width(-1)

    def unknown_glyphs(text, lang):
        measure.set_font_description(mod.font_description("15", lang))
        measure.set_text(text, -1)
        return measure.get_unknown_glyphs_count()

    failures = 0
    report = []
    for code in mod.STRINGS:
        mod.set_language(code, quiet=True)
        # Track names are themselves translated strings, so the "Audio:
        # {name}" line is measured with this language's word for French.
        sample = dict(SAMPLE, name=mod.tr("lang.fr"))

        bad = []
        for key in mod.KEYS:
            text = mod.tr(key, **sample)
            count = unknown_glyphs(text, code)
            if count:
                bad.append("%s (%d)" % (key, count))

        lines = mod.format_panel_lines(PANEL_VALUES, filename=FILENAME)
        for index, line in enumerate(lines):
            # An RTL language splits the plane line in two runs: the label and
            # the technical values are drawn by two different layouts.
            for segment in (line if isinstance(line, tuple) else (line,)):
                count = unknown_glyphs(segment, code)
                if count:
                    bad.append("panel line %d (%d)" % (index + 1, count))

        # Draw it with the player's renderer and, for non-Latin scripts,
        # write the PNG.
        osd = mod.OnScreenDisplay(FakeSubdaemon(), 2560, 1440)
        osd.show_panel(lines, kind="startup", duration=1.0)
        png = "-"
        if code in NON_LATIN:
            png = os.path.join(out_dir, "panel-%s.png" % code)
            osd.panel_surface.write_to_png(png)

        status = "ok" if not bad else "FAIL"
        if bad:
            failures += 1
        report.append("%-8s %-4s strings %d  panel %d lines  %s  %s"
                      % (code, status, len(mod.KEYS), len(lines),
                         os.path.basename(png) if png != "-" else "-",
                         ", ".join(bad)))
        if not quiet:
            print(report[-1])

    # Every string the panel really contains must also be drawable in the
    # language the font was chosen for: this is the anti-tofu guarantee.
    fallbacks = [line for line in log_lines if line.startswith("i18n:")]
    if fallbacks:
        print("\nfallbacks reported by the player:")
        for line in fallbacks:
            print("  " + line)

    # Language selection, end to end on the detection function.
    print("\ndetection:")
    variables = ("KMS_PLAY_LANG", "LC_MESSAGES", "LANG", "LANGUAGE")
    saved = {name: os.environ.get(name) for name in variables}
    cases = [
        ("KMS_PLAY_LANG=ko", {"KMS_PLAY_LANG": "ko"}, "ko"),
        ("LANG=ja_JP.UTF-8", {"LANG": "ja_JP.UTF-8"}, "ja"),
        ("LANG=xx_YY", {"LANG": "xx_YY"}, "en"),
        ("LANG=fr_FR.UTF-8", {"LANG": "fr_FR.UTF-8"}, "fr"),
        ("LC_MESSAGES=zh_TW.UTF-8", {"LC_MESSAGES": "zh_TW.UTF-8"}, "zh-Hant"),
        ("LANG=zh_HK.UTF-8", {"LANG": "zh_HK.UTF-8"}, "zh-HK"),
        ("LANG=zh_CN.UTF-8", {"LANG": "zh_CN.UTF-8"}, "zh-Hans"),
        ("LANGUAGE=pt_BR:fr_FR", {"LANGUAGE": "pt_BR:fr_FR"}, "pt"),
        ("LANG=C", {"LANG": "C"}, "en"),
        # Priority: the explicit variable wins over any locale.
        ("KMS_PLAY_LANG=th + LANG=de_DE.UTF-8",
         {"KMS_PLAY_LANG": "th", "LANG": "de_DE.UTF-8"}, "th"),
    ]
    for label, env, expected in cases:
        for name in variables:
            os.environ.pop(name, None)
        os.environ.update(env)
        found = mod.detect_language()
        ok = found == expected
        if not ok:
            failures += 1
        print("  %-34s -> %-7s %s" % (label, found, "ok" if ok else "FAIL(%s)" % expected))
    for name, value in saved.items():
        if value is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = value

    print("\n%d languages, %d failure(s); PNGs in %s" % (len(mod.STRINGS), failures, out_dir))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
