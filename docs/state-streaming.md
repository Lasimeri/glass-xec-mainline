# Application state to the Glass: the setup

Defined 2026-11-04. **Nothing here is built**, except that `glass-audio.service` is stopped and disabled, by the user's order. Host measurements and Glass readings below are real and marked with where they came from. Every power figure for a mode is arithmetic from other people's constants until gate G0 replaces it. Nothing else has been written to the Glass: no suspend, no CLUT plane, no power save change, no mono.

## Decisions, 2026-11-04


- **Accessibility bridges** the probes switched on stay as they are: no restarts, no toggling. The `a11y` source stays off.
- **No desktop audio.** Done with `systemctl --user disable --now glass-audio.service`. Default sink, streams and volumes were identical before and after. `glass setup-desktop` no longer enables it.
- **No mic.** No capture stream ever ran on the Glass: its capture PCM is closed, no `arecord`. What is left is a tap that mutes the desktop's microphone, through `glass-tap.service`, until `glass-state` replaces that session.
- **A tap wakes it, 60 s lit, then dark.** Taken as tap only: nothing else lights the panel, the host sends nothing while it is dark, and notifications wait for the next tap.

## What the measurements say

Read from the Glass 2026-11-04, plugged in, 97 %, video off, panel off, 30 s sample taken locally on the Glass:

| Reading | Number | Meaning |
| --- | --- | --- |
| wlan0 in, 30 s | 3,228 packets, 2,299,710 B: 107.6/s, 613 kbit/s | `glass-audio.service` sent raw 32 kHz samples of the desktop's main output, always, with no gate for silence (`parec` asked for `glass.monitor`; `pactl list source-outputs` showed it on another sink's monitor). 2,433,735,722 B sent in 9 h 59 min |
| Wi-Fi power save | `iw` says off (`userland/rootfs/usr/sbin/glass-wifi`), but the firmware reads PM 2 (found later, `tools/glass-wl`) | taken here as "the radio cannot sleep", which was wrong |
| `dhd_dpc`, `glass-play`, `dropbear` (audio) | 297, 163, 105 wakes/s; 10.43, 4.43, 4.13 % of one core | removing the video still leaves this |
| `tailscaled`, `glass-tap`, `glass-console`, `glass-wifi` | 16, 5.2, 2.93, 2.23 wakes/s | the rest of the floor |
| `core_pwrdm` | `OFF:0,RET:0` over 10 h | CORE has never left ON this boot |
| `suspend_stats` | success 0 | never suspended. `charger_wake_lock` was held: on the cable it cannot |
| `dss_pwrdm` | OFF with the panel off | blanking does stop the display subsystem |

So **replacing video with state is not the setup. Every session on the radio has to obey the same power rules**, or the Glass sits at the floor the audio stream holds it to.

With the audio stopped, same panel-dark state and method, the same day:

| 30 s on the Glass | With audio | Audio stopped |
| --- | --- | --- |
| wlan0 in | 3,228 packets, 2,299,710 B, 613 kbit/s | 85 packets, 11,759 B, 3.1 kbit/s |
| wlan0 out | 1,797 packets, 139,228 B | 74 packets, 7,734 B |
| CPU busy, both cores | 12.7 % | 0.84 % |
| CPU0 in C4 | 6.63 s | 27.85 s |
| `dhd_dpc` wakes/s | 297.2 | 15.3 |
| `bcmsdh_sdmmc` interrupts | 3,200 | 76 |
| `abe_pwrdm` | ON | OFF |
| `core_pwrdm` | ON, `RET:0` | **still ON, `RET:0`** |

Power could not be read: plugged in, `status=Full`. The 2.83 packets/s left are `tailscaled`, the path guards, `glass-wifi watch` and open ssh sessions, with power save still off.

The panel's scan-out is structural. `panel-notle-dpi.c`: 85,333 kHz, htotal 728, vtotal 380 = 308.46 fields/s, so a static 32 bpp picture is 284.3 MB/s of DRAM reads, for ever (arithmetic). LiKamWa 2014 measured 674 mW static plus 196 mW x brightness/255, whatever it shows. That did not hold on this stack: see the measurements below.

## Modes and the numbers

