#!/data/data/com.termux/files/usr/bin/bash
# Runs a command from the Arch rootfs WITHOUT proot:
#  - Arch's OWN glibc, with the syscalls Android's seccomp kills neutralized (patch-glibc-syscalls.py), is the loader,
#  - fakechroot (LD_PRELOAD) makes the rootfs look like /,
#  - every exec goes back through the same loader (FAKECHROOT_ELFLOADER),
#  - with ARCH_ROOT=1, fakeroot (TCP build in /usr/local, Android has no SysV IPC) fakes uid 0 and file owners.
# Usage: [ARCH_ROOT=1] arch-native.sh [bash args...]   e.g. ARCH_ROOT=1 arch-native.sh -c 'pacman -S htop'
unset LD_PRELOAD # Termux's libtermux-exec.so is a Bionic library, glibc cannot load it

R=${ARCH_ROOTFS:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/archlinux/rootfs}
# ld.so + libc.so.6 from Arch with set_robust_list/rseq patched to ENOSYS. (Termux's patched glibc 2.44 works for
# most programs, but Qt's QML JS engine aborts on it with heap corruption, so the rootfs's own glibc is used.)
G=/data/data/com.termux/files/home/native/arch-glibc
HOME_TERMUX=/data/data/com.termux/files/home
FC=$R/usr/lib/libfakeroot/fakechroot/libfakechroot.so
FR=$R/usr/local/lib/libfakeroot.so

export FAKECHROOT_BASE=$R
# No FAKECHROOT_ELFLOADER_OPT_ARGV0: with it, fakechroot 2.20.1 drops the script path when exec'ing a #! script.
export FAKECHROOT_ELFLOADER=$G/ld-linux-aarch64.so.1
# ARCH_EXCLUDE_EXTRA / ARCH_LIBPATH_EXTRA: host paths that must stay host paths (e.g. Termux's tmp with the
# Anland/Wayland sockets) and extra host library dirs searched first (e.g. our aquamarine build).
export FAKECHROOT_EXCLUDE_PATH=/dev:/proc:/sys${ARCH_EXCLUDE_EXTRA:+:$ARCH_EXCLUDE_EXTRA}
# We never use /etc/ld.so.cache (ldconfig is a no-op here), so the extra dirs packages declare in
# /etc/ld.so.conf.d (e.g. /usr/lib/libproxy for quickshell) go on the search path explicitly.
LIBDIRS=$R/usr/lib
for d in $(cat "$R"/etc/ld.so.conf.d/*.conf 2>/dev/null | grep -E '^/'); do
  [ -d "$R$d" ] && LIBDIRS=$LIBDIRS:$R$d
done
export LD_LIBRARY_PATH=${ARCH_LIBPATH_EXTRA:+$ARCH_LIBPATH_EXTRA:}$G:$LIBDIRS
LOADER="$G/ld-linux-aarch64.so.1 --library-path $LD_LIBRARY_PATH" # same libs for the first process and its children
export PATH=/usr/local/bin:/usr/bin HOME=/root

# GPU for every program, not only those Hyprland starts: Adreno through KGSL (no DRM node on Android).
# The ANLAND* variables are what make the KGSL Mesa expose dmabuf import/export, which Wayland GPU clients need.
# ARCH_GPU=zink runs OpenGL on top of Vulkan/Turnip instead of Freedreno's native GL driver.
export ANLAND=1 ANLAND_NO_DRM_DEVICE=1 TURNIP_KMD=kgsl FD_FORCE_KGSL=1 XWAYLAND_FORCE_KGSL_SURFACELESS=1
if [ "${ARCH_GPU:-freedreno}" = zink ]; then
  export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink
else
  export MESA_LOADER_DRIVER_OVERRIDE=kgsl GALLIUM_DRIVER=freedreno
fi

# android-compat.so: /proc/self/exe and /proc/*/fd links as seen from inside, shm_open without /dev/shm.
COMPAT=$HOME_TERMUX/native/android-compat.so
PRELOAD=$COMPAT:$FC
# A cwd outside the rootfs cannot be expressed inside it (getcwd fails): start in the fake $HOME.
cd "$R$HOME" || exit 1

if [ "${ARCH_ROOT:-0}" = 1 ]; then
  # faked prints "<key>:<pid>"; the ownership database persists across sessions in the rootfs.
  STATE=$R/var/lib/fakeroot.state
  : >> "$STATE" # shell builtin: PATH already points at the rootfs here
  key_pid=$($LOADER "$R/usr/local/bin/faked" --load --save-file "$STATE" < "$STATE") || exit 1
  export FAKEROOTKEY=${key_pid%%:*}
  trap 'kill ${key_pid##*:} 2>/dev/null' EXIT
  PRELOAD=$COMPAT:$FR:$FC # fakeroot above fakechroot, as in the classic "fakechroot fakeroot" stack
fi

export LD_PRELOAD=$PRELOAD
if [ "${ARCH_ROOT:-0}" = 1 ]; then
  $LOADER --preload "${PRELOAD//:/ }" "$R/usr/bin/bash" "$@"
else
  exec $LOADER --preload "${PRELOAD//:/ }" "$R/usr/bin/bash" "$@"
fi
