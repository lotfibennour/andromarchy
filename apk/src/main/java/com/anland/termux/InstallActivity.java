package com.anland.termux;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.StatFs;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import io.airlift.compress.zstd.ZstdInputStream;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * First launch: what Omarchy's setup form asks that still applies here (keyboard layouts, name and email; the
 * language and time zone come from Android), then the rootfs download and extraction. SessionService then runs
 * first-boot.sh, which applies the answers (setup.conf).
 */
public class InstallActivity extends Activity {
    /** The rootfs built by build/build-rootfs.sh, published as a GitHub release asset. */
    static final String ROOTFS_URL = "https://github.com/OWNER/andromarchy/releases/latest/download/lotfix-rootfs.tar.zst";
    /** A local copy in the app's external files dir (Android/data/andro.marchy/files/, no permission needed) is used
     *  instead when present: offline installs and tests. */
    static final String LOCAL_NAME = "lotfix-rootfs.tar.zst";
    private static final long NEEDED_BYTES = 12L << 30; // archive + extracted rootfs, with room for apps

    // {xkb layout, label}: the common ones; the first selected is the physical keyboard's.
    private static final String[][] LAYOUTS = {
            {"us", "English (US)"}, {"gb", "English (UK)"}, {"fr", "Français (AZERTY)"}, {"ara", "العربية"},
            {"de", "Deutsch"}, {"es", "Español"}, {"it", "Italiano"}, {"pt", "Português"}, {"br", "Português (BR)"},
            {"be", "Belgique"}, {"ch", "Suisse"}, {"ca", "Canada (FR)"}, {"nl", "Nederlands"}, {"tr", "Türkçe"},
            {"ru", "Русский"}, {"ua", "Українська"}, {"pl", "Polski"}, {"gr", "Ελληνικά"}, {"il", "עברית"},
            {"ir", "فارسی"}, {"jp", "日本語"}, {"kr", "한국어"}, {"cn", "中文"}, {"in", "हिन्दी"},
    };

    private final List<CheckBox> layoutBoxes = new ArrayList<>();
    private EditText name, email;
    private TextView status;
    private ProgressBar progress;
    private Button install;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(pad, pad, pad, pad);
        col.setBackgroundColor(Color.rgb(0x1a, 0x1b, 0x26));

        col.addView(text("Andromarchy", 28));
        col.addView(text("Omarchy (Arch Linux + Hyprland) on Android. First launch: a few choices, then the system "
                + "download (about 2 GB).", 15));

        col.addView(text("\nKeyboard layouts (the first one is your physical keyboard's; the on-screen keyboard "
                + "cycles through all)", 17));
        String lang = Locale.getDefault().getLanguage();
        String suggested = lang.equals("ar") ? "ara" : lang.equals("en") ? "us" : lang.equals("uk") ? "ua"
                : lang.equals("he") ? "il" : lang.equals("fa") ? "ir" : lang.equals("ja") ? "jp"
                : lang.equals("ko") ? "kr" : lang.equals("zh") ? "cn" : lang.equals("hi") ? "in"
                : lang.equals("el") ? "gr" : lang.equals("pt") && Locale.getDefault().getCountry().equals("BR") ? "br"
                : lang;
        for (String[] l : LAYOUTS) {
            CheckBox cb = new CheckBox(this);
            cb.setText(l[1] + "  (" + l[0] + ")");
            cb.setTextColor(Color.WHITE);
            cb.setTag(l[0]);
            cb.setChecked(l[0].equals(suggested) || l[0].equals("us"));
            layoutBoxes.add(cb);
            col.addView(cb);
        }

        col.addView(text("\nName and email (optional: git, and the Compose shortcuts)", 17));
        name = field("Full name");
        email = field("Email");
        col.addView(name);
        col.addView(email);

        install = new Button(this);
        install.setText("Install");
        install.setOnClickListener(v -> start());
        col.addView(install);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        col.addView(progress);
        status = text("", 14);
        col.addView(status);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(0x1a, 0x1b, 0x26));
        scroll.addView(col);
        setContentView(scroll);
    }

    private TextView text(String s, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(Color.WHITE);
        t.setGravity(Gravity.START);
        return t;
    }

    private EditText field(String hint) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setTextColor(Color.WHITE);
        e.setHintTextColor(Color.GRAY);
        e.setSingleLine();
        return e;
    }

    private void start() {
        // The physical keyboard's layout first: "us" only leads when it is the sole choice besides none.
        List<String> layouts = new ArrayList<>();
        for (CheckBox cb : layoutBoxes)
            if (cb.isChecked() && !cb.getTag().equals("us"))
                layouts.add((String) cb.getTag());
        if (layoutBoxes.get(0).isChecked())
            layouts.add(layouts.isEmpty() ? 0 : layouts.size(), "us");
        if (layouts.isEmpty())
            layouts.add("us");

        File files = getFilesDir();
        if (new StatFs(files.getPath()).getAvailableBytes() < NEEDED_BYTES) {
            status.setText("Not enough free space: about " + (NEEDED_BYTES >> 30) + " GB are needed.");
            return;
        }
        install.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        String conf = "KB_LAYOUTS=" + String.join(",", layouts) + "\nFULL_NAME=" + quote(name.getText().toString())
                + "\nEMAIL=" + quote(email.getText().toString()) + "\n";
        new Thread(() -> {
            try {
                installRootfs(conf);
                runOnUiThread(() -> {
                    startActivity(new Intent(this, MainActivity.class));
                    finish();
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    status.setText("Install failed: " + e.getMessage() + "\nCheck the connection and retry.");
                    install.setEnabled(true);
                });
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
        try (InputStream in = new ZstdInputStream(counting)) {
            TarExtractor.extract(in, partial, entry -> {
                long now = System.currentTimeMillis();
                if (now - shown[0] < 200)
                    return;
                shown[0] = now;
                String what = (local.canRead() ? "Installing" : "Downloading and installing") + " ("
                        + (read[0] >> 20) + (size > 0 ? " / " + (size >> 20) : "") + " MB)\n" + entry;
                runOnUiThread(() -> {
                    if (size > 0)
                        progress.setProgress((int) (read[0] * 1000 / size));
                    status.setText(what);
                });
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
