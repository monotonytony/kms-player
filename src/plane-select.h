// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

/*
 * plane-select.h - the one and only plane selection algorithm.
 *
 * ============================================================================
 * WHY A HEADER
 * ============================================================================
 * Two programs need the same answer:
 *
 *   kms-planes --select   exposes it on stdout, and that output is what
 *                         kms-play-py consumes (video plane for kmssink,
 *                         sub/osd planes for kms-subd);
 *   kms-subd              falls back to it when it is started by hand without
 *                         explicit plane ids, and offers it through
 *                         `kms-subd --select` so the two answers can be diffed.
 *
 * Everything is `static` and included by both .c files. That is deliberate:
 * there is exactly ONE implementation in the tree, so the two binaries cannot
 * drift apart. `diff <(kms-planes --select) <(kms-subd --select)` is the proof.
 *
 * ============================================================================
 * RULES (single screen only)
 * ============================================================================
 * Multi-head is out of scope: the goal is simply to never programme a plane
 * that belongs to another CRTC.
 *
 *   1. Connector: $KMS_PLAY_CONNECTOR, else the first connected connector
 *      with a routed CRTC, else "no connected display".
 *   2. CRTC: the one the connector is routed to. Only planes whose
 *      possible_crtcs mask contains that CRTC are ever candidates.
 *   3. Video: an overlay plane (primary only if no overlay exists) that
 *      accepts NV15 or NV12, with the lowest zpos.
 *   4. Sub / OSD: distinct ARGB8888 overlays placed above the video plane.
 *      Of the planes available, the two highest zpos are used: the highest
 *      becomes the OSD, the other becomes the subtitle plane. Keeping the
 *      pair at the top of the stack leaves lower overlays free and matches
 *      the historical 69/75 choice measured on the BPI-M7.
 *   5. zpos is read, never forced. If the property is immutable the current
 *      value IS the hardware order; callers log "zpos immutable, hw order"
 *      and verify by reading the plane back after configuration.
 *   6. Degradation ladder: one ARGB overlay -> video + OSD (subtitles off);
 *      none -> video only, with an explicit log line. No video plane at all
 *      is an error that names what is missing.
 *   7. $KMS_PLAY_PLANES="video=57,osd=75,sub=69" short-circuits selection.
 *      An id that does not exist, does not belong to the CRTC, or has the
 *      wrong format is a hard error, never a silent fallback.
 *
 * Run `kms-planes --check` for the topology this algorithm works on.
 */
#ifndef PLANE_SELECT_H
#define PLANE_SELECT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

/* kms-subd prepends timestamps to its logs; it defines PS_LOG before
 * including this header. kms-planes keeps the plain form. */
#ifndef PS_LOG
#define PS_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define PS_MAX_PLANES     64
#define PS_MAX_CRTCS      16
#define PS_MAX_CONNECTORS 16

struct ps_plane {
    uint32_t id;
    uint32_t possible_crtcs;   /* DRM bitmask: bit i => res->crtcs[i] */
    int type;                  /* DRM_PLANE_TYPE_*, or -1 if not exposed */
    int zpos_known;
    int zpos_immutable;        /* property flagged immutable: read-only order */
    uint64_t zpos;
    uint64_t zpos_min, zpos_max;
    int nv15, nv12, ar24, has_alpha;
    uint64_t alpha;
    uint32_t fb, crtc_id;
    int cx, cy;
    char formats[256];
};

struct ps_crtc {
    uint32_t id;
    int index;                 /* bit position in possible_crtcs masks */
    int active;                /* mode_valid */
    drmModeModeInfo mode;
};

struct ps_conn {
    char name[64];
    int connected;
    uint32_t encoder;
    uint32_t crtc;
    int mode_valid;            /* live mode read from the CRTC */
    drmModeModeInfo mode;
};

struct ps_topo {
    struct ps_crtc crtcs[PS_MAX_CRTCS];
    int ncrtcs;
    struct ps_conn conns[PS_MAX_CONNECTORS];
    int nconns;
    struct ps_plane planes[PS_MAX_PLANES];
    int nplanes;
};

/* Answer of the selection. 0 means "not available / disabled". */
struct ps_sel {
    uint32_t video, sub, osd;
    uint32_t crtc;
    char video_fourcc[5];
    uint32_t video_zpos, sub_zpos, osd_zpos;
    int zpos_immutable;        /* any selected plane has an immutable zpos */
};

