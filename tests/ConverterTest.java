package dev.modelswap.only;
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public final class ConverterTest {
 static void ok(boolean b,String s){if(!b)throw new AssertionError(s);}
 static ByteBuffer b(int n){return ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN);}
 static void put(File f,String n,byte[] a)throws Exception{Files.write(new File(f,n).toPath(),a);}
 static void put(File f,String n,String a)throws Exception{put(f,n,a.getBytes(StandardCharsets.UTF_8));}
 static String ini="[Constants]\nglobal persist $variant = 0\n[TextureOverridePosition]\nvb0 = ResourcePosition\n[TextureOverrideBlend]\nvb1 = ResourceBlend\n[TextureOverrideTexcoord]\nvb2 = ResourceTexcoord\n[TextureOverrideBodyA]\nib = ResourceIndex\nps-t0 = ResourceDiffuse\nif $variant == 0\ndrawindexed = 3, 0, 0\nelse if $variant == 1\ndrawindexed = 3, 3, 0\nendif\n[ResourcePosition]\nfilename = Position.buf\nstride = 40\n[ResourceBlend]\nfilename = Blend.buf\nstride = 32\n[ResourceTexcoord]\nfilename = Texcoord.buf\nstride = 12\n[ResourceIndex]\nfilename = Index.buf\nformat = R32_UINT\n[ResourceDiffuse]\nfilename = Diffuse.dds\n";
 public static void main(String[] args)throws Exception{
  File root=Files.createTempDirectory("model-converter-test").toFile(),src=new File(root,"src"),bones=new File(root,"bones"),dest=new File(root,"model");src.mkdir();bones.mkdir();put(src,"mod.ini",ini);
  boolean bad;ByteBuffer pos=b(120),blend=b(96),uv=b(36),bind=b(64);for(int i=0;i<3;i++){pos.position(i*40);pos.putFloat(i).putFloat(0).putFloat(0).putFloat(0).putFloat(1).putFloat(0).putFloat(1).putFloat(0).putFloat(0).putFloat(1);blend.putFloat(i*32,2);uv.position(i*12);uv.putInt(-1).putFloat(.25f).putFloat(.75f);}for(int i=0;i<4;i++)bind.putFloat((i*4+i)*4,1);
  put(src,"Position.buf",pos.array());put(src,"Blend.buf",blend.array());put(src,"Texcoord.buf",uv.array());put(src,"Index.buf",b(24).putInt(2).putInt(0).putInt(1).putInt(0).putInt(1).putInt(2).array());put(src,"Diffuse.dds",new byte[]{1});
  byte[] rgba=b(24).putInt(2).putInt(2).putInt(0x01020304).putInt(0x05060708).putInt(0x090a0b0c).putInt(0x0d0e0f10).array();
  Fixtures.skeleton(bones,10000107,"Scene/Avatar_Girl_Catalyst_Citlali",new String[]{"Body"},new String[]{"BoneA"},Collections.singletonMap("Body",Fixtures.target("Body",23521,3,1)));
  List<File> outputs=new ArrayList<>(ModelConverter.convert(src,bones,dest,10000107,"",true,dds->rgba,new StringBuilder()));ok(outputs.size()==1,"variants stay in one folder");
  ModelManifest mf=ModelManifest.read(dest);ok(mf.vars.get("$variant").equals(Arrays.asList(0,1))&&mf.combos.size()==2&&mf.combos.get(1).get("$variant")==1&&mf.defaultCombo==0&&Variants.read(dest).defaults.get("$variant")==0,"variants in model.json: "+mf.vars+" "+mf.combos);
  Map<String,Object> d1=Json.obj(Fixtures.draws(dest,0,1).get(0),"draw");ok(Fixtures.number(d1,"start")==3&&Fixtures.number(d1,"count")==3&&"Material0".equals(d1.get("material")),"combo 1 uses the second index range");
  Map<String,Object> p0=Fixtures.part(dest,0);ok(Fixtures.number(Json.obj(p0.get("target"),"target"),"vertices")==23521&&mf.id==10000107&&mf.names.isEmpty()&&!mf.generation.isEmpty(),"target copied, generation written");
  ok("Body".equals(p0.get("renderer"))&&Fixtures.number(p0,"bones")==1&&Json.obj(p0.get("skeleton"),"skeleton").get("bones").equals(Collections.singletonList("BoneA")),"renderer and skeleton");
  for(File out:outputs){Map<String,Object> p=Fixtures.part(out,0);ok(Fixtures.number(p,"vertices")==3&&Fixtures.number(p,"indices")==6,"vertices shared by both variants, two index ranges");ok(Fixtures.data(out,0,"uv").getFloat(4)==.25f,"V flip");
   ok(Fixtures.data(out,0,"weights").getFloat(0)==1,"normalized weights");ok(Fixtures.view(out,Json.obj(Fixtures.material(out,"Material0").get("main"),"main").get("data")).getInt(0)==0x090a0b0c,"row flip");}
  // GIMI binds both Blend and Texcoord to vb1 in separate override stages.
  put(src,"mod.ini",ini.replace("vb2 = ResourceTexcoord","vb1 = ResourceTexcoord"));
  outputs.addAll(ModelConverter.convert(src,bones,new File(root,"shared_slot"),10000107,"",false,dds->rgba,new StringBuilder()));
  // The same physical slot may be reused inside a nested CommandList too.
  String nested=ini.replace("vb2 = ResourceTexcoord","run = CommandListUV")+"\n[CommandListUV]\nvb1 = ResourceTexcoord\n";
  put(src,"mod.ini",nested);outputs.addAll(ModelConverter.convert(src,bones,new File(root,"nested"),10000107,"",false,dds->rgba,new StringBuilder()));
  // Official GIMI shape: a hash-level auto draw and per-part IB resources.
  String resourceDefs=ini.substring(ini.indexOf("[ResourcePosition]"));
  String inherited="[TextureOverrideHeroPosition]\nvb0 = ResourcePosition\n[TextureOverrideHeroBlend]\nvb1 = ResourceBlend\n[TextureOverrideHeroTexcoord]\nvb1 = ResourceTexcoord\n[TextureOverrideHeroIB]\nhash = aabbccdd\nhandling = skip\ndrawindexed = auto\n[TextureOverrideHeroBodyA]\nhash = aabbccdd\nmatch_first_index = 9000\nib = ResourceIndex\nps-t0 = ResourceDiffuse\n"+resourceDefs;
  put(src,"mod.ini",inherited);File inheritedOut=new File(root,"inherited");outputs.addAll(ModelConverter.convert(src,bones,inheritedOut,10000107,"",false,dds->rgba,new StringBuilder()));
  ok(Fixtures.number(Fixtures.part(inheritedOut,0),"indices")==6,"auto uses all split IB indices, not match_first_index offset");
  // A more specific component wins over shared character defaults.
  byte[] hairPos=pos.array().clone(),hairBlend=blend.array().clone(),hairUV=uv.array().clone();for(int i=0;i<3;i++){ByteBuffer.wrap(hairPos).order(ByteOrder.LITTLE_ENDIAN).putFloat(i*40,100+i);ByteBuffer.wrap(hairBlend).order(ByteOrder.LITTLE_ENDIAN).putFloat(i*32,.5f).putFloat(i*32+4,.5f);ByteBuffer.wrap(hairUV).order(ByteOrder.LITTLE_ENDIAN).putFloat(i*12+4,.5f).putFloat(i*12+8,.5f);}
  put(src,"HairPosition.buf",hairPos);put(src,"HairBlend.buf",hairBlend);put(src,"HairTexcoord.buf",hairUV);
  String hairResources="\n[ResourceHairPosition]\nfilename = HairPosition.buf\nstride = 40\n[ResourceHairBlend]\nfilename = HairBlend.buf\nstride = 32\n[ResourceHairTexcoord]\nfilename = HairTexcoord.buf\nstride = 12\n";
  String scoped="[TextureOverrideHeroPosition]\nvb0 = ResourcePosition\n[TextureOverrideHeroBlend]\nvb1 = ResourceBlend\n[TextureOverrideHeroTexcoord]\nvb1 = ResourceTexcoord\n[TextureOverrideHeroHairPosition]\nvb0 = ResourceHairPosition\n[TextureOverrideHeroHairBlend]\nvb1 = ResourceHairBlend\n[TextureOverrideHeroHairTexcoord]\nvb1 = ResourceHairTexcoord\n[TextureOverrideHeroBodyA]\nib = ResourceIndex\ndrawindexed = 3,0,0\n[TextureOverrideHeroHairA]\nib = ResourceIndex\ndrawindexed = 3,0,0\n"+resourceDefs+hairResources;
  put(src,"mod.ini",scoped);File scopedOut=new File(root,"scoped");outputs.addAll(ModelConverter.convert(src,bones,scopedOut,10000107,"",false,dds->rgba,new StringBuilder()));
  ByteBuffer sg=Fixtures.data(scopedOut,0,"positions");ok(Fixtures.number(Fixtures.part(scopedOut,0),"vertices")==6&&sg.getFloat(0)==2&&sg.getFloat(3*12)==102,"component Position streams stay separate");
  ok(Fixtures.data(scopedOut,0,"weights").getFloat(32*3)==.5f,"component Blend selection");ok(Fixtures.data(scopedOut,0,"uv").getFloat(3*8)==.5f,"component Texcoord selection");
  put(src,"mod.ini",scoped+"\n[TextureOverrideUnrelated]\nib=ResourceIndex\ndrawindexed=3,0,0\n");outputs.addAll(ModelConverter.convert(src,bones,new File(root,"skip"),10000107,"Unrelated=skip",false,dds->rgba,new StringBuilder()));
  // Unused ambiguous defaults must not override explicit per-draw bindings.
  String conflicting="[TextureOverrideBlend1]\nvb1=ResourceBlend\n[TextureOverrideBlend2]\nvb1=ResourceHairBlend\n";
  String explicit="[TextureOverrideBodyA]\nvb0=ResourcePosition\nvb1=ResourceBlend\nvb2=ResourceTexcoord\nib=ResourceIndex\ndrawindexed=3,0,0\n";
  put(src,"mod.ini",conflicting+explicit+resourceDefs+hairResources);outputs.addAll(ModelConverter.convert(src,bones,new File(root,"explicit"),10000107,"",false,dds->rgba,new StringBuilder()));
  // A genuine same-role ambiguity still fails with useful source sections.
  put(src,"mod.ini",conflicting+"[TextureOverridePosition]\nvb0=ResourcePosition\n[TextureOverrideTexcoord]\nvb1=ResourceTexcoord\n[TextureOverrideBodyA]\nib=ResourceIndex\ndrawindexed=3,0,0\n"+resourceDefs+hairResources);
  boolean ambiguous=false;try{ModelConverter.convert(src,bones,new File(root,"ambiguous"),10000107,"",false,dds->rgba,new StringBuilder());}catch(IllegalArgumentException ex){ambiguous=ex.getMessage().contains("Blend")&&ex.getMessage().contains("TextureOverrideBlend1")&&ex.getMessage().contains("TextureOverrideBlend2");}ok(ambiguous&&!new File(root,"ambiguous").exists(),"reject real ambiguity without selecting a random stream");
  // Resource aliases for the same file/stride are one logical candidate.
  String alias="[TextureOverrideBlend1]\nvb1=ResourceBlend\n[TextureOverrideBlend2]\nvb1=ResourceBlendAlias\n[TextureOverridePosition]\nvb0=ResourcePosition\n[TextureOverrideTexcoord]\nvb1=ResourceTexcoord\n[TextureOverrideBodyA]\nib=ResourceIndex\ndrawindexed=3,0,0\n"+resourceDefs+"\n[ResourceBlendAlias]\nfilename=Blend.buf\nstride=32\n";
  put(src,"mod.ini",alias);outputs.addAll(ModelConverter.convert(src,bones,new File(root,"alias"),10000107,"",false,dds->rgba,new StringBuilder()));
  // Conditional providers select only the active variant's logical stream.
  String conditional=ini.replace("vb1 = ResourceBlend","if $variant == 0\nvb1=ResourceBlend\nelse if $variant == 1\nvb1=ResourceHairBlend\nendif").replace("vb2 = ResourceTexcoord","vb1 = ResourceTexcoord")+hairResources;
  put(src,"mod.ini",conditional);File conditionalOut=new File(root,"conditional");outputs.addAll(ModelConverter.convert(src,bones,conditionalOut,10000107,"",true,dds->rgba,new StringBuilder()));ByteBuffer unionSkin=Fixtures.data(conditionalOut,0,"weights");ok(Fixtures.number(Fixtures.part(conditionalOut,0),"vertices")==6&&unionSkin.getFloat(0)==1f&&unionSkin.getFloat(32*3)==.5f,"each variant keeps its own Blend stream");
  // Re-import replaces the folder atomically and keeps what the runtime wrote, while the character stays the same.
  String firstGeneration=ModelManifest.read(dest).generation;put(dest,"bonemap-Part0.tsv","kept");put(dest,"learned.json","{\"format\":\"gibox-learned\",\"version\":1,\"id\":0,\"targets\":{}}");put(src,"mod.ini",ini);
  ModelConverter.Options replace=new ModelConverter.Options().variants(true).replace(true).source("/mods/x","abc");ModelConverter.convert(src,bones,dest,10000107,replace,dds->rgba,new StringBuilder());
  ModelManifest again=ModelManifest.read(dest);ok(!again.generation.equals(firstGeneration)&&new File(dest,"bonemap-Part0.tsv").isFile()&&new File(dest,"learned.json").isFile()&&again.sourceSignature.equals("abc")&&again.sourcePath.equals("/mods/x"),"replace: new generation, kept runtime files, source");
  // Converted again for another character (here: without the skeleton library), what was learned for the old one goes.
  File swapped=new File(root,"swapped");ModelConverter.convert(src,bones,swapped,10000107,new ModelConverter.Options(),dds->rgba,new StringBuilder());put(swapped,"learned.json","{}");put(swapped,"bonemap-Part0.tsv","x");
  ModelConverter.convert(src,null,swapped,10000046,new ModelConverter.Options().replace(true).names(Collections.singletonList("hutao")),dds->rgba,new StringBuilder());ok(!new File(swapped,"learned.json").exists()&&!new File(swapped,"bonemap-Part0.tsv").exists()&&ModelManifest.read(swapped).id==10000046,"another character: runtime files dropped");
  // A skeleton record of another character is refused.
  bad=false;try{ModelConverter.convert(src,bones,new File(root,"wrongid"),10000046,new ModelConverter.Options(),dds->rgba,new StringBuilder());}catch(IllegalArgumentException e){bad=e.getMessage().contains("另一个角色");}ok(bad&&!new File(root,"wrongid").exists(),"skeleton of another character");
  File[] old=root.listFiles(f->f.getName().startsWith(".old-"));ok(old!=null&&old.length==0,"old folder removed");
  put(src,"mod.ini",ini);
  Map<String,Integer> v=new HashMap<>();v.put("$x",2);ok(new ModelConverter.Expr("($x + 1) * 2 == 6 && !0",v).eval()==1,"expression");bad=false;try{new ModelConverter.Expr("system(1)",v).eval();}catch(IllegalArgumentException e){bad=true;}ok(bad,"reject unknown expression");
  put(src,"Index.buf",b(4).putInt(999).array());File failed=new File(root,"failed");bad=false;try{ModelConverter.convert(src,bones,failed,10000107,"",false,dds->rgba,new StringBuilder());}catch(IllegalArgumentException e){bad=true;}ok(bad&&!failed.exists(),"rollback on invalid input");File[] temps=root.listFiles(f->f.getName().startsWith(".model-convert-"));ok(temps!=null&&temps.length==0,"temporary cleanup");
  for(File out:outputs){Process process=new ProcessBuilder(args[0],out.getAbsolutePath()).inheritIO().start();ok(process.waitFor()==0,"native parser validates converter output");}
  // No skeleton library: renderers by name, no bone names, bone count from the mod, character name in model.json.
  put(src,"mod.ini",ini);put(src,"Index.buf",b(24).putInt(2).putInt(0).putInt(1).putInt(0).putInt(1).putInt(2).array());File named=new File(root,"named");ModelConverter.convert(src,null,named,0,new ModelConverter.Options().names(Collections.singletonList("citlali")),dds->rgba,new StringBuilder());
  Map<String,Object> np=Fixtures.part(named,0);ModelManifest nm=ModelManifest.read(named);
  ok(np.get("skeleton")==null&&np.get("target")==null&&"Body".equals(np.get("renderer"))&&Fixtures.number(np,"bones")==1&&nm.id==0&&nm.names.equals(Collections.singletonList("citlali"))&&nm.character()==0,"conversion without skeleton library");
  put(named,"learned.json","{\"format\":\"gibox-learned\",\"version\":1,\"id\":10000107,\"targets\":{}}");ok(ModelManifest.read(named).character()==10000107,"learned character ID");
  Process np2=new ProcessBuilder(args[0],named.getAbsolutePath()).inheritIO().start();ok(np2.waitFor()==0,"native loader reads the by-name model");
  // model.json is checked when the app reads it, too.
  put(named,"model.json",new String(Files.readAllBytes(new File(named,"model.json").toPath()),StandardCharsets.UTF_8).replace("\"version\": 1","\"version\": 2"));bad=false;try{ModelManifest.read(named);}catch(IllegalArgumentException e){bad=e.getMessage().contains("版本");}ok(bad,"newer format refused");
  // Our JSON: strict like the native reader, exact float round trip.
  for(String j:new String[]{"{\"a\":1,\"a\":2}","[01]","[1,]","{\"a\":1} x","[\"\u0001\"]","[1e999]"}){bad=false;try{Json.parse(j);}catch(IllegalArgumentException e){bad=true;}ok(bad,"JSON accepted: "+j);}
  Object third=((List<?>)Json.parse(Json.write(Arrays.<Object>asList(1f/3,-2.5e-7f)))).get(0);ok(Json.finite(third,"f")==1f/3,"float round trip");
  ok(Json.parse("{\"s\":\"\\u6a21\\\"\"}").equals(Collections.singletonMap("s","模\"")),"escapes");
  System.out.println("Java converter: PASS (GIBox model.json + model.bin, shared vb1, nested commands, inherited auto draw, component scopes, explicit overrides, real ambiguity, merged variants, conditional streams, targets, atomic replace keeping runtime files per character, skeleton.json, no-library conversion, strict JSON, binary interoperability)");
 }
}
