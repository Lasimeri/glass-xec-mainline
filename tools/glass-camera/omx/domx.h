/* domx.h: TI's DOMX wire (OMX over rpmsg to the Ducati) in C, shared by
 * glass-camera (the camera and the H.264 encoder) and glass-decode (the
 * H.264 decoder): the factory kernel's ION and rpmsg-omx structures, the
 * 240-byte call packets, one component per rpmsg-omx open with its reader
 * thread, TILER buffers, and the buffer calls. docs/ducati-omx.md has the
 * protocol and where each part was read. Static functions, included once
 * by each program; DOMX_PROG names the program in messages. Calls are
 * serialised (call_mu), so two threads may make them.
 */
#ifndef DOMX_PROG
#define DOMX_PROG "domx"
#endif
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
    if (HDR + p->pos + n > PKT) { fprintf(stderr, DOMX_PROG ": packet overflow\n"); exit(1); }
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
            fprintf(stderr, DOMX_PROG ": %s: connection lost (%s)\n", c->name, strerror(c->dead));
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
            return NULL;
        }
        uint32_t fn = get32(b + 8) & 0x0fffffffu;
        const uint8_t *d = b + HDR;
        if (fn == F_EVENT) {
            struct event e = { get32(d + 4), get32(d + 8), get32(d + 12), get32(d + 16) };
            fprintf(stderr, DOMX_PROG ": %s: %s %u (0x%x, 0x%x)\n", c->name, ev_name(e.ev), e.ev, e.d1, e.d2);
            if (c->nev == 64) { memmove(c->evq, c->evq + 1, sizeof c->evq[0] * 63); c->nev--; }
            c->evq[c->nev++] = e;
        } else if (fn == F_FILL_DONE || fn == F_EMPTY_DONE) {
            struct done x = { get32(d + 4), get32(d + 8), get32(d + 12), get32(d + 16), 0 };
            if (fn == F_FILL_DONE) memcpy(&x.ts, d + 20, 8);
            else x.flags |= 0x80000000u;    /* marks an EmptyBufferDone */
            if (c->ndone == 64) fprintf(stderr, DOMX_PROG ": %s: done queue full, one lost\n", c->name);
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
static pthread_mutex_t call_mu = PTHREAD_MUTEX_INITIALIZER;   /* one call in flight, whichever thread */
static uint32_t call_locked(struct comp *c, struct pkt *p, uint8_t *r);
static uint32_t call(struct comp *c, struct pkt *p, uint8_t *r) {
    pthread_mutex_lock(&call_mu);
    uint32_t e = call_locked(c, p, r);
    pthread_mutex_unlock(&call_mu);
    return e;
}
static uint32_t call_locked(struct comp *c, struct pkt *p, uint8_t *r) {
    pthread_mutex_lock(&mu);
    c->replied = 0;
    pthread_mutex_unlock(&mu);
    if (write(c->fd, p->b, PKT) != PKT) {
        fprintf(stderr, DOMX_PROG ": %s: write: %s\n", c->name, strerror(errno));
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
        fprintf(stderr, DOMX_PROG ": %s: no reply to function %u\n", c->name, get32(p->b + 8) & 0xffff);
        return NO_REPLY;
    }
    return get32(r + 12);
}

static int open_comp(struct comp *c, const char *name) {
    memset(c, 0, sizeof *c);
    c->name = name;
    c->fd = open("/dev/rpmsg-omx1", O_RDWR);
    if (c->fd < 0) { fprintf(stderr, DOMX_PROG ": /dev/rpmsg-omx1: %s (is the Ducati up?)\n", strerror(errno)); return -1; }
    char conn[48] = "OMX";
    if (ioctl(c->fd, OMX_IOCCONNECT, conn) < 0) {
        fprintf(stderr, DOMX_PROG ": connect to the OMX service: %s\n", strerror(errno));
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
    if (e) { fprintf(stderr, DOMX_PROG ": GetHandle %s: 0x%x\n", name, e); return -1; }
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

static __attribute__((unused)) uint32_t get_state(struct comp *c, uint32_t *state) {
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
    if (e) fprintf(stderr, DOMX_PROG ": FreeHandle: 0x%x\n", e);
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
    fprintf(stderr, DOMX_PROG ": %s: no %s (0x%x, 0x%x) within %d ms\n", c->name, ev_name(ev), d1, d2, ms);
    return -1;
}

static int state_to(struct comp *c, OMX_STATETYPE s, int ms) {
    uint32_t e = send_cmd(c, OMX_CommandStateSet, s);
    if (e) { fprintf(stderr, DOMX_PROG ": StateSet %d: 0x%x\n", s, e); return -1; }
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
        fprintf(stderr, DOMX_PROG ": TILER %ux%u (format %d): %s\n", w, h, fmt, strerror(errno));
        return -1;
    }
    t->ion = a.handle;
    t->stride = a.stride;
    t->size = fmt == TILFMT_PAGE ? (w + 4095) & ~4095u : a.stride * h;
    struct k_ion_fd s = { .handle = t->ion };
    if (ioctl(ion, ION_IOC_SHARE, &s) < 0) { perror(DOMX_PROG ": ION share"); return -1; }
    t->share = s.fd;
    t->map = mmap(NULL, t->size, PROT_READ | PROT_WRITE, MAP_SHARED, t->share, 0);
    if (t->map == MAP_FAILED) { t->map = NULL; perror(DOMX_PROG ": mmap"); return -1; }
    return 0;
}
static uint32_t reg(struct comp *c, struct tb *t) {
    struct k_ion_fd g = { .fd = t->share };
    if (ioctl(c->fd, OMX_IOCIONREGISTER, &g) < 0) { perror(DOMX_PROG ": register with rpmsg-omx"); return 0; }
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

static __attribute__((unused)) void show_port(struct comp *c, uint32_t port) {
    OMX_PARAM_PORTDEFINITIONTYPE d;
    INIT(d);
    d.nPortIndex = port;
    uint32_t e = xparam(c, F_GET_PARAM, OMX_IndexParamPortDefinition, &d, -1);
    if (e) { fprintf(stderr, DOMX_PROG ": %s port %u: 0x%x\n", c->name, port, e); return; }
    printf("%s port %u: %s, %s, %ux%u stride %d, colour 0x%x, coding %d, %.2f/s, %u bit/s, buffers %u (at least %u) of %u bytes\n",
           c->name, port, d.eDir == OMX_DirOutput ? "out" : "in", d.bEnabled ? "enabled" : "disabled",
           (unsigned) d.format.video.nFrameWidth, (unsigned) d.format.video.nFrameHeight, (int) d.format.video.nStride,
           d.format.video.eColorFormat, d.format.video.eCompressionFormat, d.format.video.xFramerate / 65536.0,
           (unsigned) d.format.video.nBitrate,
           (unsigned) d.nBufferCountActual, (unsigned) d.nBufferCountMin, (unsigned) d.nBufferSize);
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