static void ps_fourcc(uint32_t f, char *out)
{
    out[0] = f & 0xff;
    out[1] = (f >> 8) & 0xff;
    out[2] = (f >> 16) & 0xff;
    out[3] = (f >> 24) & 0xff;
    out[4] = 0;
    for (int i = 0; i < 4; i++)
        if (out[i] < 32 || out[i] > 126) out[i] = '.';
}

static const char *ps_type_name(int type)
{
    switch (type) {
    case DRM_PLANE_TYPE_OVERLAY: return "overlay";
    case DRM_PLANE_TYPE_PRIMARY: return "primary";
    case DRM_PLANE_TYPE_CURSOR:  return "cursor";
    default:                     return "unknown";
    }
}

/* Read one property of a plane. Returns 1 when found. */
static int ps_prop(int fd, uint32_t id, const char *name, uint64_t *val,
                   drmModePropertyPtr *out_pp)
{
    drmModeObjectProperties *op =
        drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE);
    if (!op) return 0;
    int found = 0;
    for (unsigned j = 0; j < op->count_props && !found; j++) {
        drmModePropertyPtr pp = drmModeGetProperty(fd, op->props[j]);
        if (pp && !strcmp(pp->name, name)) {
            *val = op->prop_values[j];
            if (out_pp) *out_pp = pp;
            else drmModeFreeProperty(pp);
            found = 1;
        } else if (pp) {
            drmModeFreeProperty(pp);
        }
    }
    drmModeFreeObjectProperties(op);
    return found;
}

static const char *ps_connector_type_name(uint32_t type)
{
    switch (type) {
    case DRM_MODE_CONNECTOR_Unknown:     return "Unknown";
    case DRM_MODE_CONNECTOR_VGA:         return "VGA";
    case DRM_MODE_CONNECTOR_DVII:        return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:        return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:        return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:   return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO:      return "SVIDEO";
    case DRM_MODE_CONNECTOR_LVDS:        return "LVDS";
    case DRM_MODE_CONNECTOR_Component:   return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN:     return "DIN";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:          return "TV";
    case DRM_MODE_CONNECTOR_eDP:         return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:     return "Virtual";
    case DRM_MODE_CONNECTOR_DSI:         return "DSI";
    default:                             return "Connector";
    }
}

/* Read the whole topology: CRTCs, connectors (with the live mode, which only
 * the CRTC knows), then every plane with the capabilities selection needs. */
