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
  # Some files ship read-only (perl modules: r-x------): writable while patching, mode restored after.
  local mode; mode=$(stat -c %a "$f"); chmod u+w "$f" 2>/dev/null; trap "chmod $mode \"$f\" 2>/dev/null" RETURN
  needed=$(readelf -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(\/[^]]*\)\]/\1/p') || return 0
  # RUNPATH entries rewritten relative to the file ($ORIGIN), which the loader resolves from the file's real (host)
  # location: found at once, without restarting the session for a new ld.so.conf.d entry.
  local rp new="" changed=0 d
  rp=$(readelf -d "$f" 2>/dev/null | sed -n 's/.*(R\(UN\)\?PATH).*\[\([^]]*\)\]/\2/p')
  for d in $(echo "$rp" | tr ':' ' ') $(for n in $needed; do echo "${n%/*}"; done); do
    case $d in /*) d='$ORIGIN/'$(realpath -m --relative-to="${f%/*}" "$d"); changed=1 ;; esac
    case ":$new:" in *":$d:"*) ;; *) new=${new:+$new:}$d ;; esac
  done
  [ $changed = 1 ] && patchelf --set-rpath "$new" "$f" && echo "fix-abs-needed: $f: RUNPATH $new"
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
# Which files need fixing: ONE readelf over all of them (with an extra /dev/null so it always prints "File:"
# headers), not several programs per file: a package of thousands of files took minutes, each program start costing
# tens of milliseconds under the Android runtime.
needs_fix() {
  grep -v '^$' | while read -r f; do [ -f "$f" ] && [ ! -L "$f" ] && printf '%s\n' "$f"; done |
    xargs -r -d '\n' readelf -d /dev/null 2>/dev/null |
    awk '/^File: / { f = substr($0, 7); next }
         /\(NEEDED\).*\[\// || /\((RUNPATH|RPATH)\).*[[:]\// { if (f != "" && !seen[f]++) print f }'
}

if [ "${1:-}" = --all ]; then
  find /usr/bin /usr/lib -xdev -type f \( -perm -u+x -o -name '*.so*' \) -size +1k 2>/dev/null
elif [ $# -gt 0 ]; then
  printf '%s\n' "$@"
else
  sed 's|^|/|' # from the pacman hook: paths relative to /
fi | needs_fix | while read -r f; do fix "$f"; done
exit 0
