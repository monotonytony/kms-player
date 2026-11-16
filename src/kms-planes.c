// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

/*
 * kms-planes - inspect, and select from, the DRM planes of the KMS device.
 *
 * ============================================================================
 * PURPOSE
 * ============================================================================
 * The player runs full screen and the agent driving it cannot see the screen.
 * This tool turns "did the OSD actually appear?" into a measurable fact: it
 * reads the live plane state straight from the kernel (not from our own
 * bookkeeping).
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *   kms-planes                 human readable, one block per plane, then the
 *                              connectors and CRTCs
 *   kms-planes --short         one machine-parsable line per plane, then the
 *                              connector/CRTC lines. Consumed by kms-play-py
 *                              for read-back (fmt, zpos, fb, position, size).
 *   kms-planes --check         full topology dump: per connector (name, status,
 *                              encoder, routed CRTC, live mode, vrefresh), per
 *                              CRTC (id, mode, active), per plane (id, DRM type,
 *                              zpos min/max and immutable flag, formats,
 *                              possible_crtcs decoded to CRTC ids). Read-only.
 *   kms-planes --select        the plane selection, exactly three lines:
 *                                   video 57 81 NV15 4
 *                                   sub   69 81 ARGB8888 6
 *                                   osd   75 81 ARGB8888 7
 *                              (or "sub none" / "osd none" when degraded).
 *                              Honours $KMS_PLAY_CONNECTOR and
 *                              $KMS_PLAY_PLANES; diagnostics go to stderr.
 *   kms-planes --list-video    video plane candidates, best first, one id per
 *                              line. Used by the player's single retry.
 *
 * Both read-only forms work without DRM master, so they can run alongside the
 * player without disturbing it.
 *
 * The selection algorithm lives in plane-select.h, which kms-subd includes
 * too: one implementation, two binaries.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "plane-select.h"

/* Ask the kernel what the attached framebuffer really is.
 * GETFB2 is the only ioctl that also returns the format modifier, which
 * matters here: the video plane is scanned out from a DMA-BUF with an
 * explicit modifier, and the modifier is part of the proof that no CPU copy
 * happened. */
static int fb_info(int fd, uint32_t fbid, unsigned *fmt, unsigned long long *mod,
                   uint32_t *w, uint32_t *h)
{
    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof f);
    f.fb_id = fbid;
    if (drmIoctl(fd, DRM_IOCTL_MODE_GETFB2, &f) != 0) return -1;
    *fmt = f.pixel_format;
    *mod = f.modifier[0];
    *w = f.width;
    *h = f.height;
    return 0;
}

/* possible_crtcs is a bitmask indexed by the position in drmModeRes.crtcs.
 * Printing "81" is actionable; printing 0x1 alone is not. */
static void print_possible_crtcs(const struct ps_topo *t, uint32_t mask)
{
    int first = 1;
    for (int i = 0; i < t->ncrtcs; i++) {
        if (mask & (1u << i)) {
            printf("%s%u", first ? "" : ",", t->crtcs[i].id);
            first = 0;
        }
    }
    if (first) printf("none");
}

