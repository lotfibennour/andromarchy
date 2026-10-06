package andro.marchy;

import android.content.Context;
import android.content.Intent;
import android.os.FileObserver;
import android.provider.Settings;
import android.util.Log;

import java.io.File;
import java.nio.file.Files;

/**
 * Requests from the Linux session for things only Android can do, written to run/android-request by the
 * android-request helper (atomically, by rename): "wifi-panel", "open <package>" (full screen), "split <package>" (beside Andromarchy), "float <package>" (floating window).
 */
final class AndroidRequests extends FileObserver {
    private static final String TAG = "AndroidRequests";
    private final Context ctx;
    private final File file;

    AndroidRequests(Context ctx, File runDir) {
        super(runDir, MOVED_TO | CLOSE_WRITE);
        this.ctx = ctx;
        this.file = new File(runDir, "android-request");
    }

    /**
     * A floating window centred on the screen: launch bounds (public API, honoured where freeform windows exist) plus
     * the freeform windowing mode (hidden API, by reflection: ignored where it is refused).
     */
    private android.app.ActivityOptions floating() {
        android.util.DisplayMetrics m = ctx.getResources().getDisplayMetrics();
        int w = m.widthPixels * 2 / 5, h = m.heightPixels * 3 / 5;
        int x = (m.widthPixels - w) / 2, y = (m.heightPixels - h) / 2;
        android.app.ActivityOptions o = android.app.ActivityOptions.makeBasic()
                .setLaunchBounds(new android.graphics.Rect(x, y, x + w, y + h));
        try {
            // Looked up through reflection itself ("meta-reflection"): the hidden-API check then sees a system caller.
            java.lang.reflect.Method lookup = Class.class.getDeclaredMethod("getDeclaredMethod", String.class, Class[].class);
            java.lang.reflect.Method set = (java.lang.reflect.Method) lookup.invoke(android.app.ActivityOptions.class,
                    "setLaunchWindowingMode", new Class<?>[] {int.class});
            set.invoke(o, 5); // WINDOWING_MODE_FREEFORM
        } catch (ReflectiveOperationException e) {
            Log.i(TAG, "freeform windowing mode unavailable: " + e);
        }
        return o;
    }

    @Override
    public void onEvent(int event, String path) {
        if (!"android-request".equals(path))
            return;
        String line;
        try {
            line = new String(Files.readAllBytes(file.toPath())).trim();
        } catch (Exception e) {
            return;
        }
        String[] req = line.split(" ", 2);
        Intent intent = null;
        if (req[0].equals("wifi-panel"))
            intent = new Intent(Settings.Panel.ACTION_WIFI); // apps may not switch Wi-Fi themselves
        else if ((req[0].equals("open") || req[0].equals("split")) && req.length == 2) {
            intent = ctx.getPackageManager().getLaunchIntentForPackage(req[1].trim());
            // split: beside Andromarchy (Android's split screen; a tablet enters it from full screen)
            if (intent != null && req[0].equals("split"))
                intent.addFlags(Intent.FLAG_ACTIVITY_LAUNCH_ADJACENT | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
        }
        if (req[0].equals("power") && req.length == 2) { // Omarchy's shutdown / reboot / log out / suspend
            Restarter.power(ctx, req[1].trim());
            return;
        }
        android.os.Bundle options = null;
        if (req[0].equals("float") && req.length == 2) {
            intent = ctx.getPackageManager().getLaunchIntentForPackage(req[1].trim());
            options = floating().toBundle();
            if (intent != null)
                intent.addFlags(Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
        }
        if (intent == null) {
            Log.w(TAG, "unknown request: " + line);
            return;
        }
        try {
            ctx.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), options);
        } catch (Exception e) {
            Log.w(TAG, "request failed: " + line, e);
        }
    }
}
