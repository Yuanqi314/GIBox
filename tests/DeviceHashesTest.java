package com.gibox.tool;
import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Bundled eye hashes must resolve without any device capture and override the Body fallback. */
public final class DeviceHashesTest {
 static void ok(boolean value,String why){if(!value)throw new AssertionError(why);}
 public static void main(String[] args)throws Exception{
  File base=Files.createTempDirectory("gibox-eye-hashes-").toFile(),shared=new File(base,"shared"),bones=new File(base,"bones");shared.mkdirs();
  Files.copy(new File("app/src/main/assets/device-hashes.json").toPath(),new File(shared,"device-hashes.json").toPath());
  Files.copy(new File("app/src/main/assets/offline-skeletons.json").toPath(),new File(shared,"offline-skeletons.json").toPath());
  OfflineSkeletons offline=OfflineSkeletons.load(shared);
  String[] playable={"SkirkNew","LittleIdolBoy","LittleIdolGirl","Lauma","Flins","Aino","Nefer","Durin",
      "Jahoda","Columbina","Zibai","Varka","AetherShadow","LumineShadow"};
  int[] ids={10000114,10000117,10000118,10000119,10000120,10000121,10000122,10000123,
      10000124,10000125,10000126,10000128,10000134,10000135};
  for(int i=0;i<ids.length;i++){
   String prefab=offline.prefabFor(ids[i],null);
   ok(prefab!=null&&prefab.endsWith("_"+playable[i]),"missing offline rig for "+playable[i]+": "+prefab);
   ok(DeviceHashes.character(prefab.toLowerCase(Locale.ROOT))==ids[i],"wrong avatar id for "+prefab);
  }
  String outfit="avatar_girl_sword_playergirlcostumecwxr";DeviceHashes table=DeviceHashes.load(shared);Map<String,String> renderers=table.renderers(10000007,outfit);
  Map<String,String> tps=table.renderers(10000135,null);
  ok("Body".equals(tps.get("6f94c5c0"))&&"Bang".equals(tps.get("59ce21c6")),"missing TPS body or bangs");
  {
   Set<String> sharedEye=new TreeSet<>(Arrays.asList("0e247223","36f018fc","3967859e"));
   ok(table.characters(sharedEye).isEmpty(),"shared eye hashes must not decide the character");
   Map<Integer,List<String>> hits=table.characters(Arrays.asList("36f018fc","6f94c5c0"));
   ok(hits.keySet().equals(Collections.singleton(10000135)),"TPS body hash must identify its character");
   Characters.Guess guess=new Characters.Guess();
   guess.scores.put(10000007,544);
   guess.scores.put(10000135,269);
   Characters.hashEvidence(guess,hits);
   ok(guess.recommended.size()==1&&guess.recommended.get(0).id==10000135,"generic Lumine names outranked the TPS body hash");
   for(String hash:sharedEye)ok("Pupil".equals(tps.get(hash)),"missing TPS Pupil hash "+hash);
   ModLibrary.Character lib=offline.library(offline.prefabFor(10000135,null),10000135,new File(base,"offline"));
   ModelConverter.Bones eye=null;
   for(ModelConverter.Bones b:ModelConverter.bones(lib.bones,10000135))if(b.name.equals("Pupil"))eye=b;
   ok(eye!=null&&eye.count==2&&eye.target!=null&&Long.valueOf(1).equals(eye.target.get("submeshes")),"TPS Pupil needs a complete target");
  }
  Map<String,Map<String,Object>> targets=new HashMap<>();targets.put("Body",Fixtures.target("Body",31179,2,2));targets.put("Pupil",Fixtures.target("Pupil",246,1,2));
  Fixtures.skeleton(bones,10000007,"Scene/Avatar_Girl_Sword_PlayerGirlCostumeCWXR",new String[]{"Body","Pupil"},new String[]{"Eye_L","Eye_R"},targets);
  for(String hash:new String[]{"0e247223","36f018fc","3967859e"}){
   ok("Pupil".equals(renderers.get(hash)),"missing bundled Pupil hash "+hash);
   ok(!table.renderers(10000007,"avatar_girl_sword_playergirl").containsKey(hash),"costume eye hash leaked to original outfit");
   String ini="[TextureOverrideLumineSkinEyesPosition]\nvb0 = ResourceCitlaliPosition\n[TextureOverrideLumineSkinEyesBlend]\nvb1 = ResourceCitlaliBlend\n[TextureOverrideLumineSkinEyesTexcoord]\nvb1 = ResourceCitlaliTexcoord\n[TextureOverrideLumineSkinEyesA]\nhash = "+hash+"\nib = ResourceCitlaliHeadIB\nps-t0 = ResourceCitlaliHeadDiffuse\ndrawindexed = 3,0,0\n"+GimiPatternsTest.RES;
   File src=GimiPatternsTest.mod(base,"eyes-"+hash,ini),out=new File(base,"out-"+hash);StringBuilder log=new StringBuilder();
   ModelConverter.convert(src,bones,out,10000007,new ModelConverter.Options().fallback("Body").deviceRenderers(renderers),data->new byte[]{1,0,0,0,1,0,0,0,-1,-1,-1,-1},log);
   Map<String,Object> part=Fixtures.part(out,0);ok("Pupil".equals(part.get("renderer"))&&Fixtures.number(part,"bones")==2,"eyes fell back to Body: "+log);
   Map<String,Object> model=Json.obj(Json.read(new File(out,"model.json"),4L<<20),"model");ok(((List<?>)model.get("parts")).size()==1,"unexpected replacement parts");
   ok(log.toString().contains("LumineSkinEyesA -> Pupil")&&!log.toString().contains("按默认渲染器"),"hash mapping did not take precedence");
  }
  String capture="{\"meshes\":[{\"ib\":\"36f018fc\",\"avatar\":\"avatar_girl_gun_lumineshadow\",\"renderer\":\"pupil\",\"mesh\":\"Pupil_LOD1\"}]}";
  Files.write(new File(shared,"hash-map.json").toPath(),capture.getBytes(java.nio.charset.StandardCharsets.UTF_8));
  table=DeviceHashes.load(shared);
  ok("pupil".equals(table.renderers(10000135,null).get("36f018fc"))&&"Pupil".equals(table.renderers(10000007,outfit).get("36f018fc")),
      "TPS capture must retain the bundled skin's Pupil mapping");
  ModelConverter.remove(base);System.out.println("Bundled rig and eye hashes: PASS (playable prefab links, TPS and costume scope, Pupil palette, capture overlays)");
 }
}
