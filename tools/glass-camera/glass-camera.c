/* glass-camera: the Glass's camera through the Ducati (the OMAP4's
 * Cortex-M3 cores running Google's firmware) without Android: TI's DOMX
 * wire, OMX over rpmsg, spoken directly in C. The protocol, the ioctls and
 * the sources they were read from: docs/ducati-omx.md.
 *
 *   glass-camera -i              what the camera and the encoder offer
 *   glass-camera [-w W] [-h H] [-r FPS] [-k KBIT/S] [-g GOP_S] [-n FRAMES] [-b BUFFERS] > video.h264
 *   glass-camera -y [-w W] [-h H] [-r FPS] [-n FRAMES] > frames.nv12
 *
 * Defaults 960x540 (half the camera's 1920x1080 video size), 15 frames/s,
 * 768 kbit/s, a key frame every 2 s. The camera component
 * (OMX.TI.DUCATI1.VIDEO.CAMERA, the OV5680, in video mode) fills TILER 2D
 * NV12 buffers on its preview port (2): Y in an 8-bit container, UV in a
 * 16-bit one, allocated here from ION and registered on each component's
 * rpmsg-omx connection, which turns a handle into the Ducati's address.
 * The same buffers are the H.264 encoder's (OMX.TI.DUCATI1.VIDEO.H264E)
 * input: a filled camera buffer goes to the encoder as it is
 * (EmptyThisBuffer) and back to the camera when the encoder is done
 * (EmptyBufferDone), so no picture is copied; the encoder's output, Annex-B
 * H.264 (Constrained Baseline, the parameter sets first), goes to stdout as
 * it comes. -y writes the pictures instead, as packed NV12 (W x H luma,
 * then W x H/2 interleaved chroma), no encoder. Until FRAMES (0: no end), a
 * signal or a closed stdout; a line on stderr every 5 s (frames, rate,
 * kbit/s). Both components are taken back to Loaded and freed on the way
 * out, so the next run starts clean; a run still holding the camera is
 * stopped first. The Ducati's own messages are in
 * /sys/kernel/debug/remoteproc/remoteproc0/trace1.
 *
 * Compiled on the Glass: gcc -O2 -Iomx -o glass-camera glass-camera.c -lpthread
 * (the omx/ headers are Khronos OMX 1.1 and TI's extensions, from
 * hardware/ti/omap4xxx).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <OMX_Core.h>
#include <OMX_Component.h>
#include <OMX_TI_Index.h>
#include <OMX_TI_Common.h>
#include <OMX_TI_IVCommon.h>

/* The factory kernel's ION and rpmsg-omx structures (include/linux/ion.h,
 * omap_ion.h, rpmsg_omx.h); their sizes are part of the ioctl numbers. */
struct k_ion_fd { uint32_t handle; int32_t fd; unsigned char cacheable; };
struct k_ion_handle { uint32_t handle; };
struct k_ion_custom { uint32_t cmd; uint32_t arg; };
struct k_tiler_alloc { uint32_t w, h; int32_t fmt; uint32_t flags, handle, stride, offset, out_align, token; };
_Static_assert(sizeof(struct k_ion_fd) == 12, "struct ion_fd_data is 12 bytes");
_Static_assert(sizeof(struct k_tiler_alloc) == 36, "struct omap_ion_tiler_alloc_data is 36 bytes");
#define ION_IOC_FREE _IOWR('I', 1, struct k_ion_handle)
#define ION_IOC_SHARE _IOWR('I', 4, struct k_ion_fd)
#define ION_IOC_CUSTOM _IOWR('I', 6, struct k_ion_custom)
#define OMAP_ION_TILER_ALLOC 0
#define TILFMT_8BIT 0
#define TILFMT_16BIT 1
#define TILFMT_PAGE 3
#define OMX_IOCCONNECT _IOW('X', 1, char *)
#define OMX_IOCIONREGISTER _IOWR('X', 2, struct k_ion_fd)
#define OMX_IOCIONUNREGISTER _IOWR('X', 3, struct k_ion_fd)

/* DOMX: one 240-byte packet per call (omx_rpc_stub.c). */
#define PKT 240
#define HDR 20
enum {
    F_GET_HANDLE = 0, F_SET_PARAM = 1, F_GET_PARAM = 2, F_USE_BUFFER = 3, F_FREE_HANDLE = 4,
    F_SET_CONFIG = 5, F_GET_CONFIG = 6, F_GET_STATE = 7, F_SEND_CMD = 8,
    F_FILL = 11, F_FILL_DONE = 12, F_FREE_BUFFER = 13, F_EMPTY = 14, F_EMPTY_DONE = 15, F_EVENT = 16,
};
#define NO_REPLY 0xffffffffu      /* the transport failed: no OMX result at all */

#define PORT_PREVIEW 2

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void set32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

struct pkt { uint8_t b[PKT]; size_t pos; };
static void pkt_init(struct pkt *p, uint32_t fn) {
    memset(p, 0, sizeof *p);
    p->b[1] = 0x01;                           /* desc: OMX_DESC_MSG << 8 */
    set32(p->b + 4, 0x8000);                  /* flags: OMX_POOLID_JOBID_DEFAULT */
    set32(p->b + 8, fn | 0x80000000u);        /* a function of the static table */
    set32(p->b + 16, PKT);
}
static void put(struct pkt *p, const void *v, size_t n) {
    if (HDR + p->pos + n > PKT) { fprintf(stderr, "glass-camera: packet overflow\n"); exit(1); }
    memcpy(p->b + HDR + p->pos, v, n);
    p->pos += n;
}
static void put32(struct pkt *p, uint32_t v) { put(p, &v, 4); }

