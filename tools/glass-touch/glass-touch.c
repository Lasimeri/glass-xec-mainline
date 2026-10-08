/* glass-touch: a gesture emulator for the Glass's touchpad, to try what
 * reads the pad (glass-tap) without a finger on it. A virtual input device,
 * "glass-touch", made through uinput with the real pad's axes and ranges
 * (read from the device named sensor00fn11), plays each gesture asked for
 * in the pad's own protocol, as rmi_f11 in Google's kernel sends it
 * (multi-touch protocol A, board-notle.c type_a = 1): every report lists
 * the fingers down (ABS_MT_TRACKING_ID as the finger's index, tool type,
 * pressure, widths, orientation, position, SYN_MT_REPORT) and ends with
 * SYN_REPORT; a lift is the fingers no longer listed, the last one's an
 * empty report. glass-tap reads devices of this name as it reads the pad.
 *
 *   glass-touch GESTURE...       tap | tap2 | swipe | hold, played in order
 *
 *   tap     one finger, 96 ms, still            glass-tap: "tap"
 *   tap2    two fingers, 120 ms, still          glass-tap: "tap2"
 *   swipe   one finger across 40% of the pad    nothing (it moved)
 *   hold    one finger, 720 ms                  nothing (too long)
 *
 * Reports come every REPORT_MS. The device is made 2.5 s before the first
 * gesture (glass-tap looks for new devices every 2 s) and removed at the
 * end. From the desktop: scripts/glass touch GESTURE...
 *
 * Built on the Glass (gcc from Alpine), installed as /usr/local/bin/glass-touch.
 */
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define REPORT_MS 12

static const int axes[] = { ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR, ABS_MT_ORIENTATION, ABS_MT_POSITION_X,
                            ABS_MT_POSITION_Y, ABS_MT_TOOL_TYPE, ABS_MT_TRACKING_ID, ABS_MT_PRESSURE };
#define NAXES (int) (sizeof axes / sizeof axes[0])
static struct input_absinfo info[ABS_CNT];
static int ufd;

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void emit(int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short) type; e.code = (unsigned short) code; e.value = value;
    if (write(ufd, &e, sizeof e) != (ssize_t) sizeof e) { perror("glass-touch: write"); exit(1); }
}

