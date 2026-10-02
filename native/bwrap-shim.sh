#!/bin/bash
# /usr/local/bin/bwrap for Android/Andromarchy: bubblewrap needs user namespaces, which Android denies to apps.
# GTK's image loaders (glycin) run every decoder through it: the loader died, GTK aborted on a missing icon (Brave's
# "Save as" dialog). Run the command directly: bind mounts, tmpfs and namespaces are dropped (no isolation, the same
# trade-off as the browsers' sandboxes); --setenv/--unsetenv/--clearenv/--chdir still apply.
chdir=
while [ $# -gt 0 ]; do
  case $1 in
    --) shift; break ;;
    --setenv) export "$2=$3"; shift 3 ;;
    --unsetenv) unset "$2"; shift 2 ;;
    # keeps the no-proot runtime (loader, preloads, fakechroot, locales): without it the command cannot even start
    --clearenv)
      for v in $(compgen -e); do
        case $v in LD_*|FAKECHROOT_*|FAKEROOTKEY|PATH|LOCPATH|GCONV_PATH) ;; *) unset "$v" ;; esac
      done
      shift ;;
    --chdir) chdir=$2; shift 2 ;;
    # options taking two arguments
    --bind|--bind-try|--ro-bind|--ro-bind-try|--dev-bind|--dev-bind-try|--symlink|--file|--bind-data|--ro-bind-data|\
    --chmod|--ro-overlay) shift 3 ;;
    --overlay) shift 4 ;;
    # options taking one argument
    --dev|--proc|--tmpfs|--mqueue|--dir|--remount-ro|--seccomp|--add-seccomp-fd|--userns|--userns2|--pidns|--uid|\
    --gid|--hostname|--perms|--size|--info-fd|--json-status-fd|--block-fd|--userns-block-fd|--sync-fd|--lock-file|\
    --exec-label|--file-label|--cap-add|--cap-drop|--argv0|--overlay-src|--tmp-overlay|--args) shift 2 ;;
    --*) shift ;; # flags: --unshare-*, --share-net, --die-with-parent, --new-session, --as-pid-1...
    *) break ;;
  esac
done
[ -n "$chdir" ] && cd "$chdir"
exec "$@"
