/* glass-monitor: the one stream behind the Glass's display. Raw NV12 frames of
 * one desktop monitor on stdout, for an encoder that never stops; which monitor
 * is a line on a FIFO, not a restart.
 *
 *   glass-monitor W H FPS MONITOR CONTROL CAPTURE | ffmpeg ... | ssh ...
 *   echo DP-3 > CONTROL
 *
 * CAPTURE MONITOR FPS writes that monitor's frames to its stdout
 * (scripts/glass-capture.sh). A new choice starts a second capture beside the
 * first; the first whole frame from it ends the old one, so the picture changes
 * from one frame to the next and nothing downstream sees a gap, an end of
 * stream or a new key frame. A capture that dies is started again a second later
 * and the stream stays up. While no new frame comes the last one is repeated, so
 * the decoder (which lets a picture out when the next frame arrives) and
 * glass-fb (which falls back to the console after 2 s) keep going.
 *
 * Only whole frames pass: a capture killed in mid-frame leaves its partial frame
 * in a pipe that closes with it. The bytes are never copied here: splice() moves
 * a frame's pages from the capture's pipe to stdout, tee() first keeps a second
 * reference to them in a pipe of its own, and repeating is tee() from there.
 * CPU a frame, interleaved twice: 43.1 and 45.6 us, against 64.5 and 65.0 with
 * read(), a kept copy and write() (2026-11-04, this desktop).
 *
 * Built on the desktop on first use (scripts/glass-view.sh).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PIPE_BYTES (1 << 20)       /* the unprivileged maximum: 256 pages, a frame here is 85 */
#define RESTART_S 1.0
#define SWITCH_TIMEOUT_S 10.0
#define REPEAT_S 0.5               /* no frame for this long: the last one again. Not sooner:
                                    * at 125 ms a late frame and its repeat came both, 13/s
                                    * reached the Glass and glass-fb dropped the surplus. */

struct cap { pid_t pid; int fd; double born; char name[64]; };

static struct cap cur = { -1, -1, 0, "" }, nxt = { -1, -1, 0, "" };
static volatile sig_atomic_t stop;
static size_t frame;
static int hold[2], devnull, held;
static const char *capture, *fps_s;
static double last_out, restart_at;
static unsigned long repeats;       /* since the last new frame, said when one comes */
static char want[64];

static void on_signal(int s) { (void) s; stop = 1; }

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec / 1e9;
}

static size_t in_pipe(int fd) {
    int n = 0;
    return ioctl(fd, FIONREAD, &n) == 0 && n > 0 ? (size_t) n : 0;
}

static void die(const char *what) {
    fprintf(stderr, "glass-monitor: %s: %s\n", what, strerror(errno));
    stop = 1;
}

static void spawn(struct cap *c, const char *name) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) { die("pipe"); return; }
    if (fcntl(p[0], F_SETPIPE_SZ, PIPE_BYTES) < 0) { die("pipe size"); return; }
    pid_t pid = fork();
    if (pid < 0) { die("fork"); return; }
    if (!pid) {
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(p[1], 1);
        signal(SIGPIPE, SIG_DFL);
        execl(capture, capture, name, fps_s, (char *) NULL);
        _exit(127);
    }
    close(p[1]);
    c->pid = pid;
    c->fd = p[0];
    c->born = now();
    snprintf(c->name, sizeof c->name, "%s", name);
}

static void end(struct cap *c) {
    if (c->pid > 0) kill(-c->pid, SIGTERM);
    if (c->fd >= 0) close(c->fd);
    c->pid = c->fd = -1;
    c->name[0] = 0;
}

/* One whole frame, known to be in FD's pipe, to stdout; a second reference to
 * its pages kept for repeating. */
static int deliver(int fd) {
    while (in_pipe(hold[0])) {
        if (splice(hold[0], NULL, devnull, NULL, frame, SPLICE_F_NONBLOCK) <= 0) break;
    }
    held = tee(fd, hold[1], frame, 0) == (ssize_t) frame;
    for (size_t left = frame; left; ) {
        ssize_t r = splice(fd, NULL, 1, NULL, left, SPLICE_F_MOVE);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { die("stdout"); return -1; }
        left -= (size_t) r;
    }
    if (repeats) { fprintf(stderr, "glass-monitor: the last picture repeated %lu times until this frame\n", repeats); repeats = 0; }
    last_out = now();
    return 0;
}

/* The last frame again, only into an empty pipe: tee() cannot continue a
 * partial frame, and into an empty pipe of 256 pages all 85 go at once. */
static void repeat(void) {
    if (!held || in_pipe(1)) return;
    ssize_t r = tee(hold[0], 1, frame, 0);
    if (r == (ssize_t) frame) { last_out = now(); repeats++; }
    else if (r > 0) { errno = EPIPE; die("a partial frame repeated"); }
    else if (r < 0 && errno != EINTR) die("stdout");
}

