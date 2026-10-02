#!/bin/bash
# /opt/lotfix/first-boot.sh: the device-specific half of an install, run by the app (SessionService) inside the rootfs
# before the session starts. The build did everything generic; this is what Omarchy's first boot / setup form does
# that still applies on Android (keyboard, language, name/email), plus what depends on this device.
#   ANDROMARCHY_UID/GID   the app's Android uid/gid (every process runs as it)
#   LANG                  Android's language as a glibc locale (fr_FR.UTF-8...)
# Answers of the app's setup form, in /root/.config/andromarchy/setup.conf (KEY=value lines):
#   KB_LAYOUTS (xkb, comma-separated, first = physical keyboard), FULL_NAME, EMAIL
# Every step is idempotent; LANG's locale is checked on each start (Android's language can change).
set -uo pipefail
conf=/root/.config/andromarchy/setup.conf
done_mark=/root/.local/state/andromarchy/first-boot.done

# Language: glibc reads locales from LOCPATH directories (no locale-archive), one per locale. Compile Android's.
loc=${LANG:-C.UTF-8}
if [[ $loc != C.UTF-8 && ! -d /usr/lib/locale/$loc ]]; then
  base=${loc%%.*}
  [ -f "/usr/share/i18n/locales/$base" ] && localedef --no-archive -i "$base" -f UTF-8 "/usr/lib/locale/$loc" || true
fi

[ -f "$done_mark" ] && exit 0

# The app's uid is the only user: give it /root and bash (otherwise shells and terminals get /sbin/nologin).
uid=${ANDROMARCHY_UID:-$(id -u)} gid=${ANDROMARCHY_GID:-$(id -g)}
grep -q "^[^:]*:[^:]*:$uid:" /etc/passwd || echo "android:x:$uid:$gid:Android app user:/root:/usr/bin/bash" >> /etc/passwd
grep -q "^[^:]*:[^:]*:$gid:" /etc/group || echo "android:x:$gid:" >> /etc/group

# systemd would fill it on first boot; D-Bus (dconf, portals) refuses an empty one. Arch ships it read-only.
[ -s /etc/machine-id ] || { rm -f /etc/machine-id; tr -d '-' < /proc/sys/kernel/random/uuid > /etc/machine-id; }

# Sticky world-writable as on any Linux (Hyprland starts Xwayland only with these).
mkdir -p /tmp/.X11-unix /tmp/.ICE-unix
chmod 1777 /tmp /var/tmp /tmp/.X11-unix /tmp/.ICE-unix 2>/dev/null

# DNS: the current network's own servers (omarchy-dns DHCP follows Android's network).
OMARCHY_PATH=/usr/share/omarchy /usr/share/omarchy/bin/omarchy-dns DHCP

# Setup form answers.
KB_LAYOUTS=us FULL_NAME= EMAIL=
[ -f "$conf" ] && . "$conf"
input=/root/.config/hypr/input.lua
sed -i '/-- Android\/Andromarchy: keyboard layouts/,+2d; /on-screen keyboard languages (the system layout/d; /kb_layout = "us,fr,ara"/d' "$input"
printf '\n-- Android/Andromarchy: keyboard layouts (setup form; the first one is the physical keyboard'"'"'s, the\n-- on-screen keyboard cycles through all)\nhl.config({ input = { kb_layout = "%s" } })\n' "$KB_LAYOUTS" >> "$input"
# Omarchy's install/user/git.sh and xcompose.sh.
[ -n "${FULL_NAME// /}" ] && git config --global user.name "$FULL_NAME"
[ -n "${EMAIL// /}" ] && git config --global user.email "$EMAIL"
cat > /root/.XCompose <<EOF
include "/usr/share/omarchy/default/xcompose"
<Multi_key> <space> <n> : "$FULL_NAME"
<Multi_key> <space> <e> : "$EMAIL"
EOF

mkdir -p "${done_mark%/*}" && touch "$done_mark"
echo "first boot done"
