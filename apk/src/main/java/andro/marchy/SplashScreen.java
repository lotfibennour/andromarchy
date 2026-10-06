package andro.marchy;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Dialog;
import android.graphics.Color;
import android.os.Build;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;

import java.io.File;

/**
 * Splash over the desktop while the session starts (design: splash.html at the repo root, assets/splash.html). Its
 * progress is the session's own: each step is a socket or file the session creates; the last one, desktop-ready, is
 * written by SessionService once Omarchy's shell (the bar) runs. Touch skips it.
 */
final class SplashScreen {
    private static final String[] MSGS = {
        "Starting the display bridge...", "Starting D-Bus...", "Starting PipeWire...", "Starting Hyprland...",
        "Opening the Wayland socket...", "Starting Omarchy's shell...", "Ready.",
    };

    /** Shown at least this long, even when the session is up sooner: time to see it (touch still skips it). */
    private static final long MIN_SHOWN_MS = 7000;

    static void showIfStarting(Activity a) {
        File run = SessionService.runDir(a);
        if (!SessionService.started)
            new File(run, "desktop-ready").delete(); // from a previous session
        else if (new File(run, "desktop-ready").exists())
            return; // the session is already up (activity recreated)
        WebView w = new WebView(a);
        w.setBackgroundColor(Color.rgb(0x0c, 0x0d, 0x14));
        w.setFocusable(false); // keyboard input stays with the desktop's surface
        w.setFocusableInTouchMode(false);
        w.getSettings().setJavaScriptEnabled(true);
        w.getSettings().setAllowFileAccess(true); // the rootfs's font
        w.addJavascriptInterface(new Object() {
            @JavascriptInterface
            public String info() {
                ActivityManager.MemoryInfo m = new ActivityManager.MemoryInfo();
                a.getSystemService(ActivityManager.class).getMemoryInfo(m);
                String soc = Build.VERSION.SDK_INT >= 31 && !Build.SOC_MODEL.equals(Build.UNKNOWN)
                        ? Build.SOC_MODEL : Build.HARDWARE;
                return "{\"version\":\"" + BuildConfig.VERSION_NAME + "\",\"kernel\":\"" + System.getProperty("os.version")
                        + "\",\"mem\":\"" + String.format(java.util.Locale.ROOT, "%.1f / %.1f GB",
                        (m.totalMem - m.availMem) / 1e9, m.totalMem / 1e9) + "\",\"soc\":\"" + soc + "\"}";
            }

            @JavascriptInterface
            public String state() {
                File[] wl = run.listFiles((d, n) -> n.startsWith("wayland-") && !n.endsWith(".lock"));
                boolean[] done = {
                    SessionService.daemonSocket(a).exists(), new File(run, "bus").exists(),
                    new File(run, "pipewire-0").exists(), new File(run, "hypr").isDirectory(),
                    wl != null && wl.length > 0, new File(run, "desktop-ready").exists(),
                };
                int step = 0;
                while (step < done.length && done[step])
                    step++;
                return "{\"step\":" + step + ",\"steps\":" + done.length + ",\"msg\":\"" + MSGS[step] + "\"}";
            }

            @JavascriptInterface
            public void skip() {
                a.runOnUiThread(() -> dismiss(w));
            }
        }, "A");
        w.loadUrl("file:///android_asset/splash.html");
        // Closed from here, not by the page's polling: a page that stalls must not cover the desktop.
        long shown = android.os.SystemClock.uptimeMillis();
        android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        h.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (new File(run, "desktop-ready").exists()
                        && android.os.SystemClock.uptimeMillis() - shown >= MIN_SHOWN_MS)
                    dismiss(w);
                else if (w.getTag() != null)
                    h.postDelayed(this, 300);
            }
        }, 300);
        // Its own window: a view of the activity is drawn under the desktop's SurfaceView, never over it.
        Dialog d = new Dialog(a, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        d.setCancelable(false);
        d.setContentView(w);
        w.setTag(d);
        d.show();
        d.getWindow().getInsetsController().hide(android.view.WindowInsets.Type.systemBars()); // full screen like the desktop
    }

    private static void dismiss(WebView w) {
        Dialog d = (Dialog) w.getTag();
        if (d == null)
            return;
        w.setTag(null);
        w.animate().alpha(0).setDuration(400).withEndAction(() -> {
            d.dismiss();
            w.destroy();
        });
    }
}
