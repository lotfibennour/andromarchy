#!/bin/sh
# /usr/local/bin/uwsm for Android/Andromarchy: no uwsm session manager. "uwsm stop" (Omarchy's log out) = a fresh
# session (no login screen here); "uwsm app" = run the app as uwsm-app does.
case ${1:-} in
  stop) exec android-request power logout ;;
  app) shift; exec uwsm-app "$@" ;;
  check) exit 0 ;; # "uwsm check may-start" and friends: we are always "the session"
  *) echo "uwsm: not available on Andromarchy" >&2; exit 1 ;;
esac
