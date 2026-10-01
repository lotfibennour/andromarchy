#!/bin/bash
# Builds aquamarine (Anland backend) and Hyprland (our patches) natively on an aarch64 Arch: on the tablet
# through "proot-distro login archlinux" (the no-proot runtime cannot run gcc yet: posix_spawn), in minutes
# instead of hours of qemu on a PC. Installs nothing on the running system: everything goes to a staging
# tree, packed as lotfix-hyprland-aarch64.tar.zst, which build-rootfs.sh unpacks into the rootfs.
# Usage (as root in Arch): build-hyprland.sh PATCHES_DIR OUT_DIR
set -euo pipefail
AQUAMARINE_TAG=v0.15.1
HYPRLAND_TAG=v0.56.2
P=${1:?patches dir}
OUT=${2:?out dir}
W=/tmp/lotfix-hl
S=$W/stage

pacman -S --noconfirm --needed git cmake ninja gcc pkgconf hyprwayland-scanner wayland-protocols glaze cli11 \
  hwdata libdisplay-info libinput seatd pixman zstd
rm -rf "$W" && mkdir -p "$W" "$OUT"

git clone -q --depth 1 -b $AQUAMARINE_TAG https://github.com/hyprwm/aquamarine $W/aquamarine
git -C $W/aquamarine apply "$P"/aquamarine/*.patch
cmake -S $W/aquamarine -B $W/aquamarine/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
DESTDIR=$S ninja -C $W/aquamarine/build install

# Hyprland must compile and link against OUR aquamarine (staging), not Arch's in /usr.
git clone -q --recursive --depth 1 -b $HYPRLAND_TAG https://github.com/hyprwm/Hyprland $W/Hyprland
git -C $W/Hyprland apply "$P"/hyprland/*.patch
# --define-prefix: the staged aquamarine.pc resolves to $S/usr instead of /usr (Arch's copy).
PKG_CONFIG_PATH=$S/usr/lib/pkgconfig PKG_CONFIG="pkgconf --define-prefix" \
  cmake -S $W/Hyprland -B $W/Hyprland/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_PREFIX_PATH=$S/usr
ninja -C $W/Hyprland/build
DESTDIR=$S ninja -C $W/Hyprland/build install

printf 'aquamarine %s\nhyprland %s\n' $AQUAMARINE_TAG $HYPRLAND_TAG > $S/usr/share/lotfix-hyprland-version
tar --numeric-owner -C $S -I 'zstd -T0 -19' -cf "$OUT/lotfix-hyprland-aarch64.tar.zst" .
echo "built $OUT/lotfix-hyprland-aarch64.tar.zst"
