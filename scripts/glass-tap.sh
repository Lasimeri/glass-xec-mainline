#!/bin/bash
# glass-tap.sh: a single tap on the Glass's touchpad mutes or unmutes the
# voice input, the same toggle as the number pad's period (ptt079 --toggle
# in ~/tts079: $XDG_RUNTIME_DIR/speak-079/muted, the status line, and
# "Muted." / "Listening." in 079's voice).
#
# The desktop holds an ssh session to the Glass and reads glass-tap's lines
# (tools/glass-tap, on the Glass at /usr/local/bin/glass-tap); the Glass
# never connects to the desktop and holds no key for it. The session comes
# back by itself when the Glass reboots or Wi-Fi drops. Started at login by
# ~/.config/autostart/glass-tap.desktop; a second copy refuses to start.
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
    "$top/scripts/glass" ssh /usr/local/bin/glass-tap < /dev/null 2>> "$log" |
        while read -r g; do
            case "$g" in
                tap)
                    "$toggle" --toggle
                    echo "glass-tap: $(date +%T) tap: $([ -e "$run/speak-079/muted" ] && echo muted || echo listening)" >> "$log"
                    ;;
            esac
        done
    echo "glass-tap: $(date +%T) session ended; again in 5 s" >> "$log"
    sleep 5
done
