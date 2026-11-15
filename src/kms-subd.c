// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

/*
 * kms-subd - KMS overlay service for the text layers of the player.
 *
 * ============================================================================
 * ROLE AND ARCHITECTURE
 * ============================================================================
 * The player pipeline draws video into a DRM plane (NV15/NV12, zero copy, see
 * kms-play-py). Text (subtitles, OSD) cannot be blended into that plane
 * without a copy, so the video controller (VOP) composites two additional
 * ARGB planes on top of it. This daemon owns those two planes:
 *
 *     plane "sub"   subtitles   (ARGB, zpos just above the video plane)
 *     plane "osd"   OSD         (ARGB, highest zpos)
 *
 * Why a separate process and not the player? Only one process can hold the
 * DRM master, and the master must be acquired AFTER the VT switch (see
 * kms-play-py). Keeping master ownership in a tiny long-lived child means the
 * GStreamer pipeline can be torn down and rebuilt (seeks, track changes)
 * without losing the master or the plane state.
 *
 * The video pipeline still needs to open the same DRM node. libkmsshare.so
 * (LD_PRELOAD, not part of this project) intercepts open("/dev/dri/card*") in
 * the player and returns a copy of THIS process' fd, obtained over the unix
 * socket below. The fd is sent with SCM_RIGHTS: both processes then share one
 * file description, one DRM file handle and one master. That is why the
 * player must not call drmSetMaster itself and why libkmsshare neutralises
 * drmSetMaster/drmDropMaster.
 *
 * ============================================================================
 * WIRE PROTOCOL (stdin, one command per line)
 * ============================================================================
 *   sub cue <x> <y> <w> <h> <path>   show a raw BGRA, pre-multiplied buffer
 *   sub clear                        hide the subtitle plane
 *   osd cue <x> <y> <w> <h> <path>   show a raw BGRA, pre-multiplied buffer
 *   osd clear                        hide the OSD plane
 *   cue ... / clear                  shortcut for "sub ..." (compatibility)
 *   show                             re-attach the last buffer of the plane
 *   quit                             clear both planes and exit
 *
 * <path> is a file holding w*h*4 bytes, rows packed without padding. In
 * practice the player writes /dev/shm/kms-{sub,osd}-<uid>.raw with cairo.
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *   kms-subd <socket-path> <video-plane-id> [sub-plane-id] [osd-plane-id]
 *
 * Without explicit plane ids the two highest-zpos ARGB planes are chosen:
 * highest = OSD, second highest = subtitles. The video plane id is needed
 * only to find the CRTC (via drmModeGetPlane) and to exclude it from the
 * candidates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <xf86drmMode.h>

/* Two buffers per plane: while the VOP scans out buffer A, the next frame is
 * written into buffer B and then flipped with a single drmModeSetPlane. This
 * is what avoids tearing and, more importantly, avoids modifying a buffer the
 * display controller is currently reading. */
#define MAXBUF 2

/* Number of managed planes: 0 = subtitles, 1 = OSD. */
#define NPLANS 2
#define P_SUB 0
#define P_OSD 1

/* A dumb buffer plus its framebuffer object.
 * "dumb" is the kernel's simple linear allocation API: enough for text
 * overlays, which have no tiling or compression requirements. The buffer is
 * mmap'ed into this process and filled row by row. */
struct dumbuf {
    uint32_t fb_id;      /* DRM framebuffer id handed to drmModeSetPlane */
    uint32_t handle;     /* dumb buffer handle (for destroy/map) */
    uint32_t pitch;      /* bytes per row, may exceed w*4: the kernel aligns rows */
    uint64_t size;       /* total allocation size in bytes */
    void *map;           /* CPU mapping of the buffer */
    int w, h;            /* current geometry, to know when a resize is needed */
    int used;            /* 0 = never allocated */
};

/* One overlay plane and its double buffer. */
struct plane {
    const char *name;    /* "sub" / "osd", used in logs and in the protocol */
    int id;              /* DRM plane id, 0 = not configured */
    unsigned zpos;       /* stacking order, higher is on top */
    uint32_t crtc;       /* CRTC the plane is (or will be) attached to */
    struct dumbuf buf[MAXBUF];
    int cur;             /* index of the buffer currently displayed */
    int active;          /* plane currently visible (for logging only) */
};

static int g_fd = -1;          /* the one and only DRM fd; also shared with the player */
static int g_vplane;           /* video plane id, used to locate the CRTC */
static struct plane g_pl[NPLANS];
static char *g_stage;          /* scratch buffer used to read the raw frame file */

