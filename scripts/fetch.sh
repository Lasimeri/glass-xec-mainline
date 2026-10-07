#!/bin/bash
# fetch.sh: everything this build uses, downloaded into dl/ and verified, then
# unpacked (src/, tools/apk-host/, firmware/). Nothing here is committed (see
# .gitignore). After one run the whole build works offline.
#   linux-7.2.9: sha256 from kernel.org's sha256sums.asc, whose signature is
#     checked against the Kernel.org checksum autosigner key (fetched by WKD).
#   GlassHack/factory-kernel@1091b53 (Google's Glass 3.4.83 tree, the
#     hardware reference) and pmaports' config-google-glass.armhf: sha512
#     from pmaports' linux-google-glass APKBUILD.
#   Google's XE24 factory image (stock boot, recovery, system with the
#     BCM4330 calibration and Bluetooth patch) and XE22's rooted boot.img:
#     SHA-1 as developers.google.com/glass/tools-downloads/system lists them.
#   Alpine Linux 3.24.2 armv7 minirootfs: sha256 and Natanael Copa's
#     signature, key fingerprint pinned (alpinelinux.org/downloads lists it).
#   apk-tools 3.0.8 for the host (CachyOS x86_64_v3 package): sha256 pinned,
#     and its CachyOS signature checked when pacman's keyring is present.
#   linux-firmware brcm/brcmfmac4330-sdio.bin: sha256 pinned (the same bytes
#     as Arch's linux-firmware-broadcom 20260916).
#   Alpine packages for the userland: signed, checked by apk against the
#     minirootfs' keys, cached in dl/apk-cache by scripts/build-userland.sh.
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
cd "$top"
mkdir -p dl/xe24 dl/alpine dl/host dl/firmware src firmware tools/apk-host
KV=${KV:-7.2.9}
cd dl
[ -f "linux-$KV.tar.xz" ] || curl -fLO "https://cdn.kernel.org/pub/linux/kernel/v${KV%%.*}.x/linux-$KV.tar.xz"
curl -fsLO "https://cdn.kernel.org/pub/linux/kernel/v${KV%%.*}.x/sha256sums.asc"
gpg --list-keys autosigner@kernel.org > /dev/null 2>&1 || gpg --auto-key-locate clear,wkd --locate-keys autosigner@kernel.org
gpg --verify sha256sums.asc
grep " linux-$KV.tar.xz\$" sha256sums.asc | sha256sum -c
F=1091b53a0b5e20d23a8447161e181e2a53e944de
[ -f factory-kernel-1091b53.tar.gz ] || curl -fL -o factory-kernel-1091b53.tar.gz "https://github.com/GlassHack/factory-kernel/archive/$F.tar.gz"
[ -f config-google-glass.armhf ] || curl -fLO https://gitlab.com/postmarketOS/pmaports/-/raw/master/device/testing/linux-google-glass/config-google-glass.armhf
sha512sum -c <<SUMS
b98916d6cc745115e89be35158b4cf0e025e73261a8f56302e9b74d8a7b597df8bdef1ee4ce8503dfc4fc94be16b91227b358cba7c5bc1fabde182876f6becf7  factory-kernel-1091b53.tar.gz
dff792cfbb097adc8292f9beadceb4808735bccb2e5e9455b407faaba2e2938d021fe1f0b8bfd301fe300b72ae368673d1a4dbfb0912d878af6eb171732ff25a  config-google-glass.armhf
SUMS

# Google's images. XE24's rooted boot.img and every XE23 image are gone from
# dl.google.com (403 early 2026, 404 by October); XE24 root comes from
# patching the stock boot.img instead. Nothing older than XE22 is ever
# fetched: XE9 and earlier brick a unit on XE10 or later (Google's warning).
[ -f xe24/factory-xe24.zip ] || curl -fL -o xe24/factory-xe24.zip https://storage.googleapis.com/support-kms-prod/bTh25b2gcZx5f7apQdJU3lULYTTBoZDHqdsr
[ -f boot-rooted-xe22.img ] || curl -fL -o boot-rooted-xe22.img https://dl.google.com/glass/xe22/boot.img
sha1sum -c <<SUMS
46430bc827267796566f0a73d9b503059759561c  xe24/factory-xe24.zip
ae713187ee619dd3b24e2caf650b9b7bcb68305b  boot-rooted-xe22.img
SUMS

