#!/bin/bash
# /opt/lotfix/first-session.sh (Hyprland autostart, once): the part of Omarchy's first run that needs the running
# session (D-Bus for dconf, the notification daemon): install/user/first-run/{gnome-theme,gtk-primary-paste,welcome}.
mark=/root/.local/state/andromarchy/first-session.done
[ -f "$mark" ] && exit 0
sleep 5 # the shell's notification service comes up with the bar
# The on-screen keyboard's bar icon (Squeekboard, omarchy/squeekboard/plugin): enabling needs the running shell.
# Wait for it; if it never comes, stop here without the mark: the next session tries again (a shell that failed to
# start left a new install without the keyboard's icon for good).
for i in $(seq 60); do pgrep -x quickshell >/dev/null && break; sleep 1; done
OMARCHY_PATH=/usr/share/omarchy /usr/share/omarchy/bin/omarchy-plugin-enable andromarchy.squeekboard || exit 1
gsettings set org.gnome.desktop.interface gtk-theme "Adwaita-dark"
gsettings set org.gnome.desktop.interface color-scheme "prefer-dark"
gsettings set org.gnome.desktop.interface icon-theme "Yaru-blue"
gsettings set org.gnome.desktop.interface gtk-enable-primary-paste true
# App store (GNOME Software): Flathub for this user (no system helper here; per-user installs, bwrap-flatpak.py runs them).
flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
# On a new install, GNOME Software's first install failed until the CLI had made these (the repo's temp dir, the
# exports): there from the start.
mkdir -p ~/.local/share/flatpak/repo/tmp ~/.local/share/flatpak/exports/share/applications \
  ~/.local/share/flatpak/exports/share/icons ~/.local/share/flatpak/exports/bin
# Flathub's graphics drivers stay out (~1.8 GB): apps run on our Mesa (bwrap-flatpak.py's host_gl).
flatpak --user mask 'org.freedesktop.Platform.GL.*'
flatpak --user mask 'org.freedesktop.Platform.GL32.*'
# Translations of Flatpak runtimes: the user's language and English only (GNOME's all-language one was 1.6 GB).
flatpak --user config --set languages "${LANG%%_*};en"
# (the welcome notification is Omarchy's own first run's: install-omarchy-android.sh adds the Mobile Apps line)
mkdir -p "${mark%/*}" && touch "$mark"
