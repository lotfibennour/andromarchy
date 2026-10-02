package com.anland.termux;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.util.HashSet;
import java.util.Set;

/**
 * The Android apps in Omarchy's menu: one .desktop entry (+ PNG icon) per launchable app in the rootfs's
 * ~/.local/share/applications, run through android-request ("open <package>", see AndroidRequests). The app opens
 * over Andromarchy (full screen, split screen or a HyperOS floating window). Kept in sync on install/removal.
 */
final class AndroidApps extends BroadcastReceiver {
    private static final String TAG = "AndroidApps";
    private static final int ICON = 192;
    private final File appsDir, iconsDir;

    AndroidApps(Context ctx) {
        File home = new File(SessionService.rootfs(ctx), "root/.local/share");
        appsDir = new File(home, "applications");
        iconsDir = new File(home, "icons/android");
    }

    void register(Context ctx) {
        IntentFilter f = new IntentFilter();
        f.addAction(Intent.ACTION_PACKAGE_ADDED);
        f.addAction(Intent.ACTION_PACKAGE_REMOVED);
        f.addAction(Intent.ACTION_PACKAGE_CHANGED);
        f.addDataScheme("package");
        ctx.registerReceiver(this, f);
        new Thread(() -> export(ctx)).start();
    }

    @Override
    public void onReceive(Context ctx, Intent intent) {
        new Thread(() -> export(ctx)).start();
    }

    private synchronized void export(Context ctx) {
        appsDir.mkdirs();
        iconsDir.mkdirs();
        PackageManager pm = ctx.getPackageManager();
        Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        Set<String> kept = new HashSet<>();
        for (ResolveInfo ri : pm.queryIntentActivities(launcher, 0)) {
            String pkg = ri.activityInfo.packageName;
            if (pkg.equals(ctx.getPackageName()) || !kept.add(pkg))
                continue; // ourselves, or a second launcher activity of the same app
            File icon = new File(iconsDir, pkg + ".png");
            try {
                if (!icon.exists())
                    savePng(ri.loadIcon(pm), icon);
                try (FileWriter w = new FileWriter(new File(appsDir, "android-" + pkg + ".desktop"))) {
                    w.write("[Desktop Entry]\nType=Application\n"
                            + "Name=" + ri.loadLabel(pm).toString().replace('\n', ' ') + "\n"
                            + "Comment=Android app\n"
                            + "Exec=android-request open " + pkg + "\n"
                            + "Icon=" + "/root/.local/share/icons/android/" + pkg + ".png\n"
                            + "Categories=Android;\nKeywords=android;" + pkg + ";\n");
                }
            } catch (IOException e) {
                Log.w(TAG, "export " + pkg, e);
            }
        }
        // Uninstalled apps: their entries and icons go too.
        File[] entries = appsDir.listFiles((d, n) -> n.startsWith("android-") && n.endsWith(".desktop"));
        for (File f : entries != null ? entries : new File[0]) {
            String pkg = f.getName().substring(8, f.getName().length() - 8);
            if (!kept.contains(pkg)) {
                f.delete();
                new File(iconsDir, pkg + ".png").delete();
            }
        }
    }

    private static void savePng(Drawable d, File out) throws IOException {
        Bitmap b = Bitmap.createBitmap(ICON, ICON, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        d.setBounds(0, 0, ICON, ICON);
        d.draw(c);
        try (FileOutputStream o = new FileOutputStream(out)) {
            b.compress(Bitmap.CompressFormat.PNG, 100, o);
        }
    }
}