static int ps_load(int fd, struct ps_topo *t)
{
    memset(t, 0, sizeof *t);
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { PS_LOG("plane-select: drmModeGetResources failed\n"); return -1; }

    for (int i = 0; i < res->count_crtcs && t->ncrtcs < PS_MAX_CRTCS; i++) {
        drmModeCrtc *c = drmModeGetCrtc(fd, res->crtcs[i]);
        if (!c) continue;
        struct ps_crtc *dst = &t->crtcs[t->ncrtcs];
        dst->id = c->crtc_id;
        dst->index = i;                 /* index in res->crtcs == mask bit */
        dst->active = c->mode_valid;
        dst->mode = c->mode;
        t->ncrtcs++;
        drmModeFreeCrtc(c);
    }

    for (int i = 0; i < res->count_connectors && t->nconns < PS_MAX_CONNECTORS; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        struct ps_conn *dst = &t->conns[t->nconns++];
        snprintf(dst->name, sizeof dst->name, "%s-%u",
                 ps_connector_type_name(c->connector_type), c->connector_type_id);
        dst->connected = (c->connection == DRM_MODE_CONNECTED);
        dst->encoder = c->encoder_id;
        /* The routed CRTC is what decides which mode is live; a connector
         * with an encoder but no CRTC is not scanning anything out yet. */
        if (c->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(fd, c->encoder_id);
            if (enc) {
                dst->crtc = enc->crtc_id;
                drmModeFreeEncoder(enc);
            }
        }
        if (dst->crtc) {
            drmModeCrtc *crtc = drmModeGetCrtc(fd, dst->crtc);
            if (crtc) {
                if (crtc->mode_valid) {
                    dst->mode = crtc->mode;
                    dst->mode_valid = 1;
                }
                drmModeFreeCrtc(crtc);
            }
        }
        if (!dst->mode_valid && c->count_modes) {
            dst->mode = c->modes[0];    /* best guess before the first modeset */
            dst->mode_valid = 0;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);

    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    if (!pr) { PS_LOG("plane-select: drmModeGetPlaneResources failed\n"); return -1; }

    for (unsigned i = 0; i < pr->count_planes && t->nplanes < PS_MAX_PLANES; i++) {
        uint32_t id = pr->planes[i];
        drmModePlane *p = drmModeGetPlane(fd, id);
        if (!p) continue;
        struct ps_plane *dst = &t->planes[t->nplanes++];
        dst->id = id;
        dst->possible_crtcs = p->possible_crtcs;
        dst->fb = p->fb_id;
        dst->crtc_id = p->crtc_id;
        dst->cx = p->crtc_x;
        dst->cy = p->crtc_y;
        dst->type = -1;

        size_t used = 0;
        for (unsigned k = 0; k < p->count_formats; k++) {
            char s[5];
            ps_fourcc(p->formats[k], s);
            int n = snprintf(dst->formats + used, sizeof dst->formats - used,
                             "%s%s", k ? " " : "", s);
            if (n > 0 && used + (size_t)n < sizeof dst->formats) used += (size_t)n;
            if (p->formats[k] == DRM_FORMAT_NV15) dst->nv15 = 1;
            if (p->formats[k] == DRM_FORMAT_NV12) dst->nv12 = 1;
            if (p->formats[k] == DRM_FORMAT_ARGB8888) dst->ar24 = 1;
        }
        drmModeFreePlane(p);

        /* "type" is immutable in practice, but it is still the property the
         * driver uses to declare PRIMARY / OVERLAY / CURSOR. A plane without
         * it is reported as unknown rather than assumed to be an overlay. */
        uint64_t v = 0;
        drmModePropertyPtr pp = NULL;
        if (ps_prop(fd, id, "type", &v, &pp)) {
            dst->type = (int)v;
            drmModeFreeProperty(pp);
        }
        if (ps_prop(fd, id, "zpos", &v, &pp)) {
            dst->zpos_known = 1;
            dst->zpos = v;
            dst->zpos_immutable = (pp->flags & DRM_MODE_PROP_IMMUTABLE) ? 1 : 0;
            if (pp->count_values >= 2) {
                dst->zpos_min = pp->values[0];
                dst->zpos_max = pp->values[pp->count_values - 1];
            } else {
                dst->zpos_min = dst->zpos_max = v;
            }
            drmModeFreeProperty(pp);
        }
        if (ps_prop(fd, id, "alpha", &v, NULL)) {
            dst->has_alpha = 1;
            dst->alpha = v;
        }
    }
    drmModeFreePlaneResources(pr);
    return 0;
}

static const struct ps_crtc *ps_find_crtc(const struct ps_topo *t, uint32_t id)
{
    for (int i = 0; i < t->ncrtcs; i++)
        if (t->crtcs[i].id == id) return &t->crtcs[i];
    return NULL;
}

static struct ps_plane *ps_find_plane(struct ps_topo *t, uint32_t id)
{
    for (int i = 0; i < t->nplanes; i++)
        if (t->planes[i].id == id) return &t->planes[i];
    return NULL;
}

/* Video plane ordering: overlay before primary, then lowest zpos, then NV15
 * before NV12 (10-bit is the zero-copy path this project exists for). */
static int ps_video_better(const struct ps_plane *a, const struct ps_plane *b)
{
    int ra = (a->type == DRM_PLANE_TYPE_OVERLAY) ? 0 : 1;
    int rb = (b->type == DRM_PLANE_TYPE_OVERLAY) ? 0 : 1;
    if (ra != rb) return ra < rb;
    if (a->zpos != b->zpos) return a->zpos < b->zpos;
    return a->nv15 && !b->nv15;
}

/* Connector resolution, shared by --select and by every caller. */
static const struct ps_conn *ps_pick_connector(const struct ps_topo *t,
                                               const char *want)
{
    if (want && *want) {
        for (int i = 0; i < t->nconns; i++)
            if (!strcmp(t->conns[i].name, want)) return &t->conns[i];
        PS_LOG("plane-select: connector %s not found\n", want);
        return NULL;
    }
    for (int i = 0; i < t->nconns; i++)
        if (t->conns[i].connected && t->conns[i].crtc) return &t->conns[i];
    return NULL;
}

/* Parse $KMS_PLAY_PLANES. -1 means "not overridden"; 0 means "none". */
struct ps_override {
    long video, sub, osd;
};
static void ps_parse_overrides(struct ps_override *o)
{
    o->video = o->sub = o->osd = -1;
    const char *env = getenv("KMS_PLAY_PLANES");
    if (!env || !*env) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", env);
    for (char *save = NULL, *tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            PS_LOG("plane-select: KMS_PLAY_PLANES: ignoring malformed '%s'\n", tok);
            continue;
        }
        *eq = 0;
        const char *key = tok, *val = eq + 1;
        long id;
        if (!strcmp(val, "none")) {
            id = 0;
        } else if (!strcmp(val, "auto")) {
            id = -1;
        } else {
            char *end = NULL;
            id = strtol(val, &end, 10);
            if (!end || *end || id <= 0) {
                PS_LOG("plane-select: KMS_PLAY_PLANES: '%s' is not an id or none\n",
                       val);
                id = -2;            /* invalid: reported by ps_select */
            }
        }
        if (!strcmp(key, "video")) o->video = id;
        else if (!strcmp(key, "sub")) o->sub = id;
        else if (!strcmp(key, "osd")) o->osd = id;
        else PS_LOG("plane-select: KMS_PLAY_PLANES: unknown key '%s'\n", key);
    }
}

