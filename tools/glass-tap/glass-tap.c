/* glass-tap: the Glass's touchpad (the Synaptics RMI4 pad on the side,
 * "sensor00fn11") reported as gestures, one line per gesture on stdout:
 *
 *   tap        one finger down and up within TAP_MS, moving less than a
 *              twentieth of the pad
 *   swipe      one finger along the pad, at least a fifth of its length
 *              either way, down and up within SWIPE_MS
 *   tap2       two fingers down and up within TAP2_MS, neither moving more
 *              than a twentieth (Glass's own two-finger tap)
 *
 * and a line on stderr for every touch (fingers, time, travel, verdict), so
 * a gesture that was not taken says why.
 *
 * Built on the Glass (gcc from Alpine), installed as /usr/local/bin/glass-tap.
 * The desktop reads it over ssh (scripts/glass-tap.sh): a tap toggles the
 * voice mute (the microphone), a swipe turns the camera stream and the
 * Glass's display on or off together (glass camera-display toggle).
 *
 * The pad speaks multi-touch protocol A (board-notle.c sets type_a = 1 for
 * rmi_f11; the device has no ABS_MT_SLOT): every report lists the fingers
 * down, each as ABS_MT_TRACKING_ID (its index), position and the rest, then
 * SYN_MT_REPORT; SYN_REPORT ends the report. A lifted finger is simply not
 * listed any more (Google's note in rmi_f11.c: "the input device should
 * simply stop sending data"), and when the last one lifts one empty report
 * comes (SYN_MT_REPORT, SYN_REPORT). So a touch begins at the first report
 * with a finger and ends at the first without one.
 *
 * Also read: any device named "glass-touch", the gesture emulator
 * (tools/glass-touch: `glass touch tap` from the desktop), which plays the
 * same protocol; the devices are looked for again every 2 s, so one that
 * appears later is read too.
 *
 *   glass-tap [/dev/input/eventN]     default: every pad and emulator
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define TAP_MS 300
#define TAP2_MS 400   /* two fingers seldom land and lift at one instant */
#define SWIPE_MS 1000
#define IDS 10        /* DEFAULT_MAX_ABS_MT_TRACKING_ID in rmi_f11.c */
#define PADS 4

struct pad {
    int fd, slop, span;
    char path[32];
    /* the report being read */
    int has, id, cx, cy, hx, hy, frame;
    /* the touch */
    int down, maxf, moved, travel;
    long t0;
    int x0[IDS], y0[IDS], lx[IDS], ly[IDS], seen[IDS];
};
static struct pad pads[PADS];
static int npads;
static long last_scan;   /* ms, monotonic: devices looked for every 2 s */

/* Event time in ms; the field names of newer headers (input_event_sec). */
static long ms(const struct input_event *e) { return (long) e->input_event_sec * 1000L + (long) e->input_event_usec / 1000; }

static int add_pad(const char *path) {
    for (int i = 0; i < npads; i++) if (!strcmp(pads[i].path, path)) return 0;
    if (npads == PADS) return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct pad *p = &pads[npads];
    memset(p, 0, sizeof *p);
    p->fd = fd; p->id = -1;
    snprintf(p->path, sizeof p->path, "%s", path);
    struct input_absinfo ax;
    int span = 1000;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 && ax.maximum > ax.minimum) span = ax.maximum - ax.minimum;
    p->slop = span / 20; p->span = span;
    char name[64] = "";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    fprintf(stderr, "glass-tap: %s (%s), a tap is under %d ms (two fingers %d ms) and %d units of travel\n", path, name, TAP_MS, TAP2_MS, p->slop);
    npads++;
    return 0;
}

/* Every device named as the pad or the emulator, not yet open. */
static void scan(void) {
    for (int i = 0; i < 32; i++) {
        char p[32], name[64] = "";
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        close(fd);
        if (!strcmp(name, "sensor00fn11") || !strcmp(name, "glass-touch")) add_pad(p);
    }
}

/* One finger's part of a report ends (SYN_MT_REPORT, or SYN_REPORT after a
 * finger without its own MT sync). */
