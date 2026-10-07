# Daily use

The Glass runs Alpine Linux on Google's own kernel (XE24, 3.4). It shows the desktop's left monitor, plays the desktop's sound, and turns a touchpad tap into the voice mute. Everything below starts by itself; this page says what does, how to reach the Glass, and the one command for each setting.

## What starts by itself

**On the Glass, at every boot:**

| part | what it does |
| --- | --- |
| Wi-Fi (`glass-wifi up`) | joins the stored networks, home first (`priority`); the address follows the network joined: fixed at home, DHCP elsewhere |
| clock | set from the internet once an address exists, retried for two minutes |
| Tailscale (`tailscaled`) | the Glass on the tailnet as `glass`, reachable from the desktop on any network |
| display shell (`glass-console`) | a shell with a blinking cursor and two status lines (network and address, tailnet address, battery), shown whenever no stream is |
| panel | brightness as kept (`glass-brightness`) |
| ssh (`dropbear`) | key login only, the desktop's `~/.ssh/glass_ed25519` |

**On the desktop, at login** (`~/.config/autostart`, written by `scripts/glass setup-desktop`):

| supervisor | what it does |
| --- | --- |
| `glass-viewd.sh` | waits for the Glass, opens a terminal with a shell on it, streams the monitor at 15 frames/s; restarts the stream when the Glass returns |
| `glass-audio.sh` | makes the "Google Glass" sound output the default while the Glass is up; the stereo comes back when it leaves |
| `glass-tap.sh` | a tap on the touchpad toggles the voice mute (`ptt079 --toggle`) |

## Reaching it

`scripts/glass ssh` finds the Glass by itself, in this order: the remembered address (`out/glass-ip`, at home the fixed 192.168.0.80), the router's name `glass`, the USB cable (172.16.42.1), the tailnet address, then a search of the home network by the Wi-Fi MAC. Every address is checked against one pinned host key.

On the phone's hotspot the Glass is reached through the tailnet; nothing else on the home network can reach a hotspot client.

`scripts/glass status` shows the whole state in one table: how the Glass is reachable, battery, board temperature, CPU clock, display and sound settings, what runs on it, the last stream report and the desktop's supervisors.

## Settings

On the Glass (in its shell, or from the desktop as `scripts/glass ssh 'COMMAND'`). Each is kept across boots.

| setting | command | now |
| --- | --- | --- |
| volume | `glass-audio volume 75` | 75 |
| sound output | `glass-audio route earphone 75` (the bone conduction speaker) or `headset` | earphone |
| sound delay behind the desktop | `glass-audio delay 150` (applies within 2 s) | 150 ms |
| brightness | `glass-brightness 80` (1 to 223) | 80 |
| a fixed address on a network | `glass-wifi static HomeMixed 192.168.0.80/24 192.168.0.1`; `glass-wifi static HomeMixed off` for DHCP | home: 192.168.0.80 |
| which network first | `glass-wifi priority HomeMixed 10` (higher first) | home 10, hotspot 0 |
| a new Wi-Fi network | from the desktop, without typing the passphrase: `scripts/glass-wifi-copy --from USER@HOST "NAME"`; on the Glass: `glass-wifi add "NAME"` | HomeMixed, O+ Open |

On the desktop:

| setting | where |
| --- | --- |
| which monitor | `scripts/glass-view.sh MONITOR 15` once (remembered in `out/glass-output`); now DP-2, the left one |
| frame rate | `scripts/glass-viewd.sh` passes 15; the Glass is held to 300 MHz by its own thermal governors when warm, and 24 a second was not kept there |
| video bit rate | `BITRATE=3000` (kbit/s) for `glass-view.sh` |

## Keeping it current

| when | command |
| --- | --- |
| after changing or pulling this repository | `scripts/glass update`: packages, scripts and tools on the running Glass brought up to date, tools rebuilt on the Glass only where their source changed, only what changed restarted; settings above kept; nothing flashed |
| on a new desktop install | `scripts/glass setup-desktop`: the autostart entries, the supervisors started, and what is missing with its fix |
| a fresh rootfs | `scripts/glass build`, then `scripts/glass flash-rootfs` (asks first); the image carries everything above |

## When something is off

| look at | for |
| --- | --- |
| `scripts/glass status` | the whole state first |
| `$XDG_RUNTIME_DIR/glass-viewd.log` | the stream: a line every 5 s with frames shown, dropped, late |
| `$XDG_RUNTIME_DIR/glass-audio.log` | the sound: glass-play's delay, corrections and underruns every 30 s |
| `/run/glass/rcS.log` on the Glass | boot, Wi-Fi joins, the address taken, the clock |
| `/run/glass/glass-console.log`, `/run/glass/tailscaled.log` | the display shell, Tailscale |
