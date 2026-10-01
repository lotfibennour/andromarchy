# Log the syscalls trapped while fzf walks a directory (no proot): arch-native.sh /tmp/fzf-trace.sh
GO_SYSCALLS_TRACE=1 timeout 20 fzf --walker-root /usr/share/omarchy/shell --filter qml 2>&1 >/dev/null |
  grep go-trap | grep -v 'nr=135 ' | head -25
