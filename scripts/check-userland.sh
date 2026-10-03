#!/bin/bash
# check-userland.sh ROOT...: static checks, no emulation (nothing armv7 runs
# on this host). Every ELF is 32-bit ARM hard-float; every program's
# interpreter and every NEEDED library resolves inside the same root; the
# scripts the boot path runs exist, are executable, and pass sh -n; every
# symlink in the boot path resolves inside the root.
set -uo pipefail
bad=0
fail() { echo "check-userland: $*"; bad=1; }
for r in "$@"; do
    [ -d "$r" ] || { fail "$r: no such root"; continue; }
    n=0
    while IFS= read -r -d '' f; do
        head -c 4 "$f" 2> /dev/null | grep -q $'^\x7fELF' || continue
        n=$((n + 1))
        h=$(llvm-readelf -h "$f" 2> /dev/null)
        grep -q 'Class: *ELF32' <<< "$h" && grep -q 'Machine: *ARM' <<< "$h" || { fail "$f: not 32-bit ARM"; continue; }
        # e_flags: EABI version 5 (bits 31:24) and EF_ARM_ABI_FLOAT_HARD (0x400).
        fl=$(sed -n 's/^ *Flags: *\(0x[0-9a-fA-F]*\).*/\1/p' <<< "$h")
        [ -n "$fl" ] && [ $(( (fl >> 24) & 0xff )) -eq 5 ] && [ $(( fl & 0x400 )) -ne 0 ] || fail "$f: not EABI5 hard-float (flags $fl)"
        interp=$(llvm-readelf -l "$f" 2> /dev/null | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p')
        if [ -n "$interp" ] && [ ! -e "$r$interp" ]; then fail "$f: interpreter $interp missing"; fi
        for lib in $(llvm-readelf -d "$f" 2> /dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
            found=0
            for d in /lib /usr/lib; do [ -e "$r$d/$lib" ] && found=1; done
            [ $found = 1 ] || fail "${f#$r}: needs $lib, not in $r"
        done
    done < <(find "$r" -type f -print0)
    for s in /init /etc/inittab /etc/glass/rcS /etc/glass/console /usr/sbin/glass-gadget /usr/sbin/glass-collect \
        /sbin/init /bin/sh /sbin/getty /sbin/switch_root /sbin/mdev /sbin/ip /usr/sbin/dropbear /usr/bin/dbclient \
        /usr/sbin/i2cdetect /usr/bin/devmem2 /bin/busybox-extras /root/.ssh/authorized_keys \
        /lib/firmware/brcm/brcmfmac4330-sdio.bin /lib/firmware/brcm/brcmfmac4330-sdio.txt /lib/firmware/brcm/BCM4330B1.hcd; do
        [ -e "$r$s" ] || [ -L "$r$s" ] || { fail "$r$s missing"; continue; }
        if [ -L "$r$s" ]; then
            t=$(readlink "$r$s")
            case "$t" in /*) t=$r$t ;; *) t=$(dirname "$r$s")/$t ;; esac
            [ -e "$t" ] || fail "$r$s -> $(readlink "$r$s") does not resolve in the root"
        fi
    done
    for s in "$r/init" "$r/etc/glass/"* "$r"/usr/sbin/glass-*; do
        [ -f "$s" ] || continue
        head -n 1 "$s" | grep -q '^#!/bin/sh' || continue
        [ -x "$s" ] || fail "$s not executable"
        sh -n "$s" || fail "$s: syntax"
    done
    echo "check-userland: $r: $n ELF files checked"
done
[ $bad = 0 ] && echo "check-userland: all good"
exit $bad
