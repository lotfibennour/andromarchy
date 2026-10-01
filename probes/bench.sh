# Same commands are reused for the no-proot runs (phase 2).
t() { local s=$(date +%s.%N); "$@" >/dev/null 2>&1; echo "$(echo "$(date +%s.%N) - $s" | bc) s  $*"; }
printf "int main(){return 0;}\n" > /tmp/b.c
t ls -lR /usr/share/icons
t bash -c "for i in \$(seq 200); do /bin/true; done"
t pacman -Q
t gcc -O2 -o /tmp/b /tmp/b.c
t python -c "import json, os, re"
