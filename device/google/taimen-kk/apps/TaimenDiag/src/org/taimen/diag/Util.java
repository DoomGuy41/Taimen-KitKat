package org.taimen.diag;

import android.os.SystemProperties;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.util.Arrays;

/** Small helpers: reading/writing sysfs files, properties, formatting. */
final class Util {
    private Util() {}

    /** Contents of a file (without the final newline), or "? (reason)". */
    static String read(String path) {
        BufferedReader r = null;
        try {
            r = new BufferedReader(new FileReader(path), 8192);
            StringBuilder sb = new StringBuilder();
            String l;
            while ((l = r.readLine()) != null) {
                if (sb.length() > 0) sb.append('\n');
                sb.append(l);
            }
            return sb.toString().trim();
        } catch (Exception e) {
            return "? (" + reason(e) + ")";
        } finally {
            try { if (r != null) r.close(); } catch (Exception e) { }
        }
    }

    static boolean ok(String v) {
        return v != null && !v.startsWith("? (");
    }

    /** Writes a value; returns null if it worked, the reason otherwise. */
    static String write(String path, String value) {
        FileOutputStream o = null;
        try {
            o = new FileOutputStream(path);
            o.write(value.getBytes());
            return null;
        } catch (Exception e) {
            return reason(e);
        } finally {
            try { if (o != null) o.close(); } catch (Exception e) { }
        }
    }

    static String reason(Throwable e) {
        String m = e.getMessage();
        if (m == null) return e.getClass().getSimpleName();
        int i = m.indexOf(": ");
        return i >= 0 && m.startsWith("/") ? m.substring(i + 2) : m;
    }

    static String prop(String key) {
        String v = SystemProperties.get(key, "");
        return v.length() == 0 ? "(empty)" : v;
    }

    static String[] list(String dir) {
        String[] l = new File(dir).list();
        if (l == null) return new String[0];
        Arrays.sort(l);
        return l;
    }

    /** Natural order: thermal_zone2 before thermal_zone10. */
    static String[] listNatural(String dir) {
        String[] l = list(dir);
        Arrays.sort(l, new java.util.Comparator<String>() {
            public int compare(String a, String b) {
                int na = trailing(a), nb = trailing(b);
                String pa = a.substring(0, a.length() - digits(a)), pb = b.substring(0, b.length() - digits(b));
                int c = pa.compareTo(pb);
                return c != 0 ? c : na - nb;
            }
        });
        return l;
    }

    private static int digits(String s) {
        int n = 0;
        while (n < s.length() && Character.isDigit(s.charAt(s.length() - 1 - n))) n++;
        return n;
    }

    private static int trailing(String s) {
        int d = digits(s);
        if (d == 0) return -1;
        try { return Integer.parseInt(s.substring(s.length() - d)); } catch (Exception e) { return -1; }
    }

    static String pad(String s, int n) {
        if (s == null) s = "null";
        StringBuilder sb = new StringBuilder(s);
        while (sb.length() < n) sb.append(' ');
        return sb.toString();
    }

    static String bytes(long b) {
        if (b < 1024) return b + " B";
        if (b < 1024 * 1024) return String.format("%.1f KiB", b / 1024.0);
        if (b < 1024L * 1024 * 1024) return String.format("%.1f MiB", b / 1048576.0);
        return String.format("%.2f GiB", b / 1073741824.0);
    }

    /** Temperature read from sysfs: millidegrees, tenths or degrees depending on the magnitude. */
    static String temp(String raw) {
        try {
            long v = Long.parseLong(raw.trim());
            double c = Math.abs(v) >= 1000 ? v / 1000.0 : Math.abs(v) >= 150 ? v / 10.0 : v;
            return String.format("%.1f °C", c);
        } catch (Exception e) {
            return raw;
        }
    }
}
