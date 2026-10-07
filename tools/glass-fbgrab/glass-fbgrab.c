/* glass-fbgrab: what the Glass's display shows, to stdout, as rows that
 * changed (the other way from glass-fb: the Glass's screen on the desktop).
 *
 *   glass-fbgrab [-i MS] [/dev/fb0]        (glass-display.service: the window)
 *
 * The graphics layer's picture is read where the display controller scans
 * it (DISPC GFX_BA0 through /dev/mem: glass-console's page 0, or a page
 * glass-fb flipped to), else at the framebuffer's pan offset. Every MS
 * milliseconds (250) each row is compared with the one sent last; the runs
 * of changed rows go out as one record each:
 *
 *   "GFB1" u16 width, u16 height, u16 first row, u16 rows, then rows x
 *   width x 4 bytes (BGRA, the framebuffer's order, no padding)
 *
 * The first look sends every row. A screen that does not change sends
 * nothing (glass-console's idle shell: the cursor's blink, a few rows), so
 * the radio and the CPU stay quiet while the battery charges. The video
 * overlay (glass-fb -y, the desktop's stream) is not read: what it shows
 * is the desktop itself. Ends when stdout closes (the session ended).
 *
 * Compiled on the Glass (gcc from Alpine), installed as
 * /usr/local/bin/glass-fbgrab by scripts/on-glass/update.sh.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DISPC_BASE 0x48041000u
#define DISPC_GFX_BA0 0x080

static int write_all(const void *p, size_t n) {
    const unsigned char *b = p;
    while (n) {
        ssize_t w = write(1, b, n);
        if (w <= 0) return -1;
        b += w;
        n -= (size_t) w;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *dev = "/dev/fb0";
    int ms = 250;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) ms = atoi(argv[++i]);
        else dev = argv[i];
    }
    if (ms < 20) ms = 20;
    int fb = open(dev, O_RDONLY);
    if (fb < 0) { perror(dev); return 1; }
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) { perror("glass-fbgrab: fb info"); return 1; }
    if (var.bits_per_pixel != 32) { fprintf(stderr, "glass-fbgrab: %u bits a pixel: only 32 is read\n", var.bits_per_pixel); return 1; }
    unsigned w = var.xres, h = var.yres;
    size_t line = fix.line_length, row = (size_t) w * 4, page = line * h;
    unsigned char *mem = mmap(NULL, fix.smem_len, PROT_READ, MAP_SHARED, fb, 0);
    if (mem == MAP_FAILED) { perror("glass-fbgrab: mmap"); return 1; }
    volatile uint32_t *dispc = NULL;
    int memfd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (memfd >= 0) {
        void *p = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, memfd, DISPC_BASE);
        if (p != MAP_FAILED) dispc = p;
    }
    unsigned char *prev = calloc(h, row), *cur = malloc((size_t) h * row);
    if (!prev || !cur) { fprintf(stderr, "glass-fbgrab: out of memory\n"); return 1; }
    int first = 1;
    for (;;) {
        /* The page on screen: where the controller scans, if it is in this
         * framebuffer; else the pan offset. */
        size_t off = (size_t) var.yoffset * line;
        if (dispc) {
            uint32_t ba = dispc[DISPC_GFX_BA0 / 4];
            if (ba >= fix.smem_start && ba + page <= fix.smem_start + fix.smem_len) off = ba - fix.smem_start;
        } else {
            ioctl(fb, FBIOGET_VSCREENINFO, &var);
            off = (size_t) var.yoffset * line;
        }
        for (unsigned y = 0; y < h; y++) memcpy(cur + (size_t) y * row, mem + off + (size_t) y * line, row);
        unsigned y = 0;
        while (y < h) {
            if (!first && !memcmp(cur + (size_t) y * row, prev + (size_t) y * row, row)) { y++; continue; }
            unsigned y0 = y;
            while (y < h && (first || memcmp(cur + (size_t) y * row, prev + (size_t) y * row, row))) y++;
            uint16_t hdr[4] = { (uint16_t) w, (uint16_t) h, (uint16_t) y0, (uint16_t) (y - y0) };
            if (write_all("GFB1", 4) || write_all(hdr, sizeof hdr) || write_all(cur + (size_t) y0 * row, (size_t) (y - y0) * row)) return 0;
        }
        unsigned char *t = prev; prev = cur; cur = t;
        first = 0;
        struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
}