**Measured** 2026-11-04, on battery, brightness 223, `power_save` off by `iw` (the firmware's PM 2 was not known yet), no audio, `glass-tap` and one shell session open: `voltage_now x current_now` at 1 Hz, logged on the Glass with the ssh session closed. One pass each, not interleaved: first readings, not gate G0. Video and lit came from a window with a user swiping sources, cut by the swipe log; dark was a clean 120 s with no touch.

| Condition | mW | sd | n | On 2.1 Wh |
| --- | --- | --- | --- | --- |
| Video: monitor stream, then follow | 1,105.7 | 141 to 197 | 39 | 1.90 h |
| Console lit, static, no stream | 611.2 (618.9, then 604.2) | 61 to 65 | 61 | 3.44 h |
| Panel off, no stream, settled, 12 min after boot | 301.6 (322.7, 312.2, 287.3, 284.3 by 30 s) | 97.5 | 120 | 6.96 h |

**The data contradicts LiKamWa's constants on this stack, and the data wins.** They predict lit 1,179.4 and dark 334 mW. Here dark is close (301.6) but lit is about half, and the panel adds 309.6 mW, not 845.4. Not explained; a fuel gauge is not a Monsoon, and their figure may include work this stack does not do. An earlier 474.5 mW for dark, from the first minute after boot, is withdrawn: boot settling. Cross-check: the charge fell 4 points in about 6 min, and the sum of readings gives 3.5 % of 7,560 J.

So **a static lit console costs 44.7 % less than video, 1.81x, with no suspend at all.** The earlier claim that a lit panel cannot be helped is withdrawn.

**Model, for the use chosen:** resume 2,656 mJ (LiKamWa: 1.9 s at 1,398 mW, on Android, unverified here), 60 s lit at 611.2 mW, 6.5 s dark at 301.6 mW, asleep at 22.3 mW (LiKamWa, unverified here) for the rest of the hour. **One tap is 41,288 mJ, 0.55 % of a charge, about 183 taps.**

| Taps an hour | Mean mW | Hours | Against video |
| --- | --- | --- | --- |
| 1 | 33.3 | 63.0 | 33.2x |
| 6 | 88.6 | 23.7 | 12.5x |
| 12 | 154.8 | 13.6 | 7.14x |
| 30 | 353.7 | 5.94 | 3.13x |

Every row of that table depends on suspend working, which has never run on this stack. Without it the floor is 301.6 mW, 6.96 h.

Radio, from the BCM4330 datasheet (table 44, 3.6 V): power save 1.25 mA = 4.5 mW, listen 52 mA = 187.2 mW, and a 250 ms tail after traffic (Qian 2012, Nexus One): P = 187.2 x 0.25 x r + 4.5 x (1 - 0.25 r) for r wakes a second, so 0.1/s 9.1 mW, 1/s 50.2 mW, 4/s 187.2 mW. Chip only. All the readings above have `power_save` off, so they may fall with it on: not tried.

Suspend pays when dark lasts longer than (1,398 - 301.6) x 1.9 / (301.6 - 22.3) = 7.46 s on the measured dark. DARK to SLEEP stays 6.5 s until G2 measures a real resume, which replaces the number.

## The design

```
╔ desktop, this host ════════════════════════╗        ╔ Glass, kernel 3.4.94 ════════════════════╗
║ ┌ glass-stated ──────────────────────────┐ ║ ssh    ║ ┌ glass-state, one C binary ───────────┐ ║
║ │ card table, numbered states            │ ║ :2200  ║ │ apply diffs, draw glyph cells        │ ║
║ │ row diff from the last ACKED state     ├─╫─diffs──╫▶│ one plane in DRAM, no page flips     │ ║
║ │ newest only, one frame unacked         │◀╫──acks──╫─┤ CLUT1 8.9 MB/s if gate G3 passes     │ ║
║ │ lit: 1 frame/s, dark: nothing sent     │ ║        ║ │ fallback 32 bpp, 284.3 MB/s          │ ║
║ └───────────────────┬────────────────────┘ ║        ║ └─────────┬─────────────────────────┬──┘ ║
║                     ▲ rows, unix socket    ║        ║           ▼ lit 60 s     dark 6.5 s ▼    ║
║ ┌ sources ──────────┴────────────────────┐ ║        ║ ┌ panel ──┴──┐ ┌ /sys/power/state ──┴──┐ ║
║ │ term   Konsole D-Bus, while shown      │ ║        ║ │ 640x360    │ │ mem: real suspend     │ ║
║ │ claude ~/.claude transcript .jsonl     │ ║        ║ │ 308.46     │ │ model 22.3 mW, unrun  │ ║
║ │ a11y   AT-SPI, off, the user decided   │ ║        ║ │ fields/s   │ │ wake: a tap           │ ║
║ │ notify session bus Notifications       │ ║        ║ │ lit 611 mW │ │ nothing else          │ ║
║ │ track  shared memory, read only        │ ║        ║ └────────────┘ └───────────────────────┘ ║
║ │ any    glass card NAME                 │ ║        ║                                          ║
║ └────────────────────────────────────────┘ ║        ║ measured on battery, no audio, 1 pass:   ║
║ measured changed text: nvtop 16.0, other   ║        ║ stream 1,106 mW, lit console 611 mW,     ║
║ 4.7, claude 0.3 kbit/s; video 1,500+       ║        ║ dark 302 mW; CORE ON; one tap 0.55 %     ║
╚════════════════════════════════════════════╝        ╚══════════════════════════════════════════╝
```

One ssh session, one writer, frames small enough for one TCP segment. Other sources of pictures stay what they are: `tracker`, monitors and the camera remain `glass display` sources, since state cannot show what a camera sees. `state` is one more source in that list.

### Host: sources

Everything publishes rows to `$XDG_RUNTIME_DIR/glass-state.sock`. Any program can: `make 2>&1 | tail -n 9 | glass card build`.

| Source | Reads | Proof on this host 2026-11-04 | Default |
| --- | --- | --- | --- |
| `term` | Konsole D-Bus `org.kde.konsole.Session.getAllDisplayedText` on `/Sessions/N`, polled only while its card is showing | every session, 294 to 6,821 B, about 7 ms by `qdbus6`. Touches nothing else | on |
| `claude` | `~/.claude/projects/*/*.jsonl`, as `scripts/glass-mirror.sh` does | script exists since 10-07; Claude Code is not under tmux, the transcript reflows and the screen does not | on |
| `notify` | session bus, `org.freedesktop.Notifications`, by `dbus-monitor`; shown at the next tap | service present, not run | on |
| `media` | MPRIS `PropertiesChanged` | a player answers | on |
| `track` | the tracker's shared memory, read only, nothing there changed | header read, not run | on request |
| `a11y` | AT-SPI: focused window, `object:text-changed` | see below | **off, opt-in** |

Changed text per second on real sessions, 12 samples at 1 s, sizes only, `diff` of `getAllDisplayedText`: nvtop 2,000 B/s (16.0 kbit/s, 12.3 rows/s), another monitor 589 B/s (4.7), a busy session 41 B/s (0.3), idle sessions 0. Against 1,500 to 3,000 kbit/s of video. Before framing.

**AT-SPI works, and it is why it is off.** `kwrite` under `QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1`: text read in 0.18 ms, an insert returned as `object:text-changed:insert` 17.0 ms later with the exact string, 237 nodes of `kcalc` dumped in 114.6 ms. But **registering a listener switched on bridges across the running desktop with `org.a11y.Status IsEnabled` still false**: registered apps went from 4 to 23, 29 bus connections, the browser and the secrets service among them; the journal has no `AtSpiAdaptor` line this boot before the first probe. They stay on with no listener: 89 messages in 5 s on the a11y bus. A global text listener then receives Konsole's screens (10.6 events/s, 8,775 B/s) and browser text. That is the same user's data on the same user's bus, but it should be a choice. Trigger read from the timeline, mechanism not read from source. Electron also needs `--force-renderer-accessibility` (xa11y).

### Wire

Little-endian. Header `u8 type, u8 flags, u16 len`. Payload at most 1,280 B, so a frame fits one TCP segment after about 90 B of ssh and TCP/IP. Cells are one byte, a PSF glyph index; the host lays out and maps UTF-8. Top bit of a run's length is reverse video.

| Type | Direction | Payload | Rule |
| --- | --- | --- | --- |
| `0x01 SNAP` | to Glass | `u32 state, u8 card, cols, rows, cells`; `flags & 1` = more | on connect and on a WANT with no common base |
| `0x02 DIFF` | to Glass | `u32 base, u32 state, u8 card`, then runs `u8 row, col, len, cells` | from the last ACKED state, drawn whole on arrival |
| `0x03 ATTN` | to Glass | `u8 class, card, seconds` | reserved, unused: only a tap lights the panel |
| `0x04 SPRITE` | to Glass | `u32 state, u8 card, u16 x, y, w, h, u8 bpp`, RLE | only when asked for |
| `0x81 ACK` | to host | `u32 state, u8 panel, card, battery, flags` | carries the panel's state, so the host knows it is dark |
| `0x82 WANT` | to host | `u8 card` | on every wake or card change: **the Glass pulls** |
| `0x83 INPUT` | to host | `u8 gesture` | tap, dtap, tap2, swipe+, swipe- |

Precedents: ghostty-web's semantic grid (keyframe, row patches, slow clients skip states), Neovim's `grid_line` and `flush`, Mosh SSP (diffs between numbered states, newest wins), RFB's client-demanded updates, Even G1's text and 1-bit bitmap commands, Brilliant Frame's text and palette sprites.

### When anything is sent

| State | Host may send | Why |
| --- | --- | --- |
| SLEEP, DARK | nothing at all | any unicast passes the filter and resumes the Glass, 2,656 mJ by the model. State waits for the WANT that a tap sends |
| GLANCE | newest DIFF at most 1/s, only if changed, one unacked | 50.2 mW of radio |
| LIVE, by card | at most 4/s | at 4/s the radio never sleeps, 187.2 mW: Mosh's 20 to 250 ms cadence is this row |

Telemetry never resets the timer. Only a tap does. Without this a tracker's counters would hold the panel on for ever.

### Glass: `glass-state`

One C binary from `tools/glass-console` and `tools/glass-tap`, which already have the PSF font, `poll()`, one-cell redraw, and the touchpad parser. It takes the panel from `glass-console`, and the gestures from the `glass-tap` session while this source is on. It has no blink and no status timer: **no change, no wake-up**.

Techniques, each adopted only if G3 measures it lower or faster than the plain form, otherwise the plain form stays:

- **One plane, in place, no flips.** Draw into DRAM and let scan-out pick it up. A partly drawn cell shows for one field, 3.24 ms. No DISPC write per update, so no SIGBUS when blanked: registers are touched only just after the panel comes on.
- **CLUT1.** `DISPC_GFX_ATTRIBUTES` format 0 (`dispc.c:789`), palette at `GFX_TABLE_BA` hand-placed inside fb0's own memory, loadmode from `dispc_set_loadmode` (`dispc.c:2877`). 8.9 MB/s of scan-out against 284.3, 28,800 B a frame (arithmetic). Fallback 32 bpp, as `glass-console` draws today.
- **Glyph blit.** At 1-bit, 8-pixel glyphs are byte-aligned: 16 strided byte stores per cell, a 256-entry table of doubled bytes for scale 2. No shifts, no branches.
- **Colour.** `colormix` to green only: green is 50.5 % of LED power and 71.5 % of luminance (Rec. 709), so equal luminance costs 70.6 % of the LED share, about 50.4 mW at 223 (arithmetic, assumes colormix is a power share). `mono` shows the same image in every field (`panel-notle-dpi.c:1494`), which should end colour breakup on head movement; power unknown.

Grid: 40 x 11 at scale 2, as `glass-console`; 80 x 22 at scale 1 if readable on the prism.

### Glass: power

```
SLEEP --tap--> GLANCE --60 s, no tap--> DARK --6.5 s--> SLEEP
               GLANCE <--tap----------- DARK
```

- Gestures: a tap wakes, or restarts the 60 s if lit, and toggles nothing else. A two-finger tap goes dark at once. A swipe is the next or previous card, and past the last card the next `glass display` source.
- Panel: `panel-notle-dpi/enabled`. SLEEP: `echo mem > /sys/power/state`, which on this kernel means early suspend, then real suspend once no wake lock is held (Android semantics), and `echo on` to leave. Early suspend turns the display subsystem off itself (`dss/core.c:273`). `wait_for_fb_sleep` and `wait_for_fb_wake` are the handshake. `glass-state` holds a wake lock while it is handling a wake.
- Wake: the touchpad, `board-notle.c:2103`, alone. Wake-capable in source and unused: camera button `:1255`, Wi-Fi out-of-band `bcmsdh_linux.c:631`, gauge `:1850`, head movement `:2138`, wink and don/doff through the sensor hub, version 1.22 answering (`glasshub.c:2363`). Taking the Glass off is an obvious reason to sleep; second stage.
- Wi-Fi in suspend, already in the driver: `dhd_set_suspend` sets `PM_MAX`, a unicast-only filter, every third DTIM (about 0.54 mA, 1.93 mW if DTIM is 1, arithmetic), roaming off. Firmware answers ARP (`ARP_OL_AGENT | ARP_OL_PEER_AUTO_REPLY`, `dhd_linux.c:418`) and sends a keep-alive every 28 s. On resume it sets `PM_FAST`, undoing `power_save off`.

### What every other session must do while `state` is the source

| Session | Today | In this mode | Needs |
| --- | --- | --- | --- |
| `glass-audio` | was 613 kbit/s always, `abe_pwrdm` ON | **gone, disabled 2026-11-04** | done |
| `glass-pathguard` | a ping per 3 s per session (`:45`), 1.00 to 1.33/s | none on this session. Any unicast passes the filter and resumes the Glass | ARP once a minute instead: firmware answers without a resume, if G2 confirms. `arping` needs `cap_net_raw` on the host |
| dropbear `-K 5` | a keepalive per 5 s per session | second listener `-p 2200` with no `-K`, forced command, its own key | `inittab` |
| host `ssh` | | `ServerAliveInterval=0`, `TCPKeepAlive=no` | a dead session is found when a send stalls for 10 s |
| `tailscaled` | 16 wakes/s | stopped on the home network, started off it | `glass-wifi`. **Off the home network there is no suspend**: the tailnet needs its keepalives |
| `glass-wifi watch` | 10 s | 60 s; frozen in suspend | `glass-wifi` |
| `glass-console`, `glass-tap` | 2.93, 5.2 wakes/s | stopped; `glass-state` does both, and a tap stops muting the desktop's mic | `glass display` |

**Bluetooth is not the transport.** The chip's 500 ms sniff is 0.24 mA against Wi-Fi power save's 1.25 mA: at most 3.6 mW saved, against a 334 mW floor. It would give up ssh, the tailnet and the existing paths; LiKamWa's active numbers are no better. Parked with numbers.

**Precedent and its limit.** Even G1: 640x200, green micro-LED, BLE, text and 1-bit bitmaps, shows on head-up, 0.616 Wh, vendor's "up to 1.5 days", 17 mW mean (arithmetic). The same shape, but "the display illuminates only the pixels showing information". This LCOS lights every pixel. Glass will not match it lit, and the setup depends on being dark.

## Files

| File | What |
| --- | --- |
| `tools/glass-power/glass-power.c` | G0: `voltage_now`, `current_now` each second to a file, nothing else. Neither is read anywhere in the repo today; both exist on the Glass; there is no `current_avg` |
| `tools/glass-state/proto.h` | the wire, shared |
| `tools/glass-state/state.c` | on the Glass, built there |
| `tools/glass-state/stated.c` | on the host |
| `scripts/glass` | `glass display state`, `glass card NAME`, `glass state status`, `glass power` |
| `scripts/glass-state-*` | sources; `glass-state.service` from `setup-desktop` |
| `userland/rootfs/etc/inittab` | the second dropbear |
| `userland/rootfs/usr/sbin/glass-wifi` | power save and watch by mode, `tailscaled` by network |

## Gates

In order, one at a time. Each is measured on the Glass, unplugged, above 30 %, same brightness read and not assumed, same network, 120 s a condition, interleaved three times, spread reported. Reversible: nothing is flashed, `rcS` is unchanged until G6, and a reboot restores everything.

| Gate | Does | Passes when |
| --- | --- | --- |
| G0 | logger. Video; panel off, settled; console lit. Audio is already gone. Hands off the touchpad | repeats the first reading interleaved with spread; a clean dark figure; `core_pwrdm` over the dark window |
| G1 | stop `tailscaled`, console, `glass-tap`, guards, one at a time; power save on | wlan0 at most 1 packet/s (2.83/s today), DARK at most 400 mW, `core_pwrdm RET` above 0 or the blocker named |
| G2 | `echo mem`, from a shell with `(sleep 120; echo on > /sys/power/state) &`, ssh session open | `suspend_stats` success above 0; wakes by a tap; session alive after 10 min, first bytes through; ARP answered with no resume; `dmesg` gives the DTIM; asleep at most 60 mW; tap to lit at most 2.5 s |
| G3 | lit matrix: 32 bpp, CLUT8, CLUT1 x `mono` x `colormix` at equal brightness | adopt only what is at least 5 % lower with spreads apart, no SYNCLOST, and readable to the user. Otherwise 32 bpp |
| G4 | `glass-state` and `glass-stated`, the `term` and `claude` cards | lit at most the static floor + 60 mW; wake to current state at most 300 ms awake; idle CPU about 0 % |
| G5 | `notify`, `media`, `track` | current at the next tap; none costs a wake while dark |
| G6 | a real day | at 12 taps an hour, at least 10.2 h (model 13.6 h, less the 24.9 % by which an earlier model missed) |

## Open

- CORE never left ON: not with audio, not in 30 s with it stopped on the cable, not over 120 s on battery lit, **and not over 120 s on battery dark**, CPU0 in C4 for 112.3 of them. So idle alone will not reach CORE retention on this stack; only `echo mem` might. Lead: `usb_otg_hs_ick=2`, `ocp2scp_usb_phy_phy_48m=1`, `usb_phy_cm_clk32k=1` stay held with the cable out and `android0` DISCONNECTED, and `l3init_pwrdm` never leaves ON.
- **The link on battery, in 6 min:** an AP deauth at 70 s (`WLC_E_DEAUTH_IND`, back at 82.9 s, the AP now on another channel), an unexplained stall, and `dhd_check_hang ... e=-110` at 375 s, 14 s after the user swiped to the camera, the same signature as the camera-start hang of 2026-10-11, still unfixed; `glass-wifi watch` recovered it. Signal -63 to -69 dBm against -45 on the cable.
- The AP's DTIM: not in the scan data. G2 prints it.
- Wake is 1.9 s on Android. On this stack, unknown and probably shorter: no Android framework resumes.
- A DHCP lease or group rekey while asleep: G6.
- The panel can be read from the front of the Glass (LiKamWa), so notifications are visible to whoever faces the wearer.
- Scan-out at CLUT1 lowers DRAM traffic by arithmetic. Whether it lowers **power** is what G3 is for: there is no measured figure for this SoC. The nearest is simulation, up to 30 % of memory power (Intelligent Frame Refresh, DAC 2014).

## Sources

- LiKamWa et al., Draining our Glass, APSys 2014: https://roblkw.com/likamwa2014glass.pdf
- BCM4330 data sheet 4330-DS206-R, tables 44 and 47: https://linux-sunxi.org/images/0/05/4330-DS206-R.pdf
- Qian et al., Periodic Transfers in Mobile Applications, WWW 2012: https://feng-qian.github.io/paper/periodic_www12.pdf
- Intelligent Frame Refresh for Energy-Aware Display: https://dl.acm.org/doi/pdf/10.1145/2627369.2627612
- Android suspend, Wysocki: https://static.lwn.net/images/pdf/suspend_blockers.pdf
- AT-SPI2: https://www.freedesktop.org/wiki/Accessibility/AT-SPI2/ ; events: https://accessibility.linuxfoundation.org/a11yspecs/atspi/adoc/atspi-events.html ; quirks: https://xa11y.dev/explanation/accessibility-quirks/ ; KDE: https://develop.kde.org/docs/apps/tests/appium/
- Neovim UI protocol: https://neovim.io/doc/user/api-ui-events/ ; ghostty-web: https://www.npmjs.com/package/@slopus/ghostty-web ; Mosh: https://mosh.org/mosh-paper.pdf
- Even G1: https://www.evenrealities.com/G1 ; protocol: https://github.com/even-realities/EvenDemoApp/blob/main/README.md ; Brilliant Frame: https://docs.brilliant.xyz/frame/frame-sdk-lua/
- Kernel: Google's 3.4 tree, `drivers/video/omap2/displays/panel-notle-dpi.c`, `drivers/video/omap2/dss/dispc.c`, `arch/arm/mach-omap2/board-notle.c`, `drivers/net/wireless/bcmdhd/dhd_linux.c`
