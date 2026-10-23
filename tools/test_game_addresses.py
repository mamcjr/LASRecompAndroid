"""Check the game-address tables of every game version (runtime/game_addresses/), without any game files.

Optional:
  --game KEY=FOLDER (repeatable, e.g. --game v1_0_0=ROM/01006BB00C6F0000 --game v1_0_1=/path/to/1.0.1): extracted game
    files of that version. Every patch's instruction check then runs on that version's main program, the randomizer's
    patch sites (tools/rando/patch_variants.json) must hold the same original bytes in every given version, each
    folder's release label is shown, and with v1_0_0 and another version the committed tables must equal
    tools/version_map.py's output.
  --compare-generated BEFORE AFTER: two complete generation folders made from the same exefs/main before and after a
    change must be byte-identical (gen.py's main_build_id.txt aside: older generators didn't write it).
"""
import argparse
import filecmp
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_addresses  # noqa: E402
from game_addresses import (TABLE_DIR, VERSIONS, UnknownGameVersion, load_table, select_version, table_path,  # noqa: E402
                            version_for_build_id)

ROOT = Path(__file__).resolve().parent.parent
MAIN_BASE = 0x7000000000
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--game', action='append', default=[], metavar='KEY=FOLDER')
ap.add_argument('--compare-generated', type=Path, nargs=2, metavar=('BEFORE', 'AFTER'))
args = ap.parse_args()

# ---- every version: the same names in the same order, one header each, distinct values
assert list(VERSIONS)[0] == 'v1_0_0'
tables = {}
for key in VERSIONS:
    patch_values, runtime_values = load_table(table_path(key)), load_table(table_path(key, 'runtime'))
    assert not patch_values.keys() & runtime_values.keys(), f'{key}: define each address in one header'
    values = patch_values | runtime_values
    assert len(values) == len(set(values.values())), f'{key}: use the existing name for shared addresses'
    tables[key] = (patch_values, runtime_values)
    if key != 'v1_0_0':  # (the C struct is laid out from v1_0_0's: runtime/game_addresses.h)
        assert list(patch_values) == list(tables['v1_0_0'][0]), f'patches_{key}.h names differ from v1_0_0'
        assert list(runtime_values) == list(tables['v1_0_0'][1]), f'runtime_{key}.h names differ from v1_0_0'
assert len({bid for _, bid in VERSIONS.values()}) == len(VERSIONS)
assert version_for_build_id('ae16f71e002af8cb059a9a74c4d90f34ba984892') == 'v1_0_0'
assert version_for_build_id('909E904AF78AC1B8DEEFE97AB2CCDB51968F0EC7') == 'v1_0_1'
for bad in ('', 'ae16f71e002af8cb', 'ae16f71e002af8cb059a9a74c4d90f34ba98489200000001', '00' * 20):
    try:
        version_for_build_id(bad)
    except UnknownGameVersion:
        continue
    raise AssertionError(f'build ID {bad!r} was accepted')

# versions.h's other lines name known versions; runtime/game_addresses.c includes every table
versions_h = (TABLE_DIR / 'versions.h').read_text(encoding='utf-8')
releases = re.findall(r'^GA_RELEASE\((v\d+_\d+_\d+),\s+"([^"]+)",\s+"([0-9a-f]{64})"\)', versions_h, re.M)
moves = re.findall(r'^GA_MOVED\((v\d+_\d+_\d+),\s+(0x[0-9a-f]+),\s+(0x[0-9a-f]+),\s+(0x[0-9a-f]+)\)', versions_h, re.M)
assert releases and all(k in VERSIONS for k, _, _ in releases) and all(k in VERSIONS for k, *_ in moves)
native = (ROOT / 'runtime/game_addresses.c').read_text(encoding='utf-8')
for key in VERSIONS:
    for kind in ('patches', 'runtime'):
        assert f'#include "game_addresses/{kind}_{key}.h"' in native, f'runtime/game_addresses.c: no {kind}_{key}.h'

# every version builds the same patches (tools/patches.py reads the selected table on import)
counts = {}
for key in VERSIONS:
    select_version(key)
    patches = importlib.reload(importlib.import_module('patches'))
    counts[key] = len(patches.PATCHES)
assert len(set(counts.values())) == 1, f'patch count per version: {counts}'
print(f'{len(VERSIONS)} versions, {len(tables["v1_0_0"][0])} patch / {len(tables["v1_0_0"][1])} runtime addresses, '
      f'{counts["v1_0_0"]} patches each')

# ---- with game files
games = {}
for g in args.game:
    key, _, folder = g.partition('=')
    assert key in VERSIONS and folder, f'--game {g}: KEY=FOLDER, KEY one of {", ".join(VERSIONS)}'
    games[key] = Path(folder)
if games:
    from nso import Nso
    images = {}
    for key, folder in games.items():
        n = Nso(str(folder / 'exefs/main'))
        assert version_for_build_id(n.build_id) == key, f'{folder}: main build ID {n.build_id[:16]} is not {key}'
        images[key] = n
        select_version(key)
        patches = importlib.reload(importlib.import_module('patches'))
        gen = SimpleNamespace(nso=n, base=MAIN_BASE)
        failed = []
        for a, fn in sorted(patches.PATCHES.items()):
            try:
                fn(gen, a)
            except AssertionError as e:
                failed.append(f'{a:#x} ({fn.__name__}): {e}')
        assert not failed, f'{key}: patch checks failed:\n  ' + '\n  '.join(failed)
        npdm = folder / 'exefs/main.npdm'
        sha = hashlib.sha256(npdm.read_bytes()).hexdigest() if npdm.exists() else ''
        label = next((l for k, l, s in releases if k == key and s == sha), VERSIONS[key][0])
        print(f'{key} ({folder}): release {label}, {len(patches.PATCHES)} patch checks passed')
    # the randomizer reads 1.0.0.pchtxt on every version (runtime/rando.c): the same original bytes at every site
    variants = json.loads((ROOT / 'tools/rando/patch_variants.json').read_text())
    sites = [(int(o, 16), 4) for o in variants['code']] + [(int(o, 16), 32) for o in variants['data']]
    ref = next(iter(images.values()))
    for key, n in images.items():
        bad = [f'main+{o:#x}' for o, size in sites if bytes(n.image[o:o + size]) != bytes(ref.image[o:o + size])]
        assert not bad, f'{key}: randomizer patch sites differ: {", ".join(bad)}'
    print(f'{len(sites)} randomizer patch sites hold the same bytes in {", ".join(images)}')
    if 'v1_0_0' in games:
        for key in games:
            if key != 'v1_0_0':
                subprocess.run([sys.executable, str(ROOT / 'tools/version_map.py'), str(games['v1_0_0']), str(games[key]),
                                '--from', 'v1_0_0', '--to', key, '--check'], check=True, stdout=subprocess.DEVNULL)
                print(f'{key}: the tables and GA_MOVED lines match tools/version_map.py')

# ---- generated code before / after a change
if args.compare_generated:
    before, after = args.compare_generated
    diffs = []

    def walk(c, rel=''):
        diffs.extend(f'{rel}{n}' for n in c.diff_files + c.left_only + c.right_only if n != 'main_build_id.txt')
        diffs.extend(f'{rel}{n}' for n in c.funny_files)
        for n, sub in c.subdirs.items():
            walk(sub, f'{rel}{n}/')

    walk(filecmp.dircmp(before, after, ignore=[]))  # (files of another size or time are compared by contents)
    assert not diffs, f'generated code differs: {", ".join(sorted(set(diffs))[:20])}'
    print(f'{before} and {after}: the same generated code')
print('ok')
