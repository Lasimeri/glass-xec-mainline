# The first hardware session

Step by step, for the day the Glass XE-C arrives. Each step gives the command, what you should see, and what to send back if it does not happen. Run everything from the repository root after `scripts/glass fetch`, `scripts/glass build` and `scripts/glass check`. Every `fastboot boot` runs from RAM and writes nothing; the first write to the Glass is step 8.

## 0. Prerequisites
- **Battery:** charge the Glass fully. A boot that fails halfway on a low battery looks like a kernel problem.
- **Cable:** a USB data cable. Some micro-USB cables carry power only.
- **udev rules** for fastboot and adb (Google's vendor ID `18d1`). On Arch, the `android-udev` package installs them. Otherwise create `/etc/udev/rules.d/51-android.rules`:
  ```
  SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0660", TAG+="uaccess"
  ```
  then `sudo udevadm control --reload`.
- **Serial access:** to open `/dev/ttyACM0` (the Glass's USB serial console), your user needs the device's group: `uucp` on Arch, `dialout` on Debian. Check with `stat -c %G /dev/ttyACM0` once it appears, then `sudo usermod -aG uucp $USER` and log in again.
- **Desktop tools:** `fastboot`, `adb`, `ssh`, and one of busybox, screen or picocom for the serial console.
- **NetworkManager or DHCP:** the desktop takes a DHCP address on the new USB network interface automatically. NetworkManager does this by default for a new wired device.

## 1. Which Glass it is
Note the revision, from **Settings, Device info** in Android, or later with `adb shell cat /proc/meminfo`. XE-C has 2 GB, so `MemTotal` is about 1.9 GB; earlier units show about 600 MB. This build assumes XE-C (2 GB) and EVT2-or-later GPIOs.

**Send back:** the meminfo line, and the software version (XE24 expected).

## 2. Fastboot mode
```sh
adb reboot bootloader          # with USB debugging on in Glass's settings
fastboot devices               # one serial number
fastboot getvar all 2>&1 | tee out/getvar-all.txt
```
- **Without adb** (Google's documented way, confirmed by several owners): with the Glass off, hold the camera button, press the power button, and keep holding the camera button for 10 to 15 seconds. The display stays off in fastboot mode; the white LED flashes briefly every few seconds (one owner describes a slow pulse). `fastboot devices` is the only sure sign.
- **The recovery menu** (one owner's report): hold the camera button, briefly hold power, release power as soon as the white LED lights, release the camera button 5 seconds later. A menu appears on the display with an entry for fastboot mode.
- **If it never enters fastboot:** one owner found that on one particular computer the Glass always rebooted into Android instead; another USB port or computer worked. Use a USB 2 port, no hub.
- **You should see:** `fastboot devices` lists the Glass (USB ID `18d1:9001`).
- **Send back:** `out/getvar-all.txt`. It holds the bootloader version and perhaps partition sizes. The sizes decide whether `flash-recovery` can work without a backup. Owners on XE24 report the bootloader answers `partition-size:boot` with a bare `0x`, that is, no size; the CLI treats that as unknown.

## 3. Unlock
**This wipes all user data on the Glass** (Google documents it as such).
```sh
fastboot oem unlock
fastboot oem unlock            # Glass asks twice
```
- **You should see:** the bootloader reports it is unlocked (`fastboot getvar unlocked`, if it answers).
- **If it hangs:** one XE24 owner waited 15 minutes on the second `oem unlock`, killed it, and the Glass was fine (not bricked); the unlock had taken. Give it a few minutes, then check with `fastboot getvar unlocked` or by trying step 4.
- **Send back:** the output of both commands, if either fails.

## 4. Root adb, backup, hardware info
```sh
scripts/glass root-boot        # fastboot boot out/boot-xe24-adbroot.img: stock Android, root adb, RAM only
# wait for Android to come up, then:
scripts/glass backup           # every partition except userdata and cache into backup/DATE/
scripts/glass info             # getprop, iomem, partition sizes, DISPC registers, pin mux, regulators
```
- **The first unknown:** whether Glass's bootloader supports `fastboot boot` (RAM boot) at all. Google only documents `fastboot flash`. The one report found (an XE24 owner trying a TWRP image in 2022) says `fastboot boot` sent the image ("Sending 'boot.img'") and the Glass then started normal Android. That is consistent with two things: the bootloader ignores `boot` and starts the flashed boot partition, or that TWRP image failed early. This step tells them apart: the adb-root image is Google's own XE24 kernel and ramdisk with three properties changed, so
  - Android up and `adb shell id` says `uid=0(root)`: RAM boot works;
  - Android up and adb is not root: the bootloader booted the flashed partition; RAM boot is unsupported. **Stop** and send back the fastboot output. Then root comes from writing the adb-root image into **recovery** (never boot) and starting recovery; it is Android with root adb either way, because the image carries Android's ramdisk, not a recovery one. That decision is made together.
  - `fastboot boot` fails with "unknown command" or similar: the same, send back the exact error.
- **You should see:** `backup/DATE/` with one `.img` per partition, `partitions.txt` (name, device, bytes), `SHA256SUMS`, and `info/`. `glass backup` refuses to finish if any copy's size differs from the partition's.
- **Keep:** `backup/` (it is your way back). **Send back:** `backup/DATE/partitions.txt` and the whole `info/` directory (`info/dss-dispc.txt` most of all: it holds the framebuffer address the display work needs).

## 5. The safe image
```sh
fastboot reboot bootloader     # or step 2 again
scripts/glass test-boot safe   # RAM only; waits up to 90 s
```
- **You should see:**
  - **in the glasses:** a Linux text console (80 x 22) and a shell prompt a few seconds into boot.
    - The bootloader leaves the display running (its routine at 0x24604 in `bootloader.img` scans a 640 x 360 ARGB framebuffer at `0x80500000`), and the kernel unpacks over that address, so for a moment it may show noise.
    - `/init` then points the display at the console's framebuffer (`0x9c700000`, the device tree's `simple-framebuffer`).
    - If it stays dark or shows noise: `glass ssh`, then `cat /run/glass/init.log` (the `display:` line) and `glass collect` (`dss.txt`) say why.
    - `glass-display glass-term` puts the desktop's tmux session there (step 10 sets up the desktop side).
  - a new network interface on the desktop, `enx02474c415301` (the gadget's fixed address 02:47:4c:41:53:01), with `ip -br addr` showing `172.16.42.2`;
  - `/dev/ttyACM0` (the serial console);
  - then:
    ```sh
    scripts/glass ssh              # root@172.16.42.1
    scripts/glass collect          # out/collect-DATE.tar
    ```
- **If nothing appears within 90 s:**
  - Power-cycle the Glass (hold power). The safe image has no MMC controller, so it wrote nothing.
  - Send back `out/getvar-all.txt`, `dmesg | tail -n 60` on the desktop right after the attempt (did a USB device appear at all?), and a photo of the display.
  - **Crash logs:** the crash log (ramoops at `0xA0000000`) survives only a warm reset that lands in another mainline boot. Glass's stock kernel uses that region for its own RAM console and resets it. If a later mainline boot gets far enough, `ls /sys/fs/pstore` there.
- **Send back:** the collect tar, whatever happened.

## 6. The stock-kernel image
```sh
scripts/glass test-boot stock
```
The same userland, on Google's own 3.4.94 kernel.
- **USB:** the gadget is RNDIS (the desktop's `rndis_host` driver), with the same `172.16.42.1`. There is no USB serial console on this kernel.
- **Display:** the panel is switched on by the postmarketOS hook, but this kernel has no framebuffer console, so it stays empty or shows noise.
- **When it helps:** if step 5 failed, this separates a userland problem from a mainline-kernel problem.
- **Send back:** `glass collect` output, or the same as step 5 if nothing appears.

## 7. The full image, still writing nothing
```sh
scripts/glass test-boot full
scripts/glass ssh
```
The initramfs on the full image has no `iw` or BlueZ; those come with the rootfs in step 8. Here, check the drivers:
```sh
cat /proc/partitions                     # mmcblk0 with its partitions: the eMMC is up
ls /sys/class/net                        # wlan0: brcmfmac found the BCM4330
dmesg | grep -i -e brcmf -e bluetooth -e hci_bcm -e rmi4 -e mpu -e bq27
ls /sys/class/bluetooth                  # hci0
cat /sys/class/power_supply/*/uevent     # the bq27520's capacity and voltage
cat /proc/bus/input/devices              # the touchpad, the camera button
```
Init mounts userdata read-only, with no journal replay, only to look for the rootfs marker. It switches only when the marker is there.
- **Send back:** `glass collect` output.

## 8. The rootfs
```sh
scripts/glass flash-rootfs     # the first write: userdata (typed yes)
scripts/glass test-boot full   # init switches to the rootfs on userdata
scripts/glass ssh
glass-wifi add 'SSID' 'passphrase' && glass-wifi up
glass-wifi status
glass-pan pair AA:BB:CC:DD:EE:FF # the desktop's adapter (bluetoothctl show on the desktop)
glass-pan up AA:BB:CC:DD:EE:FF
glass-term key                   # add this line to the desktop's ~/.ssh/authorized_keys
glass-term                       # the desktop's tmux session "claude"
```
- **On first boot:** the filesystem grows to fill userdata (`/run/glass/rcS.log`).
- **Wi-Fi firmware:** `glass-wifi-fw glass` switches to Google's own firmware for the next boot, if linux-firmware's fails to associate.
- **Send back:** `glass collect`, `/run/glass/rcS.log`, `/run/glass/pan.log` if PAN fails.

## 9. Making it permanent
Two shapes. **Linux as the default boot, no cable ever** (what the user chose on 2026-10-07):
```sh
scripts/glass flash-boot stock          # the stock-kernel image into boot (typed yes)
```
Stock Android's boot stays in your backup (`glass restore-partition backup/DATE boot` puts it back; fastboot stays reachable by the buttons; recovery is untouched). Or **stock Android stays the default and Linux sits in recovery**:
```sh
scripts/glass flash-recovery full
```
- **Safety:** both refuse without a backup holding that partition and its size, and both check the image against it (8 MiB each on the user's unit). An oversized image is refused by the bootloader itself too ("too large for partition", reported for an 8.4 MB recovery on XE24).
- **`flash:raw`:** recent platform-tools (the desktop has 37.0.0) ask the bootloader for the partition size before writing a boot image and fail on Glass's empty answer ("Couldn't parse partition size '0x'"). The CLI then repeats the write as `fastboot flash:raw`, which owners confirm works on XE24.
- **Booting Linux:** `adb reboot recovery` from Android, `fastboot reboot recovery` (bootloader support **unverified**), or the recovery button combination of step 2.

## 9a. Wireless: Wi-Fi, the name "glass", ssh over the air
```sh
scripts/glass-wifi-copy --from lasimeri@192.168.0.78 HomeMixed   # a laptop's saved network onto the Glass
scripts/glass ssh glass-wifi status                               # ssid, state, address
GLASS_IP=192.168.0.4 scripts/glass ssh                            # over Wi-Fi; or just scripts/glass ssh
```
The Glass joins its stored networks at boot (`rcS-rootfs`), announces the hostname `glass` with its DHCP lease, and `scripts/glass` reaches it by that name when the router resolves it, else by `GLASS_IP`, else over USB. The passphrase is never shown or typed: `glass-wifi-copy` reads it on the laptop with sudo and feeds it down the Glass's ssh session, where `glass-wifi add` keeps the derived key only (`/etc/wpa_supplicant`, root only). On the stock kernel the radio is Google's `bcmdhd`, fed its firmware from `/lib/firmware/glass`. Works since 2026-10-07 (HomeMixed, 192.168.0.4, signal -63 dBm, 39 Mbit/s, 4.5 ms round trip). Bluetooth's daemons start only with `glass-pan`: the Glass has two cores and the stream's decoder wants one.

## 9b. The desktop's monitor on the glasses
```sh
scripts/glass-view.sh                      # the primary monitor, 24 frames/s paced; Ctrl-C stops
BUFFER_MS=42 scripts/glass-view.sh         # over Wi-Fi: a one-frame jitter buffer on the Glass
PACE=0 scripts/glass-view.sh DP-2 24       # another output, frames as they come
```
Works on the stock-kernel image (step 6). Capture is the compositor's own screencast through the desktop portal (`desk cast`, ~/deskpilot; one share dialog the first time), scaled and encoded H.264 on the GPU (NVENC); the stream rides the ssh session; the Glass decodes it with ffmpeg (on the rootfs, or pushed into RAM) on one thread into `/dev/fb0`. Nothing holds a frame; the Glass decodes 65 frames/s at this size. Kernel capture (`kmsgrab`) does not work on NVIDIA's driver (tiled 16-bit float scanout).

## 10. The desktop side of `glass-term`
- **tmux:** Claude Code has to run inside tmux for the Glass to attach to the same session: `tmux new -A -s claude`, then `claude ...` inside it. Today `~/tts079/claude-voice.sh` starts it directly in Konsole; that has to change.
- **sshd:** `sudo systemctl enable --now sshd`.
- **The Glass's key:** add the line `glass-term key` prints to `~/.ssh/authorized_keys`.
- **Bluetooth PAN:** the desktop is the NAP. Both ways below are to be verified on the day.
  - **bluez-tools:** a bridge with an address and a DHCP server, then `bt-network -s nap br-glass`:
    ```sh
    sudo ip link add br-glass type bridge
    sudo ip addr add 172.16.43.1/24 dev br-glass
    sudo ip link set br-glass up
    sudo dnsmasq --interface=br-glass --bind-interfaces --dhcp-range=172.16.43.2,172.16.43.20
    bt-network -s nap br-glass
    ```
    Then, on the Glass, `glass-term lasimeri@172.16.43.1`.
  - **NetworkManager:** its Bluetooth sharing (a `bluetooth` connection of type `nap` with a bridge).

## 11. Back to stock
```sh
scripts/glass restore          # Google's XE24 boot, recovery, system; cache erased, userdata wiped
```
Or one partition from your backup: `scripts/glass restore-partition backup/DATE recovery`. `xloader`, `bootloader`, `fpga` and `efs` (per-device data: the stock fstab mounts it as `/bootconfig`) are never written by any verb.

## What happened on the first hardware day (2026-10-07)
The unit: XE21 (XRW66), 2 GB, bootloader 0.5, `getvar all` empty, no partition sizes from fastboot; the partition table from the device: boot and recovery 8 MiB each (`mmcblk0p7`, `p6`), system 1 GiB, cache 768 MiB, userdata 13.5 GB, plus xloader, bootloader, fpga, bootconfig, misc.
- **Step 3:** unlock took one second.
- **Step 4:** `fastboot boot` runs the sent kernel (the previous-boot log proved it) but Google's rooted ramdisk rebooted itself at 23 s; root adb never came. Root came instead from step 6 over ssh, and the backup was taken over ssh (`backup/20261007-014302`, every partition but userdata).
- **Step 5:** the mainline safe image took over, brought no USB up and reset after about a minute, leaving no crash record. Unsolved; a serial cable or the display are the next instruments.
- **Step 6:** the stock-kernel image worked at the first try: RNDIS in 15 s, ssh, `wlan0` found by the built-in bcmdhd. The panel stayed dark until `panel-notle-dpi/enabled` was written 0 then 1 (the driver skips the LED when the bootloader left the display active); `rcS` does that now.
- **9b:** the desktop's monitor streamed to the glasses the same night (the user: "I can see everything on my desktop now").
- **Later the same night, on the user's word ("so I don't need to connect the cable anymore"):** the rootfs into userdata and the stock-kernel Linux image into boot. Since then the Glass boots Linux by itself in about 45 seconds, joins Wi-Fi at boot, and is reached over the air (`scripts/glass ssh`, `scripts/glass-view.sh`). Stock Android's boot stays in the backup; recovery is untouched.
- **Two traps on the way, both now in the scripts:** the bootloader refuses sparse images and wrote a raw 392 MB image for twelve minutes without finishing (a reset by hand ended it; the half-written rootfs carried the marker and was booted into), so partition images now go in from Linux over ssh (`flash-rootfs`, `flash-boot`: 374 MB in 23 s), fastboot only for the 6.6 MB boot image and `erase userdata`; and this kernel has no devtmpfs, so the RAM system's /dev was a plain directory that `mount --move` could not carry into the rootfs, which then had three device nodes and an ssh server dying on every connection; /dev lives on a tmpfs now, and the rootfs fills its own if it must. A rescue image (`boot-stock-kernel-stay.img`, RAM only) stays in the initramfs to mount and repair the rootfs.

## What is known about bricking (from owners' reports, 2013 to 2022)
- **The one documented brick:** flashing an XE9-or-earlier image onto a unit running XE10 or later (Google's warning). `scripts/fetch.sh` downloads only XE24 and XE22 images; `glass restore` flashes only XE24.
- **Recoverable mishaps reported:** an `oem unlock` that hung 15 minutes and was killed; a stock boot.img flashed where the rooted one was meant (reflashed); a recovery image too large for its partition (refused by the bootloader). All were fixed from fastboot.
- **Why fastboot stays reachable:** it lives in the `bootloader` partition, which no verb here writes, and the camera-and-power combination enters it without Android. A bad boot or recovery partition is therefore always re-flashable from Google's XE24 images (`glass restore`) or your backup.
- **`fastboot boot` writes nothing:** every test-boot above runs from RAM; a hung boot is a power-cycle.
- **A factory cable** (micro-USB with the ID pin tied to VBUS) makes Google's kernel reboot into fastboot with adb on. It is one more way in if Android boots but its debugging is off.

## The rooted ROM that exists for this hardware (reference, not used)
The only non-Google ROM for the Explorer Edition is jtxdriggers' AOSP 5.1.1 (April 2016, XDA): AOSP with the XRX13B 3.4 kernel built from source and XE22's blobs, reported working on the 2 GB model; Wi-Fi, Bluetooth pairing, touchpad and camera work, audio and backlight do not. It installs by flashing `system` and `boot`, so it replaces stock Android; it is not part of this plan. `scripts/fetch.sh` keeps its zip in `dl/aosp/` (archive.org mirror, MD5 published on XDA) because its `boot.img` is a known-good custom boot image for this bootloader, a reference for this build's images (page size, load addresses, command line), and because its device tree (`github.com/justindriggers/android_device_glass_glass-1`) documents the hardware.

## Troubleshooting
| symptom | likely cause | what to do, what to send |
| --- | --- | --- |
| No USB device at all after `test-boot` (`dmesg` on the desktop shows nothing) | The kernel did not boot, or the USB mux or MUSB did not come up | Try `test-boot stock` (step 6). If stock comes up and safe does not, it is the mainline kernel or DTS: send `getvar all`, desktop `dmesg`, a display photo |
| A USB device appears, but no IP | The gadget is up, DHCP is not | `ip -br link` for the `enx...`/`usb0` interface; `sudo ip addr add 172.16.42.2/24 dev IFACE`, then `glass ssh`. The serial console (`glass serial`) works without IP: `cat /run/glass/rcS.log` |
| Up for about 30 s, then dead | Mainline switching off TWL6030 rails or clocks it thinks unused | The command line already has `clk_ignore_unused regulator_ignore_unused`; send `glass collect` taken before the 30 s mark (`regulators.txt`, `clocks.txt`) |
| Reboots in a loop | The watchdog, or a panic (`panic=10` reboots after 10 s) | The OMAP watchdog driver is built in to stop a watchdog the bootloader armed. Send the timing (seconds to reboot) and any serial output |
| `brcmfmac: ... firmware ... failed` | Firmware file name or revision | `ls /lib/firmware/brcm/`; brcmfmac asks for `brcmfmac4330-sdio.bin` and `.txt` (or the `google,glass-xec` variant); try `glass-wifi-fw glass` |
| `Bluetooth: hci0: BCM: Patch brcm/....hcd not found` | The BT chip reports a revision other than BCM4330B1 | Note the exact file name in the message; the patch is installed as `BCM4330B1.hcd` and `BCM.hcd`, so a link under the requested name fixes it |
