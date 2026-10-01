# Phase 2.1/2.2 checks, run inside arch-native.sh (bash from the Arch rootfs, no proot).
head -2 /etc/os-release
echo "binaries in /usr/bin: $(ls /usr/bin | wc -l)"
python -c 'print("python-ok")'
printf '#!/bin/sh\necho "shebang-ok $0"\n' > /tmp/t.sh
chmod +x /tmp/t.sh
/tmp/t.sh
/usr/bin/env python3 -c 'import sys; print("env-python-ok", sys.executable)'
