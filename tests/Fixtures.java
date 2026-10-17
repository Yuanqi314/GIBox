package com.gibox.tool;
import java.io.*;import java.nio.*;import java.nio.file.*;import java.nio.charset.StandardCharsets;import java.util.*;
/** Reading converted GIBox model folders and writing skeleton.json records, for the host tests. */
final class Fixtures {
 static Map<String,Object> manifest(File d)throws Exception{return Json.obj(Json.read(new File(d,"model.json"),4L<<20),"model.json");}
 static Map<String,Object> part(File d,int i)throws Exception{return Json.obj(Json.arr(manifest(d).get("parts"),"parts",1,64).get(i),"part");}
 static Map<String,Object> material(File d,String name)throws Exception{for(Object m:Json.arr(manifest(d).get("materials"),"materials",1,256))if(name.equals(Json.obj(m,"material").get("name")))return Json.obj(m,"material");throw new AssertionError("no material "+name);}
 static ByteBuffer view(File d,Object v)throws Exception{List<Object> a=Json.arr(v,"view",2,2);int off=(int)Json.integer(a.get(0),"offset",0,1L<<30),len=(int)Json.integer(a.get(1),"length",0,1L<<30);
  byte[] bin=Files.readAllBytes(new File(d,"model.bin").toPath());return ByteBuffer.wrap(Arrays.copyOfRange(bin,off,off+len)).order(ByteOrder.LITTLE_ENDIAN);}
 static ByteBuffer data(File d,int part,String key)throws Exception{return view(d,Json.obj(part(d,part).get("data"),"data").get(key));}
 static long number(Map<String,Object> o,String k){return Json.integer(o.get(k),k,Long.MIN_VALUE/2,Long.MAX_VALUE/2);}
 static List<Object> draws(File d,int part,int combo)throws Exception{return Json.arr(Json.arr(part(d,part).get("draws"),"draws",1,32).get(combo),"draw list",0,64);}
 static Map<String,Object> target(String mesh,int vertices,int submeshes,int bones){return Json.of("renderer","Body","mesh",mesh,"vertices",vertices,"submeshes",submeshes,"bones",bones);}
 /** skeleton.json as the runtime writes it: identity bindposes, a chain of parents; `targets` by renderer name (may miss some). */
 static void skeleton(File dir,int id,String root,String[] renderers,String[] bones,Map<String,Map<String,Object>> targets)throws Exception{
  dir.mkdirs();List<Object> rs=new ArrayList<>();
  for(String r:renderers){List<Object> parents=new ArrayList<>(),binds=new ArrayList<>();for(int i=0;i<bones.length;i++){parents.add(i-1);for(int j=0;j<16;j++)binds.add(j%5==0?1:0);}
   Map<String,Object> e=Json.of("name",r);if(targets!=null&&targets.containsKey(r))e.put("target",targets.get(r));e.put("bones",new ArrayList<Object>(Arrays.asList((Object[])bones)));e.put("parents",parents);e.put("bindposes",binds);rs.add(e);}
  Files.write(new File(dir,"skeleton.json").toPath(),Json.write(Json.of("format","gibox-skeleton","version",1,"id",id,"root",root,"renderers",rs)).getBytes(StandardCharsets.UTF_8));
 }
}
