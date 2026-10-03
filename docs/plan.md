# Google Glass XE-C: a pure Linux build (2026-10-03)

Goal: Glass Explorer Edition XE-C (the 2 GB revision, June 2014) running GNU/Linux with no Android userspace, used as a head-mounted terminal. It SSHes to the desktop and shows Claude Code (`tmux attach`), ideally over Bluetooth.

Status: a plan built from primary sources. Nothing here has been run on a Glass yet. Lines marked **unverified** are expectations, not facts.

## The hardware (XE-C)
- **SoC:** TI OMAP4430, dual Cortex-A9 at 1.2 GHz, ARMv7, `armhf`.
- **RAM:** 2 GB on XE-C (682 MB on earlier units). 16 GB eMMC.
- **Display:** 640x360. On the stock kernel it is an `omapdss` DPI panel called `panel-notle-dpi`, shown through `omapfb` as `/dev/fb0`. The GPU is a PowerVR SGX540: no open 3D driver, so the display is a framebuffer.
- **Wi-Fi and Bluetooth:** Broadcom BCM4330 (`fw_bcmdhd.bin` + `bcmdhd.cal` for Wi-Fi, `bcm4330.hcd` for Bluetooth). Wi-Fi is 802.11b/g, 2.4 GHz.
- **Other firmware in the factory image:**
  - `ducati-m3-core0.xem3` and `ducati-m3-core0-2gb.xem3`: the Cortex-M3 media cores. There is a separate 2 GB build, which matters for XE-C.
  - `tesla-dsp.xe64T`: the DSP.
  - `glasshub.s19`: Glass's sensor/hub microcontroller.
- **Internal codename:** "notle". The kernel config is `notle_defconfig`, the TWRP device name is `glass_1`, and the postmarketOS codename is `google-glass`.
- **Bootloader:** Google allows `fastboot oem unlock`, run twice. Boot images are Android `boot.img`.

## What exists already
- A postmarketOS port, `google-glass`, in pmaports `device/testing`:
  - added in Nov 2017 by SpinlockLabs (pmbootstrap MR 957);
  - firmware and display init added in Jun 2019 (pmaports !462, Alex Zenla).
- **Kernel package `linux-google-glass`:** Linux 3.4.83 from `github.com/GlassHack/factory-kernel` at commit `1091b53a0b5e20d23a8447161e181e2a53e944de` (default branch `glass-omap-xrv67`).
  - The config is based on `arch/arm/configs/notle_defconfig`.
  - It builds only with GCC 6, plus a patch (`gcc10-extern_YYLOC_global_declaration.patch`) so a newer host GCC can build the host tools.
  - Google's own source: `https://android.googlesource.com/kernel/omap.git` (glass branches).
- **deviceinfo** (the boot.img layout to reproduce by hand):
  - `deviceinfo_arch="armhf"`, screen 640x360, `deviceinfo_flash_method="fastboot"`
  - cmdline `console=ttyO2,115200n8 vmalloc=500M androidboot.console=ttyO2 androidboot.carrier=wifi-only product_type=w cpuidle_sysfs_switch`
  - base `0x80000000`, kernel offset `0x00008000`, ramdisk offset `0x01000000`, second offset `0x00f00000`, tags offset `0x00000100`, page size `2048`
