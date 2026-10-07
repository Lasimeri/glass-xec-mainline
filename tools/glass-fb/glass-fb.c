/* glass-fb: raw BGRA frames from stdin onto the Glass's display, each shown
 * on a vertical sync, tear-free, and on a fixed schedule when asked.
 *
 *   ffmpeg ... -f rawvideo -pix_fmt bgra - | glass-fb [-r FPS] [-b N] [/dev/fb0]
 *   ffmpeg ... -f rawvideo -pix_fmt yuv420p - | glass-fb -y [-r FPS] [-b N]
 *       (-y: I420 in, packed to YUYV for the video overlay, which converts to RGB)
 *
 * The framebuffer (omapfb, 640x360, 32 bits) holds three pages: a frame is
 * written into a page not on screen, the display controller is pointed at
 * it (pan) and the sync waited for (312 fields a second: at most 3 ms).
 *
 * -r FPS paces presentation: frames are shown at exactly 1/FPS intervals
 * on the Glass's own clock, N frames (-b, default 1) of cushion absorbing
 * the link's jitter, so motion is even however the packets arrived. A
 * frame that is not there at its slot is shown as soon as it comes and the
 * schedule re-anchors; frames piling up beyond the cushion are dropped,
 * the newest kept, so latency never grows. Without -r each frame is shown
 * as it arrives. Every 5 s a line on stderr says what happened (frames,
 * drops, late slots, the longest gap between two frames shown).
 *
 * Compiled on the Glass (gcc from Alpine), installed as
 * /usr/local/bin/glass-fb; scripts/glass-view.sh pipes ffmpeg into it.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/omapfb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef OMAPFB_WAITFORVSYNC
#define OMAPFB_WAITFORVSYNC _IO('O', 57)   /* linux/omapfb.h: OMAP_IO(57) */
#endif
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif

#define RING 4

/* OMAP4 display controller (TRM: DISPC at 0x48041000), as the initramfs's
 * display handoff already uses them. */
#define DISPC_BASE 0x48041000u
#define DISPC_GFX_BA0 0x080
#define DISPC_GFX_BA1 0x084
#define DISPC_VID1_BA0 0x0bc
#define DISPC_VID1_BA1 0x0c0
static unsigned ba0 = DISPC_GFX_BA0, ba1 = DISPC_GFX_BA1;
#define DISPC_CONTROL2 0x238
#define DISPC_GO_LCD2 0x20
static volatile uint32_t *dispc;
static unsigned long smem;

static int fb, pages, use_omap = -1;
static unsigned char *mem;
static size_t page, frame, row, line;
static struct fb_var_screeninfo var;
static int shown = 0;

static unsigned char *ring[RING];
static int head = 0, tail = 0, count = 0, eof = 0;
static unsigned long dropped = 0;
static unsigned char *prev;   /* the last frame shown, to count repeats */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int read_full(int fd, unsigned char *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t) r;
    }
    return 0;
}

