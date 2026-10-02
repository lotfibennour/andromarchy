# Lotfix : Omarchy sur Android, sans root ni proot

Une seule APK qui ouvre directement un bureau **Omarchy** (Arch Linux ARM + Hyprland) sur Android, sans root et sans proot.

Appareil cible : POCO Pad 2 Pro (Android 16, Snapdragon/Adreno).

## Choix fixés

- **Pas de proot.** Le shim LD_PRELOAD (fakechroot + fakeroot portés) gère les binaires dynamiques, et un loader `SECCOMP_RET_TRAP` + handler SIGSYS gère les binaires statiques.
- **Affichage :** Anland (GPL-3.0), fusionné avec termux-app dans une seule APK `andro.marchy` (phase 4).
- **Pas de gestion du matériel** : réseau, Bluetooth, luminosité (limite n°8 abandonnée).
- **Abandonnés :** seccomp USER_NOTIF, unshare/namespaces, vrai systemd.

## Spécificités HyperOS (Xiaomi), appareil cible

- **Gestion d'énergie agressive** : tue les apps en arrière-plan. Dev : *wake lock* Termux. APK : foreground service + batterie « Aucune restriction ».
- **Autorisation « Démarrage automatique »** propre à Xiaomi : à demander dans l'onboarding de l'APK.
- **Raccourcis clavier système** : interceptent Super et des combinaisons avant l'app (vu : Super n'arrive jamais). Service d'accessibilité (4.6) + désactiver les raccourcis HyperOS concernés.
- **Redémarrage de l'activité** au branchement d'un clavier ou à la rotation : déconnexions Anland, gérées par la reconnexion du backend.
- **« Paramètres restreints »** pour les APK hors store : bloque l'activation de l'accessibilité tant que l'utilisateur ne l'autorise pas.
- **Option « Optimisation HyperOS/MIUI »** (options développeur) : à vérifier en cas de comportement de permission inexpliqué.
- **Liens système** `/etc -> /system/etc`, `/bin -> /system/bin` : tout calcul de chemin côté hôte doit éviter de les suivre (`realpath -s`).

## Contraintes non négociables

