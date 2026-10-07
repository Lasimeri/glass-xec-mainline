#!/bin/sh
# update.sh: runs ON the Glass, sent by `scripts/glass update` with a staged
# tree in /tmp/glass-update (this checkout's userland/initramfs and
# userland/rootfs files, the tools' sources under usr/local/src, the
# console font). Brings the running Glass up to date, nothing flashed:
#   1. packages the userland needs (apk, at low priority)
#   2. every shell script parses, or nothing at all is installed
#   3. files that differ installed (.new then mv, modes set)
#   4. tools rebuilt where their source changed or the binary is missing
#      (gcc at nice 19: a build at the thermal cap slows the stream)
#   5. what changed restarted; the rest left running
# User settings are not in the staged tree and are never touched: Wi-Fi
# networks, fixed addresses, audio route and delay, brightness.
set -u
s=/tmp/glass-update
cd "$s" || exit 1
say() { echo "update: $*"; }

# 1. Packages: sound (amixer, libasound), Tailscale, btop (glass btop), and the compiler the
# tools are built with here.
missing=""
for p in alsa-utils alsa-lib tailscale btop gcc musl-dev linux-headers alsa-lib-dev; do
    apk info -e "$p" > /dev/null 2>&1 || missing="$missing $p"
done
if [ -n "$missing" ]; then
    say "installing packages:$missing"
    nice -n 19 apk add --no-progress $missing > /tmp/glass-update.apk.log 2>&1 || say "apk failed (see /tmp/glass-update.apk.log); going on"
else
    say "packages: all present"
fi

# 2. Every script must parse before anything is installed: rcS, the console
# launcher, glass-wifi and init keep the Glass reachable.
bad=0
for f in $(find . -type f ! -path './usr/local/src/*' ! -path './usr/share/*' ! -name '.update.sh'); do
    if [ "$(head -c 2 "$f")" = '#!' ] && ! sh -n "$f" 2> /dev/null; then
        say "syntax error in ${f#.}"
        bad=1
    fi
done
[ $bad = 0 ] || { say "nothing installed"; exit 1; }

# 3. Files.
changed=""
for f in $(find . -type f ! -path './usr/local/src/*' ! -name '.update.sh' | sort); do
    d=${f#.}
    cmp -s "$f" "$d" 2> /dev/null && continue
    mkdir -p "$(dirname "$d")"
    cp "$f" "$d.new" || { say "could not write $d"; continue; }
    case "$d" in
        /init | /usr/sbin/* | /etc/glass/rcS | /etc/glass/rcS-rootfs | /etc/glass/console) chmod 755 "$d.new" ;;
        *) chmod 644 "$d.new" ;;
    esac
    mv "$d.new" "$d" && changed="$changed $d"
done

# 4. Tools: built from the staged source when it differs from the one the
# binary was built from (kept in /usr/local/src after a good build only).
built=""
for t in glass-fb glass-tap glass-console glass-play glass-camera glass-fbgrab; do
    src=./usr/local/src/$t.c
    [ -f "$src" ] || continue
    if cmp -s "$src" "/usr/local/src/$t.c" && [ -x "/usr/local/bin/$t" ]; then continue; fi
    case $t in
        glass-fb) flags="-O3 -mfpu=neon -mfloat-abi=hard"; libs="-lpthread" ;;
        glass-play) flags="-O2"; libs="" ;;
        glass-camera) flags="-O2 -I./usr/local/src/omx"; libs="-lpthread" ;;
        *) flags="-O2"; libs="" ;;
    esac
    say "building $t"
    if nice -n 19 gcc -Wall $flags -o "/tmp/$t.new" "$src" $libs 2> "/tmp/$t.build.log"; then
        mkdir -p /usr/local/bin /usr/local/src
        mv "/tmp/$t.new" "/usr/local/bin/$t.new" && mv "/usr/local/bin/$t.new" "/usr/local/bin/$t"
        cp "$src" "/usr/local/src/$t.c"
        built="$built $t"
    else
        say "$t did not build (see /tmp/$t.build.log); the old one stays"
    fi
done

# 5. Restarts, only for what changed.
restarted=""
has() { case " $1 " in *" $2 "*) return 0 ;; esac; return 1; }
if has "$built" glass-console || has "$changed" /etc/glass/console; then
    for p in $(pgrep -f "glass/consol[e] display") $(pidof glass-console); do kill "$p"; done 2> /dev/null
    restarted="$restarted display-shell"
fi
if has "$built" glass-fb; then
    for p in $(pidof ffmpeg); do kill "$p"; done; sleep 1
    for p in $(pidof glass-fb); do kill "$p"; done 2> /dev/null
    restarted="$restarted stream"
fi
if has "$built" glass-play || has "$changed" /usr/sbin/glass-audio; then
    for p in $(pidof glass-play aplay); do kill "$p"; done 2> /dev/null
    restarted="$restarted sound"
fi
if has "$built" glass-tap; then
    for p in $(pidof glass-tap); do kill "$p"; done 2> /dev/null
    restarted="$restarted tap"
fi
if has "$changed" /usr/sbin/glass-wifi; then
    /usr/sbin/glass-wifi up > /dev/null 2>&1 && restarted="$restarted wifi-hook"
fi
if has "$changed" /etc/inittab; then
    kill -HUP 1 && restarted="$restarted init"
    # The ssh listener's flags may have changed (-K 5): it is restarted 3 s
    # after this update ends, init respawning it at once; done here, it took
    # this very session down with it (2026-10-07).
    setsid sh -c 'sleep 3; for p in $(pidof dropbear); do [ "$(cut -d " " -f 4 /proc/$p/stat 2> /dev/null)" = 1 ] && kill "$p"; done' \
        > /dev/null 2>&1 < /dev/null &
    restarted="$restarted ssh-listener(in 3 s)"
fi
if [ -x /usr/sbin/tailscaled ] && ! pidof tailscaled > /dev/null; then
    mkdir -p /var/lib/tailscale /run/tailscale
    setsid /usr/sbin/tailscaled --state=/var/lib/tailscale/tailscaled.state \
        --socket=/run/tailscale/tailscaled.sock > /run/glass/tailscaled.log 2>&1 < /dev/null &
    restarted="$restarted tailscaled"
fi
echo "1 4 1 7" > /proc/sys/kernel/printk

say "files changed:${changed:- none}"
say "tools built:${built:- none}"
say "restarted:${restarted:- nothing}"
if has "$changed" /etc/glass/rcS || has "$changed" /etc/glass/rcS-rootfs; then
    say "boot scripts changed: they take effect at the next boot"
fi
if [ -x /usr/bin/tailscale ] && tailscale --socket=/run/tailscale/tailscaled.sock status 2>&1 | grep -qi "logged out"; then
    say "Tailscale is not logged in: on the Glass, tailscale --socket=/run/tailscale/tailscaled.sock up --hostname=glass --accept-dns=false"
fi
echo "BUILT:$built"
cd / && rm -rf "$s"
