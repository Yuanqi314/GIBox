package dev.modelswap.only;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;

/**
 * Names the user gave to mods, by Mods entry name (app-private, one file per game: labels/<pkg>.json). Only what the
 * app shows changes: folders, models and the game's selection keep the entry name.
 */
public final class Labels {
    private Labels() {}
    public static final int MAX = 60;

    /** Empty when there is no file or it cannot be read. */
    public static Map<String, String> read(File file) {
        Map<String, String> out = new LinkedHashMap<>();
        if (!file.isFile()) return out;
        try {
            Map<String, Object> j = Json.obj(Json.read(file, 1L << 20), "labels");
            for (Map.Entry<String, Object> e : j.entrySet()) if (e.getValue() instanceof String && !((String) e.getValue()).trim().isEmpty()) out.put(e.getKey(), (String) e.getValue());
        } catch (Exception ignored) {}
        return out;
    }

    /** A blank label removes the entry (the Mods entry name shows again). Line breaks become spaces; at most MAX characters. */
    public static synchronized void set(File file, String name, String label) throws Exception {
        Map<String, String> all = new TreeMap<>(read(file));
        String clean = label == null ? "" : label.replaceAll("[\\r\\n\\t]+", " ").trim();
        if (clean.length() > MAX) clean = clean.substring(0, MAX).trim();
        if (clean.isEmpty() || clean.equals(name)) all.remove(name); else all.put(name, clean);
        File dir = file.getParentFile(); if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IllegalStateException("无法创建：" + dir);
        File tmp = new File(file.getPath() + ".tmp");
        Files.write(tmp.toPath(), Json.write(new LinkedHashMap<String, Object>(all)).getBytes(StandardCharsets.UTF_8));
        Files.move(tmp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    }

    public static String of(Map<String, String> labels, String name) { String l = labels.get(name); return l == null ? name : l; }
}
