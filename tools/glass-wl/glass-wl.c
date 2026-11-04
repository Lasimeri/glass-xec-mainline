/* glass-wl: the Wi-Fi firmware's roaming trigger and power-save mode, through
 * the bcmdhd driver's private ioctl (the kernel's own wlioctl.h numbers). This
 * rootfs has no `wl`, and `iw` does not see either: it said power save off while
 * the firmware read PM 2 (2026-11-04).
 *
 *   glass-wl                     prints: PM, roam trigger in dBm
 *   glass-wl roam-trigger DBM    below this signal the firmware looks for another
 *                                access point, every 10 s, off its channel
 *   glass-wl pm 0|1|2            off, on, fast
 *
 * Why the trigger is set (glass-wifi, on every join): the firmware's -75 dBm is
 * where this Glass often is, with one access point in reach. On the
 * live stream, 2 passes of 35 s, interleaved: at -75 the longest gap in frames
 * averaged 823 ms a 5-second report, 4 of 14 over a second; at -90 383 ms, 2 of 14.
 *
 * Built on the Glass (gcc from Alpine), installed as /usr/local/bin/glass-wl.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define WLC_GET_ROAM_TRIGGER 54
#define WLC_SET_ROAM_TRIGGER 55
#define WLC_GET_PM 85
#define WLC_SET_PM 86
#define WLC_BAND_ALL 3

struct wl_ioctl { unsigned cmd; void *buf; unsigned len; unsigned char set; unsigned used, needed; };

static int wl(int fd, unsigned cmd, void *buf, unsigned len, int set) {
    struct wl_ioctl w = { .cmd = cmd, .buf = buf, .len = len, .set = (unsigned char) set };
    struct ifreq r;
    memset(&r, 0, sizeof r);
    strcpy(r.ifr_name, "wlan0");
    r.ifr_data = (char *) &w;
    return ioctl(fd, SIOCDEVPRIVATE, &r);
}

int main(int argc, char **argv) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("glass-wl: socket"); return 1; }
    int rc = 0;
    if (argc == 3 && !strcmp(argv[1], "roam-trigger")) {
        int t[2] = { atoi(argv[2]), WLC_BAND_ALL };
        rc = wl(fd, WLC_SET_ROAM_TRIGGER, t, sizeof t, 1);
    } else if (argc == 3 && !strcmp(argv[1], "pm")) {
        int m = atoi(argv[2]);
        rc = wl(fd, WLC_SET_PM, &m, sizeof m, 1);
    } else if (argc != 1) {
        fprintf(stderr, "glass-wl [roam-trigger DBM | pm 0|1|2]\n");
        return 2;
    }
    if (rc) { perror("glass-wl"); return 1; }
    int m = -1, t[2] = { 0, 0 };
    if (wl(fd, WLC_GET_PM, &m, sizeof m, 0) || wl(fd, WLC_GET_ROAM_TRIGGER, t, sizeof t, 0)) { perror("glass-wl"); return 1; }
    printf("PM %d, roam trigger %d dBm\n", m, t[0]);
    close(fd);
    return 0;
}
