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
  gnome-themes-extra libnotify inotify-tools imagemagick

rm -rf "$OMARCHY_PATH"
cp -a "$SRC" "$OMARCHY_PATH"
# A clone made on Windows has CRLF line endings, which break every omarchy-* script.
grep -rlIZ $'\r' "$OMARCHY_PATH" | xargs -0 -r sed -i 's/\r$//' || true # none in a Linux clone: grep exits 1
# ttfx comes from the x86-only Omarchy repo; without it the screensaver loops on "command not found".
sed -i '/^trap exit_screensaver/a command -v ttfx >/dev/null || exit_screensaver' "$OMARCHY_PATH/bin/omarchy-screensaver"

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
chmod +x /usr/local/bin/systemd-cat

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

# kitty: spawn bash directly. Its default (kitten run-shell + shell integration) works only partly without proot.
grep -q '^shell /usr/bin/bash' ~/.config/kitty/kitty.conf 2>/dev/null ||
  printf '\n# Android/no-proot: spawn bash directly\nshell /usr/bin/bash\n' >> ~/.config/kitty/kitty.conf

export PATH="$OMARCHY_PATH/bin:$PATH"
mkdir -p ~/.config/omarchy/themes
OMARCHY_THEME_HEADLESS=1 omarchy-theme-set "Tokyo Night" || echo "theme-set reported errors (expected for missing apps)"

# Firefox GPU prefs as defaults for every profile (pref() in defaults/pref, same values as firefox-user.js).
sed 's/^user_pref(/pref(/' "$(dirname "$0")/firefox-user.js" > /usr/lib/firefox/defaults/pref/lotfix-android.js

fc-cache -f >/dev/null
echo "Omarchy installed in $OMARCHY_PATH"
