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
- **Without adb:** holding the camera button while powering on is said to enter the bootloader (**unverified**).
- **You should see:** `fastboot devices` lists the Glass.
- **Send back:** `out/getvar-all.txt`. It holds the bootloader version and perhaps partition sizes. The sizes decide whether `flash-recovery` can work without a backup.

## 3. Unlock
**This wipes all user data on the Glass** (Google documents it as such).
```sh
fastboot oem unlock
fastboot oem unlock            # Glass asks twice
```
- **You should see:** the bootloader reports it is unlocked (`fastboot getvar unlocked`, if it answers).
- **Send back:** the output of both commands, if either fails.

## 4. Root adb, backup, hardware info
```sh
scripts/glass root-boot        # fastboot boot out/boot-xe24-adbroot.img: stock Android, root adb, RAM only
# wait for Android to come up, then:
scripts/glass backup           # every partition except userdata and cache into backup/DATE/
scripts/glass info             # getprop, iomem, partition sizes, DISPC registers, pin mux, regulators
```
- **The first unknown:** whether Glass's bootloader supports `fastboot boot` (RAM boot) at all. Google only documents `fastboot flash`. If `glass root-boot` fails with "unknown command" or similar, **stop** and send back the exact error. Do not flash the adb-root image anywhere yet; flashing it to recovery would not help, because recovery boots the recovery ramdisk, not Android. That decision is made together.
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
```sh
scripts/glass flash-recovery full
```
- **Safety:** this refuses without a backup and without a known recovery size, and it never touches boot. Stock Android stays the normal boot; Linux is in recovery.
- **Booting Linux:** `adb reboot recovery` from Android, or `fastboot reboot recovery`. Whether the bootloader supports the latter is **unverified**, and so is a button combination for recovery.

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

## Troubleshooting
| symptom | likely cause | what to do, what to send |
| --- | --- | --- |
| No USB device at all after `test-boot` (`dmesg` on the desktop shows nothing) | The kernel did not boot, or the USB mux or MUSB did not come up | Try `test-boot stock` (step 6). If stock comes up and safe does not, it is the mainline kernel or DTS: send `getvar all`, desktop `dmesg`, a display photo |
| A USB device appears, but no IP | The gadget is up, DHCP is not | `ip -br link` for the `enx...`/`usb0` interface; `sudo ip addr add 172.16.42.2/24 dev IFACE`, then `glass ssh`. The serial console (`glass serial`) works without IP: `cat /run/glass/rcS.log` |
| Up for about 30 s, then dead | Mainline switching off TWL6030 rails or clocks it thinks unused | The command line already has `clk_ignore_unused regulator_ignore_unused`; send `glass collect` taken before the 30 s mark (`regulators.txt`, `clocks.txt`) |
| Reboots in a loop | The watchdog, or a panic (`panic=10` reboots after 10 s) | The OMAP watchdog driver is built in to stop a watchdog the bootloader armed. Send the timing (seconds to reboot) and any serial output |
| `brcmfmac: ... firmware ... failed` | Firmware file name or revision | `ls /lib/firmware/brcm/`; brcmfmac asks for `brcmfmac4330-sdio.bin` and `.txt` (or the `google,glass-xec` variant); try `glass-wifi-fw glass` |
| `Bluetooth: hci0: BCM: Patch brcm/....hcd not found` | The BT chip reports a revision other than BCM4330B1 | Note the exact file name in the message; the patch is installed as `BCM4330B1.hcd` and `BCM.hcd`, so a link under the requested name fixes it |
