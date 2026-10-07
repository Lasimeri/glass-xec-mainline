# glass-xec-mainline

The latest mainline Linux kernel and a small Alpine Linux userland for **Google Glass Explorer Edition XE-C** (the 2 GB revision, TI OMAP4430), built with no Python anywhere. The goal is Glass as a head-mounted Linux terminal into the desktop's Claude Code session.

> **Status: not yet tested on hardware.** The glasses are in the mail. Everything here is built and checked offline. Hardware facts come from Google's own Glass kernel sources (3.4.83, codename "notle") and are cross-checked against independent teardowns ([docs/build-log.md](docs/build-log.md)). Nothing is claimed to work on a Glass until it has. The first session on the hardware follows [docs/hardware-test.md](docs/hardware-test.md).

## Approach
- **Kernel:**
  - kernel.org stable 7.2.9, built with LLVM (`LLVM=1`, clang and lld) inside a bubblewrap sandbox where every Python binary is masked;
  - `omap2plus_defconfig` narrowed to OMAP4, with [config/glass.config](config/glass.config) on top;
  - built for size (`-Os`, xz) and stripped of what Glass has no hardware or use for (NFS, MTD, PCI, ATA, SCSI, IPv6, ftrace, and so on);
  - everything the full image drives is built in, so no modules ship.
- **Device tree:** two DTBs from one base, [dts/omap4-glass-xec-common.dtsi](dts/omap4-glass-xec-common.dtsi). Every pad, GPIO and address names its line in Google's `board-notle*.c`; GPIOs are those of EVT2 and later boards.
  - `safe`: memory, the TWL6030 PMIC, the USB gadget and ramoops. Every MMC controller is disabled, so it cannot touch the eMMC.
  - `full`: adds the eMMC, Wi-Fi and Bluetooth (BCM4330, from the mainline Galaxy Tab 2 pattern), the bq27520 gauge, the Synaptics touchpad, the MPU-9150 and the camera button.
- **Boot path:** Glass's Android bootloader (u-boot based) passes ATAGs only.
  - The DTB is appended to the zImage (`ARM_APPENDED_DTB`) and the bootloader's memory size (2 GB) and command line are folded into it (`ARM_ATAG_DTB_COMPAT`). The decompressor grows the DTB by 50% for that.
  - The initramfs is built into the kernel: the 15 MB decompressed kernel would overrun the bootloader's ramdisk slot 16 MB above the load address. The boot.img carries an empty ramdisk.
