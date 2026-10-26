#!/usr/bin/env python3
"""The game files check (runtime/game_addresses.c game_romfs_*, from upstream LASRecomp beta_5.3), production C on the
PC (NDK clang -> wasm, Node): the table, the C and Python hashes agreeing, and with the extracted 1.0.0 game files in
ROM/ (when there) every file one of the game's, none missing, the total right - then a renamed, a resized, a missing
and an extra file each caught. Usage: test_romfs_check.py <NDK llvm bin folder>"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
import romfs_files  # noqa: E402

out = root / 'build' / 'romfs-check-tests'
out.mkdir(parents=True, exist_ok=True)
bin_dir = Path(sys.argv[1])

# ---- the table: sorted, no hash twice, 3836 files in each version
entries = re.findall(r'^\{0x([0-9a-f]{16})ull, ([123])\},$', (root / 'runtime/game_addresses/romfs_files.h').read_text(),
                     re.M)
hashes = [int(h, 16) for h, _ in entries]
assert hashes == sorted(set(hashes)), 'romfs_files.h: not sorted, or a hash twice'
assert sum(int(m) & 1 for _, m in entries) == sum(int(m) >> 1 for _, m in entries) == 3836, 'romfs_files.h: file count'

source = (root / 'runtime/game_addresses.c').read_text()
body = source[source.index('// (from upstream LASRecomp beta_5.3: 14c1f9b, 1f93424)'):]
preamble = r'''
typedef unsigned long long uint64_t;
typedef unsigned char uint8_t;
typedef unsigned long size_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
typedef struct GameVersion { const char *key; } GameVersion;
typedef struct GameRomfsCheck GameRomfsCheck;
static unsigned char heap[8192];  // (one check at a time)
void *calloc(size_t n, size_t s) {
    for (size_t i = 0; i < n * s; i++) heap[i] = 0;
    return n * s <= sizeof heap ? heap : NULL;
}
void free(void *p) { (void)p; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int snprintf(char *o, size_t n, const char *f, ...) {  // (only "%llu" here)
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    unsigned long long v = __builtin_va_arg(ap, unsigned long long);
    __builtin_va_end(ap);
    char t[24];
    int k = 0, i = 0;
    do t[k++] = (char)('0' + v % 10), v /= 10; while (v);
    while (k && i + 1 < (int)n) o[i++] = t[--k];
    o[i] = 0;
    return i;
}
'''
wrapper = r'''
static char buf[1100];
static GameVersion versions[2] = {{"v1_0_0"}, {"v1_0_1"}};
static GameRomfsCheck *cur;
char *path_buf(void) { return buf; }
void begin(int which) { cur = game_romfs_check_begin(which < 0 ? NULL : &versions[which]); }
int check(unsigned long long size) { return game_romfs_check_file(cur, buf, size); }
int end(void) { return game_romfs_check_end(cur); }
unsigned long long hash(unsigned long long size) { return game_romfs_hash(buf, size); }
unsigned long long total(int which) { return game_romfs_bytes(&versions[which]); }
int exefs_name(void) { return game_romfs_exefs_name(buf); }
'''
(out / 'test.c').write_text(preamble + body + wrapper)
exports = ['path_buf', 'begin', 'check', 'end', 'hash', 'total', 'exefs_name']
subprocess.run([str(bin_dir / 'clang.exe'), '--target=wasm32', '-O2', '-fno-builtin', '-nostdlib', '-I', str(root / 'runtime'),
                '-c', str(out / 'test.c'), '-o', str(out / 'test.o')], check=True)
subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', *[f'--export={e}' for e in exports],
                str(out / 'test.o'), '-o', str(out / 'test.wasm')], check=True)

# ---- inputs: hash samples, and the 1.0.0 game files' list when ROM/ has them
samples = [('region_common/event/x.bfevfl', 2712), ('a', 0), ('b c/d.bin', 6232003695), ('Ä/ü.bin', 3)]
dump = root / 'ROM' / '01006BB00C6F0000' / 'romfs'
files = sorted(romfs_files.romfs_files(dump).items()) if dump.is_dir() else []
(out / 'input.json').write_text(json.dumps({'samples': samples, 'files': files}))
(out / 'run.cjs').write_text(r'''
const fs = require('fs');
const assert = require('assert/strict');
const input = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
(async () => {
  const p = (await WebAssembly.instantiate(fs.readFileSync(process.argv[2]))).instance.exports;
  const mem = () => new Uint8Array(p.memory.buffer);
  const put = s => { const b = Buffer.from(s, 'utf8'), at = p.path_buf(); mem().set(b, at); mem()[at + b.length] = 0; };
  const run = (which, list) => {  // [odd paths], missing
    p.begin(which);
    const odd = [];
    for (const [path, size] of list) { put(path); if (!p.check(BigInt(size))) odd.push(path); }
    return [odd, p.end()];
  };
  const out = {hashes: input.samples.map(([s, n]) => { put(s); return (p.hash(BigInt(n)) & 0xffffffffffffffffn).toString(16); })};
  for (const [s, want] of [['main', 1], ['subsdk1', 1], ['mainx', 0], ['romfs/main', 0]]) { put(s); assert.equal(p.exefs_name(), want, s); }
  assert.deepEqual(run(-1, [['region_common/made_up.bin', 12]]), [[], 0], 'no version: no check');
  assert.equal(p.total(0), 6232003695n);
  assert.equal(p.total(1), 6232003903n);
  if (input.files.length) {
    const f = input.files;
    assert.deepEqual(run(0, f), [[], 0], 'the 1.0.0 game files: every one the game\'s, none missing');
    assert.equal(f.reduce((a, [, n]) => a + BigInt(n), 0n), p.total(0), 'their total');
    const [path0, size0] = f[0];
    const renamed = f.map(([s, n], i) => i ? [s, n] : [s.replace(/[^/]*$/, m => m[0].toUpperCase() + m.slice(1)), n]);
    assert.deepEqual(run(0, renamed), [[renamed[0][0]], 1], 'a renamed file: odd, and one missing');
    const resized = f.map(([s, n], i) => i ? [s, n] : [s, n + 1]);
    assert.deepEqual(run(0, resized), [[path0], 1], 'a resized file: odd, and one missing');
    assert.deepEqual(run(0, f.slice(1)), [[], 1], 'a missing file');
    assert.deepEqual(run(0, [...f, ['region_common/extra.bin', 5]]), [['region_common/extra.bin'], 0], 'an extra file');
    assert.deepEqual(run(1, f)[1] > 0, true, 'the 1.0.0 files against 1.0.1: not all of its');
    out.files = f.length;
  }
  console.log(JSON.stringify(out));
})().catch(e => { console.error(e); process.exit(1); });
''')
res = json.loads(subprocess.run(['node', str(out / 'run.cjs'), str(out / 'test.wasm'), str(out / 'input.json')],
                                check=True, capture_output=True, encoding='utf-8').stdout)
for (rel, size), h in zip(samples, res['hashes']):
    assert int(h, 16) == romfs_files.entry_hash(rel, size), (rel, h)
n = 8 + (6 if 'files' in res else 0)
print(f'{n} game files checks passed (production C)' +
      (f'; the 1.0.0 game files in ROM/: {res["files"]} files' if 'files' in res else '; no 1.0.0 game files in ROM/ (skipped)'))
