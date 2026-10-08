/* glass-fb: raw BGRA frames from stdin onto the Glass's display, each shown
 * on a vertical sync, tear-free, and on a fixed schedule when asked.
 *
 *   ffmpeg ... -f rawvideo -pix_fmt bgra - | glass-fb [-r FPS] [-b N] [/dev/fb0]
 *   ffmpeg ... -f rawvideo -pix_fmt yuv420p - | glass-fb -y [-r FPS] [-b N]
 *       (-y: I420 in, packed to YUYV for the video overlay, which converts to RGB)
 *   glass-camera -y -w 640 -h 360 | glass-fb -n [-r FPS]
 *       (-n: NV12 in, as the camera gives it: the camera on the Glass's own screen)
 *
 * The framebuffer (omapfb, 640x360, 32 bits) holds three pages: a frame is
 * written into a page not on screen, the display controller is pointed at
 * it (pan) and the sync waited for (312 fields a second: at most 3 ms).
 *
 * -r FPS paces presentation: frames are shown at exactly 1/FPS intervals
 * on the Glass's own clock, N frames (-b, default 1, at most 6) of cushion absorbing
 * the link's jitter, so motion is even however the packets arrived. A
 * frame that is not there at its slot is shown as soon as it comes and the
 * schedule re-anchors; frames piling up beyond the cushion are dropped,
 * the newest kept, so latency never grows. Without -r each frame is shown
 * as it arrives. Every 5 s a line on stderr says what happened (frames,
 * drops, late slots, the longest gap between two frames shown).
 *
 * The console underneath (glass-console, a shell on page 0 of /dev/fb0) is
 * the idle picture: with -y the overlay is switched on only once the first
 * frame is in it, off again when no frame came for IDLE_S seconds (the
 * stream stopped or stalled) and at the end, also on SIGTERM, SIGINT and
 * SIGHUP; without -y the frames use pages 1 and 2 and page 0 is put back.
 * So no old picture stays on the glasses when the stream stops. While
 * frames are shown the CPU runs at its top clock (clock_set below).
 *
 * GLASS_FB_PROBE=1, or the file /run/glass/probe: a line "glass-fb: probe
 * light at T" (T on the monotonic clock, seconds) each time a frame
 * turning bright overall reaches the screen; with glass-play's probe on
 * the sound (the same clock), how far picture and sound are apart on the
 * Glass is the difference of the two times for one flash-and-beep.
 *
 * Compiled on the Glass (gcc from Alpine), installed as
 * /usr/local/bin/glass-fb; scripts/glass-view.sh pipes ffmpeg into it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#ifndef OMAPFB_WAITFORVSYNC
#define OMAPFB_WAITFORVSYNC _IO('O', 57)   /* linux/omapfb.h: OMAP_IO(57) */
#endif
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif

#define RING 8   /* frames held: the cushion (-b) goes up to RING - 2 */

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
static volatile uint32_t *dispc, *dmap;
static unsigned long smem;
#define IDLE_S 2

static int fb, pages, use_omap = -1;
static int first_page;               /* 1 without -y: page 0 is the console's */
static volatile int plane_on;        /* -y: the overlay is on screen */
static unsigned ow, oh;              /* -y: the overlay's size, the panel's */
static int idle_reset;               /* the console was shown: rebuild the cushion */
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
static int i420, nv12;   /* -n: NV12 in (glass-camera -y), U and V interleaved */
/* A line of 4:2:0 picture packed into YUYV: Y even, U, Y odd, V for each
 * pair of pixels. With NEON, 16 pixels a step (vld2 splits the Y into even
 * and odd, vst4 interleaves the four into 32 bytes): the packing was 5 % of
 * both cores at 300 MHz in C (2026-10-07). u and v are the line's chroma
 * (I420: two planes; NV12: one, interleaved, step 2). */
static void pack_line(uint8_t *o, const uint8_t *y, const uint8_t *u, const uint8_t *v, int step, unsigned w) {
    unsigned x = 0;
#ifdef __ARM_NEON
    for (; x + 16 <= w; x += 16) {
        uint8x8x2_t yy = vld2_u8(y + x);
        uint8x8x4_t q;
        if (step == 2) { uint8x8x2_t c = vld2_u8(u + x); q.val[1] = c.val[0]; q.val[3] = c.val[1]; }
        else { q.val[1] = vld1_u8(u + x / 2); q.val[3] = vld1_u8(v + x / 2); }
        q.val[0] = yy.val[0]; q.val[2] = yy.val[1];
        vst4_u8(o + 2 * x, q);
    }
#endif
    for (; x + 1 < w; x += 2) {
        o[2 * x] = y[x]; o[2 * x + 1] = u[x / 2 * step]; o[2 * x + 2] = y[x + 1]; o[2 * x + 3] = v[x / 2 * step];
    }
}

