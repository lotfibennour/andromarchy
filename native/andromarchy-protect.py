#!/usr/bin/env python3
# /usr/local/bin/andromarchy-protect: keeps Andromarchy's own builds in place across pacman transactions.
#
# Our Mesa (KGSL), Hyprland and aquamarine (Anland backend) replace files that belong to Arch packages, and our Mesa
# is built against the repo's LLVM, which a partial update may move ahead of it. An update, an app install pulling a dependency, or a full
# -Syu could overwrite or remove them: the session would not start (black screen).
#
#   save    record the current version of every protected file (copy in /opt/lotfix/protect/store)
#   check   (pacman PostTransaction hook) put back any protected file that was replaced or removed, then verify
#           that every library our builds load can be found; report problems
#
# Protected: the paths in PATHS that exist at save time, plus every symlink among them (kept as links).
import hashlib
import os
import shutil
import subprocess
import sys

STORE = "/opt/lotfix/protect/store"
MANIFEST = "/opt/lotfix/protect/manifest"
PATHS = [
    # Mesa for Adreno/KGSL (EGL, GLX, GBM, gallium, Turnip, VA bridge)
    "/usr/lib/libEGL_mesa.so.0.0.0", "/usr/lib/libGLX_mesa.so.0.0.0", "/usr/lib/libgbm.so.1.0.0",
    "/usr/lib/dri/libdril_dri.so", "/usr/lib/gbm/dri_gbm.so", "/usr/lib/libvulkan_freedreno.so",
    "/usr/lib/libgallium-26.3.0-devel.so", "/usr/lib/dri/termuxva_drv_video.so",
    "/usr/lib/libtva_drm_shim_wayland.so",
    # Hyprland + aquamarine with our patches
    "/usr/bin/Hyprland", "/usr/bin/hyprctl", "/usr/bin/start-hyprland", "/usr/lib/libaquamarine.so.0.15.1",
]
# ELF files whose dependencies are checked after each transaction
CHECK_DEPS = ["/usr/lib/libgallium-26.3.0-devel.so", "/usr/lib/libEGL_mesa.so.0.0.0", "/usr/lib/libvulkan_freedreno.so",
              "/usr/bin/Hyprland", "/usr/lib/libaquamarine.so.0.15.1"]
LIBDIRS = ["/usr/lib", "/opt/lotfix/lib"]


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def entry(path):
    """'link:<target>' for a symlink, the sha256 for a file, None if missing."""
    if os.path.islink(path):
        return "link:" + os.readlink(path)
    if os.path.isfile(path):
        return sha(path)
    return None


def save():
    os.makedirs(STORE, exist_ok=True)
    lines = []
    for p in PATHS:
        e = entry(p)
        if e is None:
            continue
        if not e.startswith("link:"):
            dst = STORE + p
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(p, dst)
        lines.append(f"{p}\t{e}\n")
    with open(MANIFEST, "w") as f:
        f.writelines(lines)
    print(f"andromarchy-protect: {len(lines)} files recorded")


def restore(path, want):
    if os.path.lexists(path):
        os.unlink(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if want.startswith("link:"):
        os.symlink(want[5:], path)
    else:
        shutil.copy2(STORE + path, path)


def needed(path):
    try:
        out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    except OSError:
        return []
    return [l.split("[", 1)[1].rstrip("]") for l in out.splitlines() if "(NEEDED)" in l]


def check():
    if not os.path.exists(MANIFEST):
        return 0
    restored, problems = [], []
    for line in open(MANIFEST):
        path, want = line.rstrip("\n").split("\t")
        if entry(path) != want:
            restore(path, want)
            restored.append(path)
    for p in CHECK_DEPS:
        if os.path.exists(p):
            for lib in needed(p):
                if not any(os.path.exists(os.path.join(d, lib)) for d in LIBDIRS):
                    problems.append(f"{p} needs {lib}, which is missing")
    if restored:
        print("andromarchy-protect: restored Andromarchy's own version of:")
        for p in restored:
            print("   ", p)
    if problems:
        msg = "\n".join(problems)
        print("andromarchy-protect: WARNING, the desktop may not start after this transaction:\n" + msg)
        subprocess.run(["notify-send", "-u", "critical", "Andromarchy: a system update broke a dependency", msg],
                       stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    return 0  # never fail the transaction: pacman has already changed the system


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "save":
        save()
    elif cmd == "check":
        sys.exit(check())
    else:
        sys.exit("usage: andromarchy-protect save|check")
