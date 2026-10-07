# Andromarchy user guide

Andromarchy runs Omarchy (Arch Linux with the Hyprland desktop) as one Android app. No root, no proot, no virtual
machine: Linux programs run directly on the tablet, with its GPU, sound, cameras and touch screen.

## What you need

- An Android tablet with Android 11 or later and a Qualcomm Adreno GPU. Andromarchy is developed on a POCO Pad 2 Pro
  (Adreno 810, HyperOS / Android 16).
- About 15 GB of free space: the system takes several GB once unpacked, and apps you add take more.
- A Wi-Fi connection for the first launch.

## Installing

1. Install the Andromarchy APK.
2. Open it. The first-launch screen asks for your name, your email (for Git) and your keyboard layouts, then downloads
   and unpacks the system. This takes a few minutes.
3. Android asks for a few permissions at once:
   - **Microphone** and **Camera**: for calls and video in Linux apps.
   - **Location**: Android only shows the Wi-Fi network's name to apps allowed to use location. Andromarchy uses it
     for the network name in the bar.
4. The desktop appears. Omarchy's menu opens with **Super + Space**.

### The Super key (recommended)

Android keeps the Super (Windows) key of hardware keyboards for itself. To use Omarchy's shortcuts with a hardware
keyboard, turn on Andromarchy's accessibility service: **Settings → Accessibility → Andromarchy → On**. It only
reads key presses while Andromarchy is in front.

## HyperOS / MIUI settings

Some gestures are taken by the system before Andromarchy sees them. Change these so the desktop's 3-finger gestures
work:

- **Settings → Additional settings → Gesture shortcuts → Take a screenshot**: turn off **Slide 3 fingers down**.
- **Settings → Additional settings → Navigation system**: turn on **Don't respond to three- and four-finger
  gestures**.

The split-screen and app-switching gestures with several fingers have no switch in Settings. With a computer and
`adb`:

```sh
adb shell settings put system enable_three_gesture 0
adb shell settings put system three_gesture_horizontal_ltr none
adb shell settings put system three_gesture_horizontal_rtl none
adb shell settings put global multi_finger_slide 0
```

To undo:

```sh
adb shell settings put system enable_three_gesture 1
adb shell settings put system three_gesture_horizontal_ltr split_ltr
adb shell settings put system three_gesture_horizontal_rtl split_rtl
adb shell settings put global multi_finger_slide 1
```

Swipes from the screen edges (home, back, notifications) stay with Android.

## Using the desktop

### Touch gestures

| Gesture | Action |
|---|---|
| 3 fingers left / right | Switch workspace |
| 3 fingers up | App launcher |
| 3 fingers down | Close the window |
| 3-finger tap | Full screen on / off |

You can change them in `~/.config/hypr/touch.lua`.

### On-screen keyboard

The keyboard (Squeekboard) appears by itself when you tap a text field, and hides while a hardware keyboard is
connected. The **⌨** icon in the bar shows or hides it at any time.

- It has a full PC layout: Esc, Tab, Ctrl, Super, Alt, AltGr, arrows, and F1–F12 on AltGr.
- **🌐** switches between the keyboard layouts you chose on the first-launch screen (Omarchy's). French,
  English and Arabic have the PC layout.
- Modifiers work like keyboard chords. Tap **Super**, then **Shift**, then **F** for Super + Shift + F; they turn off
  after the key. **Double-tap** a modifier to keep it on; tap it again to release it.
- It takes the colours of the current Omarchy theme.

### Android and Linux together

- **Copy and paste** works both ways between Android apps and Linux apps.
- **Drag and drop**: put Andromarchy and another app side by side (split screen or a floating window) and drag a photo,
  file or text onto the desktop. Files land in `~/Downloads`; text goes to the clipboard.
- **Android apps** are in the **Apps → Mobile Apps** menu and open in a floating window.

## Installing apps

- **App store**: GNOME Software, with Flathub. Flatpak apps run on the tablet's GPU.
- **Terminal**: `sudo pacman -S <package>` for Arch Linux packages, `yay -S <package>` for the AUR.
- Andromarchy's own components (graphics drivers, compositor, keyboard) are protected: an install or a full update
  (`sudo pacman -Syu`, `omarchy-update`) cannot replace them. If an update needs attention, a notification says so.

## Checking that everything works

Open a terminal and run:

```sh
andromarchy-selftest
```

It checks the GPU, sound, cameras, keyboard, clipboard, system services and app store, and prints one line per
check. Add `-v` to see why a check fails.

## Known limits

- **Flatpak apps are not isolated** from each other: Android gives apps no way to build the sandboxes Flatpak uses.
  They run with your user's access, like ordinary programs.
- **YouTube in Brave** decodes video on the processor. Hardware decoding works in `mpv` and `ffmpeg`.
- There is no real administrator account: `sudo` works for installing and configuring, but cannot change the tablet's
  own system.

## Credits

Andromarchy's Android app is based on [lfdevs/anland-termux](https://github.com/lfdevs/anland-termux) (GPL-3.0). It
uses lfdevs's Mesa for Android, termux-va and XWayland patches, and builds on Omarchy, Hyprland, Arch Linux ARM, Mesa,
Squeekboard, hyprgrass, PipeWire, Flatpak and fakechroot.