static void pack_yuyv(unsigned char *dst, const unsigned char *src) {
    unsigned w = var.xres, h = var.yres;
    if (nv12) {
        const unsigned char *Y = src, *UV = src + (size_t) w * h;
        for (unsigned yy = 0; yy < h; yy++) {
            const unsigned char *uv = UV + (size_t) (yy / 2) * w;
            pack_line(dst + (size_t) yy * line, Y + (size_t) yy * w, uv, uv + 1, 2, w);
        }
        return;
    }
    const unsigned char *Y = src, *U = src + w * h, *V = U + (w / 2) * (h / 2);
    for (unsigned yy = 0; yy < h; yy++)
        pack_line(dst + (size_t) yy * line, Y + (size_t) yy * w, U + (size_t) (yy / 2) * (w / 2), V + (size_t) (yy / 2) * (w / 2), 1, w);
}

/* -y: the overlay on, at the page the driver was last panned to (show()
 * pans before it calls this, so the first picture is the new frame). The
 * flips by register need VID1's address inside our pages: checked here,
 * once the plane has one (after a cold boot it has none before). */
static void overlay_on(void) {
    struct omapfb_plane_info pi;
    if (ioctl(fb, OMAPFB_QUERY_PLANE, &pi)) { perror("glass-fb: query plane"); return; }
    pi.pos_x = 0; pi.pos_y = 0; pi.out_width = ow; pi.out_height = oh; pi.enabled = 1;
    if (ioctl(fb, OMAPFB_SETUP_PLANE, &pi)) { perror("glass-fb: enable plane"); return; }
    plane_on = 1;
    if (!dispc && dmap) {
        uint32_t ba = dmap[ba0 / 4];
        if (ba >= smem && ba < smem + page * pages) dispc = dmap;
        else { fprintf(stderr, "glass-fb: the video layer scans 0x%08x, not this framebuffer (0x%08lx): pan only\n", ba, smem); dmap = NULL; }
        fprintf(stderr, "glass-fb: flips %s\n", dispc ? "by the display controller's GO bit" : "by pan");
    }
}

/* The CPU clock follows the stream: at least 600 MHz (OPP100, 1.2 V) while
 * frames are shown, the governor's own choice down to 300 MHz again when
 * the console is on screen. At 300 MHz, where the governor (target load
 * 90) held it, the decoder kept up only on average and 21 to 23 frames a
 * second came out (2026-10-07). Not the top point (1008 MHz, OPP Nitro):
 * the Glass's case-temperature governor (notle_pcb_sensor, case_governor)
 * and the Nitro duty-cycle governor cap the clock below it on a warm Glass
 * anyway (measured: floor 1008 MHz, hardware at 300 and 600), and the user
 * chose 600 MHz. Through scaling_min_freq of cpu0, the policy both cores
 * share; open, write and close only, so the signal handler may call it. */
static char freq_top[16] = "600000", freq_low[16];
static int clock_fast = -1;
static void read_freq(const char *name, char *out) {
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu0/cpufreq/%s", name);
    int f = open(path, O_RDONLY);
    ssize_t n = f >= 0 ? read(f, out, 15) : -1;
    if (f >= 0) close(f);
    out[n > 0 ? n : 0] = 0;
}
static void clock_set(int fast) {
    const char *v = fast ? freq_top : freq_low;
    if (fast == clock_fast || !v[0]) return;
    int f = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", O_WRONLY);
    if (f < 0) return;
    if (write(f, v, strlen(v)) > 0) clock_fast = fast;
    close(f);
}

/* The console back on screen: the overlay off (-y), or the graphics layer
 * on page 0. Also from the signal handler: ioctl and stores only. */
static void console(void) {
    clock_set(0);
    if (i420) {
        struct omapfb_plane_info pi;
        if (plane_on && ioctl(fb, OMAPFB_QUERY_PLANE, &pi) == 0) { pi.enabled = 0; ioctl(fb, OMAPFB_SETUP_PLANE, &pi); }
        plane_on = 0;
        return;
    }
    var.xoffset = 0; var.yoffset = 0;
    ioctl(fb, FBIOPAN_DISPLAY, &var);
    if (dispc) {
        dispc[ba0 / 4] = (uint32_t) smem;
        dispc[ba1 / 4] = (uint32_t) smem;
        dispc[DISPC_CONTROL2 / 4] |= DISPC_GO_LCD2;
    }
    shown = 0;
}

static void on_signal(int sig) {
    (void) sig;
    console();
    _exit(0);
}