/* Validate one overridden id against the CRTC and the role it plays.
 * Returns 0 on success, -1 on a hard error (already logged). */
static int ps_validate_override(struct ps_topo *t, uint32_t crtc, long id,
                                const char *role)
{
    if (id <= 0) {
        PS_LOG("plane-select: KMS_PLAY_PLANES: %s has an invalid id\n", role);
        return -1;
    }
    struct ps_plane *p = ps_find_plane(t, (uint32_t)id);
    if (!p) {
        PS_LOG("plane-select: KMS_PLAY_PLANES: plane %ld does not exist\n", id);
        return -1;
    }
    const struct ps_crtc *c = ps_find_crtc(t, crtc);
    if (!c) return -1;
    if (!(p->possible_crtcs & (1u << c->index))) {
        PS_LOG("plane-select: KMS_PLAY_PLANES: plane %ld does not belong to CRTC %u\n",
               id, crtc);
        return -1;
    }
    if (p->type == DRM_PLANE_TYPE_CURSOR) {
        PS_LOG("plane-select: KMS_PLAY_PLANES: plane %ld is a cursor plane\n", id);
        return -1;
    }
    if (!strcmp(role, "video")) {
        if (!p->nv15 && !p->nv12) {
            PS_LOG("plane-select: KMS_PLAY_PLANES: plane %ld accepts neither NV15 nor NV12\n",
                   id);
            return -1;
        }
    } else if (!p->ar24) {
        PS_LOG("plane-select: KMS_PLAY_PLANES: plane %ld does not accept ARGB8888\n",
               id);
        return -1;
    }
    return 0;
}

/* The selection itself. Logs to stderr (PS_LOG) when explain != 0.
 * Returns 0 and fills *sel, or -1 with a message on stderr. */
