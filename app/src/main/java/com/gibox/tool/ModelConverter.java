package com.gibox.tool;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

/**
 * INI/vertex-buffer converter. Output: one GIBox model folder
 * (format versions 2–4, read by model.hpp): model.json (manifest) + model.bin (every array on a 16-byte boundary, CRC-32 in
 * the manifest) + conversion.log. The runtime adds learned.json and bonemap-<part>.tsv.
 */
public final class ModelConverter {
    public interface TextureDecoder { byte[] decode(byte[] image) throws Exception; }
    private static void check(boolean ok,String message) { if(!ok) throw new IllegalArgumentException(message); }
    private static String lower(String s) { return s.toLowerCase(Locale.ROOT); }
    private static String clean(String s) { int p=s.indexOf(';'); return (p<0?s:s.substring(0,p)).trim(); }
    private static byte[] read(File f) throws Exception { check(f.length()<=256L*1024*1024,"文件过大："+f); return Files.readAllBytes(f.toPath()); }
    private static ByteBuffer buf(byte[] a) { return ByteBuffer.wrap(a).order(ByteOrder.LITTLE_ENDIAN); }
    private static byte[] text(String s) { return s.getBytes(StandardCharsets.UTF_8); }
    private static void write(File d,String n,byte[] b)throws Exception { Files.write(new File(d,n).toPath(),b); }
    private static String safe(String s) { check(s.matches("[A-Za-z0-9_-]+"),"不支持的部件文件名："+s);return s; }
    private static File inside(File root,String path)throws Exception {
        path=path.replace('\\','/').replace("\"","");File f=new File(root,path).getCanonicalFile();String r=root.getCanonicalPath()+File.separator;
        check(f.getPath().startsWith(r),"资源超出 MOD 目录："+path);
        // PC archives commonly differ only in casing (TexCoord / Texcoord, Resources / resources). Resolve each
        // missing component uniquely; retain the canonical containment check, including after following symlinks.
        if(!f.exists()){File cursor=root;String relative=f.getPath().substring(r.length());for(String part:relative.split("/")){
            File exact=new File(cursor,part);if(exact.exists()){cursor=exact;continue;}File[] hits=cursor.listFiles(x->x.getName().equalsIgnoreCase(part));
            check(hits==null||hits.length<=1,"资源大小写有歧义："+path);cursor=hits!=null&&hits.length==1?hits[0]:exact;
        }f=cursor.getCanonicalFile();check(f.getPath().startsWith(r),"资源超出 MOD 目录："+path);}
        return f;
    }

