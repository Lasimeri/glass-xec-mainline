#!/bin/bash
# glass-capture.sh MONITOR FPS: one desktop monitor as raw NV12, 640x360, FPS
# frames a second, on stdout; run by glass-monitor (tools/glass-monitor).
#
# The monitor comes by name from KWin (desk cast --output, no dialog and no
# remembered choice), its stream linked here by port: WirePlumber will not link a
# client to KWin's node ("target not found"; the portal's connection it does),
# pw-link will, frames in 32 to 127 ms (measured 2026-11-04).
#
# After that the chain is glass-view.sh's. The capture asks for at most 9/8 of
# FPS and KWin re-sends its last frame while the screen is still; the CUDA
# compositor at latency 0 is the one rate stage, FPS frames a second on its own
# deadline. Halving steps first (bilinear at exactly 2:1 is a true box filter),
# so text does not shimmer at 6:1.
set -u
W=640; H=360
desk=${DESK:-$HOME/deskpilot/target/release/desk}
out=${1:?MONITOR}; fps=${2:?FPS}
if [ "${3:-}" != linked ]; then
    exec "$desk" cast --output "$out" -- "$0" "$out" "$fps" linked @NODE@
fi
node=${4:?NODE}; name=glass-cap-$$
sg=$("$desk" outputs 2> /dev/null | awk -v o="$out:" '$1 == o { print $2 }')
sw=${sg%x*}; sh=${sg#*x}
steps=()
while [ $((sw / 2)) -ge "$W" ] && [ $((sh / 2)) -ge "$H" ]; do
    sw=$((sw / 2)); sh=$((sh / 2))
    steps+=(! cudascale ! "video/x-raw(memory:CUDAMemory),width=$sw,height=$sh")
done
gst-launch-1.0 -q pipewiresrc autoconnect=false do-timestamp=true keepalive-time=$((1000 / fps)) \
    stream-properties="props,node.name=$name,node.autoconnect=false" ! "video/x-raw,max-framerate=$((fps * 9 / 8))/1" \
    ! queue max-size-buffers=2 max-size-time=0 max-size-bytes=0 leaky=downstream \
    ! cudaupload "${steps[@]}" \
    ! cudacompositor latency=0 sink_0::xpos=0 sink_0::ypos=0 sink_0::width="$W" sink_0::height="$H" \
    ! "video/x-raw(memory:CUDAMemory),width=$W,height=$H,framerate=$fps/1" \
    ! cudaconvertscale ! "video/x-raw(memory:CUDAMemory),width=$W,height=$H,format=NV12" \
    ! cudadownload ! video/x-raw,format=NV12 ! fdsink fd=1 sync=false &
g=$!
trap 'kill $g 2> /dev/null; exit 0' TERM INT HUP
oid=""; iid=""; i=0
while { [ -z "$oid" ] || [ -z "$iid" ]; } && [ $i -lt 100 ]; do
    [ -n "$oid" ] || for p in $(pw-link -o -I 2> /dev/null | awk '/kwin_wayland/ { print $1 }'); do
        pw-cli info "$p" 2> /dev/null | grep -q "node.id = \"$node\"" && oid=$p
    done
    [ -n "$iid" ] || iid=$(pw-link -i -I 2> /dev/null | awk -v n="$name" 'index($0, n) { print $1; exit }')
    [ -n "$oid" ] && [ -n "$iid" ] && break
    sleep 0.05; i=$((i + 1))
done
if [ -z "$oid" ] || [ -z "$iid" ]; then
    echo "glass-capture: no ports for $out (node $node)" >&2
    kill $g 2> /dev/null
    exit 1
fi
pw-link "$oid" "$iid" 2>&1 | head -n 1 >&2
wait $g
