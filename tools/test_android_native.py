#!/usr/bin/env python3
"""The stubs of Native game code (runtime/native.c) built by its own C on the PC (WebAssembly), then disassembled.
Requires Python, Node.js and the Android NDK's clang, ld.lld and llvm-objdump. Pass its bin directory as argv[1].
The site and the stubs sit in one blob at made-up addresses; each stub's instructions and branch targets are checked.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'optimization-tests'
OUT.mkdir(parents=True, exist_ok=True)
BIN = pathlib.Path(sys.argv[1])
src = (ROOT / 'runtime/native.c').read_text()
encoders = src[src.index('static uint32_t enc_b('):src.index('// ---- the game\'s fixes')]
fixstub = src[src.index('// The original instruction, to run from the stub'):src.index('// The seed\'s code words')]
preamble = r"""
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int uint32_t;
typedef unsigned int size_t;
typedef unsigned int uintptr_t;
typedef _Bool bool;
#define true 1
#define false 0
void *memcpy(void *to, const void *from, size_t n) {
    for (size_t i = 0; i < n; i++) ((char *)to)[i] = ((const char *)from)[i];
    return to;
}
static int fatal;
#define FATAL(...) (fatal = 1)
char native_svc_gate[16], native_patch_gate[16];
struct { uint64_t tp; void *cpu; } g_native_tab[4];
static uint64_t g_tick_mul = 0x1234567890ull;
"""
tests = r"""
uint32_t blob[0x3000 / 4];  // sites at 0x100.., stubs at 0x1000.. (byte offsets; the blob's own addresses)
int fatal_seen(void) { return fatal; }
uint32_t *make(void) {
    uint32_t *b = blob;
    stub_svc(b + 0x1000 / 4, 0x1000, 0x100, 0x1e);
    stub_tls(b + 0x1100 / 4, 0x1100, 0x104, 0, 288, false);  // mrs x0, tpidrro_el0
    stub_tls(b + 0x1200 / 4, 0x1200, 0x108, 15, 280, true);  // msr tpidr_el0, x15
    stub_ticks(b + 0x1300 / 4, 0x1300, 0x10c, 3);            // mrs x3, cntpct_el0
    stub_ticks(b + 0x1400 / 4, 0x1400, 0x110, 16);
    stub_freq(b + 0x1500 / 4, 0x1500, 0x114, 5);              // mrs x5, cntfrq_el0
    stub_fix(b + 0x1600 / 4, 0x1600, 0x200, 7, 0x94000000u | ((0x2000 - 0x200) >> 2), 0x300);  // bl 0x2000, alt 0x300
    stub_fix(b + 0x1700 / 4, 0x1700, 0x204, 8, 0xd10203ffu, 0);  // sub sp, sp, #0x80; no alt
    b[0x1800 / 4] = moved(0x37380148u, 0x208, 0x1800);  // tbnz: can't move
    b[0x1804 / 4] = moved(0x54000040u, 0x20c, 0x1804);  // b.eq
    b[0x1808 / 4] = moved(0x17ffff94u, 0x210, 0x1808);  // b back
    b[0x180c / 4] = moved(0xf9400008u, 0x214, 0x180c);  // ldr x8, [x0]
    return b;
}
"""
source = OUT / 'native_stubs.c'
source.write_text(preamble + encoders + fixstub + tests)
subprocess.run([str(BIN / 'clang.exe'), '-target', 'wasm32', '-ffreestanding', '-fno-builtin', '-O1', '-c', str(source), '-o', str(OUT / 'native_stubs.o')], check=True)
subprocess.run([str(BIN / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=make', '--export=fatal_seen', '--export-memory',
                str(OUT / 'native_stubs.o'), '-o', str(OUT / 'native_stubs.wasm')], check=True)
runner = OUT / 'native_stubs.cjs'
runner.write_text("const fs = require('fs'); WebAssembly.instantiate(fs.readFileSync(process.argv[2]), {}).then(({instance}) => {"
                  " const p = instance.exports.make(); if (instance.exports.fatal_seen()) throw new Error('FATAL');"
                  " fs.writeFileSync(process.argv[3], Buffer.from(instance.exports.memory.buffer, p, 0x3000)); });")
raw = OUT / 'native_stubs.bin'
subprocess.run(['node', str(runner), str(OUT / 'native_stubs.wasm'), str(raw)], check=True)
words = raw.read_bytes()
asm = OUT / 'native_stubs.s'
asm.write_text('.text\n' + ''.join(f'.inst 0x{int.from_bytes(words[i:i + 4], "little"):08x}\n' for i in range(0, len(words), 4)))
subprocess.run([str(BIN / 'clang.exe'), '--target=aarch64-linux-android29', '-c', str(asm), '-o', str(OUT / 'native_stubs_a64.o')], check=True)
dis = subprocess.run([str(BIN / 'llvm-objdump.exe'), '-d', '--no-show-raw-insn', str(OUT / 'native_stubs_a64.o')],
                     capture_output=True, text=True, check=True).stdout
ins = {}
for line in dis.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
    if m:
        ins[int(m.group(1), 16)] = re.sub(r'\s+', ' ', re.sub(r'\s*//.*|<.*>', '', m.group(2))).strip()
count = 0


def expect(at, *want):
    global count
    for k, w in enumerate(want):
        got = ins.get(at + 4 * k)
        if got != w:
            sys.exit(f'native stubs: at {at + 4 * k:#x} {got!r}, expected {w!r}')
        count += 1


expect(0x1000, 'sub sp, sp, #0x30', 'stp x16, x17, [sp]', 'str x30, [sp, #0x10]', 'mov x17, #0x1e', 'ldr x16, 0x1028',
       'blr x16', 'ldp x16, x17, [sp]', 'ldr x30, [sp, #0x10]', 'add sp, sp, #0x30', 'b 0x104')
expect(0x1100, 'stp x14, x15, [sp, #-0x20]!', 'str x16, [sp, #0x10]', 'mrs x14, TPIDR_EL0', 'eor x15, x14, x14, lsr #12',
       'ubfx x15, x15, #12, #12', 'ldr x16, 0x1148', 'add x15, x16, x15, lsl #4', 'ldr x16, [x15]', 'eor x16, x16, x14',
       'cbz x16, 0x1130', 'add x15, x15, #0x10', 'b 0x111c', 'ldr x14, [x15, #0x8]', 'ldr x0, [x14, #0x120]',
       'ldr x16, [sp, #0x10]', 'ldp x14, x15, [sp], #0x20', 'b 0x108')
expect(0x1200 + 4 * 12, 'ldr x14, [x16, #0x8]', 'str x15, [x14, #0x118]')  # (temporaries without x15)
expect(0x1200, 'stp x14, x16, [sp, #-0x20]!', 'str x17, [sp, #0x10]')
expect(0x1300, 'stp x14, x15, [sp, #-0x10]!', 'mrs x14, CNTVCT_EL0', 'ldr x15, 0x1320', 'mul x3, x14, x15',
       'umulh x14, x14, x15', 'extr x3, x14, x3, #0x20', 'ldp x14, x15, [sp], #0x10', 'b 0x110')
expect(0x1400, 'stp x14, x15, [sp, #-0x10]!', 'mrs x14, CNTVCT_EL0', 'ldr x15, 0x1420', 'mul x16, x14, x15',
       'umulh x14, x14, x15', 'extr x16, x14, x16, #0x20')
if int.from_bytes(words[0x1320:0x1328], 'little') != 0x1234567890:
    sys.exit('native stubs: the tick multiplier literal')
expect(0x1500, 'mov x5, #0xf800', 'movk x5, #0x124, lsl #16', 'b 0x118')
expect(0x1600, 'sub sp, sp, #0x30', 'stp x16, x17, [sp]', 'str x30, [sp, #0x10]', 'mov x17, #0x7', 'ldr x16, 0x1670',
       'blr x16', 'ldr x16, [sp, #0x18]', 'cbz x16, 0x1658', 'tbnz w16, #0x1, 0x1634',
       'ldp x16, x17, [sp]', 'ldr x30, [sp, #0x10]', 'add sp, sp, #0x30', 'b 0x300',
       'tbnz w16, #0x0, 0x1648', 'ldp x16, x17, [sp]', 'ldr x30, [sp, #0x10]', 'add sp, sp, #0x30', 'ret',
       'ldp x16, x17, [sp]', 'ldr x30, [sp, #0x10]', 'add sp, sp, #0x30', 'b 0x204',
       'ldp x16, x17, [sp]', 'ldr x30, [sp, #0x10]', 'add sp, sp, #0x30', 'bl 0x2000', 'b 0x204')
expect(0x1700 + 4 * 12, 'brk #0xf2')
expect(0x1700 + 4 * 25, 'sub sp, sp, #0x80', 'b 0x208')
expect(0x1800, 'brk #0xf1', 'brk #0xf1', 'b 0x60', 'ldr x8, [x0]')
print(f'Native stubs: {count} instructions as expected (native.c\'s own encoders, disassembled)')

# The fixes' table (native.c fixes[]) against the game's own main programs, where a dump is in ROM/ (as
# test_romfs_check.py): every site holds the instruction the fix expects, in every version given
try:
    sys.path.insert(0, str(ROOT / 'tools'))
    from nso import Nso
except ImportError:
    Nso = None
mains = [(p, v) for p, v in [('ROM/01006BB00C6F0000/exefs/main', 'v1_0_0'), ('ROM/updates/v65536/exefs/main', 'v1_0_1'),
                             ('ROM/updates/v131072/exefs/main', 'v1_0_1')] if (ROOT / p).exists()]
table = re.findall(r'F\((\w+), [^,]+, (LDR_BA4|[^,]+, [^,]+), fx_\w+', src)
if Nso and mains:
    for path, v in mains:
        img = Nso(str(ROOT / path)).image
        addrs = dict((m.group(1), int(m.group(2), 16)) for m in re.finditer(r'GA_ADDR\((\w+),\s*0x([0-9a-f]+)\)',
                     (ROOT / f'runtime/game_addresses/patches_{v}.h').read_text()))
        for name, wm in table:
            word, mask = (0xBD4BA400, 0xFFFFFC00) if wm == 'LDR_BA4' else [
                0xFFFFFFFF if x.strip() == 'ANY' else eval(x.strip().rstrip('u')) for x in wm.split(',')]
            got = int.from_bytes(img[addrs[name] - 0x7000000000:][:4], 'little')
            if got & mask != word:
                sys.exit(f'native fixes: {name} in {path}: {got:08x}, expected {word:08x}')
    print(f'Native fixes: {len(table)} sites hold the expected instructions in {len(mains)} main programs')
else:
    print('Native fixes: no game dump in ROM/ (or no lz4): the table not checked against the game')
