package com.gibox.tool;

import java.io.File;
import java.io.PrintStream;
import java.lang.reflect.Method;
import java.util.*;

/** Host-only: what the old converter makes of a Present frame-loop MOD, for migoto_parity to compare the new logic
 *  core against. For every frame value the old path converts, the draws it resolves (part, IB, the three vertex
 *  streams, ps-t0..2) and the face texture; plus the loop parameters the old runtime FrameLoop advances with. It
 *  reads only the INI: buffers and textures need not be present (their file names are enough). */
public final class LegacyAnimationDump {
    private static String name(Map<String,String> b,String key){String v=b.get(key);return v==null?"-":v.trim();}
    public static void main(String[] a)throws Exception{
        File root=new File(a[0]).getCanonicalFile(),ini=new File(root,a[1]);
        ModelConverter.Ini x=new ModelConverter.Ini(root,ini);ModelConverter.Animation an=ModelConverter.Animation.parse(x);
        if(an==null)throw new IllegalArgumentException("the old converter finds no Present animation in "+ini);
        Method resources=ModelConverter.class.getDeclaredMethod("resources",ModelConverter.Ini.class,File.class,Map.class);resources.setAccessible(true);
        Method drawCalls=ModelConverter.class.getDeclaredMethod("drawCalls",ModelConverter.Ini.class,Map.class,Map.class,Map.class);drawCalls.setAccessible(true);
        Method face=ModelConverter.class.getDeclaredMethod("faceTexture",ModelConverter.Ini.class,Map.class,Map.class);face.setAccessible(true);
        try(PrintStream out=new PrintStream(a[2],"UTF-8")){
            StringBuilder values=new StringBuilder();for(int v:an.values)values.append(' ').append(v);
            out.println("animation "+an.variable+" "+an.first+" "+an.last+" "+an.step+" "+an.initial+" "+an.counter+" "+an.period+" "+an.counterStep+values);
            for(int v:an.values){Map<String,Integer> vars=new LinkedHashMap<>(x.vars);vars.put(an.variable,v);
                @SuppressWarnings("unchecked") Map<String,Object> rs=(Map<String,Object>)resources.invoke(null,x,root,vars);
                @SuppressWarnings("unchecked") List<ModelConverter.Draw> draws=(List<ModelConverter.Draw>)drawCalls.invoke(null,x,vars,rs,new HashMap<String,String>());
                for(ModelConverter.Draw d:draws)out.println("draw "+v+" "+d.part+" "+d.start+" "+d.count+" ib="+name(d.bindings,"ib")+" position="+name(d.bindings,"vertex-stream:0")
                    +" blend="+name(d.bindings,"vertex-stream:1")+" texcoord="+name(d.bindings,"vertex-stream:2")+" ps-t0="+name(d.bindings,"ps-t0")+" ps-t1="+name(d.bindings,"ps-t1")+" ps-t2="+name(d.bindings,"ps-t2"));
                ModelConverter.Resource f=(ModelConverter.Resource)face.invoke(null,x,vars,rs);
                out.println("face "+v+" "+(f==null?"-":root.toPath().relativize(f.file.toPath()).toString().replace('\\','/')));
            }
        }
    }
}
