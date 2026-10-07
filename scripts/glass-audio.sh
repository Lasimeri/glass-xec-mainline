#!/bin/bash
# glass-audio.sh: the desktop's sound in the Glass while the Glass is on the
# network, the stereo again when it is not.
#   glass-audio.sh            run (foreground; log $XDG_RUNTIME_DIR/glass-audio.log)
#   glass-audio.sh stop       end it; the stereo is the default again
#
# A PipeWire sink "Google Glass" (a null sink) is made the default output
# while the Glass answers; everything playing moves to it. The Glass plays it
# on its bone conduction transducer, or on the earbud while one is plugged
# in (glass-audio play picks, and follows plugging). Its monitor is
# read as 32 kHz mono 16-bit (64 KB/s; the bone conduction transducer is
# one channel) and sent down an ssh session to the Glass, where glass-audio
# play hands it to ALSA through the route chosen with glass-audio route.
# When the session ends (the Glass off or out of Wi-Fi), the previous
# default output comes back and the streams move with it; the sink stays,
# idle, until the Glass returns. Started at login by
# ~/.config/autostart/glass-audio.desktop; a second copy refuses to start.
#
# Sync with the picture: the picture reaches the glasses over 110 ms after
# the screen (one frame in the compositor, one of cushion on the Glass, the
# rest encode, link and decode). The sound's path is shorter, so the Glass
# holds it back, by a delay kept on the Glass itself (glass-audio delay MS
# there; default 150, the user's choice on 2026-10-07: easier on the Glass
# than 100, and synced on the Glass's end, not here). glass-play keeps that
# delay however the two clocks drift (the desktop's ran 0.05% fast: aplay
# had fallen 650 ms behind in twenty minutes).
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
run=${XDG_RUNTIME_DIR:-/tmp}
log=$run/glass-audio.log
sink=glass
# 32 kHz: all the bone conduction speaker reproduces (the user, 2026-10-07).
rate=32000

if [ "${1:-}" = stop ]; then
    for p in $(pgrep -f "scripts/glass-audio[.]sh$"); do kill "$p"; done 2> /dev/null
    exit 0
fi

exec 9> "$run/glass-audio.lock"
flock -n 9 || { echo "glass-audio: already running" >&2; exit 0; }

# The sink, once (by name; a second load would make a second one). Its
# monitor without the sink's volume (monitor.channel-volumes false): the
# stream leaves at full scale and the volume control is applied on the Glass
# (glass-audio control). A sink made before that (the monitor carrying the
# volume) is made again, once.
props='device.description="Google Glass" monitor.channel-volumes=false'
m=$(pactl list short modules | awk -v n="sink_name=$sink" '$2 == "module-null-sink" && index($0, n) { print $1; exit }')
if [ -n "$m" ] && ! pactl list short modules | awk -v m="$m" '$1 == m' | grep -q 'monitor.channel-volumes=false'; then
    pactl unload-module "$m"
    m=""
fi
if [ -z "$m" ]; then
    pactl load-module module-null-sink sink_name="$sink" sink_properties="$props" rate=48000 channels=2 > /dev/null
fi

# The desktop's volume control for the Glass: the slider and mute of the
# "Google Glass" output go to glass-audio control on the Glass, once at the
# start and on every change, and set the Glass's own level there.
volume_state() {
    echo "volume $(pactl get-sink-volume "$sink" | grep -o '[0-9]*%' | head -n 1 | tr -d %)"
    echo "mute $(pactl get-sink-mute "$sink" | awk '{ print ($2 == "yes") ? 1 : 0 }')"
}
volume_link() {   # ADDRESS
    local id
    id=$(pactl list short sinks | awk -v n="$sink" '$2 == n { print $1; exit }')
    {
        volume_state
        pactl subscribe 2> /dev/null | while read -r ev; do
            case "$ev" in *"'change' on sink #$id") volume_state ;; esac
        done
    } | GLASS_IP=$1 "$top/scripts/glass" ssh "glass-audio control" 2>> "$log"
}

