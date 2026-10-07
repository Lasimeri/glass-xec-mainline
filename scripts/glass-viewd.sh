#!/bin/bash
# glass-viewd.sh: the desktop's monitor on the glasses whenever the Glass is
# up: waits for the Glass to answer (Wi-Fi or USB), opens a terminal on the
# desktop with a shell on the Glass, runs glass-view.sh (the monitor last
# chosen, 24 frames/s), and starts it again when the Glass reboots, dies or
# leaves the network. Started at login by
# ~/.config/autostart/glass-viewd.desktop; a second copy refuses to start.
# The sound has its own supervisor (glass-audio.sh), the tap too
# (glass-tap.sh).
#   glass-viewd.sh            run (foreground; log $XDG_RUNTIME_DIR/glass-viewd.log)
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
run=${XDG_RUNTIME_DIR:-/tmp}
log=$run/glass-viewd.log
exec 9> "$run/glass-viewd.lock"
flock -n 9 || { echo "glass-viewd: already running" >&2; exit 0; }
echo "glass-viewd: $(date +%T) started" >> "$log"
while :; do
    if "$top/scripts/glass" ssh true < /dev/null > /dev/null 2>&1; then
        # Remember where it is now (address and Wi-Fi MAC), for the next search.
        "$top/scripts/glass" ip >> "$log" 2>&1
        echo "glass-viewd: $(date +%T) Glass up, streaming" >> "$log"
        # A terminal on the desktop with a shell on the Glass, once per
        # connection (it closes by itself when the Glass goes away).
        if ! pgrep -f "konsole.*tabtitle=Glass shel[l]" > /dev/null; then
            # fd 9 (this supervisor's lock) closed for the terminal, or it would
            # hold the lock after the supervisor ends and block the next start.
            setsid konsole --separate -p tabtitle="Glass shell" -e "$top/scripts/glass" ssh > /dev/null 2>&1 < /dev/null 9>&- &
        fi
        BUFFER_MS=0 "$top/scripts/glass-view.sh" "" 24 >> "$log" 2>&1 9>&-
        echo "glass-viewd: $(date +%T) stream ended" >> "$log"
        # The decoder on the Glass, if the session died under it.
        "$top/scripts/glass" ssh 'for p in $(pidof ffmpeg glass-fb); do kill $p; done; true' < /dev/null > /dev/null 2>&1
    fi
    sleep 5
done