/* One report with n fingers at x[i], y[i], as rmi_f11 sends it. */
static void report(int n, const int *x, const int *y) {
    int z = (info[ABS_MT_PRESSURE].minimum + info[ABS_MT_PRESSURE].maximum) / 2;
    int w = info[ABS_MT_TOUCH_MAJOR].minimum + (info[ABS_MT_TOUCH_MAJOR].maximum - info[ABS_MT_TOUCH_MAJOR].minimum) / 4;
    for (int i = 0; i < n; i++) {
        emit(EV_ABS, ABS_MT_TRACKING_ID, i);
        emit(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
        emit(EV_ABS, ABS_MT_PRESSURE, z);
        emit(EV_ABS, ABS_MT_TOUCH_MAJOR, w);
        emit(EV_ABS, ABS_MT_TOUCH_MINOR, w);
        emit(EV_ABS, ABS_MT_ORIENTATION, 0);
        emit(EV_ABS, ABS_MT_POSITION_X, x[i]);
        emit(EV_ABS, ABS_MT_POSITION_Y, y[i]);
        emit(EV_SYN, SYN_MT_REPORT, 0);
    }
    if (n == 0) emit(EV_SYN, SYN_MT_REPORT, 0);   /* the empty report of the last lift */
    emit(EV_SYN, SYN_REPORT, 0);
    sleep_ms(REPORT_MS);
}

/* Fingers held still for `reports` reports, then lifted; a little jitter. */
static void still(int fingers, int reports) {
    int cx = (info[ABS_MT_POSITION_X].minimum + info[ABS_MT_POSITION_X].maximum) / 2;
    int cy = (info[ABS_MT_POSITION_Y].minimum + info[ABS_MT_POSITION_Y].maximum) / 2;
    int dx = (info[ABS_MT_POSITION_X].maximum - info[ABS_MT_POSITION_X].minimum) / 8;
    for (int r = 0; r < reports; r++) {
        int j = r % 2;
        int x[2] = { cx - (fingers > 1 ? dx : 0) + j, cx + dx + j }, y[2] = { cy + j, cy + j };
        report(fingers, x, y);
    }
    report(0, NULL, NULL);
}

static void swipe(void) {
    int x0 = info[ABS_MT_POSITION_X].minimum, span = info[ABS_MT_POSITION_X].maximum - x0;
    int cy = (info[ABS_MT_POSITION_Y].minimum + info[ABS_MT_POSITION_Y].maximum) / 2;
    for (int r = 0; r < 20; r++) {
        int x = x0 + span * 3 / 10 + span * 4 / 10 * r / 19, y = cy;
        report(1, &x, &y);
    }
    report(0, NULL, NULL);
}

/* The real pad's ranges, so the emulator reads as the pad does. */
static void ranges(void) {
    for (int i = 0; i < NAXES; i++) { info[axes[i]].minimum = 0; info[axes[i]].maximum = 255; }
    info[ABS_MT_POSITION_X].maximum = 1000; info[ABS_MT_POSITION_Y].maximum = 1000;
    info[ABS_MT_TRACKING_ID].maximum = 9;
    for (int n = 0; n < 32; n++) {
        char p[32], name[64] = "";
        snprintf(p, sizeof p, "/dev/input/event%d", n);
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (!strcmp(name, "sensor00fn11")) {
            for (int i = 0; i < NAXES; i++) {
                struct input_absinfo a;
                if (ioctl(fd, EVIOCGABS(axes[i]), &a) == 0 && a.maximum > a.minimum) info[axes[i]] = a;
            }
            close(fd);
            fprintf(stderr, "glass-touch: the pad's ranges from %s: x %d..%d, y %d..%d\n", p,
                    info[ABS_MT_POSITION_X].minimum, info[ABS_MT_POSITION_X].maximum,
                    info[ABS_MT_POSITION_Y].minimum, info[ABS_MT_POSITION_Y].maximum);
            return;
        }
        close(fd);
    }
    fprintf(stderr, "glass-touch: no sensor00fn11: plain ranges (x and y 0..1000)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "glass-touch GESTURE...   (tap, tap2, swipe, hold)\n"); return 2; }
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "tap") && strcmp(argv[i], "tap2") && strcmp(argv[i], "swipe") && strcmp(argv[i], "hold")) {
            fprintf(stderr, "glass-touch: no gesture \"%s\" (tap, tap2, swipe, hold)\n", argv[i]);
            return 2;
        }
    ranges();
    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) { perror("glass-touch: /dev/uinput"); return 1; }
    ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    ioctl(ufd, UI_SET_EVBIT, EV_ABS);
    struct uinput_user_dev d;
    memset(&d, 0, sizeof d);
    snprintf(d.name, sizeof d.name, "glass-touch");
    d.id.bustype = BUS_VIRTUAL; d.id.vendor = 0x0001; d.id.product = 0x0001; d.id.version = 1;
    for (int i = 0; i < NAXES; i++) {
        ioctl(ufd, UI_SET_ABSBIT, axes[i]);
        d.absmin[axes[i]] = info[axes[i]].minimum;
        d.absmax[axes[i]] = info[axes[i]].maximum;
    }
    if (write(ufd, &d, sizeof d) != (ssize_t) sizeof d || ioctl(ufd, UI_DEV_CREATE) < 0) { perror("glass-touch: uinput"); return 1; }
    sleep_ms(2500);
    for (int i = 1; i < argc; i++) {
        fprintf(stderr, "glass-touch: %s\n", argv[i]);
        if (!strcmp(argv[i], "tap")) still(1, 8);
        else if (!strcmp(argv[i], "tap2")) still(2, 10);
        else if (!strcmp(argv[i], "swipe")) swipe();
        else still(1, 60);
        sleep_ms(700);
    }
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    return 0;
}