    static final class Section { final String name;final List<String> lines=new ArrayList<>(); Section(String n){name=n;} }
    static final class Ini {
        final LinkedHashMap<String,Section> sections=new LinkedHashMap<>();
        final LinkedHashMap<String,Integer> vars=new LinkedHashMap<>();
        final LinkedHashMap<String,Double> numbers=new LinkedHashMap<>();
        final LinkedHashMap<String,TreeSet<Integer>> choices=new LinkedHashMap<>();
        final LinkedHashMap<String,TreeSet<Integer>> excludedChoices=new LinkedHashMap<>();
        final LinkedHashSet<String> compatibility=new LinkedHashSet<>();
        final File file;
        Ini(File root)throws Exception {this(root,null);}
        /** chosen: the INI scheme picked for a package with several model INIs; null picks the only one in root. */
        Ini(File root,File chosen)throws Exception {
            file=chosen!=null?chosen:ModConfig.mainIni(root);List<Section> blocks=new ArrayList<>();
            Section current=null;for(String raw:ModConfig.source(file).split("\\r?\\n")) {
                String l=clean(raw);if(l.isEmpty())continue;
                if(l.startsWith("[")&&l.endsWith("]")){String n=l.substring(1,l.length()-1).trim();current=new Section(n);blocks.add(current);}
                else if(current!=null)current.lines.add(l);
            }
            for(Section s:blocks){String key=lower(s.name);Section previous=sections.get(key);
                if(previous==null)sections.put(key,s);
                else if(key.equals("present")){previous.lines.addAll(s.lines);compatibility.add("已合并重复 Present 段");}
                else if(key.startsWith("key")&&previous.lines.equals(s.lines))compatibility.add("已忽略完全相同的重复按键段："+s.name);
                else check(false,"重复 INI 段："+s.name);
            }
            Pattern decl=Pattern.compile("(?i)^(?:global\\s+)?(?:persist\\s+)?(\\$[a-z_][a-z0-9_]*)(?:\\s*=\\s*([+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?))?\\s*$");
            Pattern cmp=Pattern.compile("(\\$[A-Za-z_][A-Za-z0-9_]*)\\s*(?:===|!==|==|!=|<=|>=|<|>)\\s*(-?\\d+)");
            for(Section section:sections.values())for(String l:section.lines){Matcher m=decl.matcher(l);
                if(m.matches()&&(lower(section.name).equals("constants")||lower(section.name).equals("persist"))){String v=lower(m.group(1));double n=m.group(2)==null?0:Double.parseDouble(m.group(2));check(Double.isFinite(n),"变量初始值无效："+v);numbers.put(v,n);
                    if(n==Math.rint(n)&&n>=Integer.MIN_VALUE&&n<=Integer.MAX_VALUE){vars.put(v,(int)n);choices.computeIfAbsent(v,k->new TreeSet<>()).add((int)n);}}
                // Present counters and fractional periods are not model variants.
                if(!lower(section.name).equals("present")){Matcher c=cmp.matcher(l);while(c.find())choices.computeIfAbsent(lower(c.group(1)),k->new TreeSet<>()).add(Integer.parseInt(c.group(2)));}
                if(lower(section.name).startsWith("key")){int eq=l.indexOf('=');if(eq>0&&l.substring(0,eq).trim().startsWith("$")){String v=lower(l.substring(0,eq).trim());for(String v0:l.substring(eq+1).split(","))if(v0.trim().matches("-?\\d+"))choices.computeIfAbsent(v,k->new TreeSet<>()).add(Integer.parseInt(v0.trim()));}}
            }
            for(String v:choices.keySet()){check(!numbers.containsKey(v)||vars.containsKey(v),"模型款式使用非整数变量："+v);check(vars.containsKey(v),"条件变量缺少初始值："+v);}
            // A variable a key sets can only take the values the keys list (cycle, toggle, hold or activate), the value it
            // starts with, and literal values other sections assign to it. Constants that only appear in comparisons may
            // be old, disabled states or bounds; they must not add variants (Ayaka's key cycles $dress = 0,1,2, while old
            // ifs still test 3..7). A variable no key sets keeps the values its comparisons name.
            Map<String,TreeSet<Integer>> cycles=new LinkedHashMap<>();
            for(Section s:sections.values())if(lower(s.name).startsWith("key"))
                for(String l:s.lines){String[] a=kv(l);if(a==null||!vars.containsKey(a[0])||!a[1].matches("-?\\d+(?:\\s*,\\s*-?\\d+)*"))continue;
                    TreeSet<Integer> values=cycles.computeIfAbsent(a[0],k->new TreeSet<>());for(String n:a[1].split(","))values.add(Integer.parseInt(n.trim()));}
            for(Section s:sections.values()){String n=lower(s.name);if(n.startsWith("key")||n.equals("constants"))continue;
                for(String l:s.lines){String[] a=kv(l);if(a!=null&&cycles.containsKey(a[0])&&a[1].matches("-?\\d+"))cycles.get(a[0]).add(Integer.parseInt(a[1]));}}
            for(Map.Entry<String,TreeSet<Integer>> e:cycles.entrySet()){
                e.getValue().add(vars.get(e.getKey())); // The declared initial state is reachable even outside the cycle.
                TreeSet<Integer> excluded=new TreeSet<>(choices.get(e.getKey()));excluded.removeAll(e.getValue());
                if(!excluded.isEmpty())excludedChoices.put(e.getKey(),excluded);
                choices.put(e.getKey(),e.getValue());
            }
        }
        Section section(String n) { Section s=sections.get(lower(n));check(s!=null,"缺少段："+n);return s; }
        final LinkedHashSet<String> external=new LinkedHashSet<>();
        /** External calls cannot bind this mod's buffers. Known texture-reference conventions are handled below. */
        Section run(String n) { Section s=sections.get(lower(n.trim()));if(s==null){check(lower(n.trim()).startsWith("commandlist"),"缺少段："+n);external.add(n.trim());}return s; }
        List<String> active(Section s,Map<String,Integer> v){
            class Branch {boolean parent,taken,enabled,otherwise;Branch(boolean p,boolean e){parent=p;enabled=p&&e;taken=e;} }
            Deque<Branch> stack=new ArrayDeque<>();List<String> out=new ArrayList<>();boolean on=true;
            for(String l:s.lines){String a=lower(l);if(a.startsWith("if ")){Branch b=new Branch(on,new Expr(l.substring(3),v).eval()!=0);stack.push(b);on=b.enabled;}
                else if(a.startsWith("else if ")||a.startsWith("elseif ")){check(!stack.isEmpty(),"elseif 缺少 if");Branch b=stack.peek();check(!b.otherwise,"elseif 位于 else 后");boolean yes=new Expr(l.substring(a.startsWith("else if ")?8:7),v).eval()!=0;b.enabled=b.parent&&!b.taken&&yes;b.taken|=yes;on=b.enabled;}
                else if(a.equals("else")){check(!stack.isEmpty(),"else 缺少 if");Branch b=stack.peek();check(!b.otherwise,"重复 else");b.otherwise=true;b.enabled=b.parent&&!b.taken;b.taken=true;on=b.enabled;}
                else if(a.equals("endif")){check(!stack.isEmpty(),"endif 缺少 if");stack.pop();on=stack.isEmpty()||stack.peek().enabled;}
                else if(on)out.add(l);
            }check(stack.isEmpty(),"if 未结束："+s.name);return out;
        }
        List<Map<String,Integer>> variants(boolean all){List<Map<String,Integer>> o=new ArrayList<>();o.add(new LinkedHashMap<>(vars));if(!all)return o;
            for(String k:vars.keySet()){List<Map<String,Integer>> next=new ArrayList<>();TreeSet<Integer> values=choices.get(k);if(values==null)continue;for(Map<String,Integer> a:o)for(int n:values){Map<String,Integer>b=new LinkedHashMap<>(a);b.put(k,n);next.add(b);check(next.size()<=32,"款式组合超过 32；请关闭枚举或简化变量");}o=next;}return o;
        }
    }
    /** Integer arithmetic and conditions; unknown tokens fail instead of guessing. */
    static final class Expr {
        final String s;final Map<String,Integer> vars;int p;
        Expr(String s,Map<String,Integer> v){this.s=lower(s);vars=v;}
        void ws(){while(p<s.length()&&Character.isWhitespace(s.charAt(p)))p++;}
        boolean take(String a){ws();if(s.startsWith(a,p)){p+=a.length();return true;}return false;}
        long eval(){long v=or();ws();check(p==s.length(),"不支持的表达式："+s);return v;}
        long or(){long a=and();while(take("||")){long b=and();a=(a!=0||b!=0)?1:0;}return a;}
        long and(){long a=compare();while(take("&&")){long b=compare();a=(a!=0&&b!=0)?1:0;}return a;}
        long compare(){long a=add();while(true){if(take("==="))a=a==add()?1:0;else if(take("!=="))a=a!=add()?1:0;else if(take("=="))a=a==add()?1:0;else if(take("!="))a=a!=add()?1:0;else if(take("<="))a=a<=add()?1:0;else if(take(">="))a=a>=add()?1:0;else if(take("<"))a=a<add()?1:0;else if(take(">"))a=a>add()?1:0;else return a;}}
        long add(){long a=mul();while(true){if(take("+"))a+=mul();else if(take("-"))a-=mul();else return a;}}
        long mul(){long a=atom();while(true){if(take("**")){long b=atom();check(b>=0&&b<63,"幂次越界");long r=1;for(long k=0;k<b;k++)r*=a;a=r;}else if(take("*"))a*=atom();else if(take("//")){long b=atom();check(b!=0,"除零");a=Math.floorDiv(a,b);}else if(take("/")){long b=atom();check(b!=0,"除零");a/=b;}else if(take("%")){long b=atom();check(b!=0,"除零");a%=b;}else return a;}}
        long atom(){if(take("!"))return atom()==0?1:0;if(take("-"))return -atom();if(take("+"))return atom();if(take("(")){long v=or();check(take(")"),"缺少右括号");return v;}ws();int start=p;
            if(p<s.length()&&s.charAt(p)=='$'){p++;while(p<s.length()&&(Character.isLetterOrDigit(s.charAt(p))||s.charAt(p)=='_'))p++;String k=s.substring(start,p);check(vars.containsKey(k),"未知变量："+k);return vars.get(k);}
            while(p<s.length()&&Character.isDigit(s.charAt(p)))p++;check(p>start,"不支持的表达式："+s);return Long.parseLong(s.substring(start,p));}
    }
    static final class Resource {File file;int stride;String format;Resource(File f,int s,String t){file=f;stride=s;format=t;} }
    static final class Draw {String part,hash="";Map<String,String> bindings;int count,start;Draw(String p,Map<String,String>b,int c,int s){part=p;bindings=new LinkedHashMap<>(b);count=c;start=s;} }
    private static String[] kv(String l){int p=l.indexOf('=');return p<0?null:new String[]{lower(l.substring(0,p).trim()),l.substring(p+1).trim()};}
    private static boolean binding(String k){return k.equals("ib")||k.matches("vb[0-9]+")||k.matches("ps-t[0-9]+");}
    private static boolean unbound(String name){return name==null||name.equalsIgnoreCase("null");}
    private static boolean nnFix(String name){return lower(name.trim()).equals("commandlist\\global\\orfix\\nnfix");}
    private static boolean texFxComponent(String name){return lower(name.trim()).matches("commandlist\\\\texfx\\\\(?:component|c)(?:\\.[01])?");}
    private static boolean texFxArgument(String key){return key.matches("\\$\\\\texfx\\\\_[123]");}
    private static void componentDraw(Map<String,String> bindings,List<Draw> draws,String part){
        String prefix="$\\texfx\\"; // Namespace-qualified INI arguments, not local variant names.
        check(bindings.containsKey(prefix+"_1"),"TexFx Component 缺少绘制数量："+part);
        int count=Integer.parseInt(bindings.get(prefix+"_1")),start=Integer.parseInt(bindings.getOrDefault(prefix+"_2","0"));
        int base=Integer.parseInt(bindings.getOrDefault(prefix+"_3","0"));
        check(count>=0&&start>=0&&base==0,"TexFx Component 绘制参数无效（仅支持 baseVertex=0）："+part);
        if(count>0){Draw d=new Draw(part,bindings,count,start);d.bindings.put("compat:texfx-component","true");draws.add(d);}
        for(int i=1;i<=3;i++)bindings.remove(prefix+"_"+i);
    }
    /** ORFix's ReferenceNoNormal captures ps-t0 as Diffuse and ps-t1 as Lightmap, regardless of filenames.
     * Keep the resources at the call site: later bindings and other draws must not change this snapshot. */
    private static void referenceNoNormal(Map<String,String> bindings){
        bindings.put("texture-role:layout","nnfix");
        bindings.put("texture-role:diffuse",bindings.getOrDefault("ps-t0","null"));
        bindings.put("texture-role:lightmap",bindings.getOrDefault("ps-t1","null"));
    }
    private static String streamKey(int kind){return "vertex-stream:"+kind;}
    private static String streamName(int kind){return new String[]{"Position","Blend","Texcoord"}[kind];}
    private static int streamKind(Resource r){
        String n=lower(r.file.getName());
        if(n.contains("texcoord"))return 2;
        if(n.contains("blend"))return 1;
        if(n.contains("position"))return 0;
        if(r.stride>=40)return 0;
        if(r.stride==32)return 1;
        if(r.stride>=12&&r.stride<=36&&(r.stride-4)%8==0)return 2;
        return -1;
    }
    private static void assign(Map<String,Resource> rs,Map<String,String> bindings,String key,String value){
        if(key.matches("vb[0-9]+")){
            String previous=bindings.get(key);
            if(unbound(value)){
                if(!unbound(previous)){int kind=streamKind(resource(rs,previous));if(kind>=0&&previous.equals(bindings.get(streamKey(kind))))bindings.remove(streamKey(kind));}
            }else{
                int kind=streamKind(resource(rs,value));
                // One physical slot can carry different logical data in separate stages.
                if(kind>=0)bindings.put(streamKey(kind),value);
            }
        }
        bindings.put(key,value);
    }
    private static void commands(Ini ini,Section s,Map<String,Integer> vars,Map<String,Resource> rs,Map<String,String> bindings,List<Draw> draws,String part,int depth){
        check(depth<16,"CommandList 递归过深");for(String l:ini.active(s,vars)){String[] a=kv(l);if(a==null)continue;String k=a[0],v=a[1];if(binding(k))assign(rs,bindings,k,v);
            else if(k.equals("handling"))bindings.put(k,lower(v));
            else if(texFxArgument(k)){long n=new Expr(v,vars).eval();check(n>=0&&n<=Integer.MAX_VALUE,"TexFx 绘制参数越界");bindings.put(k,Long.toString(n));}
            else if(k.equals("run")){Section target=ini.run(v);if(target!=null)commands(ini,target,vars,rs,bindings,draws,part,depth+1);else if(nnFix(v))referenceNoNormal(bindings);
                else if(texFxComponent(v)){componentDraw(bindings,draws,part);ini.compatibility.add("已还原 TexFx Component 的条件绘制区间；透明、发光和自定义描边使用手机原材质，未复刻 PC 着色器");}}
            else if(k.equals("drawindexed")){String[] f=v.split(",");check(f.length<=3,"drawindexed 参数");int count=f[0].trim().equalsIgnoreCase("auto")?-1:Integer.parseInt(f[0].trim());int start=f.length>1?Integer.parseInt(f[1].trim()):0;check(f.length<3||Integer.parseInt(f[2].trim())==0,"仅支持 baseVertex=0");check(count>=-1,"drawindexed 数量");check(start>=0,"drawindexed 起始");if(count!=0)draws.add(new Draw(part,bindings,count,start));}
        }
    }
    private static Map<String,Resource> resources(Ini ini,File root,Map<String,Integer> vars)throws Exception {
        Map<String,Resource> out=new HashMap<>();for(Section s:ini.sections.values())if(lower(s.name).startsWith("resource")){String file=null,format="";int stride=0;for(String l:ini.active(s,vars)){String[] a=kv(l);if(a==null)continue;if(a[0].equals("filename"))file=a[1];if(a[0].equals("stride"))stride=Integer.parseInt(a[1]);if(a[0].equals("format"))format=a[1];}if(file!=null)out.put(lower(s.name),new Resource(inside(root,file),stride,format));}return out;
    }
    private static Resource resource(Map<String,Resource> resources,String name){if(name==null)return null;String n=name.replaceFirst("(?i)^ref\\s+","").trim();Resource r=resources.get(lower(n));check(r!=null,"缺少资源定义："+name);return r;}
    private static String identity(Resource r){return r.file.getPath()+"|"+r.stride;}
    private static Resource localStream(Map<String,Resource> rs,Map<String,String> bindings,int kind){
        LinkedHashMap<String,Resource> hits=new LinkedHashMap<>();
        for(Map.Entry<String,String> a:bindings.entrySet())if((a.getKey().matches("vb[0-9]+")||a.getKey().equals(streamKey(kind)))&&!unbound(a.getValue())){
            Resource r=resource(rs,a.getValue());if(streamKind(r)==kind)hits.put(identity(r),r);
        }
        check(hits.size()<=1,"局部 "+streamName(kind)+" 顶点流有歧义："+hits.keySet());
        return hits.isEmpty()?null:hits.values().iterator().next();
    }
    private static Resource stream(Map<String,Resource> rs,Map<String,String> bindings,int kind){Resource r=localStream(rs,bindings,kind);check(r!=null,"缺少 "+streamName(kind)+" 顶点流");return r;}
    private static boolean declaresDraw(Ini ini,Section s,Set<String> visited){
        if(!visited.add(lower(s.name)))return false;
        for(String l:s.lines){String[] a=kv(l);if(a==null)continue;if(a[0].equals("drawindexed"))return true;if(a[0].equals("run")){Section target=ini.run(a[1]);if(target!=null&&declaresDraw(ini,target,visited))return true;if(target==null&&texFxComponent(a[1]))return true;}}
        return false;
    }
    static final class Evaluated {
        final String name,scope,hash;final Map<String,String> bindings=new LinkedHashMap<>();final List<Draw> draws=new ArrayList<>();final boolean declaresDraw;
        Evaluated(Ini ini,Section s,Map<String,Integer> vars,Map<String,Resource> rs){
            name=s.name.substring(15);scope=lower(name).replaceFirst("(?:position|blend|texcoords?|vertexlimitraise)(?:[_-]?[0-9]+)?$","");
            String h="";for(String l:ini.active(s,vars)){String[] a=kv(l);if(a!=null&&a[0].equals("hash"))h=lower(a[1]);}hash=h;
            declaresDraw=declaresDraw(ini,s,new HashSet<>());commands(ini,s,vars,rs,bindings,draws,name,0);
        }
    }
    private static int scopeScore(String scope,Draw d,Map<String,Resource> rs){
        if(scope.isEmpty())return 0;
        if(lower(d.part).startsWith(scope))return scope.length();
        String ib=d.bindings.get("ib");if(!unbound(ib)){
            String r=lower(ib.replaceFirst("(?i)^ref\\s+","").replaceFirst("(?i)^resource", ""));
            if(r.startsWith(scope)||lower(resource(rs,ib).file.getName()).startsWith(scope))return scope.length();
        }
        return -1;
    }
    private static void resolveStreams(Draw d,List<Evaluated> providers,Map<String,Resource> rs){
        Set<String> scopes=new HashSet<>();for(Evaluated p:providers)if(!p.scope.isEmpty())scopes.add(p.scope);
        for(int kind=0;kind<3;kind++){
            // Explicit draw/CommandList bindings take precedence over defaults.
            if(localStream(rs,d.bindings,kind)!=null)continue;
            int best=-1;List<Evaluated> candidates=new ArrayList<>();
            for(Evaluated p:providers){if(!p.bindings.containsKey(streamKey(kind)))continue;int score=scopeScore(p.scope,d,rs);if(score<0)continue;if(score>best){candidates.clear();best=score;}if(score==best)candidates.add(p);}
            if(candidates.isEmpty()&&scopes.size()<=1)for(Evaluated p:providers)if(p.bindings.containsKey(streamKey(kind)))candidates.add(p);
            LinkedHashMap<String,String> unique=new LinkedHashMap<>();List<String> details=new ArrayList<>();
            for(Evaluated p:candidates){String value=p.bindings.get(streamKey(kind));Resource r=resource(rs,value);unique.put(identity(r),value);details.add("TextureOverride"+p.name+" -> "+value);}
            check(unique.size()==1,"部件 "+d.part+" 的 "+streamName(kind)+" 顶点流"+(unique.isEmpty()?"缺失":"有歧义")+"："+details);
            d.bindings.put(streamKey(kind),unique.values().iterator().next());
        }
    }
    private static List<Draw> drawCalls(Ini ini,Map<String,Integer> vars,Map<String,Resource> rs,Map<String,String> mapping){
        List<Evaluated> sections=new ArrayList<>(),providers=new ArrayList<>();List<Draw> draws=new ArrayList<>();Set<String> autoHashes=new HashSet<>(),usedHashes=new HashSet<>();
        for(Section s:ini.sections.values())if(lower(s.name).startsWith("textureoverride")){
            Evaluated e=new Evaluated(ini,s,vars,rs);sections.add(e);
            if(!e.declaresDraw&&!e.bindings.containsKey("ib")&&(e.bindings.containsKey(streamKey(0))||e.bindings.containsKey(streamKey(1))||e.bindings.containsKey(streamKey(2))))providers.add(e);
            for(Draw d:e.draws){d.hash=e.hash;
                if(d.bindings.containsKey("ib")){if(!unbound(d.bindings.get("ib"))){draws.add(d);usedHashes.add(e.hash);}continue;}
                // Standard GIMI: a hash-level auto draw uses the IB of its
                // match_first_index child section, whose resource is already split.
                if(d.count==-1&&d.start==0&&!e.hash.isEmpty())autoHashes.add(e.hash);
                else throw new IllegalArgumentException("绘制区段 "+e.name+" 缺少 ib 资源");
            }
        }
        for(Evaluated e:sections)if(!e.declaresDraw&&autoHashes.contains(e.hash)&&!unbound(e.bindings.get("ib"))){{Draw d=new Draw(e.name,e.bindings,-1,0);d.hash=e.hash;draws.add(d);}usedHashes.add(e.hash);}
        for(String hash:autoHashes)check(usedHashes.contains(hash),"drawindexed=auto 没有对应的 ib 资源，hash="+hash);
        for(Draw d:draws)if(!"skip".equalsIgnoreCase(mapping.get(lower(d.part))))resolveStreams(d,providers,rs);
        return draws;
    }
    /** No replacement draw can mean the mod's geometry switch is off, or a deliberate hide. Active vertex/index
     * bindings or handling=skip retain the hide behavior; no active geometry override leaves the game's model alone. */
    private static boolean geometryOverride(Ini ini,Map<String,Integer> vars,Map<String,Resource> rs){
        for(Section s:ini.sections.values())if(lower(s.name).startsWith("textureoverride")){
            Evaluated e=new Evaluated(ini,s,vars,rs);
            if("skip".equals(e.bindings.get("handling")))return true;
            for(Map.Entry<String,String> b:e.bindings.entrySet())if((b.getKey().equals("ib")||b.getKey().matches("vb[0-9]+"))&&!unbound(b.getValue()))return true;
        }
        return false;
    }
    static final int MAX_ASSIGNMENTS=4096;
    private static final Pattern CONDITION_VAR=Pattern.compile("\\$[a-z_][a-z0-9_]*");
    private static boolean controlSection(String name){String n=lower(name);return n.startsWith("key")||n.equals("present")||n.equals("constants");}
    /** Only commands reached by model overrides/resources affect imported geometry. PC mouse/menu helpers do not. */
    private static Set<Section> conversionSections(Ini ini){Set<Section> out=new LinkedHashSet<>();
        for(Section s:ini.sections.values())if(lower(s.name).startsWith("textureoverride")||lower(s.name).startsWith("resource"))conversionSections(ini,s,out);
        return out;}
    private static void conversionSections(Ini ini,Section s,Set<Section> out){if(!out.add(s))return;
        for(String l:s.lines){String[] a=kv(l);if(a!=null&&a[0].equals("run")){Section t=ini.sections.get(lower(a[1].trim()));if(t!=null)conversionSections(ini,t,out);}}}
    private static String condition(String line){String a=lower(line);return a.startsWith("if ")?a.substring(3):a.startsWith("else if ")?a.substring(8):a.startsWith("elseif ")?a.substring(7):null;}
    /** Variables an if / else if of one section reads, with the CommandLists it runs (this INI's own). */
    private static Set<String> sectionVars(Ini ini,Section s,Set<String> visited){Set<String> out=new HashSet<>();if(!visited.add(lower(s.name)))return out;
        for(String l:s.lines){String c=condition(l);if(c!=null){Matcher m=CONDITION_VAR.matcher(c);while(m.find())out.add(m.group());}
            String[] a=kv(l);if(a!=null&&a[0].equals("run")){Section t=ini.sections.get(lower(a[1].trim()));if(t!=null)out.addAll(sectionVars(ini,t,visited));}}
        return out;}
    /** Variables an if / else if of a drawing, CommandList or Resource section reads: the only ones that can change what is drawn. */
    static List<String> conditionVars(Ini ini){
        Set<String> used=new HashSet<>();for(Section s:conversionSections(ini))if(!controlSection(s.name))used.addAll(sectionVars(ini,s,new HashSet<>()));
        List<String> out=new ArrayList<>();for(String k:ini.vars.keySet())if(used.contains(k)&&ini.choices.containsKey(k))out.add(k);return out;}
    /**
     * The variables in groups that change what is drawn independently of each other: variables read by one section
     * (with its CommandLists) belong together, so do those of the sections drawing one IB hash, and those of a section that
     * binds vertex streams or of a Resource section go with every section using them. Each group is enumerated on its own
     * with the others at their initial values, so the limit applies per group, not to the whole mod.
     */
    static List<List<Integer>> groupVars(Ini ini,List<String> names){
        int[] up=new int[names.size()];for(int i=0;i<up.length;i++)up[i]=i;
        java.util.function.IntUnaryOperator root=x->{while(up[x]!=x){up[x]=up[up[x]];x=up[x];}return x;};
        java.util.function.Consumer<Collection<String>> join=vs->{int first=-1;for(String v:vs){int k=names.indexOf(v);if(k<0)continue;if(first<0)first=k;else up[root.applyAsInt(k)]=root.applyAsInt(first);}};
        Map<String,Set<String>> byHash=new HashMap<>(),resources=new HashMap<>();List<Set<String>> providers=new ArrayList<>();Map<Section,Set<String>> own=new LinkedHashMap<>();
        for(Section s:conversionSections(ini)){if(controlSection(s.name))continue;Set<String> vs=sectionVars(ini,s,new HashSet<>());own.put(s,vs);join.accept(vs);if(vs.isEmpty())continue;
            String hash="";boolean draws=false,binds=false;for(String l:s.lines){String[] a=kv(l);if(a==null)continue;if(a[0].equals("hash"))hash=lower(a[1]);if(a[0].equals("drawindexed"))draws=true;if(a[0].matches("vb[0-9]+"))binds=true;}
            boolean child=false;for(String l:s.lines){String[] a=kv(l);if(a!=null&&a[0].equals("ib"))child=true;}
            if(!hash.isEmpty()&&!child)byHash.computeIfAbsent(hash,k->new HashSet<>()).addAll(vs);
            if(lower(s.name).startsWith("resource"))resources.put(lower(s.name),vs);
            else if(lower(s.name).startsWith("textureoverride")&&binds&&!draws)providers.add(vs);}
        // Sections of one IB hash draw their own parts; only the hash-level section (no ib of its own, e.g. drawindexed =
        // auto) reaches all of them, so its variables join each of theirs.
        for(Section s:own.keySet()){String hash="";boolean child=false;for(String l:s.lines){String[] a=kv(l);if(a==null)continue;if(a[0].equals("hash"))hash=lower(a[1]);if(a[0].equals("ib"))child=true;}
            Set<String> parent=byHash.get(hash);if(!child||parent==null||parent.isEmpty())continue;Set<String> both=new HashSet<>(parent);both.addAll(own.get(s));join.accept(both);}
        // Vertex streams bound by a section without draws can reach any draw: their variables join every section's.
        // A Resource section's variables join those of the sections that name it.
        for(Map.Entry<Section,Set<String>> e:own.entrySet()){Set<String> both=new HashSet<>(e.getValue());
            for(Set<String> vs:providers)both.addAll(vs);
            for(String l:e.getKey().lines){String low=lower(l);for(Map.Entry<String,Set<String>> r:resources.entrySet())if(low.contains(r.getKey()))both.addAll(r.getValue());}
            if(!e.getValue().isEmpty()||both.size()>e.getValue().size())join.accept(both);}
        Map<Integer,List<Integer>> groups=new LinkedHashMap<>();for(int i=0;i<names.size();i++)groups.computeIfAbsent(root.applyAsInt(i),k->new ArrayList<>()).add(i);
        return new ArrayList<>(groups.values());}
    /**
     * Runs to evaluate: run 0 has every variable at its initial value; then, group by group, every other assignment of that
     * group's variables with the rest at their initial values.
     */
    static final class Variation {
        final Ini ini;final List<String> names;final List<List<Integer>> values=new ArrayList<>();final List<List<Integer>> groups;
        final List<Map<String,Integer>> runs=new ArrayList<>();final List<int[]> runOf=new ArrayList<>();  // {group, local index}
        Variation(Ini ini,List<String> names,List<List<Integer>> groups){this.ini=ini;this.names=names;this.groups=groups;for(String k:names)values.add(new ArrayList<>(ini.choices.get(k)));
            runs.add(new LinkedHashMap<>(ini.vars));runOf.add(new int[]{-1,0});
            for(int g=0;g<groups.size();g++){int n=size(g),d=local(g,ini.vars);for(int i=0;i<n;i++){if(i==d)continue;runs.add(assignment(g,i));runOf.add(new int[]{g,i});}}}
        int size(int g){long t=1;for(int v:groups.get(g)){t*=values.get(v).size();if(t>Integer.MAX_VALUE/2)return Integer.MAX_VALUE/2;}return (int)t;}
        /** Value index of the k-th variable of group g in local assignment i (the group's first variable changes slowest). */
        int digit(int g,int i,int k){List<Integer> vs=groups.get(g);int stride=1;for(int j=vs.size()-1;j>k;j--)stride*=values.get(vs.get(j)).size();return i/stride%values.get(vs.get(k)).size();}
        int local(int g,Map<String,Integer> a){List<Integer> vs=groups.get(g);int i=0;for(int k=0;k<vs.size();k++){int v=vs.get(k);i=i*values.get(v).size()+values.get(v).indexOf(a.get(names.get(v)));}return i;}
        Map<String,Integer> assignment(int g,int i){Map<String,Integer> a=new LinkedHashMap<>(ini.vars);List<Integer> vs=groups.get(g);for(int k=0;k<vs.size();k++)a.put(names.get(vs.get(k)),values.get(vs.get(k)).get(digit(g,i,k)));return a;}
        /** The run of group g's local assignment i (its initial assignment is run 0). */
        int run(int g,int i){if(i==local(g,ini.vars))return 0;for(int r=1;r<runs.size();r++)if(runOf.get(r)[0]==g&&runOf.get(r)[1]==i)return r;throw new IllegalStateException();}
    }
    private static Set<String> lines(String signature){Set<String> out=new HashSet<>();for(String l:signature.split("\n"))if(!l.isEmpty())out.add(l);return out;}
    interface Sig {String of(Map<String,Integer> assignment)throws Exception;}
    /** What some groups changed together draw, against what each one's own run adds and removes (lines as sets). */
    static boolean consistent(Ini ini,Variation var,String[] sig,Map<Integer,Integer> set,Sig sigOf)throws Exception{
        Set<String> base=lines(sig[0]),want=new HashSet<>(base);Map<String,Integer> a=new LinkedHashMap<>(ini.vars);
        for(Map.Entry<Integer,Integer> e:set.entrySet()){int g=e.getKey(),i=e.getValue();Map<String,Integer> mine=var.assignment(g,i);for(int v:var.groups.get(g))a.put(var.names.get(v),mine.get(var.names.get(v)));
            Set<String> l=lines(sig[var.run(g,i)]);for(String x:base)if(!l.contains(x))want.remove(x);for(String x:l)if(!base.contains(x))want.add(x);}
        return lines(sigOf.of(a)).equals(want);}
    /** The t-th pair of n groups (0,1), (0,2) … (n-2,n-1): does it include group g? */
    static boolean pairHas(int t,int n,int g){for(int a=0;a<n;a++)for(int b=a+1;b<n;b++){if(t--==0)return g==a||g==b;}return false;}
    /** Variables a draw depends on in more than one group: the groups were not independent after all. */
    static final class Entangled extends RuntimeException {final Set<Integer> groups;Entangled(Set<Integer> g){super("款式变量组之间相互影响");groups=g;}}
    /**
     * One part's draws (model.json version 2): each distinct draw once, in order of first use, with "when" (the variables
     * it depends on and the value tuples it is drawn for) unless it is always drawn. A variable is a dependency when
     * changing it alone turns the draw on or off somewhere in its group's runs; then the draw depends on those only.
     */
    static List<Object> drawTable(Group g,Variation var,int[] sigOf,int signatures,Set<String> shown){
        List<String> keys=new ArrayList<>();Map<String,String[]> fields=new HashMap<>();List<Set<String>> present=new ArrayList<>();
        for(int c=0;c<signatures;c++){Set<String> on=new HashSet<>();ComboRows cr=g.combos.get(c);
            if(cr!=null){check(cr.subs.size()<=64,"子网格超过 64");for(int i=0;i<cr.subs.size();i++){String k=cr.subs.get(i)[0]+","+cr.subs.get(i)[1]+","+cr.rows.get(i)[0]+","+cr.rows.get(i)[1];if(!fields.containsKey(k)){keys.add(k);fields.put(k,new String[]{Integer.toString(cr.subs.get(i)[0]),Integer.toString(cr.subs.get(i)[1]),cr.rows.get(i)[0],cr.rows.get(i)[1]});}on.add(k);}}
            present.add(on);}
        check(!keys.isEmpty()&&keys.size()<=1024,"部件绘制段数量："+keys.size());
        List<Object> out=new ArrayList<>();
        for(String k:keys){Set<Integer> varies=new TreeSet<>();List<Integer> deps=new ArrayList<>();int depGroup=-1;
            for(int gr=0;gr<var.groups.size();gr++){List<Integer> vs=var.groups.get(gr);int n=var.size(gr);
                for(int kk=0;kk<vs.size();kk++){boolean depends=false;int stride=1;for(int j=vs.size()-1;j>kk;j--)stride*=var.values.get(vs.get(j)).size();int count=var.values.get(vs.get(kk)).size();
                    for(int i=0;i<n&&!depends;i++){if(var.digit(gr,i,kk)+1>=count)continue;depends=present.get(sigOf[var.run(gr,i)]).contains(k)!=present.get(sigOf[var.run(gr,i+stride)]).contains(k);}
                    if(depends){deps.add(kk);varies.add(gr);depGroup=gr;}}}
            if(varies.size()>1)throw new Entangled(varies);
            String[] f=fields.get(k);Map<String,Object> d=Json.of("start",Integer.parseInt(f[0]),"count",Integer.parseInt(f[1]),"material",f[2],"slot",Integer.parseInt(f[3]));
            if(!deps.isEmpty()){List<Integer> vs=var.groups.get(depGroup);LinkedHashSet<List<Integer>> allow=new LinkedHashSet<>();
                for(int i=0;i<var.size(depGroup);i++)if(present.get(sigOf[var.run(depGroup,i)]).contains(k)){List<Integer> t=new ArrayList<>();for(int kk:deps)t.add(var.values.get(vs.get(kk)).get(var.digit(depGroup,i,kk)));allow.add(t);}
                List<Object> names=new ArrayList<>();for(int kk:deps){String n=var.names.get(vs.get(kk));names.add(n);shown.add(n);}
                d.put("when",Json.of("vars",names,"allow",new ArrayList<Object>(allow)));}
            else if(!present.get(sigOf[0]).contains(k))throw new Entangled(new TreeSet<>(Arrays.asList(-1)));
            out.add(d);}
        return out;}
    static final class Bones {String name;Map<String,Object> target;List<String> names;List<Integer> parents;byte[] binds;int count;boolean synthetic;
        Bones(String n,List<String> a,byte[] b,Map<String,Object> t){name=n;names=a;binds=b;target=t;count=a.size();check(count>0&&count<=512&&b.length==64*count,"骨骼记录无效："+n);}
        /** No skeleton library: a renderer known only by name; the runtime takes the skeleton from the game (GIMI indices are the game's own order). */
        Bones(String n){name=n;count=512;synthetic=true;} }
    static final String[] DEFAULT_RENDERERS={"Body","Face","Brow","Face_Eye"};
    /**
     * skeletons/<id>/skeleton.json, written by the runtime (the library folder, or a manual export <id>-<time>): one record
     * per renderer with its bone names, bindposes and the identity of its original mesh. `id` 0 accepts any character.
     */
    static List<Bones> bones(File root,int id)throws Exception {
        File f=root.isDirectory()?new File(root,"skeleton.json"):root;check(f.isFile(),"骨骼目录里没有 skeleton.json："+root);
        Map<String,Object> j=Json.obj(Json.read(f,64L<<20),"skeleton.json");
        check("gibox-skeleton".equals(j.get("format")),"不是 GIBox 骨骼记录："+f);long version=Json.integer(j.get("version"),"version",1,1000000);check(version==1,"骨骼记录版本 "+version+" 不支持（新版 GIBox 记录的？）");
        long sid=Json.integer(j.get("id"),"id",10000000,10001999);check(id==0||sid==id,"骨骼记录属于另一个角色（"+sid+"）");
        List<Bones> out=new ArrayList<>();Set<String> seen=new HashSet<>();
        for(Object r:Json.arr(j.get("renderers"),"renderers",1,64)){
            Map<String,Object> x=Json.obj(r,"renderer");String n=safe(Json.str(x.get("name"),"renderer.name",64));check(seen.add(lower(n)),"重复的渲染器："+n);
            List<String> names=new ArrayList<>();for(Object b:Json.arr(x.get("bones"),n+".bones",1,512)){String bn=Json.str(b,n+".bones",256);check(bn.indexOf('\t')<0&&bn.indexOf('\n')<0&&bn.indexOf('\r')<0,"骨骼名无效："+n);names.add(bn);}
            List<Integer> parents=new ArrayList<>();for(Object p:Json.arr(x.get("parents"),n+".parents",names.size(),names.size()))parents.add((int)Json.integer(p,n+".parents",-1,names.size()-1));
            ByteBuffer binds=buf(new byte[64*names.size()]);for(Object v:Json.arr(x.get("bindposes"),n+".bindposes",16*names.size(),16*names.size()))binds.putFloat(Json.finite(v,n+".bindposes"));
            Map<String,Object> target=fingerprint(x.get("target"));
            check(target==null||!ModelManifest.lowerLod((String)target.get("mesh")),"骨骼记录里 "+n+" 是低精度模型 "+(target==null?"":target.get("mesh"))+"（角色刚加载时记下的），骨骼比完整模型少。让角色在大世界出场一次（骨骼库会自动更新）或靠近角色重新导出后再转换");
            Bones b=new Bones(n,names,binds.array(),target);b.parents=parents;out.add(b);
        }
        return out;}
    /** The original mesh's identity (model.hpp fingerprintOf); one that does not check out is left for the runtime to learn. */
    static Map<String,Object> fingerprint(Object v){
        if(!(v instanceof Map))return null;
        try{Map<String,Object> t=Json.obj(v,"target");Object mesh=t.get("mesh");check(mesh==null||(mesh instanceof String&&((String)mesh).length()<=256),"mesh");
            return Json.of("renderer",Json.str(t.get("renderer"),"renderer",256),"mesh",mesh==null?"":mesh,"vertices",Json.integer(t.get("vertices"),"vertices",1,500000),"submeshes",Json.integer(t.get("submeshes"),"submeshes",1,64),"bones",Json.integer(t.get("bones"),"bones",1,512));}
        catch(IllegalArgumentException e){return null;}
    }
    private static Bones target(String part,List<Bones> bones,Map<String,String> map){String manual=map.get(lower(part));if(manual!=null){for(Bones b:bones)if(b.name.equalsIgnoreCase(manual))return b;throw new IllegalArgumentException("映射目标不存在："+manual);}String a=lower(part).replaceFirst("[a-z]$","");Bones best=null;int score=0;boolean tie=false;for(Bones b:bones){String n=lower(b.name).replaceFirst("_lod[0-9]+$","");int k=lower(part).endsWith(n)||a.endsWith(n)?n.length():0;if(k>score){score=k;best=b;tie=false;}else if(k>0&&k==score)tie=true;}check(!tie,"渲染器映射有歧义："+part);if(best!=null)return best;if(bones.size()==1)return bones.get(0);throw new IllegalArgumentException("请手工映射部件："+part);}
    static final class IntList {int[] data;int size;IntList(){this(1024);}IntList(int capacity){data=new int[capacity];}int size(){return size;}void add(int n){check(size<15000000,"索引数超限");if(size==data.length)data=Arrays.copyOf(data,Math.min(15000000,Math.max(size+1,size+size/2)));data[size++]=n;}}
    static final class Vertex {byte[] pos,norm,uv,tangent,color,weights;List<byte[]> extra=new ArrayList<>();boolean unweighted;int trimmedWeights;}
    /** Packed records keep retained vertex data out of thousands of Java objects and tiny arrays. */
    static final class PackedVertices {
        static final int STRIDE=112,CHUNK=4096;final List<byte[]> chunks=new ArrayList<>();int count,trimmed,requiredBones=1,extras=3;
        int size(){return count;}byte[] chunk(int i){return chunks.get(i/CHUNK);}int offset(int i){return i%CHUNK*STRIDE;}
        void add(Vertex v){check(count<500000,"顶点数超限");if(count%CHUNK==0)chunks.add(new byte[CHUNK*STRIDE]);byte[] out=chunk(count);int p=offset(count);
            System.arraycopy(v.pos,0,out,p,12);System.arraycopy(v.norm,0,out,p+12,12);System.arraycopy(v.uv,0,out,p+24,8);System.arraycopy(v.weights,0,out,p+32,32);System.arraycopy(v.color,0,out,p+64,4);System.arraycopy(v.tangent,0,out,p+68,16);
            extras=Math.min(extras,v.extra.size());for(int k=0;k<v.extra.size();k++)System.arraycopy(v.extra.get(k),0,out,p+84+8*k,8);
            out[p+108]=(byte)(v.unweighted?1:0);if(v.trimmedWeights>0)trimmed++;ByteBuffer skin=buf(v.weights);for(int k=0;k<4;k++)if(skin.getFloat(4*k)>0)requiredBones=Math.max(requiredBones,skin.getInt(16+4*k)+1);count++;}
        boolean unweighted(int i){return chunk(i)[offset(i)+108]!=0;}
        float position(int i,int axis){return buf(chunk(i)).getFloat(offset(i)+axis*4);}
        void copyWeights(int to,int from){System.arraycopy(chunk(from),offset(from)+32,chunk(to),offset(to)+32,32);chunk(to)[offset(to)+108]=0;}
        byte[] column(int stride,int field){byte[] out=new byte[count*stride];for(int i=0;i<count;i++)System.arraycopy(chunk(i),offset(i)+field,out,i*stride,stride);return out;}
    }
    /** A permutation is exact: move every attribute together and remap every index, without changing any triangle. */
    static final class WeightOrder {
        static final class Bucket {final IntList slots=new IntList(4);int cursor;}
        final Map<ByteBuffer,Bucket> buckets=new HashMap<>();final int count;
        WeightOrder(PackedVertices vertices){count=vertices.size();byte[] weights=vertices.column(32,32);for(int i=0;i<count;i++)buckets.computeIfAbsent(ByteBuffer.wrap(weights,32*i,32).slice(),k->new Bucket()).slots.add(i);}
        boolean align(Group group){if(group.vertices.size()!=count)return false;for(Bucket b:buckets.values())b.cursor=0;
            byte[] weights=group.vertices.column(32,32);int[] order=new int[count];for(int i=0;i<count;i++){Bucket b=buckets.get(ByteBuffer.wrap(weights,32*i,32).slice());if(b==null||b.cursor>=b.slots.size())return false;order[i]=b.slots.data[b.cursor++];}
            PackedVertices old=group.vertices,sorted=new PackedVertices();sorted.count=count;sorted.trimmed=old.trimmed;sorted.requiredBones=old.requiredBones;sorted.extras=old.extras;
            for(int i=0;i<count;i+=PackedVertices.CHUNK)sorted.chunks.add(new byte[PackedVertices.CHUNK*PackedVertices.STRIDE]);
            for(int i=0;i<count;i++)System.arraycopy(old.chunk(i),old.offset(i),sorted.chunk(order[i]),sorted.offset(order[i]),PackedVertices.STRIDE);
            for(int i=0;i<group.indices.size();i++)group.indices.data[i]=order[group.indices.data[i]];group.vertices=sorted;return true;}
    }
    static final class BufferCache extends LinkedHashMap<File,byte[]> {
        long bytes;BufferCache(){super(16,.75f,true);}
        @Override public byte[] put(File file,byte[] data){byte[] old=super.put(file,data);bytes+=data.length-(old==null?0:old.length);Iterator<Map.Entry<File,byte[]>> it=entrySet().iterator();while(bytes>16L*1024*1024&&it.hasNext()){bytes-=it.next().getValue().length;it.remove();}return old;}
        @Override public void clear(){super.clear();bytes=0;}
    }
    /** Submeshes and material rows of one renderer part in one variant combination. */
    static final class ComboRows {final List<int[]> subs=new ArrayList<>();final List<String[]> rows=new ArrayList<>();String key(){StringBuilder s=new StringBuilder();for(int i=0;i<subs.size();i++)s.append(subs.get(i)[0]).append(',').append(subs.get(i)[1]).append(':').append(rows.get(i)[0]).append('#').append(rows.get(i)[1]).append(';');return s.toString();}}
    /** One renderer: union of every combination's vertices and index ranges, plus per-combination submeshes. */
    static final class Group {Bones bones;String name;PackedVertices vertices=new PackedVertices();IntList indices=new IntList();Map<String,int[]> compact=new LinkedHashMap<>();Map<String,int[]> ranges=new HashMap<>();Map<Integer,ComboRows> combos=new HashMap<>();List<Object> draws;Group(Bones b,String n){bones=b;name=n;} }
    static final class MaterialDef {String name;Resource diffuse,light,normal;}
    static final class Build {final LinkedHashMap<String,Group> groups=new LinkedHashMap<>();final LinkedHashMap<String,MaterialDef> materials=new LinkedHashMap<>();final BufferCache cache=new BufferCache();}
    /** Relative to this vertex's total: a little above one float32 ULP at 1. Unused exporter slots can contain a tiny
     * positive residue and a sentinel bone index. Remove that residue before checking indices; real weights still fail. */
    static final float WEIGHT_EPSILON=2e-7f;
    static int cleanWeights(float[] w,int[] ids,int boneCount){
        float sum=0;for(float x:w){check(Float.isFinite(x)&&x>=0,"非法权重");sum+=x;}check(Float.isFinite(sum),"非法权重");int trimmed=0;
        for(int k=0;k<4;k++){
            if(sum==0||w[k]/sum<=WEIGHT_EPSILON){if(w[k]>0)trimmed++;w[k]=0;ids[k]=0;}
            else check(ids[k]>=0&&ids[k]<boneCount,"骨骼索引越界：索引 "+ids[k]+"，骨骼数 "+boneCount+"，有效权重 "+(w[k]/sum));
        }
        return trimmed;
    }
    private static Vertex vertex(Resource position,Resource blend,Resource tex,byte[] p,byte[] b,byte[] t,int i,int boneCount){
        check(position.stride>=40&&blend.stride==32&&tex.stride>=12&&tex.stride<=36&&(tex.stride-4)%8==0,"顶点流 stride 不兼容");check(i>=0&&(long)(i+1)*position.stride<=p.length&&(long)(i+1)*32<=b.length&&(long)(i+1)*tex.stride<=t.length,"顶点索引越界");int po=i*position.stride,bo=i*32,to=i*tex.stride;Vertex v=new Vertex();v.pos=Arrays.copyOfRange(p,po,po+12);v.norm=Arrays.copyOfRange(p,po+12,po+24);v.tangent=Arrays.copyOfRange(p,po+24,po+40);v.color=Arrays.copyOfRange(t,to,to+4);v.uv=uv(t,to+4);for(int k=12;k+8<=tex.stride;k+=8)v.extra.add(uv(t,to+k));v.weights=new byte[32];ByteBuffer bi=buf(b),wo=buf(v.weights);float[] w=new float[4];int[] ids=new int[4];for(int k=0;k<4;k++){w[k]=bi.getFloat(bo+4*k);ids[k]=bi.getInt(bo+16+4*k);}
        v.trimmedWeights=cleanWeights(w,ids,boneCount);boolean weighted=false;for(float x:w)weighted|=x>0;if(weighted)tidy(w,ids,wo);else v.unweighted=true;for(byte[] channel:Arrays.asList(v.pos,v.norm,v.tangent)){ByteBuffer c=buf(channel);for(int k=0;k<channel.length;k+=4)check(Float.isFinite(c.getFloat(k)),"非法几何数值");}return v;
    }
    /** One vertex's four weights: the same bone listed twice added together, largest first, summing to 1; empty slots 0 on bone 0. */
    static void tidy(float[] w,int[] ids,ByteBuffer out){
        float[] tw=new float[4];int[] ti=new int[4];int n=0;
        for(int k=0;k<4;k++){if(!(w[k]>0))continue;int j=0;while(j<n&&ti[j]!=ids[k])j++;if(j<n)tw[j]+=w[k];else{tw[n]=w[k];ti[n]=ids[k];n++;}}
        Integer[] order=new Integer[n];for(int k=0;k<n;k++)order[k]=k;final float[] fw=tw;final int[] fi=ti;Arrays.sort(order,(x,y)->fw[x]!=fw[y]?Float.compare(fw[y],fw[x]):Integer.compare(fi[x],fi[y]));
        float sum=0;for(int k=0;k<n;k++)sum+=tw[k];
        for(int k=0;k<4;k++){out.putFloat(4*k,k<n?tw[order[k]]/sum:0f);out.putInt(16+4*k,k<n?ti[order[k]]:0);}}
    private static byte[] uv(byte[] t,int p){ByteBuffer b=buf(t);float u=b.getFloat(p),v=b.getFloat(p+4);check(Float.isFinite(u)&&Float.isFinite(v),"非法 UV");return buf(new byte[8]).putFloat(u).putFloat(1-v).array();}
    /** A DDS/PNG texture as RGBA8, at most 1024 on a side, rows bottom to top: {width, height, data view}. */
    private static Map<String,Object> texture(File f,TextureDecoder decoder,BlobWriter blob,Map<File,Map<String,Object>> done)throws Exception {
        Map<String,Object> known=done.get(f);if(known!=null)return known;
        String ext=lower(f.getName());check(ext.endsWith(".dds")||ext.endsWith(".png"),"当前转换器支持 DDS / PNG 纹理："+f);byte[] raw=decoder.decode(read(f));check(raw.length>=8,"纹理解码返回值无效");ByteBuffer h=buf(raw);int w=h.getInt(0),y=h.getInt(4);check(w>0&&y>0&&w<=8192&&y<=8192&&raw.length==8+4L*w*y,"纹理解码尺寸无效");byte[] pixels=Arrays.copyOfRange(raw,8,raw.length);
        while(w>1024||y>1024){int nw=Math.max(1,w/2),nh=Math.max(1,y/2);byte[] n=new byte[nw*nh*4];for(int j=0;j<nh;j++)for(int i=0;i<nw;i++)for(int c=0;c<4;c++){int sum=0,count=0;for(int dy=0;dy<2&&j*2+dy<y;dy++)for(int dx=0;dx<2&&i*2+dx<w;dx++){sum+=pixels[((j*2+dy)*w+i*2+dx)*4+c]&255;count++;}n[(j*nw+i)*4+c]=(byte)(sum/count);}pixels=n;w=nw;y=nh;}
        byte[] rows=new byte[pixels.length];for(int j=0;j<y;j++)System.arraycopy(pixels,(y-1-j)*w*4,rows,j*w*4,w*4);
        Map<String,Object> t=Json.of("width",w,"height",y,"data",blob.put(rows));done.put(f,t);return t;
    }
    /** Draws of one variable assignment, each resolved to its target renderer. */
    private static LinkedHashMap<Draw,Bones> resolve(Ini ini,File src,List<Bones> boneRecords,Map<String,String> mapping,Map<String,Integer> vars,Map<String,Resource> rs,StringBuilder log){return resolve(ini,src,boneRecords,mapping,vars,rs,log,null);}
    private static LinkedHashMap<Draw,Bones> resolve(Ini ini,File src,List<Bones> boneRecords,Map<String,String> mapping,Map<String,Integer> vars,Map<String,Resource> rs,StringBuilder log,String fallback){
        // The initial state may legitimately have geometry replacement disabled (e.g. Shenhe's $vision=1).
        // Reject a mod only after examining all available assignments, not before its active state is visited.
        List<Draw> draws=drawCalls(ini,vars,rs,mapping);if(draws.isEmpty())return new LinkedHashMap<>();
        LinkedHashMap<Draw,Bones> targets=new LinkedHashMap<>();Map<String,Bones> byHash=new HashMap<>();List<Draw> pending=new ArrayList<>();
        for(Draw d:draws){if("skip".equalsIgnoreCase(mapping.get(lower(d.part))))continue;try{Bones b=target(d.part,boneRecords,mapping);targets.put(d,b);if(!d.hash.isEmpty()){Bones prev=byHash.putIfAbsent(d.hash,b);check(prev==null||prev==b,"同一 IB hash 的部件映射到了不同渲染器："+d.hash);}}catch(IllegalArgumentException e){if(d.hash.isEmpty()||mapping.containsKey(lower(d.part)))throw e;pending.add(d);}}
        Bones fallbackBones=null;if(fallback!=null)for(Bones b:boneRecords)if(b.name.equalsIgnoreCase(fallback))fallbackBones=b;
        for(Draw d:pending){Bones b=byHash.get(d.hash);if(b==null&&fallbackBones!=null){b=fallbackBones;byHash.put(d.hash,b);if(log!=null)log.append(d.part).append(" -> ").append(b.name).append("（未匹配到渲染器名，按默认渲染器）\n");targets.put(d,b);continue;}check(b!=null,"请手工映射部件："+d.part+"（同一 hash 的其他部件也没有匹配到渲染器）");targets.put(d,b);if(log!=null)log.append(d.part).append(" -> ").append(b.name).append("（按同一 IB hash 推定）\n");}
        LinkedHashMap<Draw,Bones> ordered=new LinkedHashMap<>();for(Draw d:draws)if(targets.containsKey(d))ordered.put(d,targets.get(d));return ordered;
    }
    private static Resource[] textures(Draw d,Map<String,Resource> rs){Resource diffuse=null,light=null,normal=null;
        if("nnfix".equals(d.bindings.get("texture-role:layout"))){
            String color=d.bindings.get("texture-role:diffuse"),mask=d.bindings.get("texture-role:lightmap");
            return new Resource[]{unbound(color)?null:resource(rs,color),unbound(mask)?null:resource(rs,mask),null};
        }
        // GIMI's legacy Nahida IB identifies the normal/diffuse/lightmap layout even in older exports whose
        // filenames misleadingly say Diffuse/LightMap/Shadow. Apply per draw, only with the full three-slot layout.
        // Explicit NNFix above has priority, including on this IB. Unknown hashes keep the existing role resolver.
        if("8b268d05".equals(d.hash)&&!unbound(d.bindings.get("ps-t0"))&&!unbound(d.bindings.get("ps-t1"))&&!unbound(d.bindings.get("ps-t2")))
            return new Resource[]{resource(rs,d.bindings.get("ps-t1")),resource(rs,d.bindings.get("ps-t2")),resource(rs,d.bindings.get("ps-t0"))};
        for(Map.Entry<String,String> binding:d.bindings.entrySet())if(binding.getKey().startsWith("ps-t")&&!unbound(binding.getValue())){Resource r=resource(rs,binding.getValue());String file=lower(r.file.getName());
            // Resource names often keep the semantic role when the file is called Skin.dds or has a typo (Ayaka's
            // ResourceAyakaHeadDiffuse.hair_type_2.black points at HairDifuse_Type_2_black.dds). Preserve file hints first.
            String role=file.contains("lightmap")||file.contains("normal")||file.contains("diffuse")?file:lower(binding.getValue());
            if(role.contains("lightmap"))light=r;else if(role.contains("normal"))normal=r;else if(role.contains("diffuse"))diffuse=r;}
        return new Resource[]{diffuse,light,normal};}
    private static String path(Resource r){return r==null?"-":r.file.getPath();}
    /**
     * The renderer material slot of a draw: a part name ending in a capital letter picks it (BodyA = 0, BodyB = 1 …);
     * otherwise the part's place among the sections of its IB hash by match_first_index (on the PC the IB is split in
     * the renderer's submesh order); otherwise 0. It never depends on which other draws are on.
     */
    private static int slot(Ini ini,Draw d){char last=d.part.charAt(d.part.length()-1);if(last>='A'&&last<='Z')return checkedSlot(last-'A');
        if(d.hash.isEmpty())return 0;TreeMap<Long,String> parts=new TreeMap<>();
        for(Section s:ini.sections.values()){if(!lower(s.name).startsWith("textureoverride"))continue;String h="";Long first=null;
            for(String l:s.lines){String[] a=kv(l);if(a==null)continue;if(a[0].equals("hash"))h=lower(a[1]);if(a[0].equals("match_first_index"))try{first=Long.parseLong(a[1].trim());}catch(NumberFormatException e){}}
            if(h.equals(d.hash)&&first!=null)parts.put(first,lower(s.name.substring(15)));}
        int i=0;for(String n:parts.values()){if(n.equals(lower(d.part)))return checkedSlot(i);i++;}return 0;}
    private static int checkedSlot(int s){check(s<64,"材质槽越界");return s;}
    /** What a combination draws, without reading buffers: used to find the variables that actually change the model. */
    private static String signature(Ini ini,File src,List<Bones> boneRecords,Map<String,String> mapping,Map<String,Integer> vars,String fallback)throws Exception{
        Map<String,Resource> rs=resources(ini,src,vars);LinkedHashMap<Draw,Bones> resolved=resolve(ini,src,boneRecords,mapping,vars,rs,null,fallback);
        StringBuilder s=new StringBuilder(!resolved.isEmpty()||geometryOverride(ini,vars,rs)?"replace\n":"original\n");
        for(Map.Entry<Draw,Bones> e:resolved.entrySet()){Draw d=e.getKey();Resource[] t=textures(d,rs);
            s.append(e.getValue().name).append('|').append(d.part).append('#').append(slot(ini,d)).append('|').append(path(stream(rs,d.bindings,0))).append('|').append(path(stream(rs,d.bindings,1))).append('|').append(path(stream(rs,d.bindings,2))).append('|').append(path(resource(rs,d.bindings.get("ib")))).append('|').append(d.start).append('|').append(d.count).append('|').append(path(t[0])).append(path(t[1])).append(path(t[2])).append('\n');}
        return s.toString();}
    private static void collect(Ini ini,File src,List<Bones> boneRecords,Map<String,String> mapping,Map<String,Integer> vars,int combo,Build b,StringBuilder log,String fallback)throws Exception {
        Map<String,Resource> rs=resources(ini,src,vars);LinkedHashMap<Draw,Bones> targets=resolve(ini,src,boneRecords,mapping,vars,rs,combo==0?log:null,fallback);
        for(Map.Entry<Draw,Bones> entry:targets.entrySet()){Draw d=entry.getKey();Bones target=entry.getValue();Group g=b.groups.computeIfAbsent(lower(target.name),k->new Group(target,"Part"+b.groups.size()));
            Resource pos=stream(rs,d.bindings,0),blend=stream(rs,d.bindings,1),tex=stream(rs,d.bindings,2),indices=resource(rs,d.bindings.get("ib"));check(indices!=null,"缺少索引流");
            if(combo==0)log.append(d.part).append(": Position=").append(pos.file.getName()).append(", Blend=").append(blend.file.getName()).append(", Texcoord=").append(tex.file.getName()).append('\n');
            byte[] p=b.cache.get(pos.file),bl=b.cache.get(blend.file),t=b.cache.get(tex.file),ix=b.cache.get(indices.file);if(p==null){p=read(pos.file);b.cache.put(pos.file,p);}if(bl==null){bl=read(blend.file);b.cache.put(blend.file,bl);}if(t==null){t=read(tex.file);b.cache.put(tex.file,t);}if(ix==null){ix=read(indices.file);b.cache.put(indices.file,ix);}
            int stride=lower(indices.format).contains("r16_uint")?2:lower(indices.format).contains("r32_uint")?4:indices.stride;check(stride==2||stride==4,"索引格式必须为 R16_UINT 或 R32_UINT");check(ix.length%stride==0,"索引流长度");int count=d.count==-1?ix.length/stride-d.start:d.count;check(count>0&&count%3==0&&(long)d.start+count<=ix.length/stride,"drawindexed 范围");
            // The same draw in several combinations shares one index range.
            String rangeKey=pos.file.getPath()+"|"+blend.file+"|"+tex.file+"|"+indices.file+"|"+stride+"|"+d.start+"|"+count;int[] range=g.ranges.get(rangeKey);
            if(range==null){String streamId=identity(pos)+"|"+identity(blend)+"|"+identity(tex);int[] remap=g.compact.get(streamId);if(remap==null){remap=new int[Math.min(p.length/pos.stride,Math.min(bl.length/32,t.length/tex.stride))];g.compact.put(streamId,remap);}int start=g.indices.size();ByteBuffer ib=buf(ix);for(int j=0;j<count;j++){int old=stride==2?(ib.getShort((d.start+j)*2)&65535):ib.getInt((d.start+j)*4);check(old>=0&&old<p.length/pos.stride&&old<bl.length/32&&old<t.length/tex.stride,"顶点索引越界");int compact=remap[old]-1;if(compact<0){compact=g.vertices.size();check(compact<500000,"顶点数超限");remap[old]=compact+1;g.vertices.add(vertex(pos,blend,tex,p,bl,t,old,target.count));}g.indices.add(compact);}range=new int[]{start,count};g.ranges.put(rangeKey,range);}
            ComboRows rows=g.combos.computeIfAbsent(combo,k->new ComboRows());Resource[] tx=textures(d,rs);String materialKey=path(tx[0])+"|"+path(tx[1])+"|"+path(tx[2]);
            if("8b268d05".equals(d.hash)&&!"nnfix".equals(d.bindings.get("texture-role:layout"))&&tx[0]!=null&&tx[1]!=null&&tx[2]!=null)
                ini.compatibility.add("已按纳西妲 IB 8b268d05 识别贴图：ps-t0 为法线，ps-t1 为颜色，ps-t2 为光照遮罩（优先于文件名）");
            MaterialDef m=b.materials.get(materialKey);if(m==null){m=new MaterialDef();m.name="Material"+b.materials.size();m.diffuse=tx[0];m.light=tx[1];m.normal=tx[2];b.materials.put(materialKey,m);}
            rows.rows.add(new String[]{m.name,Integer.toString(slot(ini,d))});rows.subs.add(range);
        }
    }
    /**
     * Vertices the mod author left without any bone weight (all four 0). On PC such a vertex is skinned by nothing and
     * collapses to the origin (a spike towards the feet); here it takes the weights of the nearest weighted vertex of the
     * same part, which is where it was meant to move with. A part with no weighted vertex at all is refused.
     */
    static int fixUnweighted(Group g){
        IntList bad=new IntList(),good=new IntList();for(int i=0;i<g.vertices.size();i++)(g.vertices.unweighted(i)?bad:good).add(i);
        if(bad.size()==0)return 0;
        check(good.size()>0,g.bones.name+"：所有顶点的权重都是 0（MOD 没有绑定骨骼），无法转换");
        check(bad.size()<=20000,g.bones.name+"：有 "+bad.size()+" 个顶点没有权重（MOD 本身的问题），太多了，无法自动修补");
        float[] gp=new float[good.size()*3];for(int j=0;j<good.size();j++)for(int k=0;k<3;k++)gp[j*3+k]=g.vertices.position(good.data[j],k);
        for(int j=0;j<bad.size();j++){int i=bad.data[j];float x=g.vertices.position(i,0),y=g.vertices.position(i,1),z=g.vertices.position(i,2);int best=0;double bd=Double.MAX_VALUE;
            for(int k=0;k<good.size();k++){double dx=gp[k*3]-x,dy=gp[k*3+1]-y,dz=gp[k*3+2]-z,d=dx*dx+dy*dy+dz*dz;if(d<bd){bd=d;best=k;}}
            g.vertices.copyWeights(i,good.data[best]);}
        return bad.size();}
    /** model.bin: written front to back with its CRC-32; put() returns the [offset, length] view of what it wrote. */
    static final class BlobWriter implements Closeable {
        static final long MAX=768L<<20;
        final OutputStream out;final java.util.zip.CRC32 crc=new java.util.zip.CRC32();long size;
        BlobWriter(File f)throws IOException{out=new BufferedOutputStream(new FileOutputStream(f),1<<16);}
        private void raw(byte[] b)throws IOException{out.write(b);crc.update(b,0,b.length);size+=b.length;check(size<=MAX,"模型数据超过 768 MB");}
        List<Object> put(byte[] b)throws IOException{int pad=(int)((16-size%16)%16);if(pad>0)raw(new byte[pad]);long at=size;raw(b);return Arrays.<Object>asList(at,(long)b.length);}
        @Override public void close()throws IOException{out.close();}
    }
    private static List<Object> emitParts(Build b,BlobWriter blob)throws Exception {
        List<Object> parts=new ArrayList<>();
            for(Group g:b.groups.values()){
                int n=g.vertices.size(),ni=g.indices.size();
                check(n>0&&n<=500000&&ni>0&&ni<=15000000&&ni%3==0,"部件尺寸越界");for(ComboRows c:g.combos.values())check(c.subs.size()<=64,"子网格超过 64");
                int bones=g.bones.synthetic?g.vertices.requiredBones:g.bones.count;
                Map<String,Object> data=new LinkedHashMap<>();
                data.put("positions",blob.put(g.vertices.column(12,0)));data.put("normals",blob.put(g.vertices.column(12,12)));data.put("uv",blob.put(g.vertices.column(8,24)));
                ByteBuffer ix=buf(new byte[4*ni]);for(int i=0;i<ni;i++)ix.putInt(g.indices.data[i]);data.put("indices",blob.put(ix.array()));
                data.put("weights",blob.put(g.vertices.column(32,32)));
                data.put("colors",blob.put(g.vertices.column(4,64)));data.put("tangents",blob.put(g.vertices.column(16,68)));
                for(int e=0;e<g.vertices.extras;e++)data.put("uv"+(e+2),blob.put(g.vertices.column(8,84+8*e)));
                Map<String,Object> part=Json.of("name",g.name,"renderer",g.bones.name,"bones",bones,"vertices",n,"indices",ni,"data",data);
                if(!g.bones.synthetic){Map<String,Object> sk=Json.of("bones",g.bones.names,"bindposes",blob.put(g.bones.binds));if(g.bones.parents!=null)sk.put("parents",new ArrayList<Object>(g.bones.parents));part.put("skeleton",sk);}
                if(g.bones.target!=null)part.put("target",g.bones.target);
                // A part not drawn at all with the initial values (only possible after falling back to them) shows nothing.
                part.put("draws",g.draws!=null?g.draws:Collections.singletonList(Json.of("start",0,"count",0,"material",b.materials.values().iterator().next().name,"slot",0)));parts.add(part);
            }
        return parts;
    }
    private static List<Object> emitMaterials(Build b,TextureDecoder decode,BlobWriter blob)throws Exception {
        List<Object> materials=new ArrayList<>();Map<File,Map<String,Object>> textures=new HashMap<>();
            for(MaterialDef m:b.materials.values()){Map<String,Object> e=Json.of("name",m.name);
                if(m.diffuse==null)e.put("original",true);
                else{e.put("main",texture(m.diffuse.file,decode,blob,textures));if(m.light!=null)e.put("lightmap",texture(m.light.file,decode,blob,textures));if(m.normal!=null)e.put("normalmap",texture(m.normal.file,decode,blob,textures));}
                materials.add(e);}
        return materials;
    }
    /** Parts and textures share one blob. Source buffers/remaps are not needed during texture decoding. */
    private static Map<String,Object> emit(File out,Build b,int combos,TextureDecoder decode)throws Exception {
        b.cache.clear();for(Group g:b.groups.values()){g.compact.clear();g.ranges.clear();}
        try(BlobWriter blob=new BlobWriter(new File(out,"model.bin"))){
            List<Object> parts=emitParts(b,blob);b.groups.clear();
            List<Object> materials=emitMaterials(b,decode,blob);blob.out.flush();
            return Json.of("blob",Json.of("file","model.bin","size",blob.size,"crc32",blob.crc.getValue()),"parts",parts,"materials",materials);
        }
    }
    /** A bounded frame loop, compiled from Present. Other dynamic programs fail explicitly. */
    static final class Animation {
        boolean stableWeights=true;String variable;int first,last,step,initial;double counter,period,counterStep;final List<Integer> values=new ArrayList<>();
        static double number(Ini ini,String expression){String x=lower(expression.trim());Double n=x.startsWith("$")?ini.numbers.get(x):Double.valueOf(x);check(n!=null&&Double.isFinite(n),"动画常量无效："+expression);return n;}
        static int integer(Ini ini,String x){double n=number(ini,x);check(n==Math.rint(n)&&Math.abs(n)<=1000000,"动画帧号必须是整数："+x);return (int)n;}
        static Animation parse(Ini ini){
            Section present=ini.sections.get("present");if(present==null)return null;List<String> model=conditionVars(ini);boolean dynamic=false;
            Set<Section> ticks=new LinkedHashSet<>();conversionSections(ini,present,ticks);
            for(Section section:ticks)for(String l:section.lines){String x=lower(l).replaceAll("\\s+","").replaceFirst("^post","");int eq=x.indexOf('=');
                if(eq>0&&model.contains(x.substring(0,eq))&&x.substring(eq+1).contains("$"))dynamic=true;}
            if(!dynamic)return null;
            List<String> lines=new ArrayList<>();for(String l:present.lines)lines.add(lower(l).replaceAll("\\s+",""));
            String why="暂不支持这个 Present 动画程序；当前支持按计数器取余、递增帧号并循环的模型动画，不能当作普通款式合并";
            check(lines.size()==8,why);
            Matcher timing=Pattern.compile("if(\\$[a-z_][a-z0-9_]*)%(.+)==0").matcher(lines.get(0));
            Matcher bound=Pattern.compile("if(\\$[a-z_][a-z0-9_]*)<(.+)").matcher(lines.get(1));
            check(timing.matches()&&bound.matches(),why);Animation a=new Animation();a.variable=bound.group(1);
            check(model.contains(a.variable),why);for(String v:model)check(v.equals(a.variable),"动画同时带其他绘制款式变量，暂不支持："+v);
            String increment=a.variable+"="+a.variable+"+",clock=timing.group(1);
            check(lines.get(2).startsWith(increment)&&lines.get(3).equals("else")&&lines.get(4).startsWith(a.variable+"=")&&lines.get(5).equals("endif")&&lines.get(6).equals("endif")&&lines.get(7).startsWith("post"+clock+"="+clock+"+"),why);
            a.first=integer(ini,lines.get(4).substring(a.variable.length()+1));a.last=integer(ini,bound.group(2));a.step=integer(ini,lines.get(2).substring(increment.length()));
            a.initial=integer(ini,a.variable);a.counter=number(ini,clock);a.period=number(ini,timing.group(2));a.counterStep=number(ini,lines.get(7).substring(("post"+clock+"="+clock+"+").length()));
            check(a.first<=a.last&&a.step>0&&a.period>0&&a.counterStep>0&&a.period<=1000000&&a.counterStep<=1000000,"动画循环边界或周期无效");
            int frame=a.initial;do{check(!a.values.contains(frame)&&a.values.size()<64,"动画超过 64 帧或帧序列不闭合");a.values.add(frame);frame=frame<a.last?frame+a.step:a.first;}while(frame!=a.initial);
            return a;
        }
        Map<String,Object> json(List<Object> frames){return Json.of("kind","present-loop","stableWeights",stableWeights,"variable",variable,"first",first,"last",last,"step",step,"initial",initial,"counter",counter,"period",period,"counterStep",counterStep,"frames",frames);}
    }
    private static void memoryBudget(Ini ini,File src,List<Bones> bones,Map<String,String> map,List<Map<String,Integer>> assignments,String fallback,StringBuilder log)throws Exception {
        Map<String,Long> streams=new HashMap<>();Set<String> ranges=new HashSet<>();long indices=0,input=0;
        for(Map<String,Integer> vars:assignments){Map<String,Resource> rs=resources(ini,src,vars);for(Draw d:resolve(ini,src,bones,map,vars,rs,null,fallback).keySet()){
            Resource p=stream(rs,d.bindings,0),b=stream(rs,d.bindings,1),t=stream(rs,d.bindings,2),ix=resource(rs,d.bindings.get("ib"));check(ix!=null&&p.stride>=40&&b.stride==32&&t.stride>=12,"顶点流或索引流无效");
            int stride=lower(ix.format).contains("r16_uint")?2:lower(ix.format).contains("r32_uint")?4:ix.stride;check(stride==2||stride==4,"索引格式必须为 R16_UINT 或 R32_UINT");
            long count=d.count<0?ix.file.length()/stride-d.start:d.count;check(count>0&&count<=15000000,"drawindexed 范围");String id=identity(p)+"|"+identity(b)+"|"+identity(t);
            long vertices=Math.min(p.file.length()/p.stride,Math.min(b.file.length()/32,t.file.length()/t.stride));streams.put(id,vertices);
            if(ranges.add(id+"|"+identity(ix)+"|"+d.start+"|"+count))indices+=count;
            input=Math.max(input,p.file.length()+b.file.length()+t.file.length()+ix.file.length());}}
        long vertices=0;for(long n:streams.values())vertices+=n;
        long estimate=vertices*128+indices*6+input+16L*1024*1024,budget=Math.min(160L*1024*1024,Runtime.getRuntime().maxMemory()/2);
        log.append("转换内存预算：几何估算 ").append(estimate/1024/1024).append(" MiB，预算 ").append(budget/1024/1024).append(" MiB\n");
        check(estimate<=budget,"模型几何预计超出转换内存预算（"+estimate/1024/1024+" / "+budget/1024/1024+" MiB）；请减少同时转换的款式或拆分动画资源");
    }
    private static File convertAnimation(Ini ini,File src,List<Bones> bones,Map<String,String> mapping,File dest,int id,Options o,TextureDecoder decoder,StringBuilder log,Animation animation)throws Exception {
        dest=dest.getCanonicalFile();File parent=dest.getParentFile();check(parent.isDirectory()||parent.mkdirs(),"无法创建输出根目录");check(o.replace||!dest.exists(),"输出目录已存在："+dest);
        File tmp=Files.createTempDirectory(parent.toPath(),".model-convert-").toFile();
        log.append("模型动画：").append(animation.values.size()).append(" 帧，按 Present 次数推进；计数周期 ").append(animation.period).append("，完整转换，不合并为款式网格\n");
        try{
            Build registry=new Build();Map<String,WeightOrder> weights=new HashMap<>();List<Object> frames=new ArrayList<>(),firstParts=null;String layout=null;
            try(BlobWriter blob=new BlobWriter(new File(tmp,"model.bin"))){
                for(int frame:animation.values){Map<String,Integer> vars=new LinkedHashMap<>(ini.vars);vars.put(animation.variable,frame);memoryBudget(ini,src,bones,mapping,Collections.singletonList(vars),o.fallback,log);
                    Build b=new Build();b.materials.putAll(registry.materials);collect(ini,src,bones,mapping,vars,0,b,log,o.fallback);check(!b.groups.isEmpty(),"动画帧没有有效模型绘制："+frame);
                    Variation one=new Variation(ini,new ArrayList<>(),new ArrayList<>());for(Group g:b.groups.values()){fixUnweighted(g);WeightOrder order=weights.get(g.name);if(order==null)weights.put(g.name,new WeightOrder(g.vertices));else if(!order.align(g))animation.stableWeights=false;g.draws=drawTable(g,one,new int[]{0},1,new HashSet<>());g.compact.clear();g.ranges.clear();}b.cache.clear();
                    List<Object> parts=emitParts(b,blob);StringBuilder key=new StringBuilder();for(Object raw:parts){Map<String,Object> p=Json.obj(raw,"part");key.append(p.get("name")).append('|').append(p.get("renderer")).append('|').append(p.get("vertices")).append('|').append(p.get("bones")).append(Json.obj(p.get("data"),"data").keySet());
                        for(Object dr:Json.arr(p.get("draws"),"draws",1,64)){Map<String,Object> d=Json.obj(dr,"draw");key.append('|').append(d.get("material")).append('#').append(d.get("slot"));}}
                    if(layout==null){layout=key.toString();firstParts=parts;}else check(layout.equals(key.toString()),"动画各帧的顶点数、骨骼或材质槽布局不同；两个播放实验暂不支持此布局：帧 "+frame);
                    registry.materials.putAll(b.materials);b.groups.clear();frames.add(Json.of("value",frame,"parts",parts));
                    check(blob.size<=256L*1024*1024,"动画几何数据超过 256 MiB 预算");log.append("动画帧 ").append(frame).append("：写出完成，累计几何 ").append(blob.size/1024/1024).append(" MiB\n");
                }
                log.append("动画权重固定排列：").append(animation.stableWeights?"所有帧一致；动态模式不改写权重":"存在实际权重变化；请使用缓存帧网格模式").append('\n');
                weights.clear();List<Object> materials=emitMaterials(registry,decoder,blob);blob.out.flush();String generation;synchronized(ModelConverter.class){generation=System.currentTimeMillis()+"-"+(++generationCounter);}
                Map<String,Object> manifest=Json.of("format","gibox-model","version",4,"name",dest.getName(),"generation",generation,"character",Json.of("id",id,"names",new ArrayList<Object>(o.names)),"flags",Json.of("originalMaterials",false,"diffuseOnly",false),"animation",animation.json(frames),"parts",firstParts,"materials",materials,"blob",Json.of("file","model.bin","size",blob.size,"crc32",blob.crc.getValue()));
                if(o.sourcePath!=null)manifest.put("source",Json.of("path",o.sourcePath,"signature",o.sourceSignature==null?"":o.sourceSignature));
                write(tmp,"conversion.log",text(log.toString()));write(tmp,"model.json",text(Json.write(manifest)));
            }
            commit(tmp,dest,o.replace);log.append("完成：").append(dest).append('\n');return dest;
        }finally{if(tmp.exists())remove(tmp);}
    }
    public static final class Options {
        public String mapping="",fallback=null,sourcePath=null,sourceSignature=null;public File ini=null;
        /** The INI scheme to convert when the mod folder holds several model INIs. */
        public Options ini(File f){ini=f;return this;}public boolean variants=false;public boolean replace=false;public List<String> names=new ArrayList<>();
        public Options mapping(String m){mapping=m==null?"":m;return this;}public Options variants(boolean v){variants=v;return this;}public Options replace(boolean r){replace=r;return this;}
        /** Renderer for draws whose names match no renderer and share no IB hash with one that does (one-click import uses "Body"). */
        public Options fallback(String r){fallback=r;return this;}
        /** Where the mod was converted from (model.json "source"): its folder and ModLibrary.signature(). */
        public Options source(String path,String signature){sourcePath=path;sourceSignature=signature;return this;}
        /** Character name tokens (model.json "character.names") for a conversion without skeleton library; the runtime finds the character by its skeleton names
         *  (with an ID: the character's model name, so it is also found on the party and detail pages before it was on the field). */
        public Options names(List<String> n){names=new ArrayList<>(n);return this;}
    }
    private static int generationCounter=0;
    /** Converts one PC mod into one GIBox model folder. Variant combinations share the vertices; each has its own draws. */
    public static File convert(File src,File boneDir,File dest,int id,Options o,TextureDecoder decoder,StringBuilder log)throws Exception {
        Ini ini=new Ini(src,o.ini);List<Bones> bones;
        log.append("模型主 INI：").append(ini.file.getName()).append('\n');
        for(File sibling:ModConfig.allInis(src))if(!sibling.equals(ini.file))log.append("未导入辅助 INI：").append(src.toPath().relativize(sibling.toPath())).append("（帮助、菜单或独立贴图配置；不包含在本次模型转换中）\n");
        if(o.variants)for(Map.Entry<String,TreeSet<Integer>> e:ini.excludedChoices.entrySet())
            log.append("款式 ").append(e.getKey()).append(" 按按键列出的值和初始值取 ").append(ini.choices.get(e.getKey())).append("；仅在条件中出现的值不作为可选款式：").append(e.getValue()).append('\n');
        check(o.names.size()<=16,"角色名过多");for(String n:o.names)check(n.matches("[a-z0-9]{3,40}"),"角色名格式："+n);
        if(boneDir==null){check((id==0&&!o.names.isEmpty())||(id>=10000000&&id<=10001999),"没有骨骼目录时需要角色 ID 或角色名");bones=new ArrayList<>();for(String r:DEFAULT_RENDERERS)bones.add(new Bones(r));if(o.fallback==null)o.fallback="Body";}
        else{check(id>=10000000&&id<=10001999,"角色 ID 越界");bones=bones(boneDir,id);}Map<String,String> map=new LinkedHashMap<>();
        if(!o.mapping.trim().isEmpty())for(String row:o.mapping.split(";")){String[] a=row.trim().split("=",-1);check(a.length==2&&!a[0].trim().isEmpty()&&!a[1].trim().isEmpty(),"映射格式：部件=渲染器;部件=skip");check(map.put(lower(a[0].trim()),a[1].trim())==null,"重复映射");}
        Animation animation=Animation.parse(ini);
        if(animation!=null)return convertAnimation(ini,src,bones,map,dest,id,o,decoder,log,animation);
        // Variants: every assignment of the variables that an if of a drawing section reads (not the ones only key or
        // Present sections use), one signature each; the distinct signatures are built, and each draw keeps the values of
        // the variables it depends on.
        List<String> condVars=o.variants?conditionVars(ini):new ArrayList<>();
        List<List<String>> named=new ArrayList<>();for(List<Integer> gr:groupVars(ini,condVars)){List<String> n=new ArrayList<>();for(int v:gr)n.add(condVars.get(v));named.add(n);}
        Map<Map<String,Integer>,String> sigCache=new HashMap<>();final Map<String,String> fmap=map;final List<Bones> fbones=bones;
        Sig sigOf0=a->{String x=sigCache.get(a);if(x==null){x=signature(ini,src,fbones,fmap,a,o.fallback);sigCache.put(new LinkedHashMap<>(a),x);}return x;};
        Variation var;String[] sig;Random rnd=new Random(1);
        // Groups that turn out to affect each other (two of them changed together draw something else than each changed
        // alone predicts) are merged and checked again; a group too large to enumerate keeps its initial values.
        while(true){
            List<String> names=new ArrayList<>();List<List<Integer>> groups=new ArrayList<>();
            for(Iterator<List<String>> it=named.iterator();it.hasNext();){List<String> gr=it.next();long t=1;for(String v:gr)t*=ini.choices.get(v).size();
                if(t>MAX_ASSIGNMENTS){log.append("这几个款式变量互相关联，取值组合过多（").append(t).append(" 种，上限 ").append(MAX_ASSIGNMENTS).append("），只用初始值：").append(gr).append('\n');it.remove();continue;}
                List<Integer> idx=new ArrayList<>();for(String v:gr){idx.add(names.size());names.add(v);}groups.add(idx);}
            var=new Variation(ini,names,groups);sig=new String[var.runs.size()];for(int i=0;i<sig.length;i++)sig[i]=sigOf0.of(var.runs.get(i));
            if(groups.size()<2)break;
            int[] bad=null;int pairs=0;
            for(int a=0;a<groups.size()&&bad==null&&pairs<300;a++)for(int b=a+1;b<groups.size()&&bad==null&&pairs<300;b++,pairs++)
                {// small pairs: every combination; larger ones: 8 samples
                 int na=var.size(a),nb=var.size(b);boolean all=(long)na*nb<=16;
                 for(int t=0;t<(all?na*nb:8)&&bad==null;t++){Map<Integer,Integer> set=new LinkedHashMap<>();set.put(a,all?t/nb:rnd.nextInt(na));set.put(b,all?t%nb:rnd.nextInt(nb));if(!consistent(ini,var,sig,set,sigOf0))bad=new int[]{a,b};}}
            if(bad!=null){List<String> merged=new ArrayList<>(named.get(bad[0]));merged.addAll(named.get(bad[1]));named.remove(bad[1]);named.set(bad[0],merged);continue;}
            boolean ok=true;for(int t=0;t<16&&ok;t++){Map<Integer,Integer> set=new LinkedHashMap<>();for(int g=0;g<groups.size();g++)if(rnd.nextBoolean())set.put(g,rnd.nextInt(var.size(g)));ok=consistent(ini,var,sig,set,sigOf0);}
            if(!ok){log.append("款式变量之间相互影响（同时改几组时画出的内容和分别改时对不上），无法分组处理，只转换默认款式\n");named.clear();continue;}
            break;}
        List<Map<String,Integer>> kept=new ArrayList<>();Map<String,Integer> sigIndex=new LinkedHashMap<>();int[] sigOf=new int[var.runs.size()];
        for(int i=0;i<var.runs.size();i++){Integer c=sigIndex.get(sig[i]);if(c==null){c=kept.size();sigIndex.put(sig[i],c);kept.add(var.runs.get(i));}sigOf[i]=c;}
        dest=dest.getCanonicalFile();File parent=dest.getParentFile();check(parent.isDirectory()||parent.mkdirs(),"无法创建输出根目录");check(o.replace||!dest.exists(),"输出目录已存在："+dest);
        check(!dest.getName().isEmpty()&&dest.getName().getBytes(StandardCharsets.UTF_8).length<=255,"模型名过长："+dest.getName());
        File tmp=Files.createTempDirectory(parent.toPath(),".model-convert-").toFile();
        try{
            memoryBudget(ini,src,bones,map,kept,o.fallback,log);Build b=new Build();for(int c=0;c<kept.size();c++)collect(ini,src,bones,map,kept.get(c),c,b,log,o.fallback);
            check(!b.groups.isEmpty(),"所选 INI "+ini.file.getName()+" 没有可转换的模型绘制："+(o.variants?"已开启款式枚举；请检查是否为纯贴图配置、部件全部跳过或依赖未支持的外部绘制命令":"当前只转换默认款式；可在手动转换中开启‘转换全部款式’，并检查部件映射及外部绘制命令"));
            Set<String> shown=new LinkedHashSet<>();
            Object replaceWhen=null;
            try{for(Group g:b.groups.values())g.draws=drawTable(g,var,sigOf,kept.size(),shown);
                Group gate=new Group(null,"replacement");
                for(int r=0;r<sig.length;r++)if(sig[r].startsWith("replace\n")&&!gate.combos.containsKey(sigOf[r])){ComboRows row=new ComboRows();row.subs.add(new int[]{0,0});row.rows.add(new String[]{"enabled","0"});gate.combos.put(sigOf[r],row);}
                replaceWhen=Json.obj(drawTable(gate,var,sigOf,kept.size(),shown).get(0),"replacement").get("when");}
            catch(Entangled e){log.append("款式变量之间相互影响（有绘制段同时取决于几组变量），无法分组处理，只转换默认款式\n");shown.clear();
                check(sig[0].startsWith("replace\n"),"初始款式关闭替换，且其关联款式无法分组；不能把关闭状态转换成空模型");
                Variation none=new Variation(ini,new ArrayList<>(),new ArrayList<>());int[] only={0};for(Group g:b.groups.values()){Map<Integer,ComboRows> c=new HashMap<>();if(g.combos.containsKey(sigOf[0]))c.put(0,g.combos.get(sigOf[0]));g.combos.clear();g.combos.putAll(c);}
                for(Group g:b.groups.values())g.draws=g.combos.isEmpty()?null:drawTable(g,none,only,1,shown);}
            for(Group g:b.groups.values()){int trimmed=g.vertices.trimmed;if(trimmed>0)log.append(g.bones.name).append("：已清理 ").append(trimmed).append(" 个顶点中占比不超过 ").append(WEIGHT_EPSILON).append(" 的微小权重，并重新归一化\n");int fixed=fixUnweighted(g);if(fixed>0)log.append(g.bones.name).append("：").append(fixed).append(" 个顶点没有权重（MOD 本身的问题，PC 上这些点会被拉到原点），已沿用最近顶点的权重\n");}
            for(Group g:b.groups.values())log.append(g.bones.name).append(": ").append(g.vertices.size()).append(" 顶点，").append(g.indices.size()).append(" 索引").append(g.bones.synthetic?"（骨骼取自游戏内角色）":g.bones.target==null?"（骨骼库没有网格特征，首次在场景中出现时识别）":"").append('\n');
            for(String x:ini.external){if(nnFix(x))log.append("已按 NNFix 识别贴图：ps-t0 为颜色图，ps-t1 为材质遮罩，不使用法线图\n");else if(!texFxComponent(x))log.append("已忽略外部 CommandList：").append(x).append('\n');}ini.external.clear();
            for(String note:ini.compatibility)log.append(note).append('\n');
            Map<String,Object> data=emit(tmp,b,kept.size(),decoder);
            String generation;synchronized(ModelConverter.class){generation=System.currentTimeMillis()+"-"+(++generationCounter);}
            Map<String,Object> manifest=Json.of("format","gibox-model","version",2,"name",dest.getName(),"generation",generation,
                "character",Json.of("id",id,"names",new ArrayList<Object>(o.names)),"flags",Json.of("originalMaterials",false,"diffuseOnly",false));
            if(!shown.isEmpty()){
                List<Object> vars=new ArrayList<>();Map<String,Object> defaults=new LinkedHashMap<>();long product=1;
                for(String k:var.names){if(!shown.contains(k))continue;List<Integer> values=var.values.get(var.names.indexOf(k));for(int x:values)check(Math.abs(x)<=1000000,"款式变量值过大："+k);vars.add(Json.of("name",k,"values",new ArrayList<Object>(values)));defaults.put(k,ini.vars.get(k));product*=values.size();}
                check(vars.size()<=16,"款式变量过多");manifest.put("variants",Json.of("vars",vars,"default",defaults));
                log.append("款式变量：").append(shown).append("，").append(product).append(" 种取值组合（游戏内切换，无需重新转换）\n");}
            manifest.putAll(data);
            if(replaceWhen!=null){manifest.put("version",3);manifest.put("replaceWhen",replaceWhen);log.append("未启用几何替换的款式保留游戏原模型和材质；此模型需要支持格式 3 的 GIBox\n");}
            if(o.sourcePath!=null)manifest.put("source",Json.of("path",o.sourcePath,"signature",o.sourceSignature==null?"":o.sourceSignature));
            write(tmp,"conversion.log",text(log.toString()));
            // Written last: the runtime reloads a folder when model.json changes.
            write(tmp,"model.json",text(Json.write(manifest)));
            commit(tmp,dest,o.replace);log.append("完成：").append(dest).append('\n');return dest;
        }finally{if(tmp.exists())remove(tmp);}
    }
    /** Old entry point kept for callers that still pass the mapping and variant flag. */
    public static List<File> convert(File src,File boneDir,File dest,int id,String mapping,boolean variants,TextureDecoder decoder,StringBuilder log)throws Exception {
        return Collections.singletonList(convert(src,boneDir,dest,id,new Options().mapping(mapping).variants(variants),decoder,log));}
    /** Swap the finished folder in: readers see either the old or the new folder, never a half-written one. */
    private static void commit(File tmp,File dest,boolean replace)throws Exception{
        if(!dest.exists()){Files.move(tmp.toPath(),dest.toPath(),StandardCopyOption.ATOMIC_MOVE);return;}check(replace,"输出目录已存在："+dest);
        File old=new File(dest.getParentFile(),".old-"+dest.getName()+"-"+System.nanoTime());Files.move(dest.toPath(),old.toPath(),StandardCopyOption.ATOMIC_MOVE);
        try{Files.move(tmp.toPath(),dest.toPath(),StandardCopyOption.ATOMIC_MOVE);}catch(Exception e){Files.move(old.toPath(),dest.toPath(),StandardCopyOption.ATOMIC_MOVE);throw e;}
        // Keep what the runtime wrote (learned.json, bonemap-*.tsv), unless the folder now belongs to another character
        // (it was learned from the old one).
        File[] leftovers=sameCharacter(old,dest)?old.listFiles():null;
        if(leftovers!=null)for(File f:leftovers){String n=f.getName();if((n.equals("learned.json")||(n.startsWith("bonemap-")&&n.endsWith(".tsv")))&&!new File(dest,n).exists())try{Files.copy(f.toPath(),new File(dest,n).toPath());}catch(Exception ignored){}}
        remove(old);
    }
    /** Same character before and after a re-conversion: same ID and names in model.json. */
    static boolean sameCharacter(File before,File after){
        try{ModelManifest a=ModelManifest.read(before),b=ModelManifest.read(after);return a.id==b.id&&a.names.equals(b.names);}catch(Exception e){return false;}
    }
    static void remove(File f)throws IOException {if(f.isDirectory()){File[] a=f.listFiles();if(a!=null)for(File c:a)remove(c);}Files.deleteIfExists(f.toPath());}
}