# The AOSP 5.1.1 ROM for glass_1 (jtxdriggers, XDA, April 2016), a reference
# only (docs/hardware-test.md): its boot.img is a known-good custom boot
# image for this bootloader. MD5 as the XDA post lists it; archive.org mirror.
mkdir -p aosp
[ -f aosp/aosp_glass-1_5.1.1_042016.zip ] || curl -fL -o aosp/aosp_glass-1_5.1.1_042016.zip https://archive.org/download/aosp_glass-1_5.1.1_042016/aosp_glass-1_5.1.1_042016.zip
md5sum -c <<SUMS
595c4e0b50fa191f42eb5609cfbf92fd  aosp/aosp_glass-1_5.1.1_042016.zip
SUMS

# Alpine's armv7 base.
A=alpine-minirootfs-3.24.2-armv7.tar.gz
AB=https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/armv7
for f in $A $A.asc $A.sha256; do [ -f alpine/$f ] || curl -fL -o alpine/$f $AB/$f; done
[ -f alpine/ncopa.asc ] || curl -fL -o alpine/ncopa.asc https://alpinelinux.org/keys/ncopa.asc
g=$(mktemp -d -p "$top/out" 2>/dev/null || mktemp -d)
chmod 700 "$g"
gpg --homedir "$g" -q --import alpine/ncopa.asc 2> /dev/null
gpg --homedir "$g" --with-colons --fingerprint | grep -q '^fpr:::::::::0482D84022F52DF1C4E7CD43293ACD0907D9495A:' ||
    { echo "fetch: ncopa.asc is not the key alpinelinux.org/downloads lists" >&2; exit 1; }
gpg --homedir "$g" --verify alpine/$A.asc alpine/$A
(cd alpine && sha256sum -c $A.sha256)

# apk for the host.
P=apk-tools-3.0.8-1.1-x86_64_v3.pkg.tar.zst
PB=https://mirror.krfoss.org/cachyos/repo/x86_64_v3/cachyos-extra-v3
for f in $P $P.sig; do [ -f host/$f ] || curl -fL -o host/$f $PB/$f; done
sha256sum -c <<SUMS
6a7c0da270213296d3d9003345025f8ca38aa9d1f5bf98904890ee79fddbdb9f  host/$P
SUMS
if [ -r /etc/pacman.d/gnupg/pubring.gpg ]; then
    cp /etc/pacman.d/gnupg/pubring.gpg "$g/pacman.gpg"
    gpg --homedir "$g" --no-default-keyring --keyring "$g/pacman.gpg" --verify host/$P.sig host/$P
fi
rm -rf "$g"

# The Wi-Fi firmware.
W=brcmfmac4330-sdio.bin
[ -f firmware/$W ] || curl -fL -o firmware/$W "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/brcm/$W"
sha256sum -c <<SUMS
69ee0e9b9a4233b320cdfdd935853cdf2fbd84ff6ec42f8ec572f2f0a19975a7  firmware/$W
SUMS

# Unpacking.
cd "$top/src"
[ -d "linux-$KV" ] || tar xf "../dl/linux-$KV.tar.xz"
[ -d "factory-kernel-$F" ] || tar xf ../dl/factory-kernel-1091b53.tar.gz
cd "$top"
[ -x tools/apk-host/usr/bin/apk ] || bsdtar -xf dl/host/$P -C tools/apk-host usr
cd dl/xe24
for f in boot.img recovery.img android-info.txt fpga.img; do [ -f $f ] || unzip -o -q factory-xe24.zip $f; done
# The BCM4330's files from the stock system partition (proprietary: they
# stay in firmware/, which is never committed).
if [ ! -f "$top/firmware/bcmdhd.cal" ] || [ ! -f "$top/firmware/bcm4330.hcd" ] || [ ! -f "$top/firmware/fw_bcmdhd.bin" ]; then
    unzip -o -q factory-xe24.zip system.img
    if file system.img | grep -q 'Android sparse'; then
        simg2img system.img system.raw.img && mv system.raw.img system.img
    fi
    debugfs -R "dump /etc/wifi/bcmdhd.cal $top/firmware/bcmdhd.cal" system.img
    debugfs -R "dump /vendor/firmware/bcm4330.hcd $top/firmware/bcm4330.hcd" system.img
    debugfs -R "dump /vendor/firmware/fw_bcmdhd.bin $top/firmware/fw_bcmdhd.bin" system.img
    rm -f system.img
fi
cp "$top/dl/firmware/$W" "$top/firmware/$W"
sha256sum -c <<SUMS
$(cat "$top/scripts/firmware.sha256" | sed "s|  |  $top/firmware/|")
SUMS
echo "fetch: everything verified; sources in src/, apk in tools/apk-host/, firmware in firmware/"
