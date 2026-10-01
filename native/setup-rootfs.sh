#!/data/data/com.termux/files/usr/bin/bash
# Prepares an Arch Linux ARM rootfs to run WITHOUT proot (see arch-native.sh). Idempotent; rerun after
# large package changes (it re-relativizes new symlinks). Needs: Termux packages glibc-repo, glibc.
# The TCP fakeroot in /usr/local is built separately (fakeroot 1.37.x: ./configure --prefix=/usr/local --with-ipc=tcp).
set -euo pipefail
R=${1:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/archlinux/rootfs}
HERE=$(dirname "$(readlink -f "$0")")
C=$R/etc/pacman.conf

# pacman 7: no Landlock in Android kernels, no real setuid to "alpm".
grep -q '^DisableSandbox' "$C" || sed -i '/^\[options\]/a DisableSandbox' "$C"
# CheckSpace reads /etc/mtab through setmntent(), which opens it inside glibc, bypassing fakechroot.
sed -i 's/^CheckSpace/#CheckSpace  # disabled: setmntent() bypasses fakechroot (no proot)/' "$C"
# Keep the shims below across glibc upgrades.
grep -q '^NoExtract.*ldconfig' "$C" || sed -i '/^\[options\]/a NoExtract = usr/bin/ldconfig usr/bin/sln' "$C"
# The KGSL Mesa must not be replaced by Arch's (Hyprland and GPU clients depend on it).
grep -q '^IgnorePkg.*mesa' "$C" || sed -i '/^\[options\]/a IgnorePkg = mesa vulkan-freedreno vulkan-mesa-layers vulkan-mesa-implicit-layers' "$C"

# The only static binaries in the rootfs: they embed Arch's glibc and die on Android's seccomp (SIGSYS).
# ldconfig is useless here (the loader uses --library-path, never /etc/ld.so.cache); sln is a static "ln -s".
for b in ldconfig sln; do
  [ -e "$R/usr/bin/$b.static" ] || mv "$R/usr/bin/$b" "$R/usr/bin/$b.static"
done
printf '#!/bin/sh\n# Android/no-proot: static glibc binaries die on seccomp, and the loader uses --library-path,\n# never /etc/ld.so.cache. Nothing to do.\nexit 0\n' > "$R/usr/bin/ldconfig"
printf '#!/bin/sh\n# Android/no-proot: static sln dies on seccomp; same job as ln -s.\nexec ln -sf "$@"\n' > "$R/usr/bin/sln"
chmod 755 "$R/usr/bin/ldconfig" "$R/usr/bin/sln"

# Symlinks: absolute -> relative (the kernel follows them outside the rootfs), and links into
# /dev, /proc, /sys -> absolute (they must reach Android's real ones).
bash "$HERE/relativize-symlinks.sh" "$R" 2>/dev/null || true # find hits unreadable proot mount stubs

# Without proot, programs run as the real Android app uid, which /etc/passwd does not know: shells and terminals
# (kitten run-shell, su, ssh...) then get /sbin/nologin. Give that uid an account with /root and bash.
uid=$(id -u) gid=$(id -g)
if grep -q "^[^:]*:[^:]*:$uid:" "$R/etc/passwd"; then # proot-distro's entry: home "/", shell /sbin/nologin
  sed -i -E "s|^([^:]*:[^:]*:$uid:[^:]*:[^:]*):[^:]*:[^:]*$|\1:/root:/usr/bin/bash|" "$R/etc/passwd"
else
  echo "android:x:$uid:$gid:Android app user:/root:/usr/bin/bash" >> "$R/etc/passwd"
fi
grep -q "^[^:]*:[^:]*:$gid:" "$R/etc/group" || echo "android:x:$gid:" >> "$R/etc/group"

# Absolute RUNPATH/RPATH entries (e.g. libproxy -> /usr/lib/libproxy) are resolved by the loader on Android's
# real filesystem, not through fakechroot. Collect them once; arch-native.sh adds ld.so.conf.d dirs to its path.
find "$R/usr/lib" "$R/usr/bin" -xdev -type f \( -name '*.so*' -o -perm -u+x \) -size +1k 2>/dev/null |
  xargs readelf -d 2>/dev/null | grep -oE '(RUNPATH|RPATH).*\[[^]]*\]' | sed -E 's/.*\[(.*)\]/\1/' | tr ':' '\n' |
  grep -E '^/' | sort -u > "$R/etc/ld.so.conf.d/zz-runpath-no-proot.conf"
echo "runpath dirs: $(wc -l < "$R/etc/ld.so.conf.d/zz-runpath-no-proot.conf")"
echo "rootfs ready for arch-native.sh"