/* One OMX component: its own rpmsg-omx open (its own ION client), a
 * reader thread that sorts what arrives into the reply slot (only one call
 * is ever in flight), the event queue and the done queue. */
struct event { uint32_t ev, d1, d2, data; };
struct done { uint32_t hdr, len, off, flags; int64_t ts; };
struct comp {
    const char *name;
    int fd, dead;
    uint32_t h;                       /* the remote handle, in every call */
    pthread_t reader;
    uint8_t reply[PKT];
    int replied;
    struct event evq[64];
    int nev;
    struct done dq[64];
    int ndone;
};
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static volatile sig_atomic_t stop;

static const char *ev_name(uint32_t e) {
    switch (e) {
    case OMX_EventCmdComplete: return "command complete";
    case OMX_EventError: return "error";
    case OMX_EventMark: return "mark";
    case OMX_EventPortSettingsChanged: return "port settings changed";
    case OMX_EventBufferFlag: return "buffer flag";
    default: return "event";
    }
}

static void *reader(void *arg) {
    struct comp *c = arg;
    uint8_t b[PKT];
    for (;;) {
        ssize_t n = read(c->fd, b, sizeof b);
        if (n < 0 && errno == EINTR) continue;
        pthread_mutex_lock(&mu);
        if (n < HDR) {
            c->dead = n < 0 ? errno : EIO;
            fprintf(stderr, "glass-camera: %s: connection lost (%s)\n", c->name, strerror(c->dead));
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
            return NULL;
        }
        uint32_t fn = get32(b + 8) & 0x0fffffffu;
        const uint8_t *d = b + HDR;
        if (fn == F_EVENT) {
            struct event e = { get32(d + 4), get32(d + 8), get32(d + 12), get32(d + 16) };
            fprintf(stderr, "glass-camera: %s: %s %u (0x%x, 0x%x)\n", c->name, ev_name(e.ev), e.ev, e.d1, e.d2);
            if (c->nev == 64) { memmove(c->evq, c->evq + 1, sizeof c->evq[0] * 63); c->nev--; }
            c->evq[c->nev++] = e;
        } else if (fn == F_FILL_DONE || fn == F_EMPTY_DONE) {
            struct done x = { get32(d + 4), get32(d + 8), get32(d + 12), get32(d + 16), 0 };
            if (fn == F_FILL_DONE) memcpy(&x.ts, d + 20, 8);
            else x.flags |= 0x80000000u;    /* marks an EmptyBufferDone */
            if (c->ndone == 64) fprintf(stderr, "glass-camera: %s: done queue full, one lost\n", c->name);
            else c->dq[c->ndone++] = x;
        } else {
            memcpy(c->reply, b, (size_t) n);
            c->replied = 1;
        }
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
}

static struct timespec deadline(int ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long) (ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    return t;
}

/* Sends P and waits for its reply (5 s); returns the OMX result, the reply
 * in R. */
static uint32_t call(struct comp *c, struct pkt *p, uint8_t *r) {
    pthread_mutex_lock(&mu);
    c->replied = 0;
    pthread_mutex_unlock(&mu);
    if (write(c->fd, p->b, PKT) != PKT) {
        fprintf(stderr, "glass-camera: %s: write: %s\n", c->name, strerror(errno));
        return NO_REPLY;
    }
    struct timespec t = deadline(5000);
    pthread_mutex_lock(&mu);
    while (!c->replied && !c->dead)
        if (pthread_cond_timedwait(&cv, &mu, &t) == ETIMEDOUT) break;
    int ok = c->replied;
    if (ok) memcpy(r, c->reply, PKT);
    pthread_mutex_unlock(&mu);
    if (!ok) {
        fprintf(stderr, "glass-camera: %s: no reply to function %u\n", c->name, get32(p->b + 8) & 0xffff);
        return NO_REPLY;
    }
    return get32(r + 12);
}

static int open_comp(struct comp *c, const char *name) {
    memset(c, 0, sizeof *c);
    c->name = name;
    c->fd = open("/dev/rpmsg-omx1", O_RDWR);
    if (c->fd < 0) { fprintf(stderr, "glass-camera: /dev/rpmsg-omx1: %s (is the Ducati up?)\n", strerror(errno)); return -1; }
    char conn[48] = "OMX";
    if (ioctl(c->fd, OMX_IOCCONNECT, conn) < 0) {
        fprintf(stderr, "glass-camera: connect to the OMX service: %s\n", strerror(errno));
        close(c->fd);
        return -1;
    }
    if (pthread_create(&c->reader, NULL, reader, c)) { close(c->fd); return -1; }
    struct pkt p;
    uint8_t r[PKT];
    char n[128] = { 0 };
    strncpy(n, name, sizeof n - 1);
    pkt_init(&p, F_GET_HANDLE);
    put32(&p, 0);
    put32(&p, 0);
    put(&p, n, sizeof n);
    put32(&p, (uint32_t) (uintptr_t) c);  /* pAppData: comes back in every callback */
    uint32_t e = call(c, &p, r);
    if (e) { fprintf(stderr, "glass-camera: GetHandle %s: 0x%x\n", name, e); return -1; }
    c->h = get32(r + HDR + 140);
    return 0;
}

/* Set or Get, Parameter or Config: S begins with its nSize. MAPOFF is the
 * offset within S of a buffer field the kernel must translate, or -1. */
static uint32_t xparam(struct comp *c, int fn, uint32_t index, void *s, int mapoff) {
    struct pkt p;
    uint8_t r[PKT];
    uint32_t size = *(uint32_t *) s;
    pkt_init(&p, (uint32_t) fn);
    put32(&p, mapoff >= 0 ? 1 : 0);
    put32(&p, mapoff >= 0 ? (uint32_t) (16 + mapoff) : 0);
    put32(&p, c->h);
    put32(&p, index);
    put(&p, s, size);
    uint32_t e = call(c, &p, r);
    if (!e && (fn == F_GET_PARAM || fn == F_GET_CONFIG)) memcpy(s, r + HDR + 16, size);
    return e;
}

static uint32_t send_cmd(struct comp *c, OMX_COMMANDTYPE cmd, uint32_t param) {
    struct pkt p;
    uint8_t r[PKT];
    pkt_init(&p, F_SEND_CMD);
    put32(&p, 0);
    put32(&p, 0);
    put32(&p, c->h);
    put32(&p, cmd);
    put32(&p, param);
    return call(c, &p, r);
}

static uint32_t get_state(struct comp *c, uint32_t *state) {
    struct pkt p;
    uint8_t r[PKT];
    pkt_init(&p, F_GET_STATE);
    put32(&p, 0);
    put32(&p, 0);
    put32(&p, c->h);
    uint32_t e = call(c, &p, r);
    *state = e ? 0 : get32(r + HDR + 12);
    return e;
}

static void free_handle(struct comp *c) {
    struct pkt p;
    uint8_t r[PKT];
    pkt_init(&p, F_FREE_HANDLE);
    put32(&p, 0);
    put32(&p, 0);
    put32(&p, c->h);
    uint32_t e = call(c, &p, r);
    if (e) fprintf(stderr, "glass-camera: FreeHandle: 0x%x\n", e);
}

/* Waits up to MS for the event (EV, D1, D2) and takes it from the queue;
 * an error event meanwhile ends the wait. 0 when seen. */
static int wait_event(struct comp *c, uint32_t ev, uint32_t d1, uint32_t d2, int ms) {
    struct timespec t = deadline(ms);
    pthread_mutex_lock(&mu);
    for (;;) {
        for (int i = 0; i < c->nev; i++) {
            struct event e = c->evq[i];
            if ((e.ev == ev && e.d1 == d1 && e.d2 == d2) || e.ev == OMX_EventError) {
                memmove(c->evq + i, c->evq + i + 1, sizeof c->evq[0] * (size_t) (c->nev - i - 1));
                c->nev--;
                pthread_mutex_unlock(&mu);
                return e.ev == OMX_EventError ? -1 : 0;
            }
        }
        if (c->dead || pthread_cond_timedwait(&cv, &mu, &t) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "glass-camera: %s: no %s (0x%x, 0x%x) within %d ms\n", c->name, ev_name(ev), d1, d2, ms);
    return -1;
}

static int state_to(struct comp *c, OMX_STATETYPE s, int ms) {
    uint32_t e = send_cmd(c, OMX_CommandStateSet, s);
    if (e) { fprintf(stderr, "glass-camera: StateSet %d: 0x%x\n", s, e); return -1; }
    return wait_event(c, OMX_EventCmdComplete, OMX_CommandStateSet, s, ms);
}


/* A TILER buffer: allocated in our ION client, shared as an fd, mapped
 * here, and registered on the connection of each component that uses it
 * (reg): the handle a connection gives back is what that component's
 * Ducati address is found from. */
struct tb { uint32_t ion, stride, size; int share; uint8_t *map; };
static int tiler(int ion, uint32_t w, uint32_t h, int fmt, struct tb *t) {
    memset(t, 0, sizeof *t);
    t->share = -1;
    struct k_tiler_alloc a = { .w = w, .h = h, .fmt = fmt };
    struct k_ion_custom cu = { OMAP_ION_TILER_ALLOC, (uint32_t) (uintptr_t) &a };
    if (ioctl(ion, ION_IOC_CUSTOM, &cu) < 0) {
        fprintf(stderr, "glass-camera: TILER %ux%u (format %d): %s\n", w, h, fmt, strerror(errno));
        return -1;
    }
    t->ion = a.handle;
    t->stride = a.stride;
    t->size = fmt == TILFMT_PAGE ? (w + 4095) & ~4095u : a.stride * h;
    struct k_ion_fd s = { .handle = t->ion };
    if (ioctl(ion, ION_IOC_SHARE, &s) < 0) { perror("glass-camera: ION share"); return -1; }
    t->share = s.fd;
    t->map = mmap(NULL, t->size, PROT_READ | PROT_WRITE, MAP_SHARED, t->share, 0);
    if (t->map == MAP_FAILED) { t->map = NULL; perror("glass-camera: mmap"); return -1; }
    return 0;
}
static uint32_t reg(struct comp *c, struct tb *t) {
    struct k_ion_fd g = { .fd = t->share };
    if (ioctl(c->fd, OMX_IOCIONREGISTER, &g) < 0) { perror("glass-camera: register with rpmsg-omx"); return 0; }
    return g.handle;
}
static void unreg(struct comp *c, uint32_t h) {
    if (!h || c->fd <= 0) return;
    struct k_ion_fd g = { .handle = h };
    ioctl(c->fd, OMX_IOCIONUNREGISTER, &g);
}
static void tiler_free(int ion, struct tb *t) {
    if (t->map) munmap(t->map, t->size);
    if (t->share >= 0) close(t->share);
    if (t->ion) { struct k_ion_handle f = { t->ion }; ioctl(ion, ION_IOC_FREE, &f); }
    memset(t, 0, sizeof *t);
    t->share = -1;
}

/* One buffer on one port, as UseBuffer answered: the remote header (named
 * in every later call) and its fields. */
struct hdr { uint32_t remote, alloc_len, in_port, out_port, b0; };

static uint32_t use_buffer(struct comp *c, uint32_t port, uint32_t size, uint32_t b0, uint32_t b1, struct hdr *o) {
    struct pkt p;
    uint8_t r[PKT];
    uint32_t nb = b1 ? 2 : 1;
    pkt_init(&p, F_USE_BUFFER);
    put32(&p, nb);
    put32(&p, 24);                   /* the buffers' offset within the data */
    put32(&p, c->h);
    put32(&p, port);
    put32(&p, 0);                    /* pAppPrivate */
    put32(&p, size);
    put32(&p, b0);
    if (b1) put32(&p, b1);
    uint32_t e = call(c, &p, r);
    if (e) return e;
    /* the remote header, then nSize, nVersion, nAllocLen, nFilledLen,
     * nOffset, pAppPrivate, pInputPortPrivate, pOutputPortPrivate,
     * hMarkTargetComponent, pMarkData, nTickCount, nTimeStamp (8), nFlags,
     * nInputPortIndex, nOutputPortIndex */
    const uint8_t *d = r + HDR + 24 + 4 * nb;
    o->remote = get32(d);
    o->alloc_len = get32(d + 12);
    o->in_port = get32(d + 60);
    o->out_port = get32(d + 64);
    o->b0 = b0;
    return 0;
}

static uint32_t fill_this(struct comp *c, struct hdr *o) {
    struct pkt p;
    uint8_t r[PKT];
    pkt_init(&p, F_FILL);
    put32(&p, 0);
    put32(&p, 0);
    put32(&p, c->h);
    put32(&p, o->remote);
    put32(&p, 0);                    /* nFilledLen */
    put32(&p, 0);                    /* nOffset */
    put32(&p, 0);                    /* nFlags */
    put32(&p, o->alloc_len);
    put32(&p, o->out_port);
    put32(&p, o->in_port);
    return call(c, &p, r);
}

static uint32_t empty_this(struct comp *c, struct hdr *o, uint32_t len, uint32_t off, uint32_t flags, int64_t ts) {
    struct pkt p;
    uint8_t r[PKT];
    pkt_init(&p, F_EMPTY);
    put32(&p, 0);
    put32(&p, 0);
    put32(&p, c->h);
    put32(&p, o->remote);
    put32(&p, len);
    put32(&p, off);
    put32(&p, flags);
    put(&p, &ts, 8);                 /* nTimeStamp */
    put32(&p, 0);                    /* hMarkTargetComponent */
    put32(&p, 0);                    /* pMarkData */
    put32(&p, o->alloc_len);
    put32(&p, o->out_port);
    put32(&p, o->in_port);
    return call(c, &p, r);
}

static uint32_t free_buffer(struct comp *c, uint32_t port, struct hdr *o) {
    struct pkt p;
    uint8_t r[PKT];
    pkt_init(&p, F_FREE_BUFFER);
    put32(&p, 0);
    put32(&p, 0);
    put32(&p, c->h);
    put32(&p, port);
    put32(&p, o->remote);
    put32(&p, o->b0);
    return call(c, &p, r);
}

#define INIT(x) do { memset(&(x), 0, sizeof (x)); (x).nSize = sizeof (x); \
    (x).nVersion.s.nVersionMajor = 1; (x).nVersion.s.nVersionMinor = 1; } while (0)

static void show_port(struct comp *c, uint32_t port) {
    OMX_PARAM_PORTDEFINITIONTYPE d;
    INIT(d);
    d.nPortIndex = port;
    uint32_t e = xparam(c, F_GET_PARAM, OMX_IndexParamPortDefinition, &d, -1);
    if (e) { fprintf(stderr, "glass-camera: %s port %u: 0x%x\n", c->name, port, e); return; }
    printf("%s port %u: %s, %s, %ux%u stride %d, colour 0x%x, coding %d, %.2f/s, %u bit/s, buffers %u (at least %u) of %u bytes\n",
           c->name, port, d.eDir == OMX_DirOutput ? "out" : "in", d.bEnabled ? "enabled" : "disabled",
           (unsigned) d.format.video.nFrameWidth, (unsigned) d.format.video.nFrameHeight, (int) d.format.video.nStride,
           d.format.video.eColorFormat, d.format.video.eCompressionFormat, d.format.video.xFramerate / 65536.0,
           (unsigned) d.format.video.nBitrate,
           (unsigned) d.nBufferCountActual, (unsigned) d.nBufferCountMin, (unsigned) d.nBufferSize);
}

static void capabilities(int ion, struct comp *c) {
    struct tb t;
    if (tiler(ion, sizeof (OMX_TI_CAPTYPE), 1, TILFMT_PAGE, &t)) return;
    uint32_t h = reg(c, &t);
    memset(t.map, 0, t.size);
    OMX_TI_CONFIG_SHAREDBUFFER s;
    INIT(s);
    s.nPortIndex = OMX_ALL;
    s.nSharedBuffSize = sizeof (OMX_TI_CAPTYPE);
    s.pSharedBuff = (OMX_U8 *) (uintptr_t) h;
    uint32_t e = h ? xparam(c, F_GET_CONFIG, OMX_TI_IndexConfigCamCapabilities, &s,
                            (int) offsetof(OMX_TI_CONFIG_SHAREDBUFFER, pSharedBuff)) : NO_REPLY;
    if (e) fprintf(stderr, "glass-camera: capabilities: 0x%x\n", e);
    else {
        /* The Glass firmware's OMX_TI_CAPTYPE is not KitKat's (its fields
         * sit elsewhere), so the size ranges are found as they are: every
         * resolution record (nSize 28, a version, a port, then min and
         * max width and height). */
        const uint8_t *m = t.map;
        for (uint32_t o = 0; o + 28 <= t.size; o += 2) {
            if (get32(m + o) != 28 || m[o + 4] != 1) continue;
            const uint8_t *q = m + o + 12;
            printf("sizes at %u: %ux%u to %ux%u\n", o, get32(q), get32(q + 4), get32(q + 8), get32(q + 12));
        }
    }
    unreg(c, h);
    tiler_free(ion, &t);
}

/* Takes the camera from an earlier run still holding it (one owner at a
 * time; a session cut by a network change leaves its process behind). */
static void takeover(void) {
    char me[16];
    snprintf(me, sizeof me, "%d", (int) getpid());
    for (int round = 0; round < 2; round++) {
        int found = 0;
        DIR *d = opendir("/proc");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9' || !strcmp(e->d_name, me)) continue;
            char path[64], comm[32] = { 0 };
            snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
            int f = open(path, O_RDONLY);
            if (f < 0) continue;
            ssize_t n = read(f, comm, sizeof comm - 1);
            close(f);
            if (n <= 0 || strncmp(comm, "glass-camera\n", 13)) continue;
            found = 1;
            kill(atoi(e->d_name), round ? SIGKILL : SIGTERM);
        }
        if (d) closedir(d);
        if (!found) return;
        fprintf(stderr, "glass-camera: an earlier run had the camera: %s\n", round ? "killed" : "stopped");
        for (int i = 0; i < 30; i++) usleep(100000);
    }
}

