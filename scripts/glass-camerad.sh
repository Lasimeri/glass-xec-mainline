#!/bin/bash
# glass-camerad.sh: the Glass's camera window on this desktop whenever the
# Glass comes up, like the picture (glass-viewd.sh), the sound and the tap:
# started at login from ~/.config/autostart (glass setup-desktop). Each time
# the Glass appears (this desktop's login with the Glass up, the Glass's
# boot, back from a lost connection) and no camera window is open, it opens
# one (glass-camera.sh: its own window, face tracking, reconnecting across
# the cable, Wi-Fi and the tailnet). A window closed by hand stays closed
# until the Glass goes away and comes back.
#   log: $XDG_RUNTIME_DIR/glass-camerad.log      stop: kill the supervisor
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
run=${XDG_RUNTIME_DIR:-/tmp}
log=$run/glass-camerad.log
pidf=$run/glass-camerad.pid
if [ -f "$pidf" ] && kill -0 "$(cat "$pidf")" 2> /dev/null; then
    echo "glass-camerad: already running (pid $(cat "$pidf"))" >&2
    exit 0
fi
echo $$ > "$pidf"
trap 'rm -f "$pidf"; exit 0' INT TERM
was_up=0
while :; do
    if [ -n "$("$top/scripts/glass" addr 2> /dev/null)" ]; then up=1; else up=0; fi
    if [ "$up" = 1 ] && [ "$was_up" = 0 ]; then
        if pgrep -f "scripts/glass-camera[.]sh" > /dev/null; then
            echo "$(date '+%F %T') the Glass is up; its camera window is already open" >> "$log"
        else
            echo "$(date '+%F %T') the Glass is up: opening its camera window" >> "$log"
            setsid -f "$top/scripts/glass-camera.sh" >> "$log" 2>&1 < /dev/null
        fi
    fi
    was_up=$up
    sleep 5
done
