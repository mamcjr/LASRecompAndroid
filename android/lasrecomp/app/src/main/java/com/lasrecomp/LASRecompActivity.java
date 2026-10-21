// LAS Recomp on Android: SDL's activity runs the runtime (libmain.so, runtime/main.c). Before the native code starts
// this sets up what the desktop finds next to its executable (runtime/platform/android.c, runtime/paths.c):
// - the shipped resources (assets/res: the start screen's font, the icon) copied to <internal files>/res;
// - the Vulkan driver: the first driver in <external files>/drivers (a .so, or an adrenotools .zip: Turnip), copied to
//   internal storage (dlopen refuses external storage) and named in LARECOMP_VK_DRIVER; none there = the system's;
// - LARECOMP_NATIVE_LIB_DIR: where adrenotools finds its hook libraries;
// - the game code (lasgame.so + lasgame.so.id, built from the player's own game files: android/build_gamelib.sh) from
//   <external files>/extracted to <internal files>/res, where runtime/gamelib.c looks for it next to "the executable".
// - the game code's builder (docs/ANDROID-PORT.md, Fase 6): assets/gen (the generator, its Python's standard library,
//   zig's headers) unpacked to <internal files>/gen once per app version, named in LARECOMP_GEN_DIR; its programs
//   (libpymain.so, libzig.so) are in nativeLibraryDir;
// - the system's pickers for the start screen (runtime/ui/launcher.mm): the game files folder (exefs/ and romfs/ are
//   found in it and copied to <external files>/extracted) and the Vulkan driver (a .zip copied to the drivers folder).
// The data root (game files, saves, settings) is <external files>: /sdcard/Android/data/com.lasrecomp.android/files.
// While in front it reports the battery and thermal state once a second for the performance overlay
// (runtime/ui/perf_overlay.mm): apps can't read those in /sys.
package com.lasrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.content.IntentFilter;
import android.content.res.AssetManager;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class LASRecompActivity extends SDLActivity {
    private static final String TAG = "LASRecomp";

    private final Handler stats = new Handler(Looper.getMainLooper());
    private final Runnable report = new Runnable() {
        @Override
        public void run() {
            reportDeviceStats();
            stats.postDelayed(this, 1000);
        }
    };

    private static native void nativeDeviceStats(float battTempC, float battPct, float watts, int thermal, float headroom);
    // the start screen (runtime/ui/launcher.mm)
    private static native boolean nativeBuildIdOk(byte[] head);
    private static native void nativeImportBegin(String name);
    private static native void nativeImportProgress(long done, long total);
    private static native void nativeImportDone(int result);
    private static native void nativeImportFailure(String reason);
    private static native void nativeImportChecking();
    private static native void nativeDriverPicked(String name);
    private static native void nativeSaveTransferDone(String message);

    private static final int REQ_GAME = 0x4c41, REQ_DRIVER = 0x4c42;
    private static final int REQ_SAVE_FOLDER = 0x4c43, REQ_SAVE_ZIP = 0x4c44, REQ_SAVE_EXPORT = 0x4c45;
    private File saveTarget;
    private boolean exportPrevious;
    private static LASRecompActivity self;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "shaderc_shared", "main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        try {
            File res = new File(getFilesDir(), "res");
            copyAssets("res", res);
            File games = new File(getExternalFilesDir(null), "extracted");
            copyIfChanged(new File(games, "lasgame.so"), new File(res, "lasgame.so"));
            copyIfChanged(new File(games, "lasgame.so.id"), new File(res, "lasgame.so.id"));
            setenv("LARECOMP_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir);
            File gen = new File(getFilesDir(), "gen");
            unpackGen(gen);
            setenv("LARECOMP_GEN_DIR", gen.getAbsolutePath());
            if (getIntent() != null && SYSTEM_DRIVER.equals(getIntent().getAction())) dropCustomDriver();
            File driver = customDriver();
            setenv("LARECOMP_VK_DRIVER", driver != null ? driver.getAbsolutePath() : "");
            // testing: the runtime's LARECOMP_* switches from the launch intent, separated by commas (adb shell drops
            // the quotes around spaces), e.g. LARECOMP_VK_DRIVER= is the system driver:
            // adb shell am start -n com.lasrecomp.android/com.lasrecomp.LASRecompActivity --es env LARECOMP_NO_LAUNCHER=1,LARECOMP_VK_DRIVER=
            String env = getIntent() != null ? getIntent().getStringExtra("env") : null;
            if (env != null) {  // (a piece without '=' continues the previous value: LARECOMP_GPUPROF_AT=1500,1750)
                String name = null, value = null;
                for (String kv : env.trim().split("[,\\s]+")) {
                    int eq = kv.indexOf('=');
                    if (eq > 0) {
                        if (name != null) setenv(name, value);
                        name = kv.substring(0, eq);
                        value = kv.substring(eq + 1);
                    } else if (name != null) value += "," + kv;
                }
                if (name != null) setenv(name, value);
                // (debugging) the game code built on the phone, out where adb can read it: a crash's host addresses
                if (env.contains("LARECOMP_EXPORT_GAMELIB")) {
                    File lib = new File(res, "lasgame.so");
                    if (lib.exists()) try (InputStream in = new FileInputStream(lib)) {
                        copy(in, new File(getExternalFilesDir(null), "lasgame-export.so"));
                    }
                }
            }
        } catch (IOException | ErrnoException e) {
            Log.e(TAG, "setup: " + e);
        }
        self = this;
        super.onCreate(state);
        // SDL's own immersive full screen (no status / navigation bar, the camera cutout used): SDL's onCreate asks
        // for the windowed style, and keeps the bars hidden once its full screen mode is on
        setWindowStyle(true);
    }

    @Override
    protected void onResume() {
        super.onResume();
        stats.post(report);
    }

    @Override
    protected void onPause() {
        stats.removeCallbacks(report);
        super.onPause();
    }

    // Battery temperature, charge and power draw, and Android's thermal state (runtime/platform/android.c).
    private void reportDeviceStats() {
        try {
            Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            float temp = -1, pct = -1, watts = 0;
            if (b != null) {
                temp = b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -10) / 10f;
                int level = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = b.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
                if (level >= 0 && scale > 0) pct = level * 100f / scale;
                int mv = b.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0);
                BatteryManager bm = getSystemService(BatteryManager.class);
                long now = bm != null ? Math.abs(bm.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)) : 0;
                if (now > 0 && now < 20000) now *= 1000;  // (some devices report mA instead of uA)
                watts = now * (float) mv / 1e9f;
            }
            PowerManager pm = getSystemService(PowerManager.class);
            int thermal = pm != null ? pm.getCurrentThermalStatus() : -1;
            float headroom = pm != null ? pm.getThermalHeadroom(10) : -1;
            nativeDeviceStats(temp, pct, watts, thermal, Float.isNaN(headroom) ? -1 : headroom);
        } catch (Throwable t) {  // (also before libmain is loaded: UnsatisfiedLinkError)
            Log.w(TAG, "device stats: " + t);
        }
    }

    // ---- the game code's builder: assets/gen, unpacked only when its bundled content changes (runtime/gamebuild.c)
    private void unpackGen(File dir) throws IOException {
        String[] top = getAssets().list("gen");
        if (top == null || top.length == 0) return;  // (an APK without it: android/deps/build_gen_deps.sh not run)
        String stamp;
        try (InputStream in = getAssets().open("gen.id")) {
            stamp = "sha256:" + new String(in.readAllBytes(), java.nio.charset.StandardCharsets.US_ASCII).trim();
        } catch (IOException missingFingerprint) {
            // Compatibility with APKs built before gen.id was introduced.
            try {
                stamp = "install:" + getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            } catch (Exception e) {
                throw new IOException("Could not identify the bundled game builder", e);
            }
        }
        File st = new File(dir, ".stamp");
        if (st.exists() && stamp.equals(readSmall(st))) return;
        deleteTree(dir);
        copyAssetTree("gen", dir);
        try (OutputStream o = new FileOutputStream(st)) {
            o.write(stamp.getBytes());
        }
        Log.i(TAG, "unpacked the game code builder to " + dir);
    }

    private void copyAssetTree(String from, File to) throws IOException {
        AssetManager am = getAssets();
        String[] names = am.list(from);
        if (names == null || names.length == 0) {  // a file (or an empty folder)
            try (InputStream in = am.open(from)) {
                to.getParentFile().mkdirs();
                copy(in, to);
            } catch (IOException e) {
                to.mkdirs();
            }
            return;
        }
        to.mkdirs();
        for (String n : names) copyAssetTree(from + "/" + n, new File(to, n));
    }

    // ---- pickers (called from the start screen, on SDL's thread)
    public static void pickGameFolder() {
        self.runOnUiThread(() -> {
            try {
                self.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_GAME);
            } catch (Exception e) {
                Log.e(TAG, "folder picker: " + e);
                nativeImportDone(1);
            }
        });
    }

    public static void pickDriver() {
        self.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            try {
                self.startActivityForResult(i, REQ_DRIVER);
            } catch (Exception e) {
                Log.e(TAG, "file picker: " + e);
            }
        });
    }

    // Called only from prelaunch settings. Native code holds PLAY until the picker/worker completes.
    // 0: folder import, 1: ZIP import, 2: ZIP export, 3: previous saves ZIP export.
    public static void transferSaves(String path, int mode) {
        LASRecompActivity activity = self;
        activity.runOnUiThread(() -> {
            activity.saveTarget = new File(path);
            activity.exportPrevious = mode == 3;
            Runnable pick = () -> {
                try {
                    Intent intent;
                    int request;
                    if (mode == 0) {
                        intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                        request = REQ_SAVE_FOLDER;
                    } else {
                        intent = new Intent(mode == 1 ? Intent.ACTION_OPEN_DOCUMENT : Intent.ACTION_CREATE_DOCUMENT);
                        intent.addCategory(Intent.CATEGORY_OPENABLE);
                        intent.setType(mode == 1 ? "*/*" : "application/zip");
                        request = mode == 1 ? REQ_SAVE_ZIP : REQ_SAVE_EXPORT;
                        if (mode != 1) intent.putExtra(Intent.EXTRA_TITLE, mode == 3 ? "Links-Awakening-previous-saves.zip" : "Links-Awakening-saves.zip");
                    }
                    activity.startActivityForResult(intent, request);
                } catch (Exception e) {
                    nativeSaveTransferDone("Could not open the save picker: " + e.getMessage());
                }
            };
            if (mode < 2) {
                new AlertDialog.Builder(activity).setTitle("Import saves?")
                    .setMessage("This replaces all current save slots. A backup is kept and can be exported with Export previous saves.")
                    .setPositiveButton("Continue", (dialog, which) -> pick.run())
                    .setNegativeButton("Cancel", (dialog, which) -> nativeSaveTransferDone("Import cancelled."))
                    .setOnCancelListener(dialog -> nativeSaveTransferDone("Import cancelled.")).show();
            } else pick.run();
        });
    }

    private void transferSaveFiles(Uri uri, int request, File target, boolean previous) {
        File stage = null, archive = null;
        try {
            if (!target.getParentFile().isDirectory() && !target.getParentFile().mkdirs())
                throw new IOException("Could not create the saves folder.");
            SaveTransfer.recover(target);
            if (request == REQ_SAVE_EXPORT) {
                // Build locally first, so invalid saves never leave a partly built archive in the destination.
                archive = File.createTempFile("save-export-", ".zip", getCacheDir());
                try (OutputStream out = new FileOutputStream(archive)) {
                    SaveTransfer.exportZip(previous ? SaveTransfer.previous(target) : target, out);
                }
                try (InputStream in = new FileInputStream(archive);
                     OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
                    SaveTransfer.copy(in, out, SaveTransfer.MAX_BYTES + (1 << 20));
                }
                nativeSaveTransferDone("Saves exported. Extract the ZIP into Link's Awakening's save folder in Eden, Yuzu or Ryujinx.");
            } else {
                stage = java.nio.file.Files.createTempDirectory(target.getParentFile().toPath(), ".save-import-").toFile();
                if (request == REQ_SAVE_ZIP) {
                    archive = File.createTempFile("save-import-", ".zip", getCacheDir());
                    try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(archive)) {
                        SaveTransfer.copy(in, out, SaveTransfer.MAX_BYTES + (1 << 20));
                    }
                    SaveTransfer.unpack(archive, stage);
                } else {
                    List<Doc> docs = children(uri, DocumentsContract.getTreeDocumentId(uri));
                    long total = 0;
                    int count = 0;
                    for (Doc doc : docs) {
                        if (doc.dir || doc.name == null || !SaveTransfer.gameFile(doc.name)) continue;
                        if (++count > SaveTransfer.MAX_FILES) throw new IOException("Too many save files.");
                        File file = new File(stage, doc.name);
                        if (file.exists()) throw new IOException("Duplicate save filename.");
                        try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(uri, doc.id));
                             OutputStream out = new FileOutputStream(file)) {
                            total += SaveTransfer.copy(in, out, SaveTransfer.MAX_BYTES - total);
                        }
                    }
                }
                SaveTransfer.install(stage, target);
                nativeSaveTransferDone("Saves imported. You can now press PLAY. Previous saves, if any, were backed up.");
            }
        } catch (Exception e) {
            Log.e(TAG, "save transfer", e);
            nativeSaveTransferDone("Save transfer failed: " + e.getMessage());
        } finally {
            try { if (stage != null) SaveTransfer.discard(stage); } catch (IOException e) { Log.w(TAG, "save staging cleanup", e); }
            if (archive != null) archive.delete();
        }
    }

    // Back to the system's Vulkan driver: the chosen package and its unpacked copy go (from the next start on)
    public static void useSystemDriver() {
        new Thread(() -> {
            self.dropCustomDriver();
            nativeDriverPicked("-");
        }, "driver").start();
    }

    private void dropCustomDriver() {
        deleteTree(new File(getExternalFilesDir(null), "drivers"));
        deleteTree(new File(getFilesDir(), "drivers"));
    }

    // The app icon's long-press shortcut (res/xml/shortcuts.xml): the system driver, settings out of reach or not - a
    // custom driver that draws garbage (Turnip_v26.3.0-R6 on an Adreno 830: stripes, the menus unreadable) or crashes.
    // Before the driver loads when it starts the app; a running one (a single instance) drops it and starts again.
    static final String SYSTEM_DRIVER = "com.lasrecomp.action.SYSTEM_DRIVER";

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent != null && SYSTEM_DRIVER.equals(intent.getAction())) {
            Log.i(TAG, "shortcut: back to the system driver");
            dropCustomDriver();
            restartApp();
        }
    }

    // a new process: the driver is loaded once, at start
    public static void restartApp() {
        self.runOnUiThread(() -> {
            Intent i = self.getPackageManager().getLaunchIntentForPackage(self.getPackageName());
            if (i != null) {
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                self.startActivity(i);
            }
            System.exit(0);
        });
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        if (req == REQ_SAVE_FOLDER || req == REQ_SAVE_ZIP || req == REQ_SAVE_EXPORT) {
            Uri uri = result == Activity.RESULT_OK && data != null ? data.getData() : null;
            File target = saveTarget;
            boolean previous = exportPrevious;
            if (uri == null || target == null) nativeSaveTransferDone("Save transfer cancelled.");
            else new Thread(() -> transferSaveFiles(uri, req, target, previous), "save-transfer").start();
            return;
        }
        if (req == REQ_GAME) {
            Uri tree = result == Activity.RESULT_OK && data != null ? data.getData() : null;
            if (tree == null) nativeImportDone(1);
            else new Thread(() -> importGame(tree), "import").start();
            return;
        }
        if (req == REQ_DRIVER) {
            Uri f = result == Activity.RESULT_OK && data != null ? data.getData() : null;
            if (f != null) new Thread(() -> importDriver(f), "driver").start();
            return;
        }
        super.onActivityResult(req, result, data);
    }

    // A document in the picked tree: id, name, folder or not, size
    private static final class Doc {
        String id, name;
        boolean dir;
        long size;
    }

    private List<Doc> children(Uri tree, String id) {
        List<Doc> out = new ArrayList<>();
        Uri u = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                         DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = getContentResolver().query(u, cols, null, null, null)) {
            if (c == null) throw new IllegalStateException("Could not list the selected folder. Select a local game folder and retry.");
            while (c.moveToNext()) {
                Doc d = new Doc();
                d.id = c.getString(0);
                d.name = c.getString(1);
                d.dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                d.size = c.isNull(3) ? -1 : c.getLong(3);
                out.add(d);
            }
        }
        return out;
    }

    private static Doc named(List<Doc> l, String name) {
        for (Doc d : l) if (d.name != null && d.name.equals(name)) return d;
        return null;
    }

    // exefs: the folder with main and main.npdm; romfs: the folder with region_common - in the picked folder or a few
    // levels below it (as runtime/ui/launcher.mm find_game)
    private Doc exefs, romfs;
    private boolean packages;

    private void find(Uri tree, Doc dir, int depth) {
        if (depth > 4 || (exefs != null && romfs != null)) return;
        List<Doc> kids = children(tree, dir.id);
        if (exefs == null && named(kids, "main") != null && named(kids, "main.npdm") != null) exefs = dir;
        Doc rc = named(kids, "region_common");
        if (romfs == null && rc != null && rc.dir) romfs = dir;
        for (Doc k : kids) {
            String n = k.name == null ? "" : k.name.toLowerCase();
            if (n.endsWith(".nsp") || n.endsWith(".xci") || n.endsWith(".nca") || n.endsWith(".nsz") || n.endsWith(".xcz")) packages = true;
            if (k.dir && !"region_common".equals(k.name)) find(tree, k, depth + 1);
        }
    }

    private long treeBytes(Uri tree, Doc dir) throws IOException {
        long n = 0;
        for (Doc k : children(tree, dir.id)) {
            if (!k.dir && k.size < 0) throw new IOException("The file provider did not report a size for " + k.name +
                    ". Copy the extracted game files to a local folder and select it again.");
            n = Math.addExact(n, k.dir ? treeBytes(tree, k) : k.size);
        }
        return n;
    }

    private long copied, total, reported;

    private void copyTree(Uri tree, Doc dir, File to) throws IOException {
        to.mkdirs();
        ContentResolver cr = getContentResolver();
        for (Doc k : children(tree, dir.id)) {
            File dst = new File(to, k.name);
            if (k.dir) {
                copyTree(tree, k, dst);
                continue;
            }
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, k.id));
                 OutputStream o = new FileOutputStream(dst)) {
                byte[] buf = new byte[1 << 20];
                long fileBytes = 0;
                for (int r; (r = in.read(buf)) > 0; ) {
                    o.write(buf, 0, r);
                    fileBytes += r;
                    copied += r;
                    if (copied - reported > (16 << 20)) {
                        reported = copied;
                        nativeImportProgress(copied, total);
                    }
                }
                if (k.size >= 0 && fileBytes != k.size)
                    throw new IOException("Incomplete copy of " + k.name + ". Select the local game folder and retry.");
            }
        }
    }

    private void importGame(Uri tree) {
        int result;
        try {
            nativeImportChecking();
            exefs = romfs = null;
            packages = false;
            Doc root = new Doc();
            root.id = DocumentsContract.getTreeDocumentId(tree);
            root.dir = true;
            find(tree, root, 0);
            if (exefs == null || romfs == null) {
                nativeImportDone(packages ? 3 : 2);
                return;
            }
            Doc main = named(children(tree, exefs.id), "main");
            byte[] head = new byte[0x48];
            try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, main.id))) {
                int got = 0;
                for (int r; got < head.length && (r = in.read(head, got, head.length - got)) > 0; ) got += r;
            }
            if (!nativeBuildIdOk(head)) {
                nativeImportDone(4);
                return;
            }
            File dataRoot = getExternalFilesDir(null);
            if (dataRoot == null) throw new IOException("App storage is unavailable. Check the phone storage and retry.");
            File games = new File(dataRoot, "extracted");
            if ("com.android.externalstorage.documents".equals(tree.getAuthority())) {
                for (Doc source : new Doc[] {exefs, romfs}) {
                    if (source.id.startsWith("primary:")) {
                        File local = new File(android.os.Environment.getExternalStorageDirectory(), source.id.substring(8));
                        if (ImportStorage.isInside(local, games))
                            throw new IOException("Choose your original game dump, not the app's imported game folder.");
                    }
                }
            }
            copied = reported = 0;
            long romfsBytes = treeBytes(tree, romfs), exefsBytes = treeBytes(tree, exefs);
            total = Math.addExact(romfsBytes, exefsBytes);
            long needed = ImportStorage.requiredBytes(exefsBytes, romfsBytes);
            long available = games.getParentFile().getUsableSpace();
            if (available < needed) {
                nativeImportFailure(ImportStorage.spaceMessage(needed, available));
                return;
            }
            // Check before nativeImportBegin removes an earlier import. Do not count that deletion as free space.
            String name = DocumentsContract.getTreeDocumentId(tree);
            nativeImportBegin(name.substring(name.lastIndexOf(':') + 1));
            copyTree(tree, exefs, new File(games, "exefs"));
            copyTree(tree, romfs, new File(games, "romfs"));
            nativeImportProgress(copied, total);
            result = 0;
        } catch (Exception e) {
            Log.e(TAG, "game files: " + e);
            nativeImportFailure(e.getMessage() != null ? e.getMessage() : "Could not read or copy the selected game folder.");
            return;
        }
        nativeImportDone(result);
    }

    // A driver package (.zip, or a bare .so) into <external files>/drivers, in place of any there (customDriver loads
    // it at the next start)
    private void importDriver(Uri f) {
        String name = "driver.zip";
        try (Cursor c = getContentResolver().query(f, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) name = c.getString(0);
        } catch (Exception ignored) {
        }
        String low = name.toLowerCase();
        if (!low.endsWith(".zip") && !low.endsWith(".so")) {
            nativeDriverPicked("");
            return;
        }
        try {
            File dir = new File(getExternalFilesDir(null), "drivers");
            deleteTree(dir);
            dir.mkdirs();
            try (InputStream in = getContentResolver().openInputStream(f)) {
                copy(in, new File(dir, name));
            }
            nativeDriverPicked(name);
        } catch (Exception e) {
            Log.e(TAG, "driver: " + e);
            nativeDriverPicked("");
        }
    }

    // (by size and date: the game code is ~100 MB)
    private static void copyIfChanged(File src, File dst) throws IOException {
        if (!src.exists()) return;
        if (dst.exists() && dst.length() == src.length() && dst.lastModified() >= src.lastModified()) return;
        try (InputStream in = new FileInputStream(src)) {
            copy(in, dst);
        }
        Log.i(TAG, "copied " + src + " -> " + dst);
    }

    private static void setenv(String name, String value) throws ErrnoException {
        Os.setenv(name, value, true);
    }

    private void copyAssets(String from, File to) throws IOException {
        AssetManager am = getAssets();
        String[] names = am.list(from);
        if (names == null) return;
        to.mkdirs();
        for (String n : names) {
            try (InputStream in = am.open(from + "/" + n)) {
                copy(in, new File(to, n));
            }
        }
    }

    // The first driver in <external files>/drivers, in internal storage (copied again only when it changed).
    private File customDriver() throws IOException {
        File src = new File(getExternalFilesDir(null), "drivers");
        src.mkdirs();
        File[] files = src.listFiles((d, n) -> n.endsWith(".so") || n.endsWith(".zip"));
        if (files == null || files.length == 0) return null;
        Arrays.sort(files);
        File f = files[0];
        File dir = new File(getFilesDir(), "drivers/" + f.getName().replaceAll("\\.(so|zip)$", ""));
        File stamp = new File(dir, ".source");
        String id = f.length() + ":" + f.lastModified();
        File lib = findLib(dir);
        if (lib != null && stamp.exists() && id.equals(readSmall(stamp))) return lib;
        deleteTree(dir);
        dir.mkdirs();
        if (f.getName().endsWith(".so")) {
            try (InputStream in = new FileInputStream(f)) {
                lib = copy(in, new File(dir, f.getName()));
            }
        } else {
            lib = null;
            try (ZipInputStream z = new ZipInputStream(new FileInputStream(f))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    String n = new File(e.getName()).getName();
                    if (!e.isDirectory() && n.endsWith(".so")) lib = copyNoClose(z, new File(dir, n));
                }
            }
        }
        if (lib != null) {
            try (OutputStream o = new FileOutputStream(stamp)) {
                o.write(id.getBytes());
            }
        }
        Log.i(TAG, "Vulkan driver: " + f.getName() + " -> " + lib);
        return lib;
    }

    private static File findLib(File dir) {
        File[] libs = dir.listFiles((d, n) -> n.endsWith(".so"));
        return libs != null && libs.length > 0 ? libs[0] : null;
    }

    private static String readSmall(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            return new String(in.readAllBytes());
        }
    }

    private static File copy(InputStream in, File dst) throws IOException {
        return copyNoClose(in, dst);
    }

    private static File copyNoClose(InputStream in, File dst) throws IOException {
        try (OutputStream o = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
        }
        return dst;
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
