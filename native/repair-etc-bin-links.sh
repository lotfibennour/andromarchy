#!/data/data/com.termux/files/usr/bin/bash
# One-off repair: the first version of relativize-symlinks.sh resolved Android's /etc (-> /system/etc) and
# /bin (-> /system/bin) and gave links under those dirs one "../" too many. For each dangling relative link
# there, drop one leading "../" and keep the change only if the link then resolves.
set -uo pipefail
R=${1:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/archlinux/rootfs}

fixed=0
while IFS= read -r -d '' link; do
  target=$(readlink "$link")
  case $target in ../*) ;; *) continue ;; esac
  [ -e "$link" ] && continue
  ln -sfn "${target#../}" "$link"
  if [ -e "$link" ]; then
    fixed=$((fixed + 1))
  else
    ln -sfn "$target" "$link" # not this bug: restore
  fi
done < <(find "$R/etc" "$R/bin" -xdev -type l -print0 2>/dev/null)
echo "repaired $fixed links"
