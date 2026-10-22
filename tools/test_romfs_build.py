#!/usr/bin/env python3
"""romfs_build.c on a synthetic game folder (Linux / WSL cc): the folder path and the entry list (Android's import,
straight from the picked folder) must give the image the previous version gave, byte for byte, and keep only the
picked loose files. Usage: test_romfs_build.py [--cc cc]"""
import argparse
import hashlib
import os
import shutil
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', default='cc')
args = p.parse_args()
out = root / 'build' / 'romfs-build-tests'
out.mkdir(parents=True, exist_ok=True)

# the previous builder, for the byte-for-byte reference
old = subprocess.run(['git', '-C', str(root), 'show', 'e32c2a5:runtime/romfs_build.c'], check=True,
                     capture_output=True, text=True).stdout
(out / 'romfs_build_old.c').write_text(old.replace('bool romfs_build(', 'bool romfs_build_old(')
                                          .replace('uint64_t romfs_dir_bytes(', 'static uint64_t unused_dir_bytes(')
                                          .replace('bool romfs_copy_tree(', 'static bool unused_copy_tree('))

game = out / 'game'
files = {  # relative path -> size (odd sizes: the 16-byte alignment; a hidden file the image must leave out)
    'region_common/ui/Game.arc': 1000, 'region_common/ui/StartUp.arc': 17, 'region_common/movie/title.webm': 4099,
    'region_common/actor/Playerlink.bfres': 33, 'region_common/actor/Other.bfres': 5000,
    'regionUS/common/ui/GameRegionCommon.arc': 64, 'regionUS/common/msg/a.msbt': 3, 'a.bin': 1, 'zz/deep/x/y.dat': 77,
    '.hidden/skip.txt': 9, 'region_common/.DS_Store': 4,
}
for rel, size in files.items():
    f = game / rel
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_bytes(hashlib.sha256(rel.encode()).digest() * (size // 32 + 1))
    os.truncate(f, size)
entries = sorted(files)  # (the order the activity lists them in doesn't matter: shuffle it a little)
entries = entries[1::2] + entries[::2]

harness = r'''
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "romfs_build.h"
bool romfs_build_old(const char *, const char *, void (*)(uint64_t, uint64_t), char *, size_t);
static const char *base;
static int open_fd(void *ctx, int i) {
    const RomfsEntry *e = ctx;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", base, e[i].path);
    return open(p, O_RDONLY);
}
static bool keep(const char *rel) {
    size_t n = strlen(rel);
    return (n > 5 && !strcmp(rel + n - 5, ".webm")) || !strcmp(rel, "region_common/ui/Game.arc");
}
int main(int argc, char **argv) {
    base = argv[1];
    char why[256] = "";
    if (!romfs_build_old(argv[1], argv[2], NULL, why, sizeof why)) { printf("old: %s\n", why); return 1; }
    if (!romfs_build(argv[1], argv[3], NULL, why, sizeof why)) { printf("new: %s\n", why); return 1; }
    int n = (argc - 6) / 2;
    RomfsEntry e[64];
    for (int i = 0; i < n; i++) e[i] = (RomfsEntry){argv[6 + 2 * i], (uint64_t)atoll(argv[7 + 2 * i])};
    if (!romfs_build_entries(e, n, open_fd, e, argv[4], argv[5], keep, NULL, why, sizeof why)) { printf("entries: %s\n", why); return 1; }
    char shortp[1024];
    snprintf(shortp, sizeof shortp, "%s.short", argv[4]);
    e[0].size += 1;  // a file shorter than its listed size must fail, not write a wrong image
    if (romfs_build_entries(e, n, open_fd, e, shortp, argv[5], keep, NULL, why, sizeof why)) { printf("short file accepted\n"); return 1; }
    e[0].size -= 2;  // and longer than listed
    if (romfs_build_entries(e, n, open_fd, e, shortp, argv[5], keep, NULL, why, sizeof why)) { printf("long file accepted\n"); return 1; }
    return 0;
}
'''
(out / 'harness.c').write_text('#include <stdlib.h>\n' + harness)
exe = out / 'test'
subprocess.run([args.cc, '-O1', '-std=gnu11', '-I', str(root / 'runtime'), str(out / 'harness.c'),
                str(root / 'runtime/romfs_build.c'), str(out / 'romfs_build_old.c'), '-o', str(exe)], check=True)
keep_dir = out / 'loose'
shutil.rmtree(keep_dir, ignore_errors=True)
keep_dir.mkdir()
argv = [str(exe), str(game), str(out / 'old.bin'), str(out / 'new.bin'), str(out / 'entries.bin'), str(keep_dir)]
for rel in entries:
    argv += [rel, str(files[rel])]
subprocess.run(argv, check=True)
old_bin, new_bin = (out / 'old.bin').read_bytes(), (out / 'new.bin').read_bytes()
assert old_bin == new_bin, 'folder build changed (desktop)'
assert (out / 'entries.bin').read_bytes() == old_bin, 'image from the entry list differs from the folder build'
assert not (out / 'entries.bin.short').exists(), 'a failed build must not leave its image'
loose = sorted(str(f.relative_to(keep_dir)).replace(os.sep, '/') for f in keep_dir.rglob('*') if f.is_file())
assert loose == ['region_common/movie/title.webm', 'region_common/ui/Game.arc'], loose
assert (keep_dir / 'region_common/movie/title.webm').read_bytes() == (game / 'region_common/movie/title.webm').read_bytes()
print('5 romfs image checks passed: folder build unchanged, entry list identical, short and long files rejected, loose copies as picked')
