package com.gibox.tool;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

/**
 * INI/vertex-buffer converter. Output: one GIBox model folder
 * (format version 1, read by model.hpp): model.json (manifest) + model.bin (every array on a 16-byte boundary, CRC-32 in
 * the manifest) + conversion.log. The runtime adds learned.json and bonemap-<part>.tsv.
 */
public final class ModelConverter {
    public interface TextureDecoder { byte[] decode(byte[] dds) throws Exception; }
    private static void check(boolean ok,String message) { if(!ok) throw new IllegalArgumentException(message); }
    private static String lower(String s) { return s.toLowerCase(Locale.ROOT); }
    private static String clean(String s) { int p=s.indexOf(';'); return (p<0?s:s.substring(0,p)).trim(); }
    private static byte[] read(File f) throws Exception { check(f.length()<=256L*1024*1024,"文件过大："+f); return Files.readAllBytes(f.toPath()); }
    private static ByteBuffer buf(byte[] a) { return ByteBuffer.wrap(a).order(ByteOrder.LITTLE_ENDIAN); }
    private static byte[] text(String s) { return s.getBytes(StandardCharsets.UTF_8); }
    private static void write(File d,String n,byte[] b)throws Exception { Files.write(new File(d,n).toPath(),b); }
    private static String safe(String s) { check(s.matches("[A-Za-z0-9_-]+"),"不支持的部件文件名："+s);return s; }
    private static File inside(File root,String path)throws Exception { path=path.replace('\\','/').replace("\"","");File f=new File(root,path).getCanonicalFile();String r=root.getCanonicalPath()+File.separator;check(f.getPath().startsWith(r),"资源超出 MOD 目录："+path);return f; }

