#!/bin/sh
# status.sh: runs ON the Glass, sent by `scripts/glass status`; prints one
# "name<TAB>state" line per row of the status table, read-only.
row() { printf '%s\t%s\n' "$1" "$2"; }
v() { cat "$1" 2> /dev/null; }

# Network: the Wi-Fi network and address, fixed or by DHCP.
ssid=$(v /run/glass/wifi-ssid)
addr=$(ip -4 addr show wlan0 2> /dev/null | sed -n 's/.*inet \([0-9./]*\).*/\1/p' | head -n 1)
if [ -n "$addr" ]; then
    how=DHCP
    [ -f /etc/glass/wifi-static ] && awk -F '\t' -v s="$ssid" '$1 == s { f = 1 } END { exit !f }' /etc/glass/wifi-static && how=fixed
    row wifi "${ssid:-?} at ${addr%/*} ($how)"
else
    row wifi "not connected"
fi
if [ "$(v /sys/class/net/rndis0/carrier)" = 1 ]; then row usb "connected (172.16.42.1)"; else row usb "not connected"; fi
if pidof tailscaled > /dev/null; then
    t=$(tailscale --socket=/run/tailscale/tailscaled.sock ip -4 2> /dev/null | head -n 1)
    row tailnet "${t:-tailscaled up, not logged in}"
else
    row tailnet "tailscaled not running"
fi

# Power and heat: battery, board temperature, CPU clock (the governors cap
# it at 300 MHz on a warm Glass).
b=/sys/class/power_supply/bq27520-0
cap=$(v $b/capacity); st=$(v $b/status)
row battery "${cap:-?}%${st:+, $(echo "$st" | tr 'A-Z' 'a-z')}"
pcb=$(v /sys/devices/platform/notle_pcb_sensor.0/temperature)
[ -n "$pcb" ] && row temperature "board $((pcb / 1000)).$(((pcb % 1000) / 100)) C"
cur=$(v /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
[ -n "$cur" ] && row cpu "$((cur / 1000)) MHz now"

# Display and sound settings, as kept.
p=/sys/devices/platform/omapdss/manager2/panel-notle-dpi
row display "brightness $(v $p/brightness) of $(v $p/brightness_limit)$(pidof glass-console > /dev/null && echo ', shell shown when idle')"
set -- $(v /etc/glass/audio-route)
row sound "${1:-earphone} at ${2:-50}%, $(v /etc/glass/audio-delay || echo 150) ms behind the desktop at home, $(v /etc/glass/audio-delay-remote || echo 400) ms over the tailnet"

# What runs: each part, up or not.
up=""; down=""
for x in glass-console glass-fb ffmpeg glass-play glass-tap tailscaled wpa_supplicant dropbear; do
    if pidof "$x" > /dev/null; then up="$up $x"; else down="$down $x"; fi
done
row running "${up# }"
[ -n "$down" ] && row "not running" "${down# }"
row uptime "$(uptime | sed 's/.* up \([^,]*\),.*/\1/')"
