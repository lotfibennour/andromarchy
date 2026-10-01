#!/data/data/com.termux/files/usr/bin/bash
# Runs a command from the Arch rootfs WITHOUT proot:
#  - Termux's glibc (patched for Android seccomp) is the loader for Arch's binaries,
#  - fakechroot (LD_PRELOAD) makes the rootfs look like /,
#  - every exec goes back through the same loader (FAKECHROOT_ELFLOADER).
# Usage: arch-native.sh [bash args...]   e.g. arch-native.sh -c 'cat /etc/os-release'
unset LD_PRELOAD # Termux's libtermux-exec.so is a Bionic library, glibc cannot load it

R=${ARCH_ROOTFS:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/archlinux/rootfs}
G=/data/data/com.termux/files/usr/glibc/lib
FC=$R/usr/lib/libfakeroot/fakechroot/libfakechroot.so

export FAKECHROOT_BASE=$R
# No FAKECHROOT_ELFLOADER_OPT_ARGV0: with it, fakechroot 2.20.1 drops the script path when exec'ing a #! script.
export FAKECHROOT_ELFLOADER=$G/ld-linux-aarch64.so.1
export FAKECHROOT_EXCLUDE_PATH=/dev:/proc:/sys
export LD_LIBRARY_PATH=$G:$R/usr/lib LD_PRELOAD=$FC
export PATH=/usr/local/bin:/usr/bin HOME=/root

exec "$G/ld-linux-aarch64.so.1" --library-path "$G:$R/usr/lib" --preload "$FC" "$R/usr/bin/bash" "$@"