    static final class Section { final String name;final List<String> lines=new ArrayList<>(); Section(String n){name=n;} }
    static final class Ini {
        final LinkedHashMap<String,Section> sections=new LinkedHashMap<>();
        final LinkedHashMap<String,Integer> vars=new LinkedHashMap<>();
        final LinkedHashMap<String,TreeSet<Integer>> choices=new LinkedHashMap<>();
        Ini(File root)throws Exception {
            File[] files=root.listFiles(f->f.isFile()&&lower(f.getName()).endsWith(".ini"));check(files!=null&&files.length==1,"MOD 根目录必须有且只有一个 INI");
            Section current=null;for(String raw:new String(read(files[0]),StandardCharsets.UTF_8).replace("\uFEFF","").split("\\r?\\n")) {
                String l=clean(raw);if(l.isEmpty())continue;
                if(l.startsWith("[")&&l.endsWith("]")){String n=l.substring(1,l.length()-1).trim();check(!sections.containsKey(lower(n)),"重复 INI 段："+n);current=new Section(n);sections.put(lower(n),current);}
                else if(current!=null)current.lines.add(l);
            }
            Pattern decl=Pattern.compile("(?i)^(?:global\\s+)?(?:persist\\s+)?(\\$[a-z_][a-z0-9_]*)(?:\\s*=\\s*(-?\\d+))?\\s*$");
            Pattern cmp=Pattern.compile("(\\$[A-Za-z_][A-Za-z0-9_]*)\\s*(?:==|!=|<=|>=|<|>)\\s*(-?\\d+)");
            for(Section s:sections.values())for(String l:s.lines){Matcher m=decl.matcher(l);if(m.matches()&&(lower(s.name).equals("constants")||lower(s.name).equals("persist"))){String v=lower(m.group(1));int n=m.group(2)==null?0:Integer.parseInt(m.group(2));vars.put(v,n);choices.computeIfAbsent(v,k->new TreeSet<>()).add(n);}Matcher c=cmp.matcher(l);while(c.find())choices.computeIfAbsent(lower(c.group(1)),k->new TreeSet<>()).add(Integer.parseInt(c.group(2)));
                if(lower(s.name).startsWith("key")){int eq=l.indexOf('=');if(eq>0&&l.substring(0,eq).trim().startsWith("$")){String v=lower(l.substring(0,eq).trim());for(String a:l.substring(eq+1).split(","))if(a.trim().matches("-?\\d+"))choices.computeIfAbsent(v,k->new TreeSet<>()).add(Integer.parseInt(a.trim()));}}
            }
            for(String v:choices.keySet())check(vars.containsKey(v),"条件变量缺少初始值："+v);
        }
        Section section(String n) { Section s=sections.get(lower(n));check(s!=null,"缺少段："+n);return s; }
        final LinkedHashSet<String> external=new LinkedHashSet<>();
        /** run = targets defined outside this INI only patch shaders/textures at draw time; they cannot bind this mod's buffers. */
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
        long compare(){long a=add();while(true){if(take("=="))a=a==add()?1:0;else if(take("!="))a=a!=add()?1:0;else if(take("<="))a=a<=add()?1:0;else if(take(">="))a=a>=add()?1:0;else if(take("<"))a=a<add()?1:0;else if(take(">"))a=a>add()?1:0;else return a;}}
        long add(){long a=mul();while(true){if(take("+"))a+=mul();else if(take("-"))a-=mul();else return a;}}
        long mul(){long a=atom();while(true){if(take("*"))a*=atom();else if(take("/")){long b=atom();check(b!=0,"除零");a/=b;}else if(take("%")){long b=atom();check(b!=0,"除零");a%=b;}else return a;}}
        long atom(){if(take("!"))return atom()==0?1:0;if(take("-"))return -atom();if(take("+"))return atom();if(take("(")){long v=or();check(take(")"),"缺少右括号");return v;}ws();int start=p;
            if(p<s.length()&&s.charAt(p)=='$'){p++;while(p<s.length()&&(Character.isLetterOrDigit(s.charAt(p))||s.charAt(p)=='_'))p++;String k=s.substring(start,p);check(vars.containsKey(k),"未知变量："+k);return vars.get(k);}
            while(p<s.length()&&Character.isDigit(s.charAt(p)))p++;check(p>start,"不支持的表达式："+s);return Long.parseLong(s.substring(start,p));}
    }
    static final class Resource {File file;int stride;String format;Resource(File f,int s,String t){file=f;stride=s;format=t;} }
    static final class Draw {String part,hash="";Map<String,String> bindings;int count,start;Draw(String p,Map<String,String>b,int c,int s){part=p;bindings=new LinkedHashMap<>(b);count=c;start=s;} }
    private static String[] kv(String l){int p=l.indexOf('=');return p<0?null:new String[]{lower(l.substring(0,p).trim()),l.substring(p+1).trim()};}
    private static boolean binding(String k){return k.equals("ib")||k.matches("vb[0-9]+")||k.matches("ps-t[0-9]+");}
    private static boolean unbound(String name){return name==null||name.equalsIgnoreCase("null");}
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
            else if(k.equals("run")){Section target=ini.run(v);if(target!=null)commands(ini,target,vars,rs,bindings,draws,part,depth+1);}
            else if(k.equals("drawindexed")){String[] f=v.split(",");check(f.length<=3,"drawindexed 参数");int count=f[0].trim().equalsIgnoreCase("auto")?-1:Integer.parseInt(f[0].trim());int start=f.length>1?Integer.parseInt(f[1].trim()):0;check(f.length<3||Integer.parseInt(f[2].trim())==0,"仅支持 baseVertex=0");check(count==-1||count>0,"drawindexed 数量");check(start>=0,"drawindexed 起始");draws.add(new Draw(part,bindings,count,start));}
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
        for(String l:s.lines){String[] a=kv(l);if(a==null)continue;if(a[0].equals("drawindexed"))return true;if(a[0].equals("run")){Section target=ini.run(a[1]);if(target!=null&&declaresDraw(ini,target,visited))return true;}}
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
    static final class Bones {String name;Map<String,Object> target;List<String> names;byte[] binds;int count;boolean synthetic;
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
            List<Object> parents=Json.arr(x.get("parents"),n+".parents",names.size(),names.size());for(Object p:parents)Json.integer(p,n+".parents",-1,names.size()-1);
            ByteBuffer binds=buf(new byte[64*names.size()]);for(Object v:Json.arr(x.get("bindposes"),n+".bindposes",16*names.size(),16*names.size()))binds.putFloat(Json.finite(v,n+".bindposes"));
            Map<String,Object> target=fingerprint(x.get("target"));
            check(target==null||!ModelManifest.lowerLod((String)target.get("mesh")),"骨骼记录里 "+n+" 是低精度模型 "+(target==null?"":target.get("mesh"))+"（角色刚加载时记下的），骨骼比完整模型少。让角色在大世界出场一次（骨骼库会自动更新）或靠近角色重新导出后再转换");
            out.add(new Bones(n,names,binds.array(),target));
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
    static final class Vertex {byte[] pos,norm,uv,tangent,color,weights;List<byte[]> extra=new ArrayList<>();boolean unweighted;}
    /** Submeshes and material rows of one renderer part in one variant combination. */
    static final class ComboRows {final List<int[]> subs=new ArrayList<>();final List<String[]> rows=new ArrayList<>();String key(){StringBuilder s=new StringBuilder();for(int i=0;i<subs.size();i++)s.append(subs.get(i)[0]).append(',').append(subs.get(i)[1]).append(':').append(rows.get(i)[0]).append('#').append(rows.get(i)[1]).append(';');return s.toString();}}
    /** One renderer: union of every combination's vertices and index ranges, plus per-combination submeshes. */
    static final class Group {Bones bones;String name;List<Vertex> vertices=new ArrayList<>();List<Integer> indices=new ArrayList<>();Map<String,Integer> compact=new LinkedHashMap<>();Map<String,int[]> ranges=new HashMap<>();Map<Integer,ComboRows> combos=new HashMap<>();Group(Bones b,String n){bones=b;name=n;} }
    static final class MaterialDef {String name;Resource diffuse,light,normal;}
    static final class Build {final LinkedHashMap<String,Group> groups=new LinkedHashMap<>();final LinkedHashMap<String,MaterialDef> materials=new LinkedHashMap<>();final Map<File,byte[]> cache=new HashMap<>();}
    private static Vertex vertex(Resource position,Resource blend,Resource tex,byte[] p,byte[] b,byte[] t,int i,int boneCount){
        check(position.stride>=40&&blend.stride==32&&tex.stride>=12&&tex.stride<=36&&(tex.stride-4)%8==0,"顶点流 stride 不兼容");check(i>=0&&(long)(i+1)*position.stride<=p.length&&(long)(i+1)*32<=b.length&&(long)(i+1)*tex.stride<=t.length,"顶点索引越界");int po=i*position.stride,bo=i*32,to=i*tex.stride;Vertex v=new Vertex();v.pos=Arrays.copyOfRange(p,po,po+12);v.norm=Arrays.copyOfRange(p,po+12,po+24);v.tangent=Arrays.copyOfRange(p,po+24,po+40);v.color=Arrays.copyOfRange(t,to,to+4);v.uv=uv(t,to+4);for(int k=12;k+8<=tex.stride;k+=8)v.extra.add(uv(t,to+k));v.weights=new byte[32];ByteBuffer bi=buf(b),wo=buf(v.weights);float sum=0;float[] w=new float[4];int[] ids=new int[4];for(int k=0;k<4;k++){w[k]=bi.getFloat(bo+4*k);ids[k]=bi.getInt(bo+16+4*k);check(Float.isFinite(w[k])&&w[k]>=0,"非法权重");check(w[k]==0||(ids[k]>=0&&ids[k]<boneCount),"骨骼索引越界");sum+=w[k];}check(Float.isFinite(sum),"非法权重");if(sum>0){for(int k=0;k<4;k++){wo.putFloat(4*k,w[k]/sum);wo.putInt(16+4*k,w[k]>0?ids[k]:0);}}else v.unweighted=true;for(byte[] channel:Arrays.asList(v.pos,v.norm,v.tangent)){ByteBuffer c=buf(channel);for(int k=0;k<channel.length;k+=4)check(Float.isFinite(c.getFloat(k)),"非法几何数值");}return v;
    }
    private static byte[] uv(byte[] t,int p){ByteBuffer b=buf(t);float u=b.getFloat(p),v=b.getFloat(p+4);check(Float.isFinite(u)&&Float.isFinite(v),"非法 UV");return buf(new byte[8]).putFloat(u).putFloat(1-v).array();}
    /** A DDS texture as RGBA8, at most 1024 on a side, rows bottom to top: {width, height, data view}. */
    private static Map<String,Object> texture(File f,TextureDecoder decoder,BlobWriter blob,Map<File,Map<String,Object>> done)throws Exception {
        Map<String,Object> known=done.get(f);if(known!=null)return known;
        check(lower(f.getName()).endsWith(".dds"),"当前转换器支持 DDS 纹理："+f);byte[] raw=decoder.decode(read(f));check(raw.length>=8,"纹理解码返回值无效");ByteBuffer h=buf(raw);int w=h.getInt(0),y=h.getInt(4);check(w>0&&y>0&&w<=8192&&y<=8192&&raw.length==8+4L*w*y,"纹理解码尺寸无效");byte[] pixels=Arrays.copyOfRange(raw,8,raw.length);
        while(w>1024||y>1024){int nw=Math.max(1,w/2),nh=Math.max(1,y/2);byte[] n=new byte[nw*nh*4];for(int j=0;j<nh;j++)for(int i=0;i<nw;i++)for(int c=0;c<4;c++){int sum=0,count=0;for(int dy=0;dy<2&&j*2+dy<y;dy++)for(int dx=0;dx<2&&i*2+dx<w;dx++){sum+=pixels[((j*2+dy)*w+i*2+dx)*4+c]&255;count++;}n[(j*nw+i)*4+c]=(byte)(sum/count);}pixels=n;w=nw;y=nh;}
        byte[] rows=new byte[pixels.length];for(int j=0;j<y;j++)System.arraycopy(pixels,(y-1-j)*w*4,rows,j*w*4,w*4);
        Map<String,Object> t=Json.of("width",w,"height",y,"data",blob.put(rows));done.put(f,t);return t;
    }
    /** Draws of one variable assignment, each resolved to its target renderer. */
    private static LinkedHashMap<Draw,Bones> resolve(Ini ini,File src,List<Bones> boneRecords,Map<String,String> mapping,Map<String,Integer> vars,Map<String,Resource> rs,StringBuilder log){return resolve(ini,src,boneRecords,mapping,vars,rs,log,null);}
    private static LinkedHashMap<Draw,Bones> resolve(Ini ini,File src,List<Bones> boneRecords,Map<String,String> mapping,Map<String,Integer> vars,Map<String,Resource> rs,StringBuilder log,String fallback){
        List<Draw> draws=drawCalls(ini,vars,rs,mapping);check(!draws.isEmpty(),"没有有效 drawindexed");
        LinkedHashMap<Draw,Bones> targets=new LinkedHashMap<>();Map<String,Bones> byHash=new HashMap<>();List<Draw> pending=new ArrayList<>();
        for(Draw d:draws){if("skip".equalsIgnoreCase(mapping.get(lower(d.part))))continue;try{Bones b=target(d.part,boneRecords,mapping);targets.put(d,b);if(!d.hash.isEmpty()){Bones prev=byHash.putIfAbsent(d.hash,b);check(prev==null||prev==b,"同一 IB hash 的部件映射到了不同渲染器："+d.hash);}}catch(IllegalArgumentException e){if(d.hash.isEmpty()||mapping.containsKey(lower(d.part)))throw e;pending.add(d);}}
        Bones fallbackBones=null;if(fallback!=null)for(Bones b:boneRecords)if(b.name.equalsIgnoreCase(fallback))fallbackBones=b;
        for(Draw d:pending){Bones b=byHash.get(d.hash);if(b==null&&fallbackBones!=null){b=fallbackBones;byHash.put(d.hash,b);if(log!=null)log.append(d.part).append(" -> ").append(b.name).append("（未匹配到渲染器名，按默认渲染器）\n");targets.put(d,b);continue;}check(b!=null,"请手工映射部件："+d.part+"（同一 hash 的其他部件也没有匹配到渲染器）");targets.put(d,b);if(log!=null)log.append(d.part).append(" -> ").append(b.name).append("（按同一 IB hash 推定）\n");}
        LinkedHashMap<Draw,Bones> ordered=new LinkedHashMap<>();for(Draw d:draws)if(targets.containsKey(d))ordered.put(d,targets.get(d));check(!ordered.isEmpty(),"全部部件均已跳过");return ordered;
    }
    private static Resource[] textures(Draw d,Map<String,Resource> rs){Resource diffuse=null,light=null,normal=null;
        for(Map.Entry<String,String> binding:d.bindings.entrySet())if(binding.getKey().startsWith("ps-t")&&!unbound(binding.getValue())){Resource r=resource(rs,binding.getValue());String file=lower(r.file.getName());if(file.contains("lightmap"))light=r;else if(file.contains("normalmap")||file.contains("normal"))normal=r;else if(file.contains("diffuse"))diffuse=r;}
        return new Resource[]{diffuse,light,normal};}
    private static String path(Resource r){return r==null?"-":r.file.getPath();}
    private static int slot(Draw d,int s){char last=d.part.charAt(d.part.length()-1);int slot=last>='A'&&last<='Z'?last-'A':s;check(slot<64,"材质槽越界");return slot;}
    /** What a combination draws, without reading buffers: used to find the variables that actually change the model. */
    private static String signature(Ini ini,File src,List<Bones> boneRecords,Map<String,String> mapping,Map<String,Integer> vars,String fallback)throws Exception{
        Map<String,Resource> rs=resources(ini,src,vars);StringBuilder s=new StringBuilder();
        for(Map.Entry<Draw,Bones> e:resolve(ini,src,boneRecords,mapping,vars,rs,null,fallback).entrySet()){Draw d=e.getKey();Resource[] t=textures(d,rs);
            s.append(e.getValue().name).append('|').append(path(stream(rs,d.bindings,0))).append('|').append(path(stream(rs,d.bindings,1))).append('|').append(path(stream(rs,d.bindings,2))).append('|').append(path(resource(rs,d.bindings.get("ib")))).append('|').append(d.start).append('|').append(d.count).append('|').append(path(t[0])).append(path(t[1])).append(path(t[2])).append('\n');}
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
            if(range==null){int start=g.indices.size();ByteBuffer ib=buf(ix);for(int j=0;j<count;j++){int old=stride==2?(ib.getShort((d.start+j)*2)&65535):ib.getInt((d.start+j)*4);String key=pos.file.getPath()+"|"+blend.file+"|"+tex.file+"|"+old;Integer compact=g.compact.get(key);if(compact==null){compact=g.vertices.size();check(compact<500000,"顶点数超限");g.compact.put(key,compact);g.vertices.add(vertex(pos,blend,tex,p,bl,t,old,target.count));}g.indices.add(compact);}range=new int[]{start,count};g.ranges.put(rangeKey,range);}
            ComboRows rows=g.combos.computeIfAbsent(combo,k->new ComboRows());Resource[] tx=textures(d,rs);String materialKey=path(tx[0])+"|"+path(tx[1])+"|"+path(tx[2]);
            MaterialDef m=b.materials.get(materialKey);if(m==null){m=new MaterialDef();m.name="Material"+b.materials.size();m.diffuse=tx[0];m.light=tx[1];m.normal=tx[2];b.materials.put(materialKey,m);}
            rows.rows.add(new String[]{m.name,Integer.toString(slot(d,rows.subs.size()))});rows.subs.add(range);
        }
    }
    /**
     * Vertices the mod author left without any bone weight (all four 0). On PC such a vertex is skinned by nothing and
     * collapses to the origin (a spike towards the feet); here it takes the weights of the nearest weighted vertex of the
     * same part, which is where it was meant to move with. A part with no weighted vertex at all is refused.
     */
    static int fixUnweighted(Group g){
        List<Integer> bad=new ArrayList<>(),good=new ArrayList<>();for(int i=0;i<g.vertices.size();i++)(g.vertices.get(i).unweighted?bad:good).add(i);
        if(bad.isEmpty())return 0;
        check(!good.isEmpty(),g.bones.name+"：所有顶点的权重都是 0（MOD 没有绑定骨骼），无法转换");
        check(bad.size()<=20000,g.bones.name+"：有 "+bad.size()+" 个顶点没有权重（MOD 本身的问题），太多了，无法自动修补");
        float[] gp=new float[good.size()*3];for(int j=0;j<good.size();j++){ByteBuffer q=buf(g.vertices.get(good.get(j)).pos);for(int k=0;k<3;k++)gp[j*3+k]=q.getFloat(4*k);}
        for(int i:bad){Vertex v=g.vertices.get(i);ByteBuffer q=buf(v.pos);float x=q.getFloat(0),y=q.getFloat(4),z=q.getFloat(8);int best=0;double bd=Double.MAX_VALUE;
            for(int j=0;j<good.size();j++){double dx=gp[j*3]-x,dy=gp[j*3+1]-y,dz=gp[j*3+2]-z,d=dx*dx+dy*dy+dz*dz;if(d<bd){bd=d;best=j;}}
            v.weights=g.vertices.get(good.get(best)).weights.clone();v.unweighted=false;}
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
    private interface Field {byte[] of(Vertex v);}
    private static byte[] column(List<Vertex> vs,int stride,Field f){ByteBuffer b=buf(new byte[vs.size()*stride]);for(Vertex v:vs)b.put(f.of(v));return b.array();}
    /** Parts and materials into model.bin; returns the manifest's parts, materials and blob entries. */
    private static Map<String,Object> emit(File out,Build b,int combos,TextureDecoder decode)throws Exception {
        List<Object> parts=new ArrayList<>(),materials=new ArrayList<>();Map<File,Map<String,Object>> textures=new HashMap<>();
        try(BlobWriter blob=new BlobWriter(new File(out,"model.bin"))){
            for(Group g:b.groups.values()){
                int n=g.vertices.size(),ni=g.indices.size();
                ComboRows any=null;for(int c=0;c<combos&&any==null;c++)any=g.combos.get(c);
                // A part absent from a combination is hidden by one empty draw.
                ComboRows hidden=new ComboRows();hidden.subs.add(new int[]{0,0});hidden.rows.add(new String[]{any.rows.get(0)[0],"0"});
                check(n>0&&n<=500000&&ni>0&&ni<=15000000&&ni%3==0,"部件尺寸越界");for(ComboRows c:g.combos.values())check(c.subs.size()<=64,"子网格超过 64");
                int bones=g.bones.count;if(g.bones.synthetic){bones=1;for(Vertex v:g.vertices){ByteBuffer w=buf(v.weights);for(int k=0;k<4;k++)if(w.getFloat(4*k)>0)bones=Math.max(bones,w.getInt(16+4*k)+1);}}
                Map<String,Object> data=new LinkedHashMap<>();
                data.put("positions",blob.put(column(g.vertices,12,v->v.pos)));data.put("normals",blob.put(column(g.vertices,12,v->v.norm)));data.put("uv",blob.put(column(g.vertices,8,v->v.uv)));
                ByteBuffer ix=buf(new byte[4*ni]);for(int i:g.indices)ix.putInt(i);data.put("indices",blob.put(ix.array()));
                data.put("weights",blob.put(column(g.vertices,32,v->v.weights)));
                data.put("colors",blob.put(column(g.vertices,4,v->v.color)));data.put("tangents",blob.put(column(g.vertices,16,v->v.tangent)));
                for(int e=0;e<3;e++){final int extra=e;boolean present=true;for(Vertex v:g.vertices)present&=v.extra.size()>extra;if(present)data.put("uv"+(e+2),blob.put(column(g.vertices,8,v->v.extra.get(extra))));}
                Map<String,Object> part=Json.of("name",g.name,"renderer",g.bones.name,"bones",bones,"vertices",n,"indices",ni,"data",data);
                if(!g.bones.synthetic)part.put("skeleton",Json.of("bones",g.bones.names,"bindposes",blob.put(g.bones.binds)));
                if(g.bones.target!=null)part.put("target",g.bones.target);
                List<Object> draws=new ArrayList<>();
                for(int c=0;c<combos;c++){ComboRows cr=g.combos.getOrDefault(c,hidden);List<Object> row=new ArrayList<>();
                    for(int i=0;i<cr.subs.size();i++)row.add(Json.of("start",cr.subs.get(i)[0],"count",cr.subs.get(i)[1],"material",cr.rows.get(i)[0],"slot",Integer.parseInt(cr.rows.get(i)[1])));draws.add(row);}
                part.put("draws",draws);parts.add(part);
            }
            for(MaterialDef m:b.materials.values()){Map<String,Object> e=Json.of("name",m.name);
                if(m.diffuse==null)e.put("original",true);
                else{e.put("main",texture(m.diffuse.file,decode,blob,textures));if(m.light!=null)e.put("lightmap",texture(m.light.file,decode,blob,textures));if(m.normal!=null)e.put("normalmap",texture(m.normal.file,decode,blob,textures));}
                materials.add(e);}
            blob.out.flush();
            return Json.of("blob",Json.of("file","model.bin","size",blob.size,"crc32",blob.crc.getValue()),"parts",parts,"materials",materials);
        }
    }
    public static final class Options {
        public String mapping="",fallback=null,sourcePath=null,sourceSignature=null;public boolean variants=false;public boolean replace=false;public List<String> names=new ArrayList<>();
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
        Ini ini=new Ini(src);List<Bones> bones;
        check(o.names.size()<=16,"角色名过多");for(String n:o.names)check(n.matches("[a-z0-9]{3,40}"),"角色名格式："+n);
        if(boneDir==null){check((id==0&&!o.names.isEmpty())||(id>=10000000&&id<=10001999),"没有骨骼目录时需要角色 ID 或角色名");bones=new ArrayList<>();for(String r:DEFAULT_RENDERERS)bones.add(new Bones(r));if(o.fallback==null)o.fallback="Body";}
        else{check(id>=10000000&&id<=10001999,"角色 ID 越界");bones=bones(boneDir,id);}Map<String,String> map=new LinkedHashMap<>();
        if(!o.mapping.trim().isEmpty())for(String row:o.mapping.split(";")){String[] a=row.trim().split("=",-1);check(a.length==2&&!a[0].trim().isEmpty()&&!a[1].trim().isEmpty(),"映射格式：部件=渲染器;部件=skip");check(map.put(lower(a[0].trim()),a[1].trim())==null,"重复映射");}
        // Variant combinations: enumerate, keep only variables that change what is drawn, default values first.
        List<Map<String,Integer>> all;try{all=ini.variants(o.variants);}catch(IllegalArgumentException e){if(!o.variants)throw e;log.append("款式组合过多，只转换默认款式：").append(e.getMessage()).append('\n');all=ini.variants(false);}
        Map<String,Integer> defaults=new LinkedHashMap<>(ini.vars);List<String> sig=new ArrayList<>();for(Map<String,Integer> v:all)sig.add(signature(ini,src,bones,map,v,o.fallback));
        List<String> relevant=new ArrayList<>();for(String k:ini.vars.keySet()){boolean changes=false;for(int i=0;i<all.size()&&!changes;i++)for(int j=i+1;j<all.size()&&!changes;j++){Map<String,Integer> a=new HashMap<>(all.get(i)),b=new HashMap<>(all.get(j));if(Objects.equals(a.get(k),b.get(k)))continue;a.remove(k);b.remove(k);if(a.equals(b)&&!sig.get(i).equals(sig.get(j)))changes=true;}if(changes)relevant.add(k);}
        List<Map<String,Integer>> kept=new ArrayList<>();Set<Map<String,Integer>> seen=new HashSet<>();
        for(Map<String,Integer> v:all){Map<String,Integer> p=new LinkedHashMap<>();for(String k:relevant)p.put(k,v.get(k));boolean isDefault=true;for(String k:relevant)isDefault&=Objects.equals(v.get(k),defaults.get(k));if(isDefault&&seen.add(p))kept.add(0,v);}
        for(Map<String,Integer> v:all){Map<String,Integer> p=new LinkedHashMap<>();for(String k:relevant)p.put(k,v.get(k));if(seen.add(p))kept.add(v);}
        if(kept.isEmpty())kept.add(defaults);
        dest=dest.getCanonicalFile();File parent=dest.getParentFile();check(parent.isDirectory()||parent.mkdirs(),"无法创建输出根目录");check(o.replace||!dest.exists(),"输出目录已存在："+dest);
        check(!dest.getName().isEmpty()&&dest.getName().getBytes(StandardCharsets.UTF_8).length<=255,"模型名过长："+dest.getName());
        File tmp=Files.createTempDirectory(parent.toPath(),".model-convert-").toFile();
        try{
            Build b=new Build();for(int c=0;c<kept.size();c++)collect(ini,src,bones,map,kept.get(c),c,b,log,o.fallback);
            for(Group g:b.groups.values()){int fixed=fixUnweighted(g);if(fixed>0)log.append(g.bones.name).append("：").append(fixed).append(" 个顶点没有权重（MOD 本身的问题，PC 上这些点会被拉到原点），已沿用最近顶点的权重\n");}
            for(Group g:b.groups.values())log.append(g.bones.name).append(": ").append(g.vertices.size()).append(" 顶点，").append(g.indices.size()).append(" 索引").append(g.bones.synthetic?"（骨骼取自游戏内角色）":g.bones.target==null?"（骨骼库没有网格特征，首次在场景中出现时识别）":"").append('\n');
            for(String x:ini.external)log.append("已忽略外部 CommandList：").append(x).append('\n');ini.external.clear();
            Map<String,Object> data=emit(tmp,b,kept.size(),decoder);
            String generation;synchronized(ModelConverter.class){generation=System.currentTimeMillis()+"-"+(++generationCounter);}
            Map<String,Object> manifest=Json.of("format","gibox-model","version",1,"name",dest.getName(),"generation",generation,
                "character",Json.of("id",id,"names",new ArrayList<Object>(o.names)),"flags",Json.of("originalMaterials",false,"diffuseOnly",false));
            if(kept.size()>1){
                List<Object> vars=new ArrayList<>();for(String k:relevant){TreeSet<Integer> values=new TreeSet<>();for(Map<String,Integer> m:kept)values.add(m.get(k));for(int x:values)check(Math.abs(x)<=1000000,"款式变量值过大："+k);vars.add(Json.of("name",k,"values",new ArrayList<Object>(values)));}
                check(vars.size()<=16,"款式变量过多");List<Object> combos=new ArrayList<>();for(Map<String,Integer> m:kept){Map<String,Object> c=new LinkedHashMap<>();for(String k:relevant)c.put(k,m.get(k));combos.add(c);}
                manifest.put("variants",Json.of("vars",vars,"combos",combos,"default",0));
                log.append("款式变量：").append(relevant).append("，").append(kept.size()).append(" 种组合（游戏内切换，无需重新转换）\n");}
            manifest.putAll(data);
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
