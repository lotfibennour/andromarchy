#!/bin/bash
# omarchy-dns for Android/Andromarchy (replaces Omarchy's, which drives NetworkManager + systemd-resolved): DNS for
# every Linux program is the rootfs's /etc/resolv.conf (android-compat points glibc's resolver at it), so the provider
# is written there directly. DHCP = the DNS servers of Android's current network ($XDG_RUNTIME_DIR/dns, from the app;
# android-nm keeps them current when the network changes).
# Usage: omarchy-dns [Cloudflare|Google|DHCP|Custom]   (no argument: print the current provider)
set -euo pipefail
R=/etc/resolv.conf

write() { # <tag> <servers...>
  local tag=$1 tmp
  shift
  tmp=$(mktemp /etc/.resolv.XXXXXX)
  { echo "# andromarchy-dns: $tag"; printf 'nameserver %s\n' "$@"; } > "$tmp"
  chmod 644 "$tmp" && mv -f "$tmp" "$R"
}

case ${1:-} in
  "")
    tag=$(sed -n 's/^# andromarchy-dns: //p' "$R" 2>/dev/null)
    if [ -n "$tag" ]; then echo "$tag"
    elif [ "$(awk '/^nameserver/ {print $2; exit}' "$R" 2>/dev/null)" = 1.1.1.1 ]; then echo Cloudflare
    elif [ "$(awk '/^nameserver/ {print $2; exit}' "$R" 2>/dev/null)" = 8.8.8.8 ]; then echo Google
    else echo Custom
    fi ;;
  Cloudflare | cloudflare) write Cloudflare 1.1.1.1 1.0.0.1 ;;
  Google | google) write Google 8.8.8.8 8.8.4.4 ;;
  DHCP | dhcp)
    read -ra servers < "${XDG_RUNTIME_DIR:-/nonexistent}/dns" 2>/dev/null || servers=()
    [ ${#servers[@]} -gt 0 ] || servers=(8.8.8.8 1.1.1.1) # no network yet
    write DHCP "${servers[@]}" ;;
  Custom | custom)
    echo "Enter your DNS servers (space-separated, e.g. '192.168.1.1 1.1.1.1'):"
    read -ra servers || true
    [ ${#servers[@]} -gt 0 ] || { echo "Error: No DNS servers provided." >&2; exit 1; }
    write Custom "${servers[@]}" ;;
  *) echo "Usage: omarchy-dns [Cloudflare|Google|DHCP|Custom]" >&2; exit 1 ;;
esac