- **Display bring-up** (the port's initramfs hook; without it the screen stays dark):
  ```sh
  echo 1 > /sys/devices/platform/omapdss/manager2/panel-notle-dpi/enabled
  echo 160 > /sys/devices/platform/omapdss/manager2/panel-notle-dpi/brightness
  echo 0 > /sys/devices/platform/omapfb/graphics/fb0/blank
  echo "U:640x360p-312" > /sys/devices/platform/omapfb/graphics/fb0/mode
  ```
- **Firmware package `firmware-google-glass`:** taken from `github.com/GlassHack/firmware` (blobs extracted from the factory image). Installed under `/lib/firmware/postmarketos/`.
- **What the port's wiki reports** (search-result snippet; the wiki blocks automated readers, so read it in a browser):
  - downstream kernel;
  - "surprisingly capable, with a desktop, WiFi, audio, Bluetooth";
  - 3D and some UIs broken;
  - not recommended for new use because Alpine may drop `armhf`.

## Before anything: backup and a way back
1. Unlock: `fastboot oem unlock`, twice. This wipes user data.
2. Get root adb with Google's own rooted `boot.img` for XE24 (Glass system downloads page). Then `adb root`.
3. List the partitions: `adb shell ls -l /dev/block/platform/*/by-name/`. The exact path is **unverified**.
4. Copy every partition to the desktop, e.g. `adb exec-out dd if=/dev/block/... > NAME.img`.
5. **Never write `xloader` or `bootloader`.** Whether OMAP4 USB peripheral boot can recover a Glass with a broken bootloader is **unverified**; treat that as fatal.
6. **Way back:** Google's final XE24 factory image.
   ```sh
   fastboot flash boot boot.img
   fastboot flash system system.img
   fastboot flash recovery recovery.img
   fastboot erase cache
   ```
   Then wipe userdata.

## Path A: postmarketOS (fastest, uses pmbootstrap)
pmbootstrap is a Python program. It runs on the desktop as a tool, and Path B avoids it.
1. `pmbootstrap init`: choose vendor `google`, device `glass`, a small UI (`none` or `console`), and include the non-free firmware.
   - If `google-glass` is missing from the list, the device was moved to archived or dropped together with `armhf`. Then either pin an older pmaports or channel that still has it, or use Path B.
2. `pmbootstrap install`.
3. Glass in fastboot (`adb reboot bootloader`, or the camera button held while powering on).
4. `pmbootstrap flasher flash_rootfs` writes userdata. Then either:
   - `pmbootstrap flasher boot` to boot from RAM first, with nothing written to `boot`; or
   - `pmbootstrap flasher flash_kernel` to keep it.
5. SSH over USB: pmOS's USB network gadget gives the Glass `172.16.42.1` from the desktop. Run `ssh user@172.16.42.1`.

## Path B: no Python, Devuan or Alpine armhf on the same kernel
Use this if pmOS no longer carries `armhf`, or to stay Python-free.
1. **Kernel:**
   - Build `GlassHack/factory-kernel` at `1091b53` with an ARM GCC 6 cross compiler (an old Linaro 6.x or crosstool-ng). Use the config `config-google-glass.armhf` from pmaports (adds DEVTMPFS and the like on top of `notle_defconfig`) and apply the YYLOC patch.
   - `make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage modules`
   - OMAP4 3.4 uses board files: no device tree, just `zImage`.
2. **Rootfs**, built on the desktop with qemu-user binfmt:
   - **Devuan armhf** (`debootstrap --arch=armhf` against Devuan): sysvinit by default. Current systemd will not run on a 3.4 kernel, so avoid Debian's systemd default. glibc still supports kernel 3.2 and later on ARM.
   - **Or Alpine armhf minirootfs:** musl + OpenRC. Fine on 3.4, if Alpine still publishes `armhf`.
   - Add the firmware blobs under `/lib/firmware` (plus wherever the bcmdhd module's `firmware_path` and `nvram_path` point).
3. **Initramfs:** a static busybox `/init` that mounts proc, sys and devtmpfs, runs the four display lines above, enables the USB network gadget (below), mounts the rootfs from userdata (or an ext4 image file on it), then `switch_root`.
4. **boot.img with a C tool** (`abootimg`, or osm0sis' C `mkbootimg`), using exactly the base, offsets, page size and cmdline from the deviceinfo above.
5. **Test without writing:** `fastboot boot boot.img`. Then `fastboot flash userdata rootfs.img` (`img2simg` if it's too big for one transfer), and `fastboot flash boot boot.img` once it boots.

## Path C: mainline kernel (research, not a recipe)
Mainline Linux supports OMAP4430 (PandaBoard), but there is no device tree for Glass.
- **Would need:** the `notle` DPI panel, the BCM4330 on SDIO (brcmfmac in mainline), the touchpad, the glasshub microcontroller, battery and charger, and audio.
- **Gain:** a modern kernel (systemd, current BlueZ, nftables).
- **Cost:** a real porting project, done from the 3.4 board files.

## Links for the SSH session
1. **USB first** (most certain). The Android 3.4 kernel has the `android_usb` gadget:
   ```sh
   echo 0 > /sys/class/android_usb/android0/enable
   echo rndis > /sys/class/android_usb/android0/functions
   echo 1 > /sys/class/android_usb/android0/enable
   ip addr add 172.16.42.1/24 dev rndis0   # interface name unverified (rndis0 or usb0)
   ```
2. **Wi-Fi:** `bcmdhd` with `fw_bcmdhd.bin` + `bcmdhd.cal`, then `wpa_supplicant` (`-Dnl80211`, else `-Dwext`). 2.4 GHz only.
3. **Bluetooth PAN** (the goal; least certain):
   - BCM4330 Bluetooth sits on a UART and needs its patch file loaded (`brcm_patchram_plus --patchram bcm4330.hcd --enable_hci --baudrate ... /dev/ttyO?`). The UART number and baud rate are **unverified**.
   - BlueZ 5 relies on the kernel's management interface. Whether 3.4 has enough of it for BlueZ 5's PAN (`bt-network`, or BlueZ's `network1` API) is **unverified**; BlueZ 4 may be needed.
   - The desktop runs NAP; the Glass connects as PANU.
   - Expect 1 to 2 Mbps, enough for a terminal.

## Display and input as a terminal
- **Display:** the kernel framebuffer console on fb0, nothing graphical needed.
  - Terminus 6x12 (`ter-112n`) gives about 106x30 characters; 8x16 (`ter-116n`) gives 80x22.
  - Whether that is readable through the prism is **unverified**.
- **Typing:**
  - the touchpad is not a keyboard;
  - a Bluetooth keyboard needs the Bluetooth stack working;
  - the simplest split: Glass only displays, input stays on the desktop (voice079 already types into Claude Code).
- **Prerequisite:** Claude Code must run inside tmux on the desktop for the Glass to attach to the same session. Today it runs straight in Konsole (`claude-voice.sh`).

## Open questions to settle on the hardware
- [ ] `free -m` shows about 2 GB with the factory kernel. Which Ducati blob does the 2 GB unit need?
- [ ] Partition names and sizes, plus a full backup taken.
- [ ] Display hook works on XE-C; best console font size through the prism.
- [ ] USB gadget interface name and the desktop side (RNDIS).
- [ ] bcmdhd Wi-Fi association.
- [ ] Bluetooth UART, patchram baud rate, BlueZ version that works on 3.4, PAN to the desktop.
- [ ] Battery and charging readout, and thermal limits. OMAP4 runs hot in this frame.

## Sources
- postmarketOS port (open in a browser; Anubis blocks automated readers): https://wiki.postmarketos.org/wiki/Google_Glass_(Explorer_Edition)
- pmaports `device-google-glass` (deviceinfo, initfs-hook.sh): https://gitlab.com/postmarketOS/pmaports/-/tree/master/device/testing/device-google-glass
- pmaports `linux-google-glass` (APKBUILD, config): https://gitlab.com/postmarketOS/pmaports/-/tree/master/device/testing/linux-google-glass
- pmaports `firmware-google-glass`: https://gitlab.com/postmarketOS/pmaports/blob/258e92e9421d43ef87ba8d83b1a47e7da10843b5/firmware/firmware-google-glass/APKBUILD
- Port MR: https://gitlab.com/postmarketOS/pmbootstrap/-/merge_requests/957
- Kernel source mirror: https://github.com/GlassHack/factory-kernel and firmware: https://github.com/GlassHack/firmware
- Google Glass XE system and kernel downloads (factory images, rooted boot.img, kernel source): https://developers.google.com/glass/tools-downloads/system
- Final XE software update: https://support.google.com/glass/answer/9649198
- XE-C RAM revision: https://techcrunch.com/2014/06/24/google-revises-glass-with-more-ram-the-day-before-io/amp/
- TWRP for Glass XE: https://xdaforums.com/t/twrp-for-the-google-glass-explorer-edition-glass_1.4422517/
