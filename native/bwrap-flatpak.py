#!/usr/bin/env python3
# /usr/local/bin/bwrap for Android/Andromarchy. bubblewrap needs user/mount namespaces; Android's kernel has user
# namespaces disabled (unshare(CLONE_NEWUSER): EINVAL), so no real bwrap can run.
#  - Flatpak-style calls (a runtime bound at /usr or an app at /app): the container is built as a directory tree of
#    relative symlinks (design of selkies-project/proot-bwrap's fakechroot mode, MPL-2.0) inside the rootfs, and the
#    command runs through fakechroot with that tree as its root, loaded by the RUNTIME's own glibc, patched against
#    the syscalls Android kills (patch-glibc-syscalls.py), since it is not Arch's.
#  - Any other caller (GTK's glycin loaders: identity binds only): the old behavior, run the command directly
#    (/usr/local/libexec/bwrap-legacy, native/bwrap-shim.sh).
# No isolation either way: paths are confined, privileges are not (the same trade-off as the browsers' sandboxes).
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOTFS = os.environ.get("FAKECHROOT_BASE", "")  # the main rootfs (real path)
RUN = os.environ.get("XDG_RUNTIME_DIR", "")
LEGACY = "/usr/local/libexec/bwrap-legacy"
STAGE = "/var/tmp/bwrap-flatpak"  # trees live inside the rootfs: their relative links reach every rootfs path

ZERO = {"--unshare-all", "--unshare-user", "--unshare-user-try", "--unshare-ipc", "--unshare-pid", "--unshare-net",
        "--unshare-uts", "--unshare-cgroup", "--unshare-cgroup-try", "--share-net", "--die-with-parent",
        "--new-session", "--as-pid-1", "--disable-userns", "--assert-userns-disabled", "--clearenv"}
ONE = {"--chdir", "--dir", "--proc", "--dev", "--mqueue", "--tmpfs", "--remount-ro", "--seccomp", "--add-seccomp-fd",
       "--userns", "--userns2", "--pidns", "--uid", "--gid", "--hostname", "--info-fd", "--json-status-fd",
       "--block-fd", "--userns-block-fd", "--sync-fd", "--lock-file", "--exec-label", "--file-label", "--cap-add",
       "--cap-drop", "--argv0", "--perms", "--size", "--unsetenv", "--overlay-src", "--tmp-overlay", "--ro-overlay"}
TWO = {"--bind", "--bind-try", "--ro-bind", "--ro-bind-try", "--dev-bind", "--dev-bind-try", "--bind-fd",
       "--ro-bind-fd", "--symlink", "--file", "--bind-data", "--ro-bind-data", "--chmod", "--setenv", "--overlay"}
BINDS = {"--bind", "--bind-try", "--ro-bind", "--ro-bind-try", "--dev-bind", "--dev-bind-try", "--bind-fd",
         "--ro-bind-fd"}


def debug(msg):
    """BWRAP_DEBUG=1 or a /var/tmp/bwrap-debug.on file (Flatpak's ldconfig call has an empty environment):
    decisions appended to /var/tmp/bwrap-debug.log (Flatpak captures some calls' stderr)."""
    if os.environ.get("BWRAP_DEBUG") or os.path.exists("/var/tmp/bwrap-debug.on"):
        with open("/var/tmp/bwrap-debug.log", "a") as f:
            f.write(msg.rstrip(chr(10)) + chr(10))


def read_fd(fd):
    data, off = b"", 0
    while True:
        try:
            chunk = os.pread(fd, 65536, off)
        except OSError:  # a pipe: plain reads
            chunk = os.read(fd, 65536)
        if not chunk:
            return data
        data += chunk
        off += len(chunk)


