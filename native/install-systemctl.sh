#!/bin/bash
# Installs docker-systemctl-replacement (gdraheim, EUPL-1.2) as systemctl/journalctl in /usr/local/bin, ahead of
# systemd's own (which needs a running systemd). It reads the real .service files: start/stop/enable, Restart=,
# "systemctl --user default" for enabled units. Run inside the rootfs as root (build), or with DEST=<rootfs>.
set -euo pipefail
V=v1.7.1097
URL=https://raw.githubusercontent.com/gdraheim/docker-systemctl-replacement/$V/files/docker
SUM_SYSTEMCTL=5f5a47f321c7a8881dfd01f774106dc4012d664bee238eada86a7112674fea93
SUM_JOURNALCTL=16a52fe484d7c0fca464c82a56de58980440362942482c0d1fbc424e69f8f833
D=${DEST:-}/usr/local/bin
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

curl -fsSL -o "$T/systemctl" "$URL/systemctl3.py"
curl -fsSL -o "$T/journalctl" "$URL/journalctl3.py"
echo "$SUM_SYSTEMCTL  $T/systemctl" | sha256sum -c --quiet
echo "$SUM_JOURNALCTL  $T/journalctl" | sha256sum -c --quiet

# Android denies listing some /sys dirs (power_supply...): daemon-reload crashed on a ConditionDirectoryNotEmpty.
# Treat an unreadable directory as empty.
grep -q '                        elif not os.listdir(filepath):' "$T/systemctl"
sed -i 's/^                        elif not os.listdir(filepath):$/                        elif not os.access(filepath, os.R_OK) or not os.listdir(filepath):/' "$T/systemctl"

# Without RestartSec=, the default 0.1 is passed as "0.1", which time_to_seconds() does not parse: restarts were
# scheduled after the 200 s maximum instead of systemd's 100 ms.
grep -q 'delay = conf.get(section, "RestartSec", default or strE(DefaultRestartSec))' "$T/systemctl"
sed -i 's/delay = conf.get(section, "RestartSec", default or strE(DefaultRestartSec))/delay = conf.get(section, "RestartSec", default or "100ms")/' "$T/systemctl"

mkdir -p "$D"
install -m 755 "$T/systemctl" "$T/journalctl" "$D/"
echo "systemctl replacement $V installed in $D"
