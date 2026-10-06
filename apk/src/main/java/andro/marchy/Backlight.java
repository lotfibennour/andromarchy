package andro.marchy;

import android.app.Activity;
import android.os.FileObserver;
import android.provider.Settings;
import android.view.WindowManager;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.file.Files;

/**
 * An "android" backlight device for the session: run/backlight/android/{brightness,max_brightness,...}, which
 * android-compat shows as /sys/class/backlight (denied to apps). Omarchy's brightness keys, its monitor panel and
 * brightnessctl write the brightness file; this applies it to the app's window (full screen, so the whole display;
 * no permission needed, unlike the system brightness).
 */
final class Backlight extends FileObserver {
    private static final int MAX = 255;
    private final Activity activity;
    private final File dir;

    static Backlight start(Activity activity) {
        File dir = new File(SessionService.runDir(activity), "backlight/android");
        dir.mkdirs();
        int current;
        try { // starts where Android's own setting is (0..255 on most devices; clamped otherwise)
            current = Math.min(MAX, Settings.System.getInt(activity.getContentResolver(), Settings.System.SCREEN_BRIGHTNESS));
        } catch (Settings.SettingNotFoundException e) {
            current = MAX / 2;
        }
        write(new File(dir, "max_brightness"), String.valueOf(MAX));
        write(new File(dir, "type"), "raw");
        write(new File(dir, "bl_power"), "0");
        write(new File(dir, "actual_brightness"), String.valueOf(current));
        write(new File(dir, "brightness"), String.valueOf(current));
        Backlight b = new Backlight(activity, dir);
        b.startWatching();
        return b;
    }

    private Backlight(Activity activity, File dir) {
        super(dir, CLOSE_WRITE | MOVED_TO);
        this.activity = activity;
        this.dir = dir;
    }

    @Override
    public void onEvent(int event, String path) {
        if (!"brightness".equals(path))
            return;
        int value;
        try {
            value = Integer.parseInt(new String(Files.readAllBytes(new File(dir, "brightness").toPath())).trim());
        } catch (Exception e) {
            return;
        }
        final int v = Math.max(1, Math.min(MAX, value)); // 0 would leave the screen black with no way back
        write(new File(dir, "actual_brightness"), String.valueOf(v));
        activity.runOnUiThread(() -> {
            WindowManager.LayoutParams lp = activity.getWindow().getAttributes();
            lp.screenBrightness = v / (float) MAX;
            activity.getWindow().setAttributes(lp);
        });
    }

    private static void write(File f, String value) {
        try (FileWriter w = new FileWriter(f)) {
            w.write(value + "\n");
        } catch (IOException ignored) {
        }
    }
}