- **Size bound:** every boot image must fit in 5,861,376 bytes, the size of the AOSP 5.1.1 boot.img that real units have held in their boot partition (Google's XE24 recovery.img, 6,344,704 bytes, proves recovery is larger still). `scripts/build-images.sh` refuses anything larger; the real sizes come from the first backup's `partitions.txt`.
- **boot.img:** written by a small C tool (`tools/bootimg`), not AOSP's `mkbootimg.py`. It round-trips Google's own images byte for byte.
- **Userland:** Alpine Linux 3.24 armv7 (musl, busybox).
  - Packages are installed by apk-tools on the host with `--no-scripts`: nothing armv7 runs on the build host, and what the package scripts would do is done in [scripts/build-userland.sh](scripts/build-userland.sh).
  - Static checks only, in [scripts/check-userland.sh](scripts/check-userland.sh), no emulation: every ELF is ARM EABI5 hard-float, and every interpreter and library resolves inside its root.
- **No Python, proven mechanically:**
  - `tools/nopython/` shims are first on `PATH` and log and fail any `python*` call;
  - the real interpreters are masked inside the sandbox;
  - `out/python-calls.log` must stay empty.

## The images (`out/`, after `glass build`)
| image | what | how it is used |
| --- | --- | --- |
| `boot-safe.img` | mainline + safe DTB + initramfs | `glass test-boot safe`: the first boot, RAM only |
| `boot-full.img` | mainline + full DTB + initramfs; switches to the rootfs on userdata when one is there | `glass test-boot full`, later `glass flash-recovery full` |
| `boot-stock-kernel.img` | Google's own XE24 kernel (3.4.94) + this initramfs as its ramdisk | `glass test-boot stock`: the userland on known-good kernel code |
| `boot-xe24-adbroot.img` | Google's XE24 boot.img with `ro.secure=0`, `ro.debuggable=1` | `glass root-boot`: Android with root adb, for `glass backup` and `glass info` |
| `rootfs.img` | the Alpine rootfs, ext4, sparse | `glass flash-rootfs` (userdata); grows to fill the partition at its first boot |

On the Glass:
- **Shells:** a root shell on the USB serial gadget (`/dev/ttyACM0` on the desktop) and on the debug UART.
- **SSH:** dropbear on `172.16.42.1`, keys only: your `~/.ssh/id_ed25519.pub` is baked in at build time, or `GLASS_SSH_PUBKEY`.
- **DHCP:** the desktop gets `172.16.42.2`.
- **Commands:**
  - `glass-collect`: a tar of everything the next device-tree iteration needs;
  - on the rootfs only: `glass-wifi`, `glass-pan` (Bluetooth PAN to the desktop) and `glass-term` (SSH into the desktop's tmux session).

## Quick start
```sh
scripts/glass fetch    # download and verify everything (once; afterwards offline)
scripts/glass build    # userland, kernel, images
scripts/glass check    # static checks and SHA256SUMS
```
Then [docs/hardware-test.md](docs/hardware-test.md), step by step. `scripts/glass help` lists every verb.

## Safety, enforced in `scripts/glass`
- **Allowlist:** only `boot`, `recovery`, `system`, `cache` and `userdata` are ever written. `xloader`, `bootloader` and `fpga` are refused in code.
- **boot:** written only by `glass restore` and `glass restore-partition`. Tests run from RAM (`fastboot boot`), and the permanent install goes to `recovery`, so stock Android always stays bootable.
- **flash-recovery:** refuses without a backup and without a known partition size, and refuses an image larger than the partition.
- **Confirmation:** every write says what it will do and needs a typed `yes` (or `--yes`).
- **oem unlock:** `fastboot oem unlock` (run twice) wipes userdata.
- **Recovery:** Google's XE24 factory image (`glass restore`) and your own backup (`glass restore-partition`) bring it back.

## Known not to work yet
- **Display:** the notle panel (an LCoS behind an iCE40 FPGA, `panel-notle-panel` at I2C4 0x49) has no mainline driver. The DTS leaves the display alone: the bootloader programs the FPGA, and `FPGA_CRESET_B`, `DISP_ENB` and `LCD_RST_N` are never named.
  - `glass-collect` reads the DISPC registers the bootloader left (framebuffer address, size, timings), guarded by the DSS clock state, and `glass info` gets the same from stock Android. That is the input for a `simple-framebuffer` node next.
  - The stock-kernel image enables the panel (the postmarketOS hook), but that kernel has no framebuffer console.
- **Not described yet:** audio (TWL6040 and the ABE), the glasshub MCU, the LTR-506 light sensor (no mainline driver), the camera, GPS.
- **Bluetooth:**
  - the UART stays at hci_bcm's default speed: the operating baud rate of Google's vendor library was not determined, so `max-speed` is left unset;
  - no host-wake interrupt, so no low-power mode.
- **Wi-Fi:** uses the in-band SDIO interrupt, not the host-wake line.
- **Untested on hardware:** `fastboot boot` support on Glass's bootloader, partition sizes, and the board revision's GPIO set.

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
| `scripts/glass` | the entry point: build, test, back up, flash, restore |
| `scripts/fetch.sh` | downloads and verifies everything; unpacks into `src/`, `tools/apk-host/`, `firmware/` |
| `scripts/build-kernel.sh` | the LLVM kernel build in the Python-free sandbox; `GLASS_INITRAMFS` builds the initramfs in; checks every config option |
| `scripts/build-userland.sh` | `build/initramfs` and `build/rootfs` from the minirootfs and apk |
| `scripts/build-images.sh` | the five images, the size bound, `out/SHA256SUMS` |
| `scripts/check-userland.sh` | static checks of both roots |
| `config/glass.config` | the kernel config fragment |
| `dts/` | the Glass device trees (copied into the kernel tree at build) |
| `userland/initramfs/`, `userland/rootfs/` | `/init`, inittab, rcS, `glass-*` commands |
| `tools/bootimg/` | Android boot image v0 in C |
| `tools/nopython/` | the `python*` shims that log and fail |
| `docs/plan.md`, `docs/build-log.md`, `docs/hardware-test.md` | the plan, the build log with the hardware table, the first hardware session |
| `dl/`, `src/`, `build/`, `out/`, `firmware/`, `backup/` | fetched, built or backed up: never committed |

## Credits
- Google's Glass kernel sources (GPL-2.0), mirrored by GlassHack.
- The postmarketOS `google-glass` port (downstream kernel, 2017-2019): the boot layout and the display bring-up.
- The mainline Galaxy Tab 2 (`omap4-samsung-espresso`) and Kindle Fire (`omap4-kc1`) device trees: the BCM4330 and eMMC patterns.

## License
Scripts and tools: MIT. Device tree and kernel configuration: GPL-2.0-only OR MIT.
