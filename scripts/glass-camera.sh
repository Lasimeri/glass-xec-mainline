#!/bin/bash
# glass-camera.sh: the Glass's camera in its own window on this desktop.
#   glass-camera.sh [WxH] [FPS] [KBIT/S]     (glass camera; 960x540 15 768)
#
# On the Glass, glass-camera (tools/glass-camera, docs/ducati-omx.md) runs
# the camera and the H.264 encoder on the Ducati: the camera's frames go to
# the encoder in place (TILER buffers both share), the A9 only passes on
# the few kilobytes each frame becomes. 960x540 is half the camera's video
# size (1920x1080; its stills go to 2592x1944), 768 kbit/s at 15 frames a
# second, no sound (the user's choice, 2026-10-07). The stream comes down
# the ssh session (the Glass's address as glass addr finds it: home Wi-Fi,
# USB or the tailnet) and mpv shows each frame as it arrives (untimed, no
# cache); ffplay when there is no mpv. Closing the window ends the capture
# on the Glass: the session closes, glass-camera's next write fails and it
# puts the camera and the encoder back to Loaded. A run left behind by a
# cut connection is taken over by the next one.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
size=${1:-960x540}
fps=${2:-15}
kbps=${3:-768}
case "$size" in
    [0-9]*x[0-9]*) ;;
    *) echo "glass camera [WxH] [FPS] [KBIT/S]   (960x540 15 768)" >&2; exit 1 ;;
esac
w=${size%x*}
h=${size#*x}
addr=${GLASS_IP:-$("$top/scripts/glass" addr)} || exit 1
title="Glass camera ($size, $fps/s, $kbps kbit/s)"
cmd="exec /usr/local/bin/glass-camera -w $w -h $h -r $fps -k $kbps"
echo "glass camera: $size at $fps/s, $kbps kbit/s, from $addr; close the window to stop" >&2
if command -v mpv > /dev/null; then
    GLASS_IP=$addr "$top/scripts/glass" ssh "$cmd" < /dev/null |
        mpv --really-quiet --title="$title" --profile=low-latency --untimed --no-cache \
            --demuxer-lavf-format=h264 --demuxer-lavf-probesize=32 --demuxer-lavf-analyzeduration=0 \
            --keep-open=no -
else
    GLASS_IP=$addr "$top/scripts/glass" ssh "$cmd" < /dev/null |
        ffplay -hide_banner -loglevel error -window_title "$title" -f h264 -framerate "$fps" \
            -fflags nobuffer -flags low_delay -framedrop -probesize 32 -analyzeduration 0 -i -
fi
