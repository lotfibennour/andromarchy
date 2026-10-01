#!/usr/bin/env python3
"""Binary-patch Arch's own glibc so it runs under Android's app seccomp filter without proot.

Android kills (SIGSYS) a few syscalls glibc issues unconditionally. glibc already copes with them being
unavailable (ENOSYS, as on old kernels), so each inline "mov x8, #NR ; svc #0" for those syscalls becomes
"mov x8, #NR ; movn x0, #37" (x0 = -38 = -ENOSYS): no syscall, same register contract.

Why not Termux's patched glibc: Qt's QML JS engine aborts with heap corruption on it
("corrupted size vs. prev_size" in QV4::ExecutionEngine), while Arch's glibc works.

Usage: patch-glibc-syscalls.py SRC_DIR DST_DIR   (copies ld-linux-aarch64.so.1 + libc.so.6, patches the copies)
"""
import os
import shutil
import struct
import sys

# aarch64 syscall numbers Android's app filter rejects but glibc tolerates as ENOSYS.
# Found one by one with strace (SIGSYS, si_code=SYS_SECCOMP); each has an ENOSYS fallback in glibc.
BLOCKED = {"set_robust_list": 99, "rseq": 293, "faccessat2": 439, "setfsuid": 151, "setfsgid": 152}
SVC0 = struct.pack("<I", 0xD4000001)
MOVN_X0_ENOSYS = struct.pack("<I", 0x92800000 | (37 << 5))  # movn x0, #37  -> x0 = -38


def movz_nr(nr):
    # "mov x8, #nr" or "mov w8, #nr" (glibc uses either)
    return [struct.pack("<I", base | (nr << 5) | 8) for base in (0xD2800000, 0x52800000)]


def patch(path):
    data = bytearray(open(path, "rb").read())
    total = 0
    for name, nr in BLOCKED.items():
        for needle in movz_nr(nr):
            start = 0
            while (i := data.find(needle, start)) != -1:
                start = i + 4
                if i % 4:
                    continue
                # the svc follows once the args are set up (7 instructions later in ld.so's TLS init);
                # stop if x8 is reloaded first: that svc would be another syscall.
                for k in range(1, 13):
                    j = i + 4 * k
                    insn = struct.unpack("<I", data[j : j + 4])[0]
                    if data[j : j + 4] == SVC0:
                        data[j : j + 4] = MOVN_X0_ENOSYS
                        total += 1
                        break
                    if insn & 0x1F == 8 and (insn & 0x7F800000) in (0x52800000, 0x12800000, 0x72800000):
                        break  # movz/movn/movk into x8/w8
        print(f"{os.path.basename(path)}: {name} sites patched: {total}")
    open(path, "wb").write(data)
    return total


src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
for lib in ("ld-linux-aarch64.so.1", "libc.so.6"):
    # patch a temp copy then rename over: the live file may be busy (running processes map it)
    tmp = os.path.join(dst, lib + ".tmp")
    shutil.copy2(os.path.join(src, lib), tmp)
    patch(tmp)
    os.replace(tmp, os.path.join(dst, lib))