static int mode_dump(int fd)
{
    struct ps_topo t;
    if (ps_load(fd, &t) < 0) return 1;

    for (int i = 0; i < t.nplanes; i++) {
        struct ps_plane *p = &t.planes[i];
        char fbfmt[8] = "-";
        unsigned long long modifier = 0;
        uint32_t fw = 0, fh = 0;
        if (p->fb) {
            unsigned f4 = 0;
            if (fb_info(fd, p->fb, &f4, &modifier, &fw, &fh) == 0) ps_fourcc(f4, fbfmt);
            else snprintf(fbfmt, sizeof fbfmt, "?");
        }
        printf("plane %u  zpos=%llu%s  alpha=%llu%s  fb=%u (%s, mod=0x%llx)  pos=+%d+%d frame=%ux%u\n",
               p->id, (unsigned long long)p->zpos, p->zpos_known ? "" : "?",
               (unsigned long long)p->alpha, p->has_alpha ? "" : "?",
               p->fb, fbfmt, modifier, p->cx, p->cy, fw, fh);
        printf("   formats: %s\n", p->formats);
        printf("   type=%s possible_crtcs=", ps_type_name(p->type));
        print_possible_crtcs(&t, p->possible_crtcs);
        printf("\n");
    }

    for (int i = 0; i < t.nconns; i++) {
        struct ps_conn *c = &t.conns[i];
        if (!c->connected) {
            printf("connector %s disconnected\n", c->name);
            continue;
        }
        if (c->mode_valid) {
            printf("connector %s connected crtc=%u mode=%ux%u vrefresh=%u\n",
                   c->name, c->crtc, c->mode.hdisplay, c->mode.vdisplay,
                   c->mode.vrefresh);
        } else if (c->mode.hdisplay) {
            printf("connector %s connected crtc=%u mode=%ux%u vrefresh=%u preferred\n",
                   c->name, c->crtc, c->mode.hdisplay, c->mode.vdisplay,
                   c->mode.vrefresh);
        } else {
            printf("connector %s connected crtc=%u mode=- vrefresh=0\n",
                   c->name, c->crtc);
        }
    }
    for (int i = 0; i < t.ncrtcs; i++) {
        struct ps_crtc *c = &t.crtcs[i];
        printf("crtc %u: mode=%s %ux%u vrefresh=%u active=%d\n", c->id,
               c->active ? c->mode.name : "-",
               c->mode.hdisplay, c->mode.vdisplay, c->mode.vrefresh, c->active);
    }
    return 0;
}

static int short_dump(int fd)
{
    struct ps_topo t;
    if (ps_load(fd, &t) < 0) return 1;

    for (int i = 0; i < t.nplanes; i++) {
        struct ps_plane *p = &t.planes[i];
        char fbfmt[8] = "-";
        unsigned long long modifier = 0;
        uint32_t fw = 0, fh = 0;
        if (p->fb) {
            unsigned f4 = 0;
            if (fb_info(fd, p->fb, &f4, &modifier, &fw, &fh) == 0) ps_fourcc(f4, fbfmt);
            else snprintf(fbfmt, sizeof fbfmt, "?");
        }
        printf("plane %u zpos=%llu fb=%u fmt=%s mod=0x%llx pos=+%d+%d frame=%ux%u nv15=%d nv12=%d ar24=%d alpha=%llu\n",
               p->id, (unsigned long long)p->zpos, p->fb, fbfmt, modifier,
               p->cx, p->cy, fw, fh, p->nv15, p->nv12, p->ar24,
               (unsigned long long)p->alpha);
    }
    for (int i = 0; i < t.nconns; i++) {
        struct ps_conn *c = &t.conns[i];
        if (!c->connected) {
            printf("connector %s disconnected\n", c->name);
            continue;
        }
        if (c->mode_valid) {
            printf("connector %s connected crtc=%u mode=%ux%u vrefresh=%u\n",
                   c->name, c->crtc, c->mode.hdisplay, c->mode.vdisplay,
                   c->mode.vrefresh);
        } else if (c->mode.hdisplay) {
            printf("connector %s connected crtc=%u mode=%ux%u vrefresh=%u preferred\n",
                   c->name, c->crtc, c->mode.hdisplay, c->mode.vdisplay,
                   c->mode.vrefresh);
        } else {
            printf("connector %s connected crtc=%u mode=- vrefresh=0\n",
                   c->name, c->crtc);
        }
    }
    for (int i = 0; i < t.ncrtcs; i++) {
        struct ps_crtc *c = &t.crtcs[i];
        printf("crtc %u: mode=%s %ux%u vrefresh=%u active=%d\n", c->id,
               c->active ? c->mode.name : "-",
               c->mode.hdisplay, c->mode.vdisplay, c->mode.vrefresh, c->active);
    }
    return 0;
}

/* Full topology, human readable and parsable by the same regexes used for
 * --short where the fields overlap. This is the dump a human reads before
 * trusting a selection on a board nobody has tested. */
