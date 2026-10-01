#!/data/data/com.termux/files/usr/bin/bash
# hyprctl for the no-proot session (run from Termux). Example: hyprctl-native.sh dispatch 'hl.dsp.exec_cmd("firefox")'
T=/data/data/com.termux/files/usr/tmp
H=/data/data/com.termux/files/home
export ARCH_EXCLUDE_EXTRA=$T:$H
export XDG_RUNTIME_DIR=$T/run HYPRLAND_INSTANCE_SIGNATURE=$(ls -t "$T/run/hypr" | head -1)
exec "$H/native/arch-native.sh" -c 'exec "$0" "$@"' "$H/Hyprland/build/hyprctl/hyprctl" "$@"
