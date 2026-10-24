#!/usr/bin/env python3
"""Check Android Vulkan preflight decisions with production C on the PC (NDK + Node)."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'build' / 'vulkan-compat-tests'
out.mkdir(parents=True, exist_ok=True)
bin_dir = Path(sys.argv[1])
features = [
    ('v12', 'timelineSemaphore'),
    ('v12', 'samplerMirrorClampToEdge'),
    ('v12', 'scalarBlockLayout'),
    ('v13', 'maintenance4'),
]
source = '''
#include "device_compat.h"
void *memset(void *p, int c, __SIZE_TYPE__ n) {
    unsigned char *b = p;
    for (__SIZE_TYPE__ i = 0; i < n; i++) b[i] = (unsigned char)c;
    return p;
}
static int equal(const char *a, const char *b) {
    if (!a) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
int check(void) {
    VkPhysicalDeviceVulkan11Features v11 = {0};
    VkPhysicalDeviceVulkan12Features v12 = {0};
    VkPhysicalDeviceVulkan13Features v13 = {0};
'''
source += '\n'.join(f'    {group}.{field} = VK_TRUE;' for group, field in features)
source += '''
    // Optional 8-bit, 16-bit storage (v11: the Adreno 610 under Turnip), dynamic rendering, synchronization2,
    // demote and terminate (v13: other ways, device.c) and int64 flags are absent: do not reject this at preflight.
    if (vk_missing_backend_feature(&v11, &v12, &v13)) return 1;
'''
for index, (group, field) in enumerate(features, 2):
    source += f'''
    {group}.{field} = VK_FALSE;
    if (!equal(vk_missing_backend_feature(&v11, &v12, &v13), "{field}")) return {index};
    {group}.{field} = VK_TRUE;
'''
source += '''
    // Extension presence alone must not permit the formatless custom-border path.
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border = {0};
    if (vk_backend_border_supported(&border)) return 11;
    border.customBorderColors = VK_TRUE;
    if (vk_backend_border_supported(&border)) return 12;
    border.customBorderColors = VK_FALSE;
    border.customBorderColorWithoutFormat = VK_TRUE;
    if (vk_backend_border_supported(&border)) return 13;
    border.customBorderColors = VK_TRUE;
    if (!vk_backend_border_supported(&border)) return 14;
    VkQueueFamilyProperties queue = {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, .queueCount = 1};
    if (!vk_backend_queue_supported(&queue, VK_SUCCESS, VK_TRUE)) return 15;
    queue.queueCount = 0;
    if (vk_backend_queue_supported(&queue, VK_SUCCESS, VK_TRUE)) return 16;
    queue.queueCount = 1; queue.queueFlags = VK_QUEUE_GRAPHICS_BIT;
    if (vk_backend_queue_supported(&queue, VK_SUCCESS, VK_TRUE)) return 17;
    queue.queueFlags = VK_QUEUE_COMPUTE_BIT;
    if (vk_backend_queue_supported(&queue, VK_SUCCESS, VK_TRUE)) return 18;
    queue.queueFlags = VK_QUEUE_TRANSFER_BIT;
    if (vk_backend_queue_supported(&queue, VK_SUCCESS, VK_TRUE)) return 19;
    queue.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    if (vk_backend_queue_supported(&queue, VK_SUCCESS, VK_FALSE)) return 20;
    if (vk_backend_queue_supported(&queue, VK_ERROR_SURFACE_LOST_KHR, VK_TRUE)) return 21;
    return 0;
}
'''
(out / 'test.c').write_text(source)
subprocess.run([str(bin_dir / 'clang.exe'), '--target=wasm32', '-ffreestanding', '-O2', '-nostdlib',
                '-I', str(root / 'runtime/nvn/vulkan'), '-I', str(root / 'android/deps/out/include'),
                '-c', str(out / 'test.c'), '-o', str(out / 'test.o')], check=True)
subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=check',
                str(out / 'test.o'), '-o', str(out / 'test.wasm')], check=True)
(out / 'run.cjs').write_text('''
const fs = require('fs');
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({instance}) => {
  const code = instance.exports.check();
  if (code) throw new Error(`Vulkan preflight check ${code} failed`);
  console.log('16 Vulkan compatibility checks passed (production C)');
}).catch(e => {console.error(e); process.exit(1);});
''')
subprocess.run(['node', str(out / 'run.cjs'), str(out / 'test.wasm')], check=True)