/* Monotonic seconds. Log timestamps are only useful for latency measurements
 * if they share one time base with the other tools, hence CLOCK_MONOTONIC. */
static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Every diagnostic goes to stderr, prefixed with a monotonic timestamp; the
 * player redirects it into ~/.cache/kms-play/last.log. stdout is not used at
 * all so that it can never be confused with the protocol. */
#define TLOG(...) do { fprintf(stderr, "[%.6f] ", mono()); fprintf(stderr, __VA_ARGS__); } while (0)

/* Find the CRTC the video plane is attached to.
 * Preferred source: the plane's own crtc_id, which is already set once the
 * pipeline is running. Fallback: the first CRTC with a valid mode, used when
 * the player starts before the video plane has been configured. */
static uint32_t find_crtc(void)
{
    drmModePlane *p = drmModeGetPlane(g_fd, g_vplane);
    if (p) {
        uint32_t c = p->crtc_id;
        drmModeFreePlane(p);
        if (c) return c;
    }
    drmModeRes *r = drmModeGetResources(g_fd);
    uint32_t c = 0;
    if (r) {
        for (int i = 0; i < r->count_crtcs && !c; i++) {
            drmModeCrtc *cc = drmModeGetCrtc(g_fd, r->crtcs[i]);
            if (cc) {
                if (cc->mode_valid) c = cc->crtc_id;
                drmModeFreeCrtc(cc);
            }
        }
        drmModeFreeResources(r);
    }
    return c;
}

/* Allocate (or reallocate) a dumb buffer and its framebuffer for w x h.
 * Returns 0 when the buffer already has the right size, so a subtitle line
 * change never reallocates. bpp=32 with depth=32 for the AddFB below makes
 * the kernel expose the buffer as ARGB8888 (XRGB would be depth 24).
 * The pitch returned by the kernel can be larger than w*4, therefore callers
 * must copy row by row instead of one big memcpy. */
static int buf_alloc(struct dumbuf *b, int w, int h)
{
    if (b->used && b->w == w && b->h == h) return 0;
    if (b->used) {
        if (b->map) munmap(b->map, b->size);
        if (b->fb_id) drmModeRmFB(g_fd, b->fb_id);
        if (b->handle) {
            struct drm_mode_destroy_dumb d = { .handle = b->handle };
            drmIoctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
        memset(b, 0, sizeof *b);
    }
    struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) { perror("CREATE_DUMB"); return -1; }
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = c.size;
    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) { perror("MAP_DUMB"); return -1; }
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, m.offset);
    if (b->map == MAP_FAILED) { perror("mmap"); b->map = NULL; return -1; }
    /* AddFB without a modifier: the kernel picks a linear layout, which every
     * VOP overlay plane supports for ARGB8888. */
    if (drmModeAddFB(g_fd, w, h, 32, 32, b->pitch, b->handle, &b->fb_id) != 0) {
        perror("AddFB"); return -1;
    }
    b->w = w; b->h = h; b->used = 1;
    return 0;
}

/* Upload a raw pre-multiplied BGRA file into the back buffer of a plane and
 * flip it. The file must contain exactly w*h*4 bytes; anything shorter is a
 * truncated write from the player and is rejected instead of being shown as
 * garbage. Position and size are in CRTC coordinates, so an OSD band at the
 * bottom is simply y = screen_height - h. */
