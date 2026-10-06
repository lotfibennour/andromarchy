#!/usr/bin/env python3
# /opt/lotfix/flatpak-fix-links.py: repairs Flatpak deployments checked out before android-compat knew that absolute
# symlink targets inside a deployment are container paths (/app/... in an app, /usr/... in a runtime). Such a target
# had been rewritten as "../../../" + the path without its leading "/" (relative from wherever libostree created the
# link). The original path is that tail; recreating the link with it lets android-compat make it relative within the
# deployment. Also undoes a first, wrong repair (the tail resolved from the link's directory: an absolute path that
# continues with "app/..." or "usr/..." after one of the deployment's ancestors). Idempotent.
import glob
import os
import sys

args = sys.argv[1:] or [os.path.expanduser("~/.local/share/flatpak"), "/var/lib/flatpak"]
fixed = 0
# arguments: installations (all their deployments) or single deployments' files dirs
targets = []
for a in args:
    if a.rstrip("/").endswith("/files"):
        targets.append((a.rstrip("/"), "app" if "/app/" in a else "usr"))
    else:
        for kind, prefix in (("app", "app"), ("runtime", "usr")):
            targets += [(f, prefix) for f in glob.glob(f"{a}/{kind}/*/*/*/*/files")]
for files, prefix in targets:
        if True:
            if os.path.islink(os.path.dirname(files)):
                continue  # "active" -> <commit>: the same deployment, seen once
            ancestors = []
            a = files
            while a not in ("/", ""):
                a = os.path.dirname(a)
                ancestors.append(a)
            for d, dirs, names in os.walk(files):
                for n in dirs + names:
                    p = os.path.join(d, n)
                    if not os.path.islink(p):
                        continue
                    t = os.readlink(p)
                    original = None
                    if not t.startswith("/"):
                        g = os.path.normpath(os.path.join(d, t))
                        if g == files or g.startswith(files + "/"):
                            continue  # a genuine link within the deployment
                        if "/repo/objects/" in g:
                            continue  # android-compat's stand-in for a hard link to the repo's object: intended
                        parts = t.split("/")
                        while parts and parts[0] == "..":
                            parts.pop(0)
                        original = "/" + "/".join(parts)
                    elif not (t == "/" + prefix or t.startswith(f"/{prefix}/")):
                        for anc in ancestors:  # the first, wrong repair
                            rest = t[len(anc) + 1:] if t.startswith(anc.rstrip("/") + "/") else None
                            if rest and (rest.startswith(prefix + "/") or rest == prefix):
                                original = "/" + rest
                                break
                    if original and original != t:
                        os.unlink(p)
                        os.symlink(original, p)  # android-compat makes it relative within the deployment
                        fixed += 1
print(f"flatpak-fix-links: {fixed} links repaired")
