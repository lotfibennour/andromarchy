-- Touchscreen gestures (hyprgrass). The plugin is loaded by autostart; config reloads once it is in.
-- Android/HyperOS keeps the screen edges (home, back, notification shade) :
-- 3-finger gestures need the HyperOS 3-finger screenshot gesture turned off.
local g = hl.plugin and hl.plugin.hyprgrass
if not g then return end

hl.config({ plugin = { hyprgrass = { sensitivity = 4.0, long_press_delay = 400 } } })

-- 3 fingers left/right: switch workspace (follows the fingers)
g.gesture({ pattern = { kind = "swipe", fingers = 3, direction = "horizontal" }, action = "workspace" })
-- 3 fingers up: app launcher
g.bind({ pattern = { kind = "swipe", fingers = 3, direction = "up" }, action = hl.dsp.exec_cmd("omarchy-launch-walker") })
-- 3 fingers down: close the window
g.bind({ pattern = { kind = "swipe", fingers = 3, direction = "down" }, action = hl.dsp.window.close() })
-- 3-finger tap: fullscreen toggle
g.bind({ pattern = { kind = "tap", fingers = 3 }, action = hl.dsp.window.fullscreen() })
