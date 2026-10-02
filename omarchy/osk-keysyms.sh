#!/bin/bash
# Andromarchy: every X keysym name -> character, from xkbcommon's header, for the on-screen keyboard plugin
# (it only knew Latin and Cyrillic names; Arabic, French accents... showed the US letters).
D=${1:-~/.config/omarchy/plugins/io.github.abdxdev.onscreen-keyboard}
H=/usr/include/xkbcommon/xkbcommon-keysyms.h
grep -q keysymUnicodeMap "$D/KeyboardLayout.js" && exit 0
{
  echo; echo "// Andromarchy: all keysym names with a Unicode character (generated from xkbcommon-keysyms.h)"
  echo "var keysymUnicodeMap = {"
  sed -n 's|^#define XKB_KEY_\([A-Za-z0-9_]*\) *0x[0-9a-fA-F]* */\* U+\([0-9A-F]*\) .*|    "\1": 0x\2,|p' "$H"
  echo "}"
} >> "$D/KeyboardLayout.js"
sed -i 's|^    if (cyrillicCharMap.hasOwnProperty(normalized)) return cyrillicCharMap\[normalized\]$|&\n    if (keysymUnicodeMap.hasOwnProperty(normalized)) return String.fromCodePoint(keysymUnicodeMap[normalized])|' "$D/KeyboardLayout.js"
