/* glass-decode: H.264 decoded on the Ducati (OMX.TI.DUCATI1.VIDEO.DECODER,
 * role video_decoder.avc: H264D in Google's firmware), not on the
 * A9: the heads-up display's picture from the desktop, at a few percent of
 * a core where ffmpeg's software decoder took 15 % of both (2026-10-07).
 *
 *   glass-decode [-w W] [-h H] < video.flv > frames.nv12
 *
 * NOT WORKING YET: every frame fails after the decoder's first port
 * reconfiguration (docs/ducati-omx.md, The decoder); kept for the next try.
 *
 * Input: FLV with H.264 (ffmpeg -f flv), because each FLV tag is a whole
 * frame with its length up front: a frame goes to the decoder the moment
 * it has arrived (in Annex-B or MPEG-TS a frame ends only when the next
 * begins, a frame of latency). The AVC sequence header (SPS, PPS) goes to
 * the decoder as its codec configuration; each frame's length-prefixed NAL
 * units go as Annex-B (start codes), one frame a buffer. Output: the
 * pictures as packed NV12, W x H (640x360) luma then interleaved chroma,
 * cropped out of the decoder's padded TILER 2D buffers, for glass-fb -n.
 * Until stdin ends, a signal or a closed stdout; a line on stderr every 5 s
 * (frames in, frames out, rate). The decoder is taken back to Loaded and
 * freed on the way out; a run still holding it is stopped first.
 *
 * The wire is domx.h (glass-camera's, shared); docs/ducati-omx.md, The
 * decoder, has the sequence. Compiled on the Glass: gcc -O2 -I omx -o
 * glass-decode glass-decode.c -lpthread (glass update does it).
 */
#define DOMX_PROG "glass-decode"
#include "domx.h"

#define PORT_IN 0
#define PORT_OUT 1
#define FLAG_EOF 0x10                 /* OMX_BUFFERFLAG_ENDOFFRAME */
#define FLAG_CONFIG 0x80              /* OMX_BUFFERFLAG_CODECCONFIG */
#define NIN 4                         /* bitstream buffers */
#define IN_SIZE (512 * 1024)          /* one frame's bitstream at most */
#define NOUT_MAX 24

static struct comp dec;
static int ion = -1;
static unsigned W = 640, H = 360;
static int probe;                   /* -i: what the decoder asks, nothing decoded */

/* Bitstream buffers (TILER 1D) and their state. */
struct inbuf { struct tb b; uint32_t h; struct hdr hd; int busy; };
static struct inbuf in[NIN];
/* Picture buffers (TILER 2D NV12: Y 8-bit, UV 16-bit). */
struct outbuf { struct tb y, uv; uint32_t hy, huv; struct hdr hd; int at_dec; /* the decoder holds it */ };
static struct outbuf out[NOUT_MAX];
static unsigned nout, out_w, out_h;
static OMX_PARAM_PORTDEFINITIONTYPE pin, pout;
static pthread_mutex_t inmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t incv = PTHREAD_COND_INITIALIZER;
static unsigned long frames_in, frames_out;
static uint32_t crop_l, crop_t;       /* the picture's place in the padded buffer, when given */
static int have_crop;
static uint8_t cfg[1024];           /* the SPS and PPS, Annex-B */
static uint32_t ncfg;
static volatile int need_cfg;       /* set by a reconfiguration: send cfg again */
static volatile int wait_key;       /* and drop frames until a key frame */
static int gate_open = 1;           /* input may go to the decoder (under inmu) */
static unsigned long handled, dropped;   /* port events answered; frames dropped waiting for a key frame */
static void gate(int open) { pthread_mutex_lock(&inmu); gate_open = open; pthread_cond_broadcast(&incv); pthread_mutex_unlock(&inmu); }

static void on_signal(int s) { (void) s; stop = 1; }

/* Ends any other run of this program (one decoder client at a time). */
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
            if (n <= 0 || strncmp(comm, "glass-decode\n", 13)) continue;
            found = 1;
            kill(atoi(e->d_name), round ? SIGKILL : SIGTERM);
        }
        if (d) closedir(d);
        if (!found) return;
        fprintf(stderr, "glass-decode: an earlier run had the decoder: %s\n", round ? "killed" : "stopped");
        for (int i = 0; i < 20; i++) usleep(100000);
    }
}

