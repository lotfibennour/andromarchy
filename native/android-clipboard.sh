#!/bin/sh
# /usr/local/bin/android-clipboard (Hyprland autostart): shared clipboard between Android and the desktop.
# Our aquamarine backend writes what Android copies to $XDG_RUNTIME_DIR/clipboard-from-android, and sends Android
# whatever lands in clipboard-to-android (inotify). Text only; the backend drops its own echo.
d=${XDG_RUNTIME_DIR:?}
pkill -f 'wl-paste --type text --watch' 2>/dev/null
wl-paste --type text --watch sh -c 'cat > "$0/clipboard-to-android.tmp" && mv -f "$0/clipboard-to-android.tmp" "$0/clipboard-to-android"' "$d" &
while inotifywait -qq -e moved_to --include '/clipboard-from-android$' "$d"; do
  wl-copy --type text/plain < "$d/clipboard-from-android"
done
