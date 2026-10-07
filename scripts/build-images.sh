#!/bin/bash
# build-images.sh: what goes to the Glass, in out/, with out/SHA256SUMS.
#   boot-safe.img        mainline + the safe DTB (no MMC: cannot touch the
#                        eMMC) + the built-in initramfs. The first boot.
#   boot-full.img        mainline + the full DTB (eMMC, Wi-Fi, Bluetooth,
#                        touchpad, IMU, gauge, camera button); switches to the
#                        rootfs on userdata when one is there.
#   boot-stock-kernel.img  Google's own XE24 kernel (3.4.94) + this
#                        initramfs as its ramdisk: the userland and USB link on
#                        known-good kernel code, when mainline does not come up.
#   boot-xe24-adbroot.img  Google's XE24 boot.img with ro.secure=0 and
#                        ro.debuggable=1: Android with root adb, for the backup
#                        and glass info (XE24's own rooted image is gone).
#   rootfs.img           the Alpine rootfs (ext4, sparse for fastboot) for
#                        userdata.
# Every boot image must fit in 5,861,376 bytes: the boot partition has held
# the AOSP 5.1.1 ROM's boot.img of that size on real units (dl/aosp), and
# the recovery partition Google's XE24 recovery.img of 6,344,704; the smaller
# of the two proven sizes is the bound, until a backup's partitions.txt
# gives the real ones.
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
KV=${KV:-7.2.9}
k=$top/build/kernel-$KV
src=$top/src/linux-$KV
out=$top/out
bi=$top/tools/bootimg/bootimg
mkdir -p "$out"
LIMIT=5861376
[ -x "$bi" ] || tcc -O2 -o "$bi" "$top/tools/bootimg/bootimg.c"

# The stock layout (deviceinfo and Google's images agree).
hdr=(--base 0x80000000 --kernel-offset 0x00008000 --ramdisk-offset 0x01000000
     --second-offset 0x00f00000 --tags-offset 0x00000100 --pagesize 2048)
# mainline: the 8250 console is ttyS2; the bootloader's ATAGs (memory,
# this command line) are folded into the appended DTB. clk_, regulator_ and
# pd_ignore_unused: mainline turns off unclaimed TWL6030 rails, clocks and
# power domains 30 s into boot, which would look like a hang (and would take
# the bootloader's display down). VGA8x16 gives 80 x 22 on the 640 x 360
# display.
mcmd="console=ttyS2,115200n8 earlycon panic=10 rdinit=/init loglevel=7 clk_ignore_unused regulator_ignore_unused pd_ignore_unused fbcon=font:VGA8x16"
scmd="console=ttyO2,115200n8 vmalloc=500M androidboot.console=ttyO2 androidboot.carrier=wifi-only product_type=w cpuidle_sysfs_switch"

fit() {   # IMG
    local s
    s=$(stat -c %s "$1")
    if [ "$s" -gt $LIMIT ]; then
        echo "build-images: $(basename "$1") is $s bytes, over the $LIMIT-byte bound" >&2
        return 1
    fi
    printf 'build-images: %-24s %8d bytes (%d under the bound)\n' "$(basename "$1")" "$s" $((LIMIT - s))
}

# A ramdisk the bootloader can load (an empty cpio, gzip): the mainline
# images carry their initramfs inside the kernel, out of reach of the ramdisk
# at 0x81000000, which the 15 MB decompressed kernel would overrun.
( cd "$out" && : | cpio -o -H newc --quiet 2> /dev/null | gzip -9n > empty-ramdisk.gz )

z=$k/arch/arm/boot/zImage
dtbdir=$k/arch/arm/boot/dts/ti/omap
[ -f "$z" ] || { echo "build-images: no $z (run scripts/build-kernel.sh)" >&2; exit 1; }
grep -q '^CONFIG_INITRAMFS_SOURCE="..*"' "$k/.config" ||
    { echo "build-images: the kernel has no built-in initramfs (GLASS_INITRAMFS)" >&2; exit 1; }
for v in safe full; do
    d=$dtbdir/omap4-glass-xec.dtb
    [ $v = safe ] && d=$dtbdir/omap4-glass-xec-safe.dtb
    [ -f "$d" ] || { echo "build-images: no $d" >&2; exit 1; }
    cat "$z" "$d" > "$out/zImage-dtb-$v"
    "$bi" pack "$out/boot-$v.img" "$out/zImage-dtb-$v" "$out/empty-ramdisk.gz" "${hdr[@]}" --cmdline "$mcmd" > /dev/null
    fit "$out/boot-$v.img"
done

