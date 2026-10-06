#!/bin/sh
# flatpak-spawn inside Andromarchy's Flatpak sandboxes (first in their PATH, /run/host/andromarchy-bin): a nested
# sandbox (--sandbox) is asked of Flatpak's portal, which refuses here (it identifies apps by an isolated root that
# Android cannot give them). glycin, GTK's image loader, uses it for every icon: GTK aborted on a missing icon and
# took the app down (Zotero's file dialogs). Run the command directly in the same sandbox, like the host's bwrap
# stand-in does: environment and directory options still apply; forwarded fds are simply inherited.
while [ $# -gt 0 ]; do
  case $1 in
    --env=*) v=${1#--env=}; export "$v"; shift ;;
    --unset-env=*) unset "${1#--unset-env=}"; shift ;;
    --directory=*) cd "${1#--directory=}" || exit 1; shift ;;
    --clear-env) shift ;; # keep the runtime's variables: without them the command cannot start here
    --) shift; break ;;
    --*) shift ;;         # --sandbox, --watch-bus, --forward-fd=N, --no-network, --sandbox-expose*, --host...
    *) break ;;
  esac
done
exec "$@"
