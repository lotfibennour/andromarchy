#!/bin/bash
# /usr/local/bin/andromarchy-keep-llvm (pacman PreTransaction hook on llvm-libs): our Mesa is built against one LLVM
# (libLLVM.so.N). An llvm-libs upgrade to another major version removes that library and the session no longer
# starts (black screen, 2026-10-06). Before the upgrade: a private copy of the LLVM our Mesa needs, in /opt/lotfix/lib,
# linked from /usr/lib (andromarchy-protect keeps the link). Mesa then keeps working until it is rebuilt.
set -u
gallium=$(ls /usr/lib/libgallium-*-devel.so 2>/dev/null | head -1)
[ -n "$gallium" ] || exit 0
need=$(readelf -d "$gallium" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(libLLVM[^]]*\)\]/\1/p' | head -1)
[ -n "$need" ] || exit 0
src=/usr/lib/$need
[ -f "$src" ] && [ ! -L "$src" ] || exit 0 # already ours, or not there
mkdir -p /opt/lotfix/lib
cp -a "$src" /opt/lotfix/lib/ || exit 0
echo "andromarchy: kept $need for our Mesa in /opt/lotfix/lib (rebuild Mesa against the new LLVM when convenient)"
# After the transaction, pacman has removed the original: the link takes its place (andromarchy-protect's check runs
# after this hook's PostTransaction companion and records it).
echo "$src" > /opt/lotfix/lib/.restore
