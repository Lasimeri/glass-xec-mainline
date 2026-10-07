#!/bin/bash
# glass-view.sh: the desktop's monitor on the Glass's display, live.
#   glass-view.sh [OUTPUT] [FPS]     default: the primary output (kscreen
#                                    priority 1, else DP-1), 24 frames/s
#
# Everything heavy happens on the desktop: the monitor comes as the
# compositor's own screencast (the desktop portal's PipeWire stream, opened
# by `desk cast` from ~/deskpilot: every frame KWin composes, no
# screenshots), GStreamer uploads it to the GPU, scales it to 640x360 and
# encodes H.264 with NVENC at BITRATE (default 3000 kbit/s), FPS frames/s.
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
bitrate=${BITRATE:-3000}
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
# The container: Matroska carries each frame's size, so the Glass releases a
# frame the moment it lands; MPEG-TS makes it wait for the next frame's start.
if gst-inspect-1.0 matroskamux > /dev/null 2>&1; then mux="matroskamux streamable=true"; else mux=mpegtsmux; fi
desk=${DESK:-$HOME/deskpilot/target/release/desk}
[ -x "$desk" ] || { echo "glass-view: no desk tool at $desk (DESK=...; cargo build --release in ~/deskpilot)" >&2; exit 1; }
command -v gst-launch-1.0 > /dev/null || { echo "glass-view: gst-launch-1.0 is not installed (gstreamer, gst-plugin-pipewire, gst-plugins-bad for nvcodec)" >&2; exit 1; }
for e in pipewiresrc cudaupload cudaconvertscale nvh264enc h264parse mpegtsmux; do
    gst-inspect-1.0 "$e" > /dev/null 2>&1 || { echo "glass-view: GStreamer element $e is missing" >&2; exit 1; }
done
# The monitor: as given, else the last one given (out/glass-output; the
# portal's remembered permission is for that one too), else the primary.
if [ -z "$out" ] && [ -s "$top/out/glass-output" ]; then
    out=$(head -n 1 "$top/out/glass-output")
fi
if [ -z "$out" ]; then
    out=$(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Output:/ { o=$3 } /priority 1/ { print o; exit }')
    [ -n "$out" ] || out=DP-1
fi
case "$out" in
    window | follow) ;;
    *)
        # Only a real monitor's name is taken (and remembered): a stray word,
        # such as a frame rate given as the first argument, is refused.
        if ! "${DESK:-$HOME/deskpilot/target/release/desk}" outputs 2>/dev/null | awk '{print $1}' | grep -qx "$out:"; then
            echo "glass-view: no monitor named '$out' (desk outputs: $("${DESK:-$HOME/deskpilot/target/release/desk}" outputs 2>/dev/null | awk -F: '/^[A-Z]/ && !/workspace/ {printf "%s ", $1}'); or window, follow)" >&2
            exit 1
        fi
        mkdir -p "$top/out"; echo "$out" > "$top/out/glass-output" ;;
esac
# OUTPUT "window": one window instead of a monitor (the portal's dialog asks
# which; the Glass screen of glass-screen.sh is the one meant), captured at
# the window's own size, so a 1280x720 window reaches the Glass at an exact
# 2:1 and its text at the size the nested session draws it.
castopt=()
[ "$out" = window ] && castopt=(--window)
# ASK=1: ask again in the dialog (another window or monitor than last time).
[ "${ASK:-0}" = 1 ] && castopt+=(--forget)
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
# The display writer on the Glass: glass-fb (tools/glass-fb, page flips on
# the vertical sync, tear-free) when the rootfs has it, else ffmpeg's own
# fbdev output (writes the visible page directly). With glass-fb, the frames
# go out as YUYV to the display controller's video overlay, which converts
# them to RGB itself (YUV=0 for the older BGRA path on the graphics layer):
# the Glass's CPU no longer converts every pixel, and copies half the bytes.
decode="$gff -hide_banner -loglevel warning -probesize 32 -analyzeduration 0 -max_delay 0 -fflags nobuffer -flags low_delay -threads 1 ${pace[*]} -i pipe:0 -fps_mode passthrough"
has_fb=0
for try in 1 2 3; do
    if "$top/scripts/glass" ssh 'test -x /usr/local/bin/glass-fb' < /dev/null > /dev/null 2>&1; then has_fb=1; break; fi
    sleep 1
