package com.gibox.tool;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

public final class Control {
    private Control() {}
    public static boolean supported(String pkg) { return "com.miHoYo.Yuanshen".equals(pkg) || "com.miHoYo.ys.bilibili".equals(pkg); }
    /**
     * Everything the game reads or writes: the game's own media folder, since the game process cannot open GIBox's
     * private storage. models/ (converted models), skeletons/ (skeleton library), control.json, status.json, the log.
     */
    public static File shared(String pkg) { return new File("/storage/emulated/0/Android/media/" + pkg + "/GIBox"); }
    public static File models(String pkg) { return new File(shared(pkg), "models"); }
    public static File bones(String pkg) { return new File(shared(pkg), "skeletons"); }
    /** The Mods folder: imported PC mods, only ever read by the app, in GIBox's private storage (filesDir/mods/<pkg>). */
    public static File mods(File filesDir, String pkg) { return new File(new File(filesDir, "mods"), pkg); }
    public static JSONObject load(String pkg) {
        try { return new JSONObject(new String(Files.readAllBytes(new File(shared(pkg), "control.json").toPath()), StandardCharsets.UTF_8)); }
        catch (Exception e) { JSONObject j = new JSONObject(); try { j.put("enabled", false); j.put("selected", new JSONArray()); j.put("exportBones", 0); j.put("logLevel", "info"); } catch (Exception ignored) {} return j; }
    }
    public static synchronized void save(String pkg, JSONObject j) throws Exception {
        File dir = shared(pkg); if (!dir.isDirectory() && !dir.mkdirs()) throw new IllegalStateException("无法创建控制目录：" + dir);
        File tmp = new File(dir, "control.json.tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) { out.write(j.toString().getBytes(StandardCharsets.UTF_8)); out.getFD().sync(); }
        Files.move(tmp.toPath(), new File(dir, "control.json").toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING, java.nio.file.StandardCopyOption.ATOMIC_MOVE);
    }
    /** A newly imported model is selected and replacement is switched on, so the import shows up without further steps. */
    public static synchronized void enable(String pkg, String model) throws Exception {
        JSONObject j = load(pkg); JSONArray a = j.optJSONArray("selected"); if (a == null) a = new JSONArray(); boolean listed = false;
        for (int i = 0; i < a.length(); i++) if (model.equals(a.optString(i))) listed = true;
        if (!listed) a.put(model); j.put("selected", a); j.put("enabled", true); save(pkg, j);
    }
    public static synchronized void disable(String pkg, String model) throws Exception {
        JSONObject j = load(pkg); JSONArray a = j.optJSONArray("selected"), keep = new JSONArray();
        if (a != null) for (int i = 0; i < a.length(); i++) if (!model.equals(a.optString(i))) keep.put(a.optString(i));
        j.put("selected", keep); save(pkg, j);
    }
    public static boolean selected(String pkg, String model) {
        JSONArray a = load(pkg).optJSONArray("selected"); if (a != null) for (int i = 0; i < a.length(); i++) if (model.equals(a.optString(i))) return true; return false;
    }
    public static boolean enabled(String pkg) { return load(pkg).optBoolean("enabled", false); }
    public static synchronized void setEnabled(String pkg, boolean on) throws Exception { JSONObject j = load(pkg); j.put("enabled", on); save(pkg, j); }
    /** Log file verbosity, as in XXMI: disabled / warning / info / debug (control.json "logLevel", read by the game too). */
    public static final String[] LOG_LEVELS = {"disabled", "warning", "info", "debug"};
    public static final String[] LOG_LEVEL_LABELS = {"禁用", "警告", "信息", "调试"};
    public static String logLevel(String pkg) { String s = load(pkg).optString("logLevel", "info"); for (String l : LOG_LEVELS) if (l.equals(s)) return s; return "info"; }
    public static int logThreshold(String pkg) { return AppLog.threshold(logLevel(pkg)); }
    public static synchronized void setLogLevel(String pkg, String level) throws Exception { JSONObject j = load(pkg); j.put("logLevel", level); save(pkg, j); }
    /** A monotonically increasing marker; game retains the preceding 30 seconds only while logLevel=debug. */
    /** Material experiments (control.json "experiments"; read by the game, debug only): glowTrace, gameMaterials,
     *  neutralColors, noUv2, separateDraws (booleans), firstRefreshMs (0 = default 1.5 s) and slotTextures (0 off,
     *  1 per-material property block, 2 per-material custom texture); for animations animationMode (0 cached frame
     *  meshes, 1 dynamic, 2 paused) and logicCore (boolean: the mod's own INI logic picks the frame shown). */
    public static boolean experiment(String pkg, String key) { JSONObject x = load(pkg).optJSONObject("experiments"); return x != null && x.optBoolean(key, false); }
    public static int experimentMs(String pkg) { return experimentInt(pkg, "firstRefreshMs"); }
    public static int experimentInt(String pkg, String key) { JSONObject x = load(pkg).optJSONObject("experiments"); return x == null ? 0 : x.optInt(key, 0); }
    public static synchronized void setExperiment(String pkg, String key, Object value) throws Exception {
        JSONObject j = load(pkg); JSONObject x = j.optJSONObject("experiments"); if (x == null) x = new JSONObject();
        x.put(key, value); j.put("experiments", x); save(pkg, j);
    }
    public static synchronized void requestRenderMark(String pkg) throws Exception {
        JSONObject j = load(pkg);
        if (!"debug".equals(j.optString("logLevel", "info"))) throw new IllegalStateException("请先把日志等级设为调试，再复现异常");
        long previous = j.optLong("renderMark", 0);
        if (previous < 0 || previous >= 9007199254740991L) throw new IllegalStateException("画面标记计数无效");
        j.put("renderMark", Math.max(System.currentTimeMillis(), previous + 1)); save(pkg, j);
    }
    /** Variant choice per model, read by the runtime from control.json "variables". */
    public static synchronized void setVariable(String pkg, String model, String var, int value) throws Exception {
        JSONObject j = load(pkg); JSONObject all = j.optJSONObject("variables"); if (all == null) all = new JSONObject();
        JSONObject m = all.optJSONObject(model); if (m == null) m = new JSONObject(); m.put(var, value); all.put(model, m); j.put("variables", all); save(pkg, j);
    }
    /** Several variables of one model in one write (显示替换 can change more than one). */
    public static synchronized void setVariables(String pkg, String model, java.util.Map<String, Integer> values) throws Exception {
        JSONObject j = load(pkg); JSONObject all = j.optJSONObject("variables"); if (all == null) all = new JSONObject();
        JSONObject m = all.optJSONObject(model); if (m == null) m = new JSONObject(); for (java.util.Map.Entry<String, Integer> e : values.entrySet()) m.put(e.getKey(), (int) e.getValue()); all.put(model, m); j.put("variables", all); save(pkg, j);
    }
    public static int variable(String pkg, String model, String var, int fallback) {
        JSONObject all = load(pkg).optJSONObject("variables"); JSONObject m = all == null ? null : all.optJSONObject(model); return m == null ? fallback : m.optInt(var, fallback);
    }
    /** status.json written by the game side (runtime / bootstrap). loaded=false: no file yet (game not started with the module). */
    public static final class Status {
        public boolean loaded; public String state = "", detail = "", scan = "", model = ""; public int characterId; public final List<String> models = new ArrayList<>();
        public long boneRefreshRequest; public int boneRefreshCharacter; public String boneRefreshState = "", boneRefreshDetail = "";
    }
    public static Status status(String pkg) {
        Status st = new Status();
        try {
            JSONObject j = new JSONObject(new String(Files.readAllBytes(new File(shared(pkg), "status.json").toPath()), StandardCharsets.UTF_8));
            st.loaded = true; st.state = j.optString("state", ""); st.detail = j.optString("detail", ""); st.scan = j.optString("scan", ""); st.model = j.optString("model", ""); st.characterId = j.optInt("characterId", 0);
            JSONArray a = j.optJSONArray("models"); if (a != null) for (int i = 0; i < a.length(); i++) st.models.add(a.optString(i));
            st.boneRefreshRequest = j.optLong("boneRefreshRequest", 0); st.boneRefreshCharacter = j.optInt("boneRefreshCharacter", 0);
            st.boneRefreshState = j.optString("boneRefreshState", ""); st.boneRefreshDetail = j.optString("boneRefreshDetail", "");
        } catch (Exception ignored) {}
        return st;
    }
    /** The game exports the current character's skeleton once more (a timestamped folder next to the bone library). */
    public static synchronized void requestBoneExport(String pkg) throws Exception { JSONObject j = load(pkg); j.put("exportBones", j.optInt("exportBones", 0) + 1); save(pkg, j); }
    /** Re-capture one character into the live bone library; the game retains the request until verified and saved. */
    public static synchronized void requestBoneRefresh(String pkg, int character) throws Exception {
        if (character < 10000000 || character > 10001999) throw new IllegalArgumentException("请先选择角色");
        JSONObject j = load(pkg); long previous = j.optLong("refreshBones", 0);
        if (previous < 0 || previous >= 9007199254740991L) previous = 0;
        j.put("refreshBones", Math.max(System.currentTimeMillis(), previous + 1)); j.put("refreshBonesCharacter", character); save(pkg, j);
    }
}
