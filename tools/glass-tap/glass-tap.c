/* glass-tap: the Glass's touchpad (the Synaptics RMI4 pad on the side,
 * "sensor00fn11") reported as gestures, one line per gesture on stdout:
 *
 *   tap        one finger down and up within TAP_MS, moving less than a
 *              twentieth of the pad
 *
 * Built on the Glass (gcc from Alpine), installed as /usr/local/bin/glass-tap.
 * The desktop reads it over ssh (scripts/glass-tap.sh) and turns a tap into
 * the voice mute toggle, the same as the number pad's period.
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
    fprintf(stderr, "glass-tap: %s, a tap is under %d ms and %d units of travel\n", path, TAP_MS, slop);

    int down = 0, fingers = 0, maxfingers = 0, x0 = 0, y0 = 0, x = 0, y = 0, have0 = 0;
    long t0 = 0;
    struct input_event e;
    while (read(fd, &e, sizeof e) == sizeof e) {
        if (e.type == EV_ABS) {
            if (e.code == ABS_MT_TRACKING_ID) {
                if (e.value >= 0) {
                    fingers++;
                    if (fingers > maxfingers) maxfingers = fingers;
                    if (!down) { down = 1; t0 = ms(&e); have0 = 0; maxfingers = 1; }
                } else if (fingers > 0) {
                    fingers--;
                }
            } else if (e.code == ABS_MT_POSITION_X) {
                x = e.value;
                if (!have0) x0 = x;
            } else if (e.code == ABS_MT_POSITION_Y) {
                y = e.value;
                if (!have0) { y0 = y; have0 = 1; }
            }
        } else if (e.type == EV_SYN && e.code == SYN_REPORT && down && fingers == 0) {
            long dt = ms(&e) - t0;
            int dx = abs(x - x0), dy = abs(y - y0);
            if (dt < TAP_MS && dx < slop && dy < slop && maxfingers == 1) {
                printf("tap\n");
                fflush(stdout);
            }
            down = 0; maxfingers = 0;
        }
    }
    return 0;
}
