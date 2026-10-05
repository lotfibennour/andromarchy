#!/bin/bash
# Which D-Bus services a Linux desktop expects are present in the session: RUNNING (owned now), ACTIVATABLE
# (started on first use), or MISSING. Run inside the session (needs its bus addresses).
# Usage: dbus-probe.sh > report
SYSTEM="org.freedesktop.NetworkManager org.freedesktop.UPower org.freedesktop.login1 org.freedesktop.systemd1
org.freedesktop.hostname1 org.freedesktop.timedate1 org.freedesktop.locale1 org.freedesktop.PolicyKit1
org.freedesktop.Accounts org.freedesktop.UDisks2 org.bluez org.freedesktop.ModemManager1 org.freedesktop.GeoClue2
org.freedesktop.resolve1 org.freedesktop.RealtimeKit1 org.freedesktop.ColorManager org.freedesktop.fwupd
net.hadess.PowerProfiles org.freedesktop.UPower.PowerProfiles fi.w1.wpa_supplicant1 org.freedesktop.Avahi
org.freedesktop.PackageKit org.freedesktop.thermald org.freedesktop.import1 org.freedesktop.machine1"

SESSION="org.freedesktop.Notifications org.freedesktop.portal.Desktop org.freedesktop.impl.portal.desktop.gtk
org.freedesktop.secrets org.freedesktop.ScreenSaver org.freedesktop.FileManager1 org.freedesktop.portal.Documents
org.freedesktop.impl.portal.PermissionStore org.gtk.vfs.Daemon org.gtk.vfs.UDisks2VolumeMonitor ca.desrt.dconf
org.a11y.Bus org.kde.StatusNotifierWatcher org.freedesktop.systemd1 org.mpris.MediaPlayer2 org.freedesktop.Tracker3.Miner.Files
org.gnome.keyring org.freedesktop.ReserveDevice1.Audio0 org.freedesktop.IBus org.fcitx.Fcitx5 org.pulseaudio.Server"

probe() { # bus name
    local bus=$1 name=$2 flag=--$1
    if busctl $flag --timeout=2 list --no-legend 2>/dev/null | awk '{print $1}' | grep -qx "$name"; then echo RUNNING
    elif busctl $flag --timeout=2 call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus \
         ListActivatableNames 2>/dev/null | tr ' ' '\n' | tr -d '"' | grep -qx "$name"; then echo ACTIVATABLE
    else echo MISSING; fi
}
for n in $SYSTEM; do printf '%-12s system  %s\n' "$(probe system $n)" "$n"; done
for n in $SESSION; do
    s=$(probe user $n)
    [ $s = MISSING ] && busctl --user list --no-legend 2>/dev/null | awk '{print $1}' | grep -q "^$n\." && s=RUNNING # mpris.*
    printf '%-12s session %s\n' "$s" "$n"
done
