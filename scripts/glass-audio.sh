#!/bin/bash
# glass-audio.sh: the desktop's sound and 079's voice in the Glass while the
# Glass is connected, the stereo again when it is not; a switch back to the
# stereo, and automatic switching off while developing.
#   glass-audio.sh                 run (the supervisor; log $XDG_RUNTIME_DIR/glass-audio.log)
#   glass-audio.sh stop            end it; the stereo is the default again
#   glass-audio.sh status          where the sound goes, automatic or not, the Glass connected or not
#   glass-audio.sh glass           the sound to the Glass now, automatic switching on
#   glass-audio.sh stereo          the sound to the stereo now, automatic switching off (it stays)
#   glass-audio.sh auto on|off     automatic switching on (follows the Glass) or off (nothing moves)
#   glass-audio.sh toggle          automatic switching flipped (a desktop notice says which)
# (scripts/glass audio ... is the same.)
#
# A PipeWire sink "Google Glass" (a null sink) is made the default output
# while the Glass answers; everything playing moves to it, 079's voice (the
# echo canceller's playback) too (the user, 2026-10-07: "Desktop and voice
# through the glasses unless disconnected, however I should have the option
# of switching back to my stereo, and you need to make a toggle to quickly
# disable automatic audio switching in order to prevent constant switches in
# audio routing when developing"). The Glass plays it on its bone conduction
# transducer, or on the earbud while one is plugged in (glass-audio play
# picks, and follows plugging). Its monitor is read as 32 kHz mono 16-bit
# (64 KB/s; the bone conduction transducer is one channel) and sent down an
# ssh session to the Glass, where glass-audio play hands it to ALSA through
# the route chosen with glass-audio route. A session ended by a change of
# path (the cable out, Wi-Fi back) starts again on the new path with the
# sound left on the Glass; only when the Glass is gone does the stereo
# become the default again, the streams with it; the sink stays, idle,
# until the Glass returns. Automatic switching off (auto off, stereo): the
# supervisor still plays whatever is sent to "Google Glass" on the Glass,
# but moves nothing, ever; kept across logins
# (~/.local/state/glass-xec/audio-auto). Started at login by
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
# The address of the Glass while a session plays (status, glass, toggle).
live=$run/glass-audio.live
auto_file=${XDG_STATE_HOME:-$HOME/.local/state}/glass-xec/audio-auto
auto_on() { [ "$(cat "$auto_file" 2> /dev/null)" != off ]; }
set_auto() { mkdir -p "$(dirname "$auto_file")"; echo "$1" > "$auto_file"; }

