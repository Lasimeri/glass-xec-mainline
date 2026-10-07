#!/bin/bash
# glass-pathguard.sh ADDRESS TARGET: ends a session to the Glass that uses
# ADDRESS as soon as the Glass is on a better path, or this one is gone, so
# its supervisor starts it again there (and in the mode that path wants:
# tailnet or home).
#   TARGET   what to end: a PID, or -PGID for a whole process group
# Home Wi-Fi is the path every session should be on (the user, 2026-10-07:
# unplugging the cable must change nothing, and when Wi-Fi comes back
# everything follows by itself):
#   - over the USB cable (172.16.42.x, taken only while Wi-Fi was down): the
#     session ends the moment the cable is unplugged (this desktop's address
#     on the link is gone, checked every second), and also once home Wi-Fi
#     (out/glass-ip) has answered both ping and ssh's port three checks in a
#     row (9 s; one good answer from a recovering radio is not enough), so
#     it moves to Wi-Fi while the cable is still in and pulling it later
#     costs nothing;
#   - over the tailnet: ends when home Wi-Fi or the USB link answers;
#   - over home Wi-Fi: ends when ADDRESS misses two pings in a row (the
#     Glass left), sooner than ssh's own keepalive.
# Ends by itself with the session. Used by glass-viewd.sh, glass-audio.sh,
# glass-tap.sh and glass-camera.sh.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
addr=${1:?ADDRESS} target=${2:?TARGET}
lan=$(head -n 1 "$top/out/glass-ip" 2> /dev/null || true)
tailnet() { case "$1" in 100.6[4-9].* | 100.[7-9][0-9].* | 100.1[01][0-9].* | 100.12[0-7].*) return 0 ;; esac; return 1; }
usb() { case "$1" in 172.16.42.*) return 0 ;; esac; return 1; }
answers() { ping -c 1 -W 1 "$1" > /dev/null 2>&1; }
ssh_port() { timeout 2 bash -c "exec 3<> /dev/tcp/$1/22" 2> /dev/null; }
# This desktop's own address on the USB link: gone the moment the cable is.
cable() { [ -n "$(ip -o -4 addr show to 172.16.42.0/24 2> /dev/null)" ]; }
miss=0 good=0 tick=0 why=""
while kill -0 -- "$target" 2> /dev/null; do
    sleep 1
    tick=$((tick + 1))
    if usb "$addr" && ! cable; then why="the USB cable was unplugged"; break; fi
    [ $((tick % 3)) = 0 ] || continue
    if tailnet "$addr"; then
        if [ -n "$lan" ] && ! tailnet "$lan" && answers "$lan"; then why="home Wi-Fi ($lan) answers"; break; fi
        if answers 172.16.42.1; then why="the USB link answers"; break; fi
        continue
    fi
    if usb "$addr" && [ -n "$lan" ] && ! usb "$lan" && ! tailnet "$lan"; then
        if answers "$lan" && ssh_port "$lan"; then good=$((good + 1)); else good=0; fi
        if [ $good -ge 3 ]; then why="home Wi-Fi ($lan) is steady again: moving off the cable"; break; fi
    fi
    if answers "$addr"; then
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