static int commit_cue(struct plane *pl, int x, int y, int w, int h, const char *path)
{
    if (!pl->id) return -1;
    uint32_t crtc = pl->crtc ? pl->crtc : find_crtc();
    if (!crtc) { TLOG("kms-subd: unknown crtc (%s)\n", pl->name); return -1; }
    pl->crtc = crtc;

    /* Alternate buffers so that the buffer being scanned out is never touched. */
    int idx = (pl->cur + 1) % MAXBUF;
    struct dumbuf *b = &pl->buf[idx];
    if (buf_alloc(b, w, h) < 0) return -1;

    size_t expected = (size_t)w * 4 * h;
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    /* One reusable staging buffer of 16 MiB covers a 2560x1440x4 band (14 MiB)
     * and every smaller OSD surface; a full screen 4K surface would need more,
     * but the player never sends one. */
    if (!g_stage) g_stage = malloc(16 * 1024 * 1024);
    if (!g_stage) { fclose(f); return -1; }
    size_t got = fread(g_stage, 1, expected, f);
    fclose(f);
    if (got != expected) {
        TLOG("kms-subd: %s short read (%zu/%zu)\n", pl->name, got, expected);
        return -1;
    }
    /* Row by row because the kernel pitch (b->pitch) may be padded. */
    for (int row = 0; row < h; row++) {
        memcpy((unsigned char *)b->map + (size_t)row * b->pitch,
               g_stage + (size_t)row * w * 4, (size_t)w * 4);
    }

    /* src_w/src_h are 16.16 fixed point: shifting by 16 means "1:1 scale",
     * so no scaling is done by the VOP and the text keeps its pixel size. */
    int r = drmModeSetPlane(g_fd, pl->id, crtc, b->fb_id, 0,
                            x, y, w, h,
                            0, 0, w << 16, h << 16);
    if (r) { TLOG("kms-subd: SetPlane %s cue: %s\n", pl->name, strerror(errno)); return -1; }
    pl->cur = idx;
    pl->active = 1;
    TLOG("kms-subd: %s cue %dx%d+%d+%d fb=%u\n", pl->name, w, h, x, y, b->fb_id);
    return 0;
}

/* Detach the plane (fb_id = 0, zero size). The planes are never disabled with
 * drmModeSetPlane's flags: passing a null framebuffer is the documented way to
 * turn an overlay off without touching the CRTC. */
