package dev.modelswap.only;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * JSON for GIBox's own files (model.json, learned.json, skeleton.json), as strict as the native reader (json.hpp):
 * objects are LinkedHashMap, arrays ArrayList, numbers Double; duplicate keys, trailing input, control characters,
 * non-finite numbers and nesting deeper than 32 are rejected. Writing keeps insertion order; small objects and arrays of
 * plain values stay on one line, so model.json stays readable.
 */
public final class Json {
    private Json() {}

    public static Object parse(String s) { P p = new P(s); Object v = p.value(0); p.ws(); check(p.i == s.length(), "JSON：多余的内容"); return v; }
    public static Object read(File f, long max) throws Exception {
        check(f.isFile(), "找不到：" + f.getName()); check(f.length() <= max, "文件过大：" + f.getName());
        String s = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); if (s.startsWith("﻿")) s = s.substring(1); return parse(s);
    }
    static void check(boolean ok, String message) { if (!ok) throw new IllegalArgumentException(message); }

    private static final class P {
        final String s; int i;
        P(String s) { this.s = s; }
        void ws() { while (i < s.length() && (s.charAt(i) == ' ' || s.charAt(i) == '\t' || s.charAt(i) == '\n' || s.charAt(i) == '\r')) i++; }
        char next() { check(i < s.length(), "JSON：意外结束"); return s.charAt(i++); }
        Object value(int depth) {
            check(depth < 32, "JSON：嵌套过深"); ws(); check(i < s.length(), "JSON：意外结束"); char c = s.charAt(i);
            if (c == '"') return string();
            if (c == '{') {
                i++; Map<String, Object> o = new LinkedHashMap<>(); ws(); if (i < s.length() && s.charAt(i) == '}') { i++; return o; }
                while (true) { ws(); String k = string(); ws(); check(next() == ':', "JSON：缺少冒号"); check(!o.containsKey(k), "JSON：重复的键 " + k); o.put(k, value(depth + 1)); ws(); char sep = next(); if (sep == '}') return o; check(sep == ',', "JSON：缺少逗号"); }
            }
            if (c == '[') {
                i++; List<Object> a = new ArrayList<>(); ws(); if (i < s.length() && s.charAt(i) == ']') { i++; return a; }
                while (true) { a.add(value(depth + 1)); ws(); char sep = next(); if (sep == ']') return a; check(sep == ',', "JSON：缺少逗号"); }
            }
            if (s.startsWith("true", i)) { i += 4; return Boolean.TRUE; }
            if (s.startsWith("false", i)) { i += 5; return Boolean.FALSE; }
            if (s.startsWith("null", i)) { i += 4; return null; }
            int start = i; if (c == '-') i++;
            check(i < s.length() && Character.isDigit(s.charAt(i)), "JSON：无效的值");
            if (s.charAt(i) == '0') i++; else while (i < s.length() && Character.isDigit(s.charAt(i))) i++;
            if (i < s.length() && s.charAt(i) == '.') { i++; int f = i; while (i < s.length() && Character.isDigit(s.charAt(i))) i++; check(i > f, "JSON：无效的小数"); }
            if (i < s.length() && (s.charAt(i) == 'e' || s.charAt(i) == 'E')) { i++; if (i < s.length() && (s.charAt(i) == '+' || s.charAt(i) == '-')) i++; int f = i; while (i < s.length() && Character.isDigit(s.charAt(i))) i++; check(i > f, "JSON：无效的指数"); }
            double d = Double.parseDouble(s.substring(start, i)); check(!Double.isInfinite(d) && !Double.isNaN(d), "JSON：数值越界"); return d;
        }
        String string() {
            check(next() == '"', "JSON：需要字符串"); StringBuilder o = new StringBuilder();
            while (true) {
                char c = next(); if (c == '"') return o.toString(); check(c >= 0x20, "JSON：字符串里有控制字符");
                if (c != '\\') { o.append(c); continue; }
                char e = next();
                switch (e) {
                    case '"': case '\\': case '/': o.append(e); break;
                    case 'b': o.append('\b'); break; case 'f': o.append('\f'); break; case 'n': o.append('\n'); break; case 'r': o.append('\r'); break; case 't': o.append('\t'); break;
                    case 'u': check(i + 4 <= s.length(), "JSON：意外结束"); try { o.append((char) Integer.parseInt(s.substring(i, i + 4), 16)); } catch (NumberFormatException x) { throw new IllegalArgumentException("JSON：无效的 \\u"); } i += 4; break;
                    default: throw new IllegalArgumentException("JSON：无效的转义");
                }
            }
        }
    }

    // ---------------------------------------------------------------- field access (each failure names the field)
    @SuppressWarnings("unchecked")
    public static Map<String, Object> obj(Object v, String what) { check(v instanceof Map, what + " 不是对象"); return (Map<String, Object>) v; }
    @SuppressWarnings("unchecked")
    public static List<Object> arr(Object v, String what, int min, int max) { check(v instanceof List, what + " 不是列表"); List<Object> a = (List<Object>) v; check(a.size() >= min && a.size() <= max, what + " 数量越界"); return a; }
    public static String str(Object v, String what, int maxLength) { check(v instanceof String && !((String) v).isEmpty() && ((String) v).length() <= maxLength, what + " 无效"); return (String) v; }
    public static long integer(Object v, String what, long min, long max) {
        check(v instanceof Double, what + " 不是数字"); double d = (Double) v; check(d == Math.floor(d) && Math.abs(d) <= 9007199254740991.0, what + " 不是整数");
        check(d >= min && d <= max, what + " 越界"); return (long) d;
    }
    public static boolean bool(Object v, String what, boolean fallback) { if (v == null) return fallback; check(v instanceof Boolean, what + " 不是布尔值"); return (Boolean) v; }
    public static float finite(Object v, String what) { check(v instanceof Double, what + " 不是数字"); float f = (float) (double) (Double) v; check(!Float.isInfinite(f) && !Float.isNaN(f), what + " 越界"); return f; }

    // ---------------------------------------------------------------- writing
    public static String write(Object v) { StringBuilder s = new StringBuilder(); write(s, v, 0); return s.append('\n').toString(); }
    private static boolean plain(Object v) { return !(v instanceof Map) && !(v instanceof List); }
    private static boolean small(Object v) {
        if (plain(v)) return true;
        if (v instanceof List) { List<?> a = (List<?>) v; for (Object x : a) if (!plain(x)) return false; return true; }
        Map<?, ?> m = (Map<?, ?>) v; if (m.size() > 6) return false; for (Object x : m.values()) if (!plain(x) && !(x instanceof List && ((List<?>) x).size() <= 2 && small(x))) return false; return true;
    }
    private static void indent(StringBuilder s, int n) { s.append('\n'); for (int i = 0; i < n; i++) s.append("  "); }
    private static void write(StringBuilder s, Object v, int depth) {
        if (v == null) s.append("null");
        else if (v instanceof String) quote(s, (String) v);
        else if (v instanceof Boolean) s.append(v);
        else if (v instanceof Integer || v instanceof Long) s.append(v);
        else if (v instanceof Float) { float f = (Float) v; check(!Float.isNaN(f) && !Float.isInfinite(f), "非有限数值"); if (f == Math.rint(f) && Math.abs(f) < 1e15f) s.append((long) f); else s.append(Float.toString(f)); }
        else if (v instanceof Double) { double d = (Double) v; check(!Double.isNaN(d) && !Double.isInfinite(d), "非有限数值"); if (d == Math.rint(d) && Math.abs(d) < 1e15) s.append((long) d); else s.append(Double.toString(d)); }
        else if (v instanceof Map) {
            Map<?, ?> m = (Map<?, ?>) v; boolean inline = small(v); s.append('{'); boolean first = true;
            for (Map.Entry<?, ?> e : m.entrySet()) { if (!first) s.append(','); first = false; if (!inline) indent(s, depth + 1); quote(s, String.valueOf(e.getKey())); s.append(inline ? ":" : ": "); write(s, e.getValue(), depth + 1); }
            if (!inline && !m.isEmpty()) indent(s, depth); s.append('}');
        } else if (v instanceof List) {
            List<?> a = (List<?>) v; boolean inline = small(v); s.append('['); boolean first = true;
            for (Object x : a) { if (!first) s.append(','); first = false; if (!inline) indent(s, depth + 1); write(s, x, depth + 1); }
            if (!inline && !a.isEmpty()) indent(s, depth); s.append(']');
        } else throw new IllegalArgumentException("无法写成 JSON：" + v.getClass().getName());
    }
    private static void quote(StringBuilder s, String t) {
        s.append('"');
        for (int i = 0; i < t.length(); i++) { char c = t.charAt(i); if (c == '"' || c == '\\') s.append('\\').append(c); else if (c < 0x20) s.append(String.format("\\u%04x", (int) c)); else s.append(c); }
        s.append('"');
    }
    /** An ordered object from key/value pairs: Json.of("a", 1, "b", "x"). */
    public static Map<String, Object> of(Object... kv) { Map<String, Object> m = new LinkedHashMap<>(); for (int i = 0; i + 1 < kv.length; i += 2) m.put((String) kv[i], kv[i + 1]); return m; }
}
