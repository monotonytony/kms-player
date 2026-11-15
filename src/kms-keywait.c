/* Attend ESC ou Q sur n importe quel peripherique d entree exposant ces touches. */
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

#define MAXFD 64

static int has_esc(int fd) {
    unsigned long bits[(KEY_MAX / 8 / sizeof(unsigned long)) + 2];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    unsigned long w = KEY_ESC / (8 * sizeof(unsigned long));
    unsigned long b = KEY_ESC % (8 * sizeof(unsigned long));
    return (bits[w] >> b) & 1UL;
}

int main(void) {
    int fds[MAXFD], n = 0;
    DIR *d = opendir("/dev/input");
    if (!d) { perror("/dev/input"); return 1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < MAXFD) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char p[128];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (!has_esc(fd)) { close(fd); continue; }
        fds[n++] = fd;
    }
    closedir(d);
    if (n == 0) { fprintf(stderr, "aucun peripherique clavier\n"); return 1; }

    struct pollfd pf[MAXFD];
    for (int i = 0; i < n; i++) { pf[i].fd = fds[i]; pf[i].events = POLLIN; pf[i].revents = 0; }

    for (;;) {
        int r = poll(pf, (nfds_t)n, -1);
        if (r < 0) { if (errno == EINTR) continue; perror("poll"); return 1; }
        for (int i = 0; i < n; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            struct input_event ev;
            ssize_t k;
            while ((k = read(pf[i].fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                if (ev.type == EV_KEY && ev.value == 1 &&
                    (ev.code == KEY_ESC || ev.code == KEY_Q)) return 0;
            }
        }
    }
}