def parse(argv):
    """[(option, [values])] in order, and the command."""
    ops, i = [], 0
    while i < len(argv):
        a = argv[i]
        if a == "--":
            return ops, argv[i + 1:]
        if a == "--args":
            extra = read_fd(int(argv[i + 1])).decode().split("\0")
            if extra and extra[-1] == "":
                extra.pop()  # the trailing NUL (empty values like --setenv VAR "" stay)
            argv = argv[:i] + extra + argv[i + 2:]
            continue
        if a in ZERO:
            ops.append((a, []))
            i += 1
        elif a in ONE:
            ops.append((a, argv[i + 1:i + 2]))
            i += 2
        elif a in TWO:
            ops.append((a, argv[i + 1:i + 3]))
            i += 3
        elif a.startswith("--"):
            sys.stderr.write(f"bwrap: unknown option {a} (next: {argv[i + 1] if i + 1 < len(argv) else ''})\n")
            ops.append((a, []))  # unknown flag: tolerated
            i += 1
        else:
            return ops, argv[i:]
    return ops, []


def real(guest):
    """The real path of a guest path: excluded trees are themselves, the rest is inside the rootfs."""
    for e in ("/dev", "/proc", "/sys", RUN, "/data/"):
        if e and (guest == e or guest.startswith(e.rstrip("/") + "/")):
            return guest
    return ROOTFS + guest


def canon(p):
    """Android names the app's data twice: /data/data/<pkg> and /data/user/0/<pkg> (a link). One form to compare."""
    p = os.path.normpath(p)
    return "/data/user/0/" + p[len("/data/data/"):] if p.startswith("/data/data/") else p


