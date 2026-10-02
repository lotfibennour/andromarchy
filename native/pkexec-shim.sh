#!/bin/bash
# /usr/local/bin/pkexec for Android/Andromarchy: polkit's pkexec is setuid (impossible for apps). Programs that elevate
# through it get the same fake root as sudo (native/sudo-shim.sh).
while [ $# -gt 0 ]; do
  case $1 in
    --user) shift 2 ;;
    --) shift; break ;;
    -*) shift ;; # --disable-internal-agent, --keep-cwd...
    *) break ;;
  esac
done
exec sudo "$@"
