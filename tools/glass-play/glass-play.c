/* glass-play: raw s16le 48 kHz mono from stdin to the Glass's speaker, at
 * a fixed delay however the two clocks drift.
 *
 *   glass-play [-t TARGET_MS | -f FILE] [-d DEVICE]   (default 150 ms, plughw:0,0)
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
 * GLASS_PLAY_PROBE=1, or the file /run/glass/probe: a line "glass-play:
 * probe beep at T" (T on the monotonic clock, seconds) for each sound
 * starting after 300 ms of quiet, T the moment its first sample leaves the
 * speaker (the device's queue ahead of it counted); against glass-fb's
 * probe (the same clock) it measures sound against picture.
 *
 * Compiled on the Glass: gcc -O2 -o glass-play glass-play.c -lasound
 * (alsa-lib-dev); scripts/build-userland.sh installs it from out/.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000
#define RING (RATE * 2)          /* two seconds */
#define CHUNK 240                /* 5 ms written at a time */
#define DEVICE_US 40000          /* the device's own buffer */
#define TOL (RATE * 5 / 1000)    /* 5 ms either side of the target */
#define CUT (RATE * 80 / 1000)   /* a lasting error this large is corrected at once */

static int16_t ring[RING];
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
    const char *dev = "plughw:0,0", *tfile = NULL;
    int target_ms = 150;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) target_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) tfile = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else { fprintf(stderr, "glass-play [-t TARGET_MS | -f FILE] [-d DEVICE] < s16le-48k-mono\n"); return 1; }
    }
    if (tfile) target_ms = read_ms(tfile, target_ms);
    if (target_ms < 50) target_ms = 50;
    long target = (long) RATE * target_ms / 1000;
    double recheck = now_s() + 2;
    const char *pe = getenv("GLASS_PLAY_PROBE");
    int probe = (pe && *pe == '1') || access("/run/glass/probe", F_OK) == 0;

    snd_pcm_t *pcm;
    int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) { fprintf(stderr, "glass-play: %s: %s\n", dev, snd_strerror(err)); return 1; }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, RATE, 1, DEVICE_US);
    if (err < 0) { fprintf(stderr, "glass-play: %s: %s\n", dev, snd_strerror(err)); return 1; }
    snd_pcm_uframes_t bufsize, persize;
    snd_pcm_get_params(pcm, &bufsize, &persize);
    fprintf(stderr, "glass-play: %s, target %d ms, device buffer %lu samples (period %lu)%s\n",
            dev, target_ms, (unsigned long) bufsize, (unsigned long) persize, probe ? ", probe on" : "");

    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    unsigned char in[8192];
    size_t carry = 0;                 /* an odd byte left from the last read */
    int eof = 0, started = 0;
    unsigned long dropped = 0, doubled = 0, cuts = 0, pads = 0, underruns = 0, phase = 0;
    double smooth = -1;              /* the delay averaged over about 2 s */
    long quiet = RATE;                /* samples of quiet before the next one (probe) */
    double report = now_s() + 30;
    int16_t out[CHUNK + 1];

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
        snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);
        if (avail < 0) {
            underruns++;
            snd_pcm_recover(pcm, (int) avail, 1);
            started = 0; smooth = -1;
            continue;
        }
        snd_pcm_sframes_t queued = 0;
        if (snd_pcm_delay(pcm, &queued) < 0) queued = 0;
        if (!started && rcount < (unsigned) (target > (long) bufsize ? target - (long) bufsize : 0) + CHUNK && !eof) {
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
                    static int16_t zero[RATE / 2];
                    if (pad > RATE / 2) pad = RATE / 2;
                    if (snd_pcm_writei(pcm, zero, pad) > 0) { pads++; queued += pad; }
                    total = (long) rcount + queued;
                }
                smooth = total;
            }
            int mode = smooth > target + TOL ? 1 : smooth < target - TOL ? -1 : 0;
            unsigned n = 0;
            while (n < CHUNK && rcount > 0) {
                int16_t s = ring[rhead];
                rhead = (rhead + 1) % RING; rcount--;
                if (mode == 1 && ++phase % 1000 == 0 && rcount > 0) {   /* one left out */
                    s = ring[rhead]; rhead = (rhead + 1) % RING; rcount--; dropped++;
                }
                out[n++] = s;
                if (mode == -1 && ++phase % 1000 == 0 && n < CHUNK) { out[n++] = s; doubled++; }
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
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, out, n);
            if (w < 0) { underruns++; snd_pcm_recover(pcm, (int) w, 1); started = 0; smooth = -1; continue; }
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
        /* Until input comes or the device is down to two chunks. */
        int ms = (int) ((queued - 2 * CHUNK) * 1000 / RATE);
        if (ms < 1) ms = 1;
        if (ms > 20) ms = 20;
        struct pollfd p = { .fd = 0, .events = POLLIN };
        if (eof) usleep(ms * 1000);
        else poll(&p, 1, ms);
    }
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    return 0;
}
