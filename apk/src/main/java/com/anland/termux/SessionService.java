package com.anland.termux;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.BroadcastReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Andromarchy session: keeps the Linux side alive in a foreground service (HyperOS kills background apps).
 *
 * Starts, once per process: the Anland display daemon, the PipeWire audio stack and Hyprland, the last two inside
 * the Arch rootfs through lotfix-run (the native launcher: patched glibc loader + fakechroot, no proot).
 * Layout, all under getFilesDir() (short: Unix socket paths are limited to 108 bytes):
 *   rootfs/            the Arch Linux ARM system
 *   run/               XDG_RUNTIME_DIR (Wayland, PipeWire, Hyprland sockets), excluded from fakechroot
 *   run/anland/        the display daemon's socket
 *   run/*.log          output of each process
 */
public class SessionService extends Service {
    private static final String TAG = "Andromarchy";
    private static final String CHANNEL = "session";
    private static final int NOTIFICATION_ID = 1;

    private final List<Process> processes = new ArrayList<>();
    private PowerManager.WakeLock wakeLock;

    public static File rootfs(Context ctx) {
        return new File(ctx.getFilesDir(), "rootfs");
    }

    public static File runDir(Context ctx) {
        return new File(ctx.getFilesDir(), "run");
    }

    public static File daemonSocket(Context ctx) {
        return new File(runDir(ctx), "anland/display_daemon.sock");
    }

    /** True once the Arch rootfs (built by build/build-rootfs.sh) is in place. */
    public static boolean rootfsReady(Context ctx) {
        return new File(rootfs(ctx), "opt/lotfix/arch-glibc/ld-linux-aarch64.so.1").exists();
    }

    public static void start(Context ctx) {
        ctx.startForegroundService(new Intent(ctx, SessionService.class));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Session", NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .setContentTitle("Andromarchy")
                .setContentText("Omarchy session running")
                .setContentIntent(open)
                .setOngoing(true)
                .build();
        startForeground(NOTIFICATION_ID, n);

        PowerManager pm = getSystemService(PowerManager.class);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "andromarchy:session");
        wakeLock.acquire();

        // Battery for the session's UPower stand-in (android-upower): apps cannot read /sys/class/power_supply.
        registerReceiver(battery, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));

        try {
            startSession();
        } catch (IOException e) {
            Log.e(TAG, "session start failed", e);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY; // the processes belong to this service: started once, in onCreate
    }

    private void startSession() throws IOException {
        File run = runDir(this);
        File sock = daemonSocket(this);
        sock.getParentFile().mkdirs();
        sock.delete(); // a stale socket from a killed session would make the daemon's bind fail

        String libDir = getApplicationInfo().nativeLibraryDir;
        String launcher = libDir + "/liblotfixrun.so";

        // 1. Display daemon: relays buffers and input between this app's display and Hyprland.
        spawn("anland", List.of(libDir + "/libanland.so", sock.getPath()), null);

        // 2. Session D-Bus (systemd provides it on a normal Linux login). Browsers, file dialogs, portals and tray
        //    apps need it; archEnv() gives every program its address.
        new File(run, "bus").delete();
        spawn("dbus", List.of(launcher, "-c", "exec dbus-daemon --session --nofork --nopidfile --address=unix:path="
                + new File(run, "bus").getPath()), archEnv(run, sock));

        // 2b. A "system" bus of our own (there is none for apps), for the services standing in for hardware ones.
        new File(run, "system_bus").delete();
        spawn("system-dbus", List.of(launcher, "-c", "exec dbus-daemon --config-file=/opt/lotfix/system-bus.conf"
                + " --nofork --nopidfile --address=unix:path=" + new File(run, "system_bus").getPath()), archEnv(run, sock));
        spawn("android-upower", List.of(launcher, "-c", "for i in $(seq 50); do [ -S \"$XDG_RUNTIME_DIR/system_bus\" ]"
                + " && break; sleep 0.1; done; exec android-upower"), archEnv(run, sock));

        // 3. Sound: Hyprland's Anland backend creates PipeWire nodes, so the stack must exist (it reconnects anyway).
        for (String p : new String[] {"pipewire", "wireplumber", "pipewire-pulse"})
            spawn(p, List.of(launcher, "-c", "exec " + p), archEnv(run, sock));

        // 4. Hyprland with Omarchy. It waits for this app's display to connect (MainActivity).
        Map<String, String> env = archEnv(run, sock);
        env.put("EGL_PLATFORM", "surfaceless");
        env.put("OMARCHY_PATH", "/usr/share/omarchy");
        spawn("hyprland", List.of(launcher, "-c", "export PATH=/usr/share/omarchy/bin:$PATH; exec /usr/bin/Hyprland"), env);
    }

    /** Environment for programs of the rootfs; lotfix-run sets the rest (loader, fakechroot, GPU...). */
    private Map<String, String> archEnv(File run, File sock) {
        Map<String, String> env = new java.util.HashMap<>(System.getenv());
        env.put("ARCH_ROOTFS", rootfs(this).getPath());
        env.put("ARCH_EXCLUDE_EXTRA", run.getPath()); // sockets: real paths, not rewritten into the rootfs
        env.put("XDG_RUNTIME_DIR", run.getPath());
        env.put("TZ", java.util.TimeZone.getDefault().getID()); // glibc only knows /etc/localtime otherwise: UTC
        env.put("ANLAND_SOCKET", sock.getPath());
        env.put("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + new File(run, "bus").getPath());
        env.put("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=" + new File(run, "system_bus").getPath());
        env.remove("WAYLAND_DISPLAY");
        env.remove("DISPLAY");
        return env;
    }

    /** Android battery state -> run/battery ("<percent> <charging 0|1>"), read by android-upower. */
    private final BroadcastReceiver battery = new BroadcastReceiver() {
        @Override
        public void onReceive(Context ctx, Intent i) {
            int level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            int status = i.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            boolean charging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL;
            if (level < 0)
                return;
            File f = new File(runDir(ctx), "battery"), tmp = new File(runDir(ctx), ".battery");
            try (java.io.FileWriter w = new java.io.FileWriter(tmp)) {
                w.write((level * 100 / scale) + " " + (charging ? 1 : 0) + "\n");
            } catch (IOException e) {
                return;
            }
            tmp.renameTo(f); // atomic: the reader never sees half a line
        }
    };

    private void spawn(String name, List<String> cmd, Map<String, String> env) throws IOException {
        ProcessBuilder pb = new ProcessBuilder(cmd);
        if (env != null) {
            pb.environment().clear();
            pb.environment().putAll(env);
        }
        File log = new File(runDir(this), name + ".log");
        pb.redirectErrorStream(true).redirectOutput(log);
        processes.add(pb.start());
        Log.i(TAG, "started " + name + " (log: " + log + ")");
    }

    @Override
    public void onDestroy() {
        unregisterReceiver(battery);
        for (Process p : processes)
            p.destroy();
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
