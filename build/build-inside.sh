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
  fakechroot fakeroot firefox kitty foot alacritty \
  mesa-utils vulkan-tools vulkan-headers wayland-utils ttf-dejavu htop ncdu tree bc


# fakeroot over TCP: Android has no SysV IPC.
mkdir -p /tmp/fr && tar -xzf $L/fakeroot.tar.gz -C /tmp/fr --strip-components=1
(cd /tmp/fr && ./configure --prefix=/usr/local --with-ipc=tcp && make -j"$(nproc)" && make install)

# aquamarine (Anland backend) + Hyprland (our patches), compiled natively by build-hyprland.sh: over Arch's copies.
tar -xf $L/lotfix-hyprland-aarch64.tar.zst -C /
# Only now: ignored earlier, pacman could not resolve them as hyprland dependencies.
grep -q '^IgnorePkg.*hyprland' $C || sed -i "/^IgnorePkg/ s/$/ aquamarine hyprland/" $C

# Omarchy.
rm -rf /tmp/omarchy
git clone -q --depth 1 ${OMARCHY_REF:+-b "$OMARCHY_REF"} https://github.com/basecamp/omarchy /tmp/omarchy
git -C /tmp/omarchy log -1 --format='omarchy %H %cd' > /etc/lotfix-omarchy-version
bash $L/omarchy/install-omarchy-android.sh /tmp/omarchy

# No-proot runtime, kept in the rootfs so it always matches its glibc: the seccomp-patched loader + libc,
# and android-compat.so (preloaded into every process by arch-native.sh).
python3 $L/native/patch-glibc-syscalls.py /usr/lib /opt/lotfix/arch-glibc
gcc -O2 -shared -fPIC -o /opt/lotfix/android-compat.so $L/native/android-compat.c $L/native/go-syscalls.c -ldl
# glibc reads locales internally from LOCPATH, which ignores locale-archive: one directory per locale.
localedef --no-archive -i en_US -f UTF-8 /usr/lib/locale/en_US.UTF-8

rm -rf /tmp/mesa /tmp/fr /tmp/omarchy
echo "lotfix rootfs built"
