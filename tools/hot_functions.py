#!/usr/bin/env python3
"""The game functions a play session entered, for the hot/cold game code build (runtime/gamebuild.c, Android).

Collect on the phone (counts from process start, the file rewritten every 1800 frames while playing):
  adb shell am start -n com.lasrecomp.android/com.lasrecomp.LASRecompActivity --es env \\
    'LARECOMP_FNPROF=0:999999999,LARECOMP_FNPROF_EVERY=1800,LARECOMP_FNPROF_OUT=/sdcard/Android/data/com.lasrecomp.android/files/fnprof.txt'
play through as many places as possible, then
  adb pull /sdcard/Android/data/com.lasrecomp.android/files/fnprof.txt session1.txt
Merge one or more sessions into the list the APK ships (android/lasrecomp/app/build.gradle.kts copies it):
  hot_functions.py session1.txt [session2.txt ...] [-o tools/hot_functions.txt]

Addresses are written as the generated code names its functions (f_<address>: main at 0x7000000000, sdk at
0x7004000000), whatever the guest window was in the session (its '# main_base' line). Only addresses: no game code."""
import argparse
from pathlib import Path

NAMED_MAIN = 0x7000000000

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('sessions', nargs='+', type=Path, help='fnprof.txt files (runtime/fnprof.c)')
p.add_argument('-o', '--out', type=Path, default=Path(__file__).resolve().parent / 'hot_functions.txt')
p.add_argument('--min-count', type=int, default=1, help='entries a function needs, summed over the sessions')
args = p.parse_args()

counts = {}
for path in args.sessions:
    base = NAMED_MAIN  # (no '# main_base' line: a desktop session, addresses already as named)
    for line in path.read_text().splitlines():
        if line.startswith('# main_base '):
            base = int(line.split()[2], 16)
            continue
        if not line.strip() or line.startswith('#'):
            continue
        addr, count = line.split()[:2]
        named = int(addr, 16) - base + NAMED_MAIN
        if not NAMED_MAIN <= named < NAMED_MAIN + 0x8000000:  # (main and sdk: anything else isn't generated code)
            continue
        counts[named] = counts.get(named, 0) + int(count)

hot = sorted(a for a, n in counts.items() if n >= args.min_count)
args.out.write_text(f'# {len(hot)} hot game functions from {len(args.sessions)} session(s): tools/hot_functions.py\n' +
                    ''.join(f'{a:x}\n' for a in hot))
print(f'{len(hot)} hot functions -> {args.out}')
