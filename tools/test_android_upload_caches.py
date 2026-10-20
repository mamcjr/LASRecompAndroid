#!/usr/bin/env python3
"""Execute the actual C upload-cache helpers on the PC via WebAssembly (no device).
Requires Python, Node.js and the Android NDK's clang/ld.lld. Pass its bin directory as argv[1].
The allocator, submission counter and arena allocator are test doubles; no Vulkan calls are executed.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'optimization-tests'
OUT.mkdir(parents=True, exist_ok=True)
BIN = pathlib.Path(sys.argv[1])
textures = (ROOT / 'runtime/nvn/vulkan/textures.c').read_text()
execution = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text()
texture_helpers = textures[textures.index('static uint8_t *upload_scratch;'):textures.index('uint64_t prof_tex_checks,')]
arena_helper = execution[execution.index('static uint64_t ga_load_copy('):execution.index('static bool gres_prepare(')]
preamble = r"""
typedef unsigned long long uint64_t;
typedef unsigned int size_t;
typedef unsigned int uintptr_t;
typedef unsigned char uint8_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
static unsigned char heap[2 << 20];
static unsigned used;
static int fail_alloc;
void *memcpy(void *to, const void *from, size_t size) {
    for (unsigned i = 0; i < size; i++) ((uint8_t *)to)[i] = ((const uint8_t *)from)[i];
    return to;
}
int memcmp(const void *a, const void *b, size_t size) {
    for (unsigned i = 0; i < size; i++) if (((const uint8_t *)a)[i] != ((const uint8_t *)b)[i]) return 1;
    return 0;
}
void *realloc(void *old, size_t n) {
    if (fail_alloc || used + n + 16 > sizeof heap) return NULL;
    unsigned *header = (unsigned *)(heap + used);
    *header = n;
    void *next = header + 1;
    if (old) memcpy(next, old, ((unsigned *)old)[-1] < n ? ((unsigned *)old)[-1] : n);
    used = (used + n + 19) & ~15u;
    return next;
}
void abort(void) { __builtin_trap(); }
char *getenv(const char *name) { return NULL; }
int atoi(const char *s) { return 0; }
typedef struct { uint64_t data_hash; } TexBackend;
static uint8_t guest[2048], arena[4096];
#define HEAP_BASE ((uintptr_t)guest)
static uint8_t *ga_ptr = arena;
static uint64_t gcopy_gen, seq = 1, top;
static uint64_t vk_pending_seq(void) { return seq; }
static uint64_t ga_ring(uint64_t n) {
    if (top + n > sizeof arena) return ~0ull;
    uint64_t off = top; top += n; return off;
}
"""
tests = r"""
#define CHECK(c) do { count++; if (!(c)) return count; } while (0)
int test(void) {
    int count = 0;
    TexBackend b = {123};
    uint8_t pixels[64] = {1, 2, 3};
    CHECK(!texture_unchanged(&b, pixels, sizeof pixels));
    CHECK(texture_unchanged(&b, pixels, sizeof pixels));
    pixels[37] = 9;
    CHECK(!texture_unchanged(&b, pixels, sizeof pixels));
    b.data_hash++;
    CHECK(!texture_unchanged(&b, pixels, sizeof pixels));
    CHECK(texture_unchanged(&b, pixels, sizeof pixels));
    texture_snapshot_forget(&b);
    CHECK(!texture_unchanged(&b, pixels, sizeof pixels));
    CHECK(!texture_unchanged(&b, pixels, (1u << 20) + 1));
    CHECK(!texture_unchanged(&b, pixels, 0));
    fail_alloc = 1;
    CHECK(!texture_unchanged(&b, pixels, 32));
    fail_alloc = 0;
    CHECK(texture_unchanged(&b, pixels, sizeof pixels));
    uint8_t *scratch = texture_scratch(64);
    scratch[13] = 42;
    CHECK(texture_scratch(16) == scratch);
    CHECK(texture_scratch(256)[13] == 42);
    CHECK(texture_scratch(128) == upload_scratch);

    guest[0] = 1;
    uint64_t a = ga_load_copy(0, 64);
    CHECK(a == 0 && top == 64 && arena[a] == 1);
    CHECK(ga_load_copy(0, 64) == a && top == 64);
    guest[63] = 7;
    uint64_t bcopy = ga_load_copy(0, 64);
    CHECK(bcopy != a && arena[bcopy + 63] == 7 && arena[a + 63] == 0);
    seq++;
    uint64_t c = ga_load_copy(0, 64);
    CHECK(c != bcopy);
    gcopy_gen++;
    uint64_t d = ga_load_copy(0, 64);
    CHECK(d != c);
    CHECK(ga_load_copy(0, 64) == d);
    top = sizeof arena;
    guest[20]++;
    CHECK(ga_load_copy(0, 64) == ~0ull);
    CHECK(arena[d + 20] == 0);
    guest[20]--;
    CHECK(ga_load_copy(0, 64) == d);
    return 0;
}
"""
source = OUT / 'upload_caches.c'
source.write_text(preamble + texture_helpers + arena_helper + tests)
subprocess.run([str(BIN / 'clang.exe'), '-target', 'wasm32', '-ffreestanding', '-fno-builtin', '-O2', '-c', str(source), '-o', str(OUT / 'upload_caches.o')], check=True)
subprocess.run([str(BIN / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=test', str(OUT / 'upload_caches.o'), '-o', str(OUT / 'upload_caches.wasm')], check=True)
runner = OUT / 'upload_caches.cjs'
runner.write_text("const fs = require('fs'); WebAssembly.instantiate(fs.readFileSync(process.argv[2]), {}).then(({instance}) => { const failure = instance.exports.test(); if (failure) throw new Error('Upload cache check ' + failure + ' failed'); console.log('Upload caches: 22 checks passed (actual C helpers, mocked allocation/submissions)'); }).catch(e => { console.error(e); process.exitCode = 1; });")
subprocess.run(['node', str(runner), str(OUT / 'upload_caches.wasm')], check=True)