static int ps_select(int fd, const char *want_connector, struct ps_sel *sel,
                     int explain)
{
    memset(sel, 0, sizeof *sel);
    struct ps_topo t;
    if (ps_load(fd, &t) < 0) return -1;

    const struct ps_conn *conn = NULL;
    if (want_connector && *want_connector) {
        for (int i = 0; i < t.nconns; i++)
            if (!strcmp(t.conns[i].name, want_connector)) conn = &t.conns[i];
        if (!conn) {
            PS_LOG("plane-select: connector %s not found (available:",
                   want_connector);
            for (int i = 0; i < t.nconns; i++)
                PS_LOG(" %s%s", t.conns[i].name,
                       t.conns[i].connected ? "" : "(disconnected)");
            PS_LOG(")\n");
            return -1;
        }
        if (!conn->connected) {
            PS_LOG("plane-select: connector %s is disconnected\n", conn->name);
            return -1;
        }
        if (!conn->crtc) {
            PS_LOG("plane-select: connector %s has no CRTC routed\n", conn->name);
            return -1;
        }
    } else {
        conn = ps_pick_connector(&t, NULL);
        if (!conn) {
            PS_LOG("plane-select: no connected display\n");
            return -1;
        }
    }
    const struct ps_crtc *crtc = ps_find_crtc(&t, conn->crtc);
    if (!crtc) {
        PS_LOG("plane-select: CRTC %u of %s is not in the CRTC list\n",
               conn->crtc, conn->name);
        return -1;
    }
    sel->crtc = crtc->id;
    uint32_t bit = 1u << crtc->index;
    if (explain)
        PS_LOG("plane-select: connector %s crtc %u (bit 0x%x), mode %ux%u@%u\n",
               conn->name, crtc->id, bit, crtc->mode.hdisplay,
               crtc->mode.vdisplay, crtc->mode.vrefresh);

    struct ps_override ov;
    ps_parse_overrides(&ov);
    if (ov.video == -2 || ov.sub == -2 || ov.osd == -2) return -1;

    /* 1. video plane */
    struct ps_plane *video = NULL;
    if (ov.video >= 0) {
        if (ps_validate_override(&t, crtc->id, ov.video, "video") < 0) return -1;
        video = ps_find_plane(&t, (uint32_t)ov.video);
        if (explain) PS_LOG("plane-select: video plane %u from KMS_PLAY_PLANES\n",
                            video->id);
    } else {
        for (int i = 0; i < t.nplanes; i++) {
            struct ps_plane *p = &t.planes[i];
            if (!(p->possible_crtcs & bit)) continue;
            if (p->type == DRM_PLANE_TYPE_CURSOR) continue;
            if (!p->nv15 && !p->nv12) continue;
            if (!video || ps_video_better(p, video)) video = p;
        }
    }
    if (!video) {
        PS_LOG("plane-select: no NV15/NV12 plane on CRTC %u (%s)\n",
               crtc->id, conn->name);
        return -1;
    }
    sel->video = video->id;
    sel->video_zpos = (uint32_t)video->zpos;
    sel->zpos_immutable |= video->zpos_immutable;
    ps_fourcc(video->nv15 ? DRM_FORMAT_NV15 : DRM_FORMAT_NV12, sel->video_fourcc);
    if (explain)
        PS_LOG("plane-select: video %u type=%s zpos=%llu%s fourcc=%s\n",
               video->id, ps_type_name(video->type),
               (unsigned long long)video->zpos,
               video->zpos_immutable ? " (immutable)" : "", sel->video_fourcc);

    /* 2. ARGB overlays above the video plane. Top two by zpos: the highest is
     * the OSD, the second highest the subtitle plane. */
    struct ps_plane *argb[PS_MAX_PLANES];
    int nargb = 0;
    for (int i = 0; i < t.nplanes; i++) {
        struct ps_plane *p = &t.planes[i];
        if (p->id == video->id) continue;
        if (!(p->possible_crtcs & bit)) continue;
        if (p->type == DRM_PLANE_TYPE_CURSOR) continue;
        if (!p->ar24) continue;
        if (p->zpos <= video->zpos) continue;
        argb[nargb++] = p;
    }
    /* descending zpos, stable for equal values */
    for (int i = 1; i < nargb; i++) {
        struct ps_plane *key = argb[i];
        int j = i - 1;
        while (j >= 0 && argb[j]->zpos < key->zpos) { argb[j + 1] = argb[j]; j--; }
        argb[j + 1] = key;
    }
    if (nargb >= 2) {
        sel->osd = argb[0]->id;
        sel->osd_zpos = (uint32_t)argb[0]->zpos;
        sel->sub = argb[1]->id;
        sel->sub_zpos = (uint32_t)argb[1]->zpos;
        if (explain)
            PS_LOG("plane-select: sub %u zpos=%u, osd %u zpos=%u (%d ARGB overlays above video)\n",
                   sel->sub, sel->sub_zpos, sel->osd, sel->osd_zpos, nargb);
    } else if (nargb == 1) {
        sel->osd = argb[0]->id;
        sel->osd_zpos = (uint32_t)argb[0]->zpos;
        if (explain)
            PS_LOG("plane-select: degraded, one ARGB overlay: osd %u, subtitles disabled\n",
                   sel->osd);
    } else if (explain) {
        PS_LOG("plane-select: degraded, no ARGB overlay above video: OSD and subtitles disabled\n");
    }

    /* 3. Overrides for the text planes. Validate against the CRTC, then make
     * sure the override does not break the stacking order. */
    if (ov.sub >= 0) {
        if (ov.sub == 0) {
            sel->sub = 0;
            sel->sub_zpos = 0;
            if (explain) PS_LOG("plane-select: sub disabled by KMS_PLAY_PLANES\n");
        } else {
            if (ps_validate_override(&t, crtc->id, ov.sub, "sub") < 0) return -1;
            struct ps_plane *p = ps_find_plane(&t, (uint32_t)ov.sub);
            if (p->id == video->id) {
                PS_LOG("plane-select: KMS_PLAY_PLANES: sub plane %u is the video plane\n",
                       p->id);
                return -1;
            }
            if (p->zpos <= video->zpos) {
                PS_LOG("plane-select: KMS_PLAY_PLANES: sub plane %u is not above video %u\n",
                       p->id, video->id);
                return -1;
            }
            sel->sub = p->id;
            sel->sub_zpos = (uint32_t)p->zpos;
        }
    }
    if (ov.osd >= 0) {
        if (ov.osd == 0) {
            sel->osd = 0;
            sel->osd_zpos = 0;
            if (explain) PS_LOG("plane-select: osd disabled by KMS_PLAY_PLANES\n");
        } else {
            if (ps_validate_override(&t, crtc->id, ov.osd, "osd") < 0) return -1;
            struct ps_plane *p = ps_find_plane(&t, (uint32_t)ov.osd);
            if (p->id == video->id) {
                PS_LOG("plane-select: KMS_PLAY_PLANES: osd plane %u is the video plane\n",
                       p->id);
                return -1;
            }
            if (p->zpos <= video->zpos) {
                PS_LOG("plane-select: KMS_PLAY_PLANES: osd plane %u is not above video %u\n",
                       p->id, video->id);
                return -1;
            }
            sel->osd = p->id;
            sel->osd_zpos = (uint32_t)p->zpos;
        }
    }
    if (sel->sub && sel->osd && sel->sub == sel->osd) {
        PS_LOG("plane-select: sub and osd resolve to the same plane %u\n", sel->sub);
        return -1;
    }

    /* Read-only immutability report: callers must not try to force the order. */
    for (int i = 0; i < t.nplanes; i++) {
        struct ps_plane *p = &t.planes[i];
        if (p->id == sel->video || p->id == sel->sub || p->id == sel->osd)
            if (p->zpos_immutable) sel->zpos_immutable = 1;
    }
    if (explain && sel->zpos_immutable)
        PS_LOG("plane-select: zpos immutable, hw order\n");
    return 0;
}

