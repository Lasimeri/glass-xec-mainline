/* glass-play: raw s16le mono from stdin (48 kHz, or -r RATE: 32000 for the
 * bone conduction speaker, which reproduces no more) to the Glass's
 * speaker, at a fixed delay however the two clocks drift.
 *
 *   glass-play [-t TARGET_MS | -f FILE] [-r RATE] [-d DEVICE]   (150 ms, 48000, /dev/snd/pcmC0D0p)
 *
 * The desktop's sound clock and the Glass's converter never run at quite
 * the same rate. aplay took what came and fell behind: measured on
 * 2026-10-07, the pipe into aplay held 650 ms of sound (full, 64 KB) after
 * twenty minutes, the sound that far behind the picture. glass-play reads
 * its input the moment it arrives (the pipe stays empty), keeps it in its
 * own ring, and holds the total delay (ring plus the device's queue) at
 * TARGET. The decisions use the delay averaged over about 2 s, so the
 * link's jitter is absorbed, not chased: while it is more than 5 ms over,
 * one sample in a thousand is left out (0.1% faster, inaudible); more
 * than 5 ms under, one in a thousand is played twice; more than 80 ms
 * over for good (a backlog) it is cut at once, more than 80 ms early it
 * is held back once with quiet (a pad). The delay is kept here on the
 * Glass: -f FILE names a file holding TARGET in ms (glass-audio delay MS
 * writes /etc/glass/audio-delay), read again every 2 s, so a new value
 * applies while playing. Every 30 s a line on stderr: the delay, samples
 * left out and doubled, cuts, pads, underruns.
 *
 * The device is spoken to directly, with the ioctls of Google's 3.4 kernel
 * (struct layouts copied from its include/sound/asound.h), not through
 * alsa-lib: Alpine's alsa-lib is built for 64-bit time and keeps asking
 * the kernel for its time64 SYNC_PTR (0xc0884123), which 3.4 does not
 * have; it never knew where the hardware was, the stream was stopped and
 * started again every one to five seconds (heard as dropouts and gaps),
 * and the refusals came 2,000 to 6,000 times a second into the kernel log
 * (2026-10-07). Here: hardware parameters, software parameters, write,
 * delay and forward, none of which carries a time. The device runs at
 * its own rate (48000, else 44100) and the stream is resampled to it
 * (linear interpolation: the transducer reproduces nothing near the
 * images); underruns are filled with silence by the kernel (the stream is
 * never stopped), and noticed as a negative delay, then skipped.
 *
 * GLASS_PLAY_PROBE=1, or the file /run/glass/probe: a line "glass-play:
 * probe beep at T" (T on the monotonic clock, seconds) for each sound
 * starting after 300 ms of quiet, T the moment its first sample leaves the
 * speaker (the device's queue ahead of it counted); against glass-fb's
 * probe (the same clock) it measures sound against picture.
 *
 * Compiled on the Glass: gcc -O2 -o glass-play glass-play.c (nothing to
 * link); scripts/build-userland.sh installs it from out/.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int rate = 48000;          /* -r: the stream's sample rate (int: the comparisons with signed device counts stay signed on 32-bit ARM) */
#define RATE rate
#define RATE_MAX 48000
#define RING ((unsigned) (RATE * 2))   /* two seconds */
#define RING_MAX (RATE_MAX * 2)
#define CHUNK (RATE * 5 / 1000)  /* 5 ms written at a time */
#define CHUNK_MAX (RATE_MAX * 5 / 1000)
/* The device's own buffer: a third of the target, 40 to 120 ms. 40 ms
 * alone ran dry 81 times in 30 s over the tailnet, the 300 MHz CPU busy
 * with WireGuard and the Opus decoder (2026-10-07). */
static unsigned device_us(int target_ms) {
    int ms = target_ms / 3;
    return (unsigned) (ms < 40 ? 40 : ms > 120 ? 120 : ms) * 1000;
}
#define TOL (RATE * 5 / 1000)    /* 5 ms either side of the target */
#define CUT (RATE * 80 / 1000)   /* a lasting error this large is corrected at once */

/* Google's 3.4 kernel's PCM interface (include/sound/asound.h, protocol
 * 2.0.10). Only what carries no time: the sizes are those of a 32-bit
 * kernel, and each is part of its ioctl's number. */
