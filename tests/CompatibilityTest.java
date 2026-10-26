package com.gibox.tool;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.*;

/** Regression inputs matching the uploaded legacy Nahida and HoHE package structures. Host-only. */
public final class CompatibilityTest {
    private static void ok(boolean value,String detail){if(!value)throw new AssertionError(detail);}
    private static final ModelConverter.TextureDecoder DECODE = image -> ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
        .putInt(1).putInt(1).putInt(image[0]&255).array();
    private static int pixel(File out,Map<String,Object> material,String role)throws Exception{
        return Fixtures.view(out,Json.obj(material.get(role),role).get("data")).getInt(0);
    }
    private static void legacy(File base)throws Exception{
        for(int mode=0;mode<3;mode++){
            String bindings="ps-t0 = ResourceNormalData\nif $swapvar == 4\nps-t1 = ResourceOtherColor\nelse\nps-t1 = ResourceColorData\nendif\nps-t2 = ResourceMaskData\n"
                +(mode==1?"run = CommandList\\global\\ORFix\\NNFix\n":"");
            String text=GimiPatternsTest.ini("global persist $swapvar = 0\n","",bindings,"")
                .replace("44444444",mode==2?"aabbccdd":"8b268d05")
                +"\n[KeySwap]\nkey = r\ntype = cycle\n$swapvar = 0,1,2,3,4\n"
                +"[ResourceNormalData]\nfilename = FalseDiffuse.dds\n[ResourceColorData]\nfilename = FalseLightMap.dds\n"
                +"[ResourceOtherColor]\nfilename = OtherLightMap.dds\n[ResourceMaskData]\nfilename = FalseShadow.dds\n";
            File src=GimiPatternsTest.mod(base,"legacy"+mode,text),out=new File(base,"legacy-out"+mode);
            GimiPatternsTest.put(src,"FalseDiffuse.dds",new byte[]{11});GimiPatternsTest.put(src,"FalseLightMap.dds",new byte[]{22});
            GimiPatternsTest.put(src,"OtherLightMap.dds",new byte[]{44});GimiPatternsTest.put(src,"FalseShadow.dds",new byte[]{33});
            // Same numeric index data, encoded as the original legacy package's R16 stream.
            GimiPatternsTest.put(src,"CitlaliHead.ib",ByteBuffer.allocate(6).order(ByteOrder.LITTLE_ENDIAN).putShort((short)0).putShort((short)1).putShort((short)2).array());
            GimiPatternsTest.put(src,"CitlaliBody.ib",ByteBuffer.allocate(6).order(ByteOrder.LITTLE_ENDIAN).putShort((short)3).putShort((short)4).putShort((short)5).array());
            GimiPatternsTest.put(src,"Citlali.ini",text.replace("R32_UINT","R16_UINT"));
            StringBuilder log=new StringBuilder();ModelConverter.convert(src,null,out,10000073,new ModelConverter.Options().variants(true).fallback("Body"),DECODE,log);
            for(int state=0;state<5;state++)for(Object raw:Fixtures.draws(out,0,Fixtures.set("$swapvar",state))){
                Map<String,Object> draw=Json.obj(raw,"draw"),m=Fixtures.material(out,(String)draw.get("material"));
                if(mode==0){ok(pixel(out,m,"main")== (state==4?44:22),"legacy IB must select real color across variants");
                    ok(pixel(out,m,"lightmap")==33&&pixel(out,m,"normalmap")==11,"legacy mask/normal roles");}
                else {ok(pixel(out,m,"main")==11&&pixel(out,m,"lightmap")== (state==4?44:22),"explicit NNFix and unknown IB preserve their conventions");
                    ok(!m.containsKey("normalmap"),"no inferred normal on NNFix or unknown layout");}
            }
            ok((mode!=0)||log.toString().contains("优先于文件名"),"layout decision must be explained");
        }
    }
    private static void hohe(File base)throws Exception{
        String key="[KeyWings]\nkey = 6\ntype = cycle\n$wings = 0,1\n";
        String text=GimiPatternsTest.ini("global persist $wings = 1\n","","drawindexed = 3,0,0\n","")
            .replace("drawindexed = auto\n","")
            .replace("ps-t0 = ResourceCitlaliBodyDiffuse\ndrawindexed = 3,0,0\n",
                "ps-t0 = ResourceCitlaliBodyDiffuse\nif $wings == 1\n$\\TexFx\\_1 = 3\n$\\TexFx\\_2 = 3\nrun = CommandList\\TexFx\\Component.1\nendif\n")
            +"\n[Present]\npost $wings = 1\n[Present]\nrun = CommandListMenu\n"+key+key
            +"[CommandListMenu]\nif $unknown_menu_only\nrun = CommandListMouseOnly\nendif\n";
        File archive=new File(base,"wrapper"),src=GimiPatternsTest.mod(archive,"HoHE",text),face=new File(src,"face");face.mkdirs();
        GimiPatternsTest.put(src,"help.ini","[Constants]\nglobal $active = 0\n[Present]\npost $active = 0\n");
        GimiPatternsTest.put(face,"face.ini","[TextureOverrideFace]\nhash = 895cd4d0\nrun = CommandList\\GIMI\\SetTextures\n");
        GimiPatternsTest.put(src,"DISABLED-old.ini",text);
        Files.move(new File(src,"CitlaliTexcoord.buf").toPath(),new File(src,"CitlaliTexCoord.buf").toPath());
        File textures=new File(src,"resources");textures.mkdir();GimiPatternsTest.put(textures,"pigment.png",new byte[]{7});
        GimiPatternsTest.put(src,"Citlali.ini",text.replace("filename = CitlaliHeadDiffuse.dds","filename = Resources/Pigment.png"));
        GimiPatternsTest.put(src,"CitlaliBody.ib",ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN).putInt(3).putInt(4).putInt(5).putInt(5).putInt(4).putInt(3).array());
        ok(ModLibrary.modRoot(archive).equals(src)&&ModLibrary.ini(src).getName().equals("Citlali.ini"),"main geometry must win over help and face INIs");
        File out=new File(base,"hohe-out");StringBuilder log=new StringBuilder();
        ModelConverter.convert(src,null,out,10000073,new ModelConverter.Options().variants(true).fallback("Body"),DECODE,log);
        ok(Fixtures.draws(out,0,Fixtures.set("$wings",0)).size()==1&&Fixtures.draws(out,0,Fixtures.set("$wings",1)).size()==2,"TexFx-only component is conditional and retained");
        ok(ModelManifest.read(out).vars.keySet().equals(Collections.singleton("$wings")),"mouse/menu conditions must not create model variants");
        List<Object> on=Fixtures.draws(out,0,Fixtures.set("$wings",1));boolean body=false,color=false;
        for(Object raw:on){Map<String,Object>d=Json.obj(raw,"draw"),m=Fixtures.material(out,(String)d.get("material"));
            if(Fixtures.number(d,"slot")==1){body=true;ok(Fixtures.number(d,"count")==3,"TexFx count");}
            else color|=pixel(out,m,"main")==7;
        }
        ByteBuffer ix=Fixtures.data(out,0,"indices"),positions=Fixtures.data(out,0,"positions");
        ok(body&&color&&positions.getFloat(ix.getInt(12)*12)==5&&positions.getFloat(ix.getInt(16)*12)==4&&positions.getFloat(ix.getInt(20)*12)==3,"TexFx start offset survives vertex compaction; PNG and PC filename casing");
        ok(log.toString().contains("重复 Present")&&log.toString().contains("重复按键")&&log.toString().contains("未复刻 PC 着色器")&&log.toString().contains("face.ini"),"compatibility decisions and limits must be visible");
        // Two actual geometry INIs must never silently pick one, even when both have a face sidecar.
        GimiPatternsTest.put(src,"second.ini",text);boolean refused=false;
        try{ModLibrary.modRoot(archive);}catch(IllegalArgumentException e){refused=e.getMessage().contains("套 INI 方案");}ok(refused,"ambiguous geometry must be refused until a scheme is chosen");
        // Several model INIs are schemes: listed, the choice stored in modelswap.txt, and only that one converted.
        ModLibrary.check(archive);List<String> schemes=Importer.schemes(archive);
        ok(schemes.equals(Arrays.asList("HoHE/Citlali.ini","HoHE/second.ini")),"schemes are the model INIs, not help/face sidecars: "+schemes);
        boolean help=false,escape=false;try{Importer.setScheme(archive,"HoHE/help.ini");}catch(IllegalArgumentException e){help=true;}
        try{Importer.setScheme(archive,"../escape.ini");}catch(IllegalArgumentException e){escape=true;}ok(help&&escape,"only model INIs inside the mod can be chosen");
        Importer.setScheme(archive,"HoHE/second.ini");ok(Importer.scheme(archive).equals("HoHE/second.ini")&&ModLibrary.mainIni(archive).getName().equals("second.ini")&&ModLibrary.modRoot(archive).equals(src),"the stored scheme is used");
        File chosenOut=new File(base,"scheme-out");StringBuilder chosenLog=new StringBuilder();
        ModelConverter.convert(src,null,chosenOut,10000073,new ModelConverter.Options().variants(true).fallback("Body").ini(ModLibrary.mainIni(archive)),DECODE,chosenLog);
        ok(chosenLog.toString().contains("模型主 INI：second.ini")&&chosenLog.toString().contains("未导入辅助 INI：Citlali.ini"),"conversion uses the chosen scheme: "+chosenLog);
        Files.delete(new File(src,"second.ini").toPath());refused=false;try{ModLibrary.mainIni(archive);}catch(IllegalArgumentException e){refused=e.getMessage().contains("已不存在");}ok(refused,"a removed scheme asks again");
        ModLibrary.setOverride(archive,"ini",null);GimiPatternsTest.put(src,"second.ini",text);
        // Conflicting duplicate model sections remain errors; only the known harmless control duplicates are merged.
        Files.delete(new File(src,"second.ini").toPath());GimiPatternsTest.put(src,"Citlali.ini",text+"\n[TextureOverrideCitlaliHead]\nib = ResourceCitlaliHeadIB\n");
        refused=false;try{new ModelConverter.Ini(src);}catch(IllegalArgumentException e){refused=e.getMessage().contains("重复 INI 段");}ok(refused,"do not merge conflicting geometry sections");
    }
    private static void animation(File base)throws Exception{
        String program="[Present]\nif $counter % $speed == 0\nif $frame < $last\n$frame = $frame + 1\nelse\n$frame = $first\nendif\nendif\npost $counter = $counter + 1\n";
        String text=GimiPatternsTest.ini("global $speed = 0.5\nglobal $counter = 0\nglobal $frame = 0\nglobal $first = 0\nglobal $last = 2\n",
            "if $frame == 1\nvb0 = ResourceNextPosition\nendif\n","","")+"[ResourceNextPosition]\nstride = 40\nfilename = Next.buf\n"+program;
        File src=GimiPatternsTest.mod(base,"animation",text),out=new File(base,"animation-out");byte[] next=Files.readAllBytes(new File(src,"CitlaliPosition.buf").toPath());ByteBuffer.wrap(next).order(ByteOrder.LITTLE_ENDIAN).putFloat(0,99);GimiPatternsTest.put(src,"Next.buf",next);
        StringBuilder log=new StringBuilder();ModelConverter.convert(src,null,out,10000107,new ModelConverter.Options().variants(true),DECODE,log);
        Map<String,Object> manifest=Json.obj(Json.read(new File(out,"model.json"),4L<<20),"model");List<Object> frames=Json.arr(Json.obj(manifest.get("animation"),"animation").get("frames"),"frames",3,3);
        ok(ModelManifest.read(out).version==4&&ModelManifest.read(out).vars.isEmpty()&&log.toString().contains("完整转换"),"animation must not become a variant union");
        ok(log.toString().contains("动画骨骼要求：Part0 / Body，无骨骼名映射，按索引绑定至少 ")&&log.toString().contains("转换成功不代表已通过游戏骨骼匹配"),"conversion must disclose the indexed skeleton requirement before device playback");
        Map<String,Object> part=Json.obj(Json.arr(Json.obj(frames.get(1),"frame").get("parts"),"parts",1,64).get(0),"part");
        ok(Fixtures.number(part,"vertices")==6&&Fixtures.view(out,Json.obj(part.get("data"),"data").get("positions")).getFloat(0)==99,"frame-local geometry is preserved");
        ok(Boolean.TRUE.equals(Json.obj(manifest.get("animation"),"animation").get("stableWeights")),"the dynamic mode must have an explicit immutable-weight guarantee");
        // Rotating triangles changes first-use vertex order. A weight permutation must move all attributes and indices together.
        String permuted=text.replace("if $frame == 1\nvb0 = ResourceNextPosition\nendif\n", "if $frame == 1\nvb0 = ResourceNextPosition\nendif\n")
            .replace("ib = ResourceCitlaliHeadIB\n","if $frame == 1\nib = ResourceNextHead\nelse\nib = ResourceCitlaliHeadIB\nendif\n")
            .replace("ib = ResourceCitlaliBodyIB\n","if $frame == 1\nib = ResourceNextBody\nelse\nib = ResourceCitlaliBodyIB\nendif\n")
            +"[ResourceNextHead]\nformat = R32_UINT\nfilename = NextHead.ib\n[ResourceNextBody]\nformat = R32_UINT\nfilename = NextBody.ib\n";
        ByteBuffer skin=ByteBuffer.wrap(Files.readAllBytes(new File(src,"CitlaliBlend.buf").toPath())).order(ByteOrder.LITTLE_ENDIAN);for(int i=0;i<6;i++)skin.putInt(i*32+16,i%2);
        GimiPatternsTest.put(src,"CitlaliBlend.buf",skin.array());GimiPatternsTest.put(src,"NextHead.ib",ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN).putInt(1).putInt(2).putInt(0).array());
        GimiPatternsTest.put(src,"NextBody.ib",ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN).putInt(4).putInt(5).putInt(3).array());GimiPatternsTest.put(src,"Citlali.ini",permuted);
        File aligned=new File(base,"aligned-animation");ModelConverter.convert(src,null,aligned,10000107,new ModelConverter.Options().variants(true),DECODE,new StringBuilder());
        Map<String,Object> alignedManifest=Json.obj(Json.read(new File(aligned,"model.json"),4L<<20),"model");List<Object> alignedFrames=Json.arr(Json.obj(alignedManifest.get("animation"),"animation").get("frames"),"frames",3,3);
        byte[] initialWeights=Fixtures.data(aligned,0,"weights").array();Map<String,Object> alignedPart=Json.obj(Json.arr(Json.obj(alignedFrames.get(1),"frame").get("parts"),"parts",1,1).get(0),"part"),data=Json.obj(alignedPart.get("data"),"data");
        ByteBuffer weights=Fixtures.view(aligned,data.get("weights")),positions=Fixtures.view(aligned,data.get("positions")),indices=Fixtures.view(aligned,data.get("indices"));
        ok(Arrays.equals(initialWeights,weights.array())&&positions.getFloat(indices.getInt(0)*12)==1&&positions.getFloat(indices.getInt(4)*12)==2&&positions.getFloat(indices.getInt(8)*12)==99,"weight order must be fixed without changing the drawn triangles");
        String generation=ModelManifest.read(out).generation;GimiPatternsTest.put(src,"Citlali.ini",text.replace("post $counter = $counter + 1","post $counter = $counter + 1\n$frame = 0"));boolean rejected=false;
        try{ModelConverter.convert(src,null,out,10000107,new ModelConverter.Options().variants(true).replace(true),DECODE,new StringBuilder());}catch(IllegalArgumentException e){rejected=e.getMessage().contains("Present 动画程序");}
        ok(rejected&&generation.equals(ModelManifest.read(out).generation),"unsupported programs fail before replacing a working conversion");
        GimiPatternsTest.put(src,"Citlali.ini",text);File models=new File(base,"animation-models");models.mkdir();
        Importer.Result result=Importer.convert(src,10000107,models,new File(base,"no-skeleton"),DECODE,new StringBuilder());ok(result.summary.contains("模型动画：3 帧"),"the import dialog must explain full animation conversion");
        // The additional blend groups of a baked animation are NOT additional observed game bones. Strict remains
        // the default. A per-mod opt-in can retain supported associations, with provenance and no geometry loss.
        File library=new File(base,"animation-library/10000107");library.mkdirs();Fixtures.skeleton(library,10000107,"Scene/Avatar_Girl_Catalyst_Citlali",new String[]{"Body"},new String[]{"BoneA","BoneB"},Collections.singletonMap("Body",Fixtures.target("Body",6,2,2)));
        for(int i=0;i<6;i++){skin.putFloat(i*32,1).putFloat(i*32+4,1).putInt(i*32+16,119).putInt(i*32+20,i%2);}GimiPatternsTest.put(src,"CitlaliBlend.buf",skin.array());
        rejected=false;try{ModelConverter.convert(src,library,out,10000107,new ModelConverter.Options().replace(true),DECODE,new StringBuilder());}catch(IllegalArgumentException e){rejected=e.getMessage().contains("骨骼索引越界");}
        ok(rejected&&generation.equals(ModelManifest.read(out).generation),"strict animated import must retain the previous working model");
        rejected=false;try{ModelConverter.convert(src,null,out,10000107,new ModelConverter.Options().replace(true).animationBoneFallback(true),DECODE,new StringBuilder());}catch(IllegalArgumentException e){rejected=e.getMessage().contains("需要完整骨骼库");}
        ok(rejected&&generation.equals(ModelManifest.read(out).generation),"fallback cannot guess a palette's valid boundary");
        byte[] originalBlend=skin.array().clone();StringBuilder fallbackLog=new StringBuilder();ModelConverter.convert(src,library,out,10000107,new ModelConverter.Options().replace(true).animationBoneFallback(true),DECODE,fallbackLog);
        Map<String,Object> fallbackManifest=Json.obj(Json.read(new File(out,"model.json"),4L<<20),"model"),fallbackAnimation=Json.obj(fallbackManifest.get("animation"),"animation");
        ok("retain-valid-experimental".equals(fallbackAnimation.get("bonePolicy"))&&fallbackLog.toString().contains("rawWeights=[1.0, 1.0")&&fallbackLog.toString().contains("animation-bone-fallback frame=2"),"lossy policy and original associations must be disclosed");
        for(Object fr:Json.arr(fallbackAnimation.get("frames"),"frames",3,3)){Map<String,Object> p=Json.obj(Json.arr(Json.obj(fr,"frame").get("parts"),"parts",1,1).get(0),"part");ByteBuffer w=Fixtures.view(out,Json.obj(p.get("data"),"data").get("weights"));
            ok(Fixtures.number(p,"vertices")==6&&Fixtures.number(p,"indices")==6,"fallback never removes new geometry");for(int i=0;i<6;i++)ok(w.getFloat(32*i)==1&&w.getInt(32*i+16)<2&&w.getFloat(32*i+4)==0,"each vertex keeps its own supported bone");}
        ok(Arrays.equals(originalBlend,Files.readAllBytes(new File(src,"CitlaliBlend.buf").toPath())),"source blend data remains untouched");
        String compatibleGeneration=ModelManifest.read(out).generation;skin.putFloat(4,0);GimiPatternsTest.put(src,"CitlaliBlend.buf",skin.array());rejected=false;
        try{ModelConverter.convert(src,library,out,10000107,new ModelConverter.Options().replace(true).animationBoneFallback(true),DECODE,new StringBuilder());}catch(IllegalArgumentException e){rejected=e.getMessage().contains("有效权重全部引用库外骨骼");}
        ok(rejected&&compatibleGeneration.equals(ModelManifest.read(out).generation),"all-extra vertices must fail, never borrow a neighbour's weight");
        GimiPatternsTest.put(src,"Citlali.ini",text+"\n[CustomShaderPose]\nvs = Pose.hlsl\n");rejected=false;try{ModelConverter.convert(src,library,out,10000107,new ModelConverter.Options().replace(true).animationBoneFallback(true),DECODE,new StringBuilder());}catch(IllegalArgumentException e){rejected=e.getMessage().contains("自定义着色器");}ok(rejected,"a custom shader requires its own semantics instead of this fallback");
        GimiPatternsTest.put(src,"Citlali.ini",text+"\n[CommandListPose]\nvs-t10 = ResourcePose\n");rejected=false;try{ModelConverter.convert(src,library,out,10000107,new ModelConverter.Options().replace(true).animationBoneFallback(true),DECODE,new StringBuilder());}catch(IllegalArgumentException e){rejected=e.getMessage().contains("自定义矩阵资源");}ok(rejected,"matrix resources cannot be ignored");
        GimiPatternsTest.put(src,"CitlaliBlend.buf",originalBlend);GimiPatternsTest.put(src,"Citlali.ini",text.replace(program,""));rejected=false;
        try{ModelConverter.convert(src,library,new File(base,"static-fallback-refused"),10000107,new ModelConverter.Options().animationBoneFallback(true),DECODE,new StringBuilder());}catch(IllegalArgumentException e){rejected=e.getMessage().contains("骨骼索引越界");}
        ok(rejected,"ordinary models keep strict validation even when the per-mod experiment is enabled");GimiPatternsTest.put(src,"Citlali.ini",text);
        File libraryRoot=library.getParentFile();ok(!Importer.animationBoneFallback(src),"per-mod fallback starts off");Importer.setAnimationBoneFallback(src,true);ok(Importer.animationBoneFallback(src),"per-mod option is read back");
        result=Importer.convert(src,10000107,models,libraryRoot,DECODE,new StringBuilder());ok(result.summary.contains("动画骨骼兼容：开启（实验）"),"import summary must disclose the experiment");
        Importer.setAnimationBoneFallback(src,false);rejected=false;try{Importer.convert(src,10000107,models,libraryRoot,DECODE,new StringBuilder());}catch(IllegalArgumentException e){rejected=e.getMessage().contains("骨骼索引越界");}ok(rejected&&!Importer.animationBoneFallback(src),"disabling restores strict conversion");
    }
    // A texture-only face override (GIMI FaceHeadDiffuse) becomes a material and a texture swap onto the game's Face renderer.
    // One that changes with a value only the face section reads is left out and said so; two face files at once are refused.
    private static void face(File base)throws Exception{
        String face="[TextureOverrideCitlaliFaceHeadDiffuse]\nhash = 55555555\nps-t0 = ResourceCitlaliFaceHeadDiffuse\n[ResourceCitlaliFaceHeadDiffuse]\nfilename = CitlaliFaceHeadDiffuse.dds\n";
        String text=GimiPatternsTest.ini("global $look = 0\n","","","")+face;File src=GimiPatternsTest.mod(base,"face",text);GimiPatternsTest.put(src,"CitlaliFaceHeadDiffuse.dds",new byte[128]);
        File out=new File(base,"face-out");StringBuilder log=new StringBuilder();ModelConverter.convert(src,null,out,10000107,new ModelConverter.Options().variants(true),DECODE,log);
        Map<String,Object> manifest=Json.obj(Json.read(new File(out,"model.json"),4L<<20),"model");Map<String,Object> swap=Json.obj(Json.arr(manifest.get("textures"),"textures",1,1).get(0),"texture");
        ok("Face".equals(swap.get("renderer"))&&"FaceTexture".equals(swap.get("material"))&&log.toString().contains("脸部贴图：CitlaliFaceHeadDiffuse.dds -> 游戏的 Face 渲染器"),"the face texture becomes a texture swap: "+log);
        boolean material=false;for(Object raw:Json.arr(manifest.get("materials"),"materials",1,64)){Map<String,Object> m=Json.obj(raw,"material");material|="FaceTexture".equals(m.get("name"))&&m.get("main")!=null;}
        ok(material,"the swap's material carries the face texture");
        String varying=GimiPatternsTest.ini("global $look = 0\n","","","")+"[TextureOverrideCitlaliFaceHeadDiffuse]\nhash = 55555555\nif $look == 0\nps-t0 = ResourceCitlaliFaceHeadDiffuse\nelse\nps-t0 = ResourceCitlaliFaceAlt\nendif\n"
            +"[ResourceCitlaliFaceHeadDiffuse]\nfilename = CitlaliFaceHeadDiffuse.dds\n[ResourceCitlaliFaceAlt]\nfilename = CitlaliFaceAlt.dds\n[KeyLook]\nkey = l\ntype = cycle\n$look = 0,1\n";
        GimiPatternsTest.put(src,"CitlaliFaceAlt.dds",new byte[128]);GimiPatternsTest.put(src,"Citlali.ini",varying);log=new StringBuilder();
        ModelConverter.convert(src,null,out,10000107,new ModelConverter.Options().variants(true).replace(true),DECODE,log);manifest=Json.obj(Json.read(new File(out,"model.json"),4L<<20),"model");
        ok(manifest.get("textures")==null&&log.toString().contains("脸部贴图随款式或动画帧变化，暂不导入"),"a face that changes with a value only it reads is left out: "+log);
        GimiPatternsTest.put(src,"Citlali.ini",text+"[TextureOverrideCitlaliFaceDiffuseTwo]\nhash = 66666666\nps-t0 = ResourceCitlaliFaceAlt\n[ResourceCitlaliFaceAlt]\nfilename = CitlaliFaceAlt.dds\n");boolean rejected=false;
        try{ModelConverter.convert(src,null,new File(base,"face-two"),10000107,new ModelConverter.Options().variants(true),DECODE,new StringBuilder());}catch(IllegalArgumentException e){rejected=e.getMessage().contains("脸部贴图有多个来源");}
        ok(rejected,"two face files in one assignment are refused");
        // A frame animation whose every frame binds the same face texture swaps it too (the Traveler mod's layout).
        String program="[Present]\nif $counter % $speed == 0\nif $frame < $last\n$frame = $frame + 1\nelse\n$frame = $first\nendif\nendif\npost $counter = $counter + 1\n";
        String animated=GimiPatternsTest.ini("global $speed = 0.5\nglobal $counter = 0\nglobal $frame = 0\nglobal $first = 0\nglobal $last = 2\n","if $frame == 1\nvb0 = ResourceNextPosition\nendif\n","","")
            +"[ResourceNextPosition]\nstride = 40\nfilename = Next.buf\n"+program+face;
        GimiPatternsTest.put(src,"Next.buf",Files.readAllBytes(new File(src,"CitlaliPosition.buf").toPath()));GimiPatternsTest.put(src,"Citlali.ini",animated);log=new StringBuilder();
        File frames=new File(base,"face-frames");ModelConverter.convert(src,null,frames,10000107,new ModelConverter.Options().variants(true),DECODE,log);manifest=Json.obj(Json.read(new File(frames,"model.json"),4L<<20),"model");
        ok(ModelManifest.read(frames).version==4&&Json.arr(manifest.get("textures"),"textures",1,1).size()==1&&log.toString().contains("脸部贴图：CitlaliFaceHeadDiffuse.dds"),"an animation keeps its face texture: "+log);
    }
    public static void main(String[] args)throws Exception{
        File base=Files.createTempDirectory("gibox-compatibility-").toFile();
        try{legacy(base);hohe(base);animation(base);face(base);System.out.println("Compatibility: PASS (face texture swap, layout precedence, variants, R16, main INI, duplicates, PC filename casing, TexFx ranges, ambiguity, INI schemes)");}
        finally{ModelConverter.remove(base);}
    }
}
