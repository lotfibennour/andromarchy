#!/usr/bin/env python3
# Run inside the rootfs (lotfix-run -c "python3 relink-host-symlinks.py") after moving it to another directory.
# Symlinks whose target embeds a host rootfs path (fakechroot stores absolute targets as host paths): make them
# relative, so they survive the rootfs being moved (Termux test dir -> the app's files dir).
import os
fixed = 0
for root, dirs, files in os.walk("/", topdown=True):
    dirs[:] = [d for d in dirs if not (root == "/" and d in ("proc", "sys", "dev"))]
    for name in dirs + files:
        p = os.path.join(root, name)
        if not os.path.islink(p):
            continue
        t = os.readlink(p)
        if t.startswith("/data/") and "/rootfs/" in t:
            guest = "/" + t.rsplit("/rootfs/", 1)[1]
            rel = os.path.relpath(guest, root)
            os.unlink(p)
            os.symlink(rel, p)
            fixed += 1
print("relinked", fixed)