/* Whatever either component has finished with, waiting up to MS. */
static int next_done(struct comp **cs, int n, struct done *d, struct comp **from, int ms) {
    struct timespec t = deadline(ms);
    pthread_mutex_lock(&mu);
    for (;;) {
        for (int i = 0; i < n; i++) {
            struct comp *c = cs[i];
            if (c->ndone) {
                *d = c->dq[0];
                memmove(c->dq, c->dq + 1, sizeof c->dq[0] * (size_t) --c->ndone);
                *from = c;
                pthread_mutex_unlock(&mu);
                return 0;
            }
            if (c->dead) { pthread_mutex_unlock(&mu); return -1; }
        }
        if (stop || pthread_cond_timedwait(&cv, &mu, &t) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&mu);
    return 1;
}

/* A camera buffer: Y and UV, registered on the camera and on the encoder,
 * with the header each of them gave it. */
struct frame {
    struct tb y, uv;
    uint32_t cy, cuv, ey, euv;
    struct hdr cam, enc;
    int at_enc;                       /* in the encoder, not the camera */
};
/* An encoder output (bitstream) buffer. */
struct bits { struct tb b; uint32_t h; struct hdr enc; };

#define PORT_ENC_IN 0
#define PORT_ENC_OUT 1
#define FLAG_EOF 0x10                 /* OMX_BUFFERFLAG_ENDOFFRAME */
#define FLAG_CONFIG 0x80              /* OMX_BUFFERFLAG_CODECCONFIG */

static void on_signal(int s) { (void) s; stop = 1; }

int main(int argc, char **argv) {
    unsigned w = 960, h = 540, fps = 15, nframes = 0, nbuf = 4, kbps = 768, gop = 2;
    int info = 0, raw = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i")) info = 1;
        else if (!strcmp(argv[i], "-y")) raw = 1;
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) w = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) h = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) kbps = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gop = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nframes = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) nbuf = (unsigned) atoi(argv[++i]);
        else {
            fprintf(stderr, "glass-camera -i\n"
                            "glass-camera [-w W] [-h H] [-r FPS] [-k KBIT/S] [-g GOP_S] [-n FRAMES] [-b BUFFERS] > video.h264\n"
                            "glass-camera -y [-w W] [-h H] [-r FPS] [-n FRAMES] > frames.nv12\n");
            return 1;
        }
    }
    if (w < 16 || h < 16 || w > 1920 || h > 1088 || (w | h) & 1 || !fps || fps > 30 || nbuf < 2 || nbuf > 16 ||
        kbps < 64 || kbps > 20000 || !gop) {
        fprintf(stderr, "glass-camera: even sizes 16x16 to 1920x1088, 1 to 30 frames/s, 64 to 20000 kbit/s, 2 to 16 buffers\n");
        return 1;
    }
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    takeover();

    int ion = open("/dev/ion", O_RDWR);
    if (ion < 0) { perror("glass-camera: /dev/ion"); return 1; }
    struct comp cam, enc;
    memset(&enc, 0, sizeof enc);
    enc.fd = -1;
    if (open_comp(&cam, "OMX.TI.DUCATI1.VIDEO.CAMERA")) return 1;
    int coding = !raw;
    if ((coding || info) && open_comp(&enc, "OMX.TI.DUCATI1.VIDEO.H264E")) return 1;
    uint32_t st0 = 0;
    get_state(&cam, &st0);
    fprintf(stderr, "glass-camera: camera open (state %u)%s\n", st0, enc.h ? ", encoder open" : "");

    if (info) {
        for (uint32_t p = 0; p < 6; p++) show_port(&cam, p);
        capabilities(ion, &cam);
        show_port(&enc, PORT_ENC_IN);
        show_port(&enc, PORT_ENC_OUT);
        free_handle(&cam);
        free_handle(&enc);
        return 0;
    }

    int rc = 1, cam_idle = 0, cam_exec = 0, enc_idle = 0, enc_exec = 0;
    struct frame fr[16];
    struct bits out[16];
    unsigned nout = 0;
    memset(fr, 0, sizeof fr);
    memset(out, 0, sizeof out);
    for (unsigned i = 0; i < 16; i++) fr[i].y.share = fr[i].uv.share = out[i].b.share = -1;
    uint32_t e;

    /* The camera: the preview port only, the primary sensor, video mode
     * (the HAL's VIDEO_MODE: sizes up to 1920x1080), a fixed rate. */
    send_cmd(&cam, OMX_CommandPortDisable, OMX_ALL);
    wait_event(&cam, OMX_EventCmdComplete, OMX_CommandPortDisable, PORT_PREVIEW, 2000);
    if ((e = send_cmd(&cam, OMX_CommandPortEnable, PORT_PREVIEW))) { fprintf(stderr, "glass-camera: PortEnable: 0x%x\n", e); goto out; }
    if (wait_event(&cam, OMX_EventCmdComplete, OMX_CommandPortEnable, PORT_PREVIEW, 3000)) goto out;
    OMX_CONFIG_SENSORSELECTTYPE sel;
    INIT(sel);
    sel.nPortIndex = OMX_ALL;
    sel.eSensor = OMX_PrimarySensor;
    if ((e = xparam(&cam, F_SET_CONFIG, OMX_TI_IndexConfigSensorSelect, &sel, -1)))
        fprintf(stderr, "glass-camera: SensorSelect: 0x%x\n", e);
    OMX_CONFIG_CAMOPERATINGMODETYPE mode;
    INIT(mode);
    mode.eCamOperatingMode = OMX_CaptureVideo;
    if ((e = xparam(&cam, F_SET_PARAM, OMX_IndexCameraOperatingMode, &mode, -1)))
        fprintf(stderr, "glass-camera: operating mode: 0x%x\n", e);

    OMX_PARAM_PORTDEFINITIONTYPE pd;
    INIT(pd);
    pd.nPortIndex = PORT_PREVIEW;
    if ((e = xparam(&cam, F_GET_PARAM, OMX_IndexParamPortDefinition, &pd, -1))) { fprintf(stderr, "glass-camera: port definition: 0x%x\n", e); goto out; }
    pd.format.video.nFrameWidth = w;
    pd.format.video.nFrameHeight = h;
    pd.format.video.nStride = 4096;      /* TILER 2D */
    pd.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedSemiPlanar;   /* NV12 as the encoder takes it (0x27) */
    pd.format.video.xFramerate = fps << 16;
    pd.nBufferSize = 4096 * h * 3 / 2;
    if (nbuf < pd.nBufferCountMin) nbuf = pd.nBufferCountMin;
    pd.nBufferCountActual = nbuf;
    if ((e = xparam(&cam, F_SET_PARAM, OMX_IndexParamPortDefinition, &pd, -1))) { fprintf(stderr, "glass-camera: set port definition: 0x%x\n", e); goto out; }
    xparam(&cam, F_GET_PARAM, OMX_IndexParamPortDefinition, &pd, -1);
    OMX_TI_CONFIG_VARFRMRANGETYPE vfr;
    INIT(vfr);
    vfr.xMin = vfr.xMax = fps << 16;
    if ((e = xparam(&cam, F_SET_CONFIG, OMX_TI_IndexConfigVarFrmRange, &vfr, -1)))
        fprintf(stderr, "glass-camera: frame rate range: 0x%x\n", e);
    fprintf(stderr, "glass-camera: camera %ux%u stride %d, %.2f/s, %u buffers of %u bytes\n",
            (unsigned) pd.format.video.nFrameWidth, (unsigned) pd.format.video.nFrameHeight, (int) pd.format.video.nStride,
            pd.format.video.xFramerate / 65536.0, (unsigned) pd.nBufferCountActual, (unsigned) pd.nBufferSize);

    /* What to allocate: the frame, or more when the component pads it. */
    uint32_t aw = w, ah = h;
    OMX_CONFIG_RECTTYPE dim;
    INIT(dim);
    dim.nPortIndex = PORT_PREVIEW;
    if (!xparam(&cam, F_GET_PARAM, OMX_TI_IndexParam2DBufferAllocDimension, &dim, -1) && dim.nWidth && dim.nHeight) {
        aw = dim.nWidth;
        ah = dim.nHeight;
    }
    for (unsigned i = 0; i < nbuf; i++) {
        if (tiler(ion, aw, ah, TILFMT_8BIT, &fr[i].y) || tiler(ion, aw / 2, ah / 2, TILFMT_16BIT, &fr[i].uv)) goto out;
        if (!(fr[i].cy = reg(&cam, &fr[i].y)) || !(fr[i].cuv = reg(&cam, &fr[i].uv))) goto out;
        if (coding && (!(fr[i].ey = reg(&enc, &fr[i].y)) || !(fr[i].euv = reg(&enc, &fr[i].uv)))) goto out;
    }

    /* The encoder: NV12 in from the camera's own buffers (the same TILER
     * memory, registered on its connection too), H.264 out at KBPS. */
    OMX_PARAM_PORTDEFINITIONTYPE ein, eout;
    if (coding) {
        INIT(ein);
        ein.nPortIndex = PORT_ENC_IN;
        if ((e = xparam(&enc, F_GET_PARAM, OMX_IndexParamPortDefinition, &ein, -1))) { fprintf(stderr, "glass-camera: encoder input: 0x%x\n", e); goto out; }
        ein.format.video.nFrameWidth = w;
        ein.format.video.nFrameHeight = h;
        ein.format.video.nStride = 4096;
        ein.format.video.nSliceHeight = h;
        ein.format.video.xFramerate = fps << 16;
        ein.format.video.eColorFormat = pd.format.video.eColorFormat;
        ein.nBufferCountActual = nbuf;
        ein.nBufferSize = pd.nBufferSize;
        if ((e = xparam(&enc, F_SET_PARAM, OMX_IndexParamPortDefinition, &ein, -1))) { fprintf(stderr, "glass-camera: set encoder input: 0x%x\n", e); goto out; }
        xparam(&enc, F_GET_PARAM, OMX_IndexParamPortDefinition, &ein, -1);

        INIT(eout);
        eout.nPortIndex = PORT_ENC_OUT;
        if ((e = xparam(&enc, F_GET_PARAM, OMX_IndexParamPortDefinition, &eout, -1))) { fprintf(stderr, "glass-camera: encoder output: 0x%x\n", e); goto out; }
        eout.format.video.nFrameWidth = w;
        eout.format.video.nFrameHeight = h;
        eout.format.video.nBitrate = kbps * 1000;
        eout.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
        nout = eout.nBufferCountMin > 4 ? eout.nBufferCountMin : 4;
        if (nout > 16) nout = 16;
        eout.nBufferCountActual = nout;
        if ((e = xparam(&enc, F_SET_PARAM, OMX_IndexParamPortDefinition, &eout, -1))) { fprintf(stderr, "glass-camera: set encoder output: 0x%x\n", e); goto out; }
        xparam(&enc, F_GET_PARAM, OMX_IndexParamPortDefinition, &eout, -1);

        OMX_VIDEO_PARAM_BITRATETYPE br;
        INIT(br);
        br.nPortIndex = PORT_ENC_OUT;
        xparam(&enc, F_GET_PARAM, OMX_IndexParamVideoBitrate, &br, -1);
        br.eControlRate = OMX_Video_ControlRateConstant;
        br.nTargetBitrate = kbps * 1000;
        if ((e = xparam(&enc, F_SET_PARAM, OMX_IndexParamVideoBitrate, &br, -1))) {
            br.eControlRate = OMX_Video_ControlRateVariable;
            if ((e = xparam(&enc, F_SET_PARAM, OMX_IndexParamVideoBitrate, &br, -1)))
                fprintf(stderr, "glass-camera: bitrate: 0x%x\n", e);
        }
        OMX_VIDEO_PARAM_AVCTYPE avc;
        INIT(avc);
        avc.nPortIndex = PORT_ENC_OUT;
        if (!xparam(&enc, F_GET_PARAM, OMX_IndexParamVideoAvc, &avc, -1)) {
            avc.eProfile = OMX_VIDEO_AVCProfileBaseline;
            avc.nPFrames = fps * gop - 1;    /* a key frame every GOP seconds */
            avc.nBFrames = 0;
            avc.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;
            avc.bEntropyCodingCABAC = OMX_FALSE;
            if ((e = xparam(&enc, F_SET_PARAM, OMX_IndexParamVideoAvc, &avc, -1)))
                fprintf(stderr, "glass-camera: AVC settings: 0x%x\n", e);
        }
        fprintf(stderr, "glass-camera: encoder %ux%u colour 0x%x in, H.264 %u kbit/s out, %u buffers of %u bytes\n",
                (unsigned) ein.format.video.nFrameWidth, (unsigned) ein.format.video.nFrameHeight, ein.format.video.eColorFormat,
                kbps, nout, (unsigned) eout.nBufferSize);
        for (unsigned i = 0; i < nout; i++) {
            if (tiler(ion, eout.nBufferSize, 1, TILFMT_PAGE, &out[i].b)) goto out;
            if (!(out[i].h = reg(&enc, &out[i].b))) goto out;
        }
    }

    /* Idle: the buffers handed over; then Executing, the encoder first so
     * that it is ready for the camera's first frame. */
    if (coding) {
        if ((e = send_cmd(&enc, OMX_CommandStateSet, OMX_StateIdle))) { fprintf(stderr, "glass-camera: encoder to Idle: 0x%x\n", e); goto out; }
        for (unsigned i = 0; i < nbuf; i++)
            if ((e = use_buffer(&enc, PORT_ENC_IN, ein.nBufferSize, fr[i].ey, fr[i].euv, &fr[i].enc))) { fprintf(stderr, "glass-camera: encoder UseBuffer in %u: 0x%x\n", i, e); goto out; }
        for (unsigned i = 0; i < nout; i++)
            if ((e = use_buffer(&enc, PORT_ENC_OUT, eout.nBufferSize, out[i].h, 0, &out[i].enc))) { fprintf(stderr, "glass-camera: encoder UseBuffer out %u: 0x%x\n", i, e); goto out; }
        if (wait_event(&enc, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateIdle, 5000)) goto out;
        enc_idle = 1;
    }
    if ((e = send_cmd(&cam, OMX_CommandStateSet, OMX_StateIdle))) { fprintf(stderr, "glass-camera: camera to Idle: 0x%x\n", e); goto out; }
    for (unsigned i = 0; i < nbuf; i++)
        if ((e = use_buffer(&cam, PORT_PREVIEW, pd.nBufferSize, fr[i].cy, fr[i].cuv, &fr[i].cam))) { fprintf(stderr, "glass-camera: camera UseBuffer %u: 0x%x\n", i, e); goto out; }
    if (wait_event(&cam, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateIdle, 5000)) goto out;
    cam_idle = 1;
    if (coding) {
        if (state_to(&enc, OMX_StateExecuting, 5000)) goto out;
        enc_exec = 1;
        for (unsigned i = 0; i < nout; i++)
            if ((e = fill_this(&enc, &out[i].enc))) { fprintf(stderr, "glass-camera: encoder FillThisBuffer: 0x%x\n", e); goto out; }
    }
    if (state_to(&cam, OMX_StateExecuting, 5000)) goto out;
    cam_exec = 1;
    for (unsigned i = 0; i < nbuf; i++)
        if ((e = fill_this(&cam, &fr[i].cam))) { fprintf(stderr, "glass-camera: FillThisBuffer: 0x%x\n", e); goto out; }
    fprintf(stderr, "glass-camera: %ux%u at %u/s, %s\n", w, h, fps, coding ? "H.264" : "NV12");

    size_t fsize = (size_t) w * h * 3 / 2;
    uint8_t *pic = raw ? malloc(fsize) : NULL;
    unsigned long frames = 0, empty = 0, shown = 0, bytes = 0, bytes_shown = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double report = 5;
    struct comp *both[2] = { &cam, &enc };
    while (!stop && (!nframes || frames < nframes)) {
        struct done d;
        struct comp *from;
        int got = next_done(both, coding ? 2 : 1, &d, &from, 2000);
        if (got < 0) break;
        if (got > 0) { if (!stop) fprintf(stderr, "glass-camera: nothing for 2 s\n"); continue; }
        if (from == &cam) {
            struct frame *f = NULL;
            for (unsigned i = 0; i < nbuf; i++) if (fr[i].cam.remote == d.hdr) f = &fr[i];
            if (!f) { fprintf(stderr, "glass-camera: a camera buffer not ours: 0x%x\n", d.hdr); continue; }
            if (!d.len) { empty++; e = fill_this(&cam, &f->cam); }
            else if (coding) {
                /* Straight to the encoder; back to the camera when the
                 * encoder is done with it (EmptyBufferDone). */
                e = empty_this(&enc, &f->enc, ein.nBufferSize, d.off, FLAG_EOF, d.ts);
                f->at_enc = 1;
                frames++;
            } else {
                /* The picture starts at nOffset in the 4096-byte rows. */
                uint32_t row = d.off / 4096, col = d.off % 4096;
                for (unsigned y = 0; y < h; y++)
                    memcpy(pic + (size_t) y * w, f->y.map + (size_t) (row + y) * f->y.stride + col, w);
                for (unsigned y = 0; y < h / 2; y++)
                    memcpy(pic + (size_t) w * h + (size_t) y * w, f->uv.map + (size_t) (row / 2 + y) * f->uv.stride + col, w);
                if (fwrite(pic, 1, fsize, stdout) != fsize || fflush(stdout)) stop = 1;
                frames++;
                e = stop ? 0 : fill_this(&cam, &f->cam);
            }
        } else if (d.flags & 0x80000000u) {
            /* EmptyBufferDone: the encoder has read a camera buffer. */
            struct frame *f = NULL;
            for (unsigned i = 0; i < nbuf; i++) if (fr[i].enc.remote == d.hdr) f = &fr[i];
            if (!f) { fprintf(stderr, "glass-camera: an encoder input not ours: 0x%x\n", d.hdr); continue; }
            f->at_enc = 0;
            e = fill_this(&cam, &f->cam);
        } else {
            /* FillBufferDone: H.264 out (the first, flagged CODECCONFIG, is
             * the stream's parameter sets). */
            struct bits *b = NULL;
            for (unsigned i = 0; i < nout; i++) if (out[i].enc.remote == d.hdr) b = &out[i];
            if (!b) { fprintf(stderr, "glass-camera: an encoder output not ours: 0x%x\n", d.hdr); continue; }
            if (d.len && d.off + d.len <= b->b.size) {
                if (fwrite(b->b.map + d.off, 1, d.len, stdout) != d.len || fflush(stdout)) stop = 1;
                bytes += d.len;
            }
            e = stop ? 0 : fill_this(&enc, &b->enc);
        }
        if (e) { fprintf(stderr, "glass-camera: handing a buffer back: 0x%x\n", e); break; }
        struct timespec tn;
        clock_gettime(CLOCK_MONOTONIC, &tn);
        double el = (double) (tn.tv_sec - t0.tv_sec) + (tn.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= report) {
            fprintf(stderr, "glass-camera: %lu frames, %.1f/s, %.0f kbit/s, %lu empty\n",
                    frames, (frames - shown) / 5.0, (bytes - bytes_shown) * 8 / 5000.0, empty);
            shown = frames;
            bytes_shown = bytes;
            report += 5;
        }
    }
    rc = 0;

