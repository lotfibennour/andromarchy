#!/system/bin/sh
# Builds the Andromarchy rootfs ON THE TABLET, natively (aarch64): no PC, no qemu, no chroot, no proot. Our own
# runtime (lotfix-run: fakechroot + fake root + android-compat) runs everything, in two rootfs:
#   - the installed one (the app's files/rootfs): downloads, Hyprland build, unpacking, packing;
#   - the new one (being built): build-inside.sh, started directly with lotfix-run (an Android program: it cannot
#     be exec'd from inside a rootfs, hence this driver in Android's shell).
# Run as the app:   run-as andro.marchy sh build-rootfs-native.sh <repo dir in the app's files>
# Output: files/build/out/lotfix-rootfs.tar.zst (+ .sha256), files/build/out/lotfix-hyprland-aarch64.tar.zst
set -eu
APP=/data/data/andro.marchy/files
R=$APP/rootfs                       # the installed rootfs (host path)
B=$APP/rootfs/var/tmp/lotfix-build  # work dir, inside it so its tools reach it (guest: /var/tmp/lotfix-build)
G=/var/tmp/lotfix-build             # ...and its guest path
N=$B/rootfs                         # the new rootfs (host path)
REPO=${1:?repo dir (patches/, omarchy/, native/, build/), as a path inside the installed rootfs}
LAUNCHER=$(dirname "$(pm path andro.marchy | cut -d: -f2)")/lib/arm64/liblotfixrun.so
RUN=$APP/run

ALARM_URL=http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
MESA_URL=https://github.com/lfdevs/mesa-for-android-container/releases/download/mesa-26.3.0-devel-20260824/mesa-for-android-container_26.3.0-devel-20260824_archlinux_arm64.tar
FAKEROOT_URL=https://deb.debian.org/debian/pool/main/f/fakeroot/fakeroot_1.37.1.1.orig.tar.gz

inside_installed() { # runs a command in the installed rootfs, as fake root
  ARCH_ROOTFS=$R ARCH_ROOT=1 ARCH_EXCLUDE_EXTRA=$RUN XDG_RUNTIME_DIR=$RUN "$LAUNCHER" -c "$1"
}

echo "== 1/3 inputs, Hyprland, base system (installed rootfs)"
inside_installed "set -euo pipefail
  mkdir -p $G/dl $G/out
  cd $G/dl
  curl -fsSL -z alarm.tar.gz -o alarm.tar.gz $ALARM_URL
  curl -fsSL -z mesa.tar -o mesa.tar $MESA_URL
  curl -fsSL -z fakeroot.tar.gz -o fakeroot.tar.gz $FAKEROOT_URL
  # Hyprland + aquamarine with our patches (skipped when already built).
  [ -f $G/out/lotfix-hyprland-aarch64.tar.zst ] || bash $REPO/build/build-hyprland.sh $REPO/patches $G/out
  rm -rf $G/rootfs && mkdir -p $G/rootfs
  # Its absolute symlinks mean the new root (android-compat writes them relative to it).
  ANDROID_COMPAT_SYMLINK_ROOT=$G/rootfs tar -xpf alarm.tar.gz -C $G/rootfs
  # Our runtime, so lotfix-run can start the new rootfs: patched loader + libc, android-compat, fakechroot,
  # fake root (TCP faked). build-inside.sh refreshes the patched glibc from the new rootfs's own.
  mkdir -p $G/rootfs/opt $G/rootfs/usr/lib/libfakeroot $G/rootfs/usr/local/bin $G/rootfs/usr/local/lib
  cp -a /opt/lotfix $G/rootfs/opt/
  cp -a /usr/lib/libfakeroot/fakechroot $G/rootfs/usr/lib/libfakeroot/
  cp -a /usr/local/bin/faked $G/rootfs/usr/local/bin/
  cp -a /usr/local/lib/libfakeroot* $G/rootfs/usr/local/lib/
  # Inputs of build-inside.sh.
  mkdir -p $G/rootfs/tmp/lotfix
  cp -r $REPO/patches $REPO/omarchy $REPO/native $G/rootfs/tmp/lotfix/
  cp $REPO/build/build-inside.sh $G/rootfs/tmp/lotfix/
  cp mesa.tar fakeroot.tar.gz $G/out/lotfix-hyprland-aarch64.tar.zst $G/rootfs/tmp/lotfix/
  rm -f $G/rootfs/etc/resolv.conf # a link to systemd-resolved's stub, absent here
  printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > $G/rootfs/etc/resolv.conf"

echo "== 2/3 build-inside.sh (new rootfs, native)"
ARCH_ROOTFS=$N ARCH_ROOT=1 ARCH_EXCLUDE_EXTRA=$RUN XDG_RUNTIME_DIR=$RUN "$LAUNCHER" \
  -c "export LOTFIX_NATIVE=1 OMARCHY_REF=${OMARCHY_REF:-}; bash /tmp/lotfix/build-inside.sh"

echo "== 3/3 pack (installed rootfs)"
inside_installed "set -euo pipefail
  rm -rf $G/rootfs/tmp/lotfix $G/rootfs/var/cache/pacman/pkg/* $G/rootfs/etc/resolv.conf
  rm -f $G/rootfs/etc/machine-id && touch $G/rootfs/etc/machine-id # first-boot.sh makes this device's
  tar --numeric-owner --hard-dereference -C $G/rootfs -I 'zstd -T0 -19' -cf $G/out/lotfix-rootfs.tar.zst .
  cd $G/out && sha256sum lotfix-rootfs.tar.zst | tee lotfix-rootfs.tar.zst.sha256"
echo "done: $B/out"
