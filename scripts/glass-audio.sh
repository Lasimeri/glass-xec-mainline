#!/bin/bash
# glass-audio.sh: the desktop's sound in the Glass while the Glass is on the
# network, the stereo again when it is not.
#   glass-audio.sh            run (foreground; log $XDG_RUNTIME_DIR/glass-audio.log)
#   glass-audio.sh stop       end it; the stereo is the default again
#
# A PipeWire sink "Google Glass" (a null sink) is made the default output
# while the Glass answers; everything playing moves to it. Its monitor is
# read as 48 kHz mono 16-bit (96 KB/s; the bone conduction transducer is
# one channel) and sent down an ssh session to the Glass, where glass-audio
# play hands it to ALSA through the route chosen with glass-audio route.
# When the session ends (the Glass off or out of Wi-Fi), the previous
# default output comes back and the streams move with it; the sink stays,
# idle, until the Glass returns. Started at login by
# ~/.config/autostart/glass-audio.desktop; a second copy refuses to start.
#
# Sync with the picture: the picture reaches the glasses about 110 ms after
# the screen (one frame in the compositor, one of cushion on the Glass, the
# rest encode, link and decode). The sound's path is shorter, so the Glass
# holds it in a buffer of AUDIO_MS (default 100) to land with the picture.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
run=${XDG_RUNTIME_DIR:-/tmp}
log=$run/glass-audio.log
sink=glass
audio_ms=${AUDIO_MS:-100}

if [ "${1:-}" = stop ]; then
    for p in $(pgrep -f "scripts/glass-audio[.]sh$"); do kill "$p"; done 2> /dev/null
    exit 0
fi

exec 9> "$run/glass-audio.lock"
flock -n 9 || { echo "glass-audio: already running" >&2; exit 0; }

# The sink, once (by name; a second load would make a second one).
if ! pactl list short sinks | awk '{print $2}' | grep -qx "$sink"; then
    pactl load-module module-null-sink sink_name="$sink" \
        sink_properties='device.description="Google Glass"' rate=48000 channels=2 > /dev/null
fi

prev=""
restore() {
    [ -n "$prev" ] || return 0
    pactl set-default-sink "$prev"
    for s in $(pactl list short sink-inputs | awk '{print $1}'); do pactl move-sink-input "$s" "$prev" 2> /dev/null; done
    echo "glass-audio: $(date +%T) back to $prev" >> "$log"
    prev=""
}
trap 'restore; exit 0' INT TERM

echo "glass-audio: $(date +%T) started, buffer $audio_ms ms" >> "$log"
while :; do
    if "$top/scripts/glass" ssh true < /dev/null > /dev/null 2>&1; then
        cur=$(pactl get-default-sink)
        [ "$cur" = "$sink" ] || prev=$cur
        pactl set-default-sink "$sink"
        for s in $(pactl list short sink-inputs | awk '{print $1}'); do pactl move-sink-input "$s" "$sink" 2> /dev/null; done
        echo "glass-audio: $(date +%T) to the Glass (was $prev)" >> "$log"
        parec -d "$sink.monitor" --format=s16le --rate=48000 --channels=1 --latency-msec=10 --raw 2>> "$log" |
            "$top/scripts/glass" ssh "BUFFER_US=$((audio_ms * 1000)) glass-audio play" 2>> "$log"
        restore
    fi
    sleep 5
done
