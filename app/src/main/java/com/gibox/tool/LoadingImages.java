package com.gibox.tool;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.regex.*;

final class LoadingImages {
    static final String FILE="loading.json";
    static boolean is(File dir){return new File(dir,FILE).isFile();}
    static File ini(File mod){if(mod==null||!mod.isDirectory())return null;for(File f:ModConfig.allInis(mod))if(Pattern.compile("(?im)^\\s*\\[ResourceLS\\.\\d+\\]\\s*$").matcher(ModConfig.source(f)).find())return f;return null;}
    static Importer.Result install(File mod,File dest,ModelConverter.TextureDecoder decoder,StringBuilder log)throws Exception{
        File ini=ini(mod);Json.check(ini!=null,"没有找到加载图片列表（ResourceLS.0 等），请选择对应的加载背景 MOD");
        ModelConverter.Ini parsed=new ModelConverter.Ini(ini.getParentFile(),ini);Map<Integer,File> images=new TreeMap<>();
        for(ModelConverter.Section s:parsed.sections.values()){
            Matcher m=Pattern.compile("(?i)^ResourceLS\\.(\\d+)$").matcher(s.name);if(!m.matches())continue;
            int index=Integer.parseInt(m.group(1));Json.check(index<64,"加载图片最多 64 张");
            for(String line:s.lines){String[] kv=line.split("=",2);if(kv.length==2&&kv[0].trim().equalsIgnoreCase("filename"))images.put(index,TextureDiag.resolve(ini.getParentFile(),kv[1].trim()));}
        }
        Json.check(!images.isEmpty(),"加载图片列表为空");boolean hide=Pattern.compile("(?im)^\\s*(?:global\\s+)?\\$skip_ui\\s*=\\s*1\\s*(?:;.*)?$").matcher(ModConfig.source(ini)).find();
        File parent=dest.getAbsoluteFile().getParentFile();Json.check(parent.isDirectory()||parent.mkdirs(),"无法创建模型目录");File tmp=Files.createTempDirectory(parent.toPath(),".loading-import-").toFile();
        try{new File(tmp,"images").mkdirs();List<Object> rows=new ArrayList<>();long total=0;
            for(File f:images.values()){
                String file="images/"+rows.size()+".rgba";
                Map<String,Object> row=writeImage(f,tmp,file,decoder,log,null);
                total+=new File(tmp,file).length();Json.check(total<=256L<<20,"加载图片总计超过 256 MiB");rows.add(row);
            }
            Map<String,Object> doc=Json.of("format","gibox-loading-images","version",1,"name",dest.getName(),"generation","l"+System.currentTimeMillis(),"images",rows,"hideUi",hide,"source",Json.of("path",mod.getAbsolutePath(),"signature",ModLibrary.signature(mod)));
            Files.write(new File(tmp,FILE).toPath(),Json.write(doc).getBytes(StandardCharsets.UTF_8));Files.write(new File(tmp,"conversion.log").toPath(),log.toString().getBytes(StandardCharsets.UTF_8));
            ModelConverter.commit(tmp,dest,true);Importer.Result result=new Importer.Result();result.id=Characters.LOADING;result.model=dest;result.summary="加载图片："+rows.size()+" 张，每次加载随机切换；在 MOD 详情中可切换是否隐藏加载提示";return result;
        }finally{if(tmp.exists())ModelConverter.remove(tmp);}
    }
    private static Map<String,Object> writeImage(File image,File dir,String file,ModelConverter.TextureDecoder decoder,StringBuilder log,String avatarColorSpace)throws Exception {
        Json.check(image.isFile()&&image.length()<=64L<<20,"图片不存在或过大："+image.getName());
        String ext=image.getName().toLowerCase(Locale.ROOT);Json.check(ext.endsWith(".dds")||ext.endsWith(".png"),"图片仅支持 DDS / PNG");
        byte[] source=Files.readAllBytes(image.toPath());
        byte[] raw=decoder.decode(source);Json.check(raw.length>=8,"图片解码返回值无效");
        ByteBuffer b=ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);int w=b.getInt(),h=b.getInt();
        Json.check(w>0&&h>0&&w<=2048&&h<=2048&&raw.length==8+4L*w*h,"图片尺寸无效或超过 2048");
        byte[] pixels=new byte[raw.length-8];for(int y=0;y<h;y++)System.arraycopy(raw,8+y*w*4,pixels,(h-1-y)*w*4,w*4);
        Files.write(new File(dir,file).toPath(),pixels);
        log.append("图片：").append(image.getName()).append(" → ").append(w).append('x').append(h).append('\n');
        Map<String,Object> row=Json.of("file",file,"width",w,"height",h);
        if(avatarColorSpace!=null)row.put("linear",avatarLinear(source,avatarColorSpace));
        return row;
    }
    private static boolean avatarLinear(byte[] source,String override){
        if(override.equalsIgnoreCase("linear"))return true;
        if(override.equalsIgnoreCase("srgb"))return false;
        ByteBuffer png=ByteBuffer.wrap(source);
        if(png.remaining()<8||png.getLong()!=0x89504e470d0a1a0aL)return false;
        while(png.remaining()>=12){
            int length=png.getInt(),type=png.getInt();
            if(length<0||length>png.remaining()-4)return false;
            if(type==0x49444154)break; // IDAT
            if(type==0x73524742|| (type==0x67414d41&&length==4&&png.getInt(png.position())==45455))return false; // sRGB / gAMA
            png.position(png.position()+length+4);
        }
        return true;
    }
    static ModelManifest summary(File dir)throws Exception{
        Map<String,Object> j=Json.obj(Json.read(new File(dir,FILE),1L<<20),FILE);Json.check("gibox-loading-images".equals(j.get("format"))&&Json.integer(j.get("version"),"version",1,1)==1,"加载图片格式无效");
        for(Object row:Json.arr(j.get("images"),"images",1,64)){Map<String,Object> r=Json.obj(row,"image");Json.check(Json.str(r.get("file"),"file",64).matches("images/[0-9]+\\.rgba"),"加载图片路径无效");Json.integer(r.get("width"),"width",1,2048);Json.integer(r.get("height"),"height",1,2048);}
        ModelManifest m=new ModelManifest();m.id=Characters.LOADING;m.version=2;m.name=Json.str(j.get("name"),"name",512);m.generation=Json.str(j.get("generation"),"generation",64);
        m.vars.put("$skip_ui",Arrays.asList(0,1));m.defaults.put("$skip_ui",Boolean.TRUE.equals(j.get("hideUi"))?1:0);
        if(j.get("source") instanceof Map){Map<String,Object> s=Json.obj(j.get("source"),"source");m.sourcePath=Json.str(s.get("path"),"source.path",4096);m.sourceSignature=Json.str(s.get("signature"),"source.signature",128);}return m;
    }

    static final class Profile {
        static final String FILE="profile.json";
        private static final Pattern RULE=Pattern.compile("(?im)^\\s*hash\\s*=\\s*(?:8556a802|683a3517523f9dee)\\s*(?:;.*)?$");
        static boolean is(File dir){return new File(dir,FILE).isFile();}
        static File ini(File mod){
            for(File f:ModConfig.allInis(mod))if(RULE.matcher(ModConfig.source(f)).find())return f;
            return null;
        }
        private static String value(ModelConverter.Section section,String key){
            for(String line:section.lines){int at=line.indexOf('=');if(at>0&&line.substring(0,at).trim().equalsIgnoreCase(key))return line.substring(at+1).trim();}
            return "";
        }
        static Importer.Result install(File mod,File ini,File dest,ModelConverter.TextureDecoder decoder,StringBuilder log)throws Exception {
            ModelConverter.Ini parsed=new ModelConverter.Ini(ini.getParentFile(),ini);Map<String,File> images=new LinkedHashMap<>();
            String avatarColorSpace="";
            for(ModelConverter.Section s:parsed.sections.values()){
                String section=s.name.toLowerCase(Locale.ROOT),hash=value(s,"hash").toLowerCase(Locale.ROOT),role;
                if(section.startsWith("textureoverride")&&hash.equals("8556a802"))role="namecard";
                else if(section.startsWith("shaderoverride")&&hash.equals("683a3517523f9dee"))role="avatar";
                else continue;
                String ref=value(s,"ps-t0").replaceFirst("(?i)^ref\\s+","").toLowerCase(Locale.ROOT);
                ModelConverter.Section resource=parsed.sections.get(ref);
                if(resource==null||!ref.startsWith("resource"))continue;
                String path=value(resource,"filename");
                if(!path.isEmpty()){
                    images.put(role,TextureDiag.resolve(ini.getParentFile(),path.replace("\"","")));
                    if(role.equals("avatar"))avatarColorSpace=value(resource,"override_color_space");
                }
            }
            File parent=dest.getAbsoluteFile().getParentFile();Json.check(parent.isDirectory()||parent.mkdirs(),"无法创建模型目录");
            File tmp=Files.createTempDirectory(parent.toPath(),".profile-import-").toFile();
            try{
                Json.check(new File(tmp,"images").mkdir(),"无法创建图片目录");
                Map<String,Object> doc=Json.of("format","gibox-profile-images","version",1,"name",dest.getName(),"generation","p"+System.nanoTime(),"source",Json.of("path",mod.getAbsolutePath(),"signature",ModLibrary.signature(mod)));
                List<String> installed=new ArrayList<>(),missing=new ArrayList<>();
                for(Map.Entry<String,File> entry:images.entrySet()){
                    String kind=entry.getKey(),label=kind.equals("avatar")?"头像":"名片";File image=entry.getValue();
                    if(!image.isFile()){missing.add(image.getName());continue;}
                    doc.put(kind,writeImage(image,tmp,"images/"+kind+".rgba",decoder,log,kind.equals("avatar")?avatarColorSpace:null));installed.add(label);
                }
                Json.check(!installed.isEmpty(),"没有可导入的头像或名片图片"+(missing.isEmpty()?"":"，缺少 "+String.join("、",missing)));
                Files.write(new File(tmp,FILE).toPath(),Json.write(doc).getBytes(StandardCharsets.UTF_8));
                if(!missing.isEmpty())log.append("包内未提供：").append(String.join("、",missing)).append('\n');
                Files.write(new File(tmp,"conversion.log").toPath(),log.toString().getBytes(StandardCharsets.UTF_8));
                ModelConverter.commit(tmp,dest,true);Importer.Result r=new Importer.Result();r.id=Characters.LOADING;r.model=dest;
                r.summary="个人资料页替换"+String.join("和",installed)+(missing.isEmpty()?"":"。包内未提供 "+String.join("、",missing));return r;
            }finally{if(tmp.exists())ModelConverter.remove(tmp);}
        }
        static ModelManifest summary(File dir)throws Exception {
            Map<String,Object> doc=Json.obj(Json.read(new File(dir,FILE),1L<<20),FILE);
            Json.check("gibox-profile-images".equals(doc.get("format"))&&Json.integer(doc.get("version"),"version",1,1)==1,"头像和名片格式无效");
            Json.check(doc.containsKey("avatar")||doc.containsKey("namecard"),"头像和名片图片为空");
            ModelManifest m=new ModelManifest();m.id=Characters.LOADING;m.version=2;m.name=Json.str(doc.get("name"),"name",512);m.generation=Json.str(doc.get("generation"),"generation",64);
            Map<String,Object> source=Json.obj(doc.get("source"),"source");m.sourcePath=Json.str(source.get("path"),"source.path",4096);m.sourceSignature=Json.str(source.get("signature"),"source.signature",128);
            return m;
        }
    }
}
