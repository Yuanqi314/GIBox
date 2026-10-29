package com.gibox.tool;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.*;

/**
 * Texture-only mods, phase 1 (read-only diagnostics): what the mod's INI asks for, written as
 * GIBox/texture-diag/<model name>.json for the game side (texture_diag.hpp), which matches it against the character's
 * live renderers and only logs. Nothing here builds a model or changes what the game draws.
 *
 * Read from the INI, never inferred from file contents: the TextureOverride hash (the PC 3DMigoto hash, not recomputed
 * here: on Android there is no D3D11 buffer it was taken from), match_first_index / match_priority, drawindexed,
 * texture bindings (ps-tN, vs-tN, this, GIMI's Resource\GIMI\Diffuse / LightMap / NormalMap with
 * CommandList\GIMI\SetTextures) with the if / else conditions they sit under, local and external CommandLists, the
 * variables and keys behind those conditions, the files the resources name (DDS / PNG headers only) and the other INIs
 * in the folder that touch the same hashes.
 */
final class TextureDiag {
    private TextureDiag() {}
    static final String DIR = "texture-diag";
    static final int VERSION = 1;

    private static String lower(String s) { return s.toLowerCase(Locale.ROOT); }
    private static String[] kv(String l) { int p = l.indexOf('='); return p < 0 ? null : new String[]{lower(l.substring(0, p).trim()), l.substring(p + 1).trim()}; }
    private static String condition(String line) { String a = lower(line); return a.startsWith("if ") ? a.substring(3).trim() : null; }
    private static String elseCondition(String line) { String a = lower(line); return a.startsWith("else if ") ? a.substring(8).trim() : a.startsWith("elif ") ? a.substring(5).trim() : a.startsWith("elseif ") ? a.substring(7).trim() : null; }
    private static String resourceName(String value) { return lower(value.replaceFirst("(?i)^ref\\s+", "").trim()); }

    /** The diagnostic file of a model name (the folder name a successful conversion would have had). */
    static File file(File dir, String name) { return new File(dir, name + ".json"); }

    /** A texture slot: GIMI's named resources, direct shader slots, or "this" (a texture-hash override). */
    static boolean textureKey(String k) { return k.matches("(ps|vs)-t[0-9]+") || k.startsWith("resource\\gimi\\") || k.equals("this"); }
    /** What the slot or the resource's names say it is; "unknown" when nothing does (left for diagnostics, not guessed). */
    static String role(String key, String resource, String file) {
        String k = lower(key);
        if (k.endsWith("\\diffuse")) return "diffuse"; if (k.endsWith("\\lightmap")) return "lightmap"; if (k.endsWith("\\normalmap")) return "normal";
        for (String n : new String[]{lower(file), lower(resource)}) {
            if (n.contains("diffuse")) return "diffuse"; if (n.contains("lightmap")) return "lightmap"; if (n.contains("normal")) return "normal";
            if (n.contains("shadowramp") || n.contains("ramp")) return "ramp"; if (n.contains("mask")) return "mask";
        }
        return "unknown";
    }

