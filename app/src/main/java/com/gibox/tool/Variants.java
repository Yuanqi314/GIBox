package com.gibox.tool;

import java.io.File;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** The switchable variables of a converted model (model.json "variants"), their values, and the default combination's values. */
public final class Variants {
    public final Map<String, List<Integer>> values = new LinkedHashMap<>();
    public final Map<String, Integer> defaults = new LinkedHashMap<>();
    /** Empty when the model has no variants or its model.json cannot be read. */
    public static Variants read(File model) {
        Variants s = new Variants();
        try {
            ModelManifest m = ModelManifest.read(model);
            s.values.putAll(m.vars);
            if (m.defaultCombo < m.combos.size()) s.defaults.putAll(m.combos.get(m.defaultCombo));
        } catch (Exception e) { s.values.clear(); s.defaults.clear(); }
        return s;
    }
    /** The value in effect: the one chosen in control.json, else the default combination's, else the first. */
    public int current(String pkg, String model, String var) {
        List<Integer> v = values.get(var); int fallback = defaults.containsKey(var) ? defaults.get(var) : v == null || v.isEmpty() ? 0 : v.get(0);
        return Control.variable(pkg, model, var, fallback);
    }
}
