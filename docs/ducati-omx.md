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
IOBUFS maps da 0x88000000 to pa 0xfc000000 (60 MB), near but not exactly
the secure_input heap (0xfc100000 by the board file's arithmetic). So every
buffer handed to the Ducati is a TILER buffer: 2D for pictures, 1D (page
format) for the rest. The kernel translates a registered handle to the
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
