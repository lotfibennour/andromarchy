// lotfix-run: runs the Arch rootfs's bash WITHOUT proot. Native port of native/arch-native.sh (same environment,
// same options), built with the Android NDK so the APK needs no shell of its own.
//   - Arch's own glibc, with the syscalls Android's seccomp kills neutralized, is the loader (<rootfs>/opt/lotfix),
//   - fakechroot (LD_PRELOAD) makes the rootfs look like /, every exec goes back through the same loader,
//   - android-compat.so fills the Android gaps, and with ARCH_ROOT=1 fakeroot (TCP) fakes uid 0.
// Usage: [ARCH_ROOTFS=<dir>] [ARCH_ROOT=1] lotfix-run [bash args...]   e.g. lotfix-run -c 'pacman -Q'
// Environment: ARCH_ROOTFS (required), ARCH_ROOT, ARCH_GPU=zink, ARCH_EXCLUDE_EXTRA, ARCH_LIBPATH_EXTRA, LANG.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH_SZ 4096
#define LIST_SZ 16384

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("lotfix-run: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(127);
}

static void setf(const char *name, const char *fmt, ...)
{
    char v[LIST_SZ];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    setenv(name, v, 1);
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int env_is(const char *name, const char *value)
{
    const char *v = getenv(name);
    return v && strcmp(v, value) == 0;
}

// We never use /etc/ld.so.cache (ldconfig is a no-op here), so the absolute dirs listed in /etc/ld.so.conf.d/*.conf
// (e.g. /usr/lib/libproxy for quickshell, RUNPATH dirs from setup-rootfs.sh) go on the search path explicitly.
static void add_ld_conf_dirs(const char *R, char *libdirs, size_t len)
{
    char d[PATH_SZ];
    snprintf(d, sizeof d, "%s/etc/ld.so.conf.d", R);
    DIR *dir = opendir(d);
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".conf"))
            continue;
        char f[PATH_SZ];
        snprintf(f, sizeof f, "%s/%s", d, e->d_name);
        FILE *fp = fopen(f, "r");
        if (!fp)
            continue;
        char line[PATH_SZ];
        while (fgets(line, sizeof line, fp)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] != '/')
                continue;
            char full[PATH_SZ];
            snprintf(full, sizeof full, "%s%s", R, line);
            if (is_dir(full) && strlen(libdirs) + strlen(full) + 2 < len) {
                strcat(libdirs, ":");
                strcat(libdirs, full);
            }
        }
        fclose(fp);
    }
    closedir(dir);
}

