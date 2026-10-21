#!/usr/bin/env python3
"""Run production BRX table inspection against the end of WebAssembly memory (no phone)."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'build/shader-constant-tests'
out.mkdir(parents=True, exist_ok=True)
bin_dir = Path(sys.argv[1])
source = (root / 'runtime/nvn/maxwell/translate.c').read_text()
start = source.index('static bool brx_table(')
helper = source[start:source.index('\nstatic int brx_of(', start)]
prefix = r'''
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int size_t;
typedef _Bool bool;
#define true 1
#define false 0
#define RZ 255
typedef struct { struct { uint32_t at, t[256]; int n; } brx[4]; int nbrx; } Ctx;
static uint64_t bits(uint64_t w, int off, int n) { return (w >> off) & ((1ull << n) - 1); }
static int64_t sbits(uint64_t w, int off, int n) { return (int64_t)(w << (64 - off - n)) >> (64 - n); }
'''
tests = r'''
int check(unsigned ptr, unsigned bytes, int from_cb1, int expected) {
    uint64_t code[16] = {0};
    code[1] = 0xef90000000000000ull | (1ull << 36) | 1;
    if (!from_cb1) code[1] = 1;
    code[2] = 0xe250000000000000ull | (1ull << 8);
    uint8_t label[20] = {0};
    Ctx c = {0};
    bool found = brx_table(&c, code, 2, 16, (const uint32_t *)ptr, bytes, label);
    if (found != (expected > 0)) return 1;
    if (c.nbrx != (expected > 0)) return 2;
    if (expected && (c.brx[0].n != expected || c.brx[0].t[0] != 24 || !label[3])) return 3;
    if (expected > 1 && (c.brx[0].t[1] != 40 || !label[5])) return 4;
    if (label[16] || label[17] || label[18] || label[19]) return 5;
    return 0;
}
'''
(out / 'test.c').write_text(prefix + helper + tests)
subprocess.run([str(bin_dir / 'clang.exe'), '--target=wasm32', '-O2', '-nostdlib', '-fno-builtin', '-c',
                str(out / 'test.c'), '-o', str(out / 'test.o')], check=True)
subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=check', '--export-memory',
                str(out / 'test.o'), '-o', str(out / 'test.wasm')], check=True)
(out / 'run.cjs').write_text(r'''
const fs = require('fs');
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({instance}) => {
  const {memory, check} = instance.exports;
  const end = memory.buffer.byteLength;
  const v = new DataView(memory.buffer);
  let cases = 0;
  function test(ptr, bytes, cb, expected) {
    const result = check(ptr, bytes, cb, expected);
    if (result) throw Error(JSON.stringify({ptr,bytes,cb,expected,result}));
    cases++;
  }
  // No readable word: even a pointer past memory must never be dereferenced.
  for (let bytes=0; bytes<4; bytes++) test(end,bytes,1,0);
  test(0,4096,1,0);
  // A single word at the last legal address. An unbounded scan traps on the next read.
  v.setUint32(end-4,0,true);
  test(end-4,4,1,1);
  test(end-4,4,0,0);
  v.setUint32(end-8,0,true); v.setUint32(end-4,16,true);
  test(end-8,8,1,2);
  test(end-8,7,1,1); // ignore trailing incomplete word
  // Invalid / unaligned / duplicate targets.
  v.setUint32(end-4,1,true); test(end-8,8,1,1);
  v.setUint32(end-4,0,true); test(end-8,8,1,1);
  // Preserve the 1024-word cap even if the caller supplies a larger available region.
  new Uint8Array(memory.buffer,end-4096,4096).fill(0);
  test(end-4096,8192,1,1);
  console.log(`${cases} bounded constant-table checks passed`);
}).catch(e => { console.error(e); process.exitCode=1; });
''')
subprocess.run(['node', str(out / 'run.cjs'), str(out / 'test.wasm')], check=True)
