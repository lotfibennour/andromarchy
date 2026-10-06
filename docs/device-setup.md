# Device setup (HyperOS / MIUI tablets)

Settings to change on the Android side so that Andromarchy works fully. They are host settings: the app cannot
change them itself.

## Touch gestures (3 fingers)

Omarchy's touch gestures (hyprgrass, `~/.config/hypr/touch.lua`) use 3 fingers. HyperOS takes 3-finger swipes for
itself (screenshot, split screen, app switching) and steals the touches from the app.

In Settings:

- Additional settings → Gesture shortcuts → Take a screenshot → turn off "Slide 3 fingers down".
- Additional settings → Navigation system: turn on "Don't respond to three- and four-finger gestures".

The 3-finger split-screen and multi-finger app-switching gestures have no switch in Settings. Turn them off with adb:

```sh
adb shell settings put system enable_three_gesture 0
adb shell settings put system three_gesture_horizontal_ltr none
adb shell settings put system three_gesture_horizontal_rtl none
adb shell settings put global multi_finger_slide 0
```

Undo:

```sh
adb shell settings put system enable_three_gesture 1
adb shell settings put system three_gesture_horizontal_ltr split_ltr
adb shell settings put system three_gesture_horizontal_rtl split_rtl
adb shell settings put global multi_finger_slide 1
```

The screen edges (home, back, notification shade) stay with Android: no Omarchy gesture starts from an edge.
