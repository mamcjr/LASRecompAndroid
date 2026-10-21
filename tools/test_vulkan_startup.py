#!/usr/bin/env python3
"""Execute production surface/cache policy on the PC using NDK clang and Node; no driver or phone."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'build/vulkan-startup-tests'
out.mkdir(parents=True, exist_ok=True)
bin_dir = Path(sys.argv[1])
source = r'''
#include "device_compat.h"
void *memset(void *p, int c, __SIZE_TYPE__ n) {
    unsigned char *b = p;
    for (__SIZE_TYPE__ i = 0; i < n; i++) b[i] = (unsigned char)c;
    return p;
}
static void put(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static int cases;
#define CHECK(x) do { cases++; if (!(x)) return -cases; } while (0)
int check(void) {
    cases = 0;
    uint8_t bytes[65] = {0}, *p = bytes + 1; // deliberately not relying on C struct alignment/packing
    VkPhysicalDeviceProperties gpu = {.vendorID = 0x5143, .deviceID = 0x44050001};
    put(p, 32); put(p+4, 1); put(p+8, gpu.vendorID); put(p+12, gpu.deviceID);
    for (int i = 0; i < VK_UUID_SIZE; i++) p[16+i] = gpu.pipelineCacheUUID[i] = (uint8_t)(i+1);
    CHECK(vk_backend_cache_compatible(p, 64, &gpu));
    CHECK(!vk_backend_cache_compatible(0, 64, &gpu));
    CHECK(!vk_backend_cache_compatible(p, 31, &gpu));
    CHECK(!vk_backend_cache_compatible(p, 0, &gpu));
    CHECK(!vk_backend_cache_compatible(p, (size_t)VK_BACKEND_PIPELINE_CACHE_MAX+1, &gpu));
    put(p, 31); CHECK(!vk_backend_cache_compatible(p, 64, &gpu)); put(p, 32);
    put(p, 64); CHECK(!vk_backend_cache_compatible(p, 64, &gpu)); put(p, 32);
    put(p+4, 2); CHECK(!vk_backend_cache_compatible(p, 64, &gpu)); put(p+4, 1);
    p[8] ^= 1; CHECK(!vk_backend_cache_compatible(p, 64, &gpu)); p[8] ^= 1;
    p[12] ^= 1; CHECK(!vk_backend_cache_compatible(p, 64, &gpu)); p[12] ^= 1;
    p[31] ^= 1; CHECK(!vk_backend_cache_compatible(p, 64, &gpu)); p[31] ^= 1;
    CHECK(vk_backend_cache_compatible(p, 32, &gpu)); // header-only policy, not opaque payload validation
    for (unsigned mask = 0; mask < 16; mask++) {
        VkCompositeAlphaFlagBitsKHR a = vk_backend_composite_alpha(mask);
        CHECK(mask ? (a && !(a & (a-1)) && (a & mask) && (!(mask & 1) || a == 1)) : a == 0);
    }
    CHECK(vk_backend_swapchain_usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    CHECK(vk_backend_swapchain_usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ==
          (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    CHECK(vk_backend_swapchain_usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    CHECK(vk_backend_swapchain_usage(0) == 0);
    CHECK(vk_backend_swapchain_usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0);
    return cases;
}
'''
(out / 'test.c').write_text(source)
subprocess.run([str(bin_dir / 'clang.exe'), '--target=wasm32', '-ffreestanding', '-O2', '-nostdlib',
                '-I', str(root / 'runtime/nvn/vulkan'), '-I', str(root / 'android/deps/out/include'),
                '-c', str(out / 'test.c'), '-o', str(out / 'test.o')], check=True)
subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=check',
                str(out / 'test.o'), '-o', str(out / 'test.wasm')], check=True)
(out / 'run.cjs').write_text(r'''
const fs = require('fs');
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({instance}) => {
  const cases = instance.exports.check();
  if (cases < 0) throw Error(`Startup check ${-cases} failed`);
  console.log(`${cases} Vulkan startup checks passed (production C; no device)`);
}).catch(e => { console.error(e); process.exitCode=1; });
''')
subprocess.run(['node', str(out / 'run.cjs'), str(out / 'test.wasm')], check=True)
