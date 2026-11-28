package com.gibox.tool;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

final class SceneModels {
    static final class Selection {
        final List<Object> renderers=new ArrayList<>(), targets=new ArrayList<>();
        final Map<String,String> hashes=new TreeMap<>();
    }
    private static boolean owner(String s) { return s.startsWith("npc_")||s.startsWith("monster_")||s.startsWith("monsterobj_")||s.startsWith("flycloak_"); }
    private static boolean weapon(String s) { return s.startsWith("Equip_")||s.startsWith("MonEquip_"); }
    private static String otherMesh(String hash) {
        switch(hash) {
            case "708a8fef": return "LoginScene_Door01_Vo";
            case "ddcd0f8b": return "Skiff_Normal_01_Model_Lod0";
            case "bf4d6430": return "Skiff_Normal_01_Model_Lod1";
            case "1ef85754": return "Skiff_Normal_01_Model_Lod2";
            default: return "";
        }
    }
    static boolean otherHashes(Collection<String> hashes) { return hashes.stream().anyMatch(h -> !otherMesh(h).isEmpty()); }
    static boolean loginDoor(Map<String,Object> target) {
        return target!=null&&"LoginScene_Door01_Vo".equals(target.get("mesh"))&&target.get("bones") instanceof Number&&((Number)target.get("bones")).intValue()==13;
    }
    static boolean otherTargets(List<Object> targets) {
        return targets.stream().allMatch(row -> {
            Object mesh=Json.obj(Json.obj(row,"target row").get("target"),"target").get("mesh");
            return "LoginScene_Door01_Vo".equals(mesh)||String.valueOf(mesh).matches("Skiff_Normal_01_Model_Lod[012]");
        });
    }
    private static String shape(Map<String,Object> m) { return m.get("mesh")+"|"+m.get("vertices"); }
    static Selection select(File shared, Collection<String> wanted) throws Exception {
        OfflineSkeletons table=OfflineSkeletons.load(shared);
        Json.check(table!=null,"内置骨骼表尚未安装，请稍后重新导入");
        Set<String> hs=new TreeSet<>();for(String h:wanted)hs.add(h.toLowerCase(Locale.ROOT));
        Map<String,Map<String,Object>> found=new TreeMap<>();
        for(String filename:new String[]{"device-hashes.json","hash-map.json"}) {
            File file=new File(shared,filename);if(!file.isFile())continue;
            Map<String,Object> map=Json.obj(Json.read(file,64L<<20),filename);
            for(Object raw:Json.arr(map.get("meshes"),"meshes",0,262144)) {
                Map<String,Object> m=Json.obj(raw,"mesh");String h=String.valueOf(m.get("ib")).toLowerCase(Locale.ROOT);
                if(hs.contains(h)&&(!h.equals("708a8fef")||!loginDoor(found.get(h))))found.put(h,m);
            }
        }
        Json.check(!found.isEmpty(),"规则的索引缓冲 hash 不在内置表中，请先采集目标网格或补充对应游戏版本的数据");
        Set<String> shapes=new HashSet<>();for(Map<String,Object> e:found.values())shapes.add(shape(e));
        Map<Integer,Map<String,Object>> rigs=new TreeMap<>();
        for(int i=0;i<table.meshCount();i++){Map<String,Object> r=table.mesh(i);if(shapes.contains(shape(r)))rigs.put(i,r);}
        Map<Integer,Set<String>> owners=new HashMap<>();
        for(Map.Entry<String,List<Object[]>> p:table.prefabRecords().entrySet())for(Object[] ref:p.getValue()) {
            int i=(Integer)ref[1];Map<String,Object> r=rigs.get(i);if(r==null)continue;
            if(owner(p.getKey()))owners.computeIfAbsent(i,k->new TreeSet<>()).add(p.getKey());
            if(weapon(String.valueOf(r.get("mesh"))))owners.computeIfAbsent(i,k->new TreeSet<>()).add("@renderer:"+(String)ref[0]);
        }
        Selection out=new Selection();Map<String,String> aliases=new HashMap<>();
        for(Map.Entry<String,Map<String,Object>> entry:found.entrySet()) {
            Map<String,Object> hash=entry.getValue();Map<String,Object> chosen=null;Set<String> allowed=new TreeSet<>();
            List<Object> subs=Json.arr(hash.get("submeshes"),"submeshes",1,64);
            if(entry.getKey().equals("708a8fef")&&loginDoor(hash)) {
                long crc=Json.integer(hash.get("bindposeCrc32"),"天空门绑定矩阵 CRC32",1,0xffffffffL);
                String alias="Scene"+out.renderers.size();
                Map<String,Object> target=Json.of("renderer",hash.get("renderer"),"mesh",hash.get("mesh"),"vertices",hash.get("vertices"),"submeshes",subs.size(),"bones",13);
                out.renderers.add(Json.of("name",alias,"target",target,"indexOrder",true));
                out.targets.add(Json.of("target",target,"owners",Arrays.asList("@renderer:"+hash.get("renderer")),"bindposeCrc32",crc));
                out.hashes.put(entry.getKey(),alias);
                continue;
            }
            for(Map.Entry<Integer,Map<String,Object>> candidate:rigs.entrySet()) {
                Map<String,Object> r=candidate.getValue();if(!shape(r).equals(shape(hash))||!owners.containsKey(candidate.getKey()))continue;
                if(r.get("submeshes") instanceof Number&&((Number)r.get("submeshes")).intValue()!=subs.size())continue;
                if(chosen!=null)Json.check(chosen.get("bones").equals(r.get("bones"))&&chosen.get("parents").equals(r.get("parents"))&&chosen.get("bindposes").equals(r.get("bindposes")),"hash "+entry.getKey()+" 对应多套不同骨骼，无法自动选择目标");
                chosen=r;allowed.addAll(owners.get(candidate.getKey()));
            }
            if(chosen==null) {
                boolean other=!entry.getKey().equals("708a8fef")&&otherMesh(entry.getKey()).equals(hash.get("mesh"))&&hash.get("bones") instanceof Number&&((Number)hash.get("bones")).intValue()==0;
                if(!other&&!weapon(String.valueOf(hash.get("mesh"))))continue;
                String alias="Scene"+out.renderers.size();
                Map<String,Object> target=Json.of("renderer",hash.get("renderer"),"mesh",hash.get("mesh"),"vertices",hash.get("vertices"),"submeshes",subs.size(),"bones",0);
                List<Object> identity=new ArrayList<>();for(int k=0;k<16;k++)identity.add(k%5==0?1:0);
                out.renderers.add(Json.of("name",alias,"target",target,"bones",Arrays.asList("__rigid__"),"parents",Arrays.asList(-1),"bindposes",identity));
                out.targets.add(Json.of("target",target,"owners",Arrays.asList("@renderer:"+hash.get("renderer"))));out.hashes.put(entry.getKey(),alias);
                continue;
            }
            String sig=shape(chosen)+"|"+subs.size()+"|"+Json.write(chosen.get("bones"))+Json.write(chosen.get("parents"))+Json.write(chosen.get("bindposes"));
            String alias=aliases.get(sig);
            if(alias==null) {
                alias="Scene"+out.renderers.size();aliases.put(sig,alias);
                Map<String,Object> target=Json.of("renderer",chosen.get("name"),"mesh",chosen.get("mesh"),"vertices",chosen.get("vertices"),"submeshes",subs.size(),"bones",((List<?>)chosen.get("bones")).size());
                out.renderers.add(Json.of("name",alias,"target",target,"bones",chosen.get("bones"),"parents",chosen.get("parents"),"bindposes",chosen.get("bindposes")));
                out.targets.add(Json.of("target",target,"owners",new ArrayList<>(allowed)));
            } else {
                int index=Integer.parseInt(alias.substring(5));Map<String,Object> t=Json.obj(out.targets.get(index),"target");Set<String> all=new TreeSet<>(allowed);
                for(Object o:Json.arr(t.get("owners"),"owners",1,20000))all.add((String)o);t.put("owners",new ArrayList<>(all));
            }
            out.hashes.put(entry.getKey(),alias);
        }
        Json.check(!out.renderers.isEmpty(),"找到网格 hash，但内置表没有对应的 NPC、怪物或武器蒙皮骨骼，请补充该目标的骨骼数据");
        return out;
    }
    static Importer.Result convert(File mod,File src,File ini,File dest,File shared,ModelConverter.TextureDecoder decoder,StringBuilder log)throws Exception {
        ModelConverter.Options opt=new ModelConverter.Options().ini(ini).replace(true).variants(true).textureMods(true)
            .textureDiag(new File(shared,"texture-diag")).pcTextures(DeviceHashes.pcTextures(shared)).source(mod.getAbsolutePath(),ModLibrary.signature(mod));
        opt.core(request->{try{return NativeBridge.draws(request);}catch(UnsatisfiedLinkError e){throw new IllegalStateException(e.getMessage());}});
        opt.rehearsal(request->{try{return NativeBridge.rehearse(request);}catch(UnsatisfiedLinkError e){throw new IllegalStateException(e.getMessage());}});
        File library=null;boolean other=false;
        if(!ModelConverter.textureOnly(new ModelConverter.Ini(src,ini))) {
            Collection<String> hashes=ModelConverter.overrideHashes(src,ini);Selection s=null;
            try{s=select(shared,hashes);}catch(Exception e){
                if(otherHashes(hashes))throw e;
                log.append("场景网格使用 INI 索引规则和实时骨骼。Unity 兼容资料不可用：").append(e.getMessage()).append('\n');
            }
            if(s==null){opt.gpuScene=true;opt.indexedVulkan=true;}
            else {
                opt.sceneTargets=s.targets;opt.deviceRenderers(s.hashes);other=otherTargets(s.targets);
                if(other)opt.core(null);
                File folder=new File(shared,"skeletons-scene");Json.check(folder.isDirectory()||folder.mkdirs(),"无法创建场景骨骼缓存");
                library=File.createTempFile("scene-",".json",folder);
                Files.write(library.toPath(),Json.write(Json.of("format","gibox-skeleton","version",1,"id",Characters.SCENE,"root","Scene","renderers",s.renderers)).getBytes(StandardCharsets.UTF_8));
                log.append("场景网格：按 ").append(s.hashes.size()).append(" 个索引缓冲 hash 匹配目标 ").append(s.renderers.size()).append(" 个网格\n");
            }
        }
        try{Importer.Result result=new Importer.Result();result.id=other?Characters.LOADING:Characters.SCENE;result.library=library!=null;
            result.model=ModelConverter.convert(src,library,dest,Characters.SCENE,opt,decoder,log);
            result.summary=other?"目标出现时自动替换网格":opt.gpuScene?"场景网格 MOD：启用 Vulkan 模型替换后按 INI 哈希匹配":library==null?"场景纯贴图 MOD：目标出现时按 hash 匹配":"场景网格 MOD：按 hash 匹配网格，目标出现时自动替换";return result;
        }finally{if(library!=null)library.delete();}
    }
}
