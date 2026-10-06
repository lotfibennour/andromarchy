-- Andromarchy: Hyprland rules for the Android screen (installed by install-omarchy-android.sh).

-- Dialogs an app draws itself (GTK file choosers in Zotero, Firefox...) opened at the very top, their title bar and
-- buttons under Omarchy's bar. Keep every modal dialog centred and within the space below the bar.
o.window({ modal = true }, { float = true, center = true, max_size = { 1200, 720 } })