/* Reads frames into the ring; when it is full the oldest frame gives way. */
static void *reader(void *arg) {
    int cushion = *(int *) arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        int slot = head;
        pthread_mutex_unlock(&mu);
        if (read_full(0, ring[slot], frame)) break;
        pthread_mutex_lock(&mu);
        if (count == cushion + 1) {   /* behind: drop the oldest, keep this one */
            tail = (tail + 1) % RING;
            count--;
            dropped++;
        }
        head = (head + 1) % RING;
        count++;
        pthread_cond_signal(&cv);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    eof = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* -y input is I420 (the decoder's own planes, no conversion in ffmpeg):
 * packed here into YUYV, two pixels a 32-bit word, a line at a time;
 * swscale's yuv420p to yuyv422 has no fast path on this build and cost
 * twice the decode (measured 2026-10-07: ffmpeg 83% against 40%). */
static int i420;
static void pack_yuyv(unsigned char *dst, const unsigned char *src) {
    unsigned w = var.xres, h = var.yres;
    const unsigned char *Y = src, *U = src + w * h, *V = U + (w / 2) * (h / 2);
    for (unsigned yy = 0; yy < h; yy++) {
        const unsigned char *y = Y + (size_t) yy * w, *u = U + (size_t) (yy / 2) * (w / 2), *vv = V + (size_t) (yy / 2) * (w / 2);
        uint32_t *o = (uint32_t *) (dst + (size_t) yy * line);
        for (unsigned x = 0; x < w / 2; x++)
            o[x] = (uint32_t) y[2 * x] | ((uint32_t) u[x] << 8) | ((uint32_t) y[2 * x + 1] << 16) | ((uint32_t) vv[x] << 24);
    }
}

static void show(const unsigned char *buf) {
    int next = (shown + 1) % pages;
    unsigned char *dst = mem + page * next;
    if (i420) {
        pack_yuyv(dst, buf);
    } else if (line == row) {
        memcpy(dst, buf, frame);
    } else {
        for (unsigned y = 0; y < var.yres; y++) memcpy(dst + (size_t) y * line, buf + y * row, row);
    }
    var.yoffset = var.yres * next;
    var.xoffset = 0;
    ioctl(fb, FBIOPAN_DISPLAY, &var);
    if (dispc) {
        /* The pan above is accepted and never reaches the hardware on the
         * stock kernel (omapfb under dsscomp; seen 2026-10-07: GFX_BA0 stayed
         * at the first page, so one frame in three was ever seen). Point the
         * graphics layer at the page directly and set LCD2's GO bit: the
         * controller takes the new address at its next vertical sync and
         * clears GO. */
        uint32_t addr = (uint32_t) (smem + page * next);
        dispc[ba0 / 4] = addr;
        dispc[ba1 / 4] = addr;
        dispc[DISPC_CONTROL2 / 4] |= DISPC_GO_LCD2;
        for (int i = 0; i < 40 && (dispc[DISPC_CONTROL2 / 4] & DISPC_GO_LCD2); i++) {
            struct timespec ts = { 0, 250000 };
            nanosleep(&ts, NULL);
        }
        shown = next;
        return;
    }
    if (use_omap != 1) {
        unsigned vs = 0;
        if (ioctl(fb, FBIO_WAITFORVSYNC, &vs) == 0) use_omap = 0;
        else use_omap = 1;
    }
    if (use_omap == 1) {
        int z = 0;
        ioctl(fb, OMAPFB_WAITFORVSYNC, &z);
    }
    shown = next;
}

/* -y: the frames are YUYV (4:2:2, 2 bytes a pixel) on the first video
 * overlay (/dev/fb1, DISPC VID1), full screen above the console: the
 * display controller converts to RGB, the CPU only copies half the bytes.
 * fb1 has no memory at boot: three pages are given to it, its format set,
 * and the plane enabled at 0,0 at the panel's size. */
static int setup_overlay(void) {
    struct fb_var_screeninfo v0;
    int f0 = open("/dev/fb0", O_RDONLY);
    if (f0 < 0 || ioctl(f0, FBIOGET_VSCREENINFO, &v0)) { perror("glass-fb: /dev/fb0"); return 1; }
    close(f0);
    unsigned w = v0.xres, h = v0.yres;
    FILE *s = fopen("/sys/class/graphics/fb1/size", "w");
    if (!s) { perror("glass-fb: fb1 size"); return 1; }
    fprintf(s, "%u\n", w * h * 2 * 3); fclose(s);
    int f1 = open("/dev/fb1", O_RDWR);
    if (f1 < 0) { perror("glass-fb: /dev/fb1"); return 1; }
    struct omapfb_plane_info pi;
    if (ioctl(f1, OMAPFB_QUERY_PLANE, &pi)) { perror("glass-fb: query plane"); return 1; }
    pi.enabled = 0; ioctl(f1, OMAPFB_SETUP_PLANE, &pi);
    struct fb_var_screeninfo v;
    ioctl(f1, FBIOGET_VSCREENINFO, &v);
    v.xres = w; v.yres = h; v.xres_virtual = w; v.yres_virtual = h * 3; v.xoffset = 0; v.yoffset = 0;
    v.bits_per_pixel = 16; v.nonstd = OMAPFB_COLOR_YUY422; v.activate = FB_ACTIVATE_NOW;
    if (ioctl(f1, FBIOPUT_VSCREENINFO, &v)) { perror("glass-fb: fb1 format"); return 1; }
    pi.pos_x = 0; pi.pos_y = 0; pi.out_width = w; pi.out_height = h; pi.enabled = 1;
    if (ioctl(f1, OMAPFB_SETUP_PLANE, &pi)) { perror("glass-fb: enable plane"); return 1; }
    close(f1);
    return 0;
}

int main(int argc, char **argv) {
    const char *dev = "/dev/fb0";
    int fps = 0, cushion = 1, yuv = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) cushion = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-y")) { yuv = 1; dev = "/dev/fb1"; }
        else dev = argv[i];
    }
    if (cushion < 1) cushion = 1;
    if (cushion > RING - 2) cushion = RING - 2;
    if (yuv && setup_overlay()) return 1;
    fb = open(dev, O_RDWR);
    if (fb < 0) { perror(dev); return 1; }
    struct fb_fix_screeninfo fix;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) { perror("fb info"); return 1; }
    line = fix.line_length;
    page = line * var.yres;
    pages = (int) (fix.smem_len / page);
    if (pages < 2) { fprintf(stderr, "glass-fb: %u bytes hold %d page(s) of %zu: nothing to flip to\n", fix.smem_len, pages, page); return 1; }
    if (pages > 3) pages = 3;
    row = (size_t) var.xres * (var.bits_per_pixel / 8);
    frame = row * var.yres;
    if (yuv) { i420 = 1; frame = (size_t) var.xres * var.yres * 3 / 2; }
    mem = mmap(NULL, page * pages, PROT_WRITE, MAP_SHARED, fb, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    if (var.yres_virtual < var.yres * pages) {
        var.yres_virtual = var.yres * pages;
        if (ioctl(fb, FBIOPUT_VSCREENINFO, &var)) perror("glass-fb: virtual height");
    }
    for (int i = 0; i < RING; i++) { ring[i] = malloc(frame); if (!ring[i]) return 1; }
    /* The display controller, when the graphics layer scans this framebuffer
     * (its address within our pages): flips go to it directly. */
    smem = fix.smem_start;
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd >= 0) {
        void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, DISPC_BASE);
        if (p != MAP_FAILED) {
            volatile uint32_t *d = p;
            if (yuv) { ba0 = DISPC_VID1_BA0; ba1 = DISPC_VID1_BA1; }
            uint32_t ba = d[ba0 / 4];
            if (ba >= smem && ba < smem + page * pages) dispc = d;
            else fprintf(stderr, "glass-fb: the graphics layer scans 0x%08x, not this framebuffer (0x%08lx): pan only\n", ba, smem);
        }
    }
    fprintf(stderr, "glass-fb: %ux%u, %u bits, %d pages, %s, flips %s\n", var.xres, var.yres, var.bits_per_pixel, pages,
            fps ? "paced" : "as frames arrive", dispc ? "by the display controller's GO bit" : "by pan");

    pthread_t rt;
    pthread_create(&rt, NULL, reader, &cushion);

    double period = fps ? 1.0 / fps : 0, next_t = 0, last_shown = 0, max_gap = 0, report = now_s() + 5;
    unsigned long frames = 0, late = 0, rep_frames = 0, rep_drop = 0, rep_late = 0, rep_same = 0;
    prev = malloc(frame);
    for (;;) {
        pthread_mutex_lock(&mu);
        while (count == 0 && !eof) pthread_cond_wait(&cv, &mu);
        if (count == 0 && eof) { pthread_mutex_unlock(&mu); break; }
        if (fps) {
            if (next_t == 0) next_t = now_s() + cushion * period;   /* the first frame: build the cushion */
            double t = now_s();
            if (t < next_t) {
                pthread_mutex_unlock(&mu);
                struct timespec ts = { .tv_sec = (time_t) (next_t - t), .tv_nsec = (long) (((next_t - t) - (time_t) (next_t - t)) * 1e9) };
                nanosleep(&ts, NULL);
                pthread_mutex_lock(&mu);
                while (count == 0 && !eof) pthread_cond_wait(&cv, &mu);
                if (count == 0 && eof) { pthread_mutex_unlock(&mu); break; }
            }
            if (now_s() > next_t + period) { late++; rep_late++; next_t = now_s(); }   /* the slot was missed: re-anchor */
        }
        int slot = tail;
        tail = (tail + 1) % RING;
        count--;
        pthread_mutex_unlock(&mu);
        int same = prev && memcmp(prev, ring[slot], frame) == 0;
        if (prev) memcpy(prev, ring[slot], frame);
        show(ring[slot]);
        if (same) rep_same++;
        double t = now_s();
        if (last_shown && t - last_shown > max_gap) max_gap = t - last_shown;
        last_shown = t;
        frames++; rep_frames++;
        if (fps) next_t += period;
        if (t >= report) {
            pthread_mutex_lock(&mu);
            unsigned long d = dropped; dropped = 0;
            pthread_mutex_unlock(&mu);
            rep_drop = d;
            fprintf(stderr, "glass-fb: %.1f frames/s shown, %.1f/s new (the rest repeats of the previous picture), %lu dropped, %lu late slots, longest gap %.0f ms, %d queued\n",
                    rep_frames / 5.0, (rep_frames - rep_same) / 5.0, rep_drop, rep_late, max_gap * 1000, count);
            rep_frames = 0; rep_late = 0; rep_same = 0; max_gap = 0; report = t + 5;
        }
    }
    fprintf(stderr, "glass-fb: %lu frames, %lu late slots\n", frames, late);
    if (yuv) {   /* the overlay off: the console underneath shows again */
        struct omapfb_plane_info pi;
        if (ioctl(fb, OMAPFB_QUERY_PLANE, &pi) == 0) { pi.enabled = 0; ioctl(fb, OMAPFB_SETUP_PLANE, &pi); }
    } else if (dispc && shown != 0) {   /* leave the console's page on screen */
        dispc[ba0 / 4] = (uint32_t) smem;
        dispc[ba1 / 4] = (uint32_t) smem;
        dispc[DISPC_CONTROL2 / 4] |= DISPC_GO_LCD2;
    }
    munmap(mem, page * pages);
    close(fb);
    return 0;
}
