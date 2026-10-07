#!/bin/bash
# glass-view.sh: the desktop's monitor on the Glass's display, live.
#   glass-view.sh [OUTPUT] [FPS]     default: the primary output (kscreen
#                                    priority 1, else DP-1), 24 frames/s
#
# Everything heavy happens on the desktop: the monitor comes as the
# compositor's own screencast (the desktop portal's PipeWire stream, opened
# by `desk cast` from ~/deskpilot: every frame KWin composes, no
# screenshots), GStreamer uploads it to the GPU, scales it to 640x360 and
# encodes H.264 with NVENC at BITRATE (default 1500 kbit/s), FPS frames/s.
# The stream rides the ssh session to the Glass (encrypted, no listener on
# either side; the desktop's firewall blocks inbound on the USB link
# anyway), where a static ffmpeg in RAM (/tmp/ffmpeg, pushed from
# dl/ffmpeg-arm if missing) decodes it into /dev/fb0. Measured 2026-10-07:
# the Glass decodes 640x360 H.264 at 65 frames/s on its two Cortex-A9
# cores; raw frames would need 28 MB/s and the link carries 3.5.
#
# The first run shows the portal's share dialog: pick the monitor; the
# answer is remembered (~/.cache/desk/cast.token; `desk cast --forget`
# asks again). kmsgrab (the kernel path) is closed on this NVIDIA driver:
# the scanout is a 16-bit float surface in a tiled layout ffmpeg cannot
# read, and its buffers cannot be mapped into CUDA from there.
# Ctrl-C stops; the Glass keeps the last frame.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
W=640; H=360
out=${1:-}
fps=${2:-24}
bitrate=${BITRATE:-1500}
bitrate=${bitrate%k}
# BUFFER_MS: a jitter buffer on the Glass for a link that jitters (Wi-Fi):
# the decoder takes that much lead at the start and then paces display by
# the stream's own timestamps, so a frame that arrives late by less than the
# lead still shows on time. 0 (the cable's default) draws on arrival; one
# frame (42 at 24 frames/s) is the setting for the air; 1 would absorb
# nothing, the frames being 42 ms apart.
buffer_ms=${BUFFER_MS:-0}
pace=()
if [ "$buffer_ms" -gt 0 ] 2> /dev/null; then
    pace=(-re -readrate_initial_burst "$(awk -v m="$buffer_ms" 'BEGIN { printf "%.3f", m / 1000 }')")
fi
# PACE=1 (the default): frames on an exact FPS grid. The portal sends a
# frame only when the compositor draws one, so the source re-sends its last
# frame every 1/FPS while the screen is still (keepalive-time), and
# videorate puts every frame on the grid with timestamps to match; the
# Glass then draws at even intervals. The hold is one compositor interval
# (7 ms at 144 Hz; up to 1/FPS while the screen is still). PACE=0 lets
# frames through as they come, at most FPS of them, never held.
pace_src=${PACE:-1}
if [ "$pace_src" = 0 ]; then
    rate=(! videorate drop-only=true max-rate="$fps")
    keep=()
else
    rate=(! videorate ! "video/x-raw,framerate=$fps/1")
    keep=(keepalive-time=$((1000 / fps)))
fi
desk=${DESK:-$HOME/deskpilot/target/release/desk}
[ -x "$desk" ] || { echo "glass-view: no desk tool at $desk (DESK=...; cargo build --release in ~/deskpilot)" >&2; exit 1; }
command -v gst-launch-1.0 > /dev/null || { echo "glass-view: gst-launch-1.0 is not installed (gstreamer, gst-plugin-pipewire, gst-plugins-bad for nvcodec)" >&2; exit 1; }
for e in pipewiresrc cudaupload cudaconvertscale nvh264enc h264parse mpegtsmux; do
    gst-inspect-1.0 "$e" > /dev/null 2>&1 || { echo "glass-view: GStreamer element $e is missing" >&2; exit 1; }
done
if [ -z "$out" ]; then
    out=$(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Output:/ { o=$3 } /priority 1/ { print o; exit }')
    [ -n "$out" ] || out=DP-1
fi

# The Glass side: its ffmpeg, on the rootfs (/usr/local/bin, built in), or
# pushed into RAM once per boot on an initramfs-only Glass.
gff=/usr/local/bin/ffmpeg
glass_ffmpeg() {
    "$top/scripts/glass" ssh "test -x $gff" 2> /dev/null && return 0
    gff=/tmp/ffmpeg
    "$top/scripts/glass" ssh "test -x $gff" 2> /dev/null && return 0
    local f
    f=$(ls -d "$top"/dl/ffmpeg-arm/ffmpeg-*-armhf-static/ffmpeg 2> /dev/null | head -n 1)
    [ -n "$f" ] || { echo "glass-view: no dl/ffmpeg-arm/ffmpeg-*-armhf-static/ffmpeg (glass fetch)" >&2; return 1; }
    echo "glass-view: pushing ffmpeg ($(stat -c %s "$f") bytes) into the Glass's RAM" >&2
    "$top/scripts/glass" ssh "cat > $gff && chmod +x $gff" < "$f"
}
glass_ffmpeg || exit 1

# Nothing holds a frame anywhere: the portal sends a frame when the
# compositor draws one (up to the monitor's rate), videorate in drop-only
# mode lets at most FPS of them through and keeps none back (a plain
# videorate would hold each frame until the next arrives), the encoder
# emits every frame as it is encoded (zero latency, no B-frames, constant
# rate), the muxer packetizes per frame, the Glass probes nothing before
# drawing, decodes on one thread (frame threads delay output by one frame
# each; one Cortex-A9 decodes this size at over 30 frames/s) and draws each
# frame as it arrives. Expected glass-to-glass: about a tenth of a second.
echo "glass-view: $out -> portal screencast, GPU scale ${W}x${H}, NVENC H.264 ${bitrate} kbit/s, $fps frames/s $([ "$pace_src" = 0 ] && echo "at most, unpaced" || echo "paced") -> Glass over ssh, jitter buffer ${buffer_ms} ms; Ctrl-C stops" >&2
echo "glass-view: the first run asks in the portal's dialog which monitor to share: pick $out" >&2
"$desk" cast -- gst-launch-1.0 -q \
    pipewiresrc fd=@FD@ path=@NODE@ do-timestamp=true "${keep[@]}" ! "video/x-raw" \
    "${rate[@]}" \
    ! cudaupload ! cudaconvertscale ! "video/x-raw(memory:CUDAMemory),width=$W,height=$H,format=NV12" \
    ! nvh264enc preset=p1 tune=ultra-low-latency rc-mode=cbr bitrate="$bitrate" gop-size="$fps" zerolatency=true bframes=0 \
    ! h264parse ! mpegtsmux ! fdsink fd=1 sync=false \
    | "$top/scripts/glass" ssh "$gff -hide_banner -loglevel warning -probesize 32 -analyzeduration 0 -fflags nobuffer -flags low_delay -threads 1 ${pace[*]} -i pipe:0 -fps_mode passthrough -pix_fmt bgra -f fbdev /dev/fb0"
