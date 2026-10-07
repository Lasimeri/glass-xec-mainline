# Google Glass XE-C: mainline Linux build log

Plan and background: [[Google Glass XE-C - pure Linux build (2026-10-03)]].

**Goal (the user's words):** "get the latest Linux kernel built for these Google Glasses, no Python at all anywhere, and just get it up and running and working for whenever I get the Google Glasses in the mail."

**Decisions:**
- **Kernel:** mainline stable from kernel.org, not Google's 3.4 kernel. Google's 3.4 tree (GlassHack/factory-kernel@1091b53, `board-notle*`) is the hardware reference only.
- **No Python anywhere:**
  - Build with LLVM (`LLVM=1`, clang/lld 22.1.8 on the desktop), not a GCC 6 cross toolchain.
  - `boot.img` made by a small C tool, not AOSP's `mkbootimg.py` (`/usr/bin/mkbootimg` here is the Python one).
  - No `make dtbs_check` (it uses Python dtschema).
- **Boot path:** Glass's Android bootloader only passes ATAGs (OMAP4 3.4 used board files), so the device tree is appended to zImage (`CONFIG_ARM_APPENDED_DTB`) with `CONFIG_ARM_ATAG_DTB_COMPAT`, which merges the bootloader's memory and command line into it.
- **Workspace:** `~/glass-xec` (`dl/`, `src/`, `out/`), on disk.

## Log
- 11:39 Host: GCC 16.2.1, clang/lld 22.1.8, tcc, jq. Missing: lzop, abootimg, qemu-arm, debootstrap. Disk: 223 GB free.
- 11:40 kernel.org: stable 7.2.9, mainline 7.3-rc5, longterm 6.18.55. Target: 7.2.9.
- 11:40 Linaro's release server is retired (the 6.5-2018.12 URL redirects to a contact page). GCC 6 is now on ARM's "Legacy Linaro GNU Toolchains" page. Not needed for the mainline path.
- 11:41 Downloading linux-7.2.9.tar.xz (+ sha256sums.asc), GlassHack factory-kernel@1091b53, pmaports `config-google-glass.armhf`.
- 11:43 Integrity:
  - `linux-7.2.9.tar.xz` sha256 OK against `sha256sums.asc`. The signature is good, from the Kernel.org checksum autosigner, key `B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1`, fetched by WKD.
  - `factory-kernel-1091b53.tar.gz` and `config-google-glass.armhf` sha512 OK against the pmaports APKBUILD sums.
- 11:44 Gates on 7.2.9 (from the source tree):
  - `ARCH_OMAP4` present;
  - `omap443x.dtsi`, `omap4.dtsi` present;
  - `ARM_APPENDED_DTB`, `ARM_ATAG_DTB_COMPAT`, `ARM_ATAG_DTB_COMPAT_CMDLINE_FROM_BOOTLOADER` / `_EXTEND` present;
  - `HIGHMEM` and `VMSPLIT_*` present.
  - No Glass or notle in any mainline DTS.
  - Closest mainline relatives: `omap4-kc1.dts` (Kindle Fire 1), `omap4-samsung-espresso*.dts` (Galaxy Tab 2), `omap4-droid4-xt894.dts`.
- 11:45 Factory kernel 3.4.83 board files: `board-notle.c`, `board-notle.h`, `board-notle-io.c`, `-wifi.c`, `-bluetooth.c`, `notle-usb-mux.c` (3723 lines). Also `drivers/video/omap2/displays/panel-notle-dpi.c`, `include/video/omap-panel-notle.h`, `notle_defconfig`, and `notle_ubuntu_defconfig` (!).

### Hardware table (source: board-notle.c / board-notle.h at 1091b53)
| block | detail | source line |
|---|---|---|
| board revision | GPIO 34/40/42 (gpmc_ad10, gpmc_a16, gpmc_a18); EVT1=1, EVT2=2, EVT3=3, DVT1=4, V1.5 proto=5 | board-notle.h |
| PMIC | TWL6030 on I2C1 (`omap4_pmic_init("twl6030", ...)`, IRQ SYS_2N) | board-notle.c:1931 |
| audio | TWL6040 (audpwron GPIO 127), ABE | board-notle.c:1388, 2026 |
| gas gauge | TI bq27520 @ I2C1 0x55 (EVT2 and later) | :1588 |
| touchpad | Synaptics RMI4 `rmi_i2c` @ I2C3 0x20; IRQ GPIO 3 (EVT2+) / 32 (EVT1) | :1597, h |
| display | `panel-notle-panel` @ I2C4 0x49 + DPI `notle_nhd_panel`; LCD_RST_N GPIO 94 (EVT2+), DISP_ENB GPIO 84 | :593, :1738 |
| FPGA | CDONE GPIO 85, CRESET_B GPIO 87 (iCE40 configuration pins) | h |
| IMU | InvenSense MPU-9150 @ I2C4 0x68; INT GPIO 4 (EVT2+) | :1741, h |
| light/prox | LTR-506ALS @ I2C4 0x3a; INT GPIO 103 (EVT2+) | :1747, h |
| glasshub MCU | `glasshub` @ I2C4 0x35 | :1759 |
| I2C speed | buses 2, 3, 4 at 384 kHz | :1933 |
| eMMC | MMC2 (`omap_hsmmc.1`), 8-bit, 1.8 V DDR, non-removable, OCR 2.9-3.0 V, vmmc from a TWL vaux | :708-716, :1061 |
| Wi-Fi | "bcm4329" slot on MMC5 SDIO, 4-bit, 1.65-1.95 V, keep-power; WL_RST_N GPIO 43, WL_BT_REG_ON GPIO 48, host wake GPIO 0 (hog 97) | :722, h |
| Bluetooth | BCM4330 BT; BT_RST_N GPIO 113 (EVT2+), BT_WAKE GPIO 36, host wake GPIO 2 (EVT2+) | :1243, h |
| USB | MUSB UTMI, OTG or peripheral, power 100; TWL6030 USB PHY; USB mux CB0 GPIO 46 (EVT2+) / CB1 GPIO 45 | :692 |
| UARTs | 2, 3, 4 initialised; UART3 = debug console (`ttyO2`) | :1226 |
| camera button | GPIO wk30 | h |
| GPS | SiRF; GPIO 138-140 (EVT2+), functional only on V1.5 | h, :2079 |
| memory | RAM console at 0x80000000+512M (0xA0000000), 2 MB; **secure monitor RAM `PHYS_ADDR_SMC_MEM` = 0xBFD00000, 3 MB** (must be no-map in mainline); ION secure-input and tiler carve-outs below it | omap_ram_console.h, omap4_ion.h |

### Exa cross-checks (no hardware yet, so every claim gets an independent source)
- **WikiDevi** (FCC A4R-X1 record): OMAP4430, SanDisk SDIN5C2-16G eMMC, Elpida RAM, USI WM-BN-BM-04 module with **Broadcom BCM4330**, **TI TWL6030B107** PMIC, **Lattice iCE40LP4K** FPGA, SiRF GSD4e GPS, Himax HX7309 LCoS 640x360. Agrees with the board code. https://wikidevi.wi-cat.ru/Google_Glass_(XEB)
- **Catwig teardown:** InvenSense **MPU-9150** on the display flex, Wolfson WM7231 MEMS mics (two). https://web.archive.org/web/20201204053209/http:/www.catwig.com/google-glass-teardown/
- **Google's 2 GB commit** (kernel/omap 23b090e, "Enable 2GB support"):
  - The memory size comes from **ATAG_MEM set by u-boot**: the Glass bootloader is u-boot-based and passes ATAGs, so `ARM_ATAG_DTB_COMPAT` will carry the XE-C's 2 GB into the DTB.
  - 2 GB units have two Elpida LPDDR2 4G-S4 parts (CS0 + CS1); the size is read from the DMM LISA map.
  - One page or MB is taken off the top so that 0x80000000 + 2 GB does not overflow 32 bits.
  - Separate 2 GB Ducati/Tesla firmware.
  - https://android.googlesource.com/kernel/omap/+/23b090eb82b00a66ba17b6258f8d535d2e1d9f9a%5E%21/
- **Appended DTB with ATAGs** (Nicolas Pitre's patch thread): `ATAG_CMDLINE`, `ATAG_MEM` and `ATAG_INITRD2` are folded into the appended DTB; growing the tree can fail, so the DTB gets padding (`dtc -p`) or placeholder `memory` / `chosen` nodes. https://lists.ozlabs.org/pipermail/devicetree-discuss/2011-June/006055.html
- **Mainline Kindle Fire 1** (OMAP4430) boots through its own u-boot (`bootz` with a DTB), not the Android bootloader. Glass has no replacement u-boot, so the appended DTB is our route.
- **`fastboot boot`** (RAM boot) on Glass: **not confirmed.** Google's page and Stack Overflow only show `fastboot flash`.
  - Plan: try `fastboot boot` first.
  - If it is unsupported, flash the mainline image to **recovery** and keep **boot** stock, so the stock system always remains bootable.
  - Glass enters a menu with camera + power; whether that menu is the bootloader's or the recovery partition's is unverified.
- **Alpine armv7:** `latest-stable` is 3.24.x (3.24.2 dated 2026-09-17), armv7 minirootfs published. https://archive.sunet.se/mirror/alpinelinux.org/v3.24/releases/armv7/
- Google's documented root and unflash commands (`fastboot oem unlock` twice, flash the rooted boot.img, `adb root`; restore boot/system/recovery). https://developers.google.com/glass/tools-downloads/system

## 2026-10-07: what the hardware taught (stock XE24 kernel 3.4.94, Alpine userland on the eMMC)
Details and commands in [daily-use.md](daily-use.md) and [ducati-omx.md](ducati-omx.md); the commits name each change.
- **Heat:** the CPU is held at 300 MHz by the thermal cap (cpufreq max_thermal, case governor 57/62 C) whatever the governor asks; the stream settled at 15 frames/s.
- **Display:** omapfb's pan never reaches the hardware; glass-fb flips by writing DISPC GFX_BA0/BA1 and the GO bit through /dev/mem. No framebuffer console: glass-console draws the idle shell.
- **Sound:** the bone conduction transducer is the TWL6040 Earphone output. Alpine's alsa-lib is time64 and calls SYNC_PTR 0xc0884123, which 3.4 lacks: the PCM stopped and restarted every one to five seconds and the refusal was logged thousands of times a second. glass-play now uses the 3.4 ioctls directly (48 kHz device, the 32 kHz stream resampled, never stopped, real-time priority): no restarts, no underruns. musl's sched_setscheduler() is an ENOSYS stub.
- **Wi-Fi:** power save must be off. After an ungraceful reboot the router refused associations (status 1) until attempts paused: shutdown now deauthenticates, the watchdog backs off. The BCM4330 driver hung once (SDIO timeout, radio off, firmware paths emptied); `glass-wifi up` restores it.
- **Ducati:** its firmware request is made before userspace exists (no hotplug helper in the kernel); glass-firmware answers it from /lib/firmware. The camera (OV5680) and the H.264 encoder work through TI's DOMX wire spoken in C (tools/glass-camera); the camera needs OMX_CaptureVideo mode to deliver frames.
- **Paths:** sessions prefer home Wi-Fi; the USB cable is a fallback; path guards move every session when the path changes (glass unplug-test, glass reboot-test).
