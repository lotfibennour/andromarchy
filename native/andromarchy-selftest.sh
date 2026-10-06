#!/bin/bash
# /usr/local/bin/andromarchy-selftest: regression test of a running Andromarchy session. Run it inside the session
# (terminal, or adb through inapp.sh) after any change or on a freshly installed image: one PASS/FAIL line per
# check, a summary, exit code = number of failures. Opens no window and leaves the clipboard as it was.
# Usage: andromarchy-selftest [-v]   (-v: show the output of failed checks)
V=${1:-}
# Same environment as the session's apps (bus addresses, DISPLAY, VA-API...), wherever this runs from (adb, ssh):
# taken from the Omarchy shell, which Hyprland started.
qs=$(pgrep -x quickshell | head -1)
[ -n "$qs" ] && while IFS= read -r -d '' kv; do case $kv in
  DBUS_*|DISPLAY=*|WAYLAND_DISPLAY=*|XDG_RUNTIME_DIR=*|LIBVA_*|TERMUX_VA_*|MESA_*|FD_*|TU_*|TURNIP_*|GALLIUM_*|ANDROMARCHY*|XWAYLAND_*|EGL_*) export "$kv";;
esac; done < /proc/$qs/environ
# Same environment as the session's apps (bus addresses, DISPLAY, VA-API...), wherever this runs from (adb, ssh):
# taken from the Omarchy shell, which Hyprland started.
qs=$(pgrep -x quickshell | head -1)
[ -n "$qs" ] && while IFS= read -r -d '' kv; do case $kv in
  DBUS_*|DISPLAY=*|WAYLAND_DISPLAY=*|XDG_RUNTIME_DIR=*|LIBVA_*|TERMUX_VA_*|MESA_*|FD_*|TU_*|TURNIP_*|GALLIUM_*|ANDROMARCHY*|XWAYLAND_*|EGL_*) export "$kv";;
esac; done < /proc/$qs/environ
pass=0; fail=0; failed=()
t() { # t "name" command...: PASS if the command succeeds within 20 s
  local name=$1; shift
  local out
  if declare -F "$1" >/dev/null; then out=$("$@" 2>&1); else out=$(timeout 20 "$@" 2>&1); fi
  if [ $? -eq 0 ]; then
    printf '  \e[32mPASS\e[0m %s\n' "$name"; pass=$((pass + 1))
  else
    printf '  \e[31mFAIL\e[0m %s\n' "$name"; fail=$((fail + 1)); failed+=("$name")
    [ "$V" = -v ] && printf '%s\n' "$out" | tail -5 | sed 's/^/         /'
  fi
}
has() { grep -qE "$1"; }
section() { printf '\n\e[1m%s\e[0m\n' "$1"; }

section "Session"
t "Hyprland running"                pgrep -x Hyprland
t "Omarchy shell (quickshell)"      pgrep -x quickshell
t "hyprctl answers"                 hyprctl version
t "no Hyprland config errors"       sh -c '[ -z "$(hyprctl configerrors | grep -v "^$")" ]'
t "touch gestures (hyprgrass)"      sh -c 'hyprctl plugin list | grep -q hyprgrass'

section "GPU"
t "Hyprland renders with our Mesa"  sh -c 'grep -q libgallium /proc/$(pgrep -x Hyprland | head -1)/maps'
t "OpenGL on Adreno (Wayland)"      sh -c 'eglinfo -B -p wayland | grep -q "renderer: Adreno"'
t "Vulkan on Adreno (Turnip)"       sh -c 'vulkaninfo --summary | grep -q "deviceName.*Adreno"'
t "X11 apps on the GPU (glamor)"    sh -c 'grep -q "KGSL surfaceless EGL backend" /data/user/0/andro.marchy/files/run/hyprland.log'
t "X11 OpenGL direct on Adreno"     sh -c 'glxinfo -B | grep -q "direct rendering: Yes" && glxinfo -B | grep -q "renderer string: Adreno"'
t "hardware video (VA-API termuxva)" sh -c 'vainfo 2>&1 | grep -q "VAProfile"'

section "Sound, microphone, camera"
t "PipeWire running"                pgrep -x pipewire
t "audio output (sink)"             sh -c 'pactl list short sinks | grep -q .'
t "microphone (source)"             sh -c 'pactl list short sources | grep -v monitor | grep -q .'
t "cameras as PipeWire nodes"       sh -c 'pw-cli ls Node | grep -q android-camera'

section "Input"
t "on-screen keyboard (Squeekboard)" sh -c 'busctl --user get-property sm.puri.OSK0 /sm/puri/OSK0 sm.puri.OSK0 Visible'
t "keyboard theme overlay"          test -s "$HOME/.local/share/squeekboard/theme/style.css"
t "PC layouts installed"            sh -c 'for l in fr us ara; do test -s ~/.local/share/squeekboard/keyboards/${l}_wide.yaml || exit 1; done'

section "Clipboard with Android"
t "bridge running"                  pgrep -f /usr/local/bin/android-clipboard
clip_test() {
  local before; before=$(wl-paste --type text 2>/dev/null)
  local mark="andromarchy-selftest-$$"
  printf '%s' "$mark" | wl-copy >/dev/null 2>&1
  sleep 1
  local ok=1
  [ "$(cat "$XDG_RUNTIME_DIR/clipboard-to-android" 2>/dev/null)" = "$mark" ] && ok=0
  printf '%s' "$before" | wl-copy >/dev/null 2>&1   # put the user's clipboard back
  return $ok
}
t "desktop copy reaches the backend" clip_test

section "D-Bus services"
for s in org.freedesktop.Notifications org.freedesktop.portal.Desktop org.freedesktop.secrets \
         org.freedesktop.login1 org.freedesktop.UPower org.freedesktop.NetworkManager; do
  bus=--user; case $s in *login1|*UPower|*NetworkManager) bus=--system;; esac
  t "$s" sh -c "busctl $bus list --no-pager | grep -q '^$s '"  # running, or activatable on first use
done

section "System"
t "network (DNS + HTTPS)"           curl -fsS -o /dev/null --max-time 10 https://archlinux.org
t "network state from Android (Wi-Fi name)" sh -c 'n=$(cat $XDG_RUNTIME_DIR/network); case $n in "wifi "*" "?*|wired) ;; *) exit 1;; esac'
t "pacman database"                 pacman -Q pacman
t "sudo (fake root)"                sudo true
t "Go programs (gum)"               gum --version
t "Bun programs (claude)"           sh -c 'command -v claude >/dev/null || exit 0; claude --version'
t "fonts"                           sh -c '[ "$(fc-list | wc -l)" -gt 10 ]'
t "protected builds intact"         sh -c 'out=$(andromarchy-protect check); [ -z "$out" ]'
t "protection hook installed"       test -f /etc/pacman.d/hooks/zz-andromarchy-protect.hook

section "Flatpak"
t "flatpak works"                   flatpak --version
t "Flathub remote"                  sh -c 'flatpak remotes | grep -q flathub'

printf '\n\e[1m%d passed, %d failed\e[0m\n' "$pass" "$fail"
for f in "${failed[@]}"; do printf '  - %s\n' "$f"; done
exit "$fail"
