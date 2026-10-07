/* glass-fbview: the Glass's screen in a window on this desktop, from
 * glass-fbgrab's records on stdin ("GFB1", u16 width, height, first row,
 * rows, then the rows' BGRA bytes): the whole picture is kept here and,
 * after each record, sent to mpv as one raw frame (shown as it comes,
 * untimed). mpv is started at the first record, which gives the size.
 *
 *   glass ssh glass-fbgrab | glass-fbview [TITLE]   (glass-display.service)
 *
 * Ends when stdin ends or the window is closed (its pipe breaks).
 * Built on the desktop into out/ by scripts/glass setup-desktop, which also
 * writes glass-display.service (the pipe above, restarted 5 s after an exit).
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_full(void *p, size_t n) {
    return fread(p, 1, n, stdin) == n ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *title = argc > 1 ? argv[1] : "Glass display";
    signal(SIGPIPE, SIG_IGN);
    unsigned w = 0, h = 0;
    unsigned char *pic = NULL;
    FILE *mpv = NULL;
    for (;;) {
        char magic[4];
        uint16_t hdr[4];
        if (read_full(magic, 4) || memcmp(magic, "GFB1", 4) || read_full(hdr, sizeof hdr)) break;
        if (!pic) {
            w = hdr[0]; h = hdr[1];
            if (!w || !h || w > 4096 || h > 4096) { fprintf(stderr, "glass-fbview: a %ux%u screen?\n", w, h); return 1; }
            pic = calloc((size_t) w * h, 4);
            char cmd[1024];
            snprintf(cmd, sizeof cmd,
                     "exec mpv --really-quiet --title='%s' --untimed --no-cache --demuxer=rawvideo "
                     "--demuxer-rawvideo-w=%u --demuxer-rawvideo-h=%u --demuxer-rawvideo-mp-format=bgra "
                     "--demuxer-rawvideo-fps=30 --window-scale=2 --scale=nearest --keep-open=yes -",
                     title, w, h);
            mpv = popen(cmd, "w");
            if (!pic || !mpv) { fprintf(stderr, "glass-fbview: could not start mpv\n"); return 1; }
        }
        if (hdr[0] != w || hdr[1] != h || (unsigned) hdr[2] + hdr[3] > h) { fprintf(stderr, "glass-fbview: a record outside the screen\n"); return 1; }
        if (read_full(pic + (size_t) hdr[2] * w * 4, (size_t) hdr[3] * w * 4)) break;
        if (fwrite(pic, (size_t) w * 4, h, mpv) != h || fflush(mpv)) return 0;
    }
    if (mpv) pclose(mpv);
    return 0;
}