/* Takes an event (EV on PORT) from the queue if one came; its nData2 in *D2. */
static int take_event(struct comp *c, uint32_t ev, uint32_t port, uint32_t *d2) {
    int got = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < c->nev && !got; i++)
        if (c->evq[i].ev == ev && c->evq[i].d1 == port) {
            if (d2) *d2 = c->evq[i].d2;
            memmove(c->evq + i, c->evq + i + 1, sizeof c->evq[0] * (size_t) (c->nev - i - 1));
            c->nev--;
            got = 1;
        }
    pthread_mutex_unlock(&mu);
    return got;
}

/* The output port as the decoder wants it: NV12 in TILER 2D (stride 4096),
 * its buffer count; then the buffers allocated at the size it asks
 * (OMX_TI_IndexParam2DBufferAllocDimension), registered. */
static int out_alloc(void) {
    uint32_t e;
    INIT(pout);
    pout.nPortIndex = PORT_OUT;
    if ((e = xparam(&dec, F_GET_PARAM, OMX_IndexParamPortDefinition, &pout, -1))) { fprintf(stderr, "glass-decode: output port: 0x%x\n", e); return -1; }
    pout.format.video.nStride = 4096;
    pout.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedSemiPlanar;
    /* Enough pictures for the reference frames the stream's level allows
     * (the decoder asked 7 for 640x368 once it had the SPS), so that it
     * needs no reconfiguration then; one reconfiguring rebuilt its codec
     * and lost the parameter sets (2026-10-07). */
    pout.nBufferCountActual = pout.nBufferCountMin + (getenv("GLASS_DECODE_EXTRA") ? (unsigned) atoi(getenv("GLASS_DECODE_EXTRA")) : 0);
    if ((e = xparam(&dec, F_SET_PARAM, OMX_IndexParamPortDefinition, &pout, -1))) { fprintf(stderr, "glass-decode: set output port: 0x%x\n", e); return -1; }
    xparam(&dec, F_GET_PARAM, OMX_IndexParamPortDefinition, &pout, -1);
    nout = pout.nBufferCountActual;
    if (nout > NOUT_MAX) { fprintf(stderr, "glass-decode: the decoder asks for %u picture buffers (at most %d here)\n", nout, NOUT_MAX); return -1; }
    OMX_CONFIG_RECTTYPE dim;
    INIT(dim);
    dim.nPortIndex = PORT_OUT;
    out_w = pout.format.video.nFrameWidth;
    out_h = pout.format.video.nFrameHeight;
    if (!xparam(&dec, F_GET_PARAM, OMX_TI_IndexParam2DBufferAllocDimension, &dim, -1) && dim.nWidth && dim.nHeight) {
        out_w = dim.nWidth;
        out_h = dim.nHeight;
    }
    for (unsigned i = 0; i < nout; i++) {
        if (tiler(ion, out_w, out_h, TILFMT_8BIT, &out[i].y) || tiler(ion, out_w / 2, out_h / 2, TILFMT_16BIT, &out[i].uv)) return -1;
        if (!(out[i].hy = reg(&dec, &out[i].y)) || !(out[i].huv = reg(&dec, &out[i].uv))) return -1;
    }
    fprintf(stderr, "glass-decode: pictures %ux%u (stride %d, slice height %u, colour 0x%x) in %u buffers of %ux%u (at least %u), %u bytes\n",
            (unsigned) pout.format.video.nFrameWidth, (unsigned) pout.format.video.nFrameHeight, (int) pout.format.video.nStride,
            (unsigned) pout.format.video.nSliceHeight, pout.format.video.eColorFormat, nout, out_w, out_h,
            (unsigned) pout.nBufferCountMin, (unsigned) pout.nBufferSize);
    return 0;
}

static int out_use(void) {
    for (unsigned i = 0; i < nout; i++) {
        uint32_t e = use_buffer(&dec, PORT_OUT, pout.nBufferSize, out[i].hy, out[i].huv, &out[i].hd);
        if (e) { fprintf(stderr, "glass-decode: UseBuffer (picture): 0x%x\n", e); return -1; }
    }
    return 0;
}

static void out_release(void) {
    for (unsigned i = 0; i < nout; i++) {
        unreg(&dec, out[i].hy);
        unreg(&dec, out[i].huv);
        tiler_free(ion, &out[i].y);
        tiler_free(ion, &out[i].uv);
        memset(&out[i], 0, sizeof out[i]);
        out[i].y.share = out[i].uv.share = -1;
    }
    nout = 0;
}

