#!/bin/bash
# /usr/local/bin/omarchy-android-patches: Andromarchy's changes to Omarchy's own files (/usr/share/omarchy, a git
# checkout that omarchy-update replaces). Run by install-omarchy-android.sh and after every Omarchy update
# (~/.config/omarchy/hooks/post-update.d/andromarchy). Idempotent.
O=${OMARCHY_PATH:-/usr/share/omarchy}
[ -d "$O/bin" ] || exit 0

# A clone made on Windows has CRLF line endings, which break every omarchy-* script.
grep -rlIZ $'\r' "$O" | xargs -0 -r sed -i 's/\r$//' || true

# ttfx comes from the x86-only Omarchy repo; without it the screensaver loops on "command not found"...
grep -q 'command -v ttfx >/dev/null || exit_screensaver' "$O/bin/omarchy-screensaver" ||
  sed -i '/^trap exit_screensaver/a command -v ttfx >/dev/null || exit_screensaver' "$O/bin/omarchy-screensaver"
# ...and the idle service's launcher would still open its full-screen terminal for that instant exit: a black blink
# every idle period. Don't launch it at all.
grep -q 'command -v ttfx >/dev/null || exit 0' "$O/bin/omarchy-launch-screensaver" ||
  sed -i '0,/^pgrep -f .\[o\]rg.omarchy.screensaver/s//# Android\/ARM: no ttfx, no screensaver (its window opened and closed at once: a black blink)\ncommand -v ttfx >\/dev\/null || exit 0\n&/' \
    "$O/bin/omarchy-launch-screensaver"

# The app's display (Hyprland output ANDROMARCHY-1) is the built-in panel: Omarchy recognises one by its connector name
# (eDP/LVDS/DSI), e.g. for the brightness keys and the monitor panel's slider.
grep -rl 'eDP|LVDS|DSI)' "$O"/bin "$O"/shell | xargs -r sed -i 's/(eDP|LVDS|DSI)/(eDP|LVDS|DSI|ANDROMARCHY)/g'
grep -rl 'DSI|ANLAND)' "$O"/bin "$O"/shell | xargs -r sed -i 's/(eDP|LVDS|DSI|ANLAND)/(eDP|LVDS|DSI|ANDROMARCHY)/g' # the output's former name

# First run (omarchy-provision-first-run, each login until every step succeeds): its user units are hardware ones
# (Bluetooth agent, sleep lock, monitor recovery, fcitx5, crash watch); enabling them here always failed, so
# first-run retried, and its welcome notifications piled up, at every start. Nothing to enable.
printf '#!/bin/bash\n# Android/Andromarchy: no hardware user units to enable (see omarchy-android-patches)\nexit 0\n' \
  > "$O/install/user/first-run/enable-user-units.sh"
# Its welcome notification also points to the Android apps.
grep -q 'Mobile Apps: your Android apps' "$O/install/user/first-run/welcome.sh" ||
  sed -i 's|Super + Space for Omarchy Menu\.'"'"'|Super + Space for Omarchy Menu.\\nApps > Mobile Apps: your Android apps.'"'"'|' \
    "$O/install/user/first-run/welcome.sh"

# Apps menu: submenus (Mobile Apps, written by the app as apps.mobile) before the applications; Omarchy sorts the
# whole list alphabetically.
grep -q 'Andromarchy: submenus first' "$O"/shell/plugins/menu/Menu.qml ||
  sed -i '/if (active === "apps") {/{n;s|$|\n          if ((a.kind === "app") !== (b.kind === "app")) return a.kind === "app" ? 1 : -1 // Andromarchy: submenus first|}' \
    "$O"/shell/plugins/menu/Menu.qml

# DNS provider (network panel): no NetworkManager/systemd-resolved here; the rootfs's /etc/resolv.conf is written
# directly (/usr/local/share/andromarchy/omarchy-dns). Omarchy's own kept as omarchy-dns.orig.
if ! cmp -s /usr/local/share/andromarchy/omarchy-dns "$O/bin/omarchy-dns"; then
  cp -a "$O/bin/omarchy-dns" "$O/bin/omarchy-dns.orig"
  install -m 755 /usr/local/share/andromarchy/omarchy-dns "$O/bin/omarchy-dns"
fi
