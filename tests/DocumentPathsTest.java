package com.gibox.tool;
import java.io.*;
import java.nio.file.*;
public final class DocumentPathsTest {
    interface Checked {void run() throws Exception;}
    static void rejects(Checked job)throws Exception {try{job.run();throw new AssertionError("unsafe path accepted");}catch(FileNotFoundException expected){}}
    public static void main(String[] args)throws Exception {
        Path temp=Files.createTempDirectory("gibox-documents"),root=Files.createDirectory(temp.resolve("mods")),outside=Files.createDirectory(temp.resolve("outside"));
        try {
            Path keep=outside.resolve("keep.txt");Files.write(keep,new byte[]{1});
            if(!DocumentPaths.resolve(root.toFile(),"mod/model.json").equals(root.resolve("mod/model.json").toFile()))throw new AssertionError("normal child");
            rejects(()->DocumentPaths.resolve(root.toFile(),"../outside/keep.txt"));
            if(!"primary:Android/media/com.miHoYo.Yuanshen/GIBox/models".equals(DocumentPaths.externalDocumentId(new File("/storage/emulated/0/Android/media/com.miHoYo.Yuanshen/GIBox/models")))
               ||DocumentPaths.externalDocumentId(new File("/data/user/0/com.gibox.tool/files/mods/x"))!=null||DocumentPaths.externalDocumentId(new File("/storage/emulated/0/../x"))!=null)throw new AssertionError("external document id");
            rejects(()->DocumentPaths.resolve(root.toFile(),"/etc/passwd"));
            rejects(()->DocumentPaths.name("../other"));rejects(()->DocumentPaths.name(".."));
            Path dir=Files.createDirectory(root.resolve("mod"));Files.createSymbolicLink(dir.resolve("link"),outside);
            rejects(()->DocumentPaths.resolve(root.toFile(),"mod/link/keep.txt"));
            DocumentPaths.deleteTree(dir.toFile());
            if(!Files.exists(keep)||Files.exists(dir))throw new AssertionError("delete followed a symlink out of the granted directory");
        } finally {DocumentPaths.deleteTree(temp.toFile());}
        System.out.println("Document access: PASS (canonical root, traversal/symlink rejection, recursive deletion never follows links)");
    }
}
