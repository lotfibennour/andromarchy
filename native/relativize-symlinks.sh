#!/data/data/com.termux/files/usr/bin/bash
# Rewrites absolute symlinks inside the rootfs as relative ones.
# fakechroot rewrites the paths programs pass to open()/stat(), but the KERNEL follows symlinks:
# /etc/os-release -> /usr/lib/os-release would resolve against Android's real /usr. Relative links resolve
# inside the rootfs. Links into /dev, /proc and /sys are left alone: those are the real Android ones.
# Usage: relativize-symlinks.sh [rootfs]   (run again after installing packages)
set -euo pipefail
R=${1:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/archlinux/rootfs}

n=0
while IFS= read -r -d '' link; do
  target=$(readlink "$link")
  case $target in
    /dev | /dev/* | /proc | /proc/* | /sys | /sys/*) continue ;;
  esac
  guest_dir=$(dirname "/${link#"$R"/}")
  # -s: compute on the guest path only. Without it, Android's own /etc -> /system/etc link is followed
  # and every link under /etc gets one "../" too many.
  ln -sfn "$(realpath -m -s --relative-to="$guest_dir" "$target")" "$link"
  n=$((n + 1))
done < <(find "$R" -xdev -type l -lname '/*' -print0)
echo "relativized $n symlinks"
