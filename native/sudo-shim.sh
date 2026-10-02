#!/bin/bash
# /usr/local/bin/sudo for Android/Andromarchy: there is no real root (setuid does not exist for apps), but pacman
# and Omarchy's installers ("sudo pacman", yay, omarchy-pkg-*) only need to look like root. Run the command as fake
# root, like `ARCH_ROOT=1 lotfix-run`: start faked (fakeroot over TCP, state kept in /var/lib/fakeroot.state) and
# add libfakeroot to the preloads, between android-compat and fakechroot.
while [ $# -gt 0 ]; do
  case $1 in
    -u|-g|-p|-C|-h|-r|-t|-U|-D|-T|-R) shift 2 ;;
    --) shift; break ;;
    -*) shift ;; # -v, -n, -E, -S...: nothing to do
    *) break ;;
  esac
done
[ $# -eq 0 ] && exit 0               # "sudo -v" and friends
[ -n "${FAKEROOTKEY:-}" ] && exec "$@" # already fake root

B=$FAKECHROOT_BASE
compat=${LD_PRELOAD%%:*} # lotfix-run puts android-compat.so first
state=$B/var/lib/fakeroot.state
keyf=$(mktemp)
: >> /var/lib/fakeroot.state
# faked runs without fakechroot (only android-compat: accept -> accept4), so its arguments are host paths (the exec
# itself still goes through fakechroot: guest path). Its stdout must be
# a file: a pipe would close after the key is read and kill it with SIGPIPE later.
LD_PRELOAD=$compat /usr/local/bin/faked --load --save-file "$state" < /var/lib/fakeroot.state > "$keyf" ||
  { echo "sudo: faked failed" >&2; rm -f "$keyf"; exit 1; }
read -r key_pid < "$keyf"
rm -f "$keyf"
export FAKEROOTKEY=${key_pid%%:*}
LD_PRELOAD="$compat:$B/usr/local/lib/libfakeroot.so:${LD_PRELOAD#*:}" "$@"
rc=$?
kill "${key_pid##*:}" 2>/dev/null
exit $rc
