package com.gibox.tool;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.FileVisitResult;
import java.nio.file.attribute.BasicFileAttributes;

/** Canonical-path boundary shared by the file manager bridge and its host checks. */
final class DocumentPaths {
    static File resolve(File root, String relative) throws FileNotFoundException {
        try {
            if (relative.startsWith("/") || relative.indexOf('\\') >= 0) throw new IOException("Invalid document path");
            File base = root.getCanonicalFile(), file = new File(base, relative).getCanonicalFile();
            if (!file.equals(base) && !file.getPath().startsWith(base.getPath() + File.separator)) throw new IOException("Document outside root");
            return file;
        } catch (IOException e) { throw new FileNotFoundException(e.getMessage()); }
    }
    /**
     * The system's own document ID for a folder on shared storage (/storage/emulated/0/x/y → "primary:x/y"), which
     * every file manager understands; null for anything else (the app's private Mods folder).
     */
    static String externalDocumentId(File folder) {
        String p = folder.getAbsolutePath(), base = "/storage/emulated/0";
        if (p.equals(base)) return "primary:";
        return p.startsWith(base + "/") && !p.contains("/../") && !p.endsWith("/..") ? "primary:" + p.substring(base.length() + 1) : null;
    }
    static String name(String name) throws FileNotFoundException {
        if (name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0)
            throw new FileNotFoundException("Invalid document name");
        return name;
    }
    static void deleteTree(File file) throws IOException {
        // File managers must not follow a nested symlink out of a granted directory when deleting its parent.
        Files.walkFileTree(file.toPath(), new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path path, BasicFileAttributes attrs) throws IOException {
                Files.delete(path); return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path path, IOException error) throws IOException {
                if(error!=null) throw error; Files.delete(path); return FileVisitResult.CONTINUE;
            }
        });
    }
}