// faked prints "<key>:<pid>" on stdout. It must be a file, not a pipe: the pipe would close once the key is read and
// a later write by the daemon would kill it with SIGPIPE. The ownership database persists in the rootfs.
static pid_t start_faked(const char *R, const char *loader, const char *libpath, const char *compat)
{
    char state[PATH_SZ], keyf[PATH_SZ], faked[PATH_SZ];
    snprintf(state, sizeof state, "%s/var/lib/fakeroot.state", R);
    snprintf(keyf, sizeof keyf, "%s/tmp/.faked.%d", R, (int)getpid());
    snprintf(faked, sizeof faked, "%s/usr/local/bin/faked", R);
    close(open(state, O_WRONLY | O_CREAT | O_CLOEXEC, 0644));

    pid_t p = fork();
    if (p < 0)
        die("fork: %s", strerror(errno));
    if (p == 0) {
        int in = open(state, O_RDONLY), out = open(keyf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (in < 0 || out < 0)
            _exit(127);
        dup2(in, 0);
        dup2(out, 1);
        // faked is a TCP server: it needs android-compat's accept() -> accept4() (seccomp kills plain accept).
        setenv("LD_PRELOAD", compat, 1);
        execl(loader, loader, "--library-path", libpath, faked, "--load", "--save-file", state, (char *)NULL);
        _exit(127);
    }
    int st;
    if (waitpid(p, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st))
        die("faked failed to start");
    char buf[128] = {0};
    FILE *fp = fopen(keyf, "r");
    if (!fp || !fgets(buf, sizeof buf, fp))
        die("faked printed no key");
    fclose(fp);
    unlink(keyf);
    char *colon = strchr(buf, ':');
    if (!colon)
        die("unexpected faked output: %s", buf);
    *colon = 0;
    setenv("FAKEROOTKEY", buf, 1);
    return (pid_t)atoi(colon + 1);
}

int main(int argc, char **argv)
{
    unsetenv("LD_PRELOAD"); // a Bionic preload (e.g. Termux's libtermux-exec.so) cannot load under glibc

    const char *R = getenv("ARCH_ROOTFS");
    if (!R || !*R)
        die("ARCH_ROOTFS is not set");
    char L[PATH_SZ], G[PATH_SZ], loader[PATH_SZ], fc[PATH_SZ], fr[PATH_SZ], compat[PATH_SZ], bash[PATH_SZ];
    snprintf(L, sizeof L, "%s/opt/lotfix", R);
    if (!is_dir(L))
        die("%s missing: not a rootfs from build/build-rootfs.sh", L);
    snprintf(G, sizeof G, "%s/arch-glibc", L);
    snprintf(loader, sizeof loader, "%s/ld-linux-aarch64.so.1", G);
    snprintf(fc, sizeof fc, "%s/usr/lib/libfakeroot/fakechroot/libfakechroot.so", R);
    snprintf(fr, sizeof fr, "%s/usr/local/lib/libfakeroot.so", R);
    snprintf(compat, sizeof compat, "%s/android-compat.so", L);
    snprintf(bash, sizeof bash, "%s/usr/bin/bash", R);

    setenv("FAKECHROOT_BASE", R, 1);
    // No FAKECHROOT_ELFLOADER_OPT_ARGV0: with it, fakechroot 2.20.1 drops the script path when exec'ing a #! script.
    setenv("FAKECHROOT_ELFLOADER", loader, 1);
    // ARCH_EXCLUDE_EXTRA: host paths that must stay host paths (the Andromarchy/Wayland socket dirs).
    const char *ex = getenv("ARCH_EXCLUDE_EXTRA");
    setf("FAKECHROOT_EXCLUDE_PATH", "/dev:/proc:/sys%s%s", ex && *ex ? ":" : "", ex && *ex ? ex : "");

    char libdirs[LIST_SZ];
    snprintf(libdirs, sizeof libdirs, "%s/usr/lib", R);
    add_ld_conf_dirs(R, libdirs, sizeof libdirs);
    const char *lx = getenv("ARCH_LIBPATH_EXTRA");
    char libpath[LIST_SZ];
    snprintf(libpath, sizeof libpath, "%s%s%s:%s", lx && *lx ? lx : "", lx && *lx ? ":" : "", G, libdirs);
    setenv("LD_LIBRARY_PATH", libpath, 1); // same libs for the first process (--library-path) and its children

    setenv("PATH", "/root/.local/bin:/usr/local/bin:/usr/bin", 1);
    setenv("HOME", "/root", 1);
    // The app's account (first-boot.sh: "android", home /root); scripts expect USER (set -u installers). Not
    // "root": apps warn about running as root (Zotero) or change their behaviour. sudo sets USER=root itself.
    setenv("USER", "android", 1);
    setenv("LOGNAME", "android", 1);
    setenv("SHELL", "/usr/bin/bash", 1); // Termux's own would leak in (terminals spawn $SHELL)
    setenv("TMPDIR", "/tmp", 1);
    unsetenv("PREFIX");
    unsetenv("ANDROID_ROOT_TERMUX");
    // glibc opens locales and gconv modules internally (no fakechroot rewrite): give it real paths. LOCPATH ignores
    // locale-archive, hence the per-locale dirs made at build time.
    setf("LOCPATH", "%s/usr/lib/locale", R);
    setf("GCONV_PATH", "%s/usr/lib/gconv", R);
    setenv("LANG", "C.UTF-8", 0);
    // glibc reads /etc/localtime internally (no fakechroot rewrite): Omarchy showed UTC. TZ comes from Android
    // (SessionService), zone files from the rootfs. AppImages: no FUSE for apps, so they extract and run.
    setf("TZDIR", "%s/usr/share/zoneinfo", R);
    setenv("APPIMAGE_EXTRACT_AND_RUN", "1", 1);

    // GPU for every program: Adreno through KGSL (no DRM node). The ANDROMARCHY* variables make the KGSL Mesa expose
    // dmabuf import/export, which Wayland GPU clients need. ARCH_GPU=zink: OpenGL on Vulkan/Turnip.
    setenv("ANDROMARCHY", "1", 1);
    setenv("ANDROMARCHY_NO_DRM_DEVICE", "1", 1);
    setenv("TURNIP_KMD", "kgsl", 1);
    setenv("FD_FORCE_KGSL", "1", 1);
    setenv("XWAYLAND_FORCE_KGSL_SURFACELESS", "1", 1);
    int zink = env_is("ARCH_GPU", "zink");
    setenv("MESA_LOADER_DRIVER_OVERRIDE", zink ? "zink" : "kgsl", 1);
    setenv("GALLIUM_DRIVER", zink ? "zink" : "freedreno", 1);

    // Firefox's sandbox broker checks fakechroot-rewritten paths against its allow list and denies them (every tab
    // crashes with "no fonts"). Trade-off: web content runs without Firefox's process sandbox.
    setenv("MOZ_DISABLE_CONTENT_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_RDD_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_SOCKET_PROCESS_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_GMP_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_UTILITY_SANDBOX", "1", 1);
    // Same for the other sandbox families, which all need user namespaces or setuid helpers (denied to apps):
    // Electron apps, and WebKitGTK's web process (bubblewrap). Chromium itself: android-compat adds --no-sandbox.
    setenv("ELECTRON_DISABLE_SANDBOX", "1", 1);
    setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1);
    // systemctl is docker-systemctl-replacement: its --user mode defaults to the system multi-user.target.
    setenv("SYSTEMD_DEFAULT_TARGET", "default.target", 1);

    // A cwd outside the rootfs cannot be expressed inside it (getcwd fails): start in the fake $HOME.
    char home[PATH_SZ];
    snprintf(home, sizeof home, "%s/root", R);
    if (chdir(home) < 0)
        die("chdir %s: %s", home, strerror(errno));

    int root = env_is("ARCH_ROOT", "1");
    pid_t faked = root ? start_faked(R, loader, libpath, compat) : 0;
    char preload[LIST_SZ], preload_args[LIST_SZ];
    if (root) { // fakeroot above fakechroot, as in the classic "fakechroot fakeroot" stack
        snprintf(preload, sizeof preload, "%s:%s:%s", compat, fr, fc);
        snprintf(preload_args, sizeof preload_args, "%s %s %s", compat, fr, fc);
    } else {
        snprintf(preload, sizeof preload, "%s:%s", compat, fc);
        snprintf(preload_args, sizeof preload_args, "%s %s", compat, fc);
    }
    setenv("LD_PRELOAD", preload, 1);

    // loader --library-path <libpath> --preload <libs> <rootfs>/usr/bin/bash <args...>
    char **av = calloc(argc + 7, sizeof *av);
    int n = 0;
    av[n++] = loader;
    av[n++] = "--library-path";
    av[n++] = libpath;
    av[n++] = "--preload";
    av[n++] = preload_args;
    av[n++] = bash;
    for (int i = 1; i < argc; i++)
        av[n++] = argv[i];
    av[n] = NULL;

    if (!root) {
        execv(loader, av);
        die("exec %s: %s", loader, strerror(errno));
    }
    // Root mode: stay around to stop faked when the command ends (it would otherwise outlive the session).
    pid_t c = fork();
    if (c < 0)
        die("fork: %s", strerror(errno));
    if (c == 0) {
        execv(loader, av);
        _exit(127);
    }
    signal(SIGINT, SIG_IGN); // the command (in our process group) handles ^C itself
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        ;
    kill(faked, SIGTERM);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