    /** Converts nothing: analyzes the INI and returns the diagnostic document; log gets one texture-diag-ini line per rule. */
    static Map<String, Object> analyze(File src, File iniFile, int id, String name, StringBuilder log) throws Exception {
        ModelConverter.Ini ini = new ModelConverter.Ini(src, iniFile);
        File base = ini.file.getAbsoluteFile().getParentFile();
        List<String> notes = new ArrayList<>();
        // The part name 3DMigoto exports carry after "TextureOverride": the mod's common prefix is the character name.
        List<String> parts = new ArrayList<>();
        for (ModelConverter.Section s : ini.sections.values()) if (lower(s.name).startsWith("textureoverride") && s.name.length() > 15) parts.add(s.name.substring(15));
        String prefix = parts.size() < 2 ? "" : parts.get(0);
        for (String p : parts) { int n = 0; while (n < prefix.length() && n < p.length() && prefix.charAt(n) == p.charAt(n)) n++; prefix = prefix.substring(0, n); }
        while (!prefix.isEmpty() && Character.isUpperCase(prefix.charAt(prefix.length() - 1))) prefix = prefix.substring(0, prefix.length() - 1);   // DressA / DressB share "Dress": not part of the name
        Map<String, List<Long>> firsts = new TreeMap<>();
        for (ModelConverter.Section s : ini.sections.values()) if (lower(s.name).startsWith("textureoverride")) { String h = ""; Long f = null;
            for (String l : s.lines) { String[] a = kv(l); if (a == null) continue; if (a[0].equals("hash")) h = lower(a[1]); if (a[0].equals("match_first_index")) try { f = Long.parseLong(a[1].trim()); } catch (NumberFormatException e) { notes.add(s.name + ": match_first_index " + a[1] + " is not a number"); } }
            if (!h.isEmpty() && f != null) firsts.computeIfAbsent(h, k -> new ArrayList<>()).add(f); }
        for (List<Long> l : firsts.values()) Collections.sort(l);

        List<Object> rules = new ArrayList<>(); Set<String> usedResources = new LinkedHashSet<>(); Set<String> conditionVars = new TreeSet<>();
        for (ModelConverter.Section s : ini.sections.values()) {
            if (!lower(s.name).startsWith("textureoverride")) continue;
            Map<String, Object> r = new LinkedHashMap<>(); String part = s.name.length() > 15 ? s.name.substring(15) : s.name;
            String hash = "", first = null, priority = null; List<Object> textures = new ArrayList<>(), draws = new ArrayList<>(), sets = new ArrayList<>(), conds = new ArrayList<>(), external = new ArrayList<>(), runs = new ArrayList<>(), other = new ArrayList<>();
            for (String l : s.lines) { String[] a = kv(l); if (a == null) continue;
                if (a[0].equals("hash")) hash = lower(a[1]); else if (a[0].equals("match_first_index")) first = a[1].trim(); else if (a[0].equals("match_priority")) priority = a[1].trim(); }
            walk(ini, s, null, new ArrayList<>(), new HashSet<>(), 0, textures, draws, sets, conds, external, runs, other, notes);
            for (Object o : textures) { @SuppressWarnings("unchecked") Map<String, Object> t = (Map<String, Object>) o; if (t.get("resource") instanceof String) usedResources.add((String) t.get("resource")); }
            for (Object c : conds) { java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\$[a-z_][a-z0-9_]*").matcher((String) c); while (m.find()) conditionVars.add(m.group()); }
            String hint = part.startsWith(prefix) ? part.substring(prefix.length()) : part;
            int slotLetter = -1; if (hint.length() >= 2 && Character.isUpperCase(hint.charAt(hint.length() - 1)) && Character.isLowerCase(hint.charAt(hint.length() - 2))) { slotLetter = hint.charAt(hint.length() - 1) - 'A'; hint = hint.substring(0, hint.length() - 1); }
            long firstIndex = -1; if (first != null) try { firstIndex = Long.parseLong(first); } catch (NumberFormatException e) { firstIndex = -2; }
            String kind = textures.isEmpty() ? "control" : firstIndex >= 0 ? "segment" : firsts.containsKey(hash) ? "whole-ib" : "hash-unknown";
            String evidence = hash.isEmpty() ? "no hash" : firsts.containsKey(hash) ? "match_first_index is used with this hash in this INI (GIMI uses it on index-buffer overrides)" : "nothing in the INI tells an index-buffer, vertex-buffer or texture hash apart";
            boolean textureName = lower(part).matches(".*(diffuse|lightmap|normalmap|shadowramp)$") || textures.stream().anyMatch(t -> "this".equals(((Map<?, ?>) t).get("slot")));
            r.put("section", s.name); r.put("part", part); r.put("hint", lower(hint)); r.put("slotLetter", slotLetter); r.put("hash", hash);
            r.put("first", firstIndex); r.put("priority", priority == null ? 0 : parseInt(priority, 0)); r.put("kind", kind); r.put("hashEvidence", evidence); r.put("textureName", textureName);
            r.put("textures", textures); r.put("drawIndexed", draws); r.put("sets", sets); r.put("conditions", conds); r.put("runs", runs); r.put("external", external); r.put("otherCommands", other);
            rules.add(r);
            StringBuilder t = new StringBuilder();
            for (Object o : textures) { Map<?, ?> x = (Map<?, ?>) o; t.append(t.length() == 0 ? "" : ", ").append(x.get("slot")).append('=').append(x.get("resource")).append('(').append(x.get("role")).append(')'); if (!"".equals(x.get("condition"))) t.append(" if ").append(x.get("condition")); }
            log.append("texture-diag-ini ").append(s.name).append(": hash=").append(hash.isEmpty() ? "-" : hash).append(" match_first_index=").append(firstIndex >= 0 ? Long.toString(firstIndex) : firstIndex == -2 ? "invalid" : "-")
                .append(" kind=").append(kind).append(" textures=[").append(t).append("] drawindexed=").append(draws).append(external.isEmpty() ? "" : " external=" + external).append('\n');
        }
        // Resources the bindings name: file, existence and header (size, format, mips); textures are not decoded.
        List<Object> resources = new ArrayList<>();
        for (String rn : usedResources) { Map<String, Object> x = new LinkedHashMap<>(); x.put("name", rn); ModelConverter.Section s = ini.sections.get(rn);
            if (s == null) { x.put("error", "no such resource section in this INI"); resources.add(x); continue; }
            List<String> files = new ArrayList<>(); for (String l : s.lines) { String[] a = kv(l); if (a != null && a[0].equals("filename")) files.add(a[1].replace("\"", "")); }
            if (files.isEmpty()) { x.put("error", "the resource names no file"); resources.add(x); continue; }
            if (files.size() > 1) x.put("note", "several filename lines (conditional); the first is described: " + files);
            x.put("file", files.get(0).replace('\\', '/'));
            try { File f = resolve(base, files.get(0)); x.put("exists", f.isFile()); if (f.isFile()) { x.put("bytes", f.length()); header(f, x); } }
            catch (Exception e) { x.put("exists", false); x.put("error", e.getMessage()); }
            resources.add(x); }
        // Variables behind the conditions: initial value, the values keys cycle through, persist.
        List<Object> variables = new ArrayList<>();
        for (String v : conditionVars) { Map<String, Object> x = new LinkedHashMap<>(); x.put("name", v); x.put("initial", ini.vars.containsKey(v) ? ini.vars.get(v) : null);
            x.put("values", ini.choices.containsKey(v) ? new ArrayList<Object>(ini.choices.get(v)) : new ArrayList<>());
            boolean persist = false; ModelConverter.Section c = ini.sections.get("constants");
            if (c != null) for (String l : c.lines) if (lower(l).matches("(global\\s+)?persist\\s+.*") && lower(l).contains(v)) persist = true;
            x.put("persist", persist); List<Object> keys = new ArrayList<>();
            for (ModelConverter.Section s : ini.sections.values()) if (lower(s.name).startsWith("key")) for (String l : s.lines) { String[] a = kv(l); if (a != null && a[0].equals(v)) keys.add(s.name); }
            x.put("keys", keys); variables.add(x); }
        // Other INIs of the folder (a disabled scheme, a helper, a second variant) that override the same hashes.
        List<Object> others = new ArrayList<>();
        for (File f : ModConfig.allInis(src)) { if (f.getCanonicalFile().equals(ini.file.getCanonicalFile())) continue; Map<String, Object> x = new LinkedHashMap<>(); x.put("ini", ModConfig.relative(src, f));
            try { String text = lower(ModConfig.source(f)); Set<String> shared = new TreeSet<>(); for (Object o : rules) { String h = (String) ((Map<?, ?>) o).get("hash"); if (!h.isEmpty() && text.matches("(?s).*hash\\s*=\\s*" + h + "\\b.*")) shared.add(h); }
                x.put("sharedHashes", new ArrayList<Object>(shared)); }
            catch (Exception e) { x.put("error", e.getMessage()); }
            others.add(x); }
        for (String n : ini.compatibility) notes.add(n);
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("format", "gibox-texture-diag"); out.put("version", VERSION); out.put("name", name); out.put("ini", ModConfig.relative(src, ini.file));
        out.put("character", id); out.put("characterName", Characters.nameOf(id)); out.put("prefix", prefix);
        out.put("phase", "read-only diagnostics: the game side matches and logs, nothing is replaced");
        out.put("rules", rules); out.put("resources", resources); out.put("variables", variables); out.put("otherInis", others); out.put("notes", new ArrayList<Object>(notes));
        log.append("texture-diag-ini ").append(rules.size()).append(" 条 TextureOverride，").append(resources.size()).append(" 个贴图资源，").append(variables.size()).append(" 个条件变量").append(others.isEmpty() ? "" : "，另有 " + others.size() + " 个 INI").append('\n');
        return out;
    }
    private static int parseInt(String s, int fallback) { try { return Integer.parseInt(s.trim()); } catch (NumberFormatException e) { return fallback; } }

    /**
     * One section's commands in order, following this INI's own CommandLists (an external one, such as GIMI's
     * CommandList\GIMI\SetTextures, is recorded, not followed). cond: the if / else branches the line sits under.
     */
    private static void walk(ModelConverter.Ini ini, ModelConverter.Section s, String via, List<String> outer, Set<String> visited, int depth, List<Object> textures, List<Object> draws,
                             List<Object> sets, List<Object> conds, List<Object> external, List<Object> runs, List<Object> other, List<String> notes) {
        if (depth > 16 || !visited.add(lower(s.name))) { if (depth > 16) notes.add(s.name + ": CommandList nesting over 16 levels; the rest is not described"); return; }
        // One frame per open if: the branch outside it and the conditions of its earlier branches (an else if / else
        // holds only when every earlier branch did not).
        final class Frame { final List<String> parent; final List<String> earlier = new ArrayList<>(); Frame(List<String> p) { parent = p; } }
        Deque<Frame> stack = new ArrayDeque<>(); List<String> branch = new ArrayList<>();
        for (String l : s.lines) {
            String c = condition(l), e = elseCondition(l), low = lower(l).trim();
            if (c != null) { Frame f = new Frame(branch); f.earlier.add(c); stack.push(f); branch = new ArrayList<>(branch); branch.add(c); conds.add(c); continue; }
            if (e != null || low.equals("else")) { if (stack.isEmpty()) { notes.add(s.name + ": else without if"); continue; } Frame f = stack.peek();
                branch = new ArrayList<>(f.parent); for (String p : f.earlier) branch.add("!(" + p + ")"); if (e != null) { branch.add(e); f.earlier.add(e); conds.add(e); } continue; }
            if (low.equals("endif")) { if (stack.isEmpty()) notes.add(s.name + ": endif without if"); else branch = stack.pop().parent; continue; }
            String[] a = kv(l); List<String> all = new ArrayList<>(outer); all.addAll(branch); String when = String.join(" && ", all);
            if (a == null) { if (!low.isEmpty()) other.add(l.trim()); continue; }
            if (textureKey(a[0])) { Map<String, Object> t = new LinkedHashMap<>(); String rn = lower(a[1]).equals("null") ? null : resourceName(a[1]); String file = "";
                if (rn != null) { ModelConverter.Section r = ini.sections.get(rn); if (r != null) for (String rl : r.lines) { String[] b = kv(rl); if (b != null && b[0].equals("filename")) file = b[1]; } }
                t.put("slot", a[0].equals("this") ? "this" : a[0]); t.put("resource", rn); t.put("role", role(a[0], rn == null ? "" : rn, file)); t.put("condition", when); t.put("via", via == null ? "" : via);
                textures.add(t); continue; }
            if (a[0].equals("drawindexed")) { Map<String, Object> d = new LinkedHashMap<>(); d.put("args", a[1]); d.put("condition", when); d.put("via", via == null ? "" : via); draws.add(d); continue; }
            if (a[0].startsWith("$")) { sets.add(a[0] + " = " + a[1] + (when.isEmpty() ? "" : " if " + when)); continue; }
            if (a[0].equals("run")) { String target = a[1].trim(); runs.add(target + (when.isEmpty() ? "" : " if " + when)); ModelConverter.Section t = ini.sections.get(lower(target));
                if (t != null) walk(ini, t, t.name, all, visited, depth + 1, textures, draws, sets, conds, external, runs, other, notes); else if (!external.contains(target)) external.add(target); continue; }
            if (a[0].equals("hash") || a[0].equals("match_first_index") || a[0].equals("match_priority")) continue;
            other.add(a[0] + " = " + a[1] + (when.isEmpty() ? "" : " if " + when));
        }
    }
    /** A resource path as 3DMigoto takes it: relative to the INI's folder; missing components matched without case. */
    private static File resolve(File base, String path) throws Exception {
        String r = base.getCanonicalPath() + File.separator; File cursor = base;
        for (String part : path.replace('\\', '/').split("/")) { if (part.isEmpty() || part.equals(".")) continue;
            File exact = new File(cursor, part); if (exact.exists() || part.equals("..")) { cursor = exact; continue; }
            File[] hits = cursor.listFiles(x -> x.getName().equalsIgnoreCase(part)); if (hits != null && hits.length > 1) throw new IllegalArgumentException("资源大小写有歧义：" + path);
            cursor = hits != null && hits.length == 1 ? hits[0] : exact; }
        File f = cursor.getCanonicalFile(); if (!f.getPath().startsWith(r)) throw new IllegalArgumentException("资源超出 MOD 目录：" + path); return f;
    }
    /** DDS (DX9 FourCC or DX10 DXGI format) and PNG headers: size, format, mip count. Nothing else is read. */
    static void header(File f, Map<String, Object> out) {
        byte[] h = new byte[148]; int n = 0;
        try (InputStream in = new FileInputStream(f)) { while (n < h.length) { int k = in.read(h, n, h.length - n); if (k < 0) break; n += k; } }
        catch (Exception e) { out.put("error", "unreadable: " + e.getMessage()); return; }
        if (n >= 128 && h[0] == 'D' && h[1] == 'D' && h[2] == 'S' && h[3] == ' ') {
            out.put("kind", "dds"); out.put("height", u32(h, 12)); out.put("width", u32(h, 16)); out.put("mips", Math.max(1L, u32(h, 28)));
            String four = new String(h, 84, 4, StandardCharsets.ISO_8859_1);
            if ((u32(h, 80) & 4) != 0) { if (four.equals("DX10")) out.put("format", n >= 132 ? "DXGI " + u32(h, 128) + dxgi((int) u32(h, 128)) : "DX10 (truncated header)"); else out.put("format", four.trim()); }
            else out.put("format", "uncompressed " + u32(h, 88) + " bpp");
            return; }
        if (n >= 24 && (h[0] & 0xff) == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') { out.put("kind", "png"); out.put("width", be32(h, 16)); out.put("height", be32(h, 20)); out.put("format", "PNG"); out.put("mips", 1); return; }
        out.put("kind", "other"); out.put("error", "neither a DDS nor a PNG header");
    }
    private static long u32(byte[] b, int o) { return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24; }
    private static long be32(byte[] b, int o) { return (b[o] & 0xffL) << 24 | (b[o + 1] & 0xffL) << 16 | (b[o + 2] & 0xffL) << 8 | (b[o + 3] & 0xffL); }
    private static String dxgi(int f) { switch (f) { case 28: return " (R8G8B8A8_UNORM)"; case 29: return " (R8G8B8A8_UNORM_SRGB)"; case 71: return " (BC1_UNORM)"; case 72: return " (BC1_UNORM_SRGB)";
        case 77: return " (BC3_UNORM)"; case 78: return " (BC3_UNORM_SRGB)"; case 80: return " (BC4_UNORM)"; case 83: return " (BC5_UNORM)"; case 98: return " (BC7_UNORM)"; case 99: return " (BC7_UNORM_SRGB)"; default: return ""; } }

    /** Writes the document atomically (the game reads either the old or the new file). */
    static File write(File dir, String name, Map<String, Object> doc) throws Exception {
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IllegalStateException("无法创建：" + dir);
        File out = file(dir, name), tmp = new File(dir, "." + name + ".json.tmp");
        Files.write(tmp.toPath(), Json.write(doc).getBytes(StandardCharsets.UTF_8));
        Files.move(tmp.toPath(), out.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE); return out;
    }
    static void remove(File dir, String name) { if (dir == null) return; File f = file(dir, name); if (f.isFile() && !f.delete()) f.deleteOnExit(); }
}
