# glass-xec-mainline

Linux for **Google Glass Explorer Edition XE-C** (the 2 GB revision, TI OMAP4430): a small Alpine Linux userland and two kernels, built with no Python anywhere. The Glass becomes a head-mounted screen, speaker and camera for the desktop: the desktop's monitor on its display, the desktop's sound on its bone conduction speaker, its camera in a window on the desktop, a shell over ssh.

> **Status (2026-10-07): hardware tested, in daily use on one XE-C** (XE21 XRW66, 2 GB). Linux is in the boot partition and starts by itself at power-on in about 45 seconds, with Google's own XE24 kernel (3.4.94) and this Alpine userland on the eMMC; stock Android's boot is kept in the backup and recovery is untouched. Everything in [Tested on the hardware](#tested-on-the-hardware) runs on it today. The mainline kernel (7.2.9) is built and checked but does not come up on the hardware yet ([below](#not-working-yet)). Day to day: [docs/daily-use.md](docs/daily-use.md). The first hardware session, step by step, and what happened in it: [docs/hardware-test.md](docs/hardware-test.md). What the hardware taught: [docs/build-log.md](docs/build-log.md). Hardware facts come from Google's own Glass kernel sources (3.4.83, codename "notle"), cross-checked against independent teardowns, and since 2026-10-07 against the unit itself.

## Tested on the hardware
| part | what works on the Glass | where |
| --- | --- | --- |
| boot | Linux from the boot partition at power-on, ssh in about 45 s; Wi-Fi, clock, tailnet, display shell, stream, sound and tap all come back by themselves after a reboot (`glass reboot-test`) | `boot-stock-kernel.img`, `glass flash-boot stock` |
| fastboot | unlock (one second), `fastboot boot` of a sent image from RAM, `fastboot flash:raw` for the boot image (current platform-tools fail Glass's empty partition-size answer) | `scripts/glass` |
| USB | RNDIS up in 15 s, ssh at `172.16.42.1`, serial console; a fallback only, every session moves to Wi-Fi when the cable comes out (`glass unplug-test`) | initramfs, `glass-pathguard.sh` |
| Wi-Fi | the BCM4330 with Google's firmware (5.90.195.122): stored networks joined at boot by priority, a fixed address at home, power save off, a watchdog for drops and driver hangs | `glass-wifi` |
| Tailscale | the Glass on the tailnet as `glass`, reachable from any network | `tailscaled` |
| display | the 640x360 Himax HX7309 LCoS (312 color fields/s): an idle shell with status lines, and the desktop's monitor at 12 frames/s (NVENC H.264 over ssh, decoded on the Glass, flipped by the display controller: 0 dropped, 0 late) | `glass-console`, `glass-view.sh`, `glass-fb` |
| sound | the desktop's sound and the voice on the bone conduction speaker (the TWL6040's Earphone output), no restarts, no underruns; Opus over the tailnet | `glass-play`, `glass audio` |
| camera | the OV5680 through the Ducati (TI's DOMX over rpmsg, spoken in C), H.264 from the Ducati's own encoder, in a window on the desktop or on the Glass's own screen | `glass camera`, `glass local-camera on`, [docs/ducati-omx.md](docs/ducati-omx.md) |
| touchpad | a tap turns the camera window and the display on or off together (off while it charges), a two-finger tap toggles the desktop's voice mute | `glass-tap`, `glass camera-display` |
| battery | the bq27520 fuel gauge: the charge on every streamed frame, on the status lines and in `glass btop`; charging while plugged in | `glass-fb`, `glass btop` |
| the Glass's screen on the desktop | the rows that changed, every 250 ms, in a window | `glass-fbgrab`, `glass-fbview` |
| writing partitions | the rootfs (374 MB) and the boot image written from Linux over ssh with a read-back check, 374 MB in 23 s | `glass flash-rootfs`, `glass flash-boot` |

Heat sets the pace: when warm, the thermal cap holds the CPU at 300 MHz whatever the governor asks, which is why the monitor stream runs at 12 frames/s and the camera at 15.

## Approach
- **Kernels:**
  - **Google's XE24 kernel (3.4.94)**, taken unchanged from the factory image, with this userland as its initramfs: the kernel the Glass runs today.
  - **Mainline:** kernel.org stable 7.2.9, built with LLVM (`LLVM=1`, clang and lld) inside a bubblewrap sandbox where every Python binary is masked; `omap2plus_defconfig` narrowed to OMAP4, with [config/glass.config](config/glass.config) on top; built for size (`-Os`, xz) and stripped of what Glass has no hardware or use for (NFS, MTD, PCI, ATA, SCSI, IPv6, ftrace, and so on); everything the full image drives is built in, so no modules ship.
- **Device tree (mainline):** two DTBs from one base, [dts/omap4-glass-xec-common.dtsi](dts/omap4-glass-xec-common.dtsi). Every pad, GPIO and address names its line in Google's `board-notle*.c`; GPIOs are those of EVT2 and later boards.
  - `safe`: memory, the TWL6030 PMIC, the USB gadget, ramoops and a `simple-framebuffer` on the display the bootloader left running. Every MMC controller is disabled, so it cannot touch the eMMC.
  - `full`: adds the eMMC, Wi-Fi and Bluetooth (BCM4330, from the mainline Galaxy Tab 2 pattern), the bq27520 gauge, the Synaptics touchpad, the MPU-9150 and the camera button.
- **Boot path:** Glass's Android bootloader (u-boot based) passes ATAGs only.
  - The DTB is appended to the zImage (`ARM_APPENDED_DTB`) and the bootloader's memory size (2 GB) and command line are folded into it (`ARM_ATAG_DTB_COMPAT`). The decompressor grows the DTB by 50% for that.
  - The initramfs is built into the mainline kernel: the 15 MB decompressed kernel would overrun the bootloader's ramdisk slot 16 MB above the load address. The boot.img carries an empty ramdisk.
- **Size bound:** every boot image must fit in 8,388,608 bytes, the size of boot and recovery on the unit (16384 sectors each, from the backup's `partitions.txt`). `scripts/build-images.sh` refuses anything larger.
- **boot.img:** written by a small C tool (`tools/bootimg`), not AOSP's `mkbootimg.py`. It round-trips Google's own images byte for byte.
- **Userland:** Alpine Linux 3.24 armv7 (musl, busybox).
  - Packages are installed by apk-tools on the host with `--no-scripts`: nothing armv7 runs on the build host, and what the package scripts would do is done in [scripts/build-userland.sh](scripts/build-userland.sh).
  - Static checks in [scripts/check-userland.sh](scripts/check-userland.sh): every ELF is ARM EABI5 hard-float, and every interpreter and library resolves inside its root.
  - The Glass's own tools (display, sound, camera, touchpad, screen capture) are C, compiled on the Glass by `glass update` where their source changed.
- **No Python, proven mechanically:**
  - `tools/nopython/` shims are first on `PATH` and log and fail any `python*` call;
  - the real interpreters are masked inside the sandbox;
  - `out/python-calls.log` must stay empty.

## The images (`out/`, after `glass build`)
| image | what | on the hardware |
| --- | --- | --- |
| `boot-stock-kernel.img` | Google's XE24 kernel (3.4.94) + this initramfs as its ramdisk; switches to the rootfs on userdata | **works**: in the boot partition of the tested unit (`glass flash-boot stock`); `glass test-boot stock` runs it from RAM |
| `boot-stock-kernel-stay.img` | the same, staying in RAM | the rescue image: mounts the rootfs to repair it |
| `rootfs.img` | the Alpine rootfs, ext4 | **works**: in userdata (`glass flash-rootfs`); grows to fill the partition at its first boot |
| `boot-xe24-adbroot.img` | Google's XE24 boot.img with `ro.secure=0`, `ro.debuggable=1` | `glass root-boot`: the sent kernel runs, but Google's ramdisk rebooted itself at 23 s; root came over ssh from the stock-kernel image instead |
| `boot-safe.img` | mainline + safe DTB + initramfs | **does not come up yet**: `glass test-boot safe` (RAM only) |
| `boot-full.img` | mainline + full DTB + initramfs; switches to the rootfs on userdata | not booted yet |

On the Glass:
- **Shells:** root over ssh (Wi-Fi, the tailnet or USB) and on the USB serial gadget (`/dev/ttyACM0` on the desktop).
- **SSH:** dropbear, keys only: `~/.ssh/glass_ed25519.pub` (else `~/.ssh/id_ed25519.pub`) is baked in at build time, or `GLASS_SSH_PUBKEY`. `scripts/glass ssh` finds the Glass by itself: `GLASS_IP`, the name `glass`, the last Wi-Fi address, or USB.
- **Over USB:** the Glass is `172.16.42.1`, the desktop gets `172.16.42.2` by DHCP.
- **Commands:** `glass-wifi`, `glass-audio`, `glass-brightness`, `glass-term` (ssh into the desktop's tmux session), `glass-pan` (Bluetooth PAN to the desktop, not tested), `glass-collect` (a tar of what the next device-tree iteration needs), `btop`.

## Quick start
A new Glass:
```sh
scripts/glass fetch    # download and verify everything (once; afterwards offline)
scripts/glass build    # userland, kernels, images
scripts/glass check    # static checks and SHA256SUMS
```
Then [docs/hardware-test.md](docs/hardware-test.md), step by step: unlock, backup, the stock-kernel image from RAM, then the rootfs and the boot partition.

A Glass already running this:
```sh
scripts/glass setup-desktop   # the desktop's side: picture, sound, tap and camera services
scripts/glass status          # the whole state as a table
scripts/glass update          # after every change here; nothing flashed
```
`scripts/glass help` lists every verb; [docs/daily-use.md](docs/daily-use.md) has the settings and where to look when something is off.

## Safety, enforced in `scripts/glass`
- **Allowlist:** only `boot`, `recovery`, `system`, `cache` and `userdata` are ever written. `xloader`, `bootloader` and `fpga` are refused in code.
- **boot:** written only by `glass flash-boot`, `glass restore` and `glass restore-partition`; `flash-boot` refuses without a backup holding boot.img and its size. Recovery is never touched by the permanent install, so the button combination still reaches it.
- **Tests from RAM first:** `fastboot boot` sends an image without writing anything; the tested unit ran the stock-kernel image that way before anything was flashed.
- **Partition images from Linux:** the bootloader refuses sparse images and stalled on a raw 392 MB one, so the rootfs and the boot image are written over ssh with a read-back check; fastboot only for small images and `erase userdata`.
- **flash-recovery:** refuses without a backup and without a known partition size, and refuses an image larger than the partition.
- **Confirmation:** every write says what it will do and needs a typed `yes` (or `--yes`).
- **oem unlock:** `fastboot oem unlock` (run twice) wipes userdata.
- **Recovery:** Google's XE24 factory image (`glass restore`) and your own backup (`glass restore-partition`) bring it back.

## Not working yet
- **The mainline kernel on the hardware:** the safe image takes over from the bootloader, brings no USB up and resets after about a minute, leaving no crash record. Unsolved; the next instruments are a serial cable on the debug UART or the display. The `full` image has not been booted.
- **Mainline device tree, for when it boots:**
  - Display: the notle panel (an LCoS behind an iCE40 FPGA, `panel-notle-panel` at I2C4 0x49) has no mainline driver. The DTS uses the bootloader's running display as a `simple-framebuffer` (`/init` points the DISPC at it) and never names `FPGA_CRESET_B`, `DISP_ENB` or `LCD_RST_N`. On the stock kernel the panel stays dark until `panel-notle-dpi` is disabled and enabled again; mainline has no such switch.
  - Not described yet: audio (TWL6040 and the ABE), the glasshub MCU, the LTR-506 light sensor (no mainline driver), the camera, GPS.
  - Bluetooth: the UART stays at hci_bcm's default speed (the operating baud rate of Google's vendor library was not determined, so `max-speed` is left unset); no host-wake interrupt, so no low-power mode.
  - Wi-Fi: uses the in-band SDIO interrupt, not the host-wake line.
  - The board revision's GPIO set is unproven until mainline runs.
- **Not tested on the stock kernel:** Bluetooth (off at boot to spare the two cores), Bluetooth PAN, GPS, the light sensor, the IMU.

## Provenance (all checked by `scripts/fetch.sh`)
| input | check |
| --- | --- |
| linux-7.2.9 | sha256 from kernel.org's `sha256sums.asc`, signed by the Kernel.org checksum autosigner (`B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1`, fetched by WKD) |
| Google's Glass kernel 3.4.83 (GlassHack/factory-kernel@1091b53), pmaports `config-google-glass.armhf` | sha512 from pmaports' APKBUILD |
| Google's XE24 factory zip, XE22 rooted boot.img | SHA-1 as developers.google.com/glass/tools-downloads/system lists them |
| AOSP 5.1.1 for glass_1 (reference only, never flashed by any verb) | MD5 as the XDA post lists it, archive.org mirror |
| Alpine 3.24.2 armv7 minirootfs | sha256 and Natanael Copa's signature, key fingerprint `0482 D840 22F5 2DF1 C4E7 CD43 293A CD09 07D9 495A` (as alpinelinux.org/downloads lists it) |
| Alpine packages | signatures checked by apk against the minirootfs' keys |
| apk-tools 3.0.8 (host) | sha256 pinned, and the CachyOS signature (`882DCFE48E2051D48E2562ABF3B607488DB35A47`) where pacman's keyring is present |
| `brcmfmac4330-sdio.bin` (linux-firmware, 5.90.195.114) | sha256 pinned; the same bytes as Arch's linux-firmware-broadcom 20260916 |
| BCM4330 calibration, Bluetooth patch, Google's Wi-Fi firmware (5.90.195.122) | extracted from the XE24 system image; sha256 pinned in `scripts/firmware.sha256`; never committed |

## Layout
| path | what |
| --- | --- |
| `scripts/glass` | the entry point: build, test, back up, flash, restore, update, status, ssh |
| `scripts/fetch.sh` | downloads and verifies everything; unpacks into `src/`, `tools/apk-host/`, `firmware/` |
| `scripts/build-kernel.sh` | the LLVM kernel build in the Python-free sandbox; `GLASS_INITRAMFS` builds the initramfs in; checks every config option |
| `scripts/build-userland.sh` | `build/initramfs` and `build/rootfs` from the minirootfs and apk |
| `scripts/build-images.sh` | the images, the size bound, `out/SHA256SUMS` |
| `scripts/check-userland.sh` | static checks of both roots |
| `scripts/on-glass/` | `update.sh` and `status.sh`, run on the Glass by `glass update` and `glass status` |
| `scripts/glass-view.sh`, `glass-viewd.sh` | the desktop's monitor to the Glass, and its supervisor |
| `scripts/glass-audio.sh`, `glass-tap.sh`, `glass-camera.sh` | the desktop's sound to the Glass, the touchpad's gestures (camera and display, voice mute), the camera window |
| `scripts/glass-pathguard.sh`, `glass-reboot-test.sh`, `glass-unplug-test.sh` | sessions follow the path (USB or Wi-Fi); the repeatable reboot and unplug checks |
| `scripts/glass-wifi-copy` | a Wi-Fi network copied from another machine without typing its passphrase |
| `config/glass.config` | the mainline kernel config fragment |
| `dts/` | the Glass device trees (copied into the kernel tree at build) |
| `userland/initramfs/`, `userland/rootfs/` | `/init`, inittab, rcS, `glass-*` commands |
| `tools/glass-fb/`, `glass-console/`, `glass-play/`, `glass-camera/`, `glass-tap/`, `glass-fbgrab/` | the Glass's own tools in C: frames onto the display, the idle shell, sound, the camera through the Ducati, the touchpad, the screen read back |
| `tools/glass-fbview/`, `glass-viewport/` | desktop tools in C: the Glass's screen in a window, a pixel-perfect region of a monitor |
| `tools/bootimg/` | Android boot image v0 in C |
| `tools/nopython/` | the `python*` shims that log and fail |
| `docs/daily-use.md` | what starts by itself, how to reach the Glass, the camera, the command for each setting, keeping it current |
| `docs/ducati-omx.md` | the Ducati's OMX over rpmsg as glass-camera speaks it: ioctls, packets, buffers, the camera's ports |
| `docs/hardware-test.md` | the first hardware session, step by step, and what happened on 2026-10-07 |
| `docs/plan.md`, `docs/build-log.md` | the plan, the build log with the hardware table and what the hardware taught |
| `dl/`, `src/`, `build/`, `out/`, `firmware/`, `backup/` | fetched, built or backed up: never committed |

## Credits
- Google's Glass kernel sources (GPL-2.0), mirrored by GlassHack.
- The postmarketOS `google-glass` port (downstream kernel, 2017-2019): the boot layout and the display bring-up.
- The mainline Galaxy Tab 2 (`omap4-samsung-espresso`) and Kindle Fire (`omap4-kc1`) device trees: the BCM4330 and eMMC patterns.

## License
Scripts and tools: MIT. Device tree and kernel configuration: GPL-2.0-only OR MIT.
