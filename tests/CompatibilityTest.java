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
    public static void main(String[] args)throws Exception{
        File base=Files.createTempDirectory("gibox-compatibility-").toFile();
        try{legacy(base);hohe(base);System.out.println("Compatibility: PASS (layout precedence, variants, R16, main INI, duplicates, PC filename casing, TexFx ranges, ambiguity, INI schemes)");}
        finally{ModelConverter.remove(base);}
    }
}
