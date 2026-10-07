#!/bin/bash
# build-userland.sh: the two Alpine 3.24 armv7 userlands, from the verified
# minirootfs plus packages that apk checks against its keys. Package scripts
# never run (--no-scripts): nothing armv7 executes on this host, and what
# those scripts would do (users, links) is done here by hand.
#   build/initramfs  the lifeline, built into the kernel: busybox, dropbear
#                    (SSH server and client), i2c-tools, devmem2, the BCM4330
#                    firmware, userland/initramfs/ on top.
#   build/rootfs     the full system for the userdata partition: Wi-Fi
#                    (wpa_supplicant, iw), Bluetooth (BlueZ, bluez-tools for
#                    PAN), tmux, e2fsprogs, userland/rootfs/ on top; written
#                    to out/rootfs.img by scripts/build-images.sh.
# The SSH key that may log in as root: GLASS_SSH_PUBKEY, else
# ~/.ssh/id_ed25519.pub, else ~/.ssh/id_rsa.pub.
#   scripts/build-userland.sh [initramfs|rootfs|all]
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
what=${1:-all}
mini=$top/dl/alpine/alpine-minirootfs-3.24.2-armv7.tar.gz
[ -f "$mini" ] || { echo "build-userland: run scripts/fetch.sh first" >&2; exit 1; }
apk() {
    LD_LIBRARY_PATH=$top/tools/apk-host/usr/lib "$top/tools/apk-host/usr/bin/apk" \
        --arch armv7 --no-scripts --cache-dir "$top/dl/apk-cache" --quiet "$@"
}
key=${GLASS_SSH_PUBKEY:-}
if [ -z "$key" ]; then
    for k in "$HOME/.ssh/id_ed25519.pub" "$HOME/.ssh/id_rsa.pub"; do
        [ -f "$k" ] && { key=$k; break; }
    done
fi
[ -n "$key" ] && [ -f "$key" ] || { echo "build-userland: no SSH public key (set GLASS_SSH_PUBKEY)" >&2; exit 1; }
mkdir -p "$top/dl/apk-cache"

# The parts both share: the scripts, the firmware, root's key.
common() {
    local r=$1
    cp -a "$top/userland/initramfs/." "$r/"
    chmod 755 "$r/init" "$r/etc/glass/rcS" "$r/etc/glass/console" "$r"/usr/sbin/glass-*
    local fw=$r/lib/firmware/brcm
    mkdir -p "$fw"
    cp "$top/firmware/brcmfmac4330-sdio.bin" "$fw/"
    # Glass's own calibration (BCM94330 rev B2) as brcmfmac's NVRAM file,
    # under the board-specific name brcmfmac tries first and the generic one.
    cp "$top/firmware/bcmdhd.cal" "$fw/brcmfmac4330-sdio.txt"
    ln -sf brcmfmac4330-sdio.txt "$fw/brcmfmac4330-sdio.google,glass-xec.txt"
    # The Bluetooth patch: btbcm asks for BCM4330B1.hcd (subversion 0x4103),
    # or BCM.hcd when the revision is not in its table.
    cp "$top/firmware/bcm4330.hcd" "$fw/BCM4330B1.hcd"
    ln -sf BCM4330B1.hcd "$fw/BCM.hcd"
    mkdir -p "$r/root/.ssh"
    cp "$key" "$r/root/.ssh/authorized_keys"
    chmod 700 "$r/root/.ssh"
    chmod 600 "$r/root/.ssh/authorized_keys"
}

if [ "$what" = initramfs ] || [ "$what" = all ]; then
    r=$top/build/initramfs
    rm -rf "$r"
    mkdir -p "$r"
    tar -xzf "$mini" -C "$r"
    apk --root "$r" add dropbear dropbear-dbclient i2c-tools devmem2 busybox-extras
    # What only apk itself needs (TLS, the package database) goes: the
    # initramfs is built into the kernel and every byte counts.
    apk --root "$r" del apk-tools ssl_client
    rm -rf "$r/lib/apk" "$r/etc/apk" "$r/usr/share/apk" "$r/var/cache/apk" \
        "$r/usr/share/man" "$r/usr/share/doc" "$r/etc/ssl" "$r/usr/lib/libcrypto.so"* "$r/usr/lib/libssl.so"* 2> /dev/null || true
    common "$r"
    du -sh "$r"
fi

if [ "$what" = rootfs ] || [ "$what" = all ]; then
    r=$top/build/rootfs
    rm -rf "$r"
    mkdir -p "$r"
    tar -xzf "$mini" -C "$r"
    apk --root "$r" add dropbear dropbear-dbclient dropbear-scp i2c-tools devmem2 busybox-extras \
        wpa_supplicant iw wireless-regdb bluez bluez-deprecated bluez-tools dbus \
        tmux kbd e2fsprogs e2fsprogs-extra evtest
    common "$r"
    cp -a "$top/userland/rootfs/." "$r/"
    chmod 755 "$r/etc/glass/rcS-rootfs" "$r"/usr/sbin/glass-*
    # Glass's own Wi-Fi firmware (5.90.195.122, 2014) beside linux-firmware's
    # (5.90.195.114, 2013): glass-wifi-fw switches between them.
    cp "$top/firmware/fw_bcmdhd.bin" "$r/lib/firmware/brcm/brcmfmac4330-sdio.glass.bin"
    # The same firmware and the calibration under the names Google's bcmdhd
    # driver (the stock kernel) takes through its module parameters; glass-wifi
    # points it there.
    mkdir -p "$r/lib/firmware/glass"
    cp "$top/firmware/fw_bcmdhd.bin" "$top/firmware/bcmdhd.cal" "$r/lib/firmware/glass/"
    echo glass > "$r/etc/hostname"
    # The static ffmpeg (dl/ffmpeg-arm, scripts/fetch.sh) that decodes the
    # desktop stream (scripts/glass-view.sh); 32 MB, so only when fetched.
    ff=$(ls -d "$top"/dl/ffmpeg-arm/ffmpeg-*-armhf-static/ffmpeg 2> /dev/null | head -n 1)
    if [ -n "$ff" ]; then
        mkdir -p "$r/usr/local/bin"
        cp "$ff" "$r/usr/local/bin/ffmpeg"
    fi
    # What dbus' pre-install script would have done.
    grep -q '^messagebus:' "$r/etc/group" || echo 'messagebus:x:101:' >> "$r/etc/group"
    grep -q '^messagebus:' "$r/etc/passwd" ||
        echo 'messagebus:x:100:101:messagebus:/var/run/dbus:/sbin/nologin' >> "$r/etc/passwd"
    grep -q '^messagebus:' "$r/etc/shadow" || echo 'messagebus:!::0:::::' >> "$r/etc/shadow"
    # The marker /init looks for before it switches to this root.
    echo "glass-xec rootfs, built $(date -u +%Y-%m-%dT%H:%MZ)" > "$r/etc/glass-rootfs"
    du -sh "$r"
fi
echo "build-userland: done ($what), root login key $key"
