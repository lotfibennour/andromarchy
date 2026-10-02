#!/bin/bash
# /usr/local/bin/andromarchy-longpress (started with Hyprland): Android-style edit menu on a long touch press.
# The app (Java) detects the press and writes a line to $XDG_RUNTIME_DIR/longpress; the chosen action is sent to the
# focused window as its usual shortcut. Browsers keep their own long-press menu.
F=$XDG_RUNTIME_DIR/longpress
rm -f "$F"
mkfifo -m 600 "$F" || exit 1
exec 3<>"$F" # our own writer end: read never sees EOF between presses

while read -r _ <&3; do
  class=$(hyprctl activewindow -j | jq -r '.class // ""' | tr '[:upper:]' '[:lower:]')
  case $class in
    "" | *firefox* | *brave* | *chrom* | *vivaldi* | *librewolf* | *zen* | *epiphany* | *opera* | *edge*) continue ;;
  esac
  term=0
  case $class in *foot* | *alacritty* | *kitty* | *konsole* | *ghostty* | *wezterm* | *terminal*) term=1 ;; esac

  choice=$(omarchy-menu-select Edit $'󰆏\tCopy' $'󰆒\tPaste' $'󰆐\tCut' $'󰒆\tSelect all' $'󰕌\tUndo' \
    $'󰑎\tRedo' 2>/dev/null) || continue
  mods=(-M ctrl)
  case $choice in
    Copy) key=c; [ $term = 1 ] && mods+=(-M shift) ;;
    Paste) key=v; [ $term = 1 ] && mods+=(-M shift) ;;
    Cut) key=x ;;
    "Select all") key=a ;;
    Undo) key=z ;;
    Redo) key=z; mods+=(-M shift) ;;
    *) continue ;;
  esac
  sleep 0.15 # the menu hands keyboard focus back to the window first
  release=()
  for ((i = ${#mods[@]} - 1; i > 0; i -= 2)); do release+=(-m "${mods[i]}"); done
  wtype "${mods[@]}" -k "$key" "${release[@]}"
done
