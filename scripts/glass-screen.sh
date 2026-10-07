#!/bin/bash
# glass-screen.sh: a screen for the glasses on the desktop: a second KWin
# running nested as a window at the top of the main monitor, WxH at SCALE,
# so whatever is launched into it draws SCALE times larger. The stream
# (`glass-view.sh window`) captures that window alone: no 4K downscale,
# text sized for a 640x360 display.
#   glass-screen.sh start [W H SCALE]   default 1280 720 2 (a 640x360 session,
#                                       everything twice the size)
#   glass-screen.sh run CMD [ARGS...]   launch a program into it
#   glass-screen.sh stop
# The window is pinned to the top-left of OUTPUT (default: the primary) by
# a KWin window rule. Inside, WAYLAND_DISPLAY=wayland-glass.
set -u
sock=wayland-glass
rule=glass-screen
case "${1:-}" in
    start)
        W=${2:-1280}; H=${3:-720}; S=${4:-2}
        if pgrep -f "kwin_wayland.*--socket $sock" > /dev/null; then
            echo "glass-screen: already running (WAYLAND_DISPLAY=$sock)"; exit 0
        fi
        out=${OUTPUT:-}
        if [ -z "$out" ]; then
            out=$(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Output:/ { o=$3 } /priority 1/ { print o; exit }')
            [ -n "$out" ] || out=DP-1
        fi
        # The output's top-left corner in global coordinates (desk outputs:
        # "NAME: WxH at X,Y").
        pos=$("$HOME/deskpilot/target/release/desk" outputs 2>/dev/null | awk -v o="$out:" '$1 == o { sub(/,$/, "", $4); print $4 }')
        pos=${pos:-0,0}
        # A window rule: this KWin's window sits at the output's top-left, sized
        # as asked, on every desktop and above others, no decoration.
        f=$HOME/.config/kwinrulesrc
        kwriteconfig6 --file "$f" --group "$rule" --key Description "Glass screen (glass-screen.sh)"
        kwriteconfig6 --file "$f" --group "$rule" --key wmclass "kwin_wayland"
        kwriteconfig6 --file "$f" --group "$rule" --key wmclassmatch 0
        kwriteconfig6 --file "$f" --group "$rule" --key title "KDE Wayland Compositor"
        kwriteconfig6 --file "$f" --group "$rule" --key titlematch 2
        kwriteconfig6 --file "$f" --group "$rule" --key position "$pos"
        kwriteconfig6 --file "$f" --group "$rule" --key positionrule 2
        kwriteconfig6 --file "$f" --group "$rule" --key size "$W,$H"
        kwriteconfig6 --file "$f" --group "$rule" --key sizerule 2
        kwriteconfig6 --file "$f" --group "$rule" --key noborder true
        kwriteconfig6 --file "$f" --group "$rule" --key noborderrule 2
        kwriteconfig6 --file "$f" --group "$rule" --key above true
        kwriteconfig6 --file "$f" --group "$rule" --key aboverule 2
        kwriteconfig6 --file "$f" --group "$rule" --key desktops "\\0"
        kwriteconfig6 --file "$f" --group "$rule" --key desktopsrule 2
        groups=$(kreadconfig6 --file "$f" --group General --key rules)
        case ",$groups," in *",$rule,"*) ;; *) kwriteconfig6 --file "$f" --group General --key rules "${groups:+$groups,}$rule" ;; esac
        n=$(kreadconfig6 --file "$f" --group General --key count); kwriteconfig6 --file "$f" --group General --key count "$(( $(echo "${groups:+$groups,}$rule" | tr ',' '\n' | grep -c .) ))"
        qdbus6 org.kde.KWin /KWin reconfigure > /dev/null 2>&1 || true
        echo "glass-screen: nested KWin ${W}x${H} at scale $S, top-left of $out ($pos); WAYLAND_DISPLAY=$sock inside"
        setsid nohup kwin_wayland --socket "$sock" --width "$W" --height "$H" --scale "$S" --xwayland --no-lockscreen \
            --exit-with-session "env WAYLAND_DISPLAY=$sock konsole --notransparency -p tabtitle=glass-screen" \
            > "${XDG_RUNTIME_DIR:-/tmp}/glass-screen.log" 2>&1 < /dev/null &
        ;;
    run)
        shift
        [ $# -gt 0 ] || { echo "glass-screen.sh run CMD [ARGS...]" >&2; exit 1; }
        pgrep -f "kwin_wayland.*--socket $sock" > /dev/null || { echo "glass-screen: not running (glass-screen.sh start)" >&2; exit 1; }
        setsid nohup env WAYLAND_DISPLAY=$sock "$@" > /dev/null 2>&1 < /dev/null &
        echo "glass-screen: $* launched into the Glass screen"
        ;;
    stop)
        for p in $(pgrep -f "kwin_wayland.*--socket $sock"); do kill "$p"; done
        # The placement rule goes with it.
        f=$HOME/.config/kwinrulesrc
        rules=$(kreadconfig6 --file "$f" --group General --key rules | tr ',' '\n' | grep -vx "$rule" | paste -sd,)
        kwriteconfig6 --file "$f" --group General --key rules "$rules"
        kwriteconfig6 --file "$f" --group General --key count "$(echo "$rules" | tr ',' '\n' | grep -c .)"
        sed -i "/^\[$rule\]\$/,/^\$/d" "$f"
        qdbus6 org.kde.KWin /KWin org.kde.KWin.reconfigure > /dev/null 2>&1 || true
        echo "glass-screen: stopped, rule removed"
        ;;
    *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
