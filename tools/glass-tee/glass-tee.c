/* glass-tee: a YUV4MPEG2 stream (facetrack's output) from stdin, every byte
 * to stdout (the camera window here) and, frame by frame, to fd 3 as its
 * reader keeps up (the Glass's own display, scripts/glass-camera.sh). The
 * reader on fd 3 gets the stream's header first and then whole frames
 * ("FRAME" line and picture), so it always sees a valid stream; frames for
 * it wait in a queue of QUEUE, and a reader that is slow, stalled or gone
 * only misses frames, the oldest first. stdout is never held up by it.
 *
 *   facetrack ... | glass-tee 3> >(ffmpeg -f yuv4mpegpipe -i pipe:0 ...) | mpv ...
 *
 * Picture size from the header: W x H, chroma by C (420 kinds 3/2, 422 2,
 * 444 3, mono 1; 420 when not given, as the format says).
 *
 * Built on the desktop by scripts/glass-camera.sh into build/.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define QUEUE 3                      /* 200 ms at 15/s: latency stays bounded */
#define LINE 256

static size_t size;                  /* one frame: its "FRAME" line and picture */
static unsigned char *slot[QUEUE];   /* frames for fd 3, oldest at head */
static int head, count, done, dead;
static char header[LINE];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static int read_full(int fd, unsigned char *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t) r;
    }
    return 0;
}

static int write_all(int fd, const void *v, size_t n) {
    const unsigned char *p = v;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t) w;
    }
    return 0;
}

/* One line up to and with its newline, a byte at a time (headers only). */
static int read_line(char *buf, size_t max) {
    size_t n = 0;
    while (n + 1 < max) {
        if (read_full(0, (unsigned char *) buf + n, 1)) return -1;
        if (buf[n++] == '\n') { buf[n] = 0; return (int) n; }
    }
    return -1;
}

/* fd 3's writer: the header, then the oldest queued frame, written whole. */
static void *writer(void *arg) {
    unsigned char *buf = arg;
    if (write_all(3, header, strlen(header))) goto gone;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!count && !done) pthread_cond_wait(&cv, &mu);
        if (!count) { pthread_mutex_unlock(&mu); return NULL; }
        memcpy(buf, slot[head], size);
        head = (head + 1) % QUEUE;
        count--;
        pthread_mutex_unlock(&mu);
        if (write_all(3, buf, size)) goto gone;
    }
gone:
    pthread_mutex_lock(&mu);
    dead = 1;
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "glass-tee: the display's reader is gone; the window goes on\n");
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    if (read_line(header, sizeof header) < 0 || strncmp(header, "YUV4MPEG2 ", 10)) { fprintf(stderr, "glass-tee: not a YUV4MPEG2 stream\n"); return 1; }
    long w = 0, h = 0, num = 3, den = 2;
    char parse[LINE];
    snprintf(parse, sizeof parse, "%s", header + 10);
    for (char *t = strtok(parse, " \n"); t; t = strtok(NULL, " \n")) {
        if (t[0] == 'W') w = atol(t + 1);
        else if (t[0] == 'H') h = atol(t + 1);
        else if (t[0] == 'C') {
            if (!strncmp(t + 1, "444", 3)) { num = 3; den = 1; }
            else if (!strncmp(t + 1, "422", 3)) { num = 2; den = 1; }
            else if (!strncmp(t + 1, "mono", 4)) { num = 1; den = 1; }
        }
    }
    if (w <= 0 || h <= 0) { fprintf(stderr, "glass-tee: no size in the header\n"); return 1; }
    size_t pic = (size_t) w * (size_t) h * (size_t) num / (size_t) den;
    size = 6 + pic;   /* "FRAME\n" and the picture */
    unsigned char *in = malloc(size), *out = malloc(size);
    if (!in || !out) { fprintf(stderr, "glass-tee: out of memory\n"); return 1; }
    for (int i = 0; i < QUEUE; i++)
        if (!(slot[i] = malloc(size))) { fprintf(stderr, "glass-tee: out of memory\n"); return 1; }
    if (write_all(1, header, strlen(header))) return 0;
    pthread_t t;
    int have3 = fcntl(3, F_GETFD) != -1;   /* without fd 3: stdout alone */
    if (have3 && pthread_create(&t, NULL, writer, out)) have3 = 0;
    char fl[LINE];
    memcpy(in, "FRAME\n", 6);
    for (;;) {
        /* The frame's line as it came (it may carry parameters) to stdout;
         * to fd 3 a plain "FRAME". */
        if (read_line(fl, sizeof fl) < 0 || strncmp(fl, "FRAME", 5)) break;
        if (read_full(0, in + 6, pic)) break;
        if (write_all(1, fl, strlen(fl)) || write_all(1, in + 6, pic)) break;
        if (!have3) continue;
        pthread_mutex_lock(&mu);
        if (!dead) {
            if (count == QUEUE) { head = (head + 1) % QUEUE; count--; }   /* full: the oldest goes */
            memcpy(slot[(head + count) % QUEUE], in, size);
            count++;
            pthread_cond_signal(&cv);
        }
        pthread_mutex_unlock(&mu);
    }
    if (have3) {
        pthread_mutex_lock(&mu);
        done = 1; count = 0;
        pthread_cond_signal(&cv);
        pthread_mutex_unlock(&mu);
        pthread_join(t, NULL);
    }
    return 0;
}
