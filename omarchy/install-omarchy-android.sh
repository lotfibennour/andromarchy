#!/bin/bash
# Installs the Android-relevant part of Omarchy 4 into an Arch Linux ARM rootfs.
# Run inside the rootfs as root, with the Omarchy repo unpacked at $1 (default /tmp/omarchy).
# Skipped on purpose: hardware/services that do not exist without root on Android
# (bluez, networkmanager, docker, cups, sddm, plymouth, snapper, ufw, power-profiles, udiskie...).
set -euo pipefail

SRC=${1:-/tmp/omarchy}
export OMARCHY_PATH=/usr/share/omarchy

# Desktop core: Quickshell (Omarchy's shell/bar), Qt, fonts, the CLI tools the omarchy-* scripts call.
pacman -S --noconfirm --needed \
  quickshell qt6-base qt6-declarative qt6-svg qt6-wayland qt6-imageformats qt6-multimedia \
  ttf-jetbrains-mono-nerd noto-fonts noto-fonts-emoji woff2-font-awesome \
  foot alacritty xdg-terminal-exec fastfetch starship btop eza bat fzf zoxide ripgrep fd \
  wl-clipboard grim slurp jq gum socat hyprpicker hyprsunset imv mpv tmux neovim lua51 \
  gnome-themes-extra libnotify inotify-tools imagemagick \
  pipewire pipewire-pulse pipewire-alsa pipewire-audio wireplumber xdg-desktop-portal xdg-desktop-portal-gtk \
  xdg-user-dirs gvfs

rm -rf "$OMARCHY_PATH"
cp -a "$SRC" "$OMARCHY_PATH"
# A clone made on Windows has CRLF line endings, which break every omarchy-* script.
grep -rlIZ $'\r' "$OMARCHY_PATH" | xargs -0 -r sed -i 's/\r$//' || true # none in a Linux clone: grep exits 1
# ttfx comes from the x86-only Omarchy repo; without it the screensaver loops on "command not found".
sed -i '/^trap exit_screensaver/a command -v ttfx >/dev/null || exit_screensaver' "$OMARCHY_PATH/bin/omarchy-screensaver"
# ...and the idle service's launcher would still open its full-screen terminal for that instant exit: a black blink
# every idle period. Don't launch it at all.
sed -i '0,/^pgrep -f .\[o\]rg.omarchy.screensaver/s//# Android\/ARM: no ttfx, no screensaver (its window opened and closed at once: a black blink)\ncommand -v ttfx >\/dev\/null || exit 0\n&/' \
  "$OMARCHY_PATH/bin/omarchy-launch-screensaver"

# No journald on Android: omarchy-launch-shell runs the shell through systemd-cat.
cat > /usr/local/bin/systemd-cat <<'EOF'
#!/bin/sh
# Android/proot: no journald. Drop systemd-cat options and run the command as-is.
while [ $# -gt 0 ]; do
  case $1 in
    -t|-p|--identifier|--priority|--stderr-priority|--level-prefix) shift 2 ;;
    --) shift; break ;;
    -*) shift ;;
    *) break ;;
  esac
done
exec "$@"
EOF
# No systemd user session: the app launcher runs "uwsm-app -- gtk-launch x.desktop" (uwsm puts apps in a scope).
cat > /usr/local/bin/uwsm-app <<'EOF'
#!/bin/sh
# Android: no systemd, so no scope. Drop uwsm-app options and run the command as-is.
while [ $# -gt 0 ]; do
  case $1 in
    -s|-a|-u|-d|-t|-S) shift 2 ;;
    --) shift; break ;;
    -*) shift ;;
    *) break ;;
  esac
done
exec "$@"
EOF
chmod 755 /usr/local/bin/uwsm-app
# No systemd: xdg-terminal-exec and omarchy-launch-browser start apps with "systemd-run --user [--scope]", which
# fails without a running systemd ("Failed to connect to user scope bus"): the config editor and other TUI apps
# opened and died. Run the command directly: in the foreground with --scope, detached otherwise (like a unit).
cat > /usr/local/bin/systemd-run <<'EOF'
#!/bin/sh
scope=0
while [ $# -gt 0 ]; do
  case $1 in
    --scope) scope=1; shift ;;
    --*=*) shift ;;
    -u|-p|-E|-M|--unit|--slice|--property|--description|--setenv|--working-directory|--machine|--uid|--gid|--nice) shift 2 ;;
    --) shift; break ;;
    -*) shift ;;
    *) break ;;
  esac
