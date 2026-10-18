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
    private static File choose(List<File> files) {
        List<File> models = new ArrayList<>(); for (File f : files) if (geometry(f)) models.add(f);
        if (models.size() == 1) return models.get(0);
        if (models.size() > 1) throw new IllegalArgumentException("找到多份模型 INI，请分开导入或先合并：" + models);
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
