package com.gibox.tool;
import java.io.*;import java.nio.*;import java.nio.file.*;import java.nio.charset.StandardCharsets;import java.util.*;
/** Common GIMI / XXMI INI patterns that must convert. Host-only regression test. */
public class GimiPatternsTest {
 static byte[] le(int n){return ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN).array();}
 static void put(File d,String n,byte[] b)throws Exception{Files.write(new File(d,n).toPath(),b);}
 static void put(File d,String n,String s)throws Exception{put(d,n,s.getBytes(StandardCharsets.UTF_8));}
 static File mod(File base,String name,String ini)throws Exception{
  File d=new File(base,name);d.mkdirs();int n=6;
  ByteBuffer p=ByteBuffer.allocate(40*n).order(ByteOrder.LITTLE_ENDIAN);for(int i=0;i<n;i++){p.putFloat(i).putFloat(0).putFloat(0);p.putFloat(0).putFloat(1).putFloat(0);p.putFloat(1).putFloat(0).putFloat(0).putFloat(1);}put(d,"CitlaliPosition.buf",p.array());
  ByteBuffer b=ByteBuffer.allocate(32*n).order(ByteOrder.LITTLE_ENDIAN);for(int i=0;i<n;i++){b.putFloat(1).putFloat(0).putFloat(0).putFloat(0);b.putInt(1).putInt(0).putInt(0).putInt(0);}put(d,"CitlaliBlend.buf",b.array());
  ByteBuffer t=ByteBuffer.allocate(20*n).order(ByteOrder.LITTLE_ENDIAN);for(int i=0;i<n;i++){t.putInt(-1).putFloat(.5f).putFloat(.25f).putFloat(0).putFloat(0);}put(d,"CitlaliTexcoord.buf",t.array());
  ByteBuffer h=ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);h.putInt(0).putInt(1).putInt(2);put(d,"CitlaliHead.ib",h.array());
  ByteBuffer y=ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);y.putInt(3).putInt(4).putInt(5);put(d,"CitlaliBody.ib",y.array());
  put(d,"CitlaliHeadDiffuse.dds",new byte[128]);put(d,"CitlaliBodyDiffuse.dds",new byte[128]);put(d,"Citlali.ini",ini);return d;}
 static File bones(File base)throws Exception{File d=new File(base,"bones");Fixtures.skeleton(d,10000107,"Scene/Avatar_Girl_Catalyst_Citlali",new String[]{"body","face","brow"},new String[]{"Root","Spine"},null);return d;}
 static final String RES=
  "[ResourceCitlaliPosition]\ntype = Buffer\nstride = 40\nfilename = CitlaliPosition.buf\n\n"+
  "[ResourceCitlaliBlend]\ntype = Buffer\nstride = 32\nfilename = CitlaliBlend.buf\n\n"+
  "[ResourceCitlaliTexcoord]\ntype = Buffer\nstride = 20\nfilename = CitlaliTexcoord.buf\n\n"+
  "[ResourceCitlaliHeadIB]\ntype = Buffer\nformat = DXGI_FORMAT_R32_UINT\nfilename = CitlaliHead.ib\n\n"+
  "[ResourceCitlaliBodyIB]\ntype = Buffer\nformat = DXGI_FORMAT_R32_UINT\nfilename = CitlaliBody.ib\n\n"+
  "[ResourceCitlaliHeadDiffuse]\nfilename = CitlaliHeadDiffuse.dds\n\n[ResourceCitlaliBodyDiffuse]\nfilename = CitlaliBodyDiffuse.dds\n";
 static String ini(String constants,String posExtra,String headExtra,String prefix){return prefix+
  "[Constants]\n"+constants+"\n"+
  "[TextureOverrideCitlaliPosition]\nhash = 11111111\nvb0 = ResourceCitlaliPosition\n"+posExtra+"\n"+
  "[TextureOverrideCitlaliBlend]\nhash = 22222222\nvb1 = ResourceCitlaliBlend\nhandling = skip\ndraw = 6,0\n\n"+
  "[TextureOverrideCitlaliTexcoord]\nhash = 33333333\nvb1 = ResourceCitlaliTexcoord\n\n"+
  "[TextureOverrideCitlaliIB]\nhash = 44444444\nhandling = skip\ndrawindexed = auto\n\n"+
  "[TextureOverrideCitlaliHead]\nhash = 44444444\nmatch_first_index = 0\nib = ResourceCitlaliHeadIB\nps-t0 = ResourceCitlaliHeadDiffuse\n"+headExtra+"\n"+
  "[TextureOverrideCitlaliBody]\nhash = 44444444\nmatch_first_index = 3\nib = ResourceCitlaliBodyIB\nps-t0 = ResourceCitlaliBodyDiffuse\n"+headExtra+"\n"+RES;}
 public static void main(String[] a)throws Exception{
  File base=new File(a[0]);File bones=bones(base);String MAP="CitlaliHead=body;CitlaliBody=body";
  ModelConverter.TextureDecoder dec=dds->{ByteBuffer o=ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);o.putInt(1).putInt(1).putInt(-1);return o.array();};
  String ORFIX="run = CommandList\\global\\ORFix\\ORFix\n";
  Object[][] cases={
   {"T0 基线（无 ORFix，手工映射）",ini("global $creditinfo = 0\n","","",""),MAP},
   {"T1 绘制段带 run = CommandList\\global\\ORFix\\ORFix",ini("global $creditinfo = 0\n","",ORFIX,""),MAP},
   {"T2 Position 段里有 $active = 1",ini("global $active = 0\n","$active = 1\n","",""),MAP},
   {"T3 global $active 无初值 + [KeySwap] condition",ini("global $active\nglobal persist $swapvar = 0\n","","","")+"\n[KeySwap]\ncondition = $active == 1\nkey = x\ntype = cycle\n$swapvar = 0,1\n",MAP},
   {"T4 文件开头有 namespace 行",ini("global $creditinfo = 0\n","","","namespace = CitlaliMod\n\n"),MAP},
   {"T5 绘制段有 ps-t1 = null",ini("global $creditinfo = 0\n","","ps-t1 = null\n",""),MAP},
   {"T6 不填映射（自动对应渲染器）",ini("global $creditinfo = 0\n","","",""),""},
   {"T7 旧版 run = CommandListSkinTexture",ini("global $creditinfo = 0\n","","run = CommandListSkinTexture\n",""),MAP},
   {"T8 [Constants] 声明 17 个变量",ini(String.join("",Collections.nCopies(1,""))+varDecls(17),"","",""),MAP},
  };
  int i=0;for(Object[] c:cases){File src=mod(base,"mod"+i,(String)c[1]);File out=new File(base,"out/m"+i);StringBuilder log=new StringBuilder();
   try{ModelConverter.convert(src,bones,out,10000107,(String)c[2],false,dec,log);}catch(Throwable e){throw new AssertionError(c[0]+"：" +e.getMessage(),e);}
   Map<String,Object> p=Fixtures.part(out,0);if(!"body".equals(p.get("renderer"))||Fixtures.number(p,"bones")!=2||!"Part0".equals(p.get("name")))throw new AssertionError(c[0]+"：part="+p.get("name")+" "+p.get("renderer")+" "+p.get("bones"));i++;}
  // Toggle mods: only variables that change the draw list become in-game variants ($active and $creditinfo do not).
  String toggle=ini("global $active\nglobal $creditinfo = 0\nglobal persist $swapvar = 0\n","$active = 1\n","","").replace("[TextureOverrideCitlaliHead]\nhash = 44444444\nmatch_first_index = 0\nib = ResourceCitlaliHeadIB\n","[TextureOverrideCitlaliHead]\nhash = 44444444\nmatch_first_index = 0\nif $swapvar == 0\nib = ResourceCitlaliHeadIB\nelse\nib = null\nendif\n")+"\n[KeySwap]\ncondition = $active == 1\nkey = x\ntype = cycle\n$swapvar = 0,1\n";
  File src=mod(base,"modToggle",toggle);File out=new File(base,"out/toggle");ModelConverter.convert(src,bones,out,10000107,new ModelConverter.Options().mapping(MAP).variants(true),dec,new StringBuilder());
  ModelManifest m=ModelManifest.read(out);if(!m.vars.toString().equals("{$swapvar=[0, 1]}")||!m.defaults.toString().equals("{$swapvar=0}")||!m.combos.isEmpty())throw new AssertionError("toggle variants="+m.vars+" "+m.defaults);
  if(Fixtures.draws(out,0,Fixtures.set("$swapvar",0)).size()!=2||Fixtures.draws(out,0,Fixtures.set("$swapvar",1)).size()!=1)throw new AssertionError("toggle hides the head with $swapvar=1");
  System.out.println("GIMI patterns: PASS (external CommandList, $active, uninitialised globals, namespace line, null texture slot, hash-based renderer, 17 globals, toggle variants)");
 }
 static String varDecls(int n){StringBuilder s=new StringBuilder();for(int i=0;i<n;i++)s.append("global $v").append(i).append(" = 0\n");return s.toString();}
}
