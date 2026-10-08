#!/bin/bash
# glass-tap.sh: gestures on the Glass's touchpad, acted on here (the user's
# mapping, 2026-10-07).
#   a tap     mutes or unmutes the voice input (the microphone), the same
#             toggle as the number pad's period (ptt079 --toggle in ~/tts079:
#             $XDG_RUNTIME_DIR/speak-079/muted, the status line, and
#             "Muted." / "Listening." in 079's voice)
#   a two-finger tap  the stream on or off: the chosen display source and
#             the display together (glass camera-display toggle: off while
#             it charges); the display going dark or lit is the answer
#   a swipe   forward: the next display source; back: the one before
#             (glass display next|prev: camera, the camera alone, each
#             monitor, follow, console); a desktop notification names it;
#             which way is forward: glass swipe-forward + or -
#
# The desktop holds an ssh session to the Glass and reads glass-tap's lines
# (tools/glass-tap, on the Glass at /usr/local/bin/glass-tap); the Glass
# never connects to the desktop and holds no key for it. The session comes
# back by itself when the Glass reboots or Wi-Fi drops; each new one ends the
# detectors a cut session left running on the Glass. Run by
# glass-tap.service (glass setup-desktop); a second copy refuses to start.
#   glass-tap.sh            run (foreground; logs to $XDG_RUNTIME_DIR/glass-tap.log)
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
toggle=${PTT079:-$HOME/tts079/ptt079}
[ -x "$toggle" ] || { echo "glass-tap: no $toggle (ptt079 with --toggle)" >&2; exit 1; }
run=${XDG_RUNTIME_DIR:-/tmp}
exec 9> "$run/glass-tap.lock"
flock -n 9 || { echo "glass-tap: already running" >&2; exit 0; }
log=$run/glass-tap.log
echo "glass-tap: $(date +%T) started" >> "$log"
while :; do
    # One address for the session; in its own process group (set -m) so the
    # path guard can end it whole when the Glass is on a better path or this
    # one went away (glass-pathguard.sh); it starts again there.
    addr=$("$top/scripts/glass" addr 2> /dev/null || true)
    set -m
    # 9>&-: the session and the guard do not hold the single-instance lock
    # (a session left behind once kept a new supervisor from starting).
    # The stream toggle runs in its own session (setsid), so the path guard
    # ending this one cannot cut it halfway, and one at a time (a swipe while
    # one is under way is dropped).
    GLASS_IP=$addr "$top/scripts/glass" ssh 'pkill -x glass-tap; exec /usr/local/bin/glass-tap' < /dev/null 2>> "$log" 9>&- |
        while read -r g; do
            case "$g" in
                tap2)
                    # Two fingers: the stream (the chosen source) on or off,
                    # the display with it (glass camera-display toggle).
                    echo "glass-tap: $(date +%T) two-finger tap: stream and display toggled" >> "$log"
                    GLASS_IP=$addr setsid flock -n "$run/glass-camera-display.lock" \
                        "$top/scripts/glass" camera-display toggle < /dev/null >> "$log" 2>&1 9>&- &
                    ;;
                "swipe +" | "swipe -" | swipe)
                    # One finger along the pad: forward is the next display
                    # source, back the one before (glass display next|prev).
                    # Which sign is forward: out/glass-swipe-forward (+ by
                    # default; glass swipe-forward - flips it). A Glass whose
                    # glass-tap is older says only "swipe": next.
                    fwd=$(head -c 1 "$top/out/glass-swipe-forward" 2> /dev/null || true)
                    dir=next
                    case "$g" in "swipe +") [ "${fwd:-+}" = + ] || dir=prev ;; "swipe -") [ "${fwd:-+}" = - ] || dir=prev ;; esac
                    echo "glass-tap: $(date +%T) $g: $dir display source" >> "$log"
                    GLASS_IP=$addr setsid flock -n "$run/glass-display.lock" \
                        "$top/scripts/glass" display "$dir" < /dev/null >> "$log" 2>&1 9>&- &
                    ;;
                tap)
                    "$toggle" --toggle
                    echo "glass-tap: $(date +%T) tap: $([ -e "$run/speak-079/muted" ] && echo muted || echo listening)" >> "$log"
                    ;;
            esac
        done &
    session=$!
    set +m
    group=$(ps -o pgid= -p "$session" 2> /dev/null | tr -d ' ')
    "$top/scripts/glass-pathguard.sh" "$addr" "-${group:-$session}" >> "$log" 2>&1 9>&- &
    guard=$!
    wait $session
    kill $guard 2> /dev/null
    echo "glass-tap: $(date +%T) session ended; again in 2 s" >> "$log"
    sleep 2
done
