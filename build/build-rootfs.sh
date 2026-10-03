#!/bin/bash
# Builds the Lotfix rootfs (Arch Linux ARM + patched Hyprland + Omarchy + no-proot runtime) on an x86 PC,
# through qemu-user binfmt (Debian/Ubuntu or WSL, as root). Output: out/lotfix-rootfs.tar.gz.
# The device-specific part (Android uid in /etc/passwd) is done on the tablet by native/setup-rootfs.sh.
# Needs $OUT/lotfix-hyprland-aarch64.tar.zst from build-hyprland.sh, run natively on the tablet (hours under qemu).
# Usage: sudo [LOTFIX_OUT=dir] build/build-rootfs.sh [workdir]
set -euo pipefail

# Pinned inputs: bump deliberately, the patches target these versions.
ALARM_URL=http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
MESA_URL=https://github.com/lfdevs/mesa-for-android-container/releases/download/mesa-26.3.0-devel-20260824/mesa-for-android-container_26.3.0-devel-20260824_archlinux_arm64.tar
FAKEROOT_URL=https://deb.debian.org/debian/pool/main/f/fakeroot/fakeroot_1.37.1.1.orig.tar.gz
OMARCHY_REF=${OMARCHY_REF:-} # empty: the repo's default branch

HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-/var/tmp/lotfix-build}
R=$W/rootfs
OUT=${LOTFIX_OUT:-$HERE/out}
mkdir -p "$W" "$OUT"

# 1. Host tools + aarch64 binfmt (WSL has no systemd: register by hand).
apt-get install -y -qq qemu-user-static binfmt-support curl libarchive-tools zstd python3 >/dev/null
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || update-binfmts --enable qemu-aarch64

# 2. Fresh Arch Linux ARM base.
if [ ! -e "$R/etc/arch-release" ]; then
  curl -fL -o "$W/alarm.tar.gz" -z "$W/alarm.tar.gz" "$ALARM_URL"
  rm -rf "$R" && mkdir -p "$R"
  bsdtar -xpf "$W/alarm.tar.gz" -C "$R"
fi

# 3. Inputs the chroot needs: our patches, scripts and the pinned downloads.
mkdir -p "$R/tmp/lotfix"
cp -r "$HERE/patches" "$HERE/omarchy" "$HERE/native" "$R/tmp/lotfix/"
cp "$OUT/lotfix-hyprland-aarch64.tar.zst" "$R/tmp/lotfix/"
curl -fsSL -o "$R/tmp/lotfix/mesa.tar" -z "$R/tmp/lotfix/mesa.tar" "$MESA_URL"
curl -fsSL -o "$R/tmp/lotfix/fakeroot.tar.gz" -z "$R/tmp/lotfix/fakeroot.tar.gz" "$FAKEROOT_URL"

cleanup() { for m in dev/pts dev proc sys; do umount -l "$R/$m" 2>/dev/null || true; done; }
trap 'rc=$?; cleanup; exit $rc' EXIT
mount -t proc proc "$R/proc"
# rslave: unmounting these later must not propagate back and unmount the host's own /dev and /sys.
mount --rbind /sys "$R/sys" && mount --make-rslave "$R/sys"
mount --rbind /dev "$R/dev" && mount --make-rslave "$R/dev"
rm -f "$R/etc/resolv.conf" && cp -L /etc/resolv.conf "$R/etc/resolv.conf"

# 4. Everything else runs inside the aarch64 rootfs.
cat > "$R/tmp/lotfix/inside.sh" <<EOF
#!/bin/bash
set -euo pipefail
export OMARCHY_REF=$OMARCHY_REF
exec bash /tmp/lotfix/build-inside.sh
EOF
cp "$HERE/build/build-inside.sh" "$R/tmp/lotfix/build-inside.sh"
chroot "$R" /bin/bash /tmp/lotfix/inside.sh

# 5. Host-side fixups, then pack. Hard links become plain files: Android's SELinux forbids creating them.
bash "$HERE/native/relativize-symlinks.sh" "$R" 2>/dev/null || true
cleanup; trap - EXIT
rm -rf "$R/tmp/lotfix" "$R/var/cache/pacman/pkg/"* "$R/etc/resolv.conf"
tar --numeric-owner --hard-dereference -C "$R" -I 'gzip -6' -cf "$OUT/lotfix-rootfs.tar.gz" .
sha256sum "$OUT/lotfix-rootfs.tar.gz" | tee "$OUT/lotfix-rootfs.tar.gz.sha256"