typedef unsigned long k_uframes;
typedef long k_sframes;
struct k_mask { uint32_t bits[8]; };
struct k_interval { unsigned min, max; unsigned openmin : 1, openmax : 1, integer : 1, empty : 1; };
struct k_hw_params {
    unsigned flags;
    struct k_mask masks[3];          /* access, format, subformat */
    struct k_mask mres[5];
    struct k_interval intervals[12]; /* sample bits .. tick time */
    struct k_interval ires[9];
    unsigned rmask, cmask, info, msbits, rate_num, rate_den;
    k_uframes fifo_size;
    unsigned char reserved[64];
};
struct k_sw_params {
    int tstamp_mode;
    unsigned period_step, sleep_min;
    k_uframes avail_min, xfer_align, start_threshold, stop_threshold, silence_threshold, silence_size, boundary;
    unsigned char reserved[64];
};
struct k_xferi { k_sframes result; void *buf; k_uframes frames; };
#define K_HW_PARAMS _IOWR('A', 0x11, struct k_hw_params)
#define K_SW_PARAMS _IOWR('A', 0x13, struct k_sw_params)
#define K_DELAY _IOR('A', 0x21, k_sframes)
#define K_PREPARE _IO('A', 0x40)
#define K_FORWARD _IOW('A', 0x49, k_uframes)
#define K_WRITEI _IOW('A', 0x50, struct k_xferi)
enum { P_ACCESS = 0, P_FORMAT = 1, P_SUBFORMAT = 2, P_FIRST_INTERVAL = 8, P_CHANNELS = 10, P_RATE = 11,
       P_PERIOD_SIZE = 13, P_BUFFER_SIZE = 17 };
#define ACCESS_RW_INTERLEAVED 3
#define FORMAT_S16_LE 2
#define SUBFORMAT_STD 0

/* The device: its rate and channels, its buffer, and the resampler from
 * the stream's rate to its own (16.16 fixed point: s0 at the position 0,
 * s1 at 1, the next output at f). */
static struct {
    int fd;
    unsigned rate, channels;
    k_uframes buffer, period;
    uint32_t step, f;
    int32_t s0, s1;
} dev;

static void hw_any(struct k_hw_params *p) {
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 3; i++) memset(p->masks[i].bits, 0xff, sizeof p->masks[i].bits);
    for (int i = 0; i < 12; i++) { p->intervals[i].min = 0; p->intervals[i].max = UINT_MAX; }
    p->rmask = ~0u;
    p->info = ~0u;
}
static void hw_mask(struct k_hw_params *p, int param, unsigned v) {
    memset(p->masks[param].bits, 0, sizeof p->masks[param].bits);
    p->masks[param].bits[v / 32] = 1u << (v % 32);
}
static void hw_range(struct k_hw_params *p, int param, unsigned min, unsigned max) {
    struct k_interval *i = &p->intervals[param - P_FIRST_INTERVAL];
    i->min = min;
    i->max = max;
    i->openmin = i->openmax = 0;
}
static unsigned hw_value(const struct k_hw_params *p, int param) { return p->intervals[param - P_FIRST_INTERVAL].min; }

