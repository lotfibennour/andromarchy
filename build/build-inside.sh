#!/bin/bash
# Runs inside the aarch64 rootfs (chroot + qemu), called by build-rootfs.sh. Inputs in /tmp/lotfix.
set -euo pipefail
L=/tmp/lotfix
C=/etc/pacman.conf
export HOME=/root

# pacman: no Landlock under qemu/Android. Our Mesa, aquamarine and Hyprland must never be replaced by Arch's.
grep -q '^DisableSandbox' $C || sed -i '/^\[options\]/a DisableSandbox' $C
grep -q '^IgnorePkg.*mesa' $C ||
  sed -i '/^\[options\]/a IgnorePkg = mesa vulkan-freedreno vulkan-mesa-layers vulkan-mesa-implicit-layers' $C

# CheckSpace needs /etc/mtab, absent in a chroot and unreadable through fakechroot on Android.
sed -i 's/^CheckSpace/#CheckSpace/' $C

rm -f /var/lib/pacman/db.lck # left behind if a previous run was interrupted
pacman-key --init
pacman-key --populate archlinuxarm
# Android runs its own kernel: drop the kernel, firmware and initramfs tooling (over 1 GB).
pacman -Rdd --noconfirm linux-aarch64 $(pacman -Qq | grep -E '^linux-firmware') mkinitcpio 2>/dev/null || true
pacman -Syu --noconfirm
# Mesa with the KGSL backend (Adreno without /dev/dri), from lfdevs/mesa-for-android-container. First: with Arch's
# mesa in IgnorePkg, pacman would otherwise satisfy GL dependencies with mesa-amber, which conflicts with it.
mkdir -p /tmp/mesa && tar -xf $L/mesa.tar -C /tmp/mesa
pacman -U --noconfirm /tmp/mesa/*.pkg.tar.*
# The rootfs validated on the tablet (pacman -Qqe), minus debugging-only tools. Omarchy's own list is in
# omarchy/install-omarchy-android.sh; hyprland/aquamarine come from Arch for their dependencies, then get replaced.
pacman -S --noconfirm --needed \
  base-devel git cmake ninja pkgconf python autoconf automake libtool po4a \
  hyprland hyprwayland-scanner wayland-protocols glaze cli11 hwdata libdisplay-info libinput seatd pixman \
  fakechroot fakeroot kitty foot alacritty python-gobject jq brightnessctl \
  mesa-utils vulkan-tools vulkan-headers wayland-utils ttf-dejavu htop ncdu tree bc


# fakeroot over TCP: Android has no SysV IPC.
mkdir -p /tmp/fr && tar -xzf $L/fakeroot.tar.gz -C /tmp/fr --strip-components=1
(cd /tmp/fr && ./configure --prefix=/usr/local --with-ipc=tcp && make -j"$(nproc)" && make install)

# aquamarine (Anland backend) + Hyprland (our patches), compiled natively by build-hyprland.sh: over Arch's copies.
tar -xf $L/lotfix-hyprland-aarch64.tar.zst -C /
# Only now: ignored earlier, pacman could not resolve them as hyprland dependencies.
grep -q '^IgnorePkg.*hyprland' $C || sed -i "/^IgnorePkg/ s/$/ aquamarine hyprland/" $C

# Browser: Brave Origin (AUR binary release, aarch64), instead of Firefox. makepkg refuses root: a throwaway user.
useradd -m aurbuild 2>/dev/null || true
rm -rf /tmp/brave && runuser -u aurbuild -- git clone -q https://aur.archlinux.org/brave-origin-bin.git /tmp/brave &&
  (cd /tmp/brave && runuser -u aurbuild -- makepkg --noconfirm -s 2>&1 | tail -3) &&
  pacman -U --noconfirm /tmp/brave/brave-origin-bin-*.pkg.tar.*
rm -rf /tmp/brave; userdel -r aurbuild 2>/dev/null || true

# Omarchy.
rm -rf /tmp/omarchy
git clone -q --depth 1 ${OMARCHY_REF:+-b "$OMARCHY_REF"} https://github.com/basecamp/omarchy /tmp/omarchy
git -C /tmp/omarchy log -1 --format='omarchy %H %cd' > /etc/lotfix-omarchy-version
bash $L/omarchy/install-omarchy-android.sh /tmp/omarchy
# systemctl without systemd (user services, Restart=), ahead of systemd's own in /usr/local/bin.
bash $L/native/install-systemctl.sh

# No-proot runtime, kept in the rootfs so it always matches its glibc: the seccomp-patched loader + libc,
# and android-compat.so (preloaded into every process by arch-native.sh).
python3 $L/native/patch-glibc-syscalls.py /usr/lib /opt/lotfix/arch-glibc
# ...and again after every glibc upgrade (pacman hook), from a copy of the script kept in the rootfs.
install -m 644 $L/native/patch-glibc-syscalls.py /opt/lotfix/
install -Dm 644 $L/native/lotfix-glibc.hook /usr/share/libalpm/hooks/zz-lotfix-glibc.hook
# Absolute NEEDED entries (mpv -> /usr/lib/libmujs.so): fixed now, and for every later install by a pacman hook.
pacman -S --noconfirm --needed patchelf
install -m 755 $L/native/fix-abs-needed.sh /opt/lotfix/
install -Dm 644 $L/native/lotfix-abs-needed.hook /usr/share/libalpm/hooks/zz-lotfix-abs-needed.hook
/opt/lotfix/fix-abs-needed.sh --all
# Omarchy's "Install" menus use sudo and yay. No real root on Android: sudo runs commands as fake root
# (native/sudo-shim.sh); yay is the upstream static aarch64 release (Go: works through go-syscalls).
install -m 755 $L/native/sudo-shim.sh /usr/local/bin/sudo
# bubblewrap needs user namespaces (denied to apps): GTK's image loaders (glycin) and others run through it.
install -m 755 $L/native/bwrap-shim.sh /usr/local/bin/bwrap
# pkexec (polkit, setuid) -> the same fake root as sudo.
install -m 755 $L/native/pkexec-shim.sh /usr/local/bin/pkexec
# No udev daemon on Android: udevadm control/trigger/settle succeed as no-ops.
install -m 755 $L/native/udevadm-shim.sh /usr/local/bin/udevadm
# The session's own system bus (SessionService) and the UPower stand-in it carries (battery from Android).
install -m 644 $L/native/system-bus.conf /opt/lotfix/system-bus.conf
install -m 755 $L/native/android-upower.py /usr/local/bin/android-upower
install -m 755 $L/native/android-nm.py /usr/local/bin/android-nm
# Linux -> Android requests (Wi-Fi panel, launching Android apps): the app watches run/android-request.
install -m 755 $L/native/android-request.sh /usr/local/bin/android-request
# First boot on the device (SessionService runs it: uid, machine-id, locale, setup form answers) and first session.
install -m 755 $L/native/first-boot.sh /opt/lotfix/first-boot.sh
install -m 755 $L/native/first-session.sh /opt/lotfix/first-session.sh
# Edit menu on a long touch press (the app writes to run/longpress).
install -m 755 $L/native/longpress-menu.sh /usr/local/bin/andromarchy-longpress
curl -fsSL -o /tmp/yay.tgz https://github.com/Jguer/yay/releases/download/v13.0.1/yay_13.0.1_aarch64.tar.gz
echo "75bc500c8677d6760f51117ae0a61689e9cf165bea3c4800825a5c879d030726  /tmp/yay.tgz" | sha256sum -c --quiet
tar xzf /tmp/yay.tgz -C /tmp && install -m 755 /tmp/yay_13.0.1_aarch64/yay /usr/local/bin/yay && rm -rf /tmp/yay*

# On-screen keyboard follows text-field focus (Omarchy autostart runs it): a minimal input-method-v2 client.
(cd /tmp && wayland-scanner client-header $L/native/input-method-unstable-v2.xml input-method-unstable-v2-client-protocol.h &&
  wayland-scanner private-code $L/native/input-method-unstable-v2.xml input-method-unstable-v2-protocol.c &&
  gcc -O2 -I/tmp -o /usr/local/bin/osk-autoshow $L/native/osk-autoshow.c input-method-unstable-v2-protocol.c -lwayland-client)
# wtype (the on-screen keyboard's typing tool) with standard keys on their real evdev codes: Chromium/Electron read
# the physical code, and stock wtype's space arrived as code 1 = Escape.
rm -rf /tmp/wtype && git clone -q https://github.com/atx/wtype /tmp/wtype && git -C /tmp/wtype checkout -q v0.4 &&
  (cd /tmp/wtype && patch -p1 < $L/patches/wtype/0001-standard-keys-use-their-real-evdev-codes.patch &&
    X=protocol/virtual-keyboard-unstable-v1.xml &&
    wayland-scanner client-header $X virtual-keyboard-unstable-v1-client-protocol.h && wayland-scanner private-code $X vk.c &&
    gcc -O2 -I. -o /usr/local/bin/wtype main.c vk.c $(pkg-config --cflags --libs wayland-client xkbcommon)) && rm -rf /tmp/wtype
gcc -O2 -shared -fPIC -o /opt/lotfix/android-compat.so $L/native/android-compat.c $L/native/go-syscalls.c -ldl
# glibc reads locales internally from LOCPATH, which ignores locale-archive: one directory per locale.
localedef --no-archive -i en_US -f UTF-8 /usr/lib/locale/en_US.UTF-8

rm -rf /tmp/mesa /tmp/fr /tmp/omarchy
echo "lotfix rootfs built"
