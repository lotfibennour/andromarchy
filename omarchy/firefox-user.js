// Android/Anland: Firefox's gfxtest finds no PCI GPU nor /dev/dri and falls back to software rendering.
// The KGSL Mesa works for Wayland clients, so force GPU compositing and WebGL.
// Copy into the Firefox profile directory as user.js.
user_pref("gfx.webrender.all", true);
user_pref("gfx.webrender.software", false);
user_pref("layers.acceleration.force-enabled", true);
user_pref("webgl.force-enabled", true);
user_pref("webgl.disabled", false);
