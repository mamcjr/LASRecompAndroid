#!/usr/bin/env python3
"""Hot / cold game code build (runtime/gamebuild.c, Android) on the PC: the production load_hot / mark_cold on a
synthetic generated file (Linux / WSL cc), and tools/hot_functions.py's address conversion. Usage:
test_gamebuild_hot.py [--cc cc]"""
import argparse
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', default='cc')
args = p.parse_args()
out = root / 'build' / 'gamebuild-hot-tests'
out.mkdir(parents=True, exist_ok=True)
checks = 0

# 1. tools/hot_functions.py: a phone session (guest window at 0x100000000) and a desktop one, merged
phone = out / 'phone.txt'
phone.write_text('# main_base 100000000\n100005350 7\n104001000 2\n100000010 1\n200000000 5\n')
desk = out / 'desk.txt'
desk.write_text('7000005350 3\n7000009000 1\n')
hot = out / 'hot.txt'
subprocess.run([sys.executable, str(root / 'tools/hot_functions.py'), str(phone), str(desk), '-o', str(hot)],
               check=True, capture_output=True)
lines = [l for l in hot.read_text().splitlines() if not l.startswith('#')]
assert lines == ['7000000010', '7000005350', '7000009000', '7004001000'], lines
checks += 1
subprocess.run([sys.executable, str(root / 'tools/hot_functions.py'), str(phone), str(desk), '-o', str(hot),
                '--min-count', '3'], check=True, capture_output=True)
lines = [l for l in hot.read_text().splitlines() if not l.startswith('#')]
assert lines == ['7000005350'], lines  # (7 + 3 entries: the same function in both sessions)
checks += 1

# 2. mark_cold, the production code, on a generated-looking file
source = (root / 'runtime/gamebuild.c').read_text()
start = source.index('static uint64_t *g_hot;')
body = source[start:source.index('#endif', source.index('static bool mark_cold('))]
(out / 'mark.c').write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
''' + body + r'''
int main(int argc, char **argv) {
    load_hot(argv[1]);
    if (!mark_cold(argv[2])) return 1;
    printf("%llu %llu %zu\n", g_cold_fns, g_all_fns, g_nhot);
    return 0;
}
''')
chunk = ('#include "cpu.h"\n'
         'void f_7000000010(Cpu *c);\n'
         'void f_7000005350(Cpu *c);\n'
         'void f_7000009000(Cpu *c) {\n    f_7000000010(c);\n}\n'
         '\n'
         'void f_7000000010(Cpu *c) {\n    /* hot */\n}\n'
         'void f_70000aaaa0(Cpu *c) {\n    // void f_7000000020(Cpu *c) { (not at a line start)\n}\n')
expected = chunk.replace('void f_7000009000(Cpu *c) {', '__attribute__((optnone, noinline)) void f_7000009000(Cpu *c) {') \
                .replace('void f_70000aaaa0(Cpu *c) {', '__attribute__((optnone, noinline)) void f_70000aaaa0(Cpu *c) {')
(out / 'list.txt').write_text('# test\n7000000010\n7000005350\n')
src = out / 'chunk.c'
src.write_bytes(chunk.encode())
subprocess.run([args.cc, '-O1', '-std=gnu11', str(out / 'mark.c'), '-o', str(out / 'mark')], check=True)
r = subprocess.run([str(out / 'mark'), str(out / 'list.txt'), str(src)], check=True, capture_output=True, text=True)
assert r.stdout.split() == ['2', '3', '2'], r.stdout  # (2 cold of 3 definitions; 2 hot addresses loaded)
checks += 1
assert src.read_bytes() == expected.encode(), src.read_text()
checks += 1
assert not (out / 'chunk.c.cold').exists(), 'temporary file left behind'
checks += 1
print(f'{checks} hot/cold build checks passed (production C and tools/hot_functions.py)')
