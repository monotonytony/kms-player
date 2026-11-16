// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

/*
 * kms-keys - minimal keyboard daemon for the KMS player.
 *
 * ============================================================================
 * WHY THIS PROGRAM EXISTS
 * ============================================================================
 * kms-play-py runs full screen on its own VT and owns the DRM master, so it
 * cannot use X11/Wayland for input. Reading /dev/input/event* directly from
 * Python would either block the GLib main loop or require one thread per
 * device. This daemon does one thing: it reads every keyboard-capable input
 * device with a single poll() loop and funnels the events into ONE line
 * oriented stream on stdout:
 *
 *     key <NAME> <1|0|2> <code>   1 = press, 0 = release, 2 = autorepeat
 *
 * <NAME> is the KEY_* token, i.e. a PHYSICAL POSITION: KEY_A is the second key
 * of the home row whatever the language. On an AZERTY keyboard the key labelled
 * A is at the US Q position and reports KEY_Q. <code> is the raw evdev code so
 * the player can translate it through the XKB layout (see keylayout in
 * kms-play-py) instead of guessing from the US name.
 *
 * The parent (kms-play-py) reads that stream on a background thread and
 * dispatches each line into the GLib main loop. kms-keys knows nothing about
 * the player semantics (seek scale, volume, subtitle cycling): it is a pure
 * transport plus the autorepeat generator described below.
 *
 * ============================================================================
 * AUTOREPEAT - WHY IT LIVES HERE AND NOT IN THE KERNEL
 * ============================================================================
 * The kernel only generates autorepeat for devices that advertise EV_REP.
 * On this box the USB Logitech receiver does (EV=120013, bit EV_REP set), but
 * the Edifier Bluetooth remote (EV=13) and every uinput test device do not.
 * Relying on the kernel would therefore make the seek scale depend on an
 * invisible per-device capability, and it would also follow whatever repeat
 * delay/rate X11 or the desktop session configured - two things the player
 * cannot observe.
 *
 * Decision: kms-keys is the SINGLE SOURCE OF TRUTH for repetition.
 *   - it keeps its own pressed/released bookkeeping per device and per key;
 *   - after REPEAT_DELAY_MS of continuous pressure it emits value=2 every
 *     REPEAT_INTERVAL_MS until the key is released;
 *   - it DROPS value=2 events coming from the device. Otherwise a keyboard
 *     with EV_REP would count every step twice (once from the kernel, once
 *     from us) and the seek scale would advance two rungs per repeat.
 *
 * REPEAT_INTERVAL_MS is deliberately coupled to the reader's debounce:
 * kms-play-py ignores steps that arrive less than 120 ms apart (SEEK_DEBOUNCE),
 * so a 60 ms repeat interval yields ~8 accepted steps per second. That is
 * fast enough to cross a long movie quickly, slow enough to stay controllable.
 *
 * ============================================================================
 * DEVICE HANDLING
 * ============================================================================
 *  - /dev/input is rescanned every RESCAN_MS because keyboards (and uinput
 *    devices created by kms-inject for tests) can appear after startup.
 *  - A device that goes away while keys are down would never deliver their
 *    release, leaving the player stuck in "held" state; proche()/close_device()
 *    synthesises the missing releases. That is why every syntactic path that
 *    drops a device (EOF, ENODEV, POLLHUP) funnels through it.
 *  - An inotify based hotplug watcher was tried and abandoned: it managed to
 *    block the daemon in a read. Polling a directory every 500 ms costs
 *    nothing on this SoC.
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *   kms-keys            run the daemon, print events on stdout
 *   kms-keys --list     print the detected devices and exit (debug)
 *
 * Exits cleanly on SIGTERM/SIGINT. SIGPIPE is ignored so that a parent dying
 * does not kill the daemon with a signal before it notices EOF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <linux/input.h>
#include <sys/ioctl.h>

/* Maximum number of input devices watched at once. The box has 7 event nodes
 * today; 64 leaves room for several uinput test devices and hotplug churn. */