static int dev_open(const char *path, unsigned want_us) {
    dev.fd = open(path, O_RDWR | O_CLOEXEC);
    if (dev.fd < 0) { fprintf(stderr, "glass-play: %s: %s\n", path, strerror(errno)); return -1; }
    static const unsigned rates[] = { 48000, 44100 };
    struct k_hw_params p;
    int ok = 0;
    for (unsigned r = 0; r < 2 && !ok; r++)
        for (unsigned c = 1; c <= 2 && !ok; c++) {
            hw_any(&p);
            hw_mask(&p, P_ACCESS, ACCESS_RW_INTERLEAVED);
            hw_mask(&p, P_FORMAT, FORMAT_S16_LE);
            hw_mask(&p, P_SUBFORMAT, SUBFORMAT_STD);
            hw_range(&p, P_CHANNELS, c, c);
            hw_range(&p, P_RATE, rates[r], rates[r]);
            unsigned want = (unsigned) ((unsigned long long) rates[r] * want_us / 1000000);
            hw_range(&p, P_BUFFER_SIZE, want / 2, want);         /* the kernel takes the largest */
            hw_range(&p, P_PERIOD_SIZE, rates[r] / 200, rates[r] / 50);   /* 5 to 20 ms */
            ok = ioctl(dev.fd, K_HW_PARAMS, &p) == 0;
        }
    if (!ok) { fprintf(stderr, "glass-play: %s takes neither 48000 nor 44100 Hz, S16_LE, 1 or 2 channels: %s\n", path, strerror(errno)); return -1; }
    dev.rate = hw_value(&p, P_RATE);
    dev.channels = hw_value(&p, P_CHANNELS);
    dev.buffer = hw_value(&p, P_BUFFER_SIZE);
    dev.period = hw_value(&p, P_PERIOD_SIZE);
    /* Never stopped: the played part is filled with silence by the
     * kernel (silence_size = boundary), so an underrun is a short quiet,
     * not a stop and a restart. The kernel's own boundary: the buffer
     * doubled while it fits a long. */
    k_uframes boundary = dev.buffer;
    while (boundary * 2 <= (k_uframes) LONG_MAX - dev.buffer) boundary *= 2;
    struct k_sw_params s;
    memset(&s, 0, sizeof s);
    s.period_step = 1;
    s.avail_min = dev.period;
    s.xfer_align = 1;
    s.start_threshold = dev.period;      /* starts once a period is queued */
    s.stop_threshold = boundary;
    s.silence_threshold = 0;
    s.silence_size = boundary;
    s.boundary = boundary;
    if (ioctl(dev.fd, K_SW_PARAMS, &s) < 0) { fprintf(stderr, "glass-play: software parameters: %s\n", strerror(errno)); return -1; }
    if (ioctl(dev.fd, K_PREPARE) < 0) { fprintf(stderr, "glass-play: prepare: %s\n", strerror(errno)); return -1; }
    dev.step = (uint32_t) (((uint64_t) RATE << 16) / dev.rate);
    dev.f = 0;
    dev.s0 = dev.s1 = 0;
    return 0;
}

/* Frames queued in the device, in the stream's samples; a negative count
 * (it played past what was written: an underrun, filled with silence) is
 * skipped over, so what comes next is heard at once. */
static long dev_queued(unsigned long *underruns) {
    k_sframes d = 0;
    if (ioctl(dev.fd, K_DELAY, &d) < 0) {
        if (errno == EPIPE) { ioctl(dev.fd, K_PREPARE); (*underruns)++; }
        return 0;
    }
    if (d < 0) {
        k_uframes skip = (k_uframes) -d;
        ioctl(dev.fd, K_FORWARD, &skip);
        (*underruns)++;
        return 0;
    }
    return (long) ((long long) d * RATE / dev.rate);
}

/* Device frames, written whole (blocking while the device is full). */
static int dev_write_frames(int16_t *f, k_uframes n, unsigned long *underruns) {
    while (n > 0) {
        struct k_xferi x = { 0, f, n };
        if (ioctl(dev.fd, K_WRITEI, &x) < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (errno == EPIPE) { ioctl(dev.fd, K_PREPARE); (*underruns)++; continue; }
            fprintf(stderr, "glass-play: write: %s\n", strerror(errno));
            return -1;
        }
        n -= (k_uframes) x.result;
        f += (size_t) x.result * dev.channels;
    }
    return 0;
}

/* N stream samples, resampled to the device's rate (and to two channels
 * if it wants two). */
static int dev_write(const int16_t *in, unsigned n, unsigned long *underruns) {
    static int16_t out[(CHUNK_MAX * 6 + 8) * 2];
    unsigned o = 0, i = 0;
    for (;;) {
        while (dev.f < 65536) {
            int32_t v = dev.s0 + (int32_t) (((int64_t) (dev.s1 - dev.s0) * dev.f) >> 16);
            out[o++] = (int16_t) v;
            if (dev.channels == 2) out[o++] = (int16_t) v;
            dev.f += dev.step;
        }
        if (i == n) break;
        dev.s0 = dev.s1;
        dev.s1 = in[i++];
        dev.f -= 65536;
    }
    return dev_write_frames(out, o / dev.channels, underruns);
}

/* N stream samples of quiet. */
static int dev_quiet(unsigned n, unsigned long *underruns) {
    static int16_t zero[RATE_MAX];   /* half a second at 48 kHz, two channels */
    k_uframes frames = (k_uframes) ((unsigned long long) n * dev.rate / RATE);
    while (frames > 0) {
        k_uframes k = frames < RATE_MAX / 2 ? frames : RATE_MAX / 2;
        if (dev_write_frames(zero, k, underruns) < 0) return -1;
        frames -= k;
    }
    dev.s0 = dev.s1 = 0;
    return 0;
}