prev=""
# Streams that follow the default: those of applications (a client) playing
# on FROM, moved to TO. Streams of modules (no client: the voice setup's
# echo canceller, loopbacks) are routed on purpose and never moved; moving
# every stream put the echo canceller's output on the Glass (2026-10-07).
move_streams() {   # FROM TO
    local from
    from=$(pactl list short sinks | awk -v n="$1" '$2 == n { print $1; exit }')
    [ -n "$from" ] || return 0
    for s in $(pactl list short sink-inputs | awk -v f="$from" '$2 == f && $3 != "-" { print $1 }'); do
        pactl move-sink-input "$s" "$2" 2> /dev/null
    done
}
restore() {
    [ -n "$prev" ] || return 0
    pactl set-default-sink "$prev"
    move_streams "$sink" "$prev"
    echo "glass-audio: $(date +%T) back to $prev" >> "$log"
    prev=""
}
trap 'for j in $(jobs -p); do kill -- -"$j" 2> /dev/null || kill "$j" 2> /dev/null; done; restore; exit 0' INT TERM

echo "glass-audio: $(date +%T) started" >> "$log"
# A real output to come back to: the stereo, else the first hardware sink.
real_output() {
    pactl list short sinks | awk '$2 ~ /^alsa_output\./ && $2 ~ /Schiit/ { print $2; f = 1; exit } END { exit !f }' ||
        pactl list short sinks | awk '$2 ~ /^alsa_output\./ { print $2; exit }'
}
# Left as the default by an earlier run that was killed mid-session: give
# the default back to a real output.
if [ "$(pactl get-default-sink)" = "$sink" ]; then
    prev=$(real_output)
    restore
fi
while :; do
    # Whenever the Glass answers: its own transducer needs nothing plugged
    # in. One address for the whole session (glass addr: home Wi-Fi, USB or
    # the tailnet).
    addr=$("$top/scripts/glass" addr 2> /dev/null || true)
    if [ -n "$addr" ] && GLASS_IP=$addr "$top/scripts/glass" ssh true < /dev/null > /dev/null 2>&1; then
        cur=$(pactl get-default-sink)
        # Already the Glass (a session that ended without giving it back):
        # the stereo is what comes back afterwards, never the Glass itself.
        [ "$cur" = "$sink" ] && cur=$(real_output)
        prev=$cur
        pactl set-default-sink "$sink"
        move_streams "$prev" "$sink"
        # In the background with a wait: a stop signal then runs the trap at
        # once (bash defers traps until a foreground pipeline ends).
        case "$addr" in
            100.6[4-9].* | 100.[7-9][0-9].* | 100.1[01][0-9].* | 100.12[0-7].*)
                # The tailnet (the hotspot, away): Opus at 48 kbit/s instead
                # of 768 kbit/s of raw samples; the Glass decodes it and
                # holds it by its remote delay (glass-audio delay remote).
                echo "glass-audio: $(date +%T) to the Glass over the tailnet at $addr, Opus 48 kbit/s (was $prev)" >> "$log"
                parec -d "$sink.monitor" --format=s16le --rate=$rate --channels=1 --latency-msec=10 --raw 2>> "$log" |
                    ffmpeg -hide_banner -loglevel error -f s16le -ar $rate -ac 1 -i - -c:a libopus -b:a 48k \
                        -application lowdelay -frame_duration 40 -f mpegts -muxdelay 0 -flush_packets 1 - 2>> "$log" |
                    GLASS_IP=$addr "$top/scripts/glass" ssh "glass-audio play opus" 2>> "$log" &
                ;;
            *)
                echo "glass-audio: $(date +%T) to the Glass at $addr (was $prev)" >> "$log"
                parec -d "$sink.monitor" --format=s16le --rate=$rate --channels=1 --latency-msec=10 --raw 2>> "$log" |
                    GLASS_IP=$addr "$top/scripts/glass" ssh "glass-audio play" 2>> "$log" &
                ;;
        esac
        session=$!
        # The volume link beside the session, in its own process group (set
        # -m) so that it ends whole with the session.
        set -m
        volume_link "$addr" &
        link=$!
        set +m
        # Ended when the Glass is on a better path or this one went away
        # (glass-pathguard.sh); the loop starts it again there, in that
        # path's mode (raw at home, Opus over the tailnet).
        "$top/scripts/glass-pathguard.sh" "$addr" "$session" >> "$log" 2>&1 &
        guard=$!
        wait $session
        kill -- -"$link" 2> /dev/null
        kill $guard 2> /dev/null
        restore
    fi
    sleep 5
done
