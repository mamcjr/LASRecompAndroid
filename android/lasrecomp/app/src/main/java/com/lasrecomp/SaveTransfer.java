package com.lasrecomp;

import java.io.*;
import java.nio.file.Files;
import java.util.*;
import java.util.zip.*;

// Plain Java so archive validation and replacement can also be tested without a device.
final class SaveTransfer {
    static final long MAX_BYTES = 64L << 20;
    static final int MAX_FILES = 128;

    static boolean saveName(String name) { return name.matches("savedata[0-9]+\\.bin"); }
    static boolean gameFile(String name) { return name.matches("[A-Za-z0-9_-]+\\.bin"); }

    static long copy(InputStream in, OutputStream out, long limit) throws IOException {
        if (in == null || out == null) throw new IOException("Could not open the selected file.");
        byte[] buf = new byte[65536];
        long count = 0;
        for (int n; (n = in.read(buf)) != -1;) {
            count += n;
            if (count > limit) throw new IOException("Save data exceeds the 64 MB limit.");
            out.write(buf, 0, n);
        }
        return count;
    }

    static void validate(File dir) throws IOException {
        File[] files = dir.listFiles();
        if (files == null) throw new IOException("Could not read the save folder.");
        boolean found = false;
        for (File f : files) if (saveName(f.getName())) {
            // The game's save version (also checked by runtime/ui/save_editor.mm). Never rewrite checksums.
            try (DataInputStream in = new DataInputStream(new FileInputStream(f))) {
                if (f.length() < 0x5584 || in.readInt() != 0x0e000000)
                    throw new IOException("Invalid Link's Awakening save: " + f.getName());
            }
            found = true;
        }
        if (!found) throw new IOException("No Link's Awakening saves found. Select the folder containing savedata0.bin, or a ZIP of that folder.");
    }

    static void unpack(File archive, File stage) throws IOException {
        try (ZipFile zip = new ZipFile(archive)) {
            String root = null;
            Set<String> names = new HashSet<>();
            List<ZipEntry> entries = new ArrayList<>();
            Enumeration<? extends ZipEntry> all = zip.entries();
            while (all.hasMoreElements()) {
                if (entries.size() == 512) throw new IOException("Select a ZIP containing only this game's saves.");
                entries.add(all.nextElement());
            }
            for (ZipEntry e : entries) {
                String path = e.getName();
                if (path.startsWith("/") || path.contains("\\") || path.contains(":") ||
                    Arrays.asList(path.split("/")).contains("..") || Arrays.asList(path.split("/")).contains(".") ||
                    !names.add(path)) throw new IOException("Unsafe or duplicate ZIP entry.");
                if (e.isDirectory()) continue;
                int slash = path.lastIndexOf('/');
                if (saveName(path.substring(slash + 1))) {
                    String parent = path.substring(0, slash + 1);
                    if (root != null && !root.equals(parent))
                        throw new IOException("ZIP contains multiple save folders. Export just one profile's saves.");
                    root = parent;
                }
            }
            if (root == null) throw new IOException("No savedata files found in the ZIP.");
            long total = 0;
            int count = 0;
            for (ZipEntry e : entries) {
                String path = e.getName();
                int slash = path.lastIndexOf('/');
                String name = path.substring(slash + 1);
                if (e.isDirectory() || !path.substring(0, slash + 1).equals(root) || !gameFile(name)) continue;
                if (++count > MAX_FILES) throw new IOException("Too many save files.");
                try (CheckedInputStream in = new CheckedInputStream(zip.getInputStream(e), new CRC32());
                     OutputStream out = new FileOutputStream(new File(stage, name))) {
                    total += copy(in, out, MAX_BYTES - total);
                    if (in.getChecksum().getValue() != e.getCrc()) throw new IOException("Damaged ZIP entry: " + name);
                }
            }
        }
        validate(stage);
    }

    static File previous(File target) { return new File(target.getParentFile(), target.getName() + ".previous"); }

    static void recover(File target) throws IOException {
        File previous = previous(target);
        if (!target.exists() && previous.exists() && !previous.renameTo(target))
            throw new IOException("Could not restore the previous saves.");
    }

    // stage and target are siblings on the same filesystem. Keep the old directory as a recoverable backup.
    static void install(File stage, File target) throws IOException {
        validate(stage);
        recover(target);
        File previous = previous(target);
        if (previous.exists()) {
            File history = new File(target.getParentFile(), target.getName() + ".backup-" + UUID.randomUUID());
            if (!previous.renameTo(history)) throw new IOException("Could not preserve the previous backup.");
        }
        boolean hadSaves = target.exists();
        if (hadSaves && !target.renameTo(previous)) throw new IOException("Could not back up the current saves.");
        if (!stage.renameTo(target)) {
            if (hadSaves && !previous.renameTo(target))
                throw new IOException("Import failed. Previous saves are safe in " + previous.getName() + "; restart the app to restore them.");
            throw new IOException("Could not install saves. Current saves were preserved.");
        }
    }

    static void exportZip(File source, OutputStream output) throws IOException {
        validate(source);
        File[] files = source.listFiles(f -> f.isFile() && gameFile(f.getName()));
        if (files == null || files.length > MAX_FILES) throw new IOException("Could not read the save files.");
        Arrays.sort(files);
        // No save_0 wrapper: these files go directly into the emulator's per-game save folder.
        try (ZipOutputStream zip = new ZipOutputStream(output)) {
            long total = 0;
            for (File file : files) {
                zip.putNextEntry(new ZipEntry(file.getName()));
                try (InputStream in = new FileInputStream(file)) { total += copy(in, zip, MAX_BYTES - total); }
                zip.closeEntry();
            }
        }
    }

    static void discard(File dir) throws IOException {
        if (!dir.exists()) return;
        File[] files = dir.listFiles();
        if (files != null) for (File f : files) Files.delete(f.toPath());
        Files.delete(dir.toPath());
    }
}
