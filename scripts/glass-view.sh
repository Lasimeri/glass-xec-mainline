#!/bin/bash
# glass-view.sh: the desktop's monitor on the Glass's display, live.
#   glass-view.sh [OUTPUT] [FPS]     default: the primary output (kscreen
#                                    priority 1, else DP-1), 30 frames/s
#
# Everything heavy happens on the desktop: the monitor is read straight from
# the display controller (ffmpeg's kmsgrab, the DRM card that drives OUTPUT),
# scaled to 640x360 on the GPU (scale_cuda) and encoded by NVENC as H.264 at
# BITRATE (default 1500k). The stream rides the ssh session to the Glass
# (encrypted, no listener on either side; the desktop's firewall blocks
# inbound on the USB link anyway), where a static ffmpeg in RAM (/tmp/ffmpeg,
# pushed from dl/ffmpeg-arm if missing) decodes it into /dev/fb0. Measured
# 2026-10-07: the Glass decodes 640x360 H.264 at 65 frames/s on its two
# Cortex-A9 cores; raw frames would need 28 MB/s and the link carries 3.5.
#
# kmsgrab needs CAP_SYS_ADMIN: a private copy of ffmpeg carries it,
#   sudo install -m 755 /usr/bin/ffmpeg /usr/local/bin/ffmpeg-kms
#   sudo setcap cap_sys_admin+ep /usr/local/bin/ffmpeg-kms
# Without it the script falls back to `desk shot` (KWin's ScreenShot2) at the
# rate KWin gives (about 10 frames/s), same GPU encode and transport.
# Ctrl-C stops; the Glass keeps the last frame.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
W=640; H=360
out=${1:-}
fps=${2:-30}
bitrate=${BITRATE:-1500k}
kms=${FFMPEG_KMS:-/usr/local/bin/ffmpeg-kms}
desk=${DESK:-$HOME/deskpilot/target/release/desk}
command -v ffmpeg > /dev/null || { echo "glass-view: ffmpeg is not installed" >&2; exit 1; }
if [ -z "$out" ]; then
    out=$(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Output:/ { o=$3 } /priority 1/ { print o; exit }')
    [ -n "$out" ] || out=DP-1
fi

# The Glass side: its ffmpeg, pushed into RAM once per boot.
glass_ffmpeg() {
    "$top/scripts/glass" ssh 'test -x /tmp/ffmpeg' 2> /dev/null && return 0
    local f
    f=$(ls -d "$top"/dl/ffmpeg-arm/ffmpeg-*-armhf-static/ffmpeg 2> /dev/null | head -n 1)
    [ -n "$f" ] || { echo "glass-view: no dl/ffmpeg-arm/ffmpeg-*-armhf-static/ffmpeg (glass fetch)" >&2; return 1; }
    echo "glass-view: pushing ffmpeg ($(stat -c %s "$f") bytes) into the Glass's RAM" >&2
    "$top/scripts/glass" ssh 'cat > /tmp/ffmpeg && chmod +x /tmp/ffmpeg' < "$f"
}
glass_ffmpeg || exit 1

# The DRM card behind OUTPUT (/sys/class/drm/cardN-OUTPUT).
card=$(ls -d /sys/class/drm/card*-"$out" 2> /dev/null | head -n 1 | sed 's|.*/\(card[0-9]*\)-.*|/dev/dri/\1|')

enc=(-c:v h264_nvenc -preset p1 -tune ull -profile:v baseline -bf 0 -g "$fps" -b:v "$bitrate" -maxrate "$bitrate" -bufsize "$bitrate" -f mpegts -)
sink=("$top/scripts/glass" ssh "/tmp/ffmpeg -hide_banner -loglevel warning -fflags nobuffer -flags low_delay -threads 2 -i pipe:0 -pix_fmt bgra -f fbdev /dev/fb0")

if [ -x "$kms" ] && getcap "$kms" 2> /dev/null | grep -q cap_sys_admin && [ -n "$card" ]; then
    echo "glass-view: $out ($card) -> kmsgrab, scale_cuda ${W}x${H}, h264_nvenc $bitrate, $fps frames/s -> Glass over ssh; Ctrl-C stops" >&2
    # kmsgrab hands DRM frame buffers to CUDA without a CPU copy (hwmap).
    "$kms" -hide_banner -loglevel warning -init_hw_device cuda=cu -filter_hw_device cu \
        -f kmsgrab -device "$card" -framerate "$fps" -i - \
        -vf "hwmap=derive_device=cuda,scale_cuda=$W:$H" "${enc[@]}" | "${sink[@]}"
else
    [ -x "$desk" ] || { echo "glass-view: neither $kms with cap_sys_admin nor $desk: no way to capture" >&2; exit 1; }
    echo "glass-view: $out -> desk shot (no $kms with cap_sys_admin), ${W}x${H} h264_nvenc $bitrate -> Glass over ssh; Ctrl-C stops" >&2
    dir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/glass-view.XXXXXX")
    trap 'rm -rf "$dir"' EXIT
    shots() {
        local f
        while :; do
            f=$("$desk" shot --screen "$out" --no-cursor --dir "$dir" 2> /dev/null | awk '{ print $1 }' | head -n 1)
            [ -n "$f" ] && [ -f "$f" ] || { sleep 1; continue; }
            cat "$f" || break
            rm -f "$f"
        done
    }
    shots | ffmpeg -hide_banner -loglevel warning -f image2pipe -vcodec png -r 10 -i - \
        -vf "scale=$W:$H:flags=bilinear" "${enc[@]}" | "${sink[@]}"
fi