# Streams that follow the default: those of applications (a client) playing
# on FROM, moved to TO, and the voice (079's speech: the echo canceller's
# playback, node echo-cancel-playback), which comes out of the Glass too
# (the user, 2026-10-07: "Desktop and voice through the glasses unless
# disconnected"; the stereo is not muted, the headset left alone).
# Other streams of modules (the microphone loopbacks into mix_mic) are
# routed on purpose and never moved.
voice_on() {   # SINK_ID: the voice's stream when it plays there
    pactl list sink-inputs 2> /dev/null | awk -v f="$1" '
        /^Sink Input #/ { id = substr($3, 2); s = "" }
        /^[ \t]+Sink: / { s = $2 }
        /node\.name = "echo-cancel-playback"/ { if (s == f) print id }'
}
move_streams() {   # FROM TO
    local from
    from=$(pactl list short sinks | awk -v n="$1" '$2 == n { print $1; exit }')
    [ -n "$from" ] || return 0
    for s in $(pactl list short sink-inputs | awk -v f="$from" '$2 == f && $3 != "-" { print $1 }') $(voice_on "$from"); do
        pactl move-sink-input "$s" "$2" 2> /dev/null
    done
}
# Streams of modules that name their output (target.object: the loopbacks
# into mix_mic) put back there whatever the default is: WirePlumber moves
# such streams with the default. The voice is not one of them (it goes with
# the Glass, move_streams). Run after each change of default, and again 2 s
# later (WirePlumber moves late).
pin_module_streams() {
    local sinks
    sinks=$(pactl list short sinks | awk '{ print $1, $2 }')
    pactl list sink-inputs 2> /dev/null | awk '
        function out() { if (id != "" && mod != "n/a" && tgt != "" && node != "echo-cancel-playback") print id, sink, tgt }
        /^Sink Input #/ { out(); id = substr($3, 2); mod = ""; sink = ""; tgt = ""; node = "" }
        /^[ \t]+Owner Module: / { mod = $3 }
        /^[ \t]+Sink: / { sink = $2 }
        /target\.object = / { tgt = $3; gsub(/"/, "", tgt) }
        /node\.name = / { node = $3; gsub(/"/, "", node) }
        END { out() }' |
        while read -r id cur tgt; do
            want=$(echo "$sinks" | awk -v n="$tgt" '$2 == n { print $1; exit }')
            [ -n "$want" ] && [ "$want" != "$cur" ] || continue
            pactl move-sink-input "$id" "$tgt" 2> /dev/null &&
                echo "glass-audio: $(date +%T) stream $id back on its own output $tgt" >> "$log"
        done
}
# A real output to come back to: the stereo, else the first hardware sink.
real_output() {
    pactl list short sinks | awk '$2 ~ /^alsa_output\./ && $2 ~ /Schiit/ { print $2; f = 1; exit } END { exit !f }' ||
        pactl list short sinks | awk '$2 ~ /^alsa_output\./ { print $2; exit }'
}

# Everything there now: the default output, the streams of FROM, the voice.
send_to() {   # FROM TO
    pactl set-default-sink "$2"
    move_streams "$1" "$2"
    pin_module_streams
}
connected() { [ -s "$live" ] && pgrep -f "scripts/glass-audio[.]sh$" > /dev/null; }
notice() {   # TEXT: on the terminal, and as a desktop notice
    echo "glass audio: $1"
    command -v notify-send > /dev/null && notify-send -a "Glass" -i audio-volume-high "Glass audio" "$1" 2> /dev/null
    echo "glass-audio: $(date +%T) $1" >> "$log"
}
where() {
    local d; d=$(pactl get-default-sink)
    [ "$d" = "$sink" ] && echo "the Glass" || pactl list sinks | awk -v n="$d" '$1 == "Name:" { f = ($2 == n) } f && $1 == "Description:" { sub(/^[ \t]*Description: /, ""); print; exit }'
}
case "${1:-}" in
    stop)
        for p in $(pgrep -f "scripts/glass-audio[.]sh$"); do kill "$p"; done 2> /dev/null
        exit 0 ;;
    status)
        echo "sound on:            $(where)"
        echo "automatic switching: $(auto_on && echo "on (the Glass while connected, else the stereo)" || echo "off (nothing moves by itself)")"
        echo "the Glass:           $(connected && echo "connected ($(cat "$live"))" || echo "not connected")"
        exit 0 ;;
    glass)
        set_auto on
        if connected; then
            send_to "$(real_output)" "$sink"
            notice "sound on the Glass; automatic switching on"
        else
            notice "automatic switching on: the sound goes to the Glass when it connects"
        fi
        exit 0 ;;
    stereo)
        set_auto off
        send_to "$sink" "$(real_output)"
        notice "sound on the stereo; automatic switching off (glass audio glass to go back)"
        exit 0 ;;
    auto | toggle)
        want=${2:-}
        [ "$1" = toggle ] && { auto_on && want=off || want=on; }
        case "$want" in
            on)
                set_auto on
                if connected; then send_to "$(real_output)" "$sink"; notice "automatic switching on: sound on the Glass"
                else send_to "$sink" "$(real_output)"; notice "automatic switching on: sound on the stereo until the Glass connects"; fi ;;
            off)
                set_auto off
                notice "automatic switching off: the sound stays on $(where) until switched by hand" ;;
            *) echo "glass audio auto on|off" >&2; exit 1 ;;
        esac
        exit 0 ;;
    '') ;;
    *) echo "glass audio [status | glass | stereo | auto on|off | toggle]" >&2; exit 1 ;;
esac

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
created=0
if [ -z "$m" ]; then
    pactl load-module module-null-sink sink_name="$sink" sink_properties="$props" rate=48000 channels=2 > /dev/null
    created=1
fi

# The output's last state (volume and mute) kept across restarts and logins
# (the user, 2026-10-07): saved on every change by a watcher for as long as
# this runs, given back when the output had to be made anew. An output that
# already existed keeps what it has (it may have been changed since).
state_file=${XDG_STATE_HOME:-$HOME/.local/state}/glass-xec/glass-output
mkdir -p "$(dirname "$state_file")"
if [ $created = 1 ] && [ -s "$state_file" ]; then
    read -r v mu < "$state_file"
    case "${v:-}" in '' | *[!0-9]*) ;; *) pactl set-sink-volume "$sink" "$v%" ;; esac
    case "${mu:-}" in 0 | 1) pactl set-sink-mute "$sink" "$mu" ;; esac
    echo "glass-audio: $(date +%T) output made anew: volume $v%, mute $mu, as last left" >> "$log"
fi
save_state() {
    local v mu
    v=$(pactl get-sink-volume "$sink" 2> /dev/null | grep -o '[0-9]*%' | head -n 1 | tr -d %)
    mu=$(pactl get-sink-mute "$sink" 2> /dev/null | awk '{ print ($2 == "yes") ? 1 : 0 }')
    [ -n "$v" ] || return 0
    [ "$v $mu" = "$(cat "$state_file" 2> /dev/null)" ] && return 0
    echo "$v $mu" > "$state_file.new" && mv "$state_file.new" "$state_file"
}
save_state
# In its own process group (set -m), so that the stop ends it whole, and
# without the lock (9>&-): a watcher left behind kept the lock and the next
# start refused (2026-10-07).
set -m
(
    id=$(pactl list short sinks | awk -v n="$sink" '$2 == n { print $1; exit }')
    pactl subscribe 2> /dev/null | while read -r ev; do
        case "$ev" in *"'change' on sink #$id") save_state ;; esac
    done
) 9>&- &
set +m

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

