#!/usr/bin/env python3
"""Validate the diagnostic counter-pair history on the PC using production C (NDK + Node)."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'build' / 'gpu-timing-tests'
out.mkdir(parents=True, exist_ok=True)
bin_dir = Path(sys.argv[1])
source = r'''
#define __ANDROID__ 1
#include "timing_trace_pair.h"
int check(void) {
    VkTimingHistory h = {0};
    VkTimingCounter start, a = {.address=0x100000000ull, .frame=5, .seq=10, .gpu_ticks=6144000};
    if (vk_timing_pair(&h, &a, &start)) return 1;
    VkTimingCounter b = {.address=a.address+16, .frame=5, .seq=12, .gpu_ticks=12288000};
    if (!vk_timing_pair(&h, &b, &start) || start.address != a.address || start.seq != 10) return 2;
    // Reuse must choose the latest record, not a stale timestamp with the same address.
    a.frame=6; a.seq=20; a.gpu_ticks=18432000;
    vk_timing_pair(&h, &a, &start);
    b.frame=6; b.seq=22;
    if (!vk_timing_pair(&h, &b, &start) || start.seq != 20 || start.frame != 6) return 3;
    // Retain an independently traceable start when more than four other counters intervene.
    a.frame=7; a.seq=30;
    vk_timing_pair(&h, &a, &start);
    for (int i=0; i<32; i++) {
        VkTimingCounter unrelated = {.address=0x200000000ull+(uint64_t)i*64, .frame=7};
        vk_timing_pair(&h, &unrelated, &start);
    }
    b.frame=7; b.seq=40;
    if (!vk_timing_pair(&h, &b, &start) || start.seq != 30) return 4;
    // Cross-frame matches remain visible to the collector rather than hiding a mismatch.
    a.frame=8; a.seq=50; vk_timing_pair(&h, &a, &start);
    b.frame=9; b.seq=51;
    if (!vk_timing_pair(&h, &b, &start) || start.frame != 8) return 5;
    // Bounded history must expire old candidates even after many wraps.
    for (int i=0; i<1024; i++) {
        VkTimingCounter unrelated = {.address=0x300000000ull+(uint64_t)i*64, .seq=(uint64_t)i};
        vk_timing_pair(&h, &unrelated, &start);
    }
    if (h.count != VK_TIMING_HISTORY || h.next >= VK_TIMING_HISTORY) return 6;
    if (vk_timing_pair(&h, &b, &start)) return 7;
    VkTimingCounter low = {.address=8};
    if (vk_timing_pair(&h, &low, &start)) return 8;
    return 0;
}
'''
(out / 'test.c').write_text(source)
include = root / 'runtime/nvn/vulkan'
subprocess.run([str(bin_dir / 'clang.exe'), '--target=wasm32', '-ffreestanding', '-O2', '-nostdlib',
                '-I', str(include), '-c', str(out / 'test.c'), '-o', str(out / 'test.o')], check=True)
subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=check',
                str(out / 'test.o'), '-o', str(out / 'test.wasm')], check=True)
(out / 'run.cjs').write_text(r'''
const fs = require('fs');
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({instance}) => {
  const code = instance.exports.check();
  if (code) throw new Error(`GPU timing history check ${code} failed`);
  console.log('8 GPU timing history checks passed (production C)');
}).catch(e => {console.error(e); process.exit(1);});
''')
subprocess.run(['node', str(out / 'run.cjs'), str(out / 'test.wasm')], check=True)