/* The decoder changed its output (the stream's real size, its padding):
 * the port disabled, the buffers freed, new ones at the size it now asks,
 * the port enabled, the buffers handed in again. */
static int reconfigure(void) {
    /* An event that changes nothing is only logged: a port disabled in
     * that state crashed the Ducati (PC 0, 2026-10-07). */
    OMX_PARAM_PORTDEFINITIONTYPE now;
    INIT(now);
    now.nPortIndex = PORT_OUT;
    if (!xparam(&dec, F_GET_PARAM, OMX_IndexParamPortDefinition, &now, -1) &&
        now.format.video.nFrameWidth == pout.format.video.nFrameWidth && now.format.video.nFrameHeight == pout.format.video.nFrameHeight &&
        now.format.video.nStride == pout.format.video.nStride && now.format.video.nSliceHeight == pout.format.video.nSliceHeight &&
        now.nBufferSize == pout.nBufferSize && now.nBufferCountMin <= pout.nBufferCountActual) {
        fprintf(stderr, "glass-decode: port settings changed, but nothing did (%ux%u, at least %u of %u): left as it is\n",
                (unsigned) now.format.video.nFrameWidth, (unsigned) now.format.video.nFrameHeight, (unsigned) now.nBufferCountMin, (unsigned) pout.nBufferCountActual);
        return 0;
    }
    fprintf(stderr, "glass-decode: the decoder changed its output: reallocating\n");
    send_cmd(&dec, OMX_CommandPortDisable, PORT_OUT);
    /* The decoder hands back every picture buffer it holds before any is
     * freed: freeing one it still held crashed the Ducati (three times,
     * 2026-10-07; it recovered by itself, the camera with it). */
    struct comp *one[1] = { &dec };
    for (int waited = 0; waited < 50; ) {
        int held = 0;
        for (unsigned i = 0; i < nout; i++) held += out[i].at_dec;
        if (!held) break;
        struct done d;
        struct comp *from;
        int got = next_done(one, 1, &d, &from, 100);
        if (got < 0) return -1;
        if (got > 0) { waited++; continue; }
        if (d.flags & 0x80000000u) {
            pthread_mutex_lock(&inmu);
            for (int i = 0; i < NIN; i++) if (in[i].hd.remote == d.hdr) in[i].busy = 0;
            pthread_cond_broadcast(&incv);
            pthread_mutex_unlock(&inmu);
        } else
            for (unsigned i = 0; i < nout; i++) if (out[i].hd.remote == d.hdr) out[i].at_dec = 0;
    }
    for (unsigned i = 0; i < nout; i++)
        if (out[i].at_dec) { fprintf(stderr, "glass-decode: the decoder kept a picture buffer: not freeing (no reconfiguration)\n"); return -1; }
    for (unsigned i = 0; i < nout; i++) if (out[i].hd.remote) free_buffer(&dec, PORT_OUT, &out[i].hd);
    if (wait_event(&dec, OMX_EventCmdComplete, OMX_CommandPortDisable, PORT_OUT, 5000)) return -1;
    out_release();
    if (out_alloc()) return -1;
    send_cmd(&dec, OMX_CommandPortEnable, PORT_OUT);
    if (out_use()) return -1;
    if (wait_event(&dec, OMX_EventCmdComplete, OMX_CommandPortEnable, PORT_OUT, 5000)) return -1;
    for (unsigned i = 0; i < nout; i++)
        { if (fill_this(&dec, &out[i].hd)) return -1; out[i].at_dec = 1; }
    have_crop = 0;
    need_cfg = !getenv("GLASS_DECODE_NOCFG");
    wait_key = 1;
    return 0;
}

static void read_crop(void) {
    OMX_CONFIG_RECTTYPE r;
    INIT(r);
    r.nPortIndex = PORT_OUT;
    if (!xparam(&dec, F_GET_CONFIG, OMX_IndexConfigCommonOutputCrop, &r, -1) && r.nWidth) {
        crop_l = (uint32_t) r.nLeft; crop_t = (uint32_t) r.nTop; have_crop = 1;
        fprintf(stderr, "glass-decode: picture %ux%u at %u,%u in the buffer\n", (unsigned) r.nWidth, (unsigned) r.nHeight, crop_l, crop_t);
    }
}

