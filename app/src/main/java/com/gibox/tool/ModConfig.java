package com.gibox.tool;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.regex.Pattern;

/** Select geometry by its statements, not by an INI's name or the number of INIs in a directory. */
final class ModConfig {
    private ModConfig() {}
    private static final Pattern INDEX = Pattern.compile("(?i)^ib\\s*=\\s*(?:ref\\s+)?resource\\S+.*");
    private static final Pattern VERTEX = Pattern.compile("(?i)^vb[0-9]+\\s*=\\s*(?:ref\\s+)?resource\\S+.*");
    private static final Pattern DRAW = Pattern.compile("(?i)^(?:drawindexed\\s*=|run\\s*=\\s*commandlist\\\\texfx\\\\(?:component|c)(?:\\.[01])?\\s*$).*");
    static List<File> inis(File dir) {
        File[] files = dir.listFiles(f -> f.isFile() && f.getName().toLowerCase(Locale.ROOT).endsWith(".ini")
            && !f.getName().toLowerCase(Locale.ROOT).startsWith("disabled"));
        List<File> out = new ArrayList<>(); if (files != null) Collections.addAll(out, files);
        out.sort(Comparator.comparing(File::getName)); return out;
    }
    static String source(File file) {
        try {
            if (file.length() > 4L * 1024 * 1024) throw new IllegalArgumentException("INI 文件过大：" + file);
            return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).replace("\uFEFF", "");
        } catch (IOException e) { throw new IllegalArgumentException("无法读取 INI：" + file, e); }
    }
    static boolean geometry(File file) {
        boolean vertex = false, draw = false; String section = "";
        for (String raw : source(file).split("\\r?\\n")) {
            int comment = raw.indexOf(';'); String l = (comment < 0 ? raw : raw.substring(0, comment)).trim();
            if (l.startsWith("[") && l.endsWith("]")) section = l.substring(1, l.length() - 1).toLowerCase(Locale.ROOT);
            else if (section.startsWith("textureoverride") || section.startsWith("commandlist")) {
                if (INDEX.matcher(l).matches()) return true;
                vertex |= VERTEX.matcher(l).matches(); draw |= DRAW.matcher(l).matches();
            }
        }
        return vertex && draw;
    }
    /** The schemes to offer: every model INI XXMI would load, then the ones a mod ships switched off in a "disabled"
     *  folder (a common way to package an alternative), except exact copies of one already listed. Picking one of
     *  those stands for switching it on and the others off, as a PC user would. A file named "DISABLED..." is not
     *  offered: that is how old versions and backups are kept. */
    static List<File> schemeInis(File dir) {
        List<File> out = geometryInis(dir); Set<String> seen = new HashSet<>(); for (File f : out) seen.add(source(f));
        List<File> off = new ArrayList<>(); disabled(dir, off, 0, false);
        for (File f : off) if (geometry(f) && seen.add(source(f))) out.add(f);
        return out;
    }
    /** Where a scheme sits once switched on: a switched-off one (its file, or a folder on its path, named "disabled...")
     *  is moved out of the switched-off folder by a PC user, so its paths are relative to the folder that holds that
     *  folder (to its own folder for a "DISABLED x.ini" file). */
    static File switchedOn(File mod, File ini) {
        try { File root = mod.getCanonicalFile(), f = ini.getCanonicalFile(), on = f.getParentFile();
            for (File d = f.getParentFile(); d != null && !d.equals(root) && d.getPath().startsWith(root.getPath()); d = d.getParentFile())
                if (d.getName().toLowerCase(Locale.ROOT).startsWith("disabled")) on = d.getParentFile();
            return on;
        } catch (IOException e) { return ini.getParentFile(); }
    }
    private static void disabled(File dir, List<File> out, int depth, boolean under) {
        File[] all = dir.listFiles(); if (all == null || depth > 4) return; Arrays.sort(all, Comparator.comparing(File::getName));
        for (File f : all) { String n = f.getName().toLowerCase(Locale.ROOT); if (n.startsWith(".")) continue;
            if (f.isDirectory()) disabled(f, out, depth + 1, under || n.startsWith("disabled")); else if (under && !n.startsWith("disabled") && n.endsWith(".ini")) out.add(f); }
    }
    /** Every INI with model geometry under dir (several = alternative schemes of one package). */
    static List<File> geometryInis(File dir) { List<File> out = new ArrayList<>(); for (File f : allInis(dir)) if (geometry(f)) out.add(f); return out; }
    /** The scheme's path as stored in modelswap.txt ("ini"): relative to the mod folder, with forward slashes. */
    static String relative(File dir, File ini) { return dir.toPath().toAbsolutePath().normalize().relativize(ini.toPath().toAbsolutePath().normalize()).toString().replace('\\', '/'); }
    /** A stored scheme, only if it is still a model INI inside dir. */
    static File scheme(File dir, String rel) {
        if (rel == null || rel.isEmpty() || rel.startsWith("/") || rel.contains("..")) return null;
        try {
            File f = new File(dir, rel).getCanonicalFile(); String root = dir.getCanonicalPath() + File.separator;
            if (!f.getPath().startsWith(root) || !f.isFile() || !f.getName().toLowerCase(Locale.ROOT).endsWith(".ini") || !geometry(f)) return null;
            return f;
        } catch (IOException e) { return null; }
    }
    /** The model INI to convert: the stored scheme, else the only one. Several without a choice ask for one. */
    static File find(File dir, String rel) {
        if (rel != null && !rel.isEmpty()) { File f = scheme(dir, rel); if (f == null) throw new IllegalArgumentException("选择的 INI 方案已不存在：" + rel + "，请重新选择"); return f; }
        return find(dir);
    }
    private static File choose(List<File> files) {
        List<File> models = new ArrayList<>(); for (File f : files) if (geometry(f)) models.add(f);
        if (models.size() == 1) return models.get(0);
        if (models.size() > 1) throw new IllegalArgumentException("这个 MOD 有 " + models.size() + " 套 INI 方案，请先选择要用哪一套");
        if (files.size() == 1) return files.get(0); // Preserve a precise converter error for texture-only inputs.
        throw new IllegalArgumentException(files.isEmpty() ? "找不到 MOD 的 INI 文件"
            : "只有帮助或贴图配置，没有可确定的模型主 INI：" + files);
    }
    static File mainIni(File dir) { return choose(inis(dir)); }
    static File find(File dir) { return choose(allInis(dir)); }
    static List<File> allInis(File dir) {
        if (!dir.isDirectory()) throw new IllegalArgumentException("不是文件夹：" + dir);
        List<File> files = new ArrayList<>(), level = new ArrayList<>(Collections.singletonList(dir));
        for (int depth = 0; depth <= 4 && !level.isEmpty(); depth++) {
            List<File> next = new ArrayList<>();
            for (File d : level) {
                files.addAll(inis(d));
                File[] children = d.listFiles(f -> f.isDirectory() && !f.getName().startsWith(".")
                    && !f.getName().toLowerCase(Locale.ROOT).startsWith("disabled"));
                if (children != null) Collections.addAll(next, children);
            }
            level = next;
        }
        return files;
    }
}
