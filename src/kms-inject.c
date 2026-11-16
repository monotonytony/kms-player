// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>

/*
 * kms-inject - virtual keyboard (uinput) used to test the KMS player.
 *
 * ============================================================================
 * WHY
 * ============================================================================
 * The player is tested from another machine: nobody can sit in front of the
 * screen and press keys. This tool creates a temporary uinput device that
 * looks like a real keyboard to the kernel, presses one key and destroys the
 * device again. kms-keys will pick it up like any other keyboard, so the test
 * exercises the complete chain:
 *
 *   uinput -> evdev -> kms-keys -> player -> cairo -> kms-subd -> SetPlane
 *
 * Every emission is timestamped with CLOCK_MONOTONIC (the same clock the
 * player logs), which is what makes latency measurement possible: compare the
 * "inject" timestamp with the "osd cue" timestamp in ~/.cache/kms-play/last.log.
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *   kms-inject <NAME> [--delay SEC] [--hold MS] [--repeat N] [--interval MS]
 *                     [--taps N] [--tap-interval MS] [--ts FILE]
 *
 *   --delay     wait before emitting, lets the player finish starting (1.0 s)
 *   --hold      how long the key stays pressed, in ms (60)
 *   --repeat N  emit N value=2 (repeat) events, simulating a kernel repeat
 *   --interval  spacing between repeats, in ms (40)
 *   --ts        timestamp file, default /tmp/kms-inject.ts
 *   --taps N    emit N full press/release taps from the SAME device (1)
 *   --tap-interval  spacing between taps, press to press, in ms (450)
 *
 * Note on --repeat: since kms-keys owns autorepeat and drops kernel value=2
 * events (see kms-keys.c), these synthetic repeats are ignored by the player.
 * Use --hold to exercise the real autorepeat path instead.
 *
 * Note on --taps: creating a uinput device costs ~0.8 s (node, udev, settle),
 * so injecting N presses as N separate runs spaces them 1.3 s apart — longer
 * than the player's 1 s scale-reset window, which makes the scale look broken
 * when it is not. --taps emits the whole sequence from one device, at the
 * cadence a human actually uses.
 *
 * The device is chmod 0666 as soon as it appears: the player runs as an
 * unprivileged user and must be able to open the new event node before the
 * key is emitted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

/* Same name table as kms-keys.c: the tokens are the test-suite vocabulary. */
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

/* Monotonic seconds; shared time base with kms-keys and the player log. */
static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Emit one EV_KEY event followed by SYN_REPORT. The report is what makes the
 * kernel deliver the event to readers; without it the key would stay queued. */
static void emit_key(int fd, unsigned code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    ssize_t ignored = write(fd, &ev, sizeof ev);
    (void)ignored;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    ignored = write(fd, &ev, sizeof ev);
    (void)ignored;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: kms-inject <NAME> [--delay s] [--hold ms] [--ts file]\n");
        return 2;
    }
    const char *name = argv[1];
    double delay = 1.0;
    int hold = 60;
    int repeat = 0;
    int interval = 40;
    int taps = 1;
    int tap_interval = 450;
    const char *tsfile = "/tmp/kms-inject.ts";

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--delay") && i + 1 < argc) delay = atof(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ts") && i + 1 < argc) tsfile = argv[++i];
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--taps") && i + 1 < argc) taps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tap-interval") && i + 1 < argc) tap_interval = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }

    unsigned code = 0;
    for (int i = 0; KEYS[i].name; i++)
        if (!strcmp(KEYS[i].name, name)) { code = KEYS[i].code; break; }
    if (!code) { fprintf(stderr, "unknown key: %s\n", name); return 2; }

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); return 1; }
    /* Only EV_KEY and EV_SYN are advertised. EV_REP is deliberately NOT set:
     * kms-keys generates autorepeat itself, so the test device behaves like
     * the Bluetooth remote, not like the USB receiver. */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (int i = 0; KEYS[i].name; i++) ioctl(fd, UI_SET_KEYBIT, KEYS[i].code);

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof uidev);
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "kms-inject-test");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x1234;
    uidev.id.product = 0x5679;
    uidev.id.version = 1;
    if (write(fd, &uidev, sizeof uidev) < 0) { perror("write uidev"); return 1; }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return 1; }

    /* The device node appears asynchronously; find it by name and relax its
     * permissions as early as possible so that kms-keys can attach before the
     * key is emitted. 40 * 25 ms = 1 s worst case. */
    for (int attempt = 0; attempt < 40; attempt++) {
        int found = 0;
        for (int i = 0; i < 96; i++) {
            char path[128], namepath[160], devname[256] = "";
            snprintf(path, sizeof path, "/dev/input/event%d", i);
            snprintf(namepath, sizeof namepath, "/sys/class/input/event%d/device/name", i);
            FILE *f = fopen(namepath, "r");
            if (!f) continue;
            if (fgets(devname, sizeof devname, f) && strstr(devname, "kms-inject-test")) {
                chmod(path, 0666);
                fprintf(stderr, "kms-inject: device %s\n", path);
                found = 1;
            }
            fclose(f);
            if (found) break;
        }
        if (found) break;
        usleep(25 * 1000);
    }
    /* Extra settle time: udev and kms-keys need to see the node before the
     * event stream starts, otherwise the first key is lost. */
    usleep(150 * 1000);

    if (delay > 0) usleep((useconds_t)(delay * 1e6));

    double t0 = mono();
    FILE *tf = fopen(tsfile, "w");
    if (tf) { fprintf(tf, "inject %s %.6f\n", name, t0); fclose(tf); }
    printf("inject %s %.6f\n", name, t0);
    fflush(stdout);

    if (taps < 1) taps = 1;
    for (int tap = 0; tap < taps; tap++) {
        emit_key(fd, code, 1);
        for (int i = 0; i < repeat; i++) {
            usleep((useconds_t)interval * 1000);
            emit_key(fd, code, 2);    /* value 2 = autorepeat, ignored by kms-keys */
        }
        if (hold > 0) usleep((useconds_t)hold * 1000);
        emit_key(fd, code, 0);
        if (tap + 1 < taps) {
            int gap = tap_interval - hold;
            if (gap > 0) usleep((useconds_t)gap * 1000);
        }
    }
    usleep(50 * 1000);

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