/* Machine output shared by `kms-planes --select` and `kms-subd --select`.
 * Exactly three lines, nothing else on stdout. */
static void ps_print_selection(const struct ps_sel *s)
{
    printf("video %u %u %s %u\n", s->video, s->crtc, s->video_fourcc,
           s->video_zpos);
    if (s->sub) printf("%-5s %u %u ARGB8888 %u\n", "sub", s->sub, s->crtc, s->sub_zpos);
    else        printf("sub none\n");
    if (s->osd) printf("%-5s %u %u ARGB8888 %u\n", "osd", s->osd, s->crtc, s->osd_zpos);
    else        printf("osd none\n");
}

/* Video candidates for the retry path, best first, one id per line.
 * kms-subd does not need it and defines PS_NO_LIST_VIDEO to keep -Wextra
 * quiet; the function would otherwise be an unused static in that binary. */
#ifndef PS_NO_LIST_VIDEO
static void ps_list_video(int fd, const char *want_connector)
{
    struct ps_topo t;
    if (ps_load(fd, &t) < 0) return;
    const struct ps_conn *conn = ps_pick_connector(&t, want_connector);
    if (!conn || !conn->crtc) return;
    const struct ps_crtc *crtc = ps_find_crtc(&t, conn->crtc);
    if (!crtc) return;
    uint32_t bit = 1u << crtc->index;
    struct ps_plane *cand[PS_MAX_PLANES];
    int n = 0;
    for (int i = 0; i < t.nplanes; i++) {
        struct ps_plane *p = &t.planes[i];
        if (!(p->possible_crtcs & bit)) continue;
        if (p->type == DRM_PLANE_TYPE_CURSOR) continue;
        if (!p->nv15 && !p->nv12) continue;
        cand[n++] = p;
    }
    for (int i = 1; i < n; i++) {
        struct ps_plane *key = cand[i];
        int j = i - 1;
        while (j >= 0 && ps_video_better(key, cand[j])) { cand[j + 1] = cand[j]; j--; }
        cand[j + 1] = key;
    }
    for (int i = 0; i < n; i++) printf("%u\n", cand[i]->id);
}
#endif

#endif /* PLANE_SELECT_H */
