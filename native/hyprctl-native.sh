#!/data/data/com.termux/files/usr/bin/bash
# hyprctl for the no-proot session (run from Termux). Example: hyprctl-native.sh dispatch 'hl.dsp.exec_cmd("firefox")'
T=/data/data/com.termux/files/usr/tmp
H=/data/data/com.termux/files/home
R=${ARCH_ROOTFS:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/archlinux/rootfs}
export ARCH_EXCLUDE_EXTRA=$T:$H
export XDG_RUNTIME_DIR=$T/run HYPRLAND_INSTANCE_SIGNATURE=$(ls -t "$T/run/hypr" | head -1)
HYPRCTL=$H/Hyprland/build/hyprctl/hyprctl
[ -e "$R/usr/share/lotfix-hyprland-version" ] && HYPRCTL=/usr/bin/hyprctl # built rootfs: ours is installed
exec "$H/native/arch-native.sh" -c 'exec "$0" "$@"' "$HYPRCTL" "$@"