/* The battery, top right on every frame shown: the fuel gauge's charge
 * (bq27520's capacity, the one glass-console's status line shows), "+"
 * while charging, full or on USB power, read every 10 s; white on a dark box, red under
 * 20 percent while not plugged in. The stream covers the console, so this is
 * where the charge is seen while it runs. GLASS_FB_BATTERY=0: none. */
#define BATT "/sys/class/power_supply/bq27520-0/"
static const unsigned char glyph5x7[13][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 },   /* % */
    { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 },   /* + */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 },   /* ? */
};
static char batt[8];
static int batt_low, batt_off = -1;
static double batt_at = -100;

static void batt_read(void) {
    double t = now_s();
    if (t - batt_at < 10) return;
    batt_at = t;
    char cap[16] = "", st[32] = "";
    FILE *f = fopen(BATT "capacity", "r");
    if (f) { if (!fgets(cap, sizeof cap, f)) cap[0] = 0; fclose(f); }
    f = fopen(BATT "status", "r");
    if (f) { if (!fgets(st, sizeof st, f)) st[0] = 0; fclose(f); }
    /* Plugged in: the USB supply online (the gauge's own status flips
     * between charging and not while its current reads 0). */
    char usb[8] = "";
    f = fopen("/sys/class/power_supply/twl6030_usb/online", "r");
    if (f) { if (!fgets(usb, sizeof usb, f)) usb[0] = 0; fclose(f); }
    if (!cap[0]) { snprintf(batt, sizeof batt, "?%%"); batt_low = 0; return; }
    int c = atoi(cap), charging = !strncmp(st, "Charging", 8) || !strncmp(st, "Full", 4) || usb[0] == '1';
    snprintf(batt, sizeof batt, "%d%%%s", c, charging ? "+" : "");
    batt_low = c < 20 && !charging;
}

/* One pixel of colour c (0 the box, 1 white, 2 red) in the page at dst:
 * YUYV (a pair of pixels shares U and V; the glyphs are drawn on whole
 * pairs) or BGRA. */
static void batt_px(unsigned char *dst, unsigned x, unsigned y, int c) {
    static const unsigned char Y[3] = { 16, 235, 81 }, U[3] = { 128, 128, 90 }, V[3] = { 128, 128, 240 };
    static const unsigned char R[3] = { 0, 255, 255 }, G[3] = { 0, 255, 0 }, B[3] = { 0, 255, 0 };
    if (x >= var.xres || y >= var.yres) return;
    unsigned char *p = dst + (size_t) y * line;
    if (i420) {
        unsigned char *q = p + (size_t) (x / 2) * 4;
        q[(x & 1) ? 2 : 0] = Y[c];
        q[1] = U[c];
        q[3] = V[c];
    } else {
        unsigned char *q = p + (size_t) x * 4;
        q[0] = B[c]; q[1] = G[c]; q[2] = R[c]; q[3] = 255;
    }
}

static void draw_batt(unsigned char *dst) {
    if (batt_off < 0) { const char *e = getenv("GLASS_FB_BATTERY"); batt_off = e && !strcmp(e, "0"); }
    if (batt_off) return;
    batt_read();
    size_t n = strlen(batt);
    if (!n) return;
    const unsigned S = 2, cw = 6 * S, ch = 7 * S, pad = 4;
    unsigned bw = (unsigned) n * cw - S + 2 * pad, bh = ch + 2 * pad;
    if (bw + 8 > var.xres) return;
    unsigned x0 = (var.xres - bw - 8) & ~1u, y0 = 8;
    for (unsigned y = 0; y < bh; y++)
        for (unsigned x = 0; x < bw; x++) batt_px(dst, x0 + x, y0 + y, 0);
    for (size_t i = 0; i < n; i++) {
        char k = batt[i];
        int g = k >= '0' && k <= '9' ? k - '0' : k == '%' ? 10 : k == '+' ? 11 : 12;
        for (unsigned r = 0; r < 7; r++)
            for (unsigned c = 0; c < 5; c++)
                if (glyph5x7[g][r] & (0x10 >> c))
                    for (unsigned dy = 0; dy < S; dy++)
                        for (unsigned dx = 0; dx < S; dx++)
                            batt_px(dst, x0 + pad + (unsigned) i * cw + c * S + dx, y0 + pad + r * S + dy, batt_low ? 2 : 1);
    }
}

