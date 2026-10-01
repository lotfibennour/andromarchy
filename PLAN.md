# Lotfix : Omarchy sur Android, sans root ni proot

Une seule APK qui ouvre directement un bureau **Omarchy** (Arch Linux ARM + Hyprland) sur Android, sans root et sans proot.

Appareil cible : POCO Pad 2 Pro (Android 16, Snapdragon/Adreno).

## Choix fixés

- **Pas de proot.** Le shim LD_PRELOAD (fakechroot + fakeroot portés) gère les binaires dynamiques, et un loader `SECCOMP_RET_TRAP` + handler SIGSYS gère les binaires statiques.
- **Affichage :** Anland (GPL-3.0), avec le client `libawl` intégré dans l'APK.
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
| 1.2 | Construire la rootfs aarch64 sur PC (Docker + qemu-user). | `rootfs.tar.zst` reproductible en une commande. |
| 1.3 | Retirer de Waybar les modules matériels. | Waybar démarre sans erreur dans les logs. |

## Phase 2 : le shim sans proot

| # | Étape | Critère de réussite |
|---|---|---|
| 2.0 | **Mesures de référence (2026-10-01)**, [probes/bench.sh](probes/bench.sh), sous proot : `ls -lR /usr/share/icons` 12,4 s ; 200 lancements 2,27 s ; `pacman -Q` 0,32 s ; `gcc` 0,48 s ; `python` 0,20 s. | — |
| 2.1 (partiel) | **Premier lancement sans proot (2026-10-01)** : l'Arch glibc 2.43 est tuée par seccomp Android (`SIGSYS` sur `set_robust_list`). Solution : la **glibc 2.44 patchée de Termux** (`pkg install glibc-repo glibc`, `$PREFIX/glibc`) comme chargeur, puis les bibliothèques d'Arch : `$PREFIX/glibc/lib/ld-linux-aarch64.so.1 --library-path $PREFIX/glibc/lib:$ROOTFS/usr/lib <binaire>`, **sans `LD_PRELOAD`** (libtermux-exec est en Bionic). `bash` d'Arch tourne. Chemins réels, sans réécriture : `ls -lR` **0,19 s (64× plus rapide)**, 200 lancements **1,34 s**. Reste : réécriture d'`execve` et des shebangs. | `bash` de la rootfs exécute un script `#!/usr/bin/env python3`. |
| 2.1 + 2.2 (en cours) | **fakechroot 2.20.1 d'Arch** (pas de shim maison) + chargeur glibc Termux : [native/arch-native.sh](native/arch-native.sh). Validé sans proot : `/usr/bin` = 1549 binaires d'Arch, `python`, `/usr/bin/env python3`, script `#!/bin/sh`. Pièges : (1) **`FAKECHROOT_ELFLOADER_OPT_ARGV0` interdit**, sinon fakechroot oublie le chemin du script lors d'un exec `#!` (sh démarre sans script, sort en 0) ; contrepartie : `sys.executable` montre le chemin Android réel ; (2) **liens symboliques absolus** : le noyau les suit hors de la rootfs, d'où [native/relativize-symlinks.sh](native/relativize-symlinks.sh) (avec `realpath -s`, sinon le `/etc -> /system/etc` d'Android fausse le calcul). Une première passe boguée a laissé un `../` en trop sous `/etc` et `/bin` : [native/repair-etc-bin-links.sh](native/repair-etc-bin-links.sh), exécuté (44 liens réparés). **✅ Validé (2026-10-01) : `head -2 /etc/os-release` → `NAME="Arch Linux ARM"`, sans proot.** | `cat /etc/os-release` affiche Arch Linux ARM. |
| 2.3 | Faux root (fakeroot). | `pacman -Syu` et `pacman -S htop` réussissent, scriptlets compris. |
| 2.4 | Loader SIGSYS pour les binaires statiques (handler freestanding, `rt_sigaction`/`rt_sigprocmask` de SIGSYS interceptés, `execve` → loader). | Un binaire Go statique lit le `/etc/passwd` virtuel et lance `/bin/sh`. |
| 2.5 | Faux `systemctl` + runit. | Les services utilisateur d'Omarchy démarrent sans erreur. |

## Phase 3 : Hyprland via le shim (dans Termux)

| # | Étape | Critère de réussite |
|---|---|---|
| 3.1 | Mesa Turnip/Zink dans la rootfs. | `vkcube` à plus de 60 fps. |
| 3.2 | Socket Wayland à chemin court (`sun_path` ≤ 108 caractères). | Un client de la rootfs se connecte à Anland. |
| 3.3 | Session Omarchy complète. | Hyprland, Waybar, lanceur et thèmes OK, stables 1 heure. |
| 3.4 | Tactile (hyprgrass + clavier virtuel). | Changement d'espace de travail au geste, saisie au clavier écran. |

## Phase 4 : l'APK unique

| # | Étape | Critère de réussite |
|---|---|---|
| 4.1 | APK targetSdk 28 + extraction (ou téléchargement) de la rootfs. | Au premier lancement, rootfs prête sans terminal. |
| 4.2 | Client d'affichage Anland intégré (`libawl`), sans Termux. | Hyprland s'affiche dans l'app, Termux désinstallé. |
| 4.3 | Foreground service + écran d'aide « processus enfants ». | Session vivante après 1 heure en arrière-plan. |
| 4.4 | Cycle de vie : veille, rotation, retour. | La session survit à 10 cycles veille/réveil. |
| 4.5 | Test grand public (avec l'activation du service clavier). | Appareil vierge : installation → bureau Omarchy en moins de 10 minutes. |
| 4.6 | **Clavier dédié** : `AccessibilityService` (`flagRequestFilterKeyEvents`) actif seulement au premier plan, `requestPointerCapture()` pour la souris, onboarding (dont « Autoriser les paramètres restreints »). | Super+Entrée ouvre un terminal dans Hyprland, Alt+Tab n'ouvre **pas** les récents d'Android, et hors de l'app le clavier se comporte normalement. |

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
