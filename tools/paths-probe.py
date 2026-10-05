#!/usr/bin/env python3
"""What a Linux desktop reads under /proc, /sys and /dev, as seen by a program in the session (Android's SELinux app
policy + android-compat's fakes). One line per path: OK / EMPTY / DENIED / MISSING / ERR. Run inside the session
(e.g. via `hyprctl dispatch exec`), not from adb: adb's run-as has another SELinux context.
Usage: paths-probe.py > report"""
import errno
import os

PROC = """cpuinfo meminfo stat uptime loadavg version cmdline mounts filesystems swaps partitions diskstats interrupts
vmstat zoneinfo buddyinfo crypto devices misc modules kallsyms cgroups pressure/cpu pressure/memory pressure/io
net/dev net/route net/tcp net/tcp6 net/udp net/unix net/if_inet6 net/arp net/wireless net/snmp
sys/kernel/hostname sys/kernel/osrelease sys/kernel/random/boot_id sys/kernel/random/uuid sys/kernel/random/entropy_avail
sys/kernel/pid_max sys/kernel/threads-max sys/kernel/yama/ptrace_scope sys/kernel/unprivileged_userns_clone
sys/kernel/cap_last_cap sys/kernel/ngroups_max sys/vm/overcommit_memory sys/vm/max_map_count sys/vm/swappiness
sys/fs/file-max sys/fs/nr_open sys/fs/inotify/max_user_watches sys/fs/inotify/max_user_instances
sys/net/core/somaxconn sys/net/ipv4/ip_local_port_range sys/net/ipv6/conf/all/disable_ipv6
self/status self/stat self/statm self/maps self/smaps self/mountinfo self/cgroup self/limits self/environ self/cmdline
self/exe self/cwd self/fd self/fdinfo self/oom_score_adj self/loginuid self/sessionid self/auxv self/io
self/ns/user self/ns/pid self/task self/attr/current self/coredump_filter 1/cmdline 1/status""".split()

SYS = """devices/system/cpu/online devices/system/cpu/possible devices/system/cpu/present
devices/system/cpu/cpu0/cpufreq/scaling_cur_freq devices/system/cpu/cpu0/cpufreq/scaling_max_freq
devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq devices/system/cpu/cpu0/topology/core_id
devices/system/cpu/cpu0/cache/index0/size devices/system/node/online
class/power_supply class/net class/backlight class/drm class/input class/sound class/thermal class/hwmon class/block
class/leds class/tty class/video4linux class/bluetooth class/rfkill class/dmi/id
fs/cgroup fs/cgroup/cgroup.controllers fs/selinux kernel/mm/transparent_hugepage/enabled kernel/random/boot_id
firmware/acpi bus/usb/devices bus/pci/devices module dev/char dev/block devices/virtual/dmi/id/product_name""".split()

DEV = """null zero full random urandom tty ptmx pts shm fuse console stdin stdout stderr fd
dri dri/card0 dri/renderD128 kgsl-3d0 dma_heap dma_heap/system ion ashmem binder hwbinder
snd snd/controlC0 input input/event0 uinput uhid video0 video32 media0 kvm net/tun rfkill hidraw0 i2c-0
loop0 loop-control mapper/control nvidia0 mali0 log kmsg""".split()


def probe(path):
    try:
        st = os.stat(path)
    except OSError as e:
        return {errno.ENOENT: "MISSING", errno.EACCES: "DENIED", errno.EPERM: "DENIED"}.get(e.errno, f"ERR {e.errno}")
    try:
        if os.path.isdir(path):
            n = len(os.listdir(path))
            return "OK" if n else "EMPTY"
        if not (os.path.isfile(path) or path.startswith("/proc")):
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)  # device: opening is the test
            os.close(fd)
            return "OK"
        with open(path, "rb") as f:
            return "OK" if f.read(4096) else "EMPTY"
    except OSError as e:
        return {errno.EACCES: "DENIED", errno.EPERM: "DENIED", errno.ENOENT: "MISSING"}.get(e.errno, f"ERR {e.errno}")


for root, names in (("/proc", PROC), ("/sys", SYS), ("/dev", DEV)):
    for n in names:
        p = f"{root}/{n}"
        print(f"{probe(p):8} {p}")