done
[ $scope = 1 ] && exec "$@"
setsid "$@" >/dev/null 2>&1 < /dev/null &
EOF
chmod 755 /usr/local/bin/systemd-run
chmod +x /usr/local/bin/systemd-cat

# The app's display (Hyprland output ANLAND-1) is the built-in panel: Omarchy recognises one by its connector name
# (eDP/LVDS/DSI), e.g. for the brightness keys and the monitor panel's slider.
grep -rl 'eDP|LVDS|DSI)' "$OMARCHY_PATH"/bin "$OMARCHY_PATH"/shell | xargs -r sed -i 's/(eDP|LVDS|DSI)/(eDP|LVDS|DSI|ANLAND)/g'
pacman -S --noconfirm --needed brightnessctl

# First run (omarchy-provision-first-run, each login until every step succeeds): its user units are hardware ones
# (Bluetooth agent, sleep lock, monitor recovery, fcitx5, crash watch) that a normal install copies in; enabling them
# here always failed, so first-run retried, and its welcome notifications piled up, at every start. Nothing to enable.
printf '#!/bin/bash\n# Android/Andromarchy: no hardware user units to enable (see install-omarchy-android.sh)\nexit 0\n' \
  > "$OMARCHY_PATH/install/user/first-run/enable-user-units.sh"
# Its welcome notification also points to the Android apps.
sed -i 's|Super + Space for Omarchy Menu\.'"'"'|Super + Space for Omarchy Menu.\\nApps > Mobile Apps: your Android apps.'"'"'|' \
  "$OMARCHY_PATH/install/user/first-run/welcome.sh"

# Apps menu: submenus (Mobile Apps, written by the app as apps.mobile) before the applications; Omarchy sorts the
# whole list alphabetically.
grep -q 'Andromarchy: submenus first' "$OMARCHY_PATH"/shell/plugins/menu/Menu.qml ||
  sed -i '/if (active === "apps") {/{n;s|$|\n          if ((a.kind === "app") !== (b.kind === "app")) return a.kind === "app" ? 1 : -1 // Andromarchy: submenus first|}' \
    "$OMARCHY_PATH"/shell/plugins/menu/Menu.qml

# DNS provider (network panel): no NetworkManager/systemd-resolved here; the rootfs's /etc/resolv.conf is written
# directly (native/omarchy-dns-android.sh). Omarchy's own kept as omarchy-dns.orig.
cp -a "$OMARCHY_PATH/bin/omarchy-dns" "$OMARCHY_PATH/bin/omarchy-dns.orig"
install -m 755 "$(dirname "$0")/../native/omarchy-dns-android.sh" "$OMARCHY_PATH/bin/omarchy-dns"

# User config, same layout as Omarchy's install (config/ -> ~/.config).
mkdir -p ~/.config
cp -a "$OMARCHY_PATH"/config/. ~/.config/
# Omarchy's terminal preference list (foot first).
cp "$OMARCHY_PATH"/default/xdg-terminal-exec/hyprland-xdg-terminals.list ~/.config/xdg-terminals.list

# No systemd: the systemctl replacement's init loop starts enabled user services and restarts them (Restart=).
# Started with Hyprland so services get WAYLAND_DISPLAY; skipped if one is still running from a previous session.
grep -q 'systemctl --user init' ~/.config/hypr/autostart.lua ||
  printf '\n-- Android/no-systemd: user services (enabled units, Restart=)\no.exec_on_start("pgrep -f \x27pytho[n].*systemctl --user init\x27 >/dev/null || systemctl --user init")\n' >> ~/.config/hypr/autostart.lua
# (the guard matches the Python process: the sh -c line running it also contains "systemctl --user init")

# Keybinds from virtual keyboards (the on-screen keyboard's wtype): wtype uploads its own keymap, and by default
# Hyprland decodes binds with the main layout, where wtype's first keycode is Escape (Super+W opened the system
# menu). Resolve binds with the sending keyboard's own keymap.
grep -q 'resolve_binds_by_sym' ~/.config/hypr/input.lua ||
  printf '\n-- Android/Andromarchy: binds from virtual keyboards (on-screen keyboard) use their own keymap\nhl.config({ input = { resolve_binds_by_sym = true } })\n' >> ~/.config/hypr/input.lua
# Hyprland is started by the app (SessionService), not start-hyprland: no "launched without start-hyprland" nag.
grep -q 'disable_watchdog_warning' ~/.config/hypr/input.lua ||
  printf '\n-- Android/Andromarchy: started by the app (SessionService), not start-hyprland\nhl.config({ misc = { disable_watchdog_warning = true } })\n' >> ~/.config/hypr/input.lua

