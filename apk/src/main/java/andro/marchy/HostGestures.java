package andro.marchy;

import android.content.ContentResolver;
import android.content.Context;
import android.content.pm.PackageManager;
import android.provider.Settings;
import android.util.Log;

/**
 * HyperOS/MIUI take multi-finger swipes for themselves (app switching) and steal them from the desktop's gestures;
 * they also restore that setting after a restart. When the user granted WRITE_SECURE_SETTINGS once
 * (adb shell pm grant andro.marchy android.permission.WRITE_SECURE_SETTINGS, see docs/user-guide.md), turn them off
 * at every start. Without the permission: nothing, the user's Android settings are theirs.
 */
final class HostGestures {
    private HostGestures() {
    }

    static void apply(Context ctx) {
        if (ctx.checkSelfPermission("android.permission.WRITE_SECURE_SETTINGS") != PackageManager.PERMISSION_GRANTED)
            return;
        ContentResolver cr = ctx.getContentResolver();
        // ponytail: only the global key; HyperOS refuses its system-table gesture keys to apps even with this
        // permission ("cannot keep your settings in the secure settings"): split screen is turned off in Settings.
        try {
            Settings.Global.putInt(cr, "multi_finger_slide", 0);
        } catch (RuntimeException e) {
            Log.w("Andromarchy", "HyperOS gesture settings (global): " + e);
        }
    }
}