/* The output side, its own thread: decoded pictures to stdout, bitstream
 * buffers back to the input side, the decoder's port changes. */
static void *output(void *arg) {
    (void) arg;
    size_t fsize = (size_t) W * H * 3 / 2;
    uint8_t *pic = malloc(fsize);
    struct comp *one[1] = { &dec };
    while (!stop) {
        uint32_t d2 = 0;
        if (take_event(&dec, OMX_EventPortSettingsChanged, PORT_OUT, &d2)) {
            /* Several may be queued: one reconfiguration answers them all. */
            int full = d2 != OMX_IndexConfigCommonOutputCrop;
            uint32_t more;
            while (take_event(&dec, OMX_EventPortSettingsChanged, PORT_OUT, &more)) full |= more != OMX_IndexConfigCommonOutputCrop;
            /* No input reaches the decoder until its output is settled. */
            gate(0);
            if (!full) read_crop();
            else if (reconfigure()) { stop = 1; break; }
            pthread_mutex_lock(&inmu); handled++; pthread_mutex_unlock(&inmu);
            gate(1);
        }
        struct done d;
        struct comp *from;
        int got = next_done(one, 1, &d, &from, 500);
        if (got < 0) { stop = 1; break; }
        if (got > 0) continue;
        if (d.flags & 0x80000000u) {
            /* EmptyBufferDone: a bitstream buffer is free again. */
            pthread_mutex_lock(&inmu);
            for (int i = 0; i < NIN; i++) if (in[i].hd.remote == d.hdr) in[i].busy = 0;
            pthread_cond_broadcast(&incv);
            pthread_mutex_unlock(&inmu);
            continue;
        }
        struct outbuf *o = NULL;
        for (unsigned i = 0; i < nout; i++) if (out[i].hd.remote == d.hdr) o = &out[i];
        if (!o) continue;   /* a buffer from before a reconfiguration */
        o->at_dec = 0;
        if (d.len) {
            /* The picture: from nOffset in the 4096-byte rows (UV at half
             * the row), or the crop the decoder reported. */
            uint32_t row = d.off / 4096, col = d.off % 4096;
            if (!d.off && have_crop) { row = crop_t; col = crop_l; }
            for (unsigned y = 0; y < H; y++)
                memcpy(pic + (size_t) y * W, o->y.map + (size_t) (row + y) * o->y.stride + col, W);
            for (unsigned y = 0; y < H / 2; y++)
                memcpy(pic + (size_t) W * H + (size_t) y * W, o->uv.map + (size_t) (row / 2 + y) * o->uv.stride + col, W);
            if (fwrite(pic, 1, fsize, stdout) != fsize || fflush(stdout)) { stop = 1; break; }
            frames_out++;
        }
        if (!stop) { if (fill_this(&dec, &o->hd)) { stop = 1; break; } o->at_dec = 1; }
    }
    free(pic);
    pthread_mutex_lock(&inmu);
    pthread_cond_broadcast(&incv);
    pthread_mutex_unlock(&inmu);
    return NULL;
}

static int read_full(uint8_t *p, size_t n) {
    while (n) {
        ssize_t r = read(0, p, n);
        if (r < 0 && errno == EINTR) { if (stop) return -1; continue; }
        if (r <= 0) return -1;
        p += r; n -= (size_t) r;
    }
    return 0;
}

/* A free bitstream buffer, waiting for one; NULL when stopping. */
static struct inbuf *free_in(void) {
    pthread_mutex_lock(&inmu);
    for (;;) {
        for (int i = 0; i < NIN && gate_open; i++)
            if (!in[i].busy) { in[i].busy = 1; pthread_mutex_unlock(&inmu); return &in[i]; }
        if (stop) break;
        struct timespec t = deadline(500);
        pthread_cond_timedwait(&incv, &inmu, &t);
    }
    pthread_mutex_unlock(&inmu);
    return NULL;
}

