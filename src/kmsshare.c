// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

/*
 * kmsshare.c - LD_PRELOAD shim that lends a DRM master to another process.
 *
 * ============================================================================
 * WHY THIS FILE EXISTS
 * ============================================================================
 * The kernel allows exactly one DRM master per card, and drmModeSetPlane()
 * requires that status. This player needs several independent writers on the
 * same CRTC: kmssink scans out the video plane, and kms-subd drives the two
 * ARGB overlay planes (subtitles and OSD). Only one of them can be master.
 *
 * The trick: kms-subd takes the master first, then hands its open file
 * descriptor to whoever asks over a Unix socket, using SCM_RIGHTS. A descriptor
 * received that way points at the SAME struct file, so it carries the same
 * master flag: both processes can then program their own planes.
 *
 * This shim is preloaded into the GStreamer sink process. It watches for
 * open()/open64()/openat() on /dev/dri/card* and returns a dup() of the shared
 * descriptor instead of opening the device fresh - a fresh open would be
 * refused, or would silently be a non-master client that cannot set planes.
 *
 * It also neutralises drmSetMaster()/drmDropMaster(): the sink normally tries
 * to become master on its own, and dropping the master (which happens on close
 * or on an explicit call) would revoke kms-subd's right to drive its planes.
 * Both calls therefore become no-ops that report success.
 *
 * Configured through the KMS_SHARE_SOCK environment variable, set by the
 * kms-play wrapper to /tmp/kms-subd-<uid>.sock.
 *
 * Failure policy: if the socket is unreachable, the shim falls back to the real
 * open() and logs to stderr. The sink will then fail to set planes, but the
 * process does not crash - the failure stays diagnosable in the player log.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Shared DRM descriptor, or -1 when unavailable. Cached after the first
 * attempt: the socket is meant to be used once, and retrying on every open()
 * would just add latency to unrelated file operations. */
static int g_shared = -1;
static int g_tried;

/* Resolved lazily through dlsym(RTLD_NEXT, ...) so the shim never recurses
 * into its own overrides. */
static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);

/*
 * Connect to the sharing socket and receive the DRM descriptor via SCM_RIGHTS.
 *
 * Returns the received descriptor (owned by this process, duplicated per open
 * call by the caller) or -1 when sharing is not configured or not available.
 *
 * The protocol is one byte of payload plus one control message carrying the
 * descriptor; anything else means the daemon is gone or misbehaving.
 */
static int get_shared(void)
{
    if (g_tried) return g_shared;
    g_tried = 1;

    const char *path = getenv("KMS_SHARE_SOCK");
    if (!path || !*path) return -1;

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "[kmsshare] connect %s: %s\n", path, strerror(errno));
        close(s);
        return -1;
    }

    char c = 0;
    struct iovec io = { .iov_base = &c, .iov_len = 1 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof cbuf;

    if (recvmsg(s, &m, 0) <= 0) {
        close(s);
        return -1;
    }

    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    int fd = -1;
    if (cm && cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
        memcpy(&fd, CMSG_DATA(cm), sizeof fd);

    close(s);
    if (fd >= 0)
        fprintf(stderr, "[kmsshare] shared DRM fd received: %d\n", fd);
    else
        fprintf(stderr, "[kmsshare] socket OK but no descriptor received\n");

    g_shared = fd;
    return fd;
}

/* True for the DRM render nodes we want to intercept: /dev/dri/card0, card1...
 * Deliberately not render nodes (/dev/dri/renderD*), which cannot be master
 * and are used for a different purpose. */
static int is_card(const char *p)
{
    return p && strncmp(p, "/dev/dri/card", 13) == 0;
}

int open(const char *path, int flags, ...)
{
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    if (is_card(path)) {
        int fd = get_shared();
        if (fd >= 0) return dup(fd);
    }
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    if (!real_open64) real_open64 = dlsym(RTLD_NEXT, "open64");
    if (is_card(path)) {
        int fd = get_shared();
        if (fd >= 0) return dup(fd);
    }
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    if (real_open64) return real_open64(path, flags, mode);
    /* Some musl/older libcs do not export open64; fall back to open. */
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    return real_open(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    if (is_card(path)) {
        int fd = get_shared();
        if (fd >= 0) return dup(fd);
    }
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    return real_openat(dirfd, path, flags, mode);
}

/*
 * The descriptor is already master on that struct file: nothing to do, and
 * above all never release it - dropping the master here would revoke kms-subd's
 * ability to program its own planes for the rest of the session.
 */
int drmSetMaster(int fd) { (void)fd; return 0; }
int drmDropMaster(int fd) { (void)fd; return 0; }
