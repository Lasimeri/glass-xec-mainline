/* glass-fb: raw BGRA frames from stdin onto the Glass's display, each shown
 * on a vertical sync, tear-free, and on a fixed schedule when asked.
 *
 *   ffmpeg ... -f rawvideo -pix_fmt bgra - | glass-fb [-r FPS] [-b N] [/dev/fb0]
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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OMAPFB_WAITFORVSYNC _IOW('O', 24, int)
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif

#define RING 4

static int fb, pages, use_omap = -1;
static unsigned char *mem;
static size_t page, frame, row, line;
static struct fb_var_screeninfo var;
static int shown = 0;

static unsigned char *ring[RING];
static int head = 0, tail = 0, count = 0, eof = 0;
static unsigned long dropped = 0;
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

static void show(const unsigned char *buf) {
    int next = (shown + 1) % pages;
    unsigned char *dst = mem + page * next;
    if (line == row) {
        memcpy(dst, buf, frame);
    } else {
        for (unsigned y = 0; y < var.yres; y++) memcpy(dst + (size_t) y * line, buf + y * row, row);
    }
    var.yoffset = var.yres * next;
    var.xoffset = 0;
    ioctl(fb, FBIOPAN_DISPLAY, &var);
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

int main(int argc, char **argv) {
    const char *dev = "/dev/fb0";
    int fps = 0, cushion = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) cushion = atoi(argv[++i]);
        else dev = argv[i];
    }
    if (cushion < 1) cushion = 1;
    if (cushion > RING - 2) cushion = RING - 2;
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
    mem = mmap(NULL, page * pages, PROT_WRITE, MAP_SHARED, fb, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    if (var.yres_virtual < var.yres * pages) {
        var.yres_virtual = var.yres * pages;
        if (ioctl(fb, FBIOPUT_VSCREENINFO, &var)) perror("glass-fb: virtual height");
    }
    for (int i = 0; i < RING; i++) { ring[i] = malloc(frame); if (!ring[i]) return 1; }
    fprintf(stderr, "glass-fb: %ux%u, %u bits, %d pages, %s\n", var.xres, var.yres, var.bits_per_pixel, pages,
            fps ? "paced" : "as frames arrive");

    pthread_t rt;
    pthread_create(&rt, NULL, reader, &cushion);

    double period = fps ? 1.0 / fps : 0, next_t = 0, last_shown = 0, max_gap = 0, report = now_s() + 5;
    unsigned long frames = 0, late = 0, rep_frames = 0, rep_drop = 0, rep_late = 0;
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
        show(ring[slot]);
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
            fprintf(stderr, "glass-fb: %.1f frames/s shown, %lu dropped, %lu late slots, longest gap %.0f ms, %d queued\n",
                    rep_frames / 5.0, rep_drop, rep_late, max_gap * 1000, count);
            rep_frames = 0; rep_late = 0; max_gap = 0; report = t + 5;
        }
    }
    fprintf(stderr, "glass-fb: %lu frames, %lu late slots\n", frames, late);
    munmap(mem, page * pages);
    close(fb);
    return 0;
}