static int commit_clear(struct plane *pl)
{
    if (!pl->id) return -1;
    uint32_t crtc = pl->crtc ? pl->crtc : find_crtc();
    if (!crtc) return -1;
    pl->crtc = crtc;
    int r = drmModeSetPlane(g_fd, pl->id, crtc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (r) { TLOG("kms-subd: SetPlane %s clear: %s\n", pl->name, strerror(errno)); return -1; }
    if (pl->active) TLOG("kms-subd: %s clear\n", pl->name);
    pl->active = 0;
    return 0;
}

/* --- plane properties (alpha, blend mode) -------------------------------- */

/* Fetch one property of a plane by name. On success *val receives the current
 * value and *op_out the property set, which the caller must free together
 * with the returned drmModePropertyPtr. */
static drmModePropertyPtr prop_find(uint32_t obj, const char *name, uint64_t *val,
                                    drmModeObjectProperties **op_out)
{
    drmModeObjectProperties *op = drmModeObjectGetProperties(g_fd, obj, DRM_MODE_OBJECT_PLANE);
    if (!op) return NULL;
    for (unsigned j = 0; j < op->count_props; j++) {
        drmModePropertyPtr pp = drmModeGetProperty(g_fd, op->props[j]);
        if (!pp) continue;
        if (!strcmp(pp->name, name)) { *val = op->prop_values[j]; *op_out = op; return pp; }
        drmModeFreeProperty(pp);
    }
    drmModeFreeObjectProperties(op);
    return NULL;
}

/* Read the zpos property of a plane (0 when absent). zpos is how the kernel
 * orders planes on the same CRTC; the highest zpos is drawn last, on top. */
static unsigned prop_zpos(uint32_t id)
{
    uint64_t v = 0;
    drmModeObjectProperties *op = NULL;
    drmModePropertyPtr pp = prop_find(id, "zpos", &v, &op);
    if (pp) {
        drmModeFreeProperty(pp);
        drmModeFreeObjectProperties(op);
        return (unsigned)v;
    }
    return 0;
}

/* Configure the two properties that make the ARGB overlay composite instead
 * of replacing the video behind it:
 *
 *   alpha            = 65535 (fully opaque plane; transparency comes from the
 *                      per-pixel alpha channel, not from the plane alpha)
 *   pixel blend mode = "Pre-multiplied", because cairo writes pre-multiplied
 *                      ARGB32. Using "Coverage" or "None" would either ignore
 *                      the alpha channel or produce halos around the glyphs.
 *
 * Both properties are optional: a driver without them simply keeps its
 * defaults, which is why failures are ignored rather than fatal.
 */
static void configure_plane(struct plane *pl)
{
    uint64_t v = 0;
    drmModeObjectProperties *op = NULL;
    drmModePropertyPtr pp;

    pp = prop_find(pl->id, "alpha", &v, &op);
    if (pp) {
        drmModeObjectSetProperty(g_fd, pl->id, DRM_MODE_OBJECT_PLANE, pp->prop_id, 65535);
        drmModeFreeProperty(pp);
        drmModeFreeObjectProperties(op);
    }
    pp = prop_find(pl->id, "pixel blend mode", &v, &op);
    if (pp) {
        for (int i = 0; i < pp->count_enums; i++) {
            if (!strcmp(pp->enums[i].name, "Pre-multiplied")) {
                drmModeObjectSetProperty(g_fd, pl->id, DRM_MODE_OBJECT_PLANE,
                                         pp->prop_id, pp->enums[i].value);
                break;
            }
        }
        drmModeFreeProperty(pp);
        drmModeFreeObjectProperties(op);
    }
}

/* Hand the DRM fd to the player over the unix socket using SCM_RIGHTS.
 * SCM_RIGHTS duplicates the file description into the receiving process, so
 * both ends share the DRM master and the same file state (e.g. dropped-master
 * state). A single dummy byte is sent along, because the receiver needs a
 * recvmsg() with a payload to obtain the control message.
 * Only one client is expected (the player); the connection is closed at once. */
static int serve_fd(int listen_fd)
{
    int c = accept(listen_fd, NULL, NULL);
    if (c < 0) return -1;
    char buf[1] = { 0 };
    struct iovec io = { .iov_base = buf, .iov_len = 1 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof cbuf;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &g_fd, sizeof(int));
    ssize_t n = sendmsg(c, &m, 0);
    close(c);
    return n > 0 ? 0 : -1;
}

/* Choose the two ARGB planes to use when the caller did not name them:
 * collect every plane that supports ARGB8888 and is not the video plane, sort
 * by zpos, then take the top one for the OSD and the next one for subtitles. */
static int auto_select_planes(void)
{
    drmModePlaneRes *pr = drmModeGetPlaneResources(g_fd);
    if (!pr) { perror("planes"); return -1; }
    unsigned cand[64], cz[64];
    int n = 0;
    for (unsigned i = 0; i < pr->count_planes && n < 64; i++) {
        unsigned id = pr->planes[i];
        if ((int)id == g_vplane) continue;
        drmModePlane *pl = drmModeGetPlane(g_fd, id);
        if (!pl) continue;
        int has_argb = 0;
        for (unsigned k = 0; k < pl->count_formats; k++)
            if (pl->formats[k] == DRM_FORMAT_ARGB8888) { has_argb = 1; break; }
        drmModeFreePlane(pl);
        if (!has_argb) continue;
        unsigned z = prop_zpos(id);
        /* insertion sort, ascending zpos; candidate lists are tiny */
        int j = n++;
        while (j > 0 && cz[j - 1] > z) { cz[j] = cz[j - 1]; cand[j] = cand[j - 1]; j--; }
        cz[j] = z;
        cand[j] = id;
    }
    drmModeFreePlaneResources(pr);
    if (n < 2) {
        fprintf(stderr, "kms-subd: fewer than two ARGB planes available (%d)\n", n);
        return -1;
    }
    if (!g_pl[P_OSD].id) { g_pl[P_OSD].id = (int)cand[n - 1]; g_pl[P_OSD].zpos = cz[n - 1]; }
    if (!g_pl[P_SUB].id) { g_pl[P_SUB].id = (int)cand[n - 2]; g_pl[P_SUB].zpos = cz[n - 2]; }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: kms-subd <socket> <video-plane> [sub-plane] [osd-plane]\n");
        return 2;
    }
    const char *sockpath = argv[1];
    g_vplane = atoi(argv[2]);
    g_pl[P_SUB].name = "sub";
    g_pl[P_OSD].name = "osd";
    g_pl[P_SUB].id = argc > 3 ? atoi(argv[3]) : 0;
    g_pl[P_OSD].id = argc > 4 ? atoi(argv[4]) : 0;
    /* cur starts at -1 conceptually: the first cue targets buffer 0. Because
     * struct plane is static, cur is 0 and (0+1)%2 = 1, which is harmless. */

    g_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (g_fd < 0) { perror("/dev/dri/card0"); return 1; }

    if (!g_pl[P_SUB].id || !g_pl[P_OSD].id) {
        if (auto_select_planes() < 0) return 1;
    }
    if (!g_pl[P_SUB].zpos) g_pl[P_SUB].zpos = prop_zpos((uint32_t)g_pl[P_SUB].id);
    if (!g_pl[P_OSD].zpos) g_pl[P_OSD].zpos = prop_zpos((uint32_t)g_pl[P_OSD].id);

    configure_plane(&g_pl[P_SUB]);
    configure_plane(&g_pl[P_OSD]);

    /* The socket is created before drmSetMaster: the player waits for the
     * path to exist and then connects, so this ordering keeps startup short.
     * A stale socket from a previous crash is unlinked first. */
    unlink(sockpath);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, sockpath, sizeof a.sun_path - 1);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    /* Mode 0660: the player runs as the same user, nobody else needs it. */
    chmod(sockpath, 0660);
    listen(lfd, 4);

    /* Take the master. The player switched VT beforehand; without that switch
     * this ioctl fails with EACCES because the current VT still owns the
     * console. A failure is not fatal: if another process already holds the
     * master (or the kernel does not require it), the planes still work. */
    if (drmSetMaster(g_fd) != 0) {
        TLOG("kms-subd: drmSetMaster: %s (continuing without master)\n", strerror(errno));
    } else {
        TLOG("kms-subd: DRM master acquired, video %d, sub %d (zpos %u), osd %d (zpos %u)\n",
             g_vplane, g_pl[P_SUB].id, g_pl[P_SUB].zpos, g_pl[P_OSD].id, g_pl[P_OSD].zpos);
    }

    /* Main loop: stdin carries the protocol, the socket carries fd requests.
     * Bytes are accumulated until '\n' because a read() can split a line. */
    char line[4096];
    size_t len = 0;
    int done = 0;
    while (!done) {
        struct pollfd pf[2] = {
            { .fd = 0, .events = POLLIN },
            { .fd = lfd, .events = POLLIN },
        };
        int r = poll(pf, 2, -1);
        if (r < 0) { if (errno == EINTR) continue; break; }

        if (pf[1].revents & POLLIN) serve_fd(lfd);
        if (!(pf[0].revents & (POLLIN | POLLHUP))) continue;

        char c;
        ssize_t n = read(0, &c, 1);
        if (n <= 0) break;              /* player exited: clean up and leave */
        if (c != '\n') {
            if (len < sizeof line - 1) line[len++] = c;
            continue;
        }
        line[len] = 0;
        len = 0;

        if (strncmp(line, "quit", 4) == 0) { done = 1; break; }

        struct plane *pl = NULL;
        const char *cmd = line;
        if (strncmp(line, "sub ", 4) == 0) { pl = &g_pl[P_SUB]; cmd = line + 4; }
        else if (strncmp(line, "osd ", 4) == 0) { pl = &g_pl[P_OSD]; cmd = line + 4; }
        /* Bare "cue"/"clear" target the subtitle plane: that was the original
         * single-plane protocol and is kept for compatibility. */
        else if (strncmp(line, "cue ", 4) == 0 || strncmp(line, "clear", 5) == 0) pl = &g_pl[P_SUB];
        else { TLOG("kms-subd: unknown command: %.40s\n", line); continue; }

        if (strncmp(cmd, "cue ", 4) == 0) {
            int x, y, w, h;
            char path[2048];
            if (sscanf(cmd + 4, "%d %d %d %d %2047s", &x, &y, &w, &h, path) == 5)
                commit_cue(pl, x, y, w, h, path);
            else
                TLOG("kms-subd: malformed cue\n");
        } else if (strncmp(cmd, "clear", 5) == 0) {
            commit_clear(pl);
        } else if (strncmp(cmd, "show", 4) == 0) {
            /* Re-attach the last buffer without re-reading a file. Used when a
             * plane was cleared and the same content must come back. */
            if (pl->cur >= 0 && pl->buf[pl->cur].used && pl->crtc) {
                struct dumbuf *b = &pl->buf[pl->cur];
                drmModeSetPlane(g_fd, pl->id, pl->crtc, b->fb_id, 0,
                                0, 0, b->w, b->h, 0, 0, b->w << 16, b->h << 16);
                pl->active = 1;
            }
        }
    }

    /* Teardown: clear the planes first (so the last frame is not left on
     * screen over the desktop), then release every buffer and drop master. */
    commit_clear(&g_pl[P_SUB]);
    commit_clear(&g_pl[P_OSD]);
    close(lfd);
    unlink(sockpath);
    for (int p = 0; p < NPLANS; p++)
        for (int i = 0; i < MAXBUF; i++) {
            struct dumbuf *b = &g_pl[p].buf[i];
            if (!b->used) continue;
            if (b->map) munmap(b->map, b->size);
            if (b->fb_id) drmModeRmFB(g_fd, b->fb_id);
            struct drm_mode_destroy_dumb d = { .handle = b->handle };
            drmIoctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
    drmDropMaster(g_fd);
    close(g_fd);
    return 0;
}
