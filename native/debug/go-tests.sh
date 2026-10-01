# Go programs under arch-native.sh (no proot): run with  arch-native.sh /tmp/go-tests.sh
echo "== gum";    timeout 10 gum --version; echo "gum-exit=$?"
echo "== fzf walker"; fzf --walker-root /usr/share/omarchy/shell --filter qml 2>&1 | head -3; echo "fzf-exit=${PIPESTATUS[0]}"
echo "== fzf stdin";  printf 'shell.qml\nfoo\n' | fzf --filter qml; echo "fzf-exit=$?"
echo "== kitten exec"; printf 'echo "kitten-bash=$BASH_VERSION"\nexit\n' | timeout 10 kitten run-shell --shell=/usr/bin/bash
