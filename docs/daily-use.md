# Daily use

The Glass runs Alpine Linux on Google's own kernel (XE24, 3.4). It shows the desktop's left monitor, plays the desktop's sound, and turns a touchpad tap into the voice mute. Everything below starts by itself; this page says what does, how to reach the Glass, and the one command for each setting.

## What starts by itself

**On the Glass, at every boot:**

| part | what it does |
| --- | --- |
| Wi-Fi (`glass-wifi up`) | joins the stored networks by `priority`: the phone's hotspot first when it is on, else home; the address follows the network joined: fixed at home, DHCP elsewhere; a watchdog reassociates after 30 s without a network and restarts Wi-Fi after 90 s |
| clock | set from the internet once an address exists, retried for two minutes |
| Tailscale (`tailscaled`) | the Glass on the tailnet as `glass`, reachable from the desktop on any network |
| display shell (`glass-console`) | a shell with a blinking cursor and two status lines (network and address, tailnet address, battery), shown whenever no stream is |
| panel | brightness as kept (`glass-brightness`) |
| ssh (`dropbear`) | key login only, the desktop's `~/.ssh/glass_ed25519` |

**On the desktop, at login** (`~/.config/autostart`, written by `scripts/glass setup-desktop`):

| supervisor | what it does |
| --- | --- |
| `glass-viewd.sh` | waits for the Glass, opens a terminal with a shell on it, streams the monitor at 15 frames/s; restarts the stream when the Glass returns |
| `glass-audio.sh` | while the Glass is connected, makes the "Google Glass" sound output the default and moves the application streams and the voice (079's speech, the echo canceller's playback) to it; when the Glass is gone, back to the stereo. A change of path (the cable out, Wi-Fi back) keeps the sound on the Glass. 32 kHz mono, raw at home and Opus at 48 kbit/s over the tailnet. The microphone loopbacks stay where they are aimed. `glass audio` switches by hand (below) |
| `glass-tap.sh` | a tap on the touchpad toggles the voice mute (`ptt079 --toggle`) |

## Reaching it

`scripts/glass ssh` finds the Glass by itself, in this order: the remembered address (`out/glass-ip`, at home the fixed 192.168.0.80), the router's name `glass`, the USB cable (172.16.42.1), the tailnet address, then a search of the home network by the Wi-Fi MAC. Every address is checked against one pinned host key.

On the phone's hotspot the Glass is reached through the tailnet; nothing else on the home network can reach a hotspot client. Over the tailnet the stream switches to a remote mode by itself: video at 12 frames/s and 1500 kbit/s with about 250 ms (3 frames) of cushion, sound as Opus held 400 ms behind. The hotspot path measured 2026-10-07 delivered about 1.7 Mbit/s: 1200 kbit/s at 15/s held 15.0 frames/s, 2000 kbit/s at 12/s showed 9.4 to 11.4 with late frames. When the Glass changes network, every session follows by itself: a path guard beside each one (`scripts/glass-pathguard.sh`) ends a tailnet session as soon as home Wi-Fi or the USB cable answers, and a home session after two missed pings; its supervisor starts it again on the new path, in that path's mode. The Glass shell window is left alone (it keeps working over the tailnet, and reopens if its path dies).

## The camera

`scripts/glass camera` opens the Glass's camera in its own window on the desktop: 960x540 (half the camera's 1920x1080 video size), 15 frames/s, H.264 at 768 kbit/s, no sound. Other values as `scripts/glass camera WxH FPS KBIT/S`. The work is done by the Ducati (the OMAP4's Cortex-M3 cores with Google's firmware): the OV5680 camera and the H.264 encoder there share the frames in place, the Glass's own CPU only passes on the stream. Closing the window stops the camera. On the Glass itself, `glass-camera -i` lists what the camera and the encoder offer; the protocol is in `docs/ducati-omx.md`.

Face tracking, as the desktop's own camera has it: the window ("079 glass", on the camera screen) is the picture after the desktop's RTX 3090 Ti has decoded it (NVDEC) and `~/tts079/facetrack` has found faces in it (YuNet through OpenCL on the same card) and locked a reticle on the nearest, with the same HUD, low-light lift, scalers and FSR as the desktop's camera windows. Like the other cameras it writes a status line a second for the Phi Stream's camera monitor (`~/.local/share/phi-stream/dev/feeds/glass.status` and `.log`: faces, lock, box, motion, light, rate) and keeps its newest frame (`~/.cache/voice-079/cam/latest-glass.jpg`). `GLASS_CAMERA_TRACK=0 scripts/glass camera` shows the camera as it is.