#define MAX_DEV 64

/* Delay before the first synthesised repeat, in ms. Matches the usual desktop
 * "key repeat delay" so that a short tap never repeats. */
#define REPEAT_DELAY_MS 300

/* Interval between synthesised repeats, in ms. See the file header: kept
 * comfortably below the reader's 120 ms seek debounce on purpose. */
#define REPEAT_INTERVAL_MS 60

/* How often /dev/input is rescanned for new devices, in ms.
 * Kept equal to POLL_MAX_MS on purpose: a uinput test device (kms-inject) only
 * lives for ~400 ms, so a slower scan would miss it entirely. A directory read
 * ten times per second costs microseconds on this SoC. */
#define RESCAN_MS 100

/* Longest poll() timeout, in ms. Bounds the reaction time to a hotplug and
 * keeps the repeat scheduler honest even when no fd is readable. */
#define POLL_MAX_MS 100

/* The key table is the contract with kms-play-py and kms-inject: the names
 * below are the exact tokens that travel on stdout. Adding a key means
 * touching this table in the three programs. */
struct key_name {
    unsigned code;
    const char *name;
};

static const struct key_name KEYS[] = {
    { KEY_ESC,          "ESC" },
    { KEY_Q,            "Q" },
    { KEY_A,            "A" },
    { KEY_S,            "S" },
    { KEY_SPACE,        "SPACE" },
    { KEY_LEFT,         "LEFT" },
    { KEY_RIGHT,        "RIGHT" },
    { KEY_UP,           "UP" },
    { KEY_DOWN,         "DOWN" },
    { KEY_ENTER,        "ENTER" },
    { KEY_VOLUMEUP,     "VOLUMEUP" },
    { KEY_VOLUMEDOWN,   "VOLUMEDOWN" },
    { KEY_MUTE,         "MUTE" },
    { KEY_PLAYPAUSE,    "PLAYPAUSE" },
    { KEY_NEXTSONG,     "NEXTSONG" },
    { KEY_PREVIOUSSONG, "PREVIOUSSONG" },
    { 0, NULL }
};

/* Number of entries in KEYS, computed once so that the per-device state
 * arrays below have a compile-time size. */
#define NKEYS ((int)(sizeof(KEYS) / sizeof(KEYS[0])) - 1)

/* Monotonic clock in seconds, used for repeat scheduling. CLOCK_MONOTONIC is
 * immune to wall clock jumps (NTP, manual date changes). */
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Translate an evdev key code into its protocol name, or NULL if the key is
 * not part of the whitelist above. */
static const char *key_name(unsigned code)
{
    for (int i = 0; KEYS[i].name; i++)
        if (KEYS[i].code == code) return KEYS[i].name;
    return NULL;
}

/* Index of a key code inside KEYS, or -1. Needed to index the per-device
 * held/repeat arrays; -1 means "not whitelisted, do not track". */
static int key_index(unsigned code)
{
    for (int i = 0; KEYS[i].name; i++)
        if (KEYS[i].code == code) return i;
    return -1;
}

/* Does this event device expose at least one key we care about?
 * EVIOCGBIT(EV_KEY, ...) returns a bitmap of supported key codes. We only
 * look at the whitelisted ones, which filters out mice, switches and audio
 * jack detection nodes that also live under /dev/input. */
static int is_interesting(int fd)
{
    unsigned long bits[(KEY_MAX / 8 / sizeof(unsigned long)) + 2];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    for (int i = 0; KEYS[i].name; i++) {
        unsigned c = KEYS[i].code;
        unsigned long word = c / (8 * sizeof(unsigned long));
        unsigned long bit = c % (8 * sizeof(unsigned long));
        if ((bits[word] >> bit) & 1UL) return 1;
    }
    return 0;
}

/* Set by the signal handler; poll() returns EINTR and the loop exits. */
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* Per-device state.
 *   held  : bitmask of KEYS[] indices currently pressed on this device
 *   next  : absolute monotonic deadline of the next repeat for that key,
 *           0.0 meaning "no repeat scheduled" (key is up)
 * Keeping this per device matters: two keyboards can press the same key and
 * releasing it on one must not cancel the repeat of the other. */
