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
grep -rlIZ $'\r' "$OMARCHY_PATH" | xargs -0 -r sed -i 's/\r$//'

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
chmod +x /usr/local/bin/systemd-cat

# User config, same layout as Omarchy's install (config/ -> ~/.config).
mkdir -p ~/.config
cp -a "$OMARCHY_PATH"/config/. ~/.config/
# foot first: it renders on the CPU, GPU clients (alacritty) need linux-dmabuf, not available yet (PLAN A.4).
cp "$OMARCHY_PATH"/default/xdg-terminal-exec/hyprland-xdg-terminals.list ~/.config/xdg-terminals.list

export PATH="$OMARCHY_PATH/bin:$PATH"
mkdir -p ~/.config/omarchy/themes
OMARCHY_THEME_HEADLESS=1 omarchy-theme-set "Tokyo Night" || echo "theme-set reported errors (expected for missing apps)"

fc-cache -f >/dev/null
echo "Omarchy installed in $OMARCHY_PATH"
