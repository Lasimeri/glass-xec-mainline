#!/bin/bash
# glass-reboot-test.sh [ROUNDS]: reboots the Glass ROUNDS times (default 1)
# and times, from each reboot, everything that should come back by itself:
#   ssh       the Glass answers ssh again
#   wifi      joined, with its address
#   clock     set from the internet (/run/glass/clock-ok)
#   tailnet   the desktop's tailscale ping to "glass" answers
#   shell     the display shell (glass-console) runs
#   stream    a frame report from the new stream at 90% of its rate or more
#   sound     glass-play started on the Glass for the new session
#   volume    the Glass's level matches the desktop slider
#   tap       the touchpad reader is back
# then lists what looked wrong: kernel errors, Wi-Fi watchdog actions, errors
# in the desktop supervisors' logs. A milestone not reached in 180 s fails the
# round. Run as `scripts/glass reboot-test [ROUNDS]`; nothing is written to
# the Glass but the reboot.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
rounds=${1:-1}
run=${XDG_RUNTIME_DIR:-/tmp}
g() { "$top/scripts/glass" "$@"; }
ms=(ssh wifi clock tailnet shell stream sound volume tap)
failures=0
declare -A total

lines() { [ -f "$1" ] && wc -l < "$1" || echo 0; }

for r in $(seq 1 "$rounds"); do
    addr=$(g addr 2> /dev/null)
    g ssh true < /dev/null > /dev/null 2>&1 || { echo "reboot-test: the Glass does not answer at $addr before round $r" >&2; exit 1; }
    mv=$(lines "$run/glass-viewd.log"); ma=$(lines "$run/glass-audio.log"); mt=$(lines "$run/glass-tap.log")
    mw=$(g ssh 'wc -l < /var/log/glass-wifi.log 2> /dev/null || echo 0' < /dev/null 2> /dev/null | tr -d '\r')
    echo "== round $r of $rounds: rebooting the Glass at $addr ($(date +%T))"
    g ssh 'sync; (sleep 1; reboot) > /dev/null 2>&1 &' < /dev/null > /dev/null 2>&1
    t0=$(date +%s)
    declare -A at=()
    # Down first, so the old system is not mistaken for the new one.
    while ping -c 1 -W 1 "$addr" > /dev/null 2>&1 && [ $(( $(date +%s) - t0 )) -lt 30 ]; do sleep 1; done
    sleep 3
    while [ $(( $(date +%s) - t0 )) -lt 180 ]; do
        now=$(( $(date +%s) - t0 ))
        if [ -z "${at[ssh]:-}" ]; then
            a=$(g addr 2> /dev/null)
            if [ -n "$a" ] && GLASS_IP=$a g ssh true < /dev/null > /dev/null 2>&1; then at[ssh]=$now; addr=$a; fi
        fi
        if [ -n "${at[ssh]:-}" ]; then
            s=$(GLASS_IP=$addr g ssh 'printf "%s %s %s %s %s\n" "$(wpa_cli -i wlan0 status 2> /dev/null | grep -c "^wpa_state=COMPLETED")" "$(ip -4 addr show wlan0 | grep -c "inet ")" "$([ -f /run/glass/clock-ok ] && echo 1 || echo 0)" "$(pidof glass-console > /dev/null && echo 1 || echo 0)" "$(amixer -c 0 cget name="DL1 Media Playback Volume" 2> /dev/null | sed -n "s/.*: values=//p")"' < /dev/null 2> /dev/null | tr -d '\r')
            set -- $s
            [ -z "${at[wifi]:-}" ] && [ "${1:-0}" = 1 ] && [ "${2:-0}" -ge 1 ] && at[wifi]=$now
            [ -z "${at[clock]:-}" ] && [ "${3:-0}" = 1 ] && at[clock]=$now
            [ -z "${at[shell]:-}" ] && [ "${4:-0}" = 1 ] && at[shell]=$now
            if [ -z "${at[volume]:-}" ] && [ -n "${at[sound]:-}" ] && [ -n "${5:-}" ]; then
                p=$(pactl get-sink-volume glass 2> /dev/null | grep -o '[0-9]*%' | head -n 1 | tr -d %)
                m=$(pactl get-sink-mute glass 2> /dev/null | awk '{ print $2 }')
                want=$(awk -v p="${p:-100}" -v m="$m" 'BEGIN { if (m == "yes" || p <= 0) { print 0; exit } d = 120 + 60 * log(p / 100) / log(10); if (d < 0) d = 0; printf "%d\n", d + 0.5 }')
                [ "$5" = "$want" ] && at[volume]=$now
            fi
        fi
        if [ -z "${at[tailnet]:-}" ] && tailscale ping -c 1 --timeout 2s glass > /dev/null 2>&1; then at[tailnet]=$now; fi
        if [ -z "${at[stream]:-}" ]; then
            f=$(tail -n +$((mv + 1)) "$run/glass-viewd.log" 2> /dev/null | grep -E 'glass-fb: [0-9.]+ frames/s shown' | tail -n 1 | sed -n 's/.*glass-fb: \([0-9.]*\) frames.*/\1/p')
            want=$(tail -n +$((mv + 1)) "$run/glass-viewd.log" 2> /dev/null | sed -n 's/.* \([0-9]*\) frames\/s from the compositor.*/\1/p' | tail -n 1)
            if [ -n "$f" ] && [ -n "$want" ] && awk -v f="$f" -v w="$want" 'BEGIN { exit !(f >= 0.9 * w) }'; then at[stream]=$now; fi
        fi
        [ -z "${at[sound]:-}" ] && tail -n +$((ma + 1)) "$run/glass-audio.log" 2> /dev/null | grep -q 'glass-play: .* Hz, target' && at[sound]=$now
        [ -z "${at[tap]:-}" ] && tail -n +$((mt + 1)) "$run/glass-tap.log" 2> /dev/null | grep -q 'glass-tap: /dev/input' && at[tap]=$now
        done=1
        for m in "${ms[@]}"; do [ -n "${at[$m]:-}" ] || done=0; done
        [ $done = 1 ] && break
        sleep 3
    done
    ok=1
    for m in "${ms[@]}"; do
        if [ -n "${at[$m]:-}" ]; then
            printf '  %-8s %4s s\n' "$m" "${at[$m]}"
            total[$m]=$(( ${total[$m]:-0} + ${at[$m]} ))
        else
            printf '  %-8s  not back in 180 s\n' "$m"
            ok=0
        fi
    done
    # What looked wrong this round.
    notes=$(GLASS_IP=$addr g ssh "dmesg | grep -iE 'oops|segfault|BUG:|panic|out of memory|killed process' | tail -n 3; tail -n +$((mw + 1)) /var/log/glass-wifi.log 2> /dev/null | grep -iE 'not joined|no address|restarting'" < /dev/null 2> /dev/null | tr -d '\r')
    notes+=$'\n'$(tail -n +$((mv + 1)) "$run/glass-viewd.log" 2> /dev/null | grep -iE 'error|refused|failed|busy' | grep -v 'probesize' | head -n 3)
    notes+=$'\n'$(tail -n +$((ma + 1)) "$run/glass-audio.log" 2> /dev/null | grep -iE 'error|refused|failed|busy' | head -n 3)
    notes+=$'\n'$(tail -n +$((mt + 1)) "$run/glass-tap.log" 2> /dev/null | grep -iE 'error|refused|failed' | head -n 3)
    notes=$(echo "$notes" | sed '/^$/d')
    if [ -n "$notes" ]; then echo "  noticed:"; echo "$notes" | sed 's/^/    /' | cut -c1-160; fi
    if [ $ok = 1 ]; then echo "  round $r: everything back"; else echo "  round $r: FAILED"; failures=$((failures + 1)); fi
    unset at
done
if [ "$rounds" -gt 1 ]; then
    echo "== $rounds rounds, $failures failed; mean seconds after the reboot:"
    for m in "${ms[@]}"; do [ -n "${total[$m]:-}" ] && printf '  %-8s %4d s\n' "$m" $(( ${total[$m]} / (rounds - failures > 0 ? rounds - failures : 1) )); done
fi
[ $failures = 0 ]