class Tree:
    def __init__(self, path):
        self.path = path  # guest path of the tree root
        self.usr_source = None  # what /usr is bound to (the runtime), for the patched-glibc cache key
        self.excluded = ["/dev", "/proc", "/sys"] + ([RUN] if RUN else [])
        os.makedirs(path)

    def here(self, cpath):
        return self.path + os.path.normpath("/" + cpath.lstrip("/"))  # (normpath keeps a leading "//")

    def mirror(self, link):
        """A symlink to a directory becomes a directory of links to its entries (so the tree can add its own)."""
        # bind links hold real host paths (older trees: relative between real paths): back to a guest path to list it
        t = os.readlink(link)  # an absolute link reads back as a guest path (the wrapper strips the rootfs)
        if t.startswith("/"):
            target, target_real = t, real(t)
        else:
            target_real = os.path.normpath(os.path.join(real(os.path.dirname(link)), t))
            target = target_real[len(ROOTFS):] if target_real.startswith(ROOTFS + "/") else target_real
        entries = os.listdir(target)
        os.unlink(link)
        os.mkdir(link, 0o755)
        for e in entries:
            host = os.path.join(target, e)
            t = os.readlink(host) if os.path.islink(host) else None
            if t is not None and not t.startswith("/"):
                os.symlink(t, os.path.join(link, e))  # relative link: means the container's path, keep it as is
            else:
                raw_symlink(os.path.join(target_real, e), os.path.join(link, e))

    def place(self, cpath):
        """The tree path for a container path, every parent a real directory of the tree."""
        parts = [p for p in cpath.split("/") if p and p != "."]
        cur = self.path
        for name in parts[:-1]:
            nxt = os.path.join(cur, name)
            hops = 0
            while os.path.islink(nxt) and hops < 16:
                t = os.readlink(nxt)
                dest = os.path.normpath(os.path.join(cur, t))
                if dest == self.path or dest.startswith(self.path + "/"):
                    nxt = dest  # a link of the tree itself (/lib -> usr/lib): follow it
                    hops += 1
                    continue
                debug(f"bwrap: mirror {nxt} for {cpath}")
                self.mirror(nxt)  # a bound host directory: give the tree its own copy of the level
                break
            if not os.path.lexists(nxt):
                os.mkdir(nxt, 0o755)
            cur = nxt
        return os.path.join(cur, parts[-1]) if parts else cur

    @staticmethod
    def clear(p):
        if os.path.islink(p) or (os.path.lexists(p) and not os.path.isdir(p)):
            os.unlink(p)
        elif os.path.isdir(p):
            shutil.rmtree(p)

    def host_of(self, cpath):
        """The real host path a container path reaches through a bind link among its parents, or None."""
        parts = [p for p in cpath.split("/") if p and p != "."]
        cur = self.path
        for i, name in enumerate(parts):
            nxt = os.path.join(cur, name)
            hops = 0
            while os.path.islink(nxt) and hops < 16:
                t = os.readlink(nxt)
                if t.startswith("/") and not (t == self.path or t.startswith(self.path + "/")):
                    return os.path.normpath(os.path.join(real(t), *parts[i + 1:]))  # a bind: host path from here
                nxt = os.path.normpath(os.path.join(cur, t))
                hops += 1
            if not os.path.isdir(nxt):
                return None
            cur = nxt
        return None

    def bind(self, src, dest):
        src, dest = os.path.normpath(src), os.path.normpath(dest)
        if any(dest == e or dest.startswith(e + "/") for e in self.excluded):
            return  # passed through as is
        if dest == "/usr":
            self.usr_source = src
        # Under a bound host directory (the home folder for filesystems=home), don't copy that level into the tree:
        # what the app then created there (Zotero's library, ~/Zotero) stayed in the temporary tree and was lost.
        host = self.host_of(dest)
        if host is not None:
            if canon(host) == canon(real(src)):
                return  # the same place already (~/.var/app/<id> onto itself)
            private = os.path.normpath(real(os.path.expanduser("~/.var/app")))
            if canon(host).startswith(canon(private) + "/"):  # the app's own data: the link goes there, as a bind would
                rf = canon(ROOTFS)
                g = canon(host)[len(rf):] if canon(host).startswith(rf + "/") else host
                if not os.path.lexists(g):
                    os.makedirs(os.path.dirname(g), exist_ok=True)
                    raw_symlink(real(src), g)
                debug(f"bwrap: bind {src} -> {dest} in place: {g}")
                return
        p = self.place(dest)
        self.clear(p)
        raw_symlink(real(src), p)  # absolute host path: "/data" is passed through inside the container
        debug(f"bwrap: bind {src} -> {dest} at {p}: {os.readlink(p)}")

    def symlink(self, target, dest):
        p = self.place(dest)
        if os.path.lexists(p):
            self.clear(p)
        if target.startswith("/"):  # absolute inside the container = relative within the tree
            target = os.path.relpath(target, os.path.dirname(os.path.normpath(dest))) or "."
        os.symlink(target, p)

    def data(self, content, dest, mode):
        p = self.place(dest)
        self.clear(p)
        with open(p, "wb") as f:
            f.write(content)
        os.chmod(p, mode)

    def directory(self, dest, mode, empty=False):
        if any(os.path.normpath(dest) == e for e in self.excluded):
            os.makedirs(self.here(dest), exist_ok=True)
            return
        host = self.host_of(dest)
        if host is not None and empty:
            # A mask (--tmpfs) over part of a bound host directory: Flatpak hides ~/.local/share/flatpak and
            # ~/.var/app from apps that see the home folder. It cannot be done in place (that would hide or empty the
            # real folders), and copying the home folder into the tree lost what the app created there. No isolation
            # here anyway: skip it.
            debug(f"bwrap: mask {dest} skipped (inside {host})")
            return
        if host is not None:  # under a bound host directory (~/.var/app/<id>/cache...): there, not a tree copy
            rf = canon(ROOTFS)
            g = canon(host)[len(rf):] if canon(host).startswith(rf + "/") else host
            os.makedirs(g, exist_ok=True)
            debug(f"bwrap: dir {dest} in place: {g}")
            return
        p = self.place(dest)
        if empty:  # --tmpfs masks whatever was there
            self.clear(p)
        elif os.path.lexists(p):  # --dir on an existing path (a bind included) only ensures it exists
            return
        os.makedirs(p, exist_ok=True)
        if mode is not None:
            os.chmod(p, mode)


