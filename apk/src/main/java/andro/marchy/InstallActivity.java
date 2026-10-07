package andro.marchy;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.StatFs;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.zip.GZIPInputStream;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Locale;

/**
 * First launch: what Omarchy's setup form asks that still applies here (keyboard layouts, name and email; the
 * language and time zone come from Android), then the rootfs download and extraction. SessionService then runs
 * first-boot.sh, which applies the answers (setup.conf).
 */
public class InstallActivity extends Activity {
    /** The rootfs built by build/build-rootfs.sh, published as a GitHub release asset. */
    static final String ROOTFS_URL = "https://github.com/lotfibennour/andromarchy/releases/latest/download/lotfix-rootfs.tar.gz";
    /** A local copy in the app's external files dir (Android/data/andro.marchy/files/, no permission needed) is used
     *  instead when present: offline installs and tests. */
    static final String LOCAL_NAME = "lotfix-rootfs.tar.gz";
    private static final long NEEDED_BYTES = 12L << 30; // archive + extracted rootfs, with room for apps

    // {xkb layout, label}: the common ones; the first selected is the physical keyboard's.
    private static final String[][] LAYOUTS = {
            {"us", "English (US)"}, {"gb", "English (UK)"}, {"fr", "Français (AZERTY)"}, {"ara", "العربية"},
            {"de", "Deutsch"}, {"es", "Español"}, {"it", "Italiano"}, {"pt", "Português"}, {"br", "Português (BR)"},
            {"be", "Belgique"}, {"ch", "Suisse"}, {"ca", "Canada (FR)"}, {"nl", "Nederlands"}, {"tr", "Türkçe"},
            {"ru", "Русский"}, {"ua", "Українська"}, {"pl", "Polski"}, {"gr", "Ελληνικά"}, {"il", "עברית"},
            {"ir", "فارسی"}, {"jp", "日本語"}, {"kr", "한국어"}, {"cn", "中文"}, {"in", "हिन्दी"},
    };

    private WebView page;

    // The page (assets/install.html, design: "first launch.html" at the repo root) calls these, as "A".
    private final Object bridge = new Object() {
        /** [{id, label, on}]: Android's language's layout first and on, then US English (on). */
        @JavascriptInterface
        public String layouts() {
            String lang = Locale.getDefault().getLanguage();
            String suggested = lang.equals("ar") ? "ara" : lang.equals("en") ? "us" : lang.equals("uk") ? "ua"
                    : lang.equals("he") ? "il" : lang.equals("fa") ? "ir" : lang.equals("ja") ? "jp"
                    : lang.equals("ko") ? "kr" : lang.equals("zh") ? "cn" : lang.equals("hi") ? "in"
                    : lang.equals("el") ? "gr" : lang.equals("pt") && Locale.getDefault().getCountry().equals("BR") ? "br"
                    : lang;
            JSONArray out = new JSONArray();
            try {
                for (String[] l : LAYOUTS)
                    if (l[0].equals(suggested))
                        out.put(new JSONObject().put("id", l[0]).put("label", l[1]).put("on", true));
                for (String[] l : LAYOUTS)
                    if (!l[0].equals(suggested))
                        out.put(new JSONObject().put("id", l[0]).put("label", l[1]).put("on", l[0].equals("us")));
            } catch (JSONException e) {
                throw new RuntimeException(e);
            }
            return out.toString();
        }

        @JavascriptInterface
        public void install(String name, String email, String layouts) {
            start(name, email, layouts);
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        page = new WebView(this);
        page.setBackgroundColor(Color.rgb(0x12, 0x13, 0x1d));
        page.getSettings().setJavaScriptEnabled(true);
        page.addJavascriptInterface(bridge, "A");
        page.loadUrl("file:///android_asset/install.html");
        setContentView(page);
    }

    /** Calls a function of the page with JSON-encoded arguments. */
    private void call(String fn, Object... args) {
        JSONArray a = new JSONArray();
        for (Object o : args)
            a.put(o);
        String js = fn + ".apply(null, " + a + ")";
        runOnUiThread(() -> page.evaluateJavascript(js, null));
    }

    private void start(String name, String email, String layouts) {
        File files = getFilesDir();
        if (new StatFs(files.getPath()).getAvailableBytes() < NEEDED_BYTES) {
            call("failed", "not enough free space: about " + (NEEDED_BYTES >> 30) + " GB are needed");
            return;
        }
        String conf = "KB_LAYOUTS=" + layouts.replaceAll("[^a-z,]", "") + "\nFULL_NAME=" + quote(name)
                + "\nEMAIL=" + quote(email) + "\n";
        new Thread(() -> {
            try {
                installRootfs(conf);
                runOnUiThread(() -> {
                    startActivity(new Intent(this, MainActivity.class));
                    finish();
                });
            } catch (Throwable e) { // shown, never a crash: the user can retry
                call("failed", String.valueOf(e.getMessage()));
            }
        }).start();
    }

    private static String quote(String s) { // setup.conf is sourced by bash
        return "'" + s.trim().replace("'", "'\\''") + "'";
    }

    private void installRootfs(String conf) throws IOException {
        File rootfs = SessionService.rootfs(this);
        File partial = new File(rootfs.getPath() + ".partial");
        deleteTree(partial); // a previous attempt that stopped halfway
        File external = new File(getExternalFilesDir(null), LOCAL_NAME);
        // or in files/ (adb run-as, or a rootfs build on the device)
        final File local = external.canRead() ? external : new File(getFilesDir(), LOCAL_NAME);
        InputStream raw;
        long total;
        if (local.canRead()) {
            raw = new FileInputStream(local);
            total = local.length();
        } else {
            HttpURLConnection c = (HttpURLConnection) new URL(ROOTFS_URL).openConnection();
            c.setInstanceFollowRedirects(true); // GitHub release assets redirect to their storage host
            c.connect();
            if (c.getResponseCode() != 200)
                throw new IOException("download: HTTP " + c.getResponseCode());
            raw = c.getInputStream();
            total = c.getContentLengthLong();
        }
        final long size = total;
        long[] read = {0};
        long[] shown = {0};
        InputStream counting = new FilterInputStream(new BufferedInputStream(raw, 1 << 20)) {
            @Override
            public int read(byte[] b, int off, int len) throws IOException {
                int n = super.read(b, off, len);
                if (n > 0)
                    read[0] += n;
                return n;
            }
        };
        try (InputStream in = new GZIPInputStream(counting, 1 << 16)) {
            TarExtractor.extract(in, partial, entry -> {
                long now = System.currentTimeMillis();
                if (now - shown[0] < 200)
                    return;
                shown[0] = now;
                String what = (local.canRead() ? "Installing" : "Downloading and installing") + " ("
                        + (read[0] >> 20) + (size > 0 ? " / " + (size >> 20) : "") + " MB)\n" + entry;
                call("progress", size > 0 ? read[0] * 1000 / size : -1, what);
            });
        }
        File setup = new File(partial, "root/.config/andromarchy/setup.conf");
        setup.getParentFile().mkdirs();
        try (FileWriter w = new FileWriter(setup)) {
            w.write(conf);
        }
        if (!partial.renameTo(rootfs))
            throw new IOException("could not move the rootfs into place");
    }

    private static void deleteTree(File f) {
        File[] children = f.isDirectory() && !isSymlink(f) ? f.listFiles() : null;
        if (children != null)
            for (File c : children)
                deleteTree(c);
        f.delete();
    }

    private static boolean isSymlink(File f) {
        try {
            return java.nio.file.Files.isSymbolicLink(f.toPath());
        } catch (Exception e) {
            return false;
        }
    }
}
