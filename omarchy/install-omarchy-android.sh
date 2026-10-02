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

# On-screen keyboard by default (abdxdev/omarchy-onscreen-keyboard, MIT): a bar plugin that types through wtype
# (Wayland virtual keyboard), so it reaches every app with accents too. Pinned commit.
OSK_ID=io.github.abdxdev.onscreen-keyboard
OSK_REV=0d42cadf48729ed2badfeee24163be7e740ec042
pacman -S --noconfirm --needed wtype
if [ ! -d ~/.config/omarchy/plugins/$OSK_ID ]; then
  mkdir -p ~/.config/omarchy/plugins
  git clone -q https://github.com/abdxdev/omarchy-onscreen-keyboard.git ~/.config/omarchy/plugins/$OSK_ID
  git -C ~/.config/omarchy/plugins/$OSK_ID checkout -q $OSK_REV
  # Modifier combos with an ASCII symbol (Super+Space: Omarchy menu) passed the character to `wtype -k`, which
  # wants a keysym name. Use the plugin's own name table for them.
  sed -i 's|    if (codePoint < 0x80) return text|    if (/^[A-Za-z0-9]$/.test(text)) return text\n    // Other ASCII (space, punctuation) needs its keysym name for wtype -k (Andromarchy fix)\n    for (var name in tokenCharMap)\n        if (tokenCharMap[name] === text) return name|' \
    ~/.config/omarchy/plugins/$OSK_ID/KeyboardLayout.js
  # The space bar typed " " as text (wtype --): apps took it for a special key. Send the space key instead.
  sed -i 's|^        sendKeys(Layout.buildTypeCommand(text))$|        // Andromarchy: space as the "space" key (typed as text, apps saw a special key)\n        sendKeys(text === " " ? Layout.buildKeyCommand("space") : Layout.buildTypeCommand(text))|' \
    ~/.config/omarchy/plugins/$OSK_ID/Keyboard.qml
  # Languages: wtype types any character itself, so the language key only changes the on-screen layout and leaves
  # the system one (physical keyboard) alone. All keysym names -> characters (Arabic, French accents...).
  sed -i 's|^        Quickshell.execDetached(\["hyprctl", "switchxkblayout", "all", "next"\])$|        // Andromarchy: wtype types characters itself; keep the system layout (physical keyboard) as is|' \
    ~/.config/omarchy/plugins/$OSK_ID/Keyboard.qml
  bash "$(dirname "$0")/osk-keysyms.sh" ~/.config/omarchy/plugins/$OSK_ID
fi
# On-screen keyboard languages, read by the plugin from kb_layout (the system layout stays the first one).
grep -q 'kb_layout = "us,fr,ara"' ~/.config/hypr/input.lua ||
  printf '\n-- Android/Andromarchy: on-screen keyboard languages (the system layout stays the first one)\nhl.config({ input = { kb_layout = "us,fr,ara" } })\n' >> ~/.config/hypr/input.lua
# Shown automatically when a text field gets focus, hidden while a physical keyboard is connected (osk-autoshow,
# built into /usr/local/bin by build-inside.sh). Started with Hyprland.
grep -q 'osk-autoshow' ~/.config/hypr/autostart.lua ||
  printf '\n-- Android/Andromarchy: on-screen keyboard follows text-field focus\no.exec_on_start("osk-autoshow")\n' >> ~/.config/hypr/autostart.lua
# Android-style edit menu on a long touch press (native/longpress-menu.sh, installed by build-inside.sh).
# Omarchy's first-run steps that need the session (dark GTK theme, primary paste, welcome notification), once.
grep -q 'first-session.sh' ~/.config/hypr/autostart.lua ||
  printf '\n-- Android/Andromarchy: first session (once)\no.exec_on_start("/opt/lotfix/first-session.sh")\n' >> ~/.config/hypr/autostart.lua
grep -q 'andromarchy-longpress' ~/.config/hypr/autostart.lua ||
  printf '\n-- Android/Andromarchy: edit menu on a long touch press\no.exec_on_start("andromarchy-longpress")\n' >> ~/.config/hypr/autostart.lua
"$OMARCHY_PATH"/bin/omarchy-plugin-enable $OSK_ID || echo "on-screen keyboard: enable it with 'omarchy plugin enable $OSK_ID'"

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