def fd_path(fd):
    """The guest path of an inherited fd. The kernel's /proc link, read with libc's own readlinkat (the preloaded
    wrappers answered "/usr" for Flatpak's runtime fd), minus the rootfs prefix."""
    import ctypes
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    buf = ctypes.create_string_buffer(4096)
    n = libc.syscall(78, -100, f"/proc/self/fd/{fd}".encode(), buf, 4095)  # SYS_readlinkat
    p = buf.raw[:n].decode() if n > 0 else os.readlink(f"/proc/self/fd/{fd}")
    return p[len(ROOTFS):] if ROOTFS and p.startswith(ROOTFS + "/") else p


def raw_symlink(target, guest_link):
    """A link to a real host path, created with libc's own symlinkat: the preloaded wrapper would rewrite the target
    relative to the rootfs, which a container's realpath cannot follow (Zotero: readlink -f "$0" gave ".")."""
    import ctypes
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    if libc.syscall(36, target.encode(), -100, real(guest_link).encode()) != 0:  # SYS_symlinkat, AT_FDCWD
        raise OSError(ctypes.get_errno(), f"symlink {guest_link} -> {target}")


def raw_execve(path, argv, env):
    """execve straight to the kernel: this process runs under the main rootfs's fakechroot, whose execve would
    rewrite the (real) loader path and run it through Arch's loader. libc's own syscall(), not the preloaded one."""
    import ctypes
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    arr = lambda xs: (ctypes.c_char_p * (len(xs) + 1))(*[x.encode() for x in xs], None)
    libc.syscall(221, path.encode(), arr(argv), arr([f"{k}={v}" for k, v in env.items()]))  # SYS_execve
    sys.stderr.write(f"bwrap: execve {path}: errno {ctypes.get_errno()}\n")


def runtime_glibc(tree):
    """The runtime's ld.so + libc, patched for Android's syscall filter (cached per runtime: keyed by where the
    tree's /usr points; fakechroot's realpath cannot follow the tree's relative links)."""
    key = tree.usr_source or tree.here("/usr")  # the runtime's own path: stable across launches
    d = "/opt/lotfix/runtime-glibc/" + hashlib.sha1(key.encode()).hexdigest()[:16]
    if not os.path.exists(d + "/libc.so.6"):
        shutil.rmtree(d, ignore_errors=True)
        subprocess.run(["python3", "/opt/lotfix/patch-glibc-syscalls.py", tree.here("/usr/lib/aarch64-linux-gnu"), d],
                       check=True, stdout=subprocess.DEVNULL)
    return d


HOST_GL = "/opt/lotfix/host-gl"
SPAWN_DIR = "/usr/local/libexec/andromarchy-flatpak"  # flatpak-spawn for inside the sandboxes (native/flatpak-spawn.sh)


GECKO_PREFS = ('user_pref("gfx.webrender.all", true);\n'
               'user_pref("layers.acceleration.force-enabled", true);\n')


def gecko_gpu_prefs():
    """Firefox-based apps (Zotero, Thunderbird, Firefox, LibreWolf) test the GPU themselves and reject ours (no PCI
    device, Mesa reports "not accelerated"): WebRender fell back to software. Every Gecko profile (a directory with
    prefs.js) gets the prefs that force it on, in its user.js: under ~/.var/app for confined apps, and in the home
    folder's own profile directories for apps that see it (Zotero: ~/.zotero)."""
    for base in [os.path.expanduser(d) for d in ("~/.var/app", "~/.zotero", "~/.mozilla", "~/.thunderbird",
                                                  "~/.librewolf")]:
        for root, dirs, files in os.walk(base) if os.path.isdir(base) else []:
            if root.count(os.sep) - base.count(os.sep) > 5:
                dirs[:] = []
                continue
            if "prefs.js" in files:
                user = os.path.join(root, "user.js")
                text = open(user).read() if os.path.exists(user) else ""
                if "gfx.webrender.all" not in text:
                    with open(user, "a") as f:
                        f.write(("\n" if text and not text.endswith("\n") else "") + GECKO_PREFS)