out:
    /* Back to Loaded, the buffers returned and freed, the handles freed:
     * the next run finds the camera and the encoder as this one did. */
    if (cam_exec && !cam.dead) state_to(&cam, OMX_StateIdle, 5000);
    if (enc_exec && !enc.dead) state_to(&enc, OMX_StateIdle, 5000);
    if (cam_idle && !cam.dead) {
        send_cmd(&cam, OMX_CommandStateSet, OMX_StateLoaded);
        for (unsigned i = 0; i < nbuf; i++)
            if (fr[i].cam.remote) free_buffer(&cam, PORT_PREVIEW, &fr[i].cam);
        wait_event(&cam, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateLoaded, 5000);
    }
    if (enc_idle && !enc.dead) {
        send_cmd(&enc, OMX_CommandStateSet, OMX_StateLoaded);
        for (unsigned i = 0; i < nbuf; i++)
            if (fr[i].enc.remote) free_buffer(&enc, PORT_ENC_IN, &fr[i].enc);
        for (unsigned i = 0; i < nout; i++)
            if (out[i].enc.remote) free_buffer(&enc, PORT_ENC_OUT, &out[i].enc);
        wait_event(&enc, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateLoaded, 5000);
    }
    if (!cam.dead) free_handle(&cam);
    if (coding && !enc.dead) free_handle(&enc);
    for (unsigned i = 0; i < 16; i++) {
        unreg(&cam, fr[i].cy);
        unreg(&cam, fr[i].cuv);
        unreg(&enc, fr[i].ey);
        unreg(&enc, fr[i].euv);
        unreg(&enc, out[i].h);
        tiler_free(ion, &fr[i].y);
        tiler_free(ion, &fr[i].uv);
        tiler_free(ion, &out[i].b);
    }
    fprintf(stderr, "glass-camera: closed\n");
    return rc;
}
