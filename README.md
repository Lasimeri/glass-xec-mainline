# glass-xec-mainline

The latest mainline Linux kernel for **Google Glass Explorer Edition XE-C** (the 2 GB revision, TI OMAP4430), with no Python anywhere in the build. The goal is Glass as a head-mounted Linux terminal.

**Status: work in progress, not yet tested on hardware.** The glasses are in the mail. Everything here is built and checked offline. Hardware facts come from Google's own Glass kernel sources and are cross-checked against independent teardowns ([docs/build-log.md](docs/build-log.md)). Nothing is claimed to work on a Glass until it has.

## Approach
- **Kernel:** current kernel.org stable (7.2.9 at the start), built with LLVM (`LLVM=1`, clang and lld), not a GCC 6 toolchain.
- **Device tree:** a new `omap4-google-glass.dts`. Every line comes from a row of the hardware table that Google's 3.4.83 board files (`board-notle*.c`, codename "notle") give.
- **Boot:** Glass's Android bootloader (u-boot-based) passes ATAGs only. The DTB is appended to zImage with `CONFIG_ARM_ATAG_DTB_COMPAT`, so the bootloader's memory size (2 GB on XE-C) and command line are folded into it.
- **boot.img:** written by a small C tool, not AOSP's `mkbootimg.py`. It must round-trip Google's own boot images byte for byte.
- **No Python, proven mechanically:**
  - the build runs with `tools/nopython/` first on `PATH` (any `python*` call is logged and fails);
  - inside a bubblewrap sandbox, the real interpreters are masked;
  - the log of calls must stay empty.
- **First boot without a serial port:** the device tree starts with what keeps a lifeline:
  1. memory (with the secure monitor's 3 MB at 0xBFD00000 kept off-limits);
  2. eMMC;
  3. the TWL6030 PMIC;
  4. USB gadget networking from the initramfs;
  5. ramoops crash logs in a fixed region, `panic=10`.

  Display, Wi-Fi, Bluetooth, audio and sensors follow.
- **Rootfs:** Alpine Linux armv7 (musl, OpenRC).

## Layout
| path | what |
| --- | --- |
| `scripts/fetch.sh` | downloads the kernel, Google's Glass tree and pmaports' config; verifies the signature and checksums; unpacks into `src/` |
| `tools/nopython/` | the `python*` shims that log and fail |
| `docs/plan.md` | the full plan: hardware, the paths considered, backup and recovery, links for SSH |
| `docs/build-log.md` | the build log, hardware table with source lines, and the cross-checks |

## Safety, before touching a Glass
- Back up every partition first.
- Never write `xloader` or `bootloader`.
- Test with `fastboot boot`; if Glass's fastboot cannot do that, use the recovery partition and keep `boot` stock.
- Google's final XE24 factory images restore it.

## Credits
- Google's Glass kernel sources (GPL-2.0), mirrored by GlassHack.
- The postmarketOS `google-glass` port (downstream kernel, 2017-2019) for the boot layout and display bring-up.

## License
Scripts and tools: MIT. Device tree and kernel configuration: GPL-2.0-only OR MIT.
