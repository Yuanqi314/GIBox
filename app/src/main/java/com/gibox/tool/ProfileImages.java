package com.gibox.tool;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.regex.Pattern;

final class ProfileImages {
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
        for(ModelConverter.Section s:parsed.sections.values()){
            String section=s.name.toLowerCase(Locale.ROOT),hash=value(s,"hash").toLowerCase(Locale.ROOT),role;
            if(section.startsWith("textureoverride")&&hash.equals("8556a802"))role="namecard";
            else if(section.startsWith("shaderoverride")&&hash.equals("683a3517523f9dee"))role="avatar";
            else continue;
            String ref=value(s,"ps-t0").replaceFirst("(?i)^ref\\s+","").toLowerCase(Locale.ROOT);
            ModelConverter.Section resource=parsed.sections.get(ref);
            if(resource==null||!ref.startsWith("resource"))continue;
            String path=value(resource,"filename");if(!path.isEmpty())images.put(role,TextureDiag.resolve(ini.getParentFile(),path.replace("\"","")));
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
                doc.put(kind,LoadingImages.writeImage(image,tmp,"images/"+kind+".rgba",decoder,log));installed.add(label);
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
