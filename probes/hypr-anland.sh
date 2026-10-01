# Runs inside Arch: patched Hyprland + Anland aquamarine backend.
export ANLAND=1 ANLAND_NO_DRM_DEVICE=1 ANLAND_SOCKET=/tmp/anland/display_daemon.sock
export MESA_LOADER_DRIVER_OVERRIDE=kgsl TURNIP_KMD=kgsl GALLIUM_DRIVER=freedreno FD_FORCE_KGSL=1 XWAYLAND_FORCE_KGSL_SURFACELESS=1 EGL_PLATFORM=surfaceless
export ANLAND_PROTO_DEBUG=1 ANLAND_INPUT_DEBUG=1 XDG_RUNTIME_DIR=/tmp/run AQ_TRACE=1 HYPRLAND_TRACE=1
export QT_QUICK_BACKEND=software OMARCHY_PATH=/usr/share/omarchy PATH=/usr/share/omarchy/bin:$PATH
unset WAYLAND_DISPLAY DISPLAY
H=/data/data/com.termux/files/home
export LD_LIBRARY_PATH=$H/aquamarine/build
exec $H/Hyprland/build/Hyprland --i-am-really-stupid