static int submit(struct inbuf *b, uint32_t len, uint32_t flags, int64_t ts) {
    uint32_t e = empty_this(&dec, &b->hd, len, 0, flags, ts);
    if (e) { fprintf(stderr, "glass-decode: EmptyThisBuffer: 0x%x\n", e); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) W = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) H = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i")) probe = 1;
        else { fprintf(stderr, "glass-decode [-i] [-w W] [-h H] < video.flv > frames.nv12\n"); return 2; }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    takeover();
    ion = open("/dev/ion", O_RDWR);
    if (ion < 0) { perror("glass-decode: /dev/ion"); return 1; }
    for (int i = 0; i < NIN; i++) in[i].b.share = -1;
    for (int i = 0; i < NOUT_MAX; i++) out[i].y.share = out[i].uv.share = -1;
    uint32_t e0;
    /* TI's one video decoder component, told its codec by its role (as
     * Android's Stagefright does: video_decoder.avc). */
    if (open_comp(&dec, "OMX.TI.DUCATI1.VIDEO.DECODER")) return 1;
    OMX_PARAM_COMPONENTROLETYPE role;
    INIT(role);
    strcpy((char *) role.cRole, "video_decoder.avc");
    if ((e0 = xparam(&dec, F_SET_PARAM, OMX_IndexParamStandardComponentRole, &role, -1))) { fprintf(stderr, "glass-decode: role video_decoder.avc: 0x%x\n", e0); return 1; }
    /* As TI's proxy sets it for Android (omx_proxy_videodec.c): crop and
     * padding changes come as port reconfigurations the way its firmware
     * was tested with. */
    OMX_TI_PARAM_ENHANCEDPORTRECONFIG er;
    INIT(er);
    er.nPortIndex = PORT_OUT;
    er.bUsePortReconfigForCrop = OMX_TRUE;
    er.bUsePortReconfigForPadding = OMX_TRUE;
    if ((e0 = xparam(&dec, F_SET_PARAM, OMX_TI_IndexParamUseEnhancedPortReconfig, &er, -1)))
        fprintf(stderr, "glass-decode: enhanced port reconfiguration: 0x%x (going on without)\n", e0);

    int rc = 1, idle = 0, exec = 0;
    uint32_t e;
    pthread_t ot;
    int ot_started = 0;
    /* The input: AVC at the stream's size, room for one frame a buffer. */
    INIT(pin);
    pin.nPortIndex = PORT_IN;
    if ((e = xparam(&dec, F_GET_PARAM, OMX_IndexParamPortDefinition, &pin, -1))) { fprintf(stderr, "glass-decode: input port: 0x%x\n", e); goto out; }
    pin.format.video.nFrameWidth = W;
    pin.format.video.nFrameHeight = (H + 15) & ~15u;   /* whole macroblocks: as the SPS will say */
    pin.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
    pin.nBufferCountActual = NIN;
    pin.nBufferSize = IN_SIZE;
    if ((e = xparam(&dec, F_SET_PARAM, OMX_IndexParamPortDefinition, &pin, -1))) { fprintf(stderr, "glass-decode: set input port: 0x%x\n", e); goto out; }
    xparam(&dec, F_GET_PARAM, OMX_IndexParamPortDefinition, &pin, -1);
    if (pin.nBufferCountActual != NIN || pin.nBufferSize < 64 * 1024) {
        fprintf(stderr, "glass-decode: the decoder took %u input buffers of %u bytes\n", (unsigned) pin.nBufferCountActual, (unsigned) pin.nBufferSize);
        goto out;
    }
    for (int i = 0; i < NIN; i++) {
        if (tiler(ion, pin.nBufferSize, 1, TILFMT_PAGE, &in[i].b)) goto out;
        if (!(in[i].h = reg(&dec, &in[i].b))) goto out;
    }
    if (probe) {
        /* The output the decoder computes before any stream, then with the
         * input told the stream's level (2.2 from NVENC at 640x368). */
        OMX_VIDEO_PARAM_AVCTYPE avc;
        INIT(avc);
        avc.nPortIndex = PORT_IN;
        e = xparam(&dec, F_GET_PARAM, OMX_IndexParamVideoAvc, &avc, -1);
        fprintf(stderr, "glass-decode: input AVC (0x%x): profile 0x%x level 0x%x refs %u\n", e, avc.eProfile, avc.eLevel, (unsigned) avc.nRefFrames);
        OMX_VIDEO_PARAM_PROFILELEVELTYPE pl;
        INIT(pl);
        pl.nPortIndex = PORT_IN;
        e = xparam(&dec, F_GET_PARAM, OMX_IndexParamVideoProfileLevelCurrent, &pl, -1);
        fprintf(stderr, "glass-decode: input profile/level current (0x%x): 0x%x 0x%x\n", e, (unsigned) pl.eProfile, (unsigned) pl.eLevel);
        INIT(pout); pout.nPortIndex = PORT_OUT;
        xparam(&dec, F_GET_PARAM, OMX_IndexParamPortDefinition, &pout, -1);
        fprintf(stderr, "glass-decode: output before: %ux%u, at least %u, %u bytes\n", (unsigned) pout.format.video.nFrameWidth,
                (unsigned) pout.format.video.nFrameHeight, (unsigned) pout.nBufferCountMin, (unsigned) pout.nBufferSize);
        avc.eLevel = OMX_VIDEO_AVCLevel22; avc.eProfile = OMX_VIDEO_AVCProfileBaseline; avc.nRefFrames = 1;
        e = xparam(&dec, F_SET_PARAM, OMX_IndexParamVideoAvc, &avc, -1);
        fprintf(stderr, "glass-decode: set AVC level 2.2, 1 reference: 0x%x\n", e);
        pl.eProfile = OMX_VIDEO_AVCProfileBaseline; pl.eLevel = OMX_VIDEO_AVCLevel22;
        e = xparam(&dec, F_SET_PARAM, OMX_IndexParamVideoProfileLevelCurrent, &pl, -1);
        fprintf(stderr, "glass-decode: set profile/level current: 0x%x\n", e);
        INIT(pout); pout.nPortIndex = PORT_OUT;
        xparam(&dec, F_GET_PARAM, OMX_IndexParamPortDefinition, &pout, -1);
        fprintf(stderr, "glass-decode: output after: %ux%u, at least %u, %u bytes\n", (unsigned) pout.format.video.nFrameWidth,
                (unsigned) pout.format.video.nFrameHeight, (unsigned) pout.nBufferCountMin, (unsigned) pout.nBufferSize);
        goto out;
    }
    if (out_alloc()) goto out;

    /* Idle with every buffer handed in, then Executing, the picture
     * buffers given to be filled. */
    if ((e = send_cmd(&dec, OMX_CommandStateSet, OMX_StateIdle))) { fprintf(stderr, "glass-decode: to Idle: 0x%x\n", e); goto out; }
    for (int i = 0; i < NIN; i++)
        if ((e = use_buffer(&dec, PORT_IN, pin.nBufferSize, in[i].h, 0, &in[i].hd))) { fprintf(stderr, "glass-decode: UseBuffer (bitstream): 0x%x\n", e); goto out; }
    if (out_use()) goto out;
    if (wait_event(&dec, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateIdle, 5000)) goto out;
    idle = 1;
    if (state_to(&dec, OMX_StateExecuting, 5000)) goto out;
    exec = 1;
    for (unsigned i = 0; i < nout; i++)
        { if ((e = fill_this(&dec, &out[i].hd))) { fprintf(stderr, "glass-decode: FillThisBuffer: 0x%x\n", e); goto out; } out[i].at_dec = 1; }
    if (pthread_create(&ot, NULL, output, NULL)) goto out;
    ot_started = 1;
    fprintf(stderr, "glass-decode: decoding %ux%u H.264 (FLV in, NV12 out)\n", W, H);

    /* The input side: FLV tags; video ones (type 9) with AVC (codec 7). */
    uint8_t fh[13];
    if (read_full(fh, 13) || memcmp(fh, "FLV", 3)) { fprintf(stderr, "glass-decode: not FLV on stdin\n"); goto out; }
    uint8_t *tag = malloc(IN_SIZE);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double report = 5;
    unsigned long last_out = 0;
    while (!stop) {
        uint8_t th[11];
        if (read_full(th, 11)) break;
        uint32_t len = (uint32_t) th[1] << 16 | (uint32_t) th[2] << 8 | th[3];
        int64_t ts = (int64_t) ((uint32_t) th[7] << 24 | (uint32_t) th[4] << 16 | (uint32_t) th[5] << 8 | th[6]) * 1000;
        if (len > IN_SIZE - 64) { fprintf(stderr, "glass-decode: a %u-byte tag: too large\n", len); break; }
        if (read_full(tag, len) || read_full(fh, 4)) break;   /* the tag, then its PreviousTagSize */
        if (th[0] != 9 || len < 5 || (tag[0] & 0x0f) != 7) continue;
        struct inbuf *b = free_in();
        if (!b) break;
        uint8_t *o = b->b.map;
        uint32_t n = 0, flags = FLAG_EOF;
        if (tag[1] == 0) {
            /* AVCDecoderConfigurationRecord: the SPS and PPS, kept as
             * Annex-B and given as the codec configuration. */
            const uint8_t *p = tag + 5, *end = tag + len;
            ncfg = 0;
            if (end - p >= 6) {
                int nsps = p[5] & 0x1f;
                p += 6;
                for (int k = 0; k < 2 && p < end; k++) {
                    int cnt = k == 0 ? nsps : *p++;
                    for (int j = 0; j < cnt && p + 2 <= end; j++) {
                        uint32_t l = (uint32_t) p[0] << 8 | p[1];
                        p += 2;
                        if (p + l > end || ncfg + 4 + l > sizeof cfg) break;
                        memcpy(cfg + ncfg, "\0\0\0\1", 4); memcpy(cfg + ncfg + 4, p, l);
                        ncfg += 4 + l; p += l;
                    }
                }
            }
            memcpy(o, cfg, ncfg);
            n = ncfg;
            flags |= FLAG_CONFIG;
            need_cfg = 0;
        } else if (tag[1] == 1) {
            /* After a reconfiguration (and at the start) nothing until a key
             * frame: a P frame whose reference went to the old codec fails. */
            if (wait_key) {
                if ((tag[0] >> 4) != 1) { pthread_mutex_lock(&inmu); b->busy = 0; pthread_mutex_unlock(&inmu); dropped++; continue; }
                wait_key = 0;
            }
            /* After the decoder rebuilt its codec (a port reconfiguration)
             * it has lost the parameter sets: they go again first. */
            if (need_cfg && ncfg) {
                /* In the key frame's own buffer, ahead of it: a separate
                 * configuration buffer did not reach the rebuilt codec (its
                 * first frame failed, 0x4000, 2026-10-07). */
                memcpy(o, cfg, ncfg);
                n = ncfg;
                need_cfg = 0;
            }
            /* Length-prefixed NAL units (4 bytes each, as ffmpeg writes FLV). */
            const uint8_t *p = tag + 5, *end = tag + len;
            while (p + 4 <= end) {
                uint32_t l = (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
                p += 4;
                if (p + l > end) break;
                memcpy(o + n, "\0\0\0\1", 4); memcpy(o + n + 4, p, l);
                n += 4 + l; p += l;
            }
            frames_in++;
        }
        if (!n) { pthread_mutex_lock(&inmu); b->busy = 0; pthread_mutex_unlock(&inmu); continue; }
        if (submit(b, n, flags, ts)) break;
        if (flags & FLAG_CONFIG) {
            /* The codec configuration alone first: the decoder answers it
             * with its port change at once (every run so far); that one is
             * finished before any frame goes in. */
            pthread_mutex_lock(&inmu);
            unsigned long h0 = handled;
            struct timespec dl = deadline(1000);
            while (handled == h0 && !stop) if (pthread_cond_timedwait(&incv, &inmu, &dl) == ETIMEDOUT) break;
            pthread_mutex_unlock(&inmu);
        }
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        double el = (double) (t.tv_sec - t0.tv_sec) + (double) (t.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= report) {
            fprintf(stderr, "glass-decode: %lu frames in, %lu out, %.1f/s\n", frames_in, frames_out, (double) (frames_out - last_out) / 5.0);
            last_out = frames_out;
            report += 5;
        }
    }
    free(tag);
    rc = 0;

out:
    stop = 1;
    if (ot_started) pthread_join(ot, NULL);
    if (exec && !dec.dead) state_to(&dec, OMX_StateIdle, 5000);
    if (idle && !dec.dead) {
        send_cmd(&dec, OMX_CommandStateSet, OMX_StateLoaded);
        for (int i = 0; i < NIN; i++) if (in[i].hd.remote) free_buffer(&dec, PORT_IN, &in[i].hd);
        for (unsigned i = 0; i < nout; i++) if (out[i].hd.remote) free_buffer(&dec, PORT_OUT, &out[i].hd);
        wait_event(&dec, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateLoaded, 5000);
    }
    if (!dec.dead) free_handle(&dec);
    for (int i = 0; i < NIN; i++) { unreg(&dec, in[i].h); tiler_free(ion, &in[i].b); }
    out_release();
    fprintf(stderr, "glass-decode: closed (%lu frames in, %lu out, %lu dropped waiting for a key frame)\n", frames_in, frames_out, dropped);
    return rc;
}