# The Glass gone: the stereo the default again and the streams on it, when
# switching is automatic (off: nothing is moved, ever).
restore() {
    rm -f "$live"
    auto_on || return 0
    local out
    out=$(real_output)
    [ "$(pactl get-default-sink)" = "$sink" ] && pactl set-default-sink "$out"
    pin_module_streams
    move_streams "$sink" "$out"
    echo "glass-audio: $(date +%T) back to $out" >> "$log"
}
trap 'for j in $(jobs -p); do kill -- -"$j" 2> /dev/null || kill "$j" 2> /dev/null; done; restore; exit 0' INT TERM

echo "glass-audio: $(date +%T) started (automatic switching $(auto_on && echo on || echo off))" >> "$log"
# Left as the default by an earlier run that was killed mid-session: give
# the default back to a real output (the session below takes it again).
[ "$(pactl get-default-sink)" = "$sink" ] && restore
while :; do
    # Whenever the Glass answers: its own transducer needs nothing plugged
    # in. One address for the whole session (glass addr: home Wi-Fi, USB or
    # the tailnet).
    addr=$("$top/scripts/glass" addr 2> /dev/null || true)
    if [ -n "$addr" ] && GLASS_IP=$addr "$top/scripts/glass" ssh true < /dev/null > /dev/null 2>&1; then
        echo "$addr" > "$live"
        if auto_on; then
            cur=$(pactl get-default-sink)
            routed="the sound moved from $cur"
            if [ "$cur" != "$sink" ]; then
                pactl set-default-sink "$sink"
                move_streams "$cur" "$sink"
            fi
            # Whatever is still elsewhere (a stream that started on the stereo
            # while the last session changed path).
            move_streams "$(real_output)" "$sink"
            pin_module_streams
            ( sleep 2; pin_module_streams ) 9>&- &
        else
            routed="automatic switching off: nothing moved"
        fi
        # In the background with a wait: a stop signal then runs the trap at
        # once (bash defers traps until a foreground pipeline ends).
        case "$addr" in
            100.6[4-9].* | 100.[7-9][0-9].* | 100.1[01][0-9].* | 100.12[0-7].*)
                # The tailnet (the hotspot, away): Opus at 48 kbit/s instead
                # of 768 kbit/s of raw samples; the Glass decodes it and
                # holds it by its remote delay (glass-audio delay remote).
                echo "glass-audio: $(date +%T) to the Glass over the tailnet at $addr, Opus 48 kbit/s ($routed)" >> "$log"
                parec -d "$sink.monitor" --format=s16le --rate=$rate --channels=1 --latency-msec=10 --raw 2>> "$log" 9>&- |
                    ffmpeg -hide_banner -loglevel error -f s16le -ar $rate -ac 1 -i - -c:a libopus -b:a 48k \
                        -application lowdelay -frame_duration 40 -f mpegts -muxdelay 0 -flush_packets 1 - 2>> "$log" 9>&- |
                    GLASS_IP=$addr "$top/scripts/glass" ssh "glass-audio play opus" 2>> "$log" 9>&- &
                ;;
            *)
                echo "glass-audio: $(date +%T) to the Glass at $addr ($routed)" >> "$log"
                parec -d "$sink.monitor" --format=s16le --rate=$rate --channels=1 --latency-msec=10 --raw 2>> "$log" 9>&- |
                    GLASS_IP=$addr "$top/scripts/glass" ssh "glass-audio play" 2>> "$log" 9>&- &
                ;;
        esac
        session=$!
        # The volume link beside the session, in its own process group (set
        # -m) so that it ends whole with the session.
        set -m
        volume_link "$addr" 9>&- &
        link=$!
        set +m
        # Ended when the Glass is on a better path or this one went away
        # (glass-pathguard.sh); the loop starts it again there, in that
        # path's mode (raw at home, Opus over the tailnet).
        "$top/scripts/glass-pathguard.sh" "$addr" "$session" >> "$log" 2>&1 9>&- &
        guard=$!
        wait $session
        kill -- -"$link" 2> /dev/null
        kill $guard 2> /dev/null
        # On another path at once (the cable out with Wi-Fi up, Wi-Fi back):
        # the sound stays on the Glass's output for the next session, no
        # trip to the stereo and back. Only the Glass gone brings the stereo.
        next=$("$top/scripts/glass" addr 2> /dev/null || true)
        if [ -z "$next" ] || ! GLASS_IP=$next "$top/scripts/glass" ssh true < /dev/null > /dev/null 2>&1; then
            restore
        else
            echo "glass-audio: $(date +%T) the Glass now at $next: the sound stays on it" >> "$log"
        fi
    fi
    sleep 2
done
