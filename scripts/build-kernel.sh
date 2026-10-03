#!/bin/bash
# build-kernel.sh: the mainline kernel for Glass (zImage, modules, and the
# Glass DTB once dts/ has it), built with LLVM (clang, lld) inside a
# bubblewrap sandbox where Python does not exist:
#   every real python binary under /usr/bin is covered by /dev/null (exec
#   fails), and tools/nopython/ (shims that log and fail) is first on PATH.
# out/python-calls.log must stay empty; the script fails if it does not.
# Then every option in config/glass.config is checked against .config.
#   KV=7.2.9 JOBS=4 scripts/build-kernel.sh [make targets]
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
KV=${KV:-7.2.9}
src=$top/src/linux-$KV
out=$top/build/kernel-$KV
log=$top/out/python-calls.log
mkdir -p "$out" "$top/out"
: > "$log"
[ -d "$src" ] || { echo "build-kernel: no $src (run scripts/fetch.sh)" >&2; exit 1; }

masks=()
for p in /usr/bin/python*; do
    [ -f "$p" ] && [ ! -L "$p" ] && masks+=(--ro-bind /dev/null "$p")
done
for d in /usr/lib/python3*; do [ -d "$d" ] && masks+=(--tmpfs "$d"); done

targets=${*:-zImage modules}
# The initramfs built into the kernel (GLASS_INITRAMFS, a directory from
# scripts/build-userland.sh): kbuild maps the build user's files to root.
frag2=""
if [ -n "${GLASS_INITRAMFS:-}" ]; then
    frag2=$top/out/initramfs.config
    cat > "$frag2" <<FRAG
CONFIG_INITRAMFS_SOURCE="$GLASS_INITRAMFS $top/userland/initramfs.list"
CONFIG_INITRAMFS_ROOT_UID=$(id -u)
CONFIG_INITRAMFS_ROOT_GID=$(id -g)
CONFIG_INITRAMFS_COMPRESSION_XZ=y
FRAG
fi
# The Glass device trees, built by kbuild from the kernel's own tree.
cp "$top"/dts/*.dts "$top"/dts/*.dtsi "$src/arch/arm/boot/dts/ti/omap/"
inner=$(cat <<EOF
set -euo pipefail
M="make -C $src O=$out ARCH=arm LLVM=1 PYTHON3=false"
[ -f $out/.config ] || \$M omap2plus_defconfig
$src/scripts/kconfig/merge_config.sh -m -O $out $out/.config $top/config/glass.config $frag2 > /dev/null
\$M olddefconfig > /dev/null
# Every option the fragment sets, as it ended up.
bad=0
while IFS= read -r l; do
    case "\$l" in
        CONFIG_*=*) k=\${l%%=*}; grep -qx "\$l" $out/.config || { echo "config: \$l wanted, got: \$(grep -E "^\$k=|^# \$k is not set" $out/.config || echo absent)"; bad=1; } ;;
        "# CONFIG_"*" is not set") k=\${l#\# }; k=\${k%% *}; grep -qE "^\$k=" $out/.config && { echo "config: \$k should be unset"; bad=1; } ;;
    esac
done < $top/config/glass.config
[ \$bad = 0 ] && echo "config: every fragment option applied"
chrt -i 0 nice -n 19 ionice -c 3 \$M -j${JOBS:-4} $targets
EOF
)
bwrap --dev-bind / / "${masks[@]}" \
    --setenv PATH "$top/tools/nopython:$PATH" --setenv NOPYTHON_LOG "$log" \
    -- bash -c "$inner"
if [ -s "$log" ]; then
    echo "build-kernel: PYTHON WAS CALLED:" >&2
    cat "$log" >&2
    exit 3
fi
echo "build-kernel: no Python called ($log empty)"
ls -la "$out/arch/arm/boot/zImage" "$out/arch/arm/boot/Image" 2>/dev/null || true
