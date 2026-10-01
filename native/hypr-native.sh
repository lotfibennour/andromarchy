#!/data/data/com.termux/files/usr/bin/bash
# Starts Hyprland + Omarchy on Anland WITHOUT proot (no fakeroot: runs as the real Android user).
# Same environment as probes/hypr-anland.sh, but every socket path is the real Android one: Termux's tmp is
# excluded from fakechroot, so Hyprland, its clients and the Anland daemon all see the same files.
T=/data/data/com.termux/files/usr/tmp
H=/data/data/com.termux/files/home

# GPU variables (KGSL, ANLAND*) are set by arch-native.sh for every program; only the compositor's own here.
export ANLAND_SOCKET=$T/anland/display_daemon.sock EGL_PLATFORM=surfaceless
export XDG_RUNTIME_DIR=$T/run
export OMARCHY_PATH=/usr/share/omarchy
unset WAYLAND_DISPLAY DISPLAY

export ARCH_EXCLUDE_EXTRA=$T:$H
export ARCH_LIBPATH_EXTRA=$H/aquamarine/build
exec "$H/native/arch-native.sh" -c "export PATH=/usr/share/omarchy/bin:\$PATH; exec $H/Hyprland/build/Hyprland"
