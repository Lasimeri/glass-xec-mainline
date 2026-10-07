#!/bin/bash
# glass-pathguard.sh ADDRESS TARGET: ends a session to the Glass that uses
# ADDRESS as soon as the Glass is on another path, so its supervisor starts
# it again there (and in the mode that path wants: tailnet or home).
#   TARGET   what to end: a PID, or -PGID for a whole process group
# Over the tailnet, the session ends when home Wi-Fi (out/glass-ip) or the
# USB link answers: those are the better paths. On home Wi-Fi or USB, it
# ends when ADDRESS misses two pings in a row (the Glass left, or was
# unplugged), sooner than ssh's own keepalive. Checks every 3 s; ends by
# itself with the session. Used by glass-viewd.sh, glass-audio.sh and
# glass-tap.sh, the supervisors of the picture, the sound and the tap.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
addr=${1:?ADDRESS} target=${2:?TARGET}
lan=$(head -n 1 "$top/out/glass-ip" 2> /dev/null || true)
tailnet() { case "$1" in 100.6[4-9].* | 100.[7-9][0-9].* | 100.1[01][0-9].* | 100.12[0-7].*) return 0 ;; esac; return 1; }
answers() { ping -c 1 -W 1 "$1" > /dev/null 2>&1; }
miss=0 why=""
while kill -0 -- "$target" 2> /dev/null; do
    sleep 3
    if tailnet "$addr"; then
        if [ -n "$lan" ] && ! tailnet "$lan" && answers "$lan"; then why="home Wi-Fi ($lan) answers"; break; fi
        if answers 172.16.42.1; then why="the USB link answers"; break; fi
    elif answers "$addr"; then
        miss=0
    else
        miss=$((miss + 1))
        if [ $miss -ge 2 ]; then why="$addr stopped answering"; break; fi
    fi
done
if [ -n "$why" ]; then
    echo "glass-pathguard: $(date +%T) $why: the session on $addr starts again"
    kill -- "$target" 2> /dev/null
fi
exit 0
