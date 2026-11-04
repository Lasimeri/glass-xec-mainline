# The Ducati and OMX over rpmsg: the wire, as used by tools/glass-camera

Google's 3.4 kernel boots the Ducati (the OMAP4's two Cortex-M3 cores, IPU)
with `ducati-m3-core0-2gb.xem3` (answered from /lib/firmware by
glass-firmware at boot; later requests, a recovery after a crash, by mdev,
which loads firmware). The firmware (Glass build of 2014-09-04, commit 7839a05,
codecs H264D 01.00.00.13, H264E 01.00.06.00, MPEG4 D/E, VC1D, MPEG2D) serves
OMX components to the A9 over rpmsg. Android reached them through TI's DOMX
(hardware/ti/omap4xxx, KitKat: `dl/ti-omap4xxx/domx`); glass-camera speaks the
same wire in C, without Android. Sources read for this (all on disk):

| what | where |
| --- | --- |
| rpmsg-omx driver (connect, write, read, buffer translation) | kernel `drivers/rpmsg/rpmsg_omx.c`, `include/linux/rpmsg_omx.h` |
| ION, OMAP TILER heap | kernel `include/linux/ion.h`, `include/linux/omap_ion.h`, `drivers/gpu/ion/omap/omap_tiler_heap.c` |
| heap sizes and addresses | kernel `arch/arm/mach-omap2/board-notle.c` (notle carveouts), `omap4_ion.c` |
| DOMX packet layouts | `domx/domx/omx_rpc/src/omx_rpc_stub.c` (calls), `omx_rpc_skel.c` (callbacks), `omx_rpc.c` (connect, reader) |
| buffers, TILER, NV12 | `domx/domx/omx_proxy_common/src/omx_proxy_common.c`, `ion/ion.c` |
| camera use | `camera/OMXCameraAdapter/OMXCameraAdapter.cpp`, `domx/omx_proxy_component/omx_camera` |
| OMX structs | `domx/omx_core/inc` (Khronos OMX 1.1 plus TI's `OMX_TI_*`) |

The kernel tree is `dl/factory-kernel-1091b53.tar.gz` (unpacked in
`~/.cache/glass-xec/kernel`).

## Devices

- `/dev/rpmsg-omx1`: the AppM3 core's OMX service (the camera, the codecs);
  `/dev/rpmsg-omx0` is the SysM3's. One open is one OMX client: it carries
  its own ION client (handles are per open).
- `/dev/ion`: the allocator. Heaps (id: name): 0 system, 1 tiler, 2
  secure_input (carveout), 3 nonsecure_tiler, 4 tiler_reservation, 5
  multimedia_carveout.
- Ducati traces: `/sys/kernel/debug/remoteproc/remoteproc0/trace0` (SysM3),
  `trace1` (AppM3: the camera and codecs print their errors here).

## ioctls (sizes from the factory kernel's headers; any other size is ENOTTY)

| ioctl | number | argument |
| --- | --- | --- |
| OMX_IOCCONNECT | `_IOW('X', 1, char *)` | 48-byte zero-padded name, "OMX" |
| OMX_IOCIONREGISTER | `_IOWR('X', 2, struct ion_fd_data)` | fd in, handle (valid on this rpmsg-omx open) out |
| OMX_IOCIONUNREGISTER | `_IOWR('X', 3, struct ion_fd_data)` | handle in |
| ION_IOC_ALLOC | `_IOWR('I', 0, ...)` | `{size_t len, align; unsigned flags (heap id mask); handle}`: 16 bytes |
| ION_IOC_FREE | `_IOWR('I', 1, ...)` | `{handle}`: 4 |
| ION_IOC_MAP | `_IOWR('I', 2, struct ion_fd_data)` | `{handle; int fd; unsigned char cacheable}`: 12; the fd is mmapped |
| ION_IOC_SHARE | `_IOWR('I', 4, struct ion_fd_data)` | as MAP; the fd goes to OMX_IOCIONREGISTER |
| ION_IOC_CUSTOM | `_IOWR('I', 6, ...)` | `{unsigned cmd; unsigned long arg}`: 8; cmd 0 = OMAP_ION_TILER_ALLOC |

OMAP_ION_TILER_ALLOC takes `{size_t w, h; int fmt; unsigned flags; handle;
size_t stride; size_t offset; u32 out_align; u32 token}` (36 bytes); fmt 0 8-bit,
1 16-bit, 2 32-bit (2D), 3 page (1D, h must be 1). It returns the handle,
`stride` (the row length in the A9's mapping of the buffer, page-aligned) and
`offset` (within the first page).

## Memory the Ducati can see

From the firmware's resource table (`.resource_table`, file offset 0x1000):
devmem entries map TILER identically, da = pa: 0x60000000 (256 MB, modes 0
and 1: 8 and 16-bit 2D), 0x70000000 (mode 2), 0x78000000 (mode 3, 1D pages).
IOBUFS maps da 0x88000000 to pa 0xfc000000 (60 MB), around
the secure_input heap (0xfc100000 by the board file's arithmetic): that is how the
Ducati sees carveout buffers, which TI's proxy gives every non-2D port on ION
builds. Pictures are TILER 2D. The kernel translates a registered handle to the
Ducati's address in `write()` (ion_phys, then rproc_pa_to_da).

## Packets

Every call is one `write()` of 240 bytes (RPC_PACKET_SIZE):

```
struct omx_packet { u16 desc;      /* 0x0100: OMX_DESC_MSG << 8 */
                    u16 msg_id;    /* 0 */
                    u32 flags;     /* 0x8000: OMX_POOLID_JOBID_DEFAULT */
                    u32 fxn_idx;   /* function | 0x80000000 */
                    i32 result;    /* OMX_ERRORTYPE, in the reply */
                    u32 data_size; /* 240 */
                    u8  data[]; }
```

`data` starts with `u32 map_info` (0 none, 1, 2, 3 buffers to translate) and
`u32 map_offset` (byte offset within `data` of the first of those buffer
fields, consecutive 4-byte handles; the kernel replaces each with the
Ducati's address). Then the arguments, packed, 4 bytes each unless noted. The
reply comes back as a read of the same layout: values returned start where
the arguments ended, a struct comes back where it was sent.

| fn | idx | arguments after map_info, map_offset | returned |
| --- | --- | --- | --- |
| GetHandle | 0 | name[128], pAppData | at 140: hComp (used in every later call), hActualComp |
| SetParameter | 1 | hComp, index, struct (nSize bytes) | |
| GetParameter | 2 | hComp, index, struct | the struct, at 16 |
| UseBuffer | 3 | hComp, port, pAppPrivate, nSizeBytes, buffer, [aux1], [metadata] (map_offset 24) | at 28/32/36: remote header, nSize, nVersion, nAllocLen, nFilledLen, nOffset, pAppPrivate, pInputPortPrivate, pOutputPortPrivate, hMarkTarget, pMarkData, nTickCount, nTimeStamp (8), nFlags, nInputPortIndex, nOutputPortIndex |
| FreeHandle | 4 | hComp | |
| SetConfig | 5 | as SetParameter | |
| GetConfig | 6 | as GetParameter | the struct, at 16 |
| GetState | 7 | hComp | state at 12 |
| SendCommand | 8 | hComp, cmd, nParam, [struct] | |
| FillThisBuffer | 11 | hComp, remote header, nFilledLen, nOffset, nFlags, nAllocLen, nOutputPortIndex, nInputPortIndex | |
| FreeBuffer | 13 | hComp, port, remote header, buffer | |
| EmptyThisBuffer | 14 | see omx_rpc_stub.c | |

A Set/GetParameter whose struct carries a buffer (a shared buffer config) sets
map_info 1 and map_offset = 16 + the field's offset within the struct.

Callbacks arrive unrequested, `data` without map_info, fields packed:

| fn | idx | fields |
| --- | --- | --- |
| FillBufferDone | 12 | pAppData, remote header, nFilledLen, nOffset, nFlags, nTimeStamp (8 bytes at 20, unaligned), hMarkTarget, pMarkData |
| EmptyBufferDone | 15 | pAppData, remote header, nFilledLen, nOffset, nFlags |
| EventHandler | 16 | pAppData, event, nData1, nData2, pEventData |

Events can arrive before the reply to the call that caused them. Only the
reader sees replies, so the reader never makes a call itself.

## The camera

`OMX.TI.DUCATI1.VIDEO.CAMERA`. Ports: 0 other in, 1 video in, 2 preview out,
3 video out, 4 measurement out, 5 image out. The HAL's order: GetHandle;
PortDisable all; PortEnable 2 (wait for its CmdComplete); SetConfig
OMX_TI_IndexConfigSensorSelect (0, primary); SetParameter PortDefinition on
port 2 (width, height, NV12 colour: the HAL uses 0x15, glass-camera 0x27 as the encoder takes it, nStride 4096,
xFramerate fps << 16, nBufferCountActual); GetParameter
OMX_TI_IndexParam2DBufferAllocDimension (the buffer size to allocate);
GetParameter OMX_TI_IndexParamMetaDataBufferInfo (a third, metadata buffer
per frame if enabled); StateSet Idle; UseBuffer each; StateSet Executing;
FillThisBuffer each. nStride 4096 means TILER 2D NV12: Y in an 8-bit
container (w x h), UV in a 16-bit one (w/2 x h/2), passed as buffer and aux1.
A filled buffer's nOffset gives the picture's start: row nOffset / 4096,
column nOffset % 4096 (UV at half the row). DCC tuning files (Android's
/data/misc/camera) are optional: without them the firmware's defaults apply.

What the Glass's firmware does differently from KitKat's headers:
`OMX_TI_CAPTYPE` has another layout (glass-camera finds the size records,
nSize 28, by scanning); the sizes it reports are preview/video 64x64 to
1920x1088 (and 1088x1920), stills to 2592x1944, thumbnails to 640x480. The
sensor is an OmniVision OV5680 (`sensor_detect_MSP.c: OV5680_EVT2`).
Preview port defaults: 320x240, colour 0x27, 60/s; port 5 (stills)
1296x972. `OMX_TI_IndexParamMetaDataBufferInfo` is unsupported (no
metadata buffers). Without `OMX_IndexCameraOperatingMode` =
`OMX_CaptureVideo` the component reaches Executing and fills nothing (the
Ducati goes idle and suspends 5 s later); with it the OV5680 is set to its
video mode (`Mode -> video 30fps [960x540]`) and frames come at the port's
rate. Harmless lines in trace1: "VTC Slice configured to 0 height", the
`AlgoAreas` NULL shared buffer, the unsupported index above, and "Buffer
pointer sent in FreeBuffer does not match" (the pointer field of
FreeBuffer is not translated by the kernel).

## The encoder

`OMX.TI.DUCATI1.VIDEO.H264E`. Port 0 in (default 176x144, stride 4096,
colour 0x27 = OMX_COLOR_FormatYUV420PackedSemiPlanar: TILER 2D NV12, the
camera's layout), port 1 out (coding 7, AVC). glass-camera sets the input
to the camera's size, stride 4096, its frame rate and buffer count; the
output to `nBitrate`; `OMX_IndexParamVideoBitrate` constant rate;
`OMX_IndexParamVideoAvc` Baseline, nPFrames = fps x 2 - 1, no B frames,
CAVLC. The camera's buffers are UseBuffer'd on the encoder's input as
well, the same share fds registered on the encoder's own connection
(handles are per connection), so a filled camera buffer is handed to the
encoder by EmptyThisBuffer as it is.

| fn | idx | arguments after map_info, map_offset |
| --- | --- | --- |
| EmptyThisBuffer | 14 | hComp, remote header, nFilledLen, nOffset, nFlags, nTimeStamp (8), hMarkTarget, pMarkData, nAllocLen, nOutputPortIndex, nInputPortIndex, [buffer, aux1 when map_info is set: Android's metadata mode, map_offset 56] |

The output is Annex-B (start codes), Constrained Baseline; the first buffer
carries SPS and PPS (flag CODECCONFIG), then an IDR. Measured 2026-10-07 at
960x540, 15/s, 768 kbit/s: 15.0 frames/s, 762 to 779 kbit/s, the A9 copying
only the bitstream.

## The decoder

**Works since 2026-11-04, bit-exact.** `tools/glass-decode`: 68 frames of `cam.flv` (this Ducati's own encoder output) compared against ffmpeg's decode, 0 differ, MD5 equal; 71 frames in about 1.1 s including setup. It is the monitor stream's decoder (`glass-decode | glass-fb -n`, `scripts/glass-view.sh`, `HW=0` for ffmpeg on the A9); the camera's heads-up display still decodes in software (concurrent encode and decode on the Ducati is untested).

**The fix: `bUsePortReconfigForPadding` is required, and its index is the firmware's own, `0x7F00007D`.** KitKat's header gives `OMX_TI_IndexParamUseEnhancedPortReconfig` as `0x7F000072`, which this firmware answers with 0x8000101a (unsupported). The real number is the sum of the subtraction chain in its SetParameter (0x1770fc to 0x177136), the same chain that reproduces PortDefinition 0x02000001, VideoPortFormat 0x06000001 and 2DBufferAllocDimension 0x7F000057. Without the flag the firmware takes a broken path in its output setup: at 0x14093c it writes the chroma's size over descs[0], not descs[1], so the codec is handed Y 768x240 and a nonsense UV, and refuses every frame with extended error 0x4000, `XDM_UNSUPPORTEDPARAM`, 0 bytes consumed. Other TI indices may be off the same way: this firmware's enum is not KitKat's. With the flag the port's frame size becomes the padded 768x480, the picture arrives as a crop change (640x360 at 32,24), and no second reconfiguration crashes anything.

**How to see inside:** the trace level is the word at Ducati 0x803B98B4, PA 0xF5FB98B4 (no symbols; 721 real checks read it, thresholds 0, 2, 3, 4, 5; default 2; 4 prints `[TRC]`). Write it with the Ducati suspended; a crash reloads the firmware and it returns to 2. Text PA = DA + 0xF5600000, data PA = DA - 0x80000000 + 0xF5C00000. The codec's live parameters can be read from memory while it sleeps. Never read `/sys/kernel/debug/ion/<pid>`: it oopses (`ion_debug_client_show`, strnlen) and leaves that client's lock held; reboot after it.

The bitstream buffers come from the ION carveout (heap mask 1 << 2), as TI's proxy does on ION builds, seen by the Ducati through IOBUFS at 0x88000000. Whether TILER 1D decodes with the fix is untested (`GLASS_DECODE_INPUT=tiler`). Before the fix both failed alike, so the carveout was not the cause.

What was seen before the fix, kept for the record:

`tools/glass-decode` (2026-10-07), kept for the next attempt; not built or
used by anything (the heads-up display decodes in software).

- `OMX.TI.DUCATI1.VIDEO.DECODER` (TI's one video decoder; `.H264D` is not a
  component name, 0x80001003), told its codec by SetParameter
  `OMX_IndexParamStandardComponentRole` "video_decoder.avc"; the firmware's
  H264D is 01.00.00.13. Port 0 bitstream in (TILER 1D, one frame a buffer),
  port 1 pictures out (TILER 2D NV12, as the camera's).
- Before any stream: input AVC profile 0x8, level 0x1000 (4.1), 1 reference;
  output 640x368 (from a 640x368 input), stride 4096, slice height 368,
  colour 0x27, `OMX_TI_IndexParam2DBufferAllocDimension` 768x480, 2949120
  bytes, at least 3 buffers. `OMX_TI_IndexParamUseEnhancedPortReconfig`
  (which TI's Android proxy sets) is unsupported here (0x8000101a).
- The first SPS brings a port change (port 1, nData2 0; trace1
  `omx_video_decoder_utils.c:[1236] Dynamic port reconfiguration
  triggered`), at least 7 buffers. The reconfiguration as Android does it
  (PortDisable, every buffer back, FreeBuffer, new buffers, PortEnable,
  FillThisBuffer) completes and the firmware makes its codec again
  (`2.Codec Create`).
- Then every frame fails in process() with extended error 0x4000, 0 bytes
  consumed: with the SPS and PPS as a configuration buffer, inline before
  the key frame, or not sent again; with NVENC's stream (level 2.2, 3
  references, VUI) and with this Ducati's own encoder output (level 4.1, 1
  reference) alike. The decoder then asks for another reconfiguration
  (`[1812] Number of buffers allocated are less than required ... Current
  nBufferCountActual = 7, New nBufferCountMin = 7`, `[1551]`).
- A PortDisable in that state, or the client ending then, crashes the
  firmware: `Exception occurred at (PC) = 00000000`, usage fault INVSTATE
  in the `OMX_Video_Decoder` task. remoteproc recovers it in about 2 s and
  glass-camera reconnects by itself; ten crashes on 2026-10-07, every one
  recovered.

Read 2026-11-04, nothing run:

- **Extended error 0x4000 is bit 14, `XDM_UNSUPPORTEDPARAM`**, "unsupported input parameter or configuration" (TI's `xdm.h`), without bit 15, `XDM_FATALERROR`. So the decoder refused its configuration, not the stream: which is why NVENC's and this Ducati's own bitstreams failed alike.
- The stride is not obviously it: TI's proxy (`omx_proxy_videodec.c`, `RPC_UTIL_SetStrideAndPadding`) sets `nStride` 4096 for 2D before Loaded to Idle and before PortEnable, as `glass-decode` does, and 4096 passes the firmware's own checks (`nStride & 0x7F == 0`, `nStride >= nFrameWidth`). TI's xdm.h names `displayWidth` as its example of this error; whether the firmware passes the stride on as one is unread.
- The firmware prints the answer at trace level: `[TRC] Stride value set on o/p port is %d`, `[TRC] Buffer provided to Codec with Padded Width = %d, Padded Height = %d`, `[TRC] Update the Output buffer details before the Codec Process call`. So the next attempt starts with the raised `traceLevel` above, not with another guess.
- `OMX_TI_IndexParamUseEnhancedPortReconfig` answering 0x8000101a is, by TI's forum, most often an index mismatch between DOMX and the firmware, and this firmware already differs from KitKat's headers. Other `OMX_TI_*` numbers may be off too.
- To separate the codec from the OMX layer: the same run with role `video_decoder.mpeg4`. 0x4000 there too means the fault is in the common output setup, not in H264D.
- Roles in this firmware: avc, h263, mpeg2, mpeg4, rv, sorspk, svc, vp6, vp7, wmv. Encoders: H264E, H264SVCE, MPEG4E. No JPEG or MJPEG component: VLCDJ is reached only inside the camera. No DCE: 0 strings in the firmware, 0 files in this kernel. Channels the firmware announces, live on the Glass: `rpmsg-omx0`, `rpmsg-omx1`, `omaprpc` (and two `rpmsg-client-sample`). `omaprpc` is a generic remote-call service: its driver is in the tree (`drivers/rpmsg/omaprpc`) but not in `notle_defconfig`, there is no `/dev` node, and what functions the firmware registers on it is unread. At idle `ivahd_pwrdm`, `cam_pwrdm`, `tesla_pwrdm` are OFF and `ipu_c0` is suspended; the only remoteproc is the Ducati, so the Tesla DSP has no way in on this kernel.

Power, 2026-11-04, `cam.flv` looped in real time from the desktop over ssh at 532 kbit/s, 12/s, on battery, panel dark, `power_save` off, two interleaved passes of 45 s at 1 Hz: idle 292.1 and 296.6 mW; delivered and discarded 529.4 and 526.1; ffmpeg software decode to null 706.0 and 705.2; hardware decode 658.6 and 650.3, copying every picture out to stdout. So the radio and ssh cost +233.3 mW, software decode +177.9, hardware decode +126.8: hardware saves 51.1 mW here. CPU0 spent 0.0 s of 45 in C4 during hardware decode against about 31 s in software decode; the firmware's trace shows `[IVAHD]: Requesting latency constraints 400`, and C3 and C4 have exit latencies of 768 and 978 us, which is the likely reason (inferred, the constraint was not read from the kernel). Not measured: a zero-copy path into a DSS overlay, a real display pipeline, latency, frame timing.

### The hold

The decoder keeps `num_ref_frames` pictures before it lets one out. On a live 12/s feed: 262 ms from frame in to picture out with NVENC's 3 references, 113 ms with 1 (`glass-decode`'s five-second lines). The picture leaves when the reference window pushes it out: in the trace `OutputId` equals `FreeBuffID` every cycle, rotating through refs + 1 buffers.

In the firmware that is `displayDelay` (static parameters +0x20, read live at Ducati 0x85C2A758 with `/dev/mem`, read only), written in two places: at 0x2C59BE, where a stream with more references than the port assumed sets it to `spsMaxRefFrames`, or to 0 (decode order) for `eProfile` Baseline at 0x2C59D2; and at 0x3218B6, in the function that computes buffer sizes, which always sets it to `spsMaxRefFrames` when that is known or `bUsePortReconfigForPadding` is on. The second runs after the first, so the 0 does not survive to the codec's re-creation.

Tried, without effect on the hold:
- `bitstream_restriction` with `max_num_reorder_frames` 0 in the SPS, in-band copies removed: 275 ms.
- `eProfile` Baseline and `nRefFrames` 1 in `OMX_IndexParamVideoAvc`: buffers 7 to 4 at 3 references, 3 to 2 at 1, hold the same.
- Output port at the padded 768x480 before starting: refused, 0x80001019.
- Input port at 768x480, to make the first output right: buffers become 896x576 and the reconfiguration still comes.
- A client-set `nBufferCountMin`: ignored.
- Bitstream buffers: 2 and 4 hold the same. One wedges: it is not returned, so keep at least 2.

So the sender uses one reference. `nvh264enc` has no property for it (ten preset and tune combinations: 3 every time); `h264_nvenc -dpb_size 1` gives `max_num_ref_frames` 1 with one active (`-refs 1` alone gives 2). Rewriting the SPS of a 3-reference stream would be wrong: its PPS defaults to 3 active references.

`h264_nvenc -rc cbr` fills to its target (3000 kbit/s on a picture `nvh264enc` CBR sends at 90, also with `-cbr_padding 0`), all of it radio. `-rc vbr -maxrate` behaves like `nvh264enc`:

| 120 frames, same source | smpte scroll, kbit/s, PSNR dB | ball, kbit/s, PSNR dB |
|---|---|---|
| `nvh264enc` CBR 3000 | 2229, 71.06 | 90, 85.84 |
| `h264_nvenc -rc cbr` | 3000, 71.02 | 3000, 85.82 |
| `-rc vbr -maxrate 3000k` | 2096, 68.65 | 91, 83.38 |
| `-rc vbr -cq 26 -maxrate 3000k` | 791, 41.25 | 18, 66.64 |
