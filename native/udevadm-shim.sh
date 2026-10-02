#!/bin/sh
# /usr/local/bin/udevadm for Android/Andromarchy: there is no udev daemon (an app cannot manage /dev). Commands that
# only talk to the daemon succeed as no-ops (installers and pacman hooks run "udevadm control --reload"/"trigger");
# the rest (info, test...) is the real udevadm.
case $1 in
  control | trigger | settle) exit 0 ;;
esac
exec /usr/bin/udevadm "$@"