static void take_command(int ctl) {
    char b[256];
    ssize_t n = read(ctl, b, sizeof b - 1);
    if (n <= 0) return;
    b[n] = 0;
    while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == ' ')) b[--n] = 0;
    char *line = strrchr(b, '\n');     /* several queued: the last counts */
    line = line ? line + 1 : b;
    if (!line[0] || strlen(line) >= sizeof want) return;
    if (!strcmp(line, want)) return;
    snprintf(want, sizeof want, "%s", line);
    end(&nxt);
    if (strcmp(want, cur.name)) spawn(&nxt, want);
    fprintf(stderr, "glass-monitor: %s asked for\n", want);
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: glass-monitor W H FPS MONITOR CONTROL CAPTURE\n");
        return 2;
    }
    frame = (size_t) atoi(argv[1]) * (size_t) atoi(argv[2]) * 3 / 2;
    fps_s = argv[3];
    snprintf(want, sizeof want, "%s", argv[4]);
    const char *ctlpath = argv[5];
    capture = argv[6];

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (fcntl(1, F_SETPIPE_SZ, PIPE_BYTES) < 0) { perror("glass-monitor: stdout is not a pipe"); return 1; }
    if (pipe2(hold, O_CLOEXEC) || fcntl(hold[0], F_SETPIPE_SZ, PIPE_BYTES) < 0) { perror("glass-monitor: pipe"); return 1; }
    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    unlink(ctlpath);
    if (mkfifo(ctlpath, 0600)) { perror("glass-monitor: control"); return 1; }
    int ctl = open(ctlpath, O_RDWR | O_NONBLOCK | O_CLOEXEC);   /* RDWR: never an end of file */
    if (ctl < 0 || devnull < 0) { perror("glass-monitor: open"); return 1; }

    spawn(&cur, want);
    last_out = now();
    while (!stop) {
        struct pollfd p[3] = { { ctl, POLLIN, 0 }, { cur.fd, POLLIN, 0 }, { nxt.fd, POLLIN, 0 } };
        double t = now(), left = last_out + REPEAT_S - t;
        int wait_ms = left > 0.001 ? (int) (left * 1e3) : 1;
        if (poll(p, 3, wait_ms) < 0 && errno != EINTR) { die("poll"); break; }

        if (p[0].revents & POLLIN) take_command(ctl);

        /* The new one's first whole frame ends the old one. */
        if (nxt.fd >= 0 && p[2].revents) {
            if (in_pipe(nxt.fd) >= frame) {
                double ms = (now() - nxt.born) * 1e3;
                end(&cur);
                cur = nxt;
                nxt.pid = nxt.fd = -1;
                nxt.name[0] = 0;
                fprintf(stderr, "glass-monitor: %s on screen, %.0f ms after it was asked for\n", cur.name, ms);
                p[1].fd = cur.fd;
                p[1].revents = POLLIN;
            } else if (p[2].revents & POLLHUP) {
                fprintf(stderr, "glass-monitor: the capture of %s ended before a frame: staying on %s\n", nxt.name, cur.name);
                end(&nxt);
                snprintf(want, sizeof want, "%s", cur.name);
            }
        }
        if (nxt.fd >= 0 && now() - nxt.born > SWITCH_TIMEOUT_S) {
            fprintf(stderr, "glass-monitor: no frame from %s in %.0f s: staying on %s\n", nxt.name, SWITCH_TIMEOUT_S, cur.name);
            end(&nxt);
            snprintf(want, sizeof want, "%s", cur.name);
        }

        if (cur.fd >= 0 && p[1].revents) {
            int moved = 0;
            while (in_pipe(cur.fd) >= frame && !stop) {
                if (deliver(cur.fd)) break;
                moved = 1;
            }
            if (!moved && (p[1].revents & POLLHUP) && in_pipe(cur.fd) < frame) {
                fprintf(stderr, "glass-monitor: the capture of %s ended: again in %.0f s, the last picture held\n", cur.name, RESTART_S);
                snprintf(want, sizeof want, "%s", cur.name);
                end(&cur);
                restart_at = now() + RESTART_S;
            } else if (!moved) {
                usleep(500);           /* a frame on its way into the pipe */
            }
        }
        if (cur.fd < 0 && nxt.fd < 0 && now() >= restart_at) spawn(&cur, want);
        while (waitpid(-1, NULL, WNOHANG) > 0) { }
        if (now() - last_out >= REPEAT_S) repeat();
    }
    end(&cur);
    end(&nxt);
    unlink(ctlpath);
    fprintf(stderr, "glass-monitor: closed\n");
    return 0;
}
