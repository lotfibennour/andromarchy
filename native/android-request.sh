#!/bin/sh
# /usr/local/bin/android-request: asks the app (Android side) to do something Linux cannot, through
# $XDG_RUNTIME_DIR/android-request (watched by the app). Requests: wifi-panel | open <package> | split <package> | float <package>
f=$XDG_RUNTIME_DIR/android-request
printf '%s\n' "$*" > "$f.$$" && mv -f "$f.$$" "$f"
