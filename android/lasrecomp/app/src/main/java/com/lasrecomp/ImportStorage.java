package com.lasrecomp;

import java.util.Locale;
import java.io.File;
import java.io.IOException;

/** Conservative additional-space estimate: exefs copy, RomFS image (built straight from the picked folder; the few
 *  loose files beside it fit the reserve), build workspace and metadata. */
final class ImportStorage {
    private static final long GIB = 1L << 30;
    static boolean isInside(File source, File destination) throws IOException {
        return source.getCanonicalFile().toPath().startsWith(destination.getCanonicalFile().toPath());
    }
    static long requiredBytes(long exefs, long romfs) {
        if (exefs < 0 || romfs < 0) throw new IllegalArgumentException("Unknown game file size");
        return Math.addExact(Math.addExact(exefs, romfs), 2 * GIB + (GIB / 4));
    }
    static String spaceMessage(long needed, long available) {
        return String.format(Locale.ROOT,
                "Not enough free space for setup. Estimated additional space: %.1f GiB; available: %.1f GiB. " +
                "Setup builds a data image from the game files and compiles the game code. " +
                "Free space and select the folder again. Existing game files have not been removed.",
                needed / (double)GIB, available / (double)GIB);
    }
}
