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
#define DOMX_PROG "glass-camera"
#include "domx.h"

#define PORT_PREVIEW 2

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
