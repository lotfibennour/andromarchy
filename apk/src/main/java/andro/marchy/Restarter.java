package andro.marchy;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * Omarchy's reboot / log out: a fresh session. Runs in its own process (":restart"): it waits for the app's main
 * process to be gone (killed by power()), then starts MainActivity again, which starts a new session.
 */
public class Restarter extends Activity {
    private static final String EXTRA_PID = "pid";

    /** Omarchy's power actions (android-request power ...), from the session's systemctl/uwsm stand-ins. */
    static void power(Context ctx, String action) {
        MainActivity main = MainActivity.sInstance;
        if (action.equals("suspend")) { // no sleep for an app: the desktop goes to the background, still running
            if (main != null)
                main.runOnUiThread(() -> main.moveTaskToBack(true));
            return;
        }
        // Omarchy closes the windows and shows its OSD first (it schedules the real action 2 s later).
        try {
            Thread.sleep(2000);
        } catch (InterruptedException ignored) {
        }
        ctx.stopService(new Intent(ctx, SessionService.class)); // ends Hyprland and every session process
        if (action.equals("reboot") || action.equals("logout"))
            ctx.startActivity(new Intent(ctx, Restarter.class).putExtra(EXTRA_PID, Process.myPid())
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        else if (main != null) // shutdown
            main.runOnUiThread(main::finishAndRemoveTask);
        try {
            Thread.sleep(500);
        } catch (InterruptedException ignored) {
        }
        Process.killProcess(Process.myPid());
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        new Thread(() -> {
            for (int i = 0; i < 100 && pid > 0 && new java.io.File("/proc/" + pid).exists(); i++)
                try {
                    Thread.sleep(100);
                } catch (InterruptedException ignored) {
                }
            startActivity(new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                    | Intent.FLAG_ACTIVITY_CLEAR_TASK));
            finish();
            Process.killProcess(Process.myPid());
        }).start();
    }
}