struct dev {
    int fd;
    char path[300];
    unsigned held;
    double next[NKEYS];
};

static struct dev g_dev[MAX_DEV];
static int g_ndev = 0;

/* Print one event line and flush. A write failure means the parent is gone:
 * exit quietly rather than spin. */
static int emit(const char *name, int value, unsigned code)
{
    if (printf("key %s %d %u\n", name, value, code) < 0) return -1;
    fflush(stdout);
    return 0;
}

/* Drop a device: synthesise the release of every key it still holds, close
 * the fd and mark the slot free. Called on EOF/ENODEV and on POLLHUP|POLLERR.
 * The synthesised releases are what keeps the reader's "key held" bookkeeping
 * (and therefore the repeat scheduler) consistent after a USB unplug. */
static void close_device(struct dev *d)
{
    if (d->fd >= 0) {
        for (int i = 0; KEYS[i].name; i++)
            if (d->held & (1u << i)) emit(KEYS[i].name, 0, KEYS[i].code);
        close(d->fd);
    }
    d->fd = -1;
    d->path[0] = 0;
    d->held = 0;
    memset(d->next, 0, sizeof d->next);
}

/* Is this path already watched? Prevents the rescan from opening the same
 * event node twice (which would duplicate every event). */
static int already_open(const char *p)
{
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].fd >= 0 && !strcmp(g_dev[i].path, p)) return 1;
    return 0;
}

/* Rescan /dev/input and attach to every new interesting device.
 * Returns the number of devices added. O_NONBLOCK is essential: a blocking
 * read on an evdev node would stall the whole daemon if an event is raced. */
static int scan(int verbose)
{
    int added = 0;
    DIR *d = opendir("/dev/input");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_ndev < MAX_DEV) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/input/%.255s", e->d_name);
        if (already_open(p)) continue;
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;                 /* EACCES is normal for other users */
        if (!is_interesting(fd)) { close(fd); continue; }
        struct dev *slot = &g_dev[g_ndev++];
        slot->fd = fd;
        slot->held = 0;
        memset(slot->next, 0, sizeof slot->next);
        snprintf(slot->path, sizeof slot->path, "%s", p);
        added++;
        if (verbose) {
            char name[256] = "?";
            ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
            fprintf(stderr, "kms-keys: %s: %s\n", p, name);
        }
    }
    closedir(d);
    return added;
}

/* Emit the repeats that are due, on every device.
 * The deadline is advanced by exactly one interval per emitted repeat; if the
 * process was starved for longer than an interval we resynchronise instead of
 * emitting a burst of catch-up repeats (a burst would be swallowed by the
 * reader debounce anyway and would only add jitter). */
static void run_repeats(void)
{
    double t = now_s();
    for (int i = 0; i < g_ndev; i++) {
        struct dev *d = &g_dev[i];
        if (d->fd < 0) continue;
        for (int k = 0; KEYS[k].name; k++) {
            if (!(d->held & (1u << k)) || d->next[k] == 0.0) continue;
            if (t < d->next[k]) continue;
            if (emit(KEYS[k].name, 2, KEYS[k].code) < 0) return;
            d->next[k] += REPEAT_INTERVAL_MS / 1000.0;
            if (d->next[k] < t) d->next[k] = t + REPEAT_INTERVAL_MS / 1000.0;
        }
    }
}

/* Earliest pending repeat deadline, or -1 when nothing is scheduled.
 * Used to size the poll() timeout so the loop sleeps exactly as long as it can. */
static double next_deadline(void)
{
    double best = -1.0;
    for (int i = 0; i < g_ndev; i++) {
        struct dev *d = &g_dev[i];
        if (d->fd < 0) continue;
        for (int k = 0; KEYS[k].name; k++) {
            if (!(d->held & (1u << k)) || d->next[k] == 0.0) continue;
            if (best < 0.0 || d->next[k] < best) best = d->next[k];
        }
    }
    return best;
}

