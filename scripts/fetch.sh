#!/bin/bash
# fetch.sh: the sources this build uses, downloaded into dl/ and verified,
# then unpacked into src/. Nothing here is committed (see .gitignore).
#   linux-7.2.9: sha256 from kernel.org's sha256sums.asc, whose signature is
#     checked against the Kernel.org checksum autosigner key (fetched by WKD).
#   GlassHack/factory-kernel@1091b53 (Google's Glass 3.4.83 tree, the
#     hardware reference) and pmaports' config-google-glass.armhf: sha512
#     from pmaports' linux-google-glass APKBUILD.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p dl src
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
cd ../src
[ -d "linux-$KV" ] || tar xf "../dl/linux-$KV.tar.xz"
[ -d "factory-kernel-$F" ] || tar xf ../dl/factory-kernel-1091b53.tar.gz
echo "fetch: sources verified and unpacked in src/"
