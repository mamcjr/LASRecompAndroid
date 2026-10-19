package com.lasrecomp;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

// Run with javac/java; synthetic save bytes only, no Android SDK or game files required.
public final class SaveTransferTest {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private interface Operation { void run() throws Exception; }
    private static void fails(Operation operation) throws Exception {
        try { operation.run(); } catch (IOException expected) { checks++; return; }
        throw new AssertionError("Expected IOException");
    }
    private static byte[] save(int marker) {
        byte[] bytes = new byte[0x5600];
        bytes[0] = 14;
        bytes[0x5500] = (byte)marker;
        return bytes;
    }
    private static File folder(File root, String name) throws IOException {
        File dir = new File(root, name);
        Files.createDirectories(dir.toPath());
        return dir;
    }
    private static void put(File dir, String name, byte[] bytes) throws IOException {
        Files.write(new File(dir, name).toPath(), bytes);
    }
    private static File zip(File root, String name, String[] paths, byte[][] bytes) throws IOException {
        File file = new File(root, name);
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(file))) {
            for (int i = 0; i < paths.length; i++) {
                out.putNextEntry(new ZipEntry(paths[i]));
                out.write(bytes[i]);
                out.closeEntry();
            }
        }
        return file;
    }
    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("las-save-test-").toFile();
        try {
            File source = folder(root, "source");
            put(source, "savedata0.bin", save(1));
            put(source, "savedata1.bin", save(2));
            put(source, "screenshot0.bin", new byte[]{1, 2, 3});
            put(source, "system.bin", new byte[]{4, 5});
            put(source, "savedata0.bin.flagsx", new byte[]{99});
            File exported = new File(root, "export.zip");
            SaveTransfer.exportZip(source, new FileOutputStream(exported));
            try (ZipFile zip = new ZipFile(exported)) {
                check(zip.size() == 4, "Only the game's BIN files are exported");
                check(zip.getEntry("savedata0.bin") != null, "Export has no save_0 wrapper");
            }
            File stage = folder(root, "roundtrip");
            SaveTransfer.unpack(exported, stage);
            for (String name : new String[]{"savedata0.bin", "savedata1.bin", "screenshot0.bin", "system.bin"})
                check(Arrays.equals(Files.readAllBytes(new File(source, name).toPath()), Files.readAllBytes(new File(stage, name).toPath())), "Byte-for-byte roundtrip: " + name);

            File nested = zip(root, "nested.zip", new String[]{"01006BB00C6F0000/0/savedata0.bin", "01006BB00C6F0000/0/screenshot0.bin", "readme.txt"}, new byte[][]{save(3), {7}, {8}});
            File nestedStage = folder(root, "nested-stage");
            SaveTransfer.unpack(nested, nestedStage);
            check(new File(nestedStage, "screenshot0.bin").exists(), "Wrapped emulator archive includes screenshots");
            check(!new File(nestedStage, "readme.txt").exists(), "Unrelated archive files ignored");
            File multiple = zip(root, "multiple.zip", new String[]{"user1/savedata0.bin", "user2/savedata0.bin"}, new byte[][]{save(1), save(2)});
            fails(() -> SaveTransfer.unpack(multiple, folder(root, "multi-stage")));
            File traversal = zip(root, "traversal.zip", new String[]{"../savedata0.bin"}, new byte[][]{save(1)});
            fails(() -> SaveTransfer.unpack(traversal, folder(root, "traversal-stage")));
            File absolute = zip(root, "absolute.zip", new String[]{"/savedata0.bin"}, new byte[][]{save(1)});
            fails(() -> SaveTransfer.unpack(absolute, folder(root, "absolute-stage")));
            File invalid = zip(root, "invalid.zip", new String[]{"savedata0.bin"}, new byte[][]{new byte[0x5600]});
            fails(() -> SaveTransfer.unpack(invalid, folder(root, "invalid-stage")));
            File shortSave = zip(root, "short.zip", new String[]{"savedata0.bin"}, new byte[][]{{14, 0, 0, 0}});
            fails(() -> SaveTransfer.unpack(shortSave, folder(root, "short-stage")));
            File empty = folder(root, "empty");
            fails(() -> SaveTransfer.validate(empty));
            fails(() -> SaveTransfer.copy(new ByteArrayInputStream(new byte[20]), new ByteArrayOutputStream(), 10));
            fails(() -> SaveTransfer.copy(null, new ByteArrayOutputStream(), 10));

            File duplicate = zip(root, "duplicate.zip", new String[]{"savedata0.bin", "savedata1.bin"}, new byte[][]{save(1), save(2)});
            byte[] duplicateBytes = Files.readAllBytes(duplicate.toPath());
            byte[] oldName = "savedata1.bin".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
            for (int i = 0; i <= duplicateBytes.length - oldName.length; i++) {
                if (Arrays.equals(Arrays.copyOfRange(duplicateBytes, i, i + oldName.length), oldName)) duplicateBytes[i + 8] = '0';
            }
            Files.write(duplicate.toPath(), duplicateBytes);
            fails(() -> SaveTransfer.unpack(duplicate, folder(root, "duplicate-stage")));
            File damaged = zip(root, "damaged.zip", new String[]{"savedata0.bin"}, new byte[][]{save(1)});
            byte[] damagedBytes = Files.readAllBytes(damaged.toPath());
            for (int i = 0; i < damagedBytes.length - 20; i++) {
                if (damagedBytes[i] == 0x50 && damagedBytes[i + 1] == 0x4b && damagedBytes[i + 2] == 1 && damagedBytes[i + 3] == 2) {
                    damagedBytes[i + 16] ^= 1; // corrupt the central directory's CRC, leaving the payload readable
                    break;
                }
            }
            Files.write(damaged.toPath(), damagedBytes);
            fails(() -> SaveTransfer.unpack(damaged, folder(root, "damaged-stage")));

            File target = folder(root, "save_0");
            put(target, "savedata0.bin", save(9));
            put(target, "savedata2.bin", save(9));
            put(target, "savedata0.bin.flagsx", new byte[]{42});
            SaveTransfer.install(stage, target);
            check(!new File(target, "savedata2.bin").exists(), "Imports replace all slots rather than mixing profiles");
            check(!new File(target, "savedata0.bin.flagsx").exists(), "Old randomizer flags do not leak into imported saves");
            File previous = SaveTransfer.previous(target);
            check(new File(previous, "savedata2.bin").exists(), "Previous slots backed up");
            check(new File(previous, "savedata0.bin.flagsx").exists(), "Backup retains sidecars");
            byte[] before = Files.readAllBytes(new File(target, "savedata0.bin").toPath());
            fails(() -> SaveTransfer.install(empty, target));
            check(Arrays.equals(before, Files.readAllBytes(new File(target, "savedata0.bin").toPath())), "Invalid import leaves current saves untouched");
            SaveTransfer.install(nestedStage, target);
            check(root.listFiles(f -> f.getName().startsWith("save_0.backup-")).length == 1, "Older backup retained after another import");
            check(Arrays.equals(before, Files.readAllBytes(new File(previous, "savedata0.bin").toPath())), "Latest previous backup matches replaced saves");

            File interrupted = new File(root, "interrupted");
            File interruptedPrevious = folder(root, "interrupted.previous");
            put(interruptedPrevious, "savedata0.bin", save(5));
            SaveTransfer.recover(interrupted);
            check(new File(interrupted, "savedata0.bin").exists() && !interruptedPrevious.exists(), "Interrupted rename recovers previous saves");
            SaveTransfer.recover(target);
            check(previous.exists(), "Completed import keeps its backup");
            System.out.println("SaveTransfer: " + checks + " checks passed");
        } finally {
            try (java.util.stream.Stream<Path> files = Files.walk(root.toPath())) {
                for (Path path : files.sorted(Comparator.reverseOrder()).toArray(Path[]::new)) Files.delete(path);
            }
        }
    }
}