def host_gl():
    """Our Mesa (KGSL: the Adreno through the only GPU interface Android gives apps) for Flatpak apps, instead of
    their runtime's (Flathub's upstream Mesa: no GPU here, it fell back to llvmpipe on the CPU). A dir of links to our
    EGL/GBM/gallium/Vulkan libraries, put first on the app's library path, and a Vulkan ICD naming our driver by its
    real path. ponytail: our Mesa is built against Arch's libraries; ANDROMARCHY_FLATPAK_GL=runtime falls back."""
    if not os.path.exists(HOST_GL + "/freedreno_icd.json"):
        os.makedirs(HOST_GL, exist_ok=True)
        for n in os.listdir("/usr/lib"):
            if n.startswith(("libEGL_mesa.so", "libGLX_mesa.so", "libgallium-", "libgbm.so", "libvulkan_freedreno")):
                link = os.path.join(HOST_GL, n)
                if not os.path.lexists(link):
                    os.symlink(os.path.relpath("/usr/lib/" + n, HOST_GL), link)
        with open(HOST_GL + "/freedreno_icd.json", "w") as f:
            json.dump({"ICD": {"api_version": "1.4.359", "library_arch": "64",
                               "library_path": real("/usr/lib/libvulkan_freedreno.so")},
                       "file_format_version": "1.0.1"}, f)
    return {
        "__EGL_VENDOR_LIBRARY_FILENAMES": real("/usr/share/glvnd/egl_vendor.d/50_mesa.json"),
        "LIBGL_DRIVERS_PATH": real("/usr/lib/dri"),
        "GBM_BACKENDS_PATH": real("/usr/lib/gbm"),
        "VK_DRIVER_FILES": real(HOST_GL + "/freedreno_icd.json"),
        "VK_ICD_FILENAMES": real(HOST_GL + "/freedreno_icd.json"),
    }


def ld_conf_dirs(tree):
    """Library dirs named by the container's ld.so.conf files (Flatpak passes them as data)."""
    dirs, todo, seen = [], [tree.here("/etc/ld.so.conf")], set()
    while todo:
        f = todo.pop(0)
        if f in seen or not os.path.isfile(f):
            continue
        seen.add(f)
        for line in open(f, errors="replace"):
            line = line.split("#")[0].strip()
            if line.startswith("include "):
                import glob
                todo += sorted(glob.glob(tree.here(line.split(None, 1)[1])))
            elif line.startswith("/"):
                dirs.append(line)
    return dirs


