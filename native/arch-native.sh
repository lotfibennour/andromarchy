#!/data/data/com.termux/files/usr/bin/bash
# Runs a command from the Arch rootfs WITHOUT proot:
#  - Termux's glibc (patched for Android seccomp) is the loader for Arch's binaries,
#  - fakechroot (LD_PRELOAD) makes the rootfs look like /,
#  - every exec goes back through the same loader (FAKECHROOT_ELFLOADER),
#  - with ARCH_ROOT=1, fakeroot (TCP build in /usr/local, Android has no SysV IPC) fakes uid 0 and file owners.
# Usage: [ARCH_ROOT=1] arch-native.sh [bash args...]   e.g. ARCH_ROOT=1 arch-native.sh -c 'pacman -S htop'
unset LD_PRELOAD # Termux's libtermux-exec.so is a Bionic library, glibc cannot load it

R=${ARCH_ROOTFS:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/archlinux/rootfs}
G=/data/data/com.termux/files/usr/glibc/lib
FC=$R/usr/lib/libfakeroot/fakechroot/libfakechroot.so
FR=$R/usr/local/lib/libfakeroot.so
LOADER="$G/ld-linux-aarch64.so.1 --library-path $G:$R/usr/lib"

export FAKECHROOT_BASE=$R
# No FAKECHROOT_ELFLOADER_OPT_ARGV0: with it, fakechroot 2.20.1 drops the script path when exec'ing a #! script.
export FAKECHROOT_ELFLOADER=$G/ld-linux-aarch64.so.1
export FAKECHROOT_EXCLUDE_PATH=/dev:/proc:/sys
export LD_LIBRARY_PATH=$G:$R/usr/lib
export PATH=/usr/local/bin:/usr/bin HOME=/root
PRELOAD=$FC

if [ "${ARCH_ROOT:-0}" = 1 ]; then
  # faked prints "<key>:<pid>"; the ownership database persists across sessions in the rootfs.
  STATE=$R/var/lib/fakeroot.state
  : >> "$STATE" # shell builtin: PATH already points at the rootfs here
  key_pid=$($LOADER "$R/usr/local/bin/faked" --load --save-file "$STATE" < "$STATE") || exit 1
  export FAKEROOTKEY=${key_pid%%:*}
  trap 'kill ${key_pid##*:} 2>/dev/null' EXIT
  PRELOAD=$FR:$FC # fakeroot above fakechroot, as in the classic "fakechroot fakeroot" stack
fi

export LD_PRELOAD=$PRELOAD
if [ "${ARCH_ROOT:-0}" = 1 ]; then
  $LOADER --preload "${PRELOAD/:/ }" "$R/usr/bin/bash" "$@"
else
  exec $LOADER --preload "$PRELOAD" "$R/usr/bin/bash" "$@"
fi