static int check_dump(int fd)
{
    struct ps_topo t;
    if (ps_load(fd, &t) < 0) return 1;

    for (int i = 0; i < t.nconns; i++) {
        struct ps_conn *c = &t.conns[i];
        if (!c->connected) {
            printf("connector %s status=disconnected encoder=%u crtc=%u\n",
                   c->name, c->encoder, c->crtc);
            continue;
        }
        if (c->mode_valid)
            printf("connector %s status=connected encoder=%u crtc=%u mode=%ux%u vrefresh=%u\n",
                   c->name, c->encoder, c->crtc, c->mode.hdisplay,
                   c->mode.vdisplay, c->mode.vrefresh);
        else if (c->mode.hdisplay)
            printf("connector %s status=connected encoder=%u crtc=%u mode=%ux%u vrefresh=%u preferred\n",
                   c->name, c->encoder, c->crtc, c->mode.hdisplay,
                   c->mode.vdisplay, c->mode.vrefresh);
        else
            printf("connector %s status=connected encoder=%u crtc=%u mode=- vrefresh=0\n",
                   c->name, c->encoder, c->crtc);
    }
    for (int i = 0; i < t.ncrtcs; i++) {
        struct ps_crtc *c = &t.crtcs[i];
        printf("crtc %u index=%d active=%d mode=%s %ux%u vrefresh=%u\n",
               c->id, c->index, c->active, c->active ? c->mode.name : "-",
               c->mode.hdisplay, c->mode.vdisplay, c->mode.vrefresh);
    }
    for (int i = 0; i < t.nplanes; i++) {
        struct ps_plane *p = &t.planes[i];
        char fbfmt[8] = "-";
        unsigned long long modifier = 0;
        uint32_t fw = 0, fh = 0;
        if (p->fb) {
            unsigned f4 = 0;
            if (fb_info(fd, p->fb, &f4, &modifier, &fw, &fh) == 0) ps_fourcc(f4, fbfmt);
            else snprintf(fbfmt, sizeof fbfmt, "?");
        }
        printf("plane %u type=%s ", p->id, ps_type_name(p->type));
        if (p->zpos_known)
            printf("zpos=%llu range=%llu..%llu immutable=%d ",
                   (unsigned long long)p->zpos,
                   (unsigned long long)p->zpos_min,
                   (unsigned long long)p->zpos_max, p->zpos_immutable);
        else
            printf("zpos=- range=- immutable=- ");
        printf("possible_crtcs=");
        print_possible_crtcs(&t, p->possible_crtcs);
        printf(" mask=0x%x\n", p->possible_crtcs);
        printf("   fb=%u (%s, mod=0x%llx) pos=+%d+%d frame=%ux%u alpha=%llu\n",
               p->fb, fbfmt, modifier, p->cx, p->cy, fw, fh,
               (unsigned long long)p->alpha);
        printf("   formats: %s\n", p->formats);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int shortmode = 0, checkmode = 0, selectmode = 0, listvideo = 0, explain = 0;
    const char *connector = getenv("KMS_PLAY_CONNECTOR");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--short")) shortmode = 1;
        else if (!strcmp(argv[i], "--check")) checkmode = 1;
        else if (!strcmp(argv[i], "--select")) selectmode = 1;
        else if (!strcmp(argv[i], "--list-video")) listvideo = 1;
        else if (!strcmp(argv[i], "--verbose")) explain = 1;
        else if (!strcmp(argv[i], "--connector") && i + 1 < argc) connector = argv[++i];
        else {
            fprintf(stderr, "usage: kms-planes [--short|--check|--select|--list-video]"
                            " [--connector NAME] [--verbose]\n");
            return 2;
        }
    }

    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/dri/card0"); return 1; }

    int rc = 0;
    if (selectmode) {
        struct ps_sel sel;
        if (ps_select(fd, connector, &sel, explain) < 0) rc = 1;
        else ps_print_selection(&sel);
    } else if (listvideo) {
        ps_list_video(fd, connector);
    } else if (checkmode) {
        rc = check_dump(fd);
    } else if (shortmode) {
        rc = short_dump(fd);
    } else {
        rc = mode_dump(fd);
    }
    close(fd);
    return rc;
}