static void show(const unsigned char *buf) {
    int next = shown + 1 >= pages ? first_page : shown + 1;
    unsigned char *dst = mem + page * next;
    if (i420) {
        pack_yuyv(dst, buf);
    } else if (line == row) {
        memcpy(dst, buf, frame);
    } else {
        for (unsigned y = 0; y < var.yres; y++) memcpy(dst + (size_t) y * line, buf + y * row, row);
    }
    draw_batt(dst);
    var.yoffset = var.yres * next;
    var.xoffset = 0;
    ioctl(fb, FBIOPAN_DISPLAY, &var);
    clock_set(1);
    if (i420 && !plane_on) overlay_on();
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
 * fb1 has no memory at boot: three pages are given to it and its format
 * set; the plane is enabled by overlay_on() at the first frame, at 0,0 at
 * the panel's size, so a stream that never starts shows the console. */
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
    ow = w; oh = h;
    close(f1);
    return 0;
}

/* Waits (mu held) for a frame; with none for IDLE_S seconds the stream is
 * taken as stopped or stalled and the console shown, once, until frames
 * come again. 0: a frame is queued; -1: the input ended. */
static int wait_frame(void) {
    while (count == 0 && !eof) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += IDLE_S;
        if (pthread_cond_timedwait(&cv, &mu, &ts) == ETIMEDOUT && count == 0 && !eof && (plane_on || shown != 0)) {
            console();
            idle_reset = 1;
            fprintf(stderr, "glass-fb: no frame for %d s: the console is on screen until frames come\n", IDLE_S);
        }
    }
    return count == 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    const char *dev = "/dev/fb0";
    int fps = 0, cushion = 1, yuv = 0;
    const char *pe = getenv("GLASS_FB_PROBE");
    int probe = (pe && *pe == '1') || access("/run/glass/probe", F_OK) == 0, was_light = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) cushion = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-y")) { yuv = 1; dev = "/dev/fb1"; }
        else if (!strcmp(argv[i], "-n")) { yuv = 1; nv12 = 1; dev = "/dev/fb1"; }
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
            if (yuv) {   /* VID1 has its address once enabled: checked then */
                ba0 = DISPC_VID1_BA0; ba1 = DISPC_VID1_BA1;
                dmap = d;
            } else {
                uint32_t ba = d[ba0 / 4];
                if (ba >= smem && ba < smem + page * pages) dispc = d;
                else fprintf(stderr, "glass-fb: the graphics layer scans 0x%08x, not this framebuffer (0x%08lx): pan only\n", ba, smem);
            }
        }
    }
    /* Without -y page 0 stays the console's (glass-console draws there) when
     * there are three pages: the frames flip between 1 and 2. */
    first_page = !yuv && pages >= 3 ? 1 : 0;
    read_freq("cpuinfo_min_freq", freq_low);
    shown = yuv ? pages - 1 : 0;
    fprintf(stderr, "glass-fb: %ux%u, %u bits, %d pages, %s, flips %s\n", var.xres, var.yres, var.bits_per_pixel, pages,
            fps ? "paced" : "as frames arrive", yuv ? "set up at the first frame" : dispc ? "by the display controller's GO bit" : "by pan");

    /* The console back on screen however this ends: a stream stopped by
     * kill, Ctrl-C or a dropped session; a closed stderr must not kill it
     * first. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &ca);

    pthread_t rt;
    pthread_create(&rt, NULL, reader, &cushion);

    double period = fps ? 1.0 / fps : 0, next_t = 0, last_shown = 0, max_gap = 0, report = now_s() + 5;
    unsigned long frames = 0, late = 0, rep_frames = 0, rep_drop = 0, rep_late = 0, rep_same = 0;
    prev = malloc(frame);
    for (;;) {
        pthread_mutex_lock(&mu);
        if (wait_frame()) { pthread_mutex_unlock(&mu); break; }
        if (idle_reset) { idle_reset = 0; next_t = 0; last_shown = 0; }
        if (fps) {
            if (next_t == 0) next_t = now_s() + cushion * period;   /* the first frame: build the cushion */
            double t = now_s();
            if (t < next_t) {
                pthread_mutex_unlock(&mu);
                struct timespec ts = { .tv_sec = (time_t) (next_t - t), .tv_nsec = (long) (((next_t - t) - (time_t) (next_t - t)) * 1e9) };
                nanosleep(&ts, NULL);
                pthread_mutex_lock(&mu);
                if (wait_frame()) { pthread_mutex_unlock(&mu); break; }
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
        if (probe) {   /* the mean brightness, every 61st byte: Y in -y frames, any channel in BGRA */
            const unsigned char *b = ring[slot];
            size_t span = i420 ? (size_t) var.xres * var.yres : frame;
            unsigned long sum = 0, n = 0;
            for (size_t i = 0; i < span; i += 61) { sum += b[i]; n++; }
            int light = n && sum / n > 128;
            if (light && !was_light) fprintf(stderr, "glass-fb: probe light at %.4f\n", now_s());
            was_light = light;
        }
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
    console();   /* the overlay off, or page 0: the console shows again */
    munmap(mem, page * pages);
    close(fb);
    return 0;
}