static int16_t ring[RING_MAX];
static unsigned rhead, rcount;   /* oldest sample, samples held */

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* -f FILE: the target in milliseconds as text (glass-audio delay MS writes
 * /etc/glass/audio-delay); read again every 2 s, so a new value applies
 * while playing. */
static int read_ms(const char *path, int fallback) {
    char b[16] = { 0 };
    int f = open(path, O_RDONLY);
    if (f < 0) return fallback;
    ssize_t n = read(f, b, sizeof b - 1);
    close(f);
    int v = n > 0 ? atoi(b) : 0;
    return v >= 50 && v <= 2000 ? v : fallback;
}

static void ring_put(const int16_t *s, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (rcount == RING) { rhead = (rhead + 1) % RING; rcount--; }   /* full: the oldest goes */
        ring[(rhead + rcount) % RING] = s[i];
        rcount++;
    }
}

int main(int argc, char **argv) {
    const char *path = "/dev/snd/pcmC0D0p", *tfile = NULL;
    int target_ms = 150;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) target_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) tfile = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) path = argv[++i];
        else { fprintf(stderr, "glass-play [-t TARGET_MS | -f FILE] [-r RATE] [-d DEVICE] < s16le-mono\n"); return 1; }
    }
    if (rate < 8000 || rate > RATE_MAX) { fprintf(stderr, "glass-play: -r between 8000 and %d\n", RATE_MAX); return 1; }
    if (tfile) target_ms = read_ms(tfile, target_ms);
    if (target_ms < 50) target_ms = 50;
    long target = (long) RATE * target_ms / 1000;
    double recheck = now_s() + 2;
    const char *pe = getenv("GLASS_PLAY_PROBE");
    int probe = (pe && *pe == '1') || access("/run/glass/probe", F_OK) == 0;

    if (dev_open(path, device_us(target_ms)) < 0) return 1;
    long bufsize = (long) ((long long) dev.buffer * RATE / dev.rate);   /* in the stream's samples */
    fprintf(stderr, "glass-play: %s at %u Hz, %u channel%s (the stream's %d Hz resampled), target %d ms, device buffer %lu frames (period %lu)%s\n",
            path, dev.rate, dev.channels, dev.channels == 1 ? "" : "s", rate, target_ms,
            (unsigned long) dev.buffer, (unsigned long) dev.period, probe ? ", probe on" : "");

    /* Real-time scheduling (SCHED_FIFO, priority 50): with both cores held
     * at 300 MHz by the thermal cap and busy with the picture's decoder
     * (load average 2.1 to 2.6), glass-play must not wait behind it for
     * longer than the device's buffer. It needs a tenth of one core, and
     * every pass of its loop sleeps (poll or usleep), so first in line costs
     * the picture nothing it can notice. musl's sched_setscheduler() is a
     * stub (ENOSYS): the system call itself. */
    struct sched_param sp = { .sched_priority = 50 };
    if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp) < 0)
        fprintf(stderr, "glass-play: no real-time priority (%s): it may underrun under load\n", strerror(errno));
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    unsigned char in[8192];
    size_t carry = 0;                 /* an odd byte left from the last read */
    int eof = 0, started = 0;
    unsigned long dropped = 0, doubled = 0, cuts = 0, pads = 0, underruns = 0, phase = 0;
    double smooth = -1;              /* the delay averaged over about 2 s */
    long quiet = RATE;                /* samples of quiet before the next one (probe) */
    double report = now_s() + 30;
    int16_t out[CHUNK_MAX + 1];

    for (;;) {
        /* Everything that has arrived, at once. */
        for (;;) {
            ssize_t n = read(0, in + carry, sizeof in - carry);
            if (n == 0) { eof = 1; break; }
            if (n < 0) { if (errno != EAGAIN && errno != EINTR) eof = 1; break; }
            size_t have = carry + (size_t) n, whole = have / 2;
            ring_put((const int16_t *) in, (unsigned) whole);
            carry = have & 1;
            if (carry) in[0] = in[have - 1];
        }
        long queued = dev_queued(&underruns);
        long avail = bufsize - queued;
        if (!started && rcount < (unsigned) (target > bufsize ? target - bufsize : 0) + CHUNK && !eof) {
            /* Filling up to the target before the first sample plays. */
        } else if (avail >= CHUNK && (rcount > 0 || (started && queued < 2 * CHUNK))) {
            /* With nothing held, quiet goes in only when the device is
             * nearly empty: padding it would add delay. */
            long total = (long) rcount + queued;
            /* Decisions on the delay averaged over about 2 s: the link
             * jitters by tens of ms (Wi-Fi), which the held sound absorbs;
             * acting on each swing cut and padded the sound dozens of
             * times a minute (2026-10-07). */
            smooth = smooth < 0 ? total : smooth + (total - smooth) * ((double) CHUNK / (2 * RATE));
            if (smooth > target + CUT) {   /* a lasting backlog: back to the target at once */
                unsigned cut = (unsigned) (total - target);
                if (cut > rcount) cut = rcount;
                rhead = (rhead + cut) % RING; rcount -= cut;
                cuts++;
                total = (long) rcount + queued;
                smooth = total;
            }
            if (started && smooth < target - CUT && rcount > 0) {
                /* Lastingly early (a larger target, or after a long
                 * stall): quiet once, back to the target, rather than
                 * playing ahead of the picture. */
                long pad = target - total;
                if (pad > avail - CHUNK) pad = avail - CHUNK;
                if (pad > 0) {
                    if (pad > RATE / 2) pad = RATE / 2;
                    if (dev_quiet((unsigned) pad, &underruns) == 0) { pads++; queued += pad; }
                    total = (long) rcount + queued;
                }
                smooth = total;
            }
            int mode = smooth > target + TOL ? 1 : smooth < target - TOL ? -1 : 0;
            unsigned n = 0;
            while (n < (unsigned) CHUNK && rcount > 0) {
                int16_t s = ring[rhead];
                rhead = (rhead + 1) % RING; rcount--;
                if (mode == 1 && ++phase % 1000 == 0 && rcount > 0) {   /* one left out */
                    s = ring[rhead]; rhead = (rhead + 1) % RING; rcount--; dropped++;
                }
                out[n++] = s;
                if (mode == -1 && ++phase % 1000 == 0 && n < (unsigned) CHUNK) { out[n++] = s; doubled++; }
                if (probe) {
                    int loud = s > 8000 || s < -8000;
                    if (loud && quiet >= RATE * 3 / 10)
                        fprintf(stderr, "glass-play: probe beep at %.4f\n", now_s() + (double) (queued + n - 1) / RATE);
                    quiet = loud ? 0 : quiet + 1;
                }
            }
            if (n == 0) {   /* nothing came: keep the device fed with quiet */
                memset(out, 0, sizeof out);
                n = CHUNK;
            }
            if (dev_write(out, n, &underruns) < 0) return 1;
            started = 1;
            continue;
        }
        if (eof && rcount == 0) break;
        if (tfile && now_s() >= recheck) {
            int t = read_ms(tfile, target_ms);
            if (t != target_ms) {
                fprintf(stderr, "glass-play: target now %d ms (was %d)\n", t, target_ms);
                target_ms = t;
                target = (long) RATE * t / 1000;
            }
            recheck = now_s() + 2;
        }
        if (now_s() >= report) {
            fprintf(stderr, "glass-play: delay %ld ms of %d (held %u, device %ld), %lu left out, %lu doubled, %lu cuts, %lu pads, %lu underruns\n",
                    ((long) rcount + queued) * 1000 / RATE, target_ms, rcount * 1000 / RATE, (long) queued * 1000 / RATE,
                    dropped, doubled, cuts, pads, underruns);
            dropped = doubled = cuts = pads = underruns = 0;
            report = now_s() + 30;
        }
        /* Until input comes, or woken with half the device's buffer still
         * to play: margin for a busy CPU (the old 10 ms margin underran
         * over the tailnet). */
        int ms = (int) ((queued - bufsize / 2) * 1000 / RATE);
        if (ms < 1) ms = 1;
        if (ms > 10) ms = 10;
        struct pollfd p = { .fd = 0, .events = POLLIN };
        if (eof) usleep(ms * 1000);
        else poll(&p, 1, ms);
    }
    /* No drain: a stream that never stops would wait on it; closing ends it. */
    close(dev.fd);
    return 0;
}