def main():
    ops, command = parse(sys.argv[1:])
    # A container needs its own tree only when /usr is a runtime or an app is at /app. Flatpak's other calls (its
    # D-Bus proxy runs with the host's own /usr bound at /usr) and glycin's (identity binds) just run directly.
    def source(o, v):
        return fd_path(int(v[0])) if o.endswith("-fd") else v[0]
    flatpak = any(o in BINDS and (v[-1] == "/app" or (v[-1] == "/usr" and os.path.normpath(source(o, v)) != "/usr"))
                  for o, v in ops)
    if True:
        debug(f"bwrap: flatpak={flatpak} rootfs={ROOTFS} command={command[:3]}\n")
    if not flatpak or not ROOTFS:
        os.execv(LEGACY, [LEGACY] + sys.argv[1:])
    if not command:
        sys.exit("bwrap: no command")

    # Deployments checked out by libostree had their absolute links made relative to the wrong root (its temporary
    # checkout dir): repaired once per deployment, the first time it runs (flatpak-fix-links.py).
    for o, v in ops:
        if o in BINDS and v[-1] in ("/usr", "/app"):
            files = source(o, v)
            stamp = os.path.join(os.path.dirname(files), ".android-links-fixed")
            if files.endswith("/files") and not os.path.exists(stamp):
                subprocess.run(["python3", "/opt/lotfix/flatpak-fix-links.py", files], stdout=subprocess.DEVNULL)
                open(stamp, "w").close()

    os.makedirs(STAGE, exist_ok=True)
    tree = Tree(f"{STAGE}/{os.getpid()}")
    env, cwd, perms, argv0 = dict(os.environ), None, None, None
    info_fds, status_fds = [], []
    try:
        for o, v in ops:
            if o in BINDS:
                src = fd_path(int(v[0])) if o.endswith("-fd") else v[0]
                if o.endswith("-try") and not os.path.lexists(src):
                    continue
                tree.bind(src, v[1])
            elif o in ("--proc", "--dev"):
                tree.directory(v[0], None)
            elif o in ("--tmpfs", "--dir", "--mqueue"):
                tree.directory(v[0], perms, empty=(o == "--tmpfs"))
            elif o == "--symlink":
                tree.symlink(v[0], v[1])
            elif o in ("--file", "--bind-data", "--ro-bind-data"):
                tree.data(read_fd(int(v[0])), v[1], perms if perms is not None else 0o644)
                os.close(int(v[0]))
            elif o == "--chmod":
                os.chmod(tree.place(v[1]), int(v[0], 8))
            elif o == "--setenv":
                env[v[0]] = v[1]
            elif o == "--unsetenv":
                env.pop(v[0], None)
            elif o == "--clearenv":
                env = {}
            elif o == "--chdir":
                cwd = v[0]
            elif o == "--argv0":
                argv0 = v[0]
            elif o == "--info-fd":
                info_fds.append(int(v[0]))
            elif o == "--json-status-fd":
                status_fds.append(int(v[0]))
            elif o == "--block-fd":
                read_fd(int(v[0]))
            if o != "--perms":
                perms = None
            else:
                perms = int(v[0], 8)

        # Directories bound under /run are Flatpak's per-app ones (XDG_RUNTIME_DIR...), holding relative links meant
        # for the container (wayland-1 -> ../../flatpak/wayland-1): mirror them so those links resolve in the tree.
        for o, v in ops:
            if o in BINDS and v[-1].startswith("/run/"):
                p = tree.here(v[-1])
                # only those with relative links: a mirrored folder is the tree's own copy, and writes into it
                # (Flatpak's ld.so cache dir) would no longer reach the bound folder
                if os.path.islink(p) and os.path.isdir(p) and any(
                        os.path.islink(os.path.join(p, e)) and not os.readlink(os.path.join(p, e)).startswith("/")
                        for e in os.listdir(p)):
                    tree.mirror(p)

        # Flatpak's first call only builds the runtime's ld.so.cache: our loader is given the library dirs instead.
        if os.path.basename(command[0]) == "ldconfig" and "-C" in command:
            cache = command[command.index("-C") + 1]
            open(tree.here(cache), "w").close()
            if True:
                d = tree.here(os.path.dirname(cache))
                debug(f"bwrap: ldconfig cache {tree.here(cache)} (dir link -> "
                                 f"{os.readlink(d) if os.path.islink(d) else 'not a link'})\n")
            sys.exit(0)

        # Our flatpak-spawn first in the app's PATH: nested sandboxes (glycin's image loaders) run directly.
        if os.path.isdir(SPAWN_DIR):
            tree.bind(SPAWN_DIR, "/run/host/andromarchy-bin")
            env["PATH"] = "/run/host/andromarchy-bin:" + env.get("PATH", "/app/bin:/usr/bin")
        glibc = runtime_glibc(tree)
        loader = real(glibc + "/ld-linux-aarch64.so.1")
        T = real(tree.path)
        libdirs = [p for p in (env.get("LD_LIBRARY_PATH", "").split(":") + ld_conf_dirs(tree)
                               + ["/usr/lib/aarch64-linux-gnu", "/usr/lib"]) if p.startswith("/")]
        gl = os.environ.get("ANDROMARCHY_FLATPAK_GL", env.get("ANDROMARCHY_FLATPAK_GL", "host")) != "runtime"
        first = [real(glibc)] + ([real(HOST_GL)] if gl else [])
        # last: our own /usr/lib, only for what our Mesa needs and the runtime lacks (its LLVM, ...)
        last = [ROOTFS + "/usr/lib"] if gl else []
        libpath = ":".join(first + [T + p for p in dict.fromkeys(libdirs)] + last)
        if gl:
            env.update(host_gl())
            # Our KGSL Mesa reports MESA_query_renderer "accelerated" = false (no DRM device behind it): Firefox-based
            # apps (Zotero, Thunderbird) read that as software and turn GPU rendering off. Force it.
            env.setdefault("MOZ_ACCELERATED", "1")
            try:
                gecko_gpu_prefs()
            except OSError:
                pass
        compat = real("/opt/lotfix/android-compat.so")
        fc = real("/usr/lib/libfakeroot/fakechroot/libfakechroot.so")
        for k in ("ANDROMARCHY", "ANDROMARCHY_NO_DRM_DEVICE", "TURNIP_KMD", "FD_FORCE_KGSL", "MESA_LOADER_DRIVER_OVERRIDE",
                  "GALLIUM_DRIVER", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "ANDROMARCHY_VENDOR",
                  "ANDROMARCHY_MODEL", "TZ"):
            if k in os.environ and k not in env:
                env[k] = os.environ[k]
        env.update({
            "FAKECHROOT": "true", "FAKECHROOT_BASE": T, "FAKECHROOT_ELFLOADER": loader,
            # "/data": the tree's links to bound folders are real host paths (/data/user/0/...); no container path
            # starts with it, and realpath/exec through them then reach real, existing files
            "FAKECHROOT_EXCLUDE_PATH": ":".join(tree.excluded + ["/data"]),
            "LD_LIBRARY_PATH": libpath, "LD_PRELOAD": f"{compat}:{fc}",
            "GCONV_PATH": T + "/usr/lib/aarch64-linux-gnu/gconv", "TZDIR": T + "/usr/share/zoneinfo",
        })
        for k in ("FAKECHROOT_VERSION",):
            if k in os.environ:
                env[k] = os.environ[k]

        prog = command[0]
        if "/" not in prog:
            for d in env.get("PATH", "/usr/bin").split(":"):
                if os.access(tree.here(os.path.join(d, prog)), os.X_OK):
                    prog = os.path.join(d, prog)
                    break
        args = command[1:]
        with open(tree.here(prog), "rb") as f:  # guest path: this process is still under the main fakechroot
            head = f.read(256)
        if head.startswith(b"#!"):  # a script: its interpreter, from the tree, reads the script by container path
            parts = head[2:].split(b"\n")[0].decode().split(None, 1)
            args = parts[1:] + [prog] + args
            prog = parts[0]
        host_prog = real(tree.here(prog))
        av = [loader, "--library-path", libpath, "--preload", f"{compat} {fc}", host_prog] + args
        if argv0:
            av[5:6] = [host_prog]
        if cwd:
            try:
                os.chdir(tree.here(cwd))
            except OSError:
                pass
        env["PWD"] = cwd or "/"

        pid = os.fork()
        if pid == 0:
            raw_execve(loader, av, env)
            os._exit(127)
        for fd in info_fds + status_fds:
            os.write(fd, (json.dumps({"child-pid": pid}) + "\n").encode())
        for fd in info_fds:
            os.close(fd)
        _, st = os.waitpid(pid, 0)
        code = os.waitstatus_to_exitcode(st)
        code = code if code >= 0 else 128 - code
        for fd in status_fds:
            os.write(fd, (json.dumps({"exit-code": code}) + "\n").encode())
        sys.exit(code)
    finally:
        if not (os.environ.get("BWRAP_KEEP_TREE") or os.path.exists("/var/tmp/bwrap-debug.on")):  # debugging
            shutil.rmtree(tree.path, ignore_errors=True)


if __name__ == "__main__":
    main()
