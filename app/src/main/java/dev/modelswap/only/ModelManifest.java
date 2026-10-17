package dev.modelswap.only;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * What the app needs from a GIBox model folder's model.json (format "gibox-model", version 1; the full check of the
 * data is the runtime's, model.hpp): the character, the variants, where it was converted from. learned.json adds the
 * character ID the runtime found for a model converted by name.
 */
public final class ModelManifest {
    public int id;
    /** Character ID found in game for a model converted by name (learned.json), else 0. */
    public int learnedId;
    public final List<String> names = new ArrayList<>();
    public String name = "", generation = "", sourcePath = "", sourceSignature = "";
    public final Map<String, List<Integer>> vars = new LinkedHashMap<>();
    public final List<Map<String, Integer>> combos = new ArrayList<>();
    public int defaultCombo;

    public static boolean exists(File dir) { return new File(dir, "model.json").isFile(); }

    public static ModelManifest read(File dir) throws Exception {
        Map<String, Object> m = Json.obj(Json.read(new File(dir, "model.json"), 4L << 20), "model.json");
        Json.check("gibox-model".equals(m.get("format")), "不是 GIBox 模型：" + dir.getName());
        long version = Json.integer(m.get("version"), "version", 1, 1000000); Json.check(version == 1, "模型格式版本 " + version + " 不支持（新版 GIBox 转换的？）：" + dir.getName());
        ModelManifest r = new ModelManifest();
        r.name = Json.str(m.get("name"), "name", 512); r.generation = Json.str(m.get("generation"), "generation", 64);
        Map<String, Object> c = Json.obj(m.get("character"), "character");
        r.id = (int) Json.integer(c.get("id"), "character.id", 0, 10001999); Json.check(r.id == 0 || r.id >= 10000000, "角色 ID 越界");
        if (c.get("names") != null) for (Object n : Json.arr(c.get("names"), "character.names", 0, 16)) { String s = Json.str(n, "character.names", 40); Json.check(s.matches("[a-z0-9]{3,40}"), "角色名格式：" + s); r.names.add(s); }
        Json.check(r.id != 0 || !r.names.isEmpty(), "模型没有角色 ID 也没有角色名");
        Object v = m.get("variants");
        if (v != null) {
            Map<String, Object> vo = Json.obj(v, "variants");
            for (Object x : Json.arr(vo.get("vars"), "variants.vars", 0, 16)) {
                Map<String, Object> e = Json.obj(x, "variants.vars"); String n = Json.str(e.get("name"), "variants.vars.name", 64); Json.check(n.length() > 1 && n.startsWith("$") && !r.vars.containsKey(n), "款式变量：" + n);
                List<Integer> values = new ArrayList<>(); for (Object k : Json.arr(e.get("values"), n, 1, 64)) values.add((int) Json.integer(k, n, -1000000, 1000000)); r.vars.put(n, values);
            }
            for (Object x : Json.arr(vo.get("combos"), "variants.combos", 1, 32)) {
                Map<String, Integer> combo = new LinkedHashMap<>();
                for (Map.Entry<String, Object> e : Json.obj(x, "variants.combos").entrySet()) { Json.check(r.vars.containsKey(e.getKey()), "款式组合里有未声明的变量：" + e.getKey()); combo.put(e.getKey(), (int) Json.integer(e.getValue(), e.getKey(), -1000000, 1000000)); }
                Json.check(combo.size() == r.vars.size(), "款式组合必须给每个变量赋值"); r.combos.add(combo);
            }
            r.defaultCombo = (int) Json.integer(vo.get("default"), "variants.default", 0, r.combos.size() - 1);
        }
        Object s = m.get("source");
        if (s != null) { Map<String, Object> so = Json.obj(s, "source"); if (so.get("path") instanceof String) r.sourcePath = (String) so.get("path"); if (so.get("signature") instanceof String) r.sourceSignature = (String) so.get("signature"); }
        File learned = new File(dir, "learned.json");
        if (learned.isFile()) try {
            Map<String, Object> l = Json.obj(Json.read(learned, 1L << 20), "learned.json");
            if ("gibox-learned".equals(l.get("format")) && l.get("id") != null) { int id = (int) Json.integer(l.get("id"), "learned.id", 0, 10001999); if (id == 0 || id >= 10000000) r.learnedId = id; }
        } catch (Exception ignored) {}
        return r;
    }

    /** The character this model is for: its own ID, else the one the runtime learned. */
    public int character() { return id != 0 ? id : learnedId; }
}
