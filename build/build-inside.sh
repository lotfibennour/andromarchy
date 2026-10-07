#!/bin/bash
# Runs inside the aarch64 rootfs, called by build-rootfs.sh (PC: chroot + qemu) or build-rootfs-native.sh (on the
# tablet: through our own runtime as fake root, LOTFIX_NATIVE=1, no chroot/qemu/proot). Inputs in /tmp/lotfix.
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
rm -rf /etc/pacman.d/gnupg # a keyring left by an interrupted run
pacman-key --init
pacman-key --populate archlinuxarm
# Android runs its own kernel: drop the kernel, firmware and initramfs tooling (over 1 GB).
pacman -Rdd --noconfirm linux-aarch64 $(pacman -Qq | grep -E '^linux-firmware') mkinitcpio 2>/dev/null || true
pacman -Syu --noconfirm
if [ -n "${LOTFIX_NATIVE:-}" ]; then
  # Native: the running loader + libc are the patched copies in /opt/lotfix/arch-glibc, refreshed now from the
  # glibc just installed (the pacman hook doing it on later upgrades is installed below).
  pacman -S --noconfirm --needed python
  python3 $L/native/patch-glibc-syscalls.py /usr/lib /opt/lotfix/arch-glibc
fi
# Mesa with the KGSL backend (Adreno without /dev/dri), from lfdevs/mesa-for-android-container. First: with Arch's
# mesa in IgnorePkg, pacman would otherwise satisfy GL dependencies with mesa-amber, which conflicts with it.
mkdir -p /tmp/mesa && tar -xf $L/mesa.tar -C /tmp/mesa
pacman -U --noconfirm /tmp/mesa/*.pkg.tar.*
# The rootfs validated on the tablet (pacman -Qqe), minus debugging-only tools. Omarchy's own list is in
# omarchy/install-omarchy-android.sh; hyprland/aquamarine come from Arch for their dependencies, then get replaced.
# --overwrite: the native build copied fakechroot's library in beforehand, to start this rootfs.
pacman -S --noconfirm --needed --overwrite '/usr/lib/libfakeroot/fakechroot/*' \
  base-devel git cmake ninja pkgconf python autoconf automake libtool po4a \
  hyprland hyprwayland-scanner wayland-protocols glaze cli11 hwdata libdisplay-info libinput seatd pixman \
  fakechroot fakeroot kitty foot alacritty python-gobject jq brightnessctl gnome-keyring libsecret flatpak gnome-software nautilus squeekboard \
  mesa-utils libva-utils vulkan-tools vulkan-headers wayland-utils ttf-dejavu htop ncdu tree bc glm meson


# fakeroot over TCP: Android has no SysV IPC.
rm -rf /tmp/fr && mkdir -p /tmp/fr && tar -xzf $L/fakeroot.tar.gz -C /tmp/fr --strip-components=1
# A forked child gets a fresh lock and its own faked connection (cargo deadlocked at fork under sudo).
for p in $L/patches/fakeroot/*.patch; do patch -d /tmp/fr -p1 < "$p"; done
(cd /tmp/fr && ./configure --prefix=/usr/local --with-ipc=tcp && make -j"$(nproc)" && make install)

# aquamarine (Andromarchy backend) + Hyprland (our patches), compiled natively by build-hyprland.sh: over Arch's copies.
tar -xf $L/lotfix-hyprland-aarch64.tar.zst -C /
# Only now: ignored earlier, pacman could not resolve them as hyprland dependencies.
grep -q '^IgnorePkg.*hyprland' $C || sed -i "/^IgnorePkg/ s/$/ aquamarine hyprland/" $C

# hyprgrass (touchscreen gestures), at the commit hyprgrass pins for our Hyprland (hyprpm.toml: v0.56.2).
rm -rf /tmp/hyprgrass && git clone -q https://github.com/horriblename/hyprgrass /tmp/hyprgrass && git -C /tmp/hyprgrass checkout -q 8e605468cbf429791d61ddbfaeb14ff5e08fec3e
(cd /tmp/hyprgrass && meson setup build -Dbuildtype=release && ninja -C build)
install -Dm755 /tmp/hyprgrass/build/src/libhyprgrass.so /usr/lib/hyprland-plugins/libhyprgrass.so

# Squeekboard on the overlay layer (above fullscreen windows) + current Rust's close() signature: our build in
# /opt/lotfix (pacman never touches it), started by /usr/local/bin/squeekboard. Rust only for this build.
pacman -S --noconfirm --needed rust glib2-devel
rm -rf /root/.cargo/registry/src # sources left half-unpacked by an interrupted run
rm -rf /tmp/squeekboard && git clone -q --depth 1 -b v1.43.1 https://gitlab.gnome.org/World/Phosh/squeekboard.git /tmp/squeekboard
git -C /tmp/squeekboard apply $L/patches/squeekboard/*.patch
(cd /tmp/squeekboard && meson setup build --buildtype=release -Dtests=false && ninja -C build)
install -Dm755 /tmp/squeekboard/build/src/squeekboard /opt/lotfix/squeekboard
pacman -Rns --noconfirm rust


# Omarchy.
rm -rf /tmp/omarchy
rm -rf /tmp/omarchy && git clone -q --depth 1 ${OMARCHY_REF:+-b "$OMARCHY_REF"} https://github.com/basecamp/omarchy /tmp/omarchy
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

# (after fix-abs-needed: autoreconf runs perl, which could not load libperl.so before)
# fakechroot: Arch's package rebuilt with our patch (a dirfd's directory without fchdir: threads resolved paths against
# each other's, Flatpak runtime installs failed). Arch's recipe and patches; git apply (patch -i stopped after the first
# file here). Only the library replaced (a rename: this build runs on it); andromarchy-protect keeps it across updates.
rm -rf /tmp/fc && git clone -q --depth 1 https://gitlab.archlinux.org/archlinux/packaging/packages/fakechroot.git /tmp/fc
curl -fsSL https://github.com/dex4er/fakechroot/archive/2.20.1.tar.gz | tar -xz -C /tmp/fc
(cd /tmp/fc/fakechroot-2.20.1 && for p in ../statx.patch ../fakechroot-{2,3,4,5}.patch $L/patches/fakechroot/*.patch; do git apply "$p"; done &&
  autoreconf -fi && ./configure --prefix=/usr --sbindir=/usr/bin --libdir=/usr/lib/libfakeroot --sysconfdir=/etc && make -j"$(nproc)" &&
  cp src/.libs/libfakechroot.so /usr/lib/libfakeroot/fakechroot/.libfakechroot.so.new &&
  mv /usr/lib/libfakeroot/fakechroot/.libfakechroot.so.new /usr/lib/libfakeroot/fakechroot/libfakechroot.so)
# Omarchy's "Install" menus use sudo and yay. No real root on Android: sudo runs commands as fake root
# (native/sudo-shim.sh); yay is the upstream static aarch64 release (Go: works through go-syscalls).
install -m 755 $L/native/sudo-shim.sh /usr/local/bin/sudo
# bubblewrap needs user namespaces (denied to apps): GTK's image loaders (glycin) and others run through it.
install -m 755 $L/native/bwrap-shim.sh /usr/local/bin/bwrap
# pkexec (polkit, setuid) -> the same fake root as sudo.
install -m 755 $L/native/pkexec-shim.sh /usr/local/bin/pkexec

# Browser: Brave Origin (AUR binary release, aarch64), instead of Firefox. makepkg refuses to run as root:
# - native (fake root): its dependencies installed here (we are root), then makepkg without the fake root (the
#   real app uid underneath) and without -s (no sudo needed);
# - PC chroot (real root): a throwaway user.
rm -rf /tmp/brave && git clone -q https://aur.archlinux.org/brave-origin-bin.git /tmp/brave
if [ -n "${LOTFIX_NATIVE:-}" ]; then
  pacman -S --noconfirm --needed $(cd /tmp/brave && source ./PKGBUILD && echo "${depends[@]}")
  # makepkg packages under Arch's fakeroot, which takes our fakechroot (its path holds "libfakeroot") for a nested
  # fakeroot and stops. Owners do not matter here: a fakeroot that just runs the command.
  mkdir -p /tmp/nofakeroot && printf '#!/bin/sh\n[ "$1" = -- ] && shift\nexec "$@"\n' > /tmp/nofakeroot/fakeroot
  chmod 755 /tmp/nofakeroot/fakeroot
  (cd /tmp/brave && env -u FAKEROOTKEY PATH=/tmp/nofakeroot:$PATH LD_PRELOAD="$(printf '%s' "$LD_PRELOAD" |
    tr ':' '\n' | grep -v libfakeroot.so | paste -sd:)" makepkg --noconfirm 2>&1 | tail -3)
  rm -rf /tmp/nofakeroot
else
  useradd -m aurbuild 2>/dev/null || true
  chown -R aurbuild /tmp/brave
  (cd /tmp/brave && runuser -u aurbuild -- makepkg --noconfirm -s 2>&1 | tail -3)
  userdel -r aurbuild 2>/dev/null || true
fi
pacman -U --noconfirm /tmp/brave/brave-origin-bin-*.pkg.tar.*
rm -rf /tmp/brave
xdg-settings set default-web-browser brave-origin.desktop || true # Omarchy's install ran before it existed
# No udev daemon on Android: udevadm control/trigger/settle succeed as no-ops.
install -m 755 $L/native/udevadm-shim.sh /usr/local/bin/udevadm
# The session's own system bus (SessionService) and the UPower stand-in it carries (battery from Android).
install -m 644 $L/native/system-bus.conf /opt/lotfix/system-bus.conf
install -m 755 $L/native/android-upower.py /usr/local/bin/android-upower
install -m 755 $L/native/android-nm.py /usr/local/bin/android-nm
# The other services a desktop expects on D-Bus (tools/dbus-probe.sh): system and session stand-ins.
install -m 755 $L/native/bwrap-shim.sh -D /usr/local/libexec/bwrap-legacy
install -m 755 $L/native/bwrap-flatpak.py /usr/local/bin/bwrap  # Flatpak apps (trees + runtime glibc); others: legacy
install -Dm 755 $L/native/flatpak-spawn.sh /usr/local/libexec/andromarchy-flatpak/flatpak-spawn # nested sandboxes, run directly
install -m 755 $L/native/flatpak-fix-links.py /opt/lotfix/flatpak-fix-links.py
# Flatpak runs bwrap from /usr/bin with an empty environment (no PATH): the stand-in there too, kept on updates.
ln -sf /usr/local/bin/bwrap /usr/bin/bwrap
grep -q "^NoExtract.*usr/bin/bwrap" /etc/pacman.conf ||
  sed -i '/^\[options\]/a # Andromarchy: bubblewrap cannot run on Android (no user namespaces); /usr/bin/bwrap is our stand-in\nNoExtract = usr/bin/bwrap' /etc/pacman.conf
install -m 755 $L/native/android-system-services.py /usr/local/bin/android-system-services
install -m 755 $L/native/android-session-services.py /usr/local/bin/android-session-services
# Linux -> Android requests (Wi-Fi panel, launching Android apps): the app watches run/android-request.
install -m 755 $L/native/android-request.sh /usr/local/bin/android-request
# First boot on the device (SessionService runs it: uid, machine-id, locale, setup form answers) and first session.
install -m 755 $L/native/first-boot.sh /opt/lotfix/first-boot.sh
install -m 755 $L/native/first-session.sh /opt/lotfix/first-session.sh
# Edit menu on a long touch press (the app writes to run/longpress).
# No uwsm: "uwsm stop" (log out) -> a fresh session through the app.
install -m 755 $L/native/uwsm-shim.sh /usr/local/bin/uwsm
curl -fsSL -o /tmp/yay.tgz https://github.com/Jguer/yay/releases/download/v13.0.1/yay_13.0.1_aarch64.tar.gz
echo "75bc500c8677d6760f51117ae0a61689e9cf165bea3c4800825a5c879d030726  /tmp/yay.tgz" | sha256sum -c --quiet
tar xzf /tmp/yay.tgz -C /tmp && install -m 755 /tmp/yay_13.0.1_aarch64/yay /usr/local/bin/yay && rm -rf /tmp/yay*

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

# Our Mesa (KGSL + termux-va VA bridge), built here against the repo's current LLVM: the prebuilt packages above
# only provide pacman's view of "mesa" (dependencies); their libraries are replaced by this build. A prebuilt Mesa
# pinned an LLVM that Arch dropped (llvm-libs 23 removed libLLVM.so.22.1: the session did not start).
pacman -S --noconfirm --needed llvm clang libclc spirv-llvm-translator glslang cbindgen python-mako python-yaml   python-packaging python-ply byacc bison flex libva wayland-protocols libxrandr libxshmfence libxxf86vm libglvnd
rm -rf /tmp/mesa-src && git clone -q -b test/add-va-bridge https://github.com/lfdevs/mesa-for-android-container /tmp/mesa-src
git -C /tmp/mesa-src checkout -q ef25369b9c
# The validated build's options, every one that differs from Mesa's defaults (termux-va: hardware video).
(cd /tmp/mesa-src && meson setup build --prefix=/usr --buildtype=release -Dplatforms=x11,wayland \
  -Dgallium-drivers=freedreno,zink,virgl,llvmpipe -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
  -Dgallium-va=enabled -Dtermux-va-bridge=enabled -Dtermux-va-wayland-shim=enabled -Dvideo-codecs=all \
  -Degl=enabled -Dglvnd=enabled -Dglx=dri -Dgles1=disabled -Dgles2=enabled -Dgallium-mediafoundation=disabled \
  -Dintel-rt=disabled -Dmicrosoft-clc=disabled -Dvalgrind=disabled -Dlibunwind=disabled && ninja -C build)
# Protected: every file the install writes, links made by its scripts too (the DRI driver names): newer than this.
touch /tmp/mesa-install.stamp && sleep 1 && ninja -C /tmp/mesa-src/build install
mkdir -p /opt/lotfix/protect && find /usr -newer /tmp/mesa-install.stamp \( -type f -o -type l \) > /opt/lotfix/protect/mesa.list
rm -rf /tmp/mesa-src

# XWayland with GPU acceleration without /dev/dri (glamor + DRI3 on KGSL surfaceless): lfdevs's patches
# (lfdevs/xwayland, test/support-xwayland-surfaceless) on the official 24.1.13. In /usr/local/bin, first in PATH:
# Arch's xorg-xwayland stays as installed (pacman never touches ours).
pacman -S --noconfirm --needed xtrans xorgproto libxkbfile xorg-font-util libxfont2 libxcvt libdecor libtirpc libei
rm -rf /tmp/xwayland && git clone -q --depth 1 -b xwayland-24.1.13 https://gitlab.freedesktop.org/xorg/xserver.git /tmp/xwayland
for p in $L/patches/xwayland/*.patch; do git -C /tmp/xwayland apply "$p"; done
(cd /tmp/xwayland && meson setup build --prefix=/usr --buildtype=release -Dxvfb=false -Dxdmcp=false -Dglamor=true   -Dxkb_dir=/usr/share/X11/xkb -Dxkb_output_dir=/var/lib/xkb && ninja -C build)
install -m 755 /tmp/xwayland/build/hw/xwayland/Xwayland /usr/local/bin/Xwayland
rm -rf /tmp/xwayland

# Build-only tools out of the image (~250 MB): Mesa's compilers, cargo's cache. llvm-libs (Mesa's runtime) stays.
pacman -Rns --noconfirm clang llvm libclc spirv-llvm-translator cbindgen 2>/dev/null || true
rm -rf /root/.cargo /tmp/mesa-install.stamp

# Protection of our own builds across pacman transactions (native/andromarchy-protect.py): a record of every
# protected file; the hook puts them back after any install/upgrade/removal.
install -m 755 $L/native/andromarchy-protect.py /usr/local/bin/andromarchy-protect
install -m 755 $L/native/android-clipboard.sh /usr/local/bin/android-clipboard
install -m 755 $L/native/andromarchy-selftest.sh /usr/local/bin/andromarchy-selftest
install -Dm 644 $L/native/zz-andromarchy-protect.hook /etc/pacman.d/hooks/zz-andromarchy-protect.hook
install -Dm 644 $L/native/00-andromarchy-protect-save.hook /etc/pacman.d/hooks/00-andromarchy-protect-save.hook
install -m 755 $L/native/andromarchy-keep-llvm.sh /usr/local/bin/andromarchy-keep-llvm
install -Dm 644 $L/native/00-andromarchy-keep-llvm.hook /etc/pacman.d/hooks/00-andromarchy-keep-llvm.hook
andromarchy-protect save

rm -rf /tmp/mesa /tmp/fr /tmp/omarchy
echo "lotfix rootfs built"