static void contact_end(struct pad *p, long t) {
    if (!p->has) return;
    if (!p->down) {
        p->down = 1; p->t0 = t; p->maxf = 0; p->moved = 0; p->travel = 0;
        memset(p->seen, 0, sizeof p->seen);
    }
    int id = p->id >= 0 && p->id < IDS ? p->id : (p->frame < IDS ? p->frame : IDS - 1);
    p->frame++;
    if (p->hx && p->hy) {
        if (!p->seen[id]) { p->x0[id] = p->cx; p->y0[id] = p->cy; p->seen[id] = 1; }
        else {
            int d = abs(p->cx - p->x0[id]);
            if (abs(p->cy - p->y0[id]) > d) d = abs(p->cy - p->y0[id]);
            if (d > p->travel) p->travel = d;
            if (d >= p->slop) p->moved = 1;
        }
        p->lx[id] = p->cx; p->ly[id] = p->cy;
    }
    p->has = 0; p->id = -1; p->hx = p->hy = 0;
}

/* How far the finger ended along the pad from where it landed (x, signed);
 * one finger's touch, so the one finger seen. */
static int along(const struct pad *p) {
    for (int i = 0; i < IDS; i++) if (p->seen[i]) return p->lx[i] - p->x0[i];
    return 0;
}

static void event(struct pad *p, const struct input_event *e) {
    if (e->type == EV_ABS && e->code >= ABS_MT_TOUCH_MAJOR && e->code <= ABS_MT_PRESSURE) {
        p->has = 1;
        if (e->code == ABS_MT_TRACKING_ID) p->id = e->value;
        else if (e->code == ABS_MT_POSITION_X) { p->cx = e->value; p->hx = 1; }
        else if (e->code == ABS_MT_POSITION_Y) { p->cy = e->value; p->hy = 1; }
    } else if (e->type == EV_SYN && e->code == SYN_MT_REPORT) {
        contact_end(p, ms(e));
    } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
        contact_end(p, ms(e));
        if (p->frame > 0) {
            if (p->frame > p->maxf) p->maxf = p->frame;
        } else if (p->down) {
            long dt = ms(e) - p->t0;
            int dx = p->maxf == 1 ? along(p) : 0;
            const char *g = NULL, *why = "no gesture";
            if (!p->moved && p->maxf == 1 && dt < TAP_MS) g = "tap";
            else if (!p->moved && p->maxf == 2 && dt < TAP2_MS) g = "tap2";
            else if (p->moved && p->maxf == 1 && dt < SWIPE_MS && abs(dx) >= p->span / 5) g = "swipe";
            else if (p->moved) why = p->maxf == 1 && dt < SWIPE_MS ? "moved, too short for a swipe" : "moved, no gesture";
            else why = dt >= (p->maxf == 2 ? TAP2_MS : TAP_MS) ? "too long, no tap" : "no gesture";
            if (g) { printf("%s\n", g); fflush(stdout); }
            fprintf(stderr, "glass-tap: touch on %s: %d finger(s), %ld ms, %d units of travel, %+d along: %s\n", p->path, p->maxf, dt,
                    p->travel, dx, g ? g : why);
            p->down = 0;
        }
        p->frame = 0;
    }
}

int main(int argc, char **argv) {
    int fixed = argc > 1;
    if (fixed) { if (add_pad(argv[1])) { perror(argv[1]); return 1; } }
    else scan();
    if (!npads) { fprintf(stderr, "glass-tap: no touchpad (sensor00fn11)\n"); return 1; }
    for (;;) {
        struct pollfd pf[PADS];
        for (int i = 0; i < npads; i++) { pf[i].fd = pads[i].fd; pf[i].events = POLLIN; pf[i].revents = 0; }
        int r = poll(pf, (nfds_t) npads, 2000);
        if (r < 0 && errno != EINTR) return 1;
        for (int i = 0; i < npads; i++) {
            if (!(pf[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            struct input_event e;
            ssize_t n;
            while ((n = read(pads[i].fd, &e, sizeof e)) == (ssize_t) sizeof e) event(&pads[i], &e);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                /* gone (the emulator ended): forget it */
                fprintf(stderr, "glass-tap: %s gone\n", pads[i].path);
                close(pads[i].fd);
                pads[i] = pads[--npads];
                pf[i] = pf[npads];
                i--;
            }
        }
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long now = (long) ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        if (!fixed && now - last_scan >= 2000) { scan(); last_scan = now; }
        if (!npads) { fprintf(stderr, "glass-tap: no touchpad left\n"); return 1; }
    }
}
