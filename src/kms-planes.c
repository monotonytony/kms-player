// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

/*
 * kms-planes - dump the state of every DRM plane on the KMS device.
 *
 * ============================================================================
 * PURPOSE
 * ============================================================================
 * The player runs full screen and the agent driving it cannot see the screen.
 * This tool turns "did the OSD actually appear?" into a measurable fact: it
 * reads the live plane state straight from the kernel (not from our own
 * bookkeeping) and prints, for each plane:
 *
 *   - its zpos (stacking order) and plane alpha;
 *   - the formats the plane can scan out (NV15, NV12, AR24 ...);
 *   - the framebuffer currently attached (id, fourcc, DRM modifier) and the
 *     position it is placed at.
 *
 * A plane with fb=0 is switched off. A plane with fmt=AR24 and a non-zero
 * position is currently compositing text over the video.
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *   kms-planes            human readable, one block per plane + CRTC modes
 *   kms-planes --short    one machine-parsable line per plane; this is the
 *                         form kms-play-py parses to pick the NV15 video plane
 *
 * Reading these properties does not require DRM master, so this tool can run
 * alongside the player without disturbing it.
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

/* Convert a DRM fourcc integer into its 4-character code (e.g. 0x34325241 ->
 * "AR24"). Non printable bytes become '.', which is what makes unknown
 * formats readable instead of dumping control characters. */
static void fourcc(unsigned int f, char *out)
{
    out[0] = f & 0xff;
    out[1] = (f >> 8) & 0xff;
    out[2] = (f >> 16) & 0xff;
    out[3] = (f >> 24) & 0xff;
    out[4] = 0;
    for (int i = 0; i < 4; i++) if (out[i] < 32 || out[i] > 126) out[i] = '.';
}

/* Read one plane property by name into *val.
 * Returns 0 on success, -1 when the property does not exist on this plane
 * (drivers expose different subsets). */
static int prop_get(int fd, uint32_t id, const char *name, uint64_t *val)
{
    drmModeObjectProperties *op = drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE);
    if (!op) return -1;
    int found = -1;
    for (unsigned j = 0; j < op->count_props; j++) {
        drmModePropertyPtr pp = drmModeGetProperty(fd, op->props[j]);
        if (pp) {
            if (!strcmp(pp->name, name)) { *val = op->prop_values[j]; found = 0; }
            drmModeFreeProperty(pp);
        }
        if (found == 0) break;
    }
    drmModeFreeObjectProperties(op);
    return found;
}

/* Ask the kernel what the attached framebuffer really is.
 * GETFB2 is the only ioctl that also returns the format modifier, which
 * matters here: the video plane is scanned out from a DMA-BUF with an
 * explicit (usually linear or AFBC) modifier, and the modifier is part of the
 * proof that no CPU copy happened. */
static int fb_info(int fd, uint32_t fbid, unsigned *fmt, unsigned long long *mod)
{
    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof f);
    f.fb_id = fbid;
    if (drmIoctl(fd, DRM_IOCTL_MODE_GETFB2, &f) != 0) return -1;
    *fmt = f.pixel_format;
    *mod = f.modifier[0];
    return 0;
}

int main(int argc, char **argv)
{
    int shortmode = (argc > 1 && !strcmp(argv[1], "--short"));
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/dri/card0"); return 1; }

    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    if (!pr) { perror("plane resources"); return 1; }

    drmModeRes *res = drmModeGetResources(fd);

    for (unsigned i = 0; i < pr->count_planes; i++) {
        uint32_t id = pr->planes[i];
        drmModePlane *p = drmModeGetPlane(fd, id);
        if (!p) continue;

        uint64_t zpos = 0, alpha = 0;
        int has_z = prop_get(fd, id, "zpos", &zpos) == 0;
        int has_a = prop_get(fd, id, "alpha", &alpha) == 0;

        /* Build the format list and remember the three formats this project
         * cares about: NV15 (10-bit 4:2:0 video), NV12 (8-bit) and AR24 (text
         * overlays). kms-play-py greps nv15=1 to find its video plane. */
        char fmt[256] = "";
        int nv15 = 0, ar24 = 0, nv12 = 0;
        for (unsigned k = 0; k < p->count_formats; k++) {
            char s[5];
            fourcc(p->formats[k], s);
            snprintf(fmt + strlen(fmt), sizeof fmt - strlen(fmt), "%s ", s);
            if (p->formats[k] == DRM_FORMAT_NV15) nv15 = 1;
            if (p->formats[k] == DRM_FORMAT_ARGB8888) ar24 = 1;
            if (p->formats[k] == DRM_FORMAT_NV12) nv12 = 1;
        }

        uint32_t fbid = p->fb_id;
        char fbfmt[8] = "-";
        unsigned long long modifier = 0;
        if (fbid) {
            unsigned f4 = 0;
            if (fb_info(fd, fbid, &f4, &modifier) == 0) fourcc(f4, fbfmt);
            else snprintf(fbfmt, sizeof fbfmt, "?");
        }

        if (shortmode) {
            printf("plane %u zpos=%llu fb=%u fmt=%s mod=0x%llx pos=+%u+%u nv15=%d nv12=%d ar24=%d alpha=%llu\n",
                   id, (unsigned long long)zpos, fbid, fbfmt, modifier,
                   p->crtc_x, p->crtc_y, nv15, nv12, ar24, (unsigned long long)alpha);
        } else {
            printf("plane %u  zpos=%llu%s  alpha=%llu%s  fb=%u (%s, mod=0x%llx)  pos=+%u+%u\n",
                   id, (unsigned long long)zpos, has_z ? "" : "?",
                   (unsigned long long)alpha, has_a ? "" : "?",
                   fbid, fbfmt, modifier, p->crtc_x, p->crtc_y);
            printf("   formats: %s\n", fmt);
        }
        drmModeFreePlane(p);
    }

    /* CRTC modes are printed in long mode only: they show the actual scanout
     * resolution, which the player also derives from the connector's preferred
     * mode. */
    if (res) {
        for (int i = 0; i < res->count_crtcs; i++) {
            drmModeCrtc *c = drmModeGetCrtc(fd, res->crtcs[i]);
            if (!c) continue;
            printf("crtc %u: mode=%s %ux%u active=%d\n", c->crtc_id,
                   c->mode_valid ? c->mode.name : "-",
                   c->mode.hdisplay, c->mode.vdisplay, c->mode_valid);
            drmModeFreeCrtc(c);
        }
        drmModeFreeResources(res);
    }
    drmModeFreePlaneResources(pr);
    close(fd);
    return 0;
}