The USB cable is a charger and a fallback, never the working path: every session (stream, sound, volume link, tap, the camera window) is on home Wi-Fi whenever Wi-Fi answers. A session that fell back to the cable (Wi-Fi was down) moves to Wi-Fi by itself once Wi-Fi has answered steadily for 9 s, cable still in; and if the cable comes out while a session is on it, that session ends at once and starts again on Wi-Fi (or the tailnet) as soon as the Glass answers there. Pulling the cable therefore changes nothing in normal use. `scripts/glass unplug-test [ROUNDS] [HOLD_S]` proves it, repeatably, without touching the cable: it switches the Glass's USB off from the Glass's side (the desktop sees an unplug), once with everything on Wi-Fi and once with Wi-Fi down first so everything is on the cable, times each session's return to Wi-Fi, then switches USB back on and checks nothing moved back.

`scripts/glass status` shows the whole state in one table: how the Glass is reachable, battery, board temperature, CPU clock, display and sound settings, what runs on it, the last stream report and the desktop's supervisors.

## Where the sound goes

On the desktop, `scripts/glass audio` (instant; also in the application launcher and KRunner as "Glass audio: ..."):

| command | what it does |
| --- | --- |
| `glass audio` | where the sound is, whether switching is automatic, whether the Glass is connected |
| `glass audio glass` | the desktop's sound and the voice to the Glass now, and automatic switching on: on the Glass while it is connected, the stereo when it is not |
| `glass audio stereo` | everything to the stereo now, and it stays there (automatic switching off) |
| `glass audio auto off` | automatic switching off, for development: nothing moves by itself, the sound stays where it is (the Glass still plays whatever is sent to "Google Glass") |
| `glass audio auto on` | automatic again (to the Glass at once if it is connected) |
| `glass audio toggle` | automatic switching flipped, with a desktop notice saying which |

The choice is kept across logins (`~/.local/state/glass-xec/audio-auto`).

## Settings

On the Glass (in its shell, or from the desktop as `scripts/glass ssh 'COMMAND'`). Each is kept across boots.

| setting | command | now |
| --- | --- | --- |
| volume | the desktop's volume control for the "Google Glass" output (slider and mute, volume keys while it is the default) sets the Glass's own level, live; on the Glass `glass-audio volume 75` and `glass-audio mute on` | 100% |
| sound output | `glass-audio route earphone 75` (the bone conduction speaker) or `headset` | earphone |
| sound delay behind the desktop | `glass-audio delay 150` at home, `glass-audio delay remote 400` over the tailnet (each applies within 2 s) | 150 ms, 400 ms |
| brightness | `glass-brightness 80` (1 to 223) | 80 |
| a fixed address on a network | `glass-wifi static HomeMixed 192.168.0.80/24 192.168.0.1`; `glass-wifi static HomeMixed off` for DHCP | home: 192.168.0.80 |
| which network first | `glass-wifi priority "O+ Open" 20` (higher first) | hotspot 20, home 10 |
| this network now | `glass-wifi use "O+ Open"` (until `glass-wifi use auto` or a reboot) | by priority |
| a new Wi-Fi network | from the desktop, without typing the passphrase: `scripts/glass-wifi-copy --from USER@HOST "NAME"`; on the Glass: `glass-wifi add "NAME"` | HomeMixed, O+ Open |

On the desktop:

| setting | where |
| --- | --- |
| which monitor | `scripts/glass-view.sh MONITOR 15` once (remembered in `out/glass-output`); now DP-2, the left one |
| frame rate | `scripts/glass-viewd.sh` passes 15; the Glass is held to 300 MHz by its own thermal governors when warm, and 24 a second was not kept there |
| video bit rate and cushion | `BITRATE=3000` (kbit/s) and `CUSHION=1` (frames, up to 6) for `glass-view.sh`; over the tailnet 1500, about 250 ms, and `REMOTE_FPS=12` by default |

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
| `/var/log/glass-wifi.log` on the Glass | Wi-Fi joins and drops, the address taken, the watchdog's actions, kept across reboots |
| `/run/glass/rcS.log` on the Glass | this boot: boot steps, Wi-Fi, the clock |
| `/run/glass/glass-console.log`, `/run/glass/tailscaled.log` | the display shell, Tailscale |
