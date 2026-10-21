#!/usr/bin/env python3
"""Generate real translated shaders, check arithmetic against uint64, and reject SPIR-V Int64 use."""
import argparse
import concurrent.futures
import struct
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=Path, default=root / 'build/shader-int32-tests')
p.add_argument('--cc', default='cc')
p.add_argument('--cxx', default='c++')
p.add_argument('--glslc', default='glslc')
p.add_argument('--generated-only', action='store_true', help='Validate files already produced by the native C/C++ tests')
p.add_argument('--legacy-shadow-extension', action='store_true', help='Old SDK glslc only: remove the unused shadow-LOD extension in these synthetic tests')
args = p.parse_args()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
if not args.generated_only:
    generator = out / 'generate'
    subprocess.run([args.cc, '-O2', str(root / 'tools/test_shader_int32.c'), '-lm', '-o', str(generator)], check=True)
    subprocess.run([str(generator), str(out)], check=True)
    math = out / 'math'
    subprocess.run([args.cxx, '-O2', '-fsanitize=undefined', str(out / 'math.cpp'), '-o', str(math)], check=True)
    subprocess.run([str(math)], check=True)

paths = sorted([*out.glob('case*.comp'), *out.glob('case*.vert'), *out.glob('case*.frag')])
if len(paths) != 186:
    raise RuntimeError(f'Expected 186 shaders from the production translator, found {len(paths)}')


def check(path):
    source = path.read_text()
    if 'uint64_t' in source or 'types_int64' in source:
        return str(path) + ': 64-bit shader source remains'
    compiled = path
    if args.legacy_shadow_extension:
        # No shadow samplers in these tests; do not change production source to accommodate an old compiler.
        compiled = path.with_name('sdk-' + path.name)
        compiled.write_text(source.replace('#extension GL_EXT_texture_shadow_lod : require\n', ''))
    spv = path.with_suffix(path.suffix + '.spv')
    r = subprocess.run([args.glslc, '--target-env=vulkan1.3', str(compiled), '-o', str(spv)], capture_output=True, text=True)
    if r.returncode:
        return str(path) + ': ' + r.stderr
    binary = spv.read_bytes()
    words = struct.unpack('<' + 'I' * (len(binary) // 4), binary)
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 65535
        if not n or i + n > len(words):
            return str(path) + ': malformed SPIR-V'
        if op == 17 and words[i + 1] == 11:
            return str(path) + ': Int64 capability remains'
        if op == 21 and words[i + 2] == 64:
            return str(path) + ': 64-bit integer type remains'
        i += n
    return None


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    errors = [e for e in pool.map(check, paths) if e]
if errors:
    raise RuntimeError('\n'.join(errors[:8]))
print(f'{len(paths)} translated shaders compiled without Int64 capability/types' +
      (' (unused shadow-LOD extension omitted for legacy SDK compiler)' if args.legacy_shadow_extension else ''))
