package andro.marchy;

import android.app.Activity;
import android.content.ClipData;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.view.DragEvent;
import android.view.View;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * Drag and drop from Android apps (split screen / floating window) onto the desktop: files land in ~/Downloads,
 * text goes to the desktop clipboard. ponytail: no drop into a given Linux window (that needs a Wayland drag).
 */
final class DropTarget implements View.OnDragListener {
    private final Activity activity;

    DropTarget(Activity activity) {
        this.activity = activity;
    }

    @Override
    public boolean onDrag(View v, DragEvent e) {
        if (e.getAction() != DragEvent.ACTION_DROP)
            return true; // accept every drag, so Android lets us have the drop
        activity.requestDragAndDropPermissions(e); // read access to the content:// URIs until the activity ends
        ClipData clip = e.getClipData();
        if (clip == null)
            return false;
        new Thread(() -> receive(clip)).start();
        return true;
    }

    private void receive(ClipData clip) {
        File dir = new File(SessionService.rootfs(activity), "root/Downloads");
        dir.mkdirs();
        int files = 0;
        String last = null;
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < clip.getItemCount(); i++) {
            ClipData.Item item = clip.getItemAt(i);
            Uri uri = item.getUri();
            if (uri == null) {
                CharSequence t = item.coerceToText(activity);
                if (t != null && t.length() > 0)
                    text.append(text.length() > 0 ? "\n" : "").append(t);
                continue;
            }
            File out = unique(dir, name(uri));
            try (InputStream in = activity.getContentResolver().openInputStream(uri);
                 OutputStream o = new FileOutputStream(out)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; )
                    o.write(buf, 0, n);
                files++;
                last = out.getName();
            } catch (Exception ex) {
                out.delete();
            }
        }
        if (text.length() > 0)
            Native.nativeSendClipboard(text.toString().getBytes(StandardCharsets.UTF_8));
        String msg = files == 1 ? last + " → ~/Downloads"
                : files > 1 ? files + " files → ~/Downloads"
                : text.length() > 0 ? "Text copied to the desktop clipboard" : "Nothing to receive";
        activity.runOnUiThread(() -> Toast.makeText(activity, msg, Toast.LENGTH_SHORT).show());
    }

    private String name(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null)
                return c.getString(0).replace('/', '_');
        } catch (Exception ignored) {
        }
        String s = uri.getLastPathSegment();
        return s != null ? s.replace('/', '_') : "dropped";
    }

    private static File unique(File dir, String name) {
        File f = new File(dir, name);
        int dot = name.lastIndexOf('.');
        for (int i = 2; f.exists(); i++)
            f = new File(dir, dot > 0 ? name.substring(0, dot) + " (" + i + ")" + name.substring(dot) : name + " (" + i + ")");
        return f;
    }
}