/* Drain everything currently queued on one device.
 * Returns 1 if the device is gone (EOF / ENODEV), 0 otherwise. */
static int drain_device(struct dev *d)
{
    struct input_event ev;
    while (1) {
        ssize_t n = read(d->fd, &ev, sizeof ev);
        if (n == (ssize_t)sizeof ev) {
            if (ev.type != EV_KEY) continue;      /* EV_SYN, EV_MSC, EV_LED... */
            /* value > 2 does not exist in the input protocol; ignore junk. */
            if (ev.value > 2) continue;
            const char *name = key_name(ev.code);
            if (!name) continue;
            int k = key_index(ev.code);
            if (ev.value == 2) {
                /* Kernel autorepeat: deliberately dropped. kms-keys owns
                 * repetition (see file header) so the scale advances once
                 * per rung, not once per kernel repeat. */
                continue;
            }
            if (ev.value == 1) {
                d->held |= (1u << k);
                d->next[k] = now_s() + REPEAT_DELAY_MS / 1000.0;
            } else {
                d->held &= ~(1u << k);
                d->next[k] = 0.0;
            }
            if (emit(name, ev.value, ev.code) < 0) return 1;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n < 0 && errno == EINTR) continue;
        return 1;                    /* 0 = EOF, ENODEV = unplugged */
    }
}

int main(int argc, char **argv)
{
    int list_only = (argc > 1 && !strcmp(argv[1], "--list"));
    scan(1);
    if (list_only) return 0;
    /* No keyboard at startup is not fatal: devices can be plugged later and
     * the rescan below will pick them up. The player stays usable with a
     * remote or a keyboard plugged after launch. */
    if (g_ndev == 0) fprintf(stderr, "kms-keys: no keyboard yet, waiting for hotplug\n");

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);        /* detect the dead parent via write() == -1 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct pollfd pf[MAX_DEV];
    double last_scan = now_s();
    while (!g_stop) {
        int n = g_ndev;
        for (int i = 0; i < n; i++) {
            pf[i].fd = g_dev[i].fd;
            pf[i].events = POLLIN;
            pf[i].revents = 0;
        }
        /* Sleep until either an event arrives or the next repeat is due,
         * never longer than POLL_MAX_MS so hotplug is noticed quickly. */
        double dl = next_deadline();
        int timeout = POLL_MAX_MS;
        if (dl > 0.0) {
            int ms = (int)((dl - now_s()) * 1000.0);
            if (ms < 0) ms = 0;
            if (ms < timeout) timeout = ms;
        }
        int r = poll(pf, (nfds_t)n, timeout);
        if (r < 0 && errno != EINTR) { perror("poll"); return 1; }

        for (int i = 0; i < n; i++) {
            if (!pf[i].revents) continue;
            /* Read pending events before acting on POLLHUP: a device that
             * disappears can deliver its final release and POLLHUP in the
             * same poll() round, and that release must not be lost. */
            if (!(pf[i].revents & POLLIN)) {
                if (pf[i].revents & (POLLHUP | POLLERR | POLLNVAL)) close_device(&g_dev[i]);
                continue;
            }
            if (drain_device(&g_dev[i])) close_device(&g_dev[i]);
        }

        run_repeats();

        /* Compact the array so that freed slots can be reused by the rescan.
         * Order does not matter: devices are independent. */
        int j = 0;
        for (int i = 0; i < g_ndev; i++) {
            if (g_dev[i].fd < 0) continue;
            if (j != i) g_dev[j] = g_dev[i];
            j++;
        }
        g_ndev = j;

        /* Hotplug check: cheap directory read, at most every RESCAN_MS. */
        if (now_s() - last_scan >= RESCAN_MS / 1000.0) {
            last_scan = now_s();
            scan(1);
        }
    }
    for (int i = 0; i < g_ndev; i++) close_device(&g_dev[i]);
    return 0;
}