- Projet entier sous **GPL-3.0**, code source public (à cause d'Anland).
- **targetSdk 28** (W^X Android 10+), donc distribution hors Play Store (GitHub/F-Droid).
- **Snapdragon uniquement** (Turnip/Zink via `/dev/kgsl-3d0`).
- L'utilisateur active à la main l'option développeur « Désactiver les restrictions des processus enfants » et le service d'accessibilité du clavier.
- Risque existentiel : si Android relève le targetSdk minimal installable à 29 ou plus, le projet tombe.

---

## Phase 0 : valider l'affichage (le plus risqué, à faire d'abord)

> proot-distro est toléré **uniquement** pour ce test jetable. Il ne fait pas partie du produit.

| # | Étape | Critère de réussite |
|---|---|---|
| 0.1 | anland-termux + Weston sur la tablette. | `weston-terminal` s'affiche. |
| 0.2 | Arch Linux ARM (test jetable) + Hyprland **imbriqué** dans Weston/Anland. | Hyprland plein écran, `kitty` s'ouvre au raccourci. |
| 0.3 | Mesure. | Latence de saisie acceptable, plus de 50 fps en déplaçant une fenêtre. |

**0.1 validée (2026-10-01)** : Weston + `weston-terminal` affichés dans Anland Termux 5.13.3, Termux GitHub, noyau 6.1 android14. Configuration nécessaire :
- paquets : `anland`, `xwayland`, `weston` (release 5.13.3) + Mesa Freedreno de lfdevs (`freedreno-26.3.0-devel-20260824`), bloqués avec `apt-mark hold xwayland mesa mesa-vulkan-icd-freedreno` ;
- variables : `MESA_LOADER_DRIVER_OVERRIDE=kgsl TURNIP_KMD=kgsl GALLIUM_DRIVER=freedreno FD_FORCE_KGSL=1 XWAYLAND_FORCE_KGSL_SURFACELESS=1 EGL_PLATFORM=surfaceless ANLAND_NO_DRM_DEVICE=1` ;
- sans le Mesa Freedreno : `failed to create dri2 screen` / `EGL_NOT_INITIALIZED`.

**0.2, premier essai (2026-10-01)** : Arch Linux ARM (`menci/archlinuxarm`) dans proot-distro, socket Anland visible dans `/tmp/run/wayland-anland` (`XDG_RUNTIME_DIR=/tmp/run`). Hyprland démarre, puis échoue dans Weston 14 : `invalid version for global wl_compositor: expected at most 5, got 6`. Ce n'est pas une limite Android mais une incompatibilité de protocole : essayer sous KWin 6.7 (anland).
**0.2, second essai sous KWin 6.7 anland** : KWin rend correctement avec `FD_FORCE_KGSL=1` (sinon `EGL_BAD_PARAMETER` à l'import dmabuf). Hyprland se connecte à `wayland-kwin` (strace : `connect` OK), puis abandonne `CBackend::create()` **sans jamais ouvrir `/dev/dri`**. `wayland-info` montre que KWin annonce `zwp_linux_dmabuf_v1` v5 avec une **liste de formats vide** et sans périphérique principal (`ANLAND_NO_DRM_DEVICE`). Or aquamarine n'a que des allocateurs GBM/DRM : sans nœud DRM, il n'a ni allocateur ni renderer. **Le mode imbriqué ne suffit pas : il faut un allocateur/renderer aquamarine sans DRM (patch), ou un compositeur wlroots (Sway, rendu pixman/shm) à la place de Hyprland.**
**Décision (2026-10-01) : option A, porter le backend Anland dans aquamarine/Hyprland.**

### Phase 0.A : Hyprland natif sur Anland

Modèle : les patchs KWin de lfdevs (`lfdevs/kwin`, branche `debian/trixie`, `debian/patches/`) :
- `Add-support-for-Anland-display-daemon.patch` (~4 600 lignes) : dossier autonome `src/backends/anland/`. `display_producer.c`, `socket_utils.c` et `protocol.h` sont du C pur, réutilisables tels quels (GPL).
- `backends-anland-Add-support-for-running-without-a-DR.patch` (~160 lignes) : EGL surfaceless et dmabuf sans `DrmDevice`.

Fonctionnement : **c'est le démon Android qui alloue les buffers** (`get_buf_count`, `get_dmabuf_fd_at`, `get_dmabuf_info_at`). Le compositeur les importe en `EGLImage` (KGSL), dessine dedans, puis `set_render_fence` + `trigger_refresh`. L'entrée arrive par `poll_input_event`. Aucun allocateur GBM n'est nécessaire.

| # | Étape | Critère de réussite |
|---|---|---|
| A.1 | Mesa KGSL dans Arch (`lfdevs/mesa-for-android-container`). | `eglinfo` en `EGL_PLATFORM=surfaceless` affiche Freedreno/Adreno dans Arch. |
| ✅ A.1 | **Validée (2026-10-01).** Mesa `mesa-for-android-container` 26.3.0 installé dans Arch : Adreno 810, GLES 3.2. Sonde [probes/anland-egl/](probes/anland-egl/) (glibc, Arch) : connexion au démon, 4 buffers importés en EGLImage, 300 images validées par le client, écran bleu visible. **Piège :** sans l'environnement complet (`ANLAND=1 ANLAND_NO_DRM_DEVICE=1 XWAYLAND_FORCE_KGSL_SURFACELESS=1 FD_FORCE_KGSL=1 …`), `eglCreateImage(EGL_LINUX_DMA_BUF_EXT)` échoue en `EGL_BAD_PARAMETER`, et l'extension n'est jamais annoncée par `eglinfo` : le code ne doit pas exiger `EGL_EXT_image_dma_buf_import`. | |
| ✅ A.2 | **Validée (2026-10-01).** aquamarine v0.15.1 (même version que pacman, pour Hyprland 0.56.2) compile en ~2 min dans Arch sur la tablette (`ninja -j6`, dépendance `seatd` et non `libseat`). `LD_LIBRARY_PATH=…/aquamarine/build` : Hyprland charge bien notre `libaquamarine.so.14`. | |
| A.2b | **Hyprland doit aussi être patché** (`src/render/OpenGL.cpp`) : son EGL n'accepte que `EGL_PLATFORM_GBM_KHR` ou `EGL_PLATFORM_DEVICE_EXT` à partir du fd DRM du backend (ligne ~161, `RASSERT` ligne ~358), et il ne détecte l'import dmabuf que via la chaîne d'extensions (lignes 175-176), jamais annoncée ici. Patch : repli `EGL_PLATFORM_SURFACELESS_MESA` quand `drmRenderNodeFD() < 0`, et forcer `EXT_image_dma_buf_import` sous `ANLAND=1`. | Hyprland patché compile et crée son contexte EGL surfaceless sans DRM. |
| A.2c | Patch Hyprland écrit : [patches/hyprland/0001-render-surfaceless-EGL-for-Anland.patch](patches/hyprland/0001-render-surfaceless-EGL-for-Anland.patch) (+13 lignes, `OpenGL.cpp`). | Compile, puis Hyprland passe l'init EGL avec `ANLAND=1`. |
| A.3 | Backend aquamarine `anland`. **Conception :** (1) `AQ_BACKEND_ANLAND`, choisi dans `CBackend::create()` quand `ANLAND` est défini, à la place de wayland/drm (aucun changement côté Hyprland) ; (2) un allocateur `anland` dont `acquire()` renvoie les buffers du démon (dmabuf, `get_dmabuf_info_at`) ; (3) **piège :** le client Android choisit l'index du prochain buffer (`get_selected_idx`, lu en mémoire partagée), alors que `CSwapchain::next()` tourne en rond (`lastAcquired + 1`). Il faut un crochet dans le swapchain (fork) pour suivre `get_selected_idx` ; (4) `commit()` : fence via `set_render_fence`, puis `trigger_refresh` ; l'eventfd `get_buffer_ready_fd` sert de signal de frame (pollFD). : sortie = buffers du démon importés en EGLImage, présentation par fence + `trigger_refresh`. | Hyprland démarre avec `AQ_BACKEND=anland` (ou équivalent) et affiche un fond d'écran dans l'app Anland. |
| A.3 (code) | **Écrit et compilé (2026-10-01)** : [patches/aquamarine/0001-backend-add-Anland-backend.patch](patches/aquamarine/0001-backend-add-Anland-backend.patch). `CAnlandBackend` / `CAnlandOutput` / `CAnlandAllocator` / `CAnlandBuffer` (~300 lignes) + protocole Anland copié de KWin, sélection via `ANLAND` dans `CBackend::create()`, swapchain qui suit `get_selected_idx` (longueur forcée au nombre de buffers, `age = 0`). Pas de changement d'ABI. Non géré : entrée (A.5), reconnexion (A.6), curseur matériel. Reste à tester avec Hyprland patché. | |
| ✅ A.3 | **Validée (2026-10-01) : Hyprland 0.56.2 s'affiche dans l'app Anland**, rendu GPU Adreno via notre backend (bandeau d'erreurs de config, notification et curseur visibles). Lanceur : [probes/hypr-anland.sh](probes/hypr-anland.sh). Pièges : (1) l'installation des dépendances de Hyprland avait remplacé le Mesa KGSL par celui d'Arch (`failed to create dri2 screen`), d'où `IgnorePkg = mesa vulkan-freedreno …` dans `/etc/pacman.conf` ; (2) `get_selected_idx` avance pendant le rendu, c'est normal (KWin le lit aussi une seule fois par image). Reste : polices (texte en carrés), config Omarchy. | |
| ✅ A.5 (partiel) | **2026-10-01** : clavier, souris et tactile virtuels (`CAnlandKeyboard/Pointer/Touch`, annoncés depuis la boucle idle, sinon crash dans `CInputManager::newKeyboard`). Codes evdev reçus tels quels (A → 30), tactile normalisé par la taille du buffer. Terminal `foot` ouvert et texte lisible (police `ttf-dejavu` + `fc-cache`). ~90 images/s. **Super/Meta n'arrive jamais** : Android l'intercepte (voir 4.6, service d'accessibilité). | |
| A.6 (partiel) | Reconnexion écrite (callback fallback + timerfd + `pollFDsChanged`), mais **la rotation casse la session** : l'app se déconnecte, les buffers passent en 1600×2560 et `try_exit_fallback` échoue en boucle ; Hyprland dessine alors dans des fd fermés (`kgsl_bo_from_dmabuf: Bad file descriptor`). À faire : suspendre le rendu en fallback, changer le mode de sortie à la reconnexion. | |
| ✅ A.5 + A.6 | **Validées par l'utilisateur (2026-10-01)** : terminal `foot` utilisable au clavier USB, affichage stable plus d'une minute. Quatre causes de déconnexion trouvées et corrigées dans le backend : (1) **battement de cœur** : l'app Android tourne en pas à pas et réinitialise le lien si aucune image n'arrive en 5 s ; un Hyprland inactif n'en envoie pas, donc `needsFrame` est forcé après 1 s sans image ; (2) **`ANLAND_SOCKET` retiré de l'environnement** après lecture, sinon les programmes lancés par Hyprland (assistant hyprtoolkit, lui aussi basé sur aquamarine) se connectaient au démon comme producteur et éjectaient Hyprland ; (3) **`trigger_refresh` seulement quand le client est prêt** (sinon différé au signal « buffer prêt ») ; (4) **descripteurs retirés puis réajoutés** à la reconnexion, car Hyprland les identifie par numéro et les nouveaux réutilisent souvent les anciens. Aussi : le flux micro du canal audio est lu et ignoré. Diagnostic : journal du démon (`anland > ~/anland-daemon.log`). Toujours ouverts : touche Super (4.6), rotation. | |
| ✅ Omarchy (partiel) | **2026-10-01** : Omarchy 4 (config Hyprland en Lua, compatible 0.56) installé par [omarchy/install-omarchy-android.sh](omarchy/install-omarchy-android.sh) : thème Tokyo Night, barre Quickshell lancée automatiquement, terminal `foot` via `xdg-terminal-exec`, clavier validé par l'utilisateur. Correctifs : signature d'instance Hyprland courte ([patch 0002](patches/hyprland/0002-short-instance-signature-for-Anland.patch)), car sinon le chemin de socket dépasse 108 octets et `hyprctl` (utilisé partout par Omarchy) échoue ; `systemd-cat` remplacé (pas de journald) ; fins de ligne CRLF retirées ; changement de taille de l'écran à la reconnexion (`events.state`). Dispatch Lua : `hyprctl dispatch 'hl.dsp.exec_cmd("…")'`. **Limite actuelle : les clients GPU** (Alacritty, Quickshell en GL, navigateurs) n'ont pas d'EGL, faute de `linux-dmabuf` côté Hyprland, d'où Quickshell en `QT_QUICK_BACKEND=software` : c'est A.4. | |
| ✅ A.4 (clients GPU) | **Validée par l'utilisateur (2026-10-01)** : applis en GPU natif. Alacritty (OpenGL via Freedreno/KGSL) et `vkcube` (**Vulkan via Turnip**) fluides, barre Quickshell en GPU (plus de `QT_QUICK_BACKEND=software`). [Patch 0003](patches/hyprland/0003-linux-dmabuf-without-DRM-for-Anland.patch) : Hyprland garde `linux-dmabuf` sans nœud DRM (périphérique 0, sans les vérifications de render node), comme KWin Anland. Point clé : avec l'environnement `ANLAND*`, le Mesa KGSL annonce `EGL_EXT_image_dma_buf_import(_modifiers)` et 75 formats ([probes/anland-egl/formats.c](probes/anland-egl/formats.c)) ; sans lui, rien. Les applis doivent donc hériter de cet environnement (c'est le cas quand Hyprland les lance). | |
| ✅ Firefox | **2026-10-01** : Firefox s'ouvre et navigue. Par défaut `WebRender (Software)` : son sondage GPU (`/usr/lib/firefox/gfxtest glx`) sort en erreur sans GPU PCI ni `/dev/dri`. Avec [omarchy/firefox-user.js](omarchy/firefox-user.js) copié en `user.js` dans le profil : **`WebRender` GPU, freedreno Adreno 810, GLES 3.2**. Lenteur au démarrage et au chargement attribuée à proot (appels système), à mesurer avant/après la phase 2. | |
| A.4 | Hyprland sans DRM : renderer EGL surfaceless + feedback dmabuf sans périphérique principal (équivalent du second patch KWin). | `kitty` (client GPU) s'ouvre et s'affiche. |
| A.5 | Entrée : tactile, clavier, souris via `poll_input_event`. | Super+Entrée ouvre kitty, toucher et glisser déplacent une fenêtre. |
| A.6 | Reconnexion (`set_fallback_callback`, `try_exit_fallback`). | Quitter puis revenir dans l'app Anland 10 fois sans plantage de Hyprland. |

Notes pour la phase 2 : pacman 7 exige `DisableSandbox` (pas de Landlock dans le noyau, pas de vrai `setuid` vers `alpm`), et Hyprland refuse root (lancer sous un utilisateur normal).

**Décision :** imbriqué OK → continuer. Imbriqué KO → backend aquamarine « anland » (plusieurs mois), ou abandon.

## Phase 1 : rootfs Omarchy (en parallèle de la phase 2)

| # | Étape | Critère de réussite |
|---|---|---|
| 1.1 | Extraire la liste de paquets et les dotfiles d'Omarchy, et retirer boot, LUKS, Limine, snapper, iwd, bluez et brightnessctl. | Liste de paquets + `dotfiles/` versionnés. |
| ✅ 1.2 | **Validée par l'utilisateur (2026-10-01)** : [build/build-hyprland.sh](build/build-hyprland.sh) compile aquamarine + Hyprland nativement sur la tablette (~1 h, contre des heures sous qemu) ; [build/build-rootfs.sh](build/build-rootfs.sh) assemble sur PC (WSL + qemu-user, chroot) Arch Linux ARM, Mesa KGSL, fakeroot TCP, Omarchy et le runtime sans proot (`/opt/lotfix`) en `lotfix-rootfs.tar.zst` (1 Go, 3,5 Go décompressé). Sur la tablette : décompression + `setup-rootfs.sh` (passwd, machine-id, resolv.conf). Corrigés en route : Firefox (memfd/shm entre processus, bac à sable incompatible avec fakechroot), DNS sans proot (glibc lit `/etc/resolv.conf` en interne : serveurs posés dans `_res` par thread). | `rootfs.tar.zst` reproductible en une commande. |
| ✅ Manques (2026-10-02) | **Validés par l'utilisateur** : gcc/g++ sans proot (`posix_spawn` réimplémenté, `argv[0]` côté rootfs) ; **XWayland** (`/tmp/.X11-unix` en 1777, `setgid`/`setuid` sans effet tolérés, liens physiques remplacés par une copie exclusive) ; pacman (`fchmodat2` neutralisé dans glibc) ; **rotation** portrait/paysage ([patch Hyprland 0005](patches/hyprland/0005-follow-Anland-output-resize-on-rotation.patch) : rechargement juste après le changement de taille). Tactile OK. Reste : clavier à l'écran (phase 4, APK). | |
| 1.3 | Retirer de Waybar les modules matériels. | Waybar démarre sans erreur dans les logs. |

## Phase 2 : le shim sans proot

| # | Étape | Critère de réussite |
|---|---|---|
| 2.0 | **Mesures de référence (2026-10-01)**, [probes/bench.sh](probes/bench.sh), sous proot : `ls -lR /usr/share/icons` 12,4 s ; 200 lancements 2,27 s ; `pacman -Q` 0,32 s ; `gcc` 0,48 s ; `python` 0,20 s. | — |
| 2.1 (partiel) | **Premier lancement sans proot (2026-10-01)** : l'Arch glibc 2.43 est tuée par seccomp Android (`SIGSYS` sur `set_robust_list`). Solution : la **glibc 2.44 patchée de Termux** (`pkg install glibc-repo glibc`, `$PREFIX/glibc`) comme chargeur, puis les bibliothèques d'Arch : `$PREFIX/glibc/lib/ld-linux-aarch64.so.1 --library-path $PREFIX/glibc/lib:$ROOTFS/usr/lib <binaire>`, **sans `LD_PRELOAD`** (libtermux-exec est en Bionic). `bash` d'Arch tourne. Chemins réels, sans réécriture : `ls -lR` **0,19 s (64× plus rapide)**, 200 lancements **1,34 s**. Reste : réécriture d'`execve` et des shebangs. | `bash` de la rootfs exécute un script `#!/usr/bin/env python3`. |
| 2.1 + 2.2 (en cours) | **fakechroot 2.20.1 d'Arch** (pas de shim maison) + chargeur glibc Termux : [native/arch-native.sh](native/arch-native.sh). Validé sans proot : `/usr/bin` = 1549 binaires d'Arch, `python`, `/usr/bin/env python3`, script `#!/bin/sh`. Pièges : (1) **`FAKECHROOT_ELFLOADER_OPT_ARGV0` interdit**, sinon fakechroot oublie le chemin du script lors d'un exec `#!` (sh démarre sans script, sort en 0) ; contrepartie : `sys.executable` montre le chemin Android réel ; (2) **liens symboliques absolus** : le noyau les suit hors de la rootfs, d'où [native/relativize-symlinks.sh](native/relativize-symlinks.sh) (avec `realpath -s`, sinon le `/etc -> /system/etc` d'Android fausse le calcul). Une première passe boguée a laissé un `../` en trop sous `/etc` et `/bin` : [native/repair-etc-bin-links.sh](native/repair-etc-bin-links.sh), exécuté (44 liens réparés). **✅ Validé (2026-10-01) : `head -2 /etc/os-release` → `NAME="Arch Linux ARM"`, sans proot.** | `cat /etc/os-release` affiche Arch Linux ARM. |
| ✅ 2.3 | **Validée (2026-10-01) sans proot** : `ARCH_ROOT=1 native/arch-native.sh -c 'pacman -S htop'` installe htop (`/usr/bin/htop` en `root root` fictif). fakeroot **recompilé en `--with-ipc=tcp`** dans `/usr/local` (Android n'a pas de SysV IPC : `Function not implemented`), démon `faked` lancé par le lanceur, base de propriétaires persistante `/var/lib/fakeroot.state`, pile `LD_PRELOAD=libfakeroot:libfakechroot` avec chemins Android réels. pacman : `CheckSpace` désactivé (`setmntent()` ouvre `/etc/mtab` en interne, sans passer par fakechroot). Liens relatifs vers `/proc`, `/dev`, `/sys` (ex. `/etc/mtab`) rendus absolus. **Reste** : un hook tué par `SIGSYS`, probablement `ldconfig` (statique, glibc d'Arch), à traiter en 2.4. | `pacman -Syu` et `pacman -S htop` réussissent, scriptlets compris. |
| ✅ 2.4 (réduite) | **2026-10-01** : recensement de la rootfs : **seuls 2 binaires statiques**, `ldconfig` et `sln` (glibc). Remplacés par des scripts (`ldconfig` est inutile puisque le chargeur utilise `--library-path` ; `sln` → `ln -sf`), protégés par `NoExtract`. Le hook tué par `SIGSYS` disparaît. Le lanceur démarre dans le `/root` de la rootfs (un cwd hors rootfs fait échouer `getcwd`). Tout est regroupé dans [native/setup-rootfs.sh](native/setup-rootfs.sh) (idempotent). **Le loader SIGSYS n'est pas nécessaire tant qu'aucun binaire statique (Go, Rust musl) n'est installé** : à faire seulement le jour où un tel binaire est requis. | — |
| 2.4 (si besoin) | Loader SIGSYS pour les binaires statiques (handler freestanding, `rt_sigaction`/`rt_sigprocmask` de SIGSYS interceptés, `execve` → loader). | Un binaire Go statique lit le `/etc/passwd` virtuel et lance `/bin/sh`. |
| ✅ 2.6 | **Omarchy entier sans proot, validé par l'utilisateur (2026-10-01)** : [native/hypr-native.sh](native/hypr-native.sh) lance Hyprland via `arch-native.sh`, avec la barre Quickshell, le fond d'écran, le tactile et le clavier. Correctifs : (1) **glibc** : celle de Termux (2.44) casse le moteur JS de Qt (`corrupted size vs. prev_size` dans `QV4::ExecutionEngine`, même sans fakechroot). On utilise **la glibc d'Arch**, avec les appels tués par seccomp neutralisés en binaire (`svc` → `-ENOSYS`) : `set_robust_list`, `rseq`, `faccessat2`, via [native/patch-glibc-syscalls.py](native/patch-glibc-syscalls.py) (appels trouvés un par un avec `strace -e trace=none`) ; (2) **`/dev/shm` absent** sur Android et `shm_open()` non interceptable : [patch Hyprland 0004](patches/hyprland/0004-memfd-instead-of-shm_open-for-Anland.patch) utilise `memfd_create`, avec `dup` pour la copie lecture seule (SELinux refuse la réouverture d'un memfd via `/proc/self/fd`) ; (3) **RUNPATH absolus** (ex. `/usr/lib/libproxy`) recensés par `setup-rootfs.sh` dans `ld.so.conf.d`, lu par le lanceur ; (4) **liens `.l2s`** de proot (émulation de liens physiques, interdits par SELinux) réécrits relativement. **Reste** : XWayland (keymap), `posix_spawn` non intercepté par fakechroot (gcc → `cc1`), Firefox et les mesures de vitesse. | Bureau Omarchy complet sans proot. |
| ✅ 2.7 | **Firefox + GPU systématique sans proot, validé par l'utilisateur (2026-10-01)**. Firefox s'ouvre en ~6 s dans la session (WebRender GPU, Adreno 810). Mesures avec [probes/bench.sh](probes/bench.sh), proot → sans proot : `ls -lR` 10,4 s → 2,5 s, 200 lancements 3,8 s → 1,4 s, `pacman -Q` 0,61 s → 0,07 s, `python` 0,23 s → 0,10 s (`gcc` exclu : `posix_spawn` non intercepté). [native/android-compat.c](native/android-compat.c), préchargé d'office : `/proc/self/exe` = vrai programme (via argv[0], résolu, vu depuis la rootfs ; sinon Firefox « Couldn't load XPCOM »), liens `/proc/*/fd|cwd` sans préfixe de la rootfs, **`shm_open`/`shm_unlink` → `/tmp/.shm`** (sinon Firefox `MOZ_CRASH` sur `/dev/shm/org.mozilla.ipc.*`), variantes fortifiées `__readlink*_chk` incluses. **GPU pour tout programme** dans `arch-native.sh` (KGSL + `ANLAND*`) ; `ARCH_GPU=zink` pour OpenGL sur Vulkan/Turnip. [native/hyprctl-native.sh](native/hyprctl-native.sh). | — |
| 2.8 (terminaux) | **2026-10-01, sans proot** : **foot ✅, Alacritty ✅, kitty ⚠️** (fonctionne avec une commande explicite, ex. `kitty bash` ; son lancement par défaut passe par `kitten`, en Go). Correctifs dans [android-compat.c](native/android-compat.c) : `accept` → `accept4` (accept tué par seccomp, cassait le serveur TCP de fakeroot, donc pacman) ; `syscall()` filtré (`kcmp` de Mesa et autres → `ENOSYS`) ; **`tcgetattr`/`tcsetattr`/`isatty`/`ttyname` via `TCGETS`/`TCSETS`** (la glibc récente utilise `TCGETS2`, refusé par l'allowlist d'ioctl SELinux sur les pty : foot ne pouvait pas lancer son shell). Lanceur : `SHELL=/usr/bin/bash`, `TMPDIR=/tmp` (le `SHELL` de Termux fuyait, d'où Alacritty qui lançait le bash Bionic). **Limite découverte : les programmes Go font leurs appels système (`open`, `execve`…) sans la glibc, donc hors fakechroot** : `kitten` lance le `/bin/sh` d'Android. Touche aussi `gum`, `fzf`, `lazygit` utilisés par Omarchy → il faut l'interception au niveau des appels système (étape 2.4, SIGSYS). | foot, Alacritty, kitty ouvrent un shell utilisable. |
| ✅ 2.4 bis (Go) | **Programmes Go sans proot (2026-10-01)** : [native/go-syscalls.c](native/go-syscalls.c), lié à `android-compat.so`. Si le programme principal a une section `.go.buildinfo`, un filtre seccomp **TRAP** les appels de chemins **émis depuis le code du binaire Go** (IP dans son segment texte). Ceux de la glibc et de fakechroot passent sans coût, et les autres programmes ne sont pas touchés. Le gestionnaire `SIGSYS` traduit les chemins (règles de fakechroot), fait l'appel, gère `execve` via le chargeur (avec les `#!`), `getcwd`/`readlinkat` vus depuis la rootfs, `accept`→`accept4`, et `ENOSYS` pour les appels interdits. Cohabitation avec Go : `rt_sigaction(SIGSYS)` brut, `sigaction()` de la glibc (cgo) et `pthread_sigmask`/`sigprocmask` ne peuvent ni remplacer ni bloquer `SIGSYS`. Validé : `gum`, `fzf`, `kitten run-shell` (lance le bash d'Arch). **kitty ✅** avec `shell /usr/bin/bash`. Compte passwd de l'UID Android corrigé (shell `/usr/bin/bash`, home `/root`, au lieu de `/sbin/nologin` de proot-distro). Traces : `GO_SYSCALLS_DEBUG=1`, `GO_SYSCALLS_TRACE=1`. | — |
| 2.5 | Faux `systemctl` + runit. | Les services utilisateur d'Omarchy démarrent sans erreur. |

## Phase 3 : Hyprland via le shim (dans Termux)

| # | Étape | Critère de réussite |
|---|---|---|
| 3.1 | Mesa Turnip/Zink dans la rootfs. | `vkcube` à plus de 60 fps. |
| 3.2 | Socket Wayland à chemin court (`sun_path` ≤ 108 caractères). | Un client de la rootfs se connecte à Anland. |
| 3.3 | Session Omarchy complète. | Hyprland, Waybar, lanceur et thèmes OK, stables 1 heure. |
| 3.4 | Tactile (hyprgrass + clavier virtuel). | Changement d'espace de travail au geste, saisie au clavier écran. |

## Phase 4 : l'APK unique

**Décision (2026-10-02), option C allégée :** une seule APK `andro.marchy` (nom court : limite `sun_path` de 108 octets) qui reprend l'appli d'affichage et le démon d'**anland-termux** et le code Java utile de **termux-app** (vue terminal, service de premier plan, wake-lock), tous GPL-3.0. Elle **coexiste** avec un Termux officiel. **Aucun paquet Termux** : un lanceur natif (NDK) remplace `arch-native.sh`/`hypr-native.sh`, le téléchargement et l'extraction se font en Java, le démon Anland (747 lignes de C sans dépendance) est compilé avec le NDK, et `setup-rootfs.sh` s'exécute avec le `bash` d'Arch. Tout le reste passe par `pacman`.

**Étude 4.0 (faite) :** l'appli d'affichage (`MainActivity`, JNI : Surface, dma-buf, sockets, audio) se connecte au démon par socket Unix ; dans une seule appli, le pont `sharedUserId`/Binder disparaît. Anland-termux contient déjà le **clavier physique dédié** (`KeyInterceptor`, service d'accessibilité) et le **clavier à l'écran** (`SystemIME`, `VirtualKeyboardView`) : 4.5 et 4.6 sont surtout à valider avec Hyprland.

### Limites Android à respecter (Android 16 / API 36 sur l'appareil cible)

| Limite | Conséquence pour l'APK |
|---|---|
| **W^X** : une appli avec targetSdk ≥ 29 ne peut pas exécuter de fichiers de son dossier de données | **targetSdk 28**, donc hors Play Store (GitHub/F-Droid) ; risque si Android relève le targetSdk minimal installable (aujourd'hui 24) au-dessus de 28 |
| **Processus fantômes** (Android 12+) : enfants tués au-delà de 32, ou en arrière-plan | Aide à l'option développeur « Désactiver les restrictions des processus enfants » (déjà désactivée sur la tablette) |
| **Paramètres restreints** (Android 13+) pour les APK hors store | Guider vers « Autoriser les paramètres restreints » avant d'activer l'accessibilité |
| **Notifications** (Android 13+) | Demande de `POST_NOTIFICATIONS`, nécessaire au service de premier plan |
| **Veille, Doze, limites d'arrière-plan** | Service de premier plan + wake-lock ; HyperOS : « Démarrage automatique » et batterie « Aucune restriction » |
| **seccomp des applis** | glibc patchée (+ hook pacman), `android-compat.so` (accept4, setgid/setuid, kcmp…), interception Go |
| **SELinux** : pas de liens physiques, pas de `/proc/uptime` ni `/proc/stat`, pas de réouverture d'un memfd par `/proc/self/fd`, pas de `TCGETS2` | Contournements dans `android-compat.so` (copie exclusive, fichiers `/proc` émulés, etc.) |
| **Pas de `/dev/shm`, ni de DRM** | `shm_open` dans `/tmp/.shm`, memfd ; GPU via KGSL (Snapdragon uniquement) |
| **umask 077** à l'extraction | Extraction en conservant les droits ; `/tmp` en 1777 (`setup-rootfs.sh`) |
| **Taille de page** : 4 Ko sur la tablette, 16 Ko sur certains appareils Android 15+ | Bibliothèques NDK de l'APK alignées sur 16 Ko ; binaires Arch : à vérifier sur un appareil 16 Ko |
| **Raccourcis système HyperOS** (Super, Alt+Tab…) | Service d'accessibilité (`KeyInterceptor`) + désactiver les raccourcis HyperOS concernés |
| **Redémarrage de l'activité** (rotation, clavier branché) | Reconnexion du backend Anland, rechargement Hyprland à la rotation (patch 0005) |

| # | Étape | Critère de réussite |
|---|---|---|
| ✅ 4.1 | **Lanceur natif** (2026-10-02 : [native/launcher/lotfix-run.c](native/launcher/lotfix-run.c), environnement identique au script, NDK 28 API 24, segments alignés 16 Ko, session Hyprland lancée avec) (NDK) qui remplace `arch-native.sh`/`hypr-native.sh` (variables, chargeur patché, préchargements, `faked` pour le mode root). | Depuis Termux, le lanceur ouvre le `bash` d'Arch et démarre Hyprland comme les scripts. |
| 4.1b | **Audio** : brancher le son d'Anland (PipeWire côté Linux, audio natif de l'appli) sur la session Hyprland. | Une vidéo dans Firefox s'entend, le volume se règle depuis Omarchy. |
| 🟡 4.2 | **Fusion** : appli `andro.marchy` = affichage anland-termux + terminal et service termux-app + démon Anland (NDK) + lanceur ; l'appli s'ouvre sur Omarchy, le terminal reste accessible. | Une seule APK affiche Omarchy, à côté d'un Termux officiel installé. |
| | **Fait (2026-10-02, validé par l'utilisateur)** : APK `andro.marchy` installée, session complète sans Termux (démon Anland, PipeWire, Hyprland + Omarchy depuis `files/rootfs`) : affichage, fond d'écran, tactile, son, réseau. Pièges : permission `INTERNET` absente de l'appli Anland ; le `tar` d'Android (toybox) applique les droits des dossiers en lecture seule avant d'y écrire (extraire avec le `tar` GNU d'Arch) ; liens symboliques à chemin hôte après déplacement du système ([native/relink-host-symlinks.py](native/relink-host-symlinks.py)). Clavier virtuel : Maj/Ctrl/Alt des touches du clavier Android transmis, texte saisi tapé en disposition US (ASCII ; accents et emoji : protocole de saisie Wayland, plus tard). **Reste** : terminal Termux intégré. | |
| 4.3 | **Premier lancement** : téléchargement ou import du système Arch, sha256, extraction (droits conservés), `setup-rootfs.sh`, barre de progression. | Appareil vierge : bureau Omarchy sans taper aucune commande. |
| 4.4 | **Cycle de vie et HyperOS** : un service pour Hyprland et Anland, wake-lock, aide aux réglages (processus fantômes, démarrage automatique, batterie, notifications), veille/réveil. | Session vivante après 1 h en arrière-plan et 10 cycles veille/réveil. |
| ✅ 4.5 | **Clavier physique dédié** (2026-10-02, validé : `KeyInterceptor` d'anland-termux, réglage « interception des touches » ; pour le test, `WRITE_SECURE_SETTINGS` accordée par adb ; grand public : activation manuelle guidée dans l'onboarding, voir 4.4) : valider `KeyInterceptor` avec Hyprland, capture de la souris, aide aux paramètres restreints. | Super+Entrée ouvre un terminal, Alt+Tab reste dans Hyprland, clavier normal hors de l'appli. |
| 4.6 | **Clavier à l'écran** : valider `SystemIME`/`VirtualKeyboardView` avec Hyprland (accents, emoji). | Taper dans un terminal et dans Firefox sans clavier physique. |
| 4.7 | **Mises à jour** : ✅ hook pacman qui repatche glibc (2026-10-02, testé) ; mise à jour de l'appli et du système. | `pacman -Syu` avec une nouvelle glibc ne casse rien. |
| 4.8 | **Test grand public.** | Installation → bureau Omarchy en moins de 10 minutes. |
| 4.9 | **Pont d'état Android** : l'appli lit batterie, réseau, volume et luminosité (API Android, `/sys` étant interdit) et les transmet au système Arch (faux UPower, etc.) ; réglage du volume et de la luminosité via Android. Pas de gestion du Wi-Fi ni du Bluetooth depuis Linux. | La barre d'Omarchy affiche le niveau de batterie et l'état du réseau. |

## Problèmes connus (à corriger plus tard)

| Problème | Ce qu'on sait | Pistes |
|---|---|---|
| **Firefox : rendu saccadé, fenêtre qui clignote** (vidéo, défilement, interface) | Propre à Firefox : mpv en GPU est stable. Identique sur l'ancien et le nouveau système Arch, avec ou sans pont audio et patch 0005 (tests A/B du 2026-10-02) : ce n'est pas une régression. Corrigés en chemin : GPU (`/proc/bus/pci/devices` vide, `gfx.blocklist.all=-1`), mémoire partagée en RAM (réouverture de memfd émulée). Forcer l'alpha à 1 en sortie (patch 0006) a aggravé : retiré. | Synchronisation vsync/frame callbacks de Firefox (`widget.wayland.*`), comparer avec Chromium, journaux `MOZ_LOG` de la couche Wayland. |
| **Firefox plante sur « Enregistrer le lien sous… »** | Pas de rapport de plantage. | Pas de bus D-Bus de session (sans systemd) : lancer `dbus-daemon --session` avec la session, puis `xdg-desktop-portal(-gtk/-hyprland)`. Relancer Firefox avec sa sortie d'erreurs pour confirmer. |
| **Audio : démarrage manuel** | Le pont PipeWire marche (son validé), mais PipeWire, WirePlumber et pipewire-pulse ont été lancés à la main. | Les démarrer avec la session (autostart ou services utilisateur), puis commiter le pont audio d'aquamarine (`anland_audio.c`). |

## Phase 5 : maintenance

| # | Étape | Critère de réussite |
|---|---|---|
| 5.1 | Suivi des mises à jour d'Omarchy (fork + rebase des dotfiles). | Une mise à jour upstream intégrée en moins d'une journée. |
| 5.2 | Suite de régression (pacman, Hyprland, 30 apps courantes). | Taux de réussite publié à chaque release. |

---

## Prochaine action

Phase 0.1–0.2 sur la tablette. Avant de commencer, ouvrir une discussion avec les mainteneurs d'Anland (SuperTurtleDev, lfdevs) sur le projet d'APK autonome.

## Références

- Anland : https://github.com/SuperTurtleDev/anland
- anland-termux : https://github.com/lfdevs/anland-termux
- Précédent d'architecture (APK unique, rootfs glibc sans proot) : Winlator
