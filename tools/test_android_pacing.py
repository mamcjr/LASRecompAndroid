#!/usr/bin/env python3
"""Replay production Android pacing on the PC, with a deterministic clock (NDK + Node)."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'build' / 'pacing-tests'
out.mkdir(parents=True, exist_ok=True)
bin_dir = Path(sys.argv[1])
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
start = source.index('static _Atomic bool pace_slow;')
body = source[start:source.index("// a pair's end counter", start)]
preamble = r'''
typedef unsigned long long uint64_t;
typedef unsigned char uint8_t;
typedef unsigned int size_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
#define __ANDROID__ 1
static int log_count;
__attribute__((noinline)) void test_log(const char *fmt, ...) { log_count++; }
#define LOG(...) test_log(__VA_ARGS__)
struct { int fps_mode, res_mode; } g_settings;
uint64_t g_blocked_ns, gpu_busy_total, cum_exec, prof_up_cum;
int g_present_interval = 1;
static uint64_t clock_ns = 1000000000ull, render_ns, hint_target;
static int disabled, tracing;
void *memset(void *p, int v, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = v;
    return p;
}
char *getenv(const char *name) {
    const char *key = "LARECOMP_PACE";
    int i = 0;
    while (key[i] && key[i] == name[i]) i++;
    if (disabled && !key[i] && !name[i]) return "0";
    key = "LARECOMP_PACE_TRACE";
    i = 0;
    while (key[i] && key[i] == name[i]) i++;
    return tracing && !key[i] && !name[i] ? "1" : NULL;
}
int atoi(const char *s) { return s[0] - '0'; }
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i]; }
    return 0;
}
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
static const char *level_name = "Field";  // (one scene unless a test changes it: scene())
const char *game_level_name(void) { return level_name; }
uint64_t plat_now_ns(void) { return clock_ns; }
bool vk_async_on(void) { return true; }
uint64_t vk_async_frame_work_ns(void) { return render_ns; }
uint64_t vk_async_frame_cpu_ns(void) { return render_ns; }
void plat_frame_work_gpu(uint64_t a, uint64_t b, uint64_t target, uint64_t c) { hint_target = target; }
'''
wrapper = r'''
int frame(int period_us, int work_us, int interval, int mode, int resolution) {
    clock_ns += (uint64_t)period_us * 1000;
    g_blocked_ns += (uint64_t)(period_us > work_us ? period_us - work_us : 0) * 1000;
    render_ns = (uint64_t)work_us * 1000;
    g_present_interval = interval;
    g_settings.fps_mode = mode;
    g_settings.res_mode = resolution;
    vk_pace_frame();
    return pace_slow;
}
int target_us(void) { return hint_target / 1000; }
void scene(int other) { level_name = other ? "Dungeon" : "Field"; }
void compiles(int n) {  // shader jobs queued or running; 0: the last one ends now
    if (!n && shader_jobs) shader_jobs_done_ns = clock_ns;
    shader_jobs = n;
}
void pacing_off(void) { disabled = 1; }
void tracing_on(void) { tracing = 1; }
int logs(void) { return log_count; }
'''
(out / 'test.c').write_text(preamble + body + wrapper)
subprocess.run([str(bin_dir / 'clang.exe'), '--target=wasm32', '-O2', '-fno-builtin', '-nostdlib',
                '-c', str(out / 'test.c'), '-o', str(out / 'test.o')], check=True)
subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=frame',
                '--export=target_us', '--export=pacing_off', '--export=tracing_on', '--export=logs', '--export=compiles', '--export=scene',
                str(out / 'test.o'), '-o', str(out / 'test.wasm')], check=True)
(out / 'run.cjs').write_text(r'''
const fs = require('fs');
const assert = require('assert/strict');
const bytes = fs.readFileSync(process.argv[2]);
async function fresh() { return (await WebAssembly.instantiate(bytes)).instance.exports; }
function run(p, n, period, work, interval=1, mode=0, res=1) {
  let slow;
  for (let i=0; i<n; i++) slow = p.frame(period, work, interval, mode, res);
  return slow;
}
// A demanding workload fails several trials, reaching the longest backoff.
function backoff(p) {
  run(p, 1, 16667, 12000);
  let slow = false, trials = 0;
  for (let i=0; i<12000 && trials<4; i++) {
    const next = p.frame(slow ? 33333 : 16667, slow ? 28000 : 18000, slow ? 2 : 1, 0, 1);
    if (slow && !next) trials++;
    slow = !!next;
  }
  assert.equal(trials, 4, 'overloaded workload must still get periodic trials');
  assert.equal(run(p, 90, 16667, 18000), 1, 'failed trial must return to 30');
}
(async () => {
  let p = await fresh();
  assert.equal(run(p, 600, 16667, 12000), 0, 'sustainable 60 must stay fast');
  assert.equal(run(p, 20, 16667, 18000), 1, 'sustained overload must fall back');
  assert.equal(run(p, 60, 33333, 28000, 2), 1, 'fallback must not oscillate immediately');
  assert.equal(p.target_us(), 16666, 'ADPF target remains 60 even in fallback');

  p = await fresh(); backoff(p);
  assert.equal(run(p, 60, 16667, 12000, 1, 0, 12), 0, 'lower resolution must clear old backoff');
  assert.equal(p.target_us(), 16666, 'resolution transition preserves ADPF target');

  p = await fresh(); backoff(p);
  assert.equal(run(p, 30, 33333, 12000, 2, 1), 1, 'fixed 30 is honored');
  assert.equal(run(p, 60, 16667, 12000), 0, 'return to Auto must clear fixed-mode history');

  p = await fresh(); backoff(p);
  assert.equal(p.frame(200000, 12000, 2, 0, 1), 1, 'ordinary hitch must not reset backoff');
  assert.equal(run(p, 60, 33333, 28000, 2), 1, 'hitch must not trigger another trial');
  assert.equal(run(p, 60, 16667, 18000, 1, 2), 0, 'fixed 60 is honored');
  p = await fresh(); p.pacing_off();
  assert.equal(run(p, 60, 33333, 28000, 2, 1), 0, 'PACE=0 must bypass fixed mode too');
  assert.equal(run(p, 600, 16667, 18000), 0, 'PACE=0 must bypass overload fallback');
  p = await fresh(); p.tracing_on();
  assert.equal(run(p, 600, 16667, 12000), 0, 'trace must preserve sustainable 60');
  assert.ok(p.logs() >= 4 && p.logs() < 10, 'trace must log summaries, not every frame');
  assert.equal(run(p, 20, 16667, 18000), 1, 'trace must preserve overload fallback');
  // shaders compiling (a new area: issue #3) - their long frames aren't a workload 60 doesn't fit
  p = await fresh();
  run(p, 60, 16667, 12000);
  p.compiles(3);
  assert.equal(run(p, 300, 20000, 18000), 0, 'frames while shaders compile must not force 30');
  p.compiles(0);
  assert.equal(run(p, 45, 20000, 18000), 0, 'nor those in the 1 s after the last one');
  assert.equal(run(p, 120, 16667, 18000), 1, 'an overload after the compiles still falls back');
  // a new scene's first seconds without compiles are real work (the GPU on its first draws): they still count
  p = await fresh();
  run(p, 600, 16667, 12000);
  p.scene(1);
  assert.equal(run(p, 20, 20000, 18000), 1, 'a new scene that does not fit 60 still falls back');
  console.log('20 Android pacing behavior checks passed (production C)');
})().catch(e => { console.error(e); process.exit(1); });
''')
subprocess.run(['node', str(out / 'run.cjs'), str(out / 'test.wasm')], check=True)
