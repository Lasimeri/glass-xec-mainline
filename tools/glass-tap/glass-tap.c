/* glass-tap: the Glass's touchpad (the Synaptics RMI4 pad on the side,
 * "sensor00fn11") reported as gestures, one line per gesture on stdout:
 *
 *   tap        one finger down and up within TAP_MS, moving less than a
 *              twentieth of the pad
 *   tap2       two fingers down and up within TAP2_MS, neither moving more
 *              than that (Glass's own two-finger tap)
 *
 * Built on the Glass (gcc from Alpine), installed as /usr/local/bin/glass-tap.
 * The desktop reads it over ssh (scripts/glass-tap.sh): a tap turns the
 * camera window and the Glass's display on or off together (glass
 * camera-display toggle), a two-finger tap toggles the voice mute, the same
 * as the number pad's period.
 *
 * The pad reports by multi-touch protocol B (rmi_f11.c in Google's kernel:
 * input_mt_slot and input_mt_report_slot_state when type_a is off): a slot
 * per finger, a tracking id at each touch and -1 at its lift, so each
 * finger's travel is measured from its own first position.
 *
 *   glass-tap [/dev/input/eventN]     default: the device named sensor00fn11
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define TAP_MS 300
#define TAP2_MS 400   /* two fingers seldom land and lift at one instant */
#define SLOTS 10      /* DEFAULT_MAX_ABS_MT_TRACKING_ID in rmi_f11.c */

static int find_pad(char *path, size_t n) {
    for (int i = 0; i < 32; i++) {
        char p[64], name[128] = "";
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        close(fd);
        if (!strcmp(name, "sensor00fn11")) { snprintf(path, n, "%s", p); return 0; }
    }
    return -1;
}

/* Event time in ms; the field names of newer headers (input_event_sec). */
static long ms(const struct input_event *e) { return (long) e->input_event_sec * 1000L + (long) e->input_event_usec / 1000; }

int main(int argc, char **argv) {
    char path[64];
    if (argc > 1) snprintf(path, sizeof path, "%s", argv[1]);
    else if (find_pad(path, sizeof path)) { fprintf(stderr, "glass-tap: no touchpad (sensor00fn11)\n"); return 1; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return 1; }
    struct input_absinfo ax;
    int span = 1000;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 && ax.maximum > ax.minimum) span = ax.maximum - ax.minimum;
    int slop = span / 20;
    int slot = 0;
    if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &ax) == 0 && ax.value >= 0 && ax.value < SLOTS) slot = ax.value;
    fprintf(stderr, "glass-tap: %s, a tap is under %d ms (two fingers %d ms) and %d units of travel\n", path, TAP_MS, TAP2_MS, slop);

    /* One touch: from the first finger down to the last finger up. */
    int down = 0, fingers = 0, maxfingers = 0, moved = 0;
    int x0[SLOTS], y0[SLOTS], hx[SLOTS] = { 0 }, hy[SLOTS] = { 0 };
    long t0 = 0;
    struct input_event e;
    while (read(fd, &e, sizeof e) == sizeof e) {
        if (e.type == EV_ABS) {
            if (e.code == ABS_MT_SLOT) {
                slot = e.value >= 0 && e.value < SLOTS ? e.value : SLOTS - 1;
            } else if (e.code == ABS_MT_TRACKING_ID) {
                if (e.value >= 0) {
                    if (!down) { down = 1; t0 = ms(&e); maxfingers = 0; moved = 0; }
                    if (++fingers > maxfingers) maxfingers = fingers;
                    hx[slot] = hy[slot] = 0;
                } else if (fingers > 0) {
                    fingers--;
                }
            } else if (e.code == ABS_MT_POSITION_X) {
                if (!hx[slot]) { x0[slot] = e.value; hx[slot] = 1; }
                else if (abs(e.value - x0[slot]) >= slop) moved = 1;
            } else if (e.code == ABS_MT_POSITION_Y) {
                if (!hy[slot]) { y0[slot] = e.value; hy[slot] = 1; }
                else if (abs(e.value - y0[slot]) >= slop) moved = 1;
            }
        } else if (e.type == EV_SYN && e.code == SYN_REPORT && down && fingers == 0) {
            long dt = ms(&e) - t0;
            if (!moved && maxfingers == 1 && dt < TAP_MS) printf("tap\n");
            else if (!moved && maxfingers == 2 && dt < TAP2_MS) printf("tap2\n");
            fflush(stdout);
            down = 0;
        }
    }
    return 0;
}
