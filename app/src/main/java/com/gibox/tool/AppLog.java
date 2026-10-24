package com.gibox.tool;

import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * "GIBox Log.txt" in the XXMI Launcher line format, the same file and format the game side writes (logfmt.hpp):
 *   2026-10-17 21:03:12,345 482113 importer INFO 导入 Citlali Summer：茜特菈莉（10000107）
 * The six digits are this process's instance id (start time in ms mod 1000000), as in XXMI.
 */
public final class AppLog {
    private AppLog() {}
    public static final int DEBUG = 10, INFO = 20, WARNING = 30, ERROR = 40, DISABLED = 1000;
    public static final String FILE = "GIBox Log.txt";
    private static final int INSTANCE = (int) (System.currentTimeMillis() % 1000000);

    public static String levelName(int level) { return level >= ERROR ? "ERROR" : level >= WARNING ? "WARNING" : level >= INFO ? "INFO" : "DEBUG"; }
    /** control.json "logLevel" (disabled / warning / info / debug) as a threshold. */
    public static int threshold(String logLevel) {
        String s = logLevel == null ? "" : logLevel.trim().toLowerCase(Locale.ROOT);
        return s.equals("disabled") ? DISABLED : s.equals("warning") ? WARNING : s.equals("debug") ? DEBUG : INFO;
    }
    public static String line(String logger, int level, String message, long millis) {
        String head = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss,SSS", Locale.ROOT).format(new Date(millis)) + " " + String.format(Locale.ROOT, "%06d", INSTANCE) + " " + logger + " " + levelName(level) + " ";
        return head + message.replace("\r", " ").replace("\n", "\n    ") + "\n";
    }
    /** Appends one line when the level passes; past 2 MB the file moves to "GIBox Log.txt.1". Never throws. */
    public static synchronized void write(File dir, int threshold, String logger, int level, String message) {
        if (level < threshold) return;
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) return;
            File f = new File(dir, FILE); if (f.length() > 2L * 1024 * 1024) { File old = new File(dir, FILE + ".1"); old.delete(); f.renameTo(old); }
            try (FileOutputStream out = new FileOutputStream(f, true)) { out.write(line(logger, level, message, System.currentTimeMillis()).getBytes(StandardCharsets.UTF_8)); }
        } catch (Throwable ignored) {}
    }
    public static void log(String pkg, String logger, int level, String message) { write(Control.shared(pkg), Control.logThreshold(pkg), logger, level, message); }
    public static void debug(String pkg, String logger, String message) { log(pkg, logger, DEBUG, message); }
    public static void info(String pkg, String logger, String message) { log(pkg, logger, INFO, message); }
    public static void warning(String pkg, String logger, String message) { log(pkg, logger, WARNING, message); }
    public static void error(String pkg, String logger, String message) { log(pkg, logger, ERROR, message); }
    public static File file(String pkg) { return new File(Control.shared(pkg), FILE); }

    /** The last `maxBytes` of the log, starting at a line boundary. */
    public static String tail(File f, int maxBytes) {
        try (RandomAccessFile in = new RandomAccessFile(f, "r")) {
            long n = in.length(), start = Math.max(0, n - maxBytes); byte[] b = new byte[(int) (n - start)]; in.seek(start); in.readFully(b);
            String s = new String(b, StandardCharsets.UTF_8); if (start > 0) { int nl = s.indexOf('\n'); if (nl >= 0) s = s.substring(nl + 1); }
            return s;
        } catch (Exception e) { return ""; }
    }
}
