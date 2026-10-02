#!/bin/sh
# /usr/local/bin/systemctl for Android/Andromarchy: power verbs go to the app (there is no machine to reboot:
# reboot = a fresh session, poweroff = close the app, suspend = to the background); the rest is the
# docker-systemctl-replacement in /usr/local/libexec (services, user units, Restart=...).
for a in "$@"; do
  case $a in
    -*) continue ;;
    reboot | kexec | soft-reboot) exec android-request power reboot ;;
    poweroff | halt) exec android-request power shutdown ;;
    suspend | hibernate | hybrid-sleep | suspend-then-hibernate | sleep) exec android-request power suspend ;;
    *) break ;; # the first verb decides
  esac
done
exec /usr/local/libexec/systemctl "$@"
