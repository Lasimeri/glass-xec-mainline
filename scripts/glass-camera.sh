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
# an ssh session (the Glass's address as glass addr finds it: home Wi-Fi,
# USB or the tailnet) into one mpv window that shows each frame as it
# arrives (untimed, no cache); ffplay when there is no mpv.
#
# The window outlives the path: a guard (glass-pathguard.sh) ends the
# session when the Glass moves (the cable unplugged, Wi-Fi back, the
# tailnet), and a new one starts on the new path into the same window (a
# FIFO held open here, so the player never sees an end; every session
# begins with the stream's parameter sets and a key frame). Closing the
# window ends it all: the session closes, glass-camera's next write fails
# and it puts the camera and the encoder back to Loaded. A run left behind
# by a cut connection is taken over by the next one on the Glass.
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
run=${XDG_RUNTIME_DIR:-/tmp}
log=$run/glass-camera.log
title="Glass camera ($size, $fps/s, $kbps kbit/s)"
cmd="exec /usr/local/bin/glass-camera -w $w -h $h -r $fps -k $kbps"
fifo=$(mktemp -u "$run/glass-camera.XXXXXX")
mkfifo "$fifo" || exit 1
# Face tracking, as the desktop's own camera has it (~/tts079 facetrack,
# cam079's inside view): the Glass cannot, so the desktop's RTX 3090 Ti
# does it all. Its video decoder (NVDEC, ffmpeg -hwaccel cuda) turns the
# H.264 into pictures, facetrack finds faces with YuNet through OpenCL on
# the same card, locks a reticle on the nearest ("SUBJECT: ADMIN"), lifts
# low light, and hands the annotated picture to mpv with the same scalers
# and FSR as the other camera windows, on the camera screen. Like the
# other cameras it keeps a status line a second for the Phi Stream's
# camera monitor (feeds/glass.status, .log) and its newest frame
# (~/.cache/voice-079/cam/latest-glass.jpg). GLASS_CAMERA_TRACK=0 shows the
# camera as it is; without facetrack the plain window is used.
t079=${TTS079:-$HOME/tts079}
ft=${FACETRACK:-$t079/facetrack}
if [ "${GLASS_CAMERA_TRACK:-1}" != 0 ] && [ -x "$ft" ] && command -v mpv > /dev/null; then
    feeds=${CAM079_FEEDS:-$HOME/.local/share/phi-stream/dev/feeds}
    snapdir=$HOME/.cache/voice-079/cam
    mkdir -p "$feeds" "$snapdir"
    screen=${CAM079_SCREEN:-$("$t079/state079" get screen DP-2 2> /dev/null || echo DP-2)}
    fsr=()
    [ -f "$t079/shaders/FSR.glsl" ] && fsr=(--glsl-shaders="$t079/shaders/FSR.glsl")
    (
        ffmpeg -hide_banner -loglevel error -nostdin -hwaccel cuda -fflags nobuffer -flags low_delay \
            -probesize 32 -analyzeduration 0 -f h264 -i - -fps_mode passthrough -f rawvideo -pix_fmt bgr24 - < "$fifo" 2>> "$log" |
            FACETRACK_STATUS="$feeds/glass" FACETRACK_CAM=glass FACETRACK_SNAP="$snapdir/latest-glass.jpg" \
                OPENCV_THREAD_POOL_ACTIVE_WAIT_WORKER=0 OPENCV_THREAD_POOL_ACTIVE_WAIT_MAIN=0 \
                OPENCV_THREAD_POOL_ACTIVE_WAIT_PAUSE_LIMIT=0 OPENCV_FOR_THREADS_NUM="${FACETRACK_THREADS:-4}" \
                "$ft" - "$w" "$h" "SUBJECT: ADMIN" 2>> "$log" |
            mpv --really-quiet --title="079 glass" --profile=low-latency --untimed --no-cache \
                --scale=ewa_lanczossharp --cscale=ewa_lanczossharp --dscale=mitchell \
                --correct-downscaling=yes --linear-downscaling=yes --sigmoid-upscaling=yes \
                --dither-depth=auto --temporal-dither=yes "${fsr[@]}" --screen-name="$screen" -
    ) &
elif command -v mpv > /dev/null; then
    mpv --really-quiet --title="$title" --profile=low-latency --untimed --no-cache \
        --demuxer-lavf-format=h264 --demuxer-lavf-probesize=32 --demuxer-lavf-analyzeduration=0 \
        --keep-open=no - < "$fifo" &
else
    ffplay -hide_banner -loglevel error -window_title "$title" -f h264 -framerate "$fps" \
        -fflags nobuffer -flags low_delay -framedrop -probesize 32 -analyzeduration 0 -i - < "$fifo" &
fi
player=$!
exec 7> "$fifo"      # held for the window's life: no end between sessions
rm -f "$fifo"
session=""
trap '[ -n "$session" ] && kill -- -"$session" 2> /dev/null; kill $player 2> /dev/null; exit 0' INT TERM
echo "glass camera: $size at $fps/s, $kbps kbit/s; close the window to stop (log: $log)" >&2
while kill -0 $player 2> /dev/null; do
    addr=${GLASS_IP:-$("$top/scripts/glass" addr 2> /dev/null)}
    if [ -z "$addr" ]; then sleep 2; continue; fi
    echo "glass-camera: $(date +%T) from $addr" >> "$log"
    set -m
    GLASS_IP=$addr "$top/scripts/glass" ssh "$cmd" < /dev/null >&7 2>> "$log" &
    session=$!
    set +m
    "$top/scripts/glass-pathguard.sh" "$addr" "-$session" >> "$log" 2>&1 &
    guard=$!
    # The session ends by itself (the path went, the guard ended it) or
    # the window was closed: then it is ended here.
    while kill -0 $session 2> /dev/null; do
        kill -0 $player 2> /dev/null || { kill -- -"$session" 2> /dev/null; break; }
        sleep 1
    done
    wait $session 2> /dev/null
    kill $guard 2> /dev/null
    session=""
    # Only GLASS_IP given on the command line pins the path; otherwise the
    # next session asks again where the Glass is.
    kill -0 $player 2> /dev/null && sleep 1
done
exec 7>&-
exit 0
