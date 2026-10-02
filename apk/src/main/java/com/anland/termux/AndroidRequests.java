package com.anland.termux;

import android.content.Context;
import android.content.Intent;
import android.os.FileObserver;
import android.provider.Settings;
import android.util.Log;

import java.io.File;
import java.nio.file.Files;

/**
 * Requests from the Linux session for things only Android can do, written to run/android-request by the
 * android-request helper (atomically, by rename): "wifi-panel", "open <package>".
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
        else if (req[0].equals("open") && req.length == 2)
            intent = ctx.getPackageManager().getLaunchIntentForPackage(req[1].trim());
        if (intent == null) {
            Log.w(TAG, "unknown request: " + line);
            return;
        }
        try {
            ctx.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } catch (Exception e) {
            Log.w(TAG, "request failed: " + line, e);
        }
    }
}
