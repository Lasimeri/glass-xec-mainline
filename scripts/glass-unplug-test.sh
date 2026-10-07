#!/bin/bash
# glass-unplug-test.sh [ROUNDS] [HOLD_S]: what happens when the USB cable
# comes out, ROUNDS times (default 1), the cable "unplugged" for HOLD_S
# seconds (default 40) from the Glass's side: its USB gadget switched off,
# which this desktop sees exactly as an unplug (the link and its address
# vanish), and switched on again by a timer on the Glass whatever happens
# here. Each round has two parts:
#   A  on Wi-Fi: every session is on home Wi-Fi; the cable comes out: each
#      must carry on untouched (or be back on Wi-Fi within 60 s).
#   B  on the cable: Wi-Fi is taken down on the Glass (glass-wifi down), the
#      sessions fall back to the cable; then the cable comes out as Wi-Fi
#      comes back: each must start again on Wi-Fi by itself (timed).
# The sessions: stream (the picture), sound (glass-play), volume (the
# desktop slider's link), tap (the touchpad reader), camera (when a camera
# window is open). Then, the cable back for 15 s, every session must still
# be on Wi-Fi (none moves back onto the cable). Needs the cable in and home
# Wi-Fi up. Run as `scripts/glass unplug-test [ROUNDS] [HOLD_S]`; nothing is
# written to the Glass.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
rounds=${1:-1}
hold=${2:-40}
run=${XDG_RUNTIME_DIR:-/tmp}
g() { "$top/scripts/glass" "$@"; }
lan=$(head -n 1 "$top/out/glass-ip" 2> /dev/null || true)
usb=172.16.42.1
[ -n "$lan" ] || { echo "unplug-test: no home Wi-Fi address in out/glass-ip" >&2; exit 1; }
cable() { [ -n "$(ip -o -4 addr show to 172.16.42.0/24 2> /dev/null)" ]; }
lines() { [ -f "$1" ] && wc -l < "$1" || echo 0; }
# Each session's ssh, by what it runs on the Glass.
declare -A mark=([stream]='pidof ffmpeg glass-fb' [sound]='glass-audio play' [volume]='glass-audio control' [tap]='/usr/local/bin/glass-tap' [camera]='/usr/local/bin/glass-camera')
declare -A logf=([stream]=glass-viewd [sound]=glass-audio [volume]=glass-audio [tap]=glass-tap [camera]=glass-camera)
declare -A fresh=([stream]='glass-fb: [0-9.]+ frames/s shown' [sound]='glass-play: .* Hz, target' [volume]='' [tap]='glass-tap: /dev/input' [camera]='glass-camera: [0-9]+ frames, [1-9]')
session() {   # PART: "PID ADDRESS" of its ssh now, or nothing
    ps -eo pid,args | grep -F -- "${mark[$1]}" | grep -E '^ *[0-9]+ ssh ' | grep -oE '^ *[0-9]+|root@[0-9.]+' | tr -d ' ' | sed 's/root@//' | paste -sd ' ' | cut -d' ' -f1,2
}
name() { case "$1" in 172.16.42.*) echo cable ;; 100.*) echo tailnet ;; "") echo none ;; *) echo Wi-Fi ;; esac; }
declare -A mk
marks() { for p in "${parts[@]}"; do mk[$p]=$(lines "$run/${logf[$p]}.log"); done; }
# Started again on ADDRESS since the marks: on it, and (when the session
# has one) a line of its log saying it works.
restarted_on() {   # PART ADDRESS
    local s; s=$(session "$1")
    [ "${s#* }" = "$2" ] || return 1
    [ -z "${fresh[$1]}" ] && return 0
    tail -n +$(( ${mk[$1]} + 1 )) "$run/${logf[$1]}.log" 2> /dev/null | grep -qE "${fresh[$1]}"
}
wait_all_on() {   # ADDRESS SECONDS: until every session is on ADDRESS
    local t=$(( $(date +%s) + $2 )) p all
    while [ "$(date +%s)" -lt $t ]; do
        all=1
        for p in "${parts[@]}"; do s=$(session "$p"); [ "${s#* }" = "$1" ] || all=0; done
        [ $all = 1 ] && return 0
        sleep 1
    done
    return 1
}
after_replug() {   # wait for the cable, 15 s more, every session on Wi-Fi
    local t0=$1 ok=1 p a
    while ! cable && [ $(( $(date +%s) - t0 )) -lt $((hold + 60)) ]; do sleep 1; done
    if ! cable; then
        # The Glass's own view of its USB power: 0 is a cable physically out.
        local vbus
        vbus=$(GLASS_IP=$lan g ssh 'cat /sys/class/power_supply/twl6030_usb/online 2> /dev/null' < /dev/null 2> /dev/null | tr -d '\r')
        if [ "$vbus" = 0 ]; then echo "    the cable is physically out (the Glass runs on its battery): plug it in and run the test again"
        else echo "    the USB link did not come back (the Glass switched it on after $hold s; replug the cable)"; fi
        return 1
    fi
    echo "    the cable back after $(( $(date +%s) - t0 )) s; 15 s later:"
    sleep 15
    for p in "${parts[@]}"; do
        a=$(session "$p"); a=${a#* }
        printf '    %-8s on %s\n' "$p" "$(name "$a")"
        [ "$a" = "$lan" ] || ok=0
    done
    [ $ok = 1 ]
}
gadget_off() {   # VIA SECONDS [THEN]: the USB gadget off for SECONDS, sent over VIA; THEN run on the Glass meanwhile
    GLASS_IP=$1 g ssh "a=/sys/class/android_usb/android0; setsid sh -c \"echo 0 > \$a/enable; ${3:+( $3 ) &} sleep $2; echo 1 > \$a/enable\" > /dev/null 2>&1 < /dev/null &" < /dev/null > /dev/null 2>&1
}
failures=0
for r in $(seq 1 "$rounds"); do
    cable || { echo "unplug-test: the USB cable is not in (no 172.16.42.x here): plug it in first" >&2; exit 1; }
    GLASS_IP=$lan g ssh true < /dev/null > /dev/null 2>&1 || { echo "unplug-test: home Wi-Fi ($lan) does not answer ssh: the test needs it" >&2; exit 1; }
    parts=(stream sound volume tap)
    [ -n "$(session camera)" ] && parts+=(camera)
    ok=1
    echo "== round $r of $rounds ($(date +%T))"

    echo "  A. the cable out while every session is on Wi-Fi"
    if ! wait_all_on "$lan" 60; then
        for p in "${parts[@]}"; do s=$(session "$p"); printf '    %-8s on %s\n' "$p" "$(name "${s#* }")"; done
        echo "    not all on Wi-Fi within 60 s"; ok=0
    fi
    declare -A before=()
    for p in "${parts[@]}"; do s=$(session "$p"); before[$p]=${s%% *}; done
    marks
    gadget_off "$lan" "$hold"
    t0=$(date +%s)
    while cable && [ $(( $(date +%s) - t0 )) -lt 10 ]; do sleep 1; done
    cable && { echo "    the link did not go (the gadget did not switch off)"; ok=0; }
    sleep 20
    for p in "${parts[@]}"; do
        s=$(session "$p")
        if [ "${s%% *}" = "${before[$p]}" ] && [ "${s#* }" = "$lan" ]; then printf '    %-8s carried on, untouched\n' "$p"
        elif restarted_on "$p" "$lan"; then printf '    %-8s restarted, on Wi-Fi\n' "$p"
        else printf '    %-8s NOT on Wi-Fi (on %s)\n' "$p" "$(name "${s#* }")"; ok=0; fi
    done
    after_replug "$t0" || ok=0

    echo "  B. Wi-Fi down, the sessions on the cable; then the cable out as Wi-Fi comes back"
    GLASS_IP=$usb g ssh 'setsid /usr/sbin/glass-wifi down > /dev/null 2>&1 < /dev/null &' < /dev/null > /dev/null 2>&1
    t1=$(date +%s)
    if wait_all_on "$usb" 60; then
        echo "    all on the cable after $(( $(date +%s) - t1 )) s"
    else
        for p in "${parts[@]}"; do s=$(session "$p"); printf '    %-8s on %s\n' "$p" "$(name "${s#* }")"; done
        echo "    not all on the cable within 60 s"; ok=0
    fi
    marks
    gadget_off "$usb" "$hold" "/usr/sbin/glass-wifi up"
    t0=$(date +%s)
    declare -A at=()
    while [ $(( $(date +%s) - t0 )) -lt 90 ]; do
        now=$(( $(date +%s) - t0 ))
        for p in "${parts[@]}"; do
            [ -z "${at[$p]:-}" ] && restarted_on "$p" "$lan" && at[$p]=$now
        done
        all=1
        for p in "${parts[@]}"; do [ -n "${at[$p]:-}" ] || all=0; done
        [ $all = 1 ] && break
        sleep 1
    done
    echo "    back on Wi-Fi, seconds after the cable came out (Wi-Fi rejoining meanwhile):"
    for p in "${parts[@]}"; do
        if [ -n "${at[$p]:-}" ]; then printf '    %-8s %4s s\n' "$p" "${at[$p]}"
        else s=$(session "$p"); printf '    %-8s  not back on Wi-Fi in 90 s (on %s)\n' "$p" "$(name "${s#* }")"; ok=0; fi
    done
    unset at
    after_replug "$t0" || ok=0

    if [ $ok = 1 ]; then echo "  round $r: everything moved to Wi-Fi and stayed"; else echo "  round $r: FAILED"; failures=$((failures + 1)); fi
    unset before
    [ "$r" -lt "$rounds" ] && sleep 10
done
[ $failures = 0 ]
