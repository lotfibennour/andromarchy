package andro.marchy;

import android.system.ErrnoException;
import android.system.Os;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Extracts the rootfs tarball (GNU tar, as build-rootfs.sh writes it: --numeric-owner, hard links dereferenced).
 * Regular files, directories and symlinks, with their permission bits; GNU long names and pax path headers.
 * Directory modes are applied last: a read-only directory set first would refuse its own children (toybox's tar
 * failed that way). Ownership is not kept: every file belongs to the app's uid, the only user here.
 */
final class TarExtractor {
    interface Progress {
        void onEntry(String name);
    }

    static void extract(InputStream in, File dest, Progress progress) throws IOException {
        byte[] header = new byte[512];
        byte[] buf = new byte[1 << 16];
        String longName = null, longLink = null;
        List<Object[]> dirModes = new ArrayList<>(); // {File, mode}
        while (readFully(in, header)) {
            if (isZero(header))
                break; // end of archive
            String name = longName != null ? longName : str(header, 0, 100);
            String prefix = str(header, 345, 155);
            if (longName == null && !prefix.isEmpty())
                name = prefix + "/" + name;
            String link = longLink != null ? longLink : str(header, 157, 100);
            longName = longLink = null;
            int mode = (int) octal(header, 100, 8);
            long size = octal(header, 124, 12);
            char type = (char) header[156];

            if (type == 'L' || type == 'K') { // GNU long name / long link target: the data is the name
                String s = new String(readData(in, size), StandardCharsets.UTF_8).replace("\0", "");
                if (type == 'L')
                    longName = s;
                else
                    longLink = s;
                continue;
            }
            if (type == 'x' || type == 'g') { // pax extended header: path= and linkpath= records
                String pax = new String(readData(in, size), StandardCharsets.UTF_8);
                for (String rec : pax.split("\n")) {
                    int sp = rec.indexOf(' '), eq = rec.indexOf('=');
                    if (sp < 0 || eq < sp)
                        continue;
                    String key = rec.substring(sp + 1, eq), val = rec.substring(eq + 1);
                    if (type == 'x' && key.equals("path"))
                        longName = val;
                    else if (type == 'x' && key.equals("linkpath"))
                        longLink = val;
                }
                continue;
            }

            while (name.startsWith("./"))
                name = name.substring(2);
            File f = new File(dest, name);
            if (!f.getCanonicalPath().startsWith(dest.getCanonicalPath())) // "../" in a name: never outside dest
                throw new IOException("unsafe path in archive: " + name);
            if (progress != null)
                progress.onEntry(name);

            switch (type) {
                case '5':
                    f.mkdirs();
                    dirModes.add(new Object[] {f, mode});
                    break;
                case '2':
                    f.getParentFile().mkdirs();
                    f.delete();
                    try {
                        Os.symlink(link, f.getPath());
                    } catch (ErrnoException e) {
                        throw new IOException("symlink " + name, e);
                    }
                    break;
                case '0':
                case '\0':
                case '7':
                    f.getParentFile().mkdirs();
                    try (OutputStream out = new BufferedOutputStream(new FileOutputStream(f), 1 << 16)) {
                        long left = size;
                        while (left > 0) {
                            int n = in.read(buf, 0, (int) Math.min(buf.length, left));
                            if (n < 0)
                                throw new IOException("truncated archive at " + name);
                            out.write(buf, 0, n);
                            left -= n;
                        }
                    }
                    skip(in, pad(size));
                    chmod(f, mode);
                    break;
                default: // hard links (dereferenced by the build), devices, fifos: nothing an app can create
                    skip(in, size + pad(size));
            }
        }
        for (int i = dirModes.size() - 1; i >= 0; i--) // deepest first
            chmod((File) dirModes.get(i)[0], (Integer) dirModes.get(i)[1]);
    }

    private static void chmod(File f, int mode) {
        try {
            Os.chmod(f.getPath(), mode & 07777);
        } catch (ErrnoException e) {
            try {
                Os.chmod(f.getPath(), mode & 0777); // setuid/setgid bits are not for apps
            } catch (ErrnoException ignored) {
            }
        }
    }

    private static byte[] readData(InputStream in, long size) throws IOException {
        byte[] d = new byte[(int) size];
        if (!readFully(in, d))
            throw new IOException("truncated archive");
        skip(in, pad(size));
        return d;
    }

    private static long pad(long size) {
        return (512 - size % 512) % 512;
    }

    private static boolean readFully(InputStream in, byte[] b) throws IOException {
        int off = 0;
        while (off < b.length) {
            int n = in.read(b, off, b.length - off);
            if (n < 0)
                return off == 0 ? false : fail();
            off += n;
        }
        return true;
    }

    private static boolean fail() throws IOException {
        throw new IOException("truncated archive");
    }

    private static void skip(InputStream in, long n) throws IOException {
        byte[] b = new byte[8192];
        while (n > 0) {
            int r = in.read(b, 0, (int) Math.min(b.length, n));
            if (r < 0)
                throw new IOException("truncated archive");
            n -= r;
        }
    }

    private static boolean isZero(byte[] h) {
        for (byte x : h)
            if (x != 0)
                return false;
        return true;
    }

    private static String str(byte[] h, int off, int len) {
        int end = off;
        while (end < off + len && h[end] != 0)
            end++;
        return new String(h, off, end - off, StandardCharsets.UTF_8);
    }

    private static long octal(byte[] h, int off, int len) {
        if ((h[off] & 0x80) != 0) { // GNU base-256 for large sizes
            long v = h[off] & 0x7f;
            for (int i = 1; i < len; i++)
                v = (v << 8) | (h[off + i] & 0xff);
            return v;
        }
        String s = str(h, off, len).trim();
        return s.isEmpty() ? 0 : Long.parseLong(s, 8);
    }
}
