#!/bin/bash
# Some packages link a library by absolute path (mpv: NEEDED /usr/lib/libmujs.so). Without proot, ld.so opens that
# path on Android's filesystem (fakechroot cannot rewrite the loader's own opens): "cannot open shared object file".
# Rewrite such entries to the bare file name, found through the library path like any other.
# Usage, inside the rootfs: fix-abs-needed.sh --all | FILE...   (--all: every ELF in /usr/bin and /usr/lib)
#        from a pacman hook: the changed paths arrive on stdin (NeedsTargets), relative to /.
set -uo pipefail

fix() {
  local f=$1 needed
  [ -f "$f" ] && [ ! -L "$f" ] || return 0
  needed=$(readelf -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(\/[^]]*\)\]/\1/p') || return 0
  for n in $needed; do
    patchelf --replace-needed "$n" "${n##*/}" "$f" && echo "fix-abs-needed: $f: $n -> ${n##*/}"
    # The bare name must still be found: put the library's directory on the search path (lotfix-run reads
    # /etc/ld.so.conf.d). nvim -> /usr/lib/lua/5.1/lpeg.so broke without it.
    grep -qxF "${n%/*}" /etc/ld.so.conf.d/zz-abs-needed.conf 2>/dev/null || echo "${n%/*}" >> /etc/ld.so.conf.d/zz-abs-needed.conf
  done
  # Absolute RUNPATH/RPATH dirs (epiphany -> /usr/lib/epiphany) are searched on Android's filesystem by the loader:
  # same treatment, on the search path.
  for d in $(readelf -d "$f" 2>/dev/null | sed -n 's/.*(R\(UN\)\?PATH).*\[\([^]]*\)\]/\2/p' | tr ':' ' '); do
    case $d in /*) grep -qxF "$d" /etc/ld.so.conf.d/zz-abs-needed.conf 2>/dev/null || echo "$d" >> /etc/ld.so.conf.d/zz-abs-needed.conf ;; esac
  done
}

if [ "${1:-}" = --all ]; then
  find /usr/bin /usr/lib -xdev -type f \( -perm -u+x -o -name '*.so*' \) -size +1k 2>/dev/null | while read -r f; do fix "$f"; done
elif [ $# -gt 0 ]; then
  for f in "$@"; do fix "$f"; done
else
  while read -r p; do fix "/$p"; done
fi
exit 0
