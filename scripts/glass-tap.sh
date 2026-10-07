#!/bin/bash
# glass-tap.sh: gestures on the Glass's touchpad, acted on here.
#   a tap             the camera window and the Glass's display on or off
#                     together (glass camera-display toggle: off while it
#                     charges); the display going dark or lit is the answer
#   a two-finger tap  mutes or unmutes the voice input, the same toggle as
#                     the number pad's period (ptt079 --toggle in ~/tts079:
#                     $XDG_RUNTIME_DIR/speak-079/muted, the status line, and
#                     "Muted." / "Listening." in 079's voice)
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
    # The toggle runs in its own session (setsid), so the path guard ending
    # this one cannot cut it halfway, and one at a time (a tap while one is
    # under way is dropped).
    GLASS_IP=$addr "$top/scripts/glass" ssh 'pkill -x glass-tap; exec /usr/local/bin/glass-tap' < /dev/null 2>> "$log" 9>&- |
        while read -r g; do
            case "$g" in
                tap)
                    echo "glass-tap: $(date +%T) tap: camera window and display toggled" >> "$log"
                    GLASS_IP=$addr setsid flock -n "$run/glass-camera-display.lock" \
                        "$top/scripts/glass" camera-display toggle < /dev/null >> "$log" 2>&1 9>&- &
                    ;;
                tap2)
                    "$toggle" --toggle
                    echo "glass-tap: $(date +%T) two-finger tap: $([ -e "$run/speak-079/muted" ] && echo muted || echo listening)" >> "$log"
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