done
if [ "$has_fb" = 1 ] && [ "${YUV:-1}" = 1 ]; then
    sink="$decode -pix_fmt yuv420p -f rawvideo - | /usr/local/bin/glass-fb -y -r $fps -b 1"
    echo "glass-view: the Glass shows YUYV frames on its video overlay (converted by the display controller), on its vertical sync, $fps/s with one frame of cushion (glass-fb)" >&2
elif [ "$has_fb" = 1 ]; then
    sink="$decode -pix_fmt bgra -f rawvideo - | /usr/local/bin/glass-fb -r $fps -b 1"
    echo "glass-view: the Glass shows frames on its vertical sync, on a $fps/s schedule with one frame of cushion (glass-fb)" >&2
else
    sink="$decode -pix_fmt bgra -f fbdev /dev/fb0"
    echo "glass-view: no glass-fb on the Glass: ffmpeg writes the visible page directly (no sync, no pacing)" >&2
fi

# OUTPUT "follow": pixel perfect. A 640x360 window of the primary monitor,
# one pixel to one pixel, that follows the pointer (desk cursor, through a
# KWin script) and pans when the pointer nears its edge: the glasses show
# what you are looking at, at the size the desktop draws it.
# tools/glass-viewport (C, GStreamer) runs the GPU pipeline with the moving
# window; built here on first use.
if [ "$out" = follow ]; then
    out=$(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Output:/ { o=$3 } /priority 1/ { print o; exit }')
    [ -n "$out" ] || out=DP-1
    geo=$("$desk" outputs 2>/dev/null | awk -v o="$out:" '$1 == o { print $2, $4 }')
    ow=${geo%% *}; pos=${geo##* }; pos=${pos%,}; ow=${ow%x*}; oh=${geo%% *}; oh=${oh#*x}; ox=${pos%,*}; oy=${pos#*,}
    [ -n "$ow" ] && [ -n "$oh" ] || { echo "glass-view: no geometry for $out (desk outputs)" >&2; exit 1; }
    vp=$top/build/glass-viewport
    if [ ! -x "$vp" ] || [ "$top/tools/glass-viewport/glass-viewport.c" -nt "$vp" ]; then
        mkdir -p "$top/build"
        gcc -O2 -o "$vp" "$top/tools/glass-viewport/glass-viewport.c" $(pkg-config --cflags --libs gstreamer-1.0) -lpthread || exit 1
    fi
    glass_ffmpeg || exit 1
    fifo=$(mktemp -u "${XDG_RUNTIME_DIR:-/tmp}/glass-cursor.XXXXXX")
    mkfifo "$fifo"
    trap 'rm -f "$fifo"' EXIT
    "$desk" cursor > "$fifo" &
    cur=$!
    trap 'kill $cur 2> /dev/null; rm -f "$fifo"' EXIT
    # VIEW=WxH: the region of the monitor shown (default 640x360: one pixel to
    # one pixel; 1280x720: a 720p area at an exact 2:1).
    view=${VIEW:-${W}x${H}}
    vw=${view%x*}; vh=${view#*x}
    echo "glass-view: $out -> a ${vw}x${vh} window following the pointer, shown at ${W}x${H} ($((vw / W)):1), NVENC H.264 ${bitrate} kbit/s, $fps frames/s -> Glass over ssh; Ctrl-C stops" >&2
    "$desk" cast -- "$vp" @FD@ @NODE@ "$ow" "$oh" "$ox" "$oy" "$vw" "$vh" "$fps" "$bitrate" "$fifo" \
        | "$top/scripts/glass" ssh "$sink"
    exit
fi
# One rate stage, on the GPU: the capture asks KWin for at most SRC_FPS
# (default  9/8 of FPS; max-framerate is honoured, a fixed framerate is refused, and
# KWin under-delivers against the cap, so asking a little above FPS keeps
# every output slot fed with a fresh frame) and re-sends its last frame
# while the screen is still; a queue takes frames off PipeWire's thread at
# once; the CUDA compositor at latency 0 emits exactly FPS frames a second
# on its own deadline (the newest frame for each slot, the extras dropped),
# which is what the Glass presents on its fixed schedule (glass-fb).
#
# The downscale: the CUDA scalers are bilinear only, and one bilinear pass
# at 6:1 (3840x2160 to 640x360) skips pixels, so text shimmers. Halving
# steps first (bilinear at exactly 2:1 averages each 2x2 block, a true box
# filter), then the compositor's last step is mild (960 to 640, 1.5:1).
# Measured on a 144 Hz monitor (2026-10-07, KWin 6.7, left monitor): a cap of
# 30 delivers 26.8/s, so the 24/s compositor skips about 3 source frames a
# second (motion jumps); a cap of 27 delivers 24.45/s, 77% of intervals
# exactly 6 refreshes (41.67 ms), so it skips about one frame every 2 s;
# 24 under-delivers (22.4/s, repeats). Default: 9/8 of the stream rate.
src_fps=${SRC_FPS:-$(( fps * 9 / 8 ))}
steps=()
if [ "$out" != window ]; then
    sg=$("$desk" outputs 2>/dev/null | awk -v o="$out:" '$1 == o { print $2 }')
    sw=${sg%x*}; sh=${sg#*x}
    if [ -n "$sw" ] && [ -n "$sh" ]; then
        while [ $((sw / 2)) -ge "$W" ] && [ $((sh / 2)) -ge "$H" ]; do
            sw=$((sw / 2)); sh=$((sh / 2))
            steps+=(! cudascale ! "video/x-raw(memory:CUDAMemory),width=$sw,height=$sh")
        done
    fi
fi
for e in cudacompositor cudascale; do
    gst-inspect-1.0 "$e" > /dev/null 2>&1 || { echo "glass-view: GStreamer element $e is missing" >&2; exit 1; }
done
echo "glass-view: $out -> portal screencast (at most $src_fps/s), GPU downscale in $((${#steps[@]} / 4)) halving step(s) then to ${W}x${H}, $fps frames/s from the compositor, NVENC H.264 ${bitrate} kbit/s -> Glass over ssh; Ctrl-C stops" >&2
if [ "$out" = window ]; then
    echo "glass-view: the first run asks in the portal's dialog which window to share: pick the Glass screen (KDE Wayland Compositor)" >&2
else
    echo "glass-view: the first run asks in the portal's dialog which monitor to share: pick $out" >&2
fi
"$desk" cast "${castopt[@]}" -- gst-launch-1.0 -q \
    pipewiresrc fd=@FD@ path=@NODE@ do-timestamp=true keepalive-time=$((1000 / fps)) ! "video/x-raw,max-framerate=$src_fps/1" \
    ! queue max-size-buffers=2 max-size-time=0 max-size-bytes=0 leaky=downstream \
    ! cudaupload "${steps[@]}" \
    ! cudacompositor latency=0 sink_0::xpos=0 sink_0::ypos=0 sink_0::width="$W" sink_0::height="$H" \
    ! "video/x-raw(memory:CUDAMemory),width=$W,height=$H,framerate=$fps/1" \
    ! cudaconvertscale ! "video/x-raw(memory:CUDAMemory),width=$W,height=$H,format=NV12" \
    ! nvh264enc preset=p1 tune=ultra-low-latency rc-mode=cbr bitrate="$bitrate" vbv-buffer-size=$((bitrate / fps)) gop-size="$fps" zerolatency=true bframes=0 rc-lookahead=0 \
    ! h264parse ! $mux ! fdsink fd=1 sync=false \
    | "$top/scripts/glass" ssh "$sink"
