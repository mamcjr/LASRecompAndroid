#!/usr/bin/env python3
"""Replay production Dynamic resolution (output_scale.c dyn_update) on the PC, Android and desktop builds (NDK + Node)."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'build' / 'dynamic-resolution-tests'
out.mkdir(parents=True, exist_ok=True)
bin_dir = Path(sys.argv[1])
source = (root / 'runtime/nvn/output_scale.c').read_text()
start = source.index('// Dynamic resolution: a factor')
body = source[start:source.index('double output_scale_dynamic_factor', start)]
preamble = r'''
typedef _Bool bool;
#define true 1
#define false 0
#define LOG(...) ((void)0)
struct { int fps_mode; } g_settings;
static double fmax(double a, double b) { return a > b ? a : b; }
static double fmin(double a, double b) { return a < b ? a : b; }
static double round(double x) { return (double)(long long)(x + 0.5); }
'''
wrapper = r'''
static double now;
double frames(int n, double dt_ms, int mode) {  // n presented frames dt_ms apart; the factor after them
    g_settings.fps_mode = mode;
    for (int i = 0; i < n; i++) { now += dt_ms / 1000.0; dyn_update(now, -1); }
    return g_dyn;
}
double frames_gpu(int n, double dt_ms, double gpu_ms, int mode) {  // the same with the GPU's time per frame
    g_settings.fps_mode = mode;
    for (int i = 0; i < n; i++) { now += dt_ms / 1000.0; dyn_update(now, gpu_ms); }
    return g_dyn;
}
'''
runner = r'''
const fs = require('fs');
const assert = require('assert/strict');
async function load(file) { return (await WebAssembly.instantiate(fs.readFileSync(file))).instance.exports; }
(async () => {
  let a = await load(process.argv[2]);
  assert.equal(a.frames(1200, 33.3, 1), 1, 'fixed 30 on target keeps full resolution');
  a = await load(process.argv[2]);
  assert.ok(a.frames(600, 45, 1) < 1, 'fixed 30 missing its target still lowers it');
  a = await load(process.argv[2]);
  assert.ok(a.frames(600, 33.3, 0) < 1, 'Auto still aims at 60');
  a = await load(process.argv[2]);
  assert.equal(a.frames(1200, 16.7, 2), 1, 'fixed 60 on target keeps full resolution');
  // with the GPU's time (issue #3: a new area's shaders compiling - long frames, the GPU idle - took it to 0.6)
  a = await load(process.argv[2]);
  assert.equal(a.frames_gpu(600, 36, 6, 0), 1, 'long frames with the GPU idle keep full resolution');
  a = await load(process.argv[2]);
  assert.ok(a.frames_gpu(600, 20, 18, 0) < 1, 'a GPU at its limit lowers it');
  a = await load(process.argv[2]);
  assert.equal(a.frames_gpu(600, 33.3, 20, 1), 1, 'fixed 30: 20 ms of GPU is within its budget');
  a = await load(process.argv[2]);
  assert.equal(a.frames_gpu(600, 16.7, 100, 0), 1, 'a GPU time above the frame time is ignored (frame time on target)');
  a = await load(process.argv[2]);
  const low = a.frames_gpu(300, 20, 18, 0);
  assert.ok(low < 1 && a.frames_gpu(1200, 16.7, 8, 0) > low, 'it comes back up with room on the GPU');
  const d = await load(process.argv[3]);
  assert.ok(d.frames(600, 33.3, 1) < 1, 'desktop build unchanged');
  const d2 = await load(process.argv[3]);
  assert.ok(d2.frames_gpu(600, 36, 6, 0) < 1, 'desktop build unchanged with a GPU time (long frames lower it)');
  console.log('11 Dynamic resolution checks passed (production C)');
})().catch(e => { console.error(e); process.exit(1); });
'''
for name, android in (('android', True), ('desktop', False)):
    c = out / f'{name}.c'
    c.write_text(('#define __ANDROID__ 1\n' if android else '') + preamble + body + wrapper)
    subprocess.run([str(bin_dir / 'clang.exe'), '--target=wasm32', '-O2', '-fno-builtin', '-nostdlib',
                    '-c', str(c), '-o', str(out / f'{name}.o')], check=True)
    subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=frames', '--export=frames_gpu',
                    str(out / f'{name}.o'), '-o', str(out / f'{name}.wasm')], check=True)
(out / 'run.cjs').write_text(runner)
subprocess.run(['node', str(out / 'run.cjs'), str(out / 'android.wasm'), str(out / 'desktop.wasm')], check=True)
