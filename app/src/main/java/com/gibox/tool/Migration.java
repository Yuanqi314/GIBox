package com.gibox.tool;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;

/**
 * One-time move from GIBox 0.5 and earlier, whose files sat in the game's media folder under 独立模型替换/:
 * <ul>
 * <li>imported mods, 独立模型替换/mods/*, move to GIBox's private storage (Control.mods), modelswap.txt included;</li>
 * <li>独立模型替换/control.json (switch, selection, variants, log level) is copied to GIBox/control.json if there is none.</li>
 * </ul>
 * Models converted by 0.5 are in a format GIBox no longer reads: the moved mods are converted again (AppState offers
 * it). The skeleton library is recorded again by the game the next time each character appears.
 */
public final class Migration {
    private Migration() {}

    public static File legacy(String pkg) { return new File("/storage/emulated/0/Android/media/" + pkg + "/独立模型替换"); }

    public static final class Report {
        /** The mods now in the Mods folder. */
        public final List<File> mods = new ArrayList<>();
        public boolean control;
        public final List<String> errors = new ArrayList<>();
    }

    private static File[] oldMods(File legacy) { File[] a = new File(legacy, "mods").listFiles(f -> f.isDirectory() && !f.getName().startsWith(".")); return a == null ? new File[0] : a; }

    public static boolean needed(File legacy, File shared) {
        return oldMods(legacy).length > 0 || (new File(legacy, "control.json").isFile() && !new File(shared, "control.json").exists());
    }

    public static Report run(File legacy, File shared, File modsRoot) {
        Report r = new Report();
        File control = new File(legacy, "control.json"), target = new File(shared, "control.json");
        if (control.isFile() && !target.exists()) try {
            if (!shared.isDirectory() && !shared.mkdirs()) throw new IllegalStateException("无法创建：" + shared);
            File tmp = new File(shared, "control.json.tmp"); Files.copy(control.toPath(), tmp.toPath(), StandardCopyOption.REPLACE_EXISTING);
            Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE); r.control = true;
        } catch (Exception e) { r.errors.add("control.json：" + e.getMessage()); }
        File[] mods = oldMods(legacy);
        if (mods.length > 0 && !modsRoot.isDirectory() && !modsRoot.mkdirs()) { r.errors.add("无法创建：" + modsRoot); return r; }
        for (File d : mods) {
            File dest = free(modsRoot, ModLibrary.modelName(d.getName()));
            try {
                // Shared storage and private storage are different file systems: copy, then delete the original.
                Importer.copy(d, dest, 0); ModelConverter.remove(d); r.mods.add(dest);
            } catch (Exception e) {
                r.errors.add(d.getName() + "：" + e.getMessage());
                try { ModelConverter.remove(dest); } catch (Exception ignored) {}
            }
        }
        try { ModelConverter.remove(new File(legacy, "mods/.pending")); } catch (Exception ignored) {}
        return r;
    }

    private static File free(File root, String name) {
        File f = new File(root, name); if (!f.exists()) return f;
        for (int i = 2; ; i++) { f = new File(root, ModLibrary.modelName(name + " (" + i + ")")); if (!f.exists()) return f; }
    }
}
