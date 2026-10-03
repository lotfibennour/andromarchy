#!/bin/bash
# /opt/lotfix/first-session.sh (Hyprland autostart, once): the part of Omarchy's first run that needs the running
# session (D-Bus for dconf, the notification daemon): install/user/first-run/{gnome-theme,gtk-primary-paste,welcome}.
mark=/root/.local/state/andromarchy/first-session.done
[ -f "$mark" ] && exit 0
sleep 5 # the shell's notification service comes up with the bar
# The on-screen keyboard plugin (cloned at build time): enabling needs the running shell.
OMARCHY_PATH=/usr/share/omarchy /usr/share/omarchy/bin/omarchy-plugin-enable io.github.abdxdev.onscreen-keyboard
gsettings set org.gnome.desktop.interface gtk-theme "Adwaita-dark"
gsettings set org.gnome.desktop.interface color-scheme "prefer-dark"
gsettings set org.gnome.desktop.interface icon-theme "Yaru-blue"
gsettings set org.gnome.desktop.interface gtk-enable-primary-paste true
omarchy-notification-send -u critical "Learn Keybindings" \
  $'Super + K for cheatsheet.\nSuper + Space for Omarchy Menu.\nMobile Apps: your Android apps.' \
  --exec omarchy-menu-keybindings
mkdir -p "${mark%/*}" && touch "$mark"