# On-screen keyboard: Squeekboard (Phosh's, Arch package). It shows by itself when a text field gets focus
# (input-method-v2); the bar icon (plugin andromarchy.squeekboard) toggles it. PC-style AZERTY layout with
# Ctrl/Super/Alt/AltGr/arrows (terminals too), a globe key for the layout list, Omarchy theme colours.
SQ="$(dirname "$0")/squeekboard"
pacman -S --noconfirm --needed squeekboard
install -m755 "$SQ"/squeekboard "$SQ"/squeekboard-toggle "$SQ"/squeekboard-theme "$SQ"/squeekboard-next-layout /usr/local/bin/
for f in fr_wide fr terminal/fr_wide terminal/fr url/fr_wide url/fr email/fr_wide email/fr; do
  install -Dm644 "$SQ"/fr_wide.yaml ~/.local/share/squeekboard/keyboards/$f.yaml
done
mkdir -p ~/.config/omarchy/plugins/andromarchy.squeekboard ~/.config/omarchy/hooks/theme-set.d
cp "$SQ"/plugin/* ~/.config/omarchy/plugins/andromarchy.squeekboard/
printf '#!/bin/bash\n# Squeekboard follows the Omarchy theme\nsqueekboard-theme\n' > ~/.config/omarchy/hooks/theme-set.d/squeekboard
chmod +x ~/.config/omarchy/hooks/theme-set.d/squeekboard
install -m755 "$SQ"/squeekboard-session /usr/local/bin/
# Globe key: XF86Keyboard -> next language (Squeekboard's own menu is a popover that gets no touch)
grep -q 'squeekboard-next-layout' ~/.config/hypr/bindings.lua ||
  printf '
-- Android/Andromarchy: Squeekboard globe key -> next keyboard language
o.bind("XF86Keyboard", "Next on-screen keyboard language", hl.dsp.exec_cmd("squeekboard-next-layout"))
' >> ~/.config/hypr/bindings.lua
grep -q 'squeekboard-session' ~/.config/hypr/autostart.lua ||
  printf '\n-- Android/Andromarchy: Squeekboard (shows by itself in text fields; bar icon toggles it)\no.exec_on_start("squeekboard-session")\n' >> ~/.config/hypr/autostart.lua
# Touchscreen gestures (hyprgrass, built by build-inside.sh): 3 fingers. HyperOS's own 3-finger gestures must be off
# (docs/device-setup.md).
install -Dm644 "$(dirname "$0")/touch.lua" ~/.config/hypr/touch.lua
grep -q 'require("hypr.touch")' ~/.config/hypr/hyprland.lua ||
  sed -i 's/^require("hypr.autostart")$/require("hypr.autostart")\nrequire("hypr.touch")/' ~/.config/hypr/hyprland.lua
grep -q 'libhyprgrass' ~/.config/hypr/autostart.lua ||
  printf '\n-- Android/Andromarchy: touchscreen gestures\no.exec_on_start("hyprctl plugin load /usr/lib/hyprland-plugins/libhyprgrass.so && hyprctl reload")\n' >> ~/.config/hypr/autostart.lua
# Omarchy's first-run steps that need the session (dark GTK theme, primary paste, welcome notification), once.
grep -q 'first-session.sh' ~/.config/hypr/autostart.lua ||
  printf '\n-- Android/Andromarchy: first session (once)\no.exec_on_start("/opt/lotfix/first-session.sh")\n' >> ~/.config/hypr/autostart.lua
"$OMARCHY_PATH"/bin/omarchy-plugin-enable andromarchy.squeekboard || echo "on-screen keyboard: enable it with 'omarchy plugin enable andromarchy.squeekboard'"

# kitty: spawn bash directly. Its default (kitten run-shell + shell integration) works only partly without proot.
grep -q '^shell /usr/bin/bash' ~/.config/kitty/kitty.conf 2>/dev/null ||
  printf '\n# Android/no-proot: spawn bash directly\nshell /usr/bin/bash\n' >> ~/.config/kitty/kitty.conf

export PATH="$OMARCHY_PATH/bin:$PATH"
mkdir -p ~/.config/omarchy/themes
OMARCHY_THEME_HEADLESS=1 omarchy-theme-set "Tokyo Night" || echo "theme-set reported errors (expected for missing apps)"


# Default browser: Brave Origin (build-inside.sh installs it; Omarchy's own default is Chromium).
xdg-settings set default-web-browser brave-origin.desktop || echo "default browser: set it with xdg-settings"

fc-cache -f >/dev/null
echo "Omarchy installed in $OMARCHY_PATH"
