
# Andromarchy

**Omarchy (Arch Linux + Hyprland) on Android, as one app. No root, no proot, no virtual machine.**

Andromarchy runs a full Arch Linux ARM desktop on an Android tablet. Linux programs run directly on the device, with
its GPU (Mesa Turnip/Freedreno on KGSL), sound, cameras, touch screen and on-screen keyboard. You get Omarchy's
desktop with its apps, the Flatpak store, Brave, VS Code, and anything else from Arch Linux ARM.

<img width="800" height="450" alt="andromarchy_demo" src="https://github.com/user-attachments/assets/61e4b65d-52a7-4d93-b21b-31fb3c5ac1d8" />

## Requirements

- Android 11 or later, with a **Qualcomm Adreno GPU** (Snapdragon).
- About 15 GB of free space.
- Wi-Fi for the first launch, which downloads the system (about 1.9 GB).

Developed and tested on a POCO Pad 2 Pro (Adreno 810, HyperOS / Android 16). Other Snapdragon tablets should work but
are untested.

## Install

1. Download `Andromarchy.apk` from the [latest release](../../releases/latest) and install it.
2. Open it. The first-launch screen asks for your name, email and keyboard layouts, then downloads and unpacks the
   system.
3. The desktop appears. Omarchy's menu opens with **Super + Space**.

The [user guide](docs/user-guide.md) explains the touch gestures, the on-screen keyboard, the Android settings to
change (HyperOS/MIUI take some gestures for themselves), installing apps, and the self-test.

## How it works

- **No proot.** Programs run under a patched glibc loader, with `fakechroot` (made thread-safe) and `fakeroot` over
  TCP for the root filesystem view. A preloaded library (`native/android-compat.c`) covers what Android forbids to
  apps: blocked syscalls, hard links, `O_TMPFILE`, `/proc/self/exe`. Programs that make system calls themselves (Go,
  Bun, Free Pascal) get the same translation through a seccomp trap.
- **Display:** Hyprland on an Android backend for aquamarine. The app shows the output as an Android surface and
  forwards touch, keyboard, mouse and the clipboard.
- **GPU:** Mesa with KGSL support (no `/dev/dri` needed); XWayland is GPU-accelerated too.
- **Flatpak** without user namespaces: sandboxes are built as trees of links (`native/bwrap-flatpak.py`), with a
  document portal and `flatpak-spawn` stand-ins.
- **Updates are safe:** `pacman -Syu` works. Pacman hooks keep our builds (Mesa, Hyprland, aquamarine, fakechroot) in
  place across updates.

## Known limits

- Flatpak apps are not isolated from each other: Android gives apps no way to build real sandboxes.
- YouTube in Brave decodes video on the processor (hardware decoding works in `mpv` and `ffmpeg`).
- Old games that change the screen resolution (Allegro 4 and similar) are drawn full-screen but don't receive taps
  yet. Play them in windowed mode.
- There is no real administrator account: `sudo` installs and configures Linux software but cannot change Android.

## Building

`build/build-rootfs-native.sh` builds the system image on the device itself, inside a running Andromarchy, with
`build/build-inside.sh` (Mesa, XWayland, Hyprland, Squeekboard, fakechroot, Omarchy). The APK builds with Gradle in
`apk/`. The patches to upstream projects are in `patches/`.

## License and credits

GPL-3.0 (see [LICENSE](LICENSE)). The Android app is based on
[lfdevs/anland-termux](https://github.com/lfdevs/anland-termux) (GPL-3.0). Andromarchy uses lfdevs's Mesa for
Android, termux-va and XWayland patches, and builds on [Omarchy](https://github.com/basecamp/omarchy), Hyprland,
Arch Linux ARM, Mesa, Squeekboard, hyprgrass, PipeWire, Flatpak and fakechroot.
