package andro.marchy;

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
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.wifi.WifiInfo;
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
 * Starts, once per process: the Andromarchy display daemon, the PipeWire audio stack and Hyprland, the last two inside
 * the Arch rootfs through lotfix-run (the native launcher: patched glibc loader + fakechroot, no proot).
 * Layout, all under getFilesDir() (short: Unix socket paths are limited to 108 bytes):
 *   rootfs/            the Arch Linux ARM system
 *   run/               XDG_RUNTIME_DIR (Wayland, PipeWire, Hyprland sockets), excluded from fakechroot
 *   run/andromarchy/        the display daemon's socket
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

    /** The session runs in this process (false: anything left in run/ is from a previous one). */
    static volatile boolean started;

    public static File runDir(Context ctx) {
        return new File(ctx.getFilesDir(), "run");
    }

    public static File daemonSocket(Context ctx) {
        return new File(runDir(ctx), "andromarchy/display_daemon.sock");
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
        started = true;
        HostGestures.apply(this); // HyperOS 3-finger gestures off again after a restart, if allowed
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
        startLocation(this);
        // Active network for the NetworkManager stand-in (android-nm): apps cannot see the interfaces.
        getSystemService(ConnectivityManager.class).registerDefaultNetworkCallback(network);
        // Requests from Linux for Android-only actions (Wi-Fi panel, launching Android apps).
        requests = new AndroidRequests(this, runDir(this));
        requests.startWatching();
        // Android apps in Omarchy's menu (.desktop entries in the rootfs, launched through the requests above).
        apps = new AndroidApps(this);
        apps.register(this);

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

        // 0. First boot of this rootfs on this device (once; the locale part on each start): see first-boot.sh.
        try {
            ProcessBuilder pb = new ProcessBuilder(launcher, "/opt/lotfix/first-boot.sh");
            pb.environment().clear();
            pb.environment().putAll(archEnv(run, sock));
            Process fb = pb.redirectErrorStream(true)
                    .redirectOutput(new File(run, "first-boot.log")).start();
            fb.getOutputStream().close();
            fb.waitFor();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }

        // 1. Display daemon: relays buffers and input between this app's display and Hyprland.
        spawn("andromarchy", List.of(libDir + "/libandromarchy.so", sock.getPath()), null);

        // 2. Session D-Bus (systemd provides it on a normal Linux login). Browsers, file dialogs, portals and tray
        //    apps need it; archEnv() gives every program its address.
        new File(run, "bus").delete();
        spawn("dbus", List.of(launcher, "-c", "exec dbus-daemon --session --nofork --nopidfile --address=unix:path="
                + new File(run, "bus").getPath()), archEnv(run, sock));

        // 2b. A "system" bus of our own (there is none for apps), for the services standing in for hardware ones.
        new File(run, "system_bus").delete();
        spawn("system-dbus", List.of(launcher, "-c", "exec dbus-daemon --config-file=/opt/lotfix/system-bus.conf"
                + " --nofork --nopidfile --address=unix:path=" + new File(run, "system_bus").getPath()), archEnv(run, sock));
        spawn("android-nm", List.of(launcher, "-c", "for i in $(seq 50); do [ -S \"$XDG_RUNTIME_DIR/system_bus\" ]"
                + " && break; sleep 0.1; done; exec android-nm"), archEnv(run, sock));
        spawn("android-upower", List.of(launcher, "-c", "for i in $(seq 50); do [ -S \"$XDG_RUNTIME_DIR/system_bus\" ]"
                + " && break; sleep 0.1; done; exec android-upower"), archEnv(run, sock));
        // The other system services a desktop expects (login1, hostname1, timedate1, locale1, PolicyKit, power
        // profiles, Accounts, RealtimeKit, GeoClue), as stand-ins: native/android-system-services.py.
        spawn("android-system-services", List.of(launcher, "-c", "for i in $(seq 50); do [ -S \"$XDG_RUNTIME_DIR/"
                + "system_bus\" ] && break; sleep 0.1; done; exec android-system-services"), archEnv(run, sock));
        // Session side: ScreenSaver inhibit and FileManager1 (native/android-session-services.py), and the secrets
        // keyring (passwords for browsers, VS Code, git). No login screen unlocks a keyring here: a password-less
        // "login" keyring is created once (gnome-keyring's plain format) and made the default.
        spawn("android-session-services", List.of(launcher, "-c", "for i in $(seq 50); do [ -S \"$XDG_RUNTIME_DIR/"
                + "bus\" ] && break; sleep 0.1; done; exec android-session-services"), archEnv(run, sock));
        spawn("gnome-keyring", List.of(launcher, "-c", "for i in $(seq 50); do [ -S \"$XDG_RUNTIME_DIR/bus\" ] && break;"
                + " sleep 0.1; done; K=$HOME/.local/share/keyrings; mkdir -p $K && chmod 700 $K;"
                + " [ -f $K/login.keyring ] || printf '[keyring]\\ndisplay-name=Login\\nctime=0\\nmtime=0\\n"
                + "lock-on-idle=false\\nlock-after=false\\n' > $K/login.keyring; [ -f $K/default ] || printf login > $K/default;"
                + " exec gnome-keyring-daemon --foreground --components=secrets < /dev/null"), archEnv(run, sock));

        // 2c. Hardware video (termux-va, third_party/): decodes and encodes with MediaCodec for the VA-API bridge.
        spawn("termux-va", List.of(libDir + "/libtermuxva.so", "--sock", new File(run, "termux-va.sock").getPath()),
                null);

        // 3. Sound: Hyprland's Andromarchy backend creates PipeWire nodes, so the stack must exist (it reconnects anyway).
        //    wireplumber and pipewire-pulse give up when PipeWire is not up yet (a cold first start): they wait for its
        //    socket.
        spawn("pipewire", List.of(launcher, "-c", "exec pipewire"), archEnv(run, sock));
        for (String p : new String[] {"wireplumber", "pipewire-pulse"})
            spawn(p, List.of(launcher, "-c", "for i in $(seq 100); do [ -S \"$XDG_RUNTIME_DIR/pipewire-0\" ] && break;"
                    + " sleep 0.1; done; exec " + p), archEnv(run, sock));

        // 4. Hyprland with Omarchy. It waits for this app's display to connect (MainActivity).
        Map<String, String> env = archEnv(run, sock);
        env.put("EGL_PLATFORM", "surfaceless");
        env.put("OMARCHY_PATH", "/usr/share/omarchy");
        spawn("hyprland", List.of(launcher, "-c", "export PATH=/usr/share/omarchy/bin:$PATH; exec /usr/bin/Hyprland"), env);

        // 5. desktop-ready (ends the splash, SplashScreen): Omarchy's shell runs, plus a second to draw the bar.
        spawn("desktop-ready", List.of(launcher, "-c", "for i in $(seq 600); do pgrep -x quickshell >/dev/null"
                + " || pgrep -x qs >/dev/null && break; sleep 0.2; done; sleep 1; touch \"$XDG_RUNTIME_DIR/desktop-ready\""),
                archEnv(run, sock));
    }

    /** Android's language as a glibc locale name (fr_FR.UTF-8); C.UTF-8 when it has no country. */
    static String androidLocale(Context ctx) {
        java.util.Locale l = ctx.getResources().getConfiguration().getLocales().get(0);
        return l.getCountry().isEmpty() ? "C.UTF-8" : l.getLanguage() + "_" + l.getCountry() + ".UTF-8";
    }

    /** Environment for programs of the rootfs; lotfix-run sets the rest (loader, fakechroot, GPU...). */
    private Map<String, String> archEnv(File run, File sock) {
        Map<String, String> env = new java.util.HashMap<>(System.getenv());
        env.put("ARCH_ROOTFS", rootfs(this).getPath());
        env.put("ARCH_EXCLUDE_EXTRA", run.getPath()); // sockets: real paths, not rewritten into the rootfs
        env.put("XDG_RUNTIME_DIR", run.getPath());
        env.put("TZ", java.util.TimeZone.getDefault().getID()); // glibc only knows /etc/localtime otherwise: UTC
        env.put("LANG", androidLocale(this)); // Android's language (first-boot.sh compiles its locale)
        env.put("ANDROMARCHY_UID", String.valueOf(android.os.Process.myUid()));
        env.put("ANDROMARCHY_GID", String.valueOf(android.os.Process.myUid()));
        env.put("ANDROMARCHY_SOCKET", sock.getPath());
        // The display's physical size (landscape, mm) for the compositor's output: clients size themselves from it
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        ((android.view.WindowManager) getSystemService(WINDOW_SERVICE)).getDefaultDisplay().getRealMetrics(dm);
        int wmm = Math.round(Math.max(dm.widthPixels / dm.xdpi, dm.heightPixels / dm.ydpi) * 25.4f);
        int hmm = Math.round(Math.min(dm.widthPixels / dm.xdpi, dm.heightPixels / dm.ydpi) * 25.4f);
        env.put("ANDROMARCHY_PHYSICAL_MM", wmm + "x" + hmm);
        // Flatpak apps' launchers (exported .desktop files, icons): a normal login gets these from
        // /etc/profile.d/flatpak.sh or systemd's environment generator; this session starts from the app.
        env.put("XDG_DATA_DIRS", "/root/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share"
                + ":/usr/local/share:/usr/share");
        env.put("ANDROMARCHY_VENDOR", android.os.Build.MANUFACTURER); // the machine's identity (DMI) for Linux tools
        env.put("ANDROMARCHY_MODEL", android.os.Build.MODEL);
        env.put("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + new File(run, "bus").getPath());
        env.put("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=" + new File(run, "system_bus").getPath());
        // Hardware video: VA-API through Mesa's termuxva bridge to the termux-va daemon (MediaCodec), when the
        // rootfs's Mesa has the bridge.
        if (new File(rootfs(this), "usr/lib/dri/termuxva_drv_video.so").exists()) {
            env.put("LIBVA_DRIVER_NAME", "termuxva");
            env.put("TERMUX_VA_BRIDGE", "1");
            env.put("TERMUX_VA_GPU_BACKEND", "kgsl");
            env.put("TERMUX_VA_SOCKET", new File(run, "termux-va.sock").getPath());
        }
        env.remove("WAYLAND_DISPLAY");
        env.remove("DISPLAY");
        return env;
    }

    /** Android's default network -> run/network ("wifi <strength> <ssid>" | "wired" | "none"), read by android-nm. */
    private AndroidRequests requests;
    private AndroidApps apps;

    // FLAG_INCLUDE_LOCATION_INFO: the Wi-Fi name (SSID) instead of "<unknown ssid>", once location is granted.
    private final ConnectivityManager.NetworkCallback network = new ConnectivityManager.NetworkCallback(
            ConnectivityManager.NetworkCallback.FLAG_INCLUDE_LOCATION_INFO) {
        @Override
        public void onLinkPropertiesChanged(Network n, android.net.LinkProperties lp) {
            StringBuilder dns = new StringBuilder(); // for omarchy-dns "DHCP": the network's own DNS servers
            for (java.net.InetAddress a : lp.getDnsServers())
                dns.append(dns.length() > 0 ? " " : "").append(a.getHostAddress());
            writeState("dns", dns.toString());
        }

        @Override
        public void onCapabilitiesChanged(Network n, NetworkCapabilities caps) {
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                int strength = 0;
                String ssid = "";
                if (caps.getTransportInfo() instanceof WifiInfo) {
                    WifiInfo w = (WifiInfo) caps.getTransportInfo();
                    strength = Math.max(0, Math.min(100, 2 * (w.getRssi() + 100))); // -100..-50 dBm -> 0..100
                    ssid = w.getSSID().replace("\"", ""); // redacted without the location permission
                    if (ssid.startsWith("<"))
                        ssid = "";
                }
                writeState("network", "wifi " + strength + " " + ssid);
            } else
                writeState("network", "wired"); // mobile data, USB, Ethernet: Quickshell only knows wired
        }

        @Override
        public void onLost(Network n) {
            writeState("network", "none");
        }
    };

    /** Atomic write of one line to run/<name>: the reader never sees half a line. */
    private void writeState(String name, String line) {
        File f = new File(runDir(this), name), tmp = new File(runDir(this), "." + name);
        try (java.io.FileWriter w = new java.io.FileWriter(tmp)) {
            w.write(line + "\n");
        } catch (IOException e) {
            return;
        }
        tmp.renameTo(f);
    }

    /** Android battery state -> run/battery ("<percent> <charging 0|1>"), read by android-upower. */
    /** Android's network location -> run/location ("lat lon accuracy unixtime"), served as GeoClue2 by
     *  android-system-services. Network provider: coarse, cheap on battery; every 10 minutes or 500 m. Called again
     *  by MainActivity once the permission is granted. */
    static void startLocation(Context ctx) {
        if (ctx.checkSelfPermission(android.Manifest.permission.ACCESS_COARSE_LOCATION)
                != android.content.pm.PackageManager.PERMISSION_GRANTED)
            return;
        android.location.LocationManager lm = ctx.getSystemService(android.location.LocationManager.class);
        String provider = lm.isProviderEnabled(android.location.LocationManager.NETWORK_PROVIDER)
                ? android.location.LocationManager.NETWORK_PROVIDER : android.location.LocationManager.PASSIVE_PROVIDER;
        android.location.LocationListener l = loc -> {
            File f = new File(runDir(ctx), "location"), tmp = new File(runDir(ctx), ".location");
            try (java.io.FileWriter w = new java.io.FileWriter(tmp)) {
                w.write(loc.getLatitude() + " " + loc.getLongitude() + " " + loc.getAccuracy() + " "
                        + loc.getTime() / 1000 + "\n");
            } catch (IOException e) {
                return;
            }
            tmp.renameTo(f);
        };
        android.location.Location last = lm.getLastKnownLocation(provider);
        if (last != null)
            l.onLocationChanged(last);
        lm.requestLocationUpdates(provider, 10 * 60 * 1000L, 500f, l, android.os.Looper.getMainLooper());
    }

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
        getSystemService(ConnectivityManager.class).unregisterNetworkCallback(network);
        requests.stopWatching();
        unregisterReceiver(apps);
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
