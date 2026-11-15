// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

/*
 * kms-keywait.c - block until ESC or Q is pressed on any keyboard.
 *
 * ============================================================================
 * WHY THIS FILE STILL EXISTS
 * ============================================================================
 * This was the first way the player learned about "stop": a tiny helper that
 * blocks in poll() and exits, which is enough for a one-shot playback session.
 * The current player uses kms-keys instead, because it needs the full keyboard
 * map and never exits while playing.
 *
 * kms-keywait is kept because older installations, desktop launchers and
 * scripts still call it, and because it is a convenient smoke test of the input
 * path: if it does not react, the player will not either.
 *
 * Behaviour: opens every /dev/input/event* node that advertises KEY_ESC,
 * waits with a single poll() on all of them, and returns 0 as soon as ESC or Q
 * is pressed (value 1 = press; releases and autorepeat are ignored).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <errno.h>
#include <linux/input.h>
#include <sys/ioctl.h>

/* Upper bound on simultaneously watched devices. A desktop board exposes a
 * handful at most; the cap only exists to keep static arrays small. */
#define MAXFD 64

/* The path is "/dev/input/" (11 chars) plus a dentry name up to 255 bytes plus
 * the terminating NUL - hence 267. Using a smaller buffer makes the compiler
 * warn about truncation, and a truncated path would silently fail to open. */
#define PATH_MAX_INPUT 300

/*
 * Test whether a device can report the ESC key.
 *
 * EVIOCGBIT(EV_KEY, ...) fills a bitmap with one bit per key code. A device
 * that has no ESC bit is a mouse, a power button or a touchscreen: opening it
 * would only add a descriptor to the poll() set for nothing.
 */
static int has_esc(int fd)
{
    unsigned long bits[(KEY_MAX / 8 / sizeof(unsigned long)) + 2];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    unsigned long word = KEY_ESC / (8 * sizeof(unsigned long));
    unsigned long bit  = KEY_ESC % (8 * sizeof(unsigned long));
    return (bits[word] >> bit) & 1UL;
}

int main(void)
{
    int fds[MAXFD];
    int n = 0;

    DIR *d = opendir("/dev/input");
    if (!d) { perror("/dev/input"); return 1; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < MAXFD) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;

        char path[PATH_MAX_INPUT];
        int written = snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        if (written < 0 || (size_t)written >= sizeof path) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (!has_esc(fd)) { close(fd); continue; }

        fds[n++] = fd;
    }
    closedir(d);

    if (n == 0) {
        fprintf(stderr, "kms-keywait: no keyboard device found\n");
        return 1;
    }

    struct pollfd pf[MAXFD];
    for (int i = 0; i < n; i++) {
        pf[i].fd = fds[i];
        pf[i].events = POLLIN;
        pf[i].revents = 0;
    }

    for (;;) {
        int r = poll(pf, (nfds_t)n, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }

        for (int i = 0; i < n; i++) {
            if (!(pf[i].revents & POLLIN)) continue;

            struct input_event ev;
            ssize_t got;
            while ((got = read(pf[i].fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                /* value 1 = key press. Q is accepted as well as ESC because
                 * some remote controls have no ESC at all. */
                if (ev.type == EV_KEY && ev.value == 1 &&
                    (ev.code == KEY_ESC || ev.code == KEY_Q))
                    return 0;
            }
        }
    }
}
