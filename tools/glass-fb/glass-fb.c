/* glass-fb: raw BGRA frames from stdin onto the Glass's display, each shown
 * at the next vertical sync, tear-free: the framebuffer (omapfb, 640x360,
 * 32 bits) holds three pages; a frame is written into a page that is not
 * on screen, then the display controller is pointed at it (pan) and the
 * sync waited for. The display runs at 312 fields a second, so the wait is
 * at most 3 ms. Compiled on the Glass (gcc from Alpine), installed as
 * /usr/local/bin/glass-fb; scripts/glass-view.sh pipes ffmpeg's rawvideo
 * into it.
 *
 *   ffmpeg ... -f rawvideo -pix_fmt bgra - | glass-fb [/dev/fb0]
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* omapfb's own vsync wait (drivers/video/omap2/omapfb, 'O' 24); the generic
 * FBIO_WAITFORVSYNC ('F' 0x20) is tried first. */
#define OMAPFB_WAITFORVSYNC _IOW('O', 24, int)
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif

static int read_full(int fd, unsigned char *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t) r;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    int fb = open(dev, O_RDWR);
    if (fb < 0) { perror(dev); return 1; }
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) { perror("fb info"); return 1; }
    size_t page = (size_t) fix.line_length * var.yres;
    int pages = (int) (fix.smem_len / page);
    if (pages < 2) { fprintf(stderr, "glass-fb: %u bytes hold %d page(s) of %zu: no page to flip to\n", fix.smem_len, pages, page); return 1; }
    if (pages > 3) pages = 3;
    size_t frame = (size_t) var.xres * var.yres * (var.bits_per_pixel / 8);
    unsigned char *mem = mmap(NULL, page * pages, PROT_WRITE, MAP_SHARED, fb, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    if (var.yres_virtual < var.yres * pages) {
        var.yres_virtual = var.yres * pages;
        if (ioctl(fb, FBIOPUT_VSCREENINFO, &var)) perror("glass-fb: virtual height");
    }
    fprintf(stderr, "glass-fb: %ux%u, %u bits, %d pages of %zu bytes, line %u\n", var.xres, var.yres, var.bits_per_pixel, pages, page, fix.line_length);
    unsigned char *buf = malloc(frame);
    if (!buf) return 1;
    int shown = 0, use_omap = 0;
    unsigned vs = 0;
    for (unsigned long n = 0;; n++) {
        if (read_full(0, buf, frame)) break;
        int next = (shown + 1) % pages;
        unsigned char *dst = mem + page * next;
        if (fix.line_length == var.xres * (var.bits_per_pixel / 8)) {
            memcpy(dst, buf, frame);
        } else {
            size_t row = (size_t) var.xres * (var.bits_per_pixel / 8);
            for (unsigned y = 0; y < var.yres; y++) memcpy(dst + (size_t) y * fix.line_length, buf + y * row, row);
        }
        var.yoffset = var.yres * next;
        var.xoffset = 0;
        if (ioctl(fb, FBIOPAN_DISPLAY, &var)) { if (n == 0) perror("glass-fb: pan"); }
        if (!use_omap) {
            if (ioctl(fb, FBIO_WAITFORVSYNC, &vs)) { use_omap = 1; }
        }
        if (use_omap) {
            int z = 0;
            if (ioctl(fb, OMAPFB_WAITFORVSYNC, &z) && n == 0) perror("glass-fb: no vsync wait");
        }
        shown = next;
    }
    munmap(mem, page * pages);
    close(fb);
    return 0;
}
