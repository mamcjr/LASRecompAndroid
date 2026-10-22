#!/usr/bin/env python3
"""Exercise the production import-space policy on a PC JDK, without Android or a phone."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build/import-storage-tests'
out.mkdir(parents=True, exist_ok=True)
harness = out / 'ImportStorageTest.java'
harness.write_text('''
package com.lasrecomp;
import java.io.File;
import java.util.Locale;
public final class ImportStorageTest {
    public static void main(String[] args) throws Exception {
        long reserve = (2L << 30) + (1L << 28);
        if (ImportStorage.requiredBytes(100, 1000) != reserve + 1100) throw new AssertionError("exefs + image + reserve");
        if (ImportStorage.requiredBytes(0, 0) != reserve) throw new AssertionError("workspace reserve");
        for (long[] sizes : new long[][] {{-1, 0}, {0, -1}, {0, Long.MAX_VALUE}, {Long.MAX_VALUE, 1}}) {
            boolean rejected = false;
            try { ImportStorage.requiredBytes(sizes[0], sizes[1]); }
            catch (IllegalArgumentException | ArithmeticException expected) { rejected = true; }
            if (!rejected) throw new AssertionError("invalid/overflow estimate accepted");
        }
        Locale.setDefault(Locale.FRANCE);
        String message = ImportStorage.spaceMessage(3L << 30, 1L << 29);
        if (!message.contains("3.0 GiB") || !message.contains("0.5 GiB")) throw new AssertionError("space details");
        File target = new File(args[0], "extracted");
        if (!ImportStorage.isInside(target, target)) throw new AssertionError("same source/destination");
        if (!ImportStorage.isInside(new File(target, "romfs"), target)) throw new AssertionError("nested source");
        if (ImportStorage.isInside(new File(args[0], "extracted-backup"), target)) throw new AssertionError("sibling prefix");
        if (!ImportStorage.isInside(new File(target, "romfs/../exefs"), target)) throw new AssertionError("normalized source");
        System.out.println("11 import storage checks passed (production Java)");
    }
}
''', encoding='utf-8')
source = root / 'android/lasrecomp/app/src/main/java/com/lasrecomp/ImportStorage.java'
subprocess.run(['javac', '-d', str(out), str(source), str(harness)], check=True)
subprocess.run(['java', '-cp', str(out), 'com.lasrecomp.ImportStorageTest', str(out)], check=True)
