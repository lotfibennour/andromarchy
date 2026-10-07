-- Andromarchy: Hyprland rules for the Android screen (installed by install-omarchy-android.sh).

-- Omarchy centres its floating windows (portal file choosers...) before resizing them to 875x600: a dialog that
-- opened taller than the screen ended up above it (y = -21), its title bar and buttons under the bar. Place them
-- from their final size, in the space below the bar.
o.window({ tag = "floating-window" }, { move = { "(monitor_w*0.5-window_w*0.5)", "(monitor_h*0.5-window_h*0.5+13)" } })
o.window({ modal = true }, { float = true, center = true, max_size = { 1200, 720 } })

-- X11 apps sized for the screen: Omarchy leaves XWayland unscaled (crisp, for GTK's GDK_SCALE), but games and Qt/.NET/
-- Java/SDL apps on X11 do not scale themselves: on the tablet's 2x screen they were half size, tiny text. Scaled by
-- Hyprland instead (slightly soft). GTK apps run on Wayland here, unaffected.
-- ponytail: a GTK app forced onto X11 is drawn 4x (GDK_SCALE=2, then scaled); none found
hl.config({ xwayland = { force_zero_scaling = false } })
