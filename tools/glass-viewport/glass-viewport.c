/* glass-viewport: a pixel-perfect WxH window of a monitor's screencast that
 * follows the pointer, encoded on the GPU for the Glass. Built by
 * scripts/glass-view.sh (gcc, GStreamer); run by `desk cast` with the
 * PipeWire fd and node substituted.
 *
 *   glass-viewport FD NODE OUTW OUTH OX OY W H FPS KBIT CURSOR_FILE
 *
 * The monitor is OUTWxOUTH at global (OX,OY). CURSOR_FILE carries "X Y"
 * lines in global coordinates (desk cursor). The viewport keeps the pointer
 * inside a central zone (a quarter of the viewport on each side): it moves
 * only when the pointer nears an edge, like a camera panning, and never
 * leaves the monitor. The frame is not scaled: a CUDA compositor with a WxH
 * canvas shows the monitor placed at (-vx,-vy), one source pixel per
 * output pixel; then NV12 and NVENC, MPEG-TS on stdout.
 */
#include <gst/gst.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GstElement *pipeline, *vp;
static int outw, outh, ox, oy, W, H;
static int vx = 0, vy = 0;

static void place(int cx, int cy) {
    int mx = W / 4, my = H / 4;
    int nx = vx, ny = vy;
    cx -= ox; cy -= oy;
    if (cx < vx + mx) nx = cx - mx;
    else if (cx > vx + W - mx) nx = cx - (W - mx);
    if (cy < vy + my) ny = cy - my;
    else if (cy > vy + H - my) ny = cy - (H - my);
    if (nx < 0) nx = 0;
    if (nx > outw - W) nx = outw - W;
    if (ny < 0) ny = 0;
    if (ny > outh - H) ny = outh - H;
    if (nx == vx && ny == vy) return;
    vx = nx; vy = ny;
    GstPad *pad = gst_element_get_static_pad(vp, "sink_0");
    if (pad) {
        g_object_set(pad, "xpos", -vx, "ypos", -vy, NULL);
        gst_object_unref(pad);
    }
}

static void *follow(void *arg) {
    FILE *f = fopen((const char *) arg, "r");
    if (!f) { perror("glass-viewport: cursor file"); return NULL; }
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int cx, cy;
        if (sscanf(line, "%d %d", &cx, &cy) == 2) place(cx, cy);
    }
    fclose(f);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 12) {
        fprintf(stderr, "usage: glass-viewport FD NODE OUTW OUTH OX OY W H FPS KBIT CURSOR_FILE\n");
        return 2;
    }
    gst_init(&argc, &argv);
    const char *fd = argv[1], *node = argv[2];
    outw = atoi(argv[3]); outh = atoi(argv[4]); ox = atoi(argv[5]); oy = atoi(argv[6]);
    W = atoi(argv[7]); H = atoi(argv[8]);
    int fps = atoi(argv[9]), kbit = atoi(argv[10]);
    const char *cursor = argv[11];
    if (W > outw || H > outh) { fprintf(stderr, "glass-viewport: %dx%d does not fit %dx%d\n", W, H, outw, outh); return 2; }
    vx = (outw - W) / 2; vy = (outh - H) / 2;

    char desc[2048];
    snprintf(desc, sizeof desc,
        "pipewiresrc fd=%s path=%s do-timestamp=true keepalive-time=%d ! video/x-raw "
        "! videorate ! video/x-raw,framerate=%d/1 "
        "! cudaupload ! cudacompositor name=vp sink_0::xpos=%d sink_0::ypos=%d "
        "! video/x-raw(memory:CUDAMemory),width=%d,height=%d "
        "! cudaconvertscale ! video/x-raw(memory:CUDAMemory),width=%d,height=%d,format=NV12 "
        "! nvh264enc preset=p1 tune=ultra-low-latency rc-mode=cbr bitrate=%d gop-size=%d zerolatency=true bframes=0 "
        "! h264parse ! mpegtsmux ! fdsink fd=1 sync=false",
        fd, node, 1000 / fps, fps, -vx, -vy, W, H, W, H, kbit, fps);
    GError *err = NULL;
    pipeline = gst_parse_launch(desc, &err);
    if (!pipeline || err) { fprintf(stderr, "glass-viewport: %s\n", err ? err->message : "no pipeline"); return 1; }
    vp = gst_bin_get_by_name(GST_BIN(pipeline), "vp");
    if (!vp) { fprintf(stderr, "glass-viewport: no compositor\n"); return 1; }

    pthread_t t;
    pthread_create(&t, NULL, follow, (void *) cursor);
    pthread_detach(t);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fprintf(stderr, "glass-viewport: %dx%d of %dx%d, following the pointer, %d frames/s, %d kbit/s\n", W, H, outw, outh, fps, kbit);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    int rc = 0;
    if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        gst_message_parse_error(msg, &err, NULL);
        fprintf(stderr, "glass-viewport: %s\n", err->message);
        rc = 1;
    }
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return rc;
}