# The stock kernel with this userland: its ramdisk is gzip (all it reads),
# owned by root through the kernel's own gen_init_cpio, without the
# brcmfmac firmware (the stock kernel's bcmdhd loads its own).
stock=$top/build/stock
mkdir -p "$stock"
"$bi" unpack "$top/dl/xe24/boot.img" "$stock" > /dev/null
slim=$top/build/initramfs-stock
rm -rf "$slim"
cp -a "$top/build/initramfs" "$slim"
rm -rf "$slim/lib/firmware"
( cd "$k" && "$src/usr/gen_initramfs.sh" -o "$stock/initramfs.cpio" -u "$(id -u)" -g "$(id -g)" \
    "$slim" "$top/userland/initramfs.list" )
gzip -9n < "$stock/initramfs.cpio" > "$stock/initramfs.cpio.gz"
"$bi" pack "$out/boot-stock-kernel.img" "$stock/kernel" "$stock/initramfs.cpio.gz" "${hdr[@]}" --cmdline "$scmd" > /dev/null
fit "$out/boot-stock-kernel.img" || echo "build-images: boot-stock-kernel.img is for fastboot boot only (RAM)"

# Google's XE24 Android with root adb: its own ramdisk with the three
# properties flipped in default.prop; nothing else changes.
ar=$top/build/xe24-adbroot
rm -rf "$ar"
mkdir -p "$ar/root"
( cd "$ar/root" && gzip -dc "$stock/ramdisk" | cpio -id --quiet 2> /dev/null )
sed -i -e 's/^ro.secure=1/ro.secure=0/' -e 's/^ro.adb.secure=1/ro.adb.secure=0/' \
    -e 's/^ro.debuggable=0/ro.debuggable=1/' "$ar/root/default.prop"
grep -q '^ro.secure=0' "$ar/root/default.prop" && grep -q '^ro.debuggable=1' "$ar/root/default.prop" ||
    { echo "build-images: default.prop did not take the root properties" >&2; exit 1; }
( cd "$ar/root" && find . | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet | gzip -9n > "$ar/ramdisk.gz" )
"$bi" pack "$out/boot-xe24-adbroot.img" "$stock/kernel" "$ar/ramdisk.gz" "${hdr[@]}" --cmdline "$scmd" > /dev/null
fit "$out/boot-xe24-adbroot.img"

# The rootfs: ext4 sized to its content plus room, every file root-owned
# (debugfs, since mke2fs -d copies the build user's ids), checked by e2fsck,
# then sparse for fastboot. It grows to fill userdata at its first boot.
if [ -d "$top/build/rootfs" ]; then
    r=$top/build/rootfs
    mb=$(( $(du -sm "$r" | cut -f1) * 2 + 256 ))
    img=$out/rootfs.ext4
    rm -f "$img"
    # Without the features Google's 3.4 kernel cannot mount (metadata_csum and
    # its seed from 3.6 on, orphan_file, 64bit), so both kernels can use it.
    mke2fs -q -t ext4 -O ^metadata_csum,^metadata_csum_seed,^orphan_file,^64bit -L glass-root -d "$r" "$img" "${mb}M"
    ( cd "$r" && find . -mindepth 1 | sed 's|^\.||' | while IFS= read -r p; do
        printf 'sif "%s" uid 0\nsif "%s" gid 0\n' "$p" "$p"
      done; printf 'sif / uid 0\nsif / gid 0\n' ) > "$out/rootfs-owner.cmd"
    debugfs -w -f "$out/rootfs-owner.cmd" "$img" > /dev/null 2> "$out/rootfs-owner.log"
    if grep -v '^debugfs' "$out/rootfs-owner.log" | grep -q .; then
        echo "build-images: debugfs reported:" >&2
        head "$out/rootfs-owner.log" >&2
        exit 1
    fi
    e2fsck -fn "$img" > "$out/rootfs-fsck.log" 2>&1 || { cat "$out/rootfs-fsck.log" >&2; exit 1; }
    [ "$(debugfs -R 'stat /etc/passwd' "$img" 2> /dev/null | sed -n 's/.*User: *\([0-9]*\).*/\1/p')" = 0 ] ||
        { echo "build-images: /etc/passwd is not root-owned in the image" >&2; exit 1; }
    img2simg "$img" "$out/rootfs.img"
    rm -f "$img"
    echo "build-images: rootfs.img $(stat -c %s "$out/rootfs.img") bytes (sparse), ${mb} MB filesystem"
fi

( cd "$out" && sha256sum boot-*.img rootfs.img 2> /dev/null > SHA256SUMS )
echo "build-images: done; out/SHA256SUMS"
