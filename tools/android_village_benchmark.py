#!/usr/bin/env python3
"""Frame-scripted village benchmark; `plan` is read-only, `run` restores the installed APK in finally."""
import argparse
import hashlib
import json
import math
import re
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PKG = 'com.lasrecomp.android'
BASE = '/sdcard/Android/data/' + PKG + '/files'
TITLE = '400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6'
VARIANTS = {
    'control': '',
    'descriptor': 'LARECOMP_VK_DESCRIPTOR_PLAN=1',
    'upload': 'LARECOMP_VK_UPLOAD_BARRIER=1',
    'combined': 'LARECOMP_VK_DESCRIPTOR_PLAN=1,LARECOMP_VK_UPLOAD_BARRIER=1',
}
MARKER = re.compile(r'\[benchmark\] frame (\d+) requested (\d+) monotonic-ns (\d+)')
POSITION = re.compile(r'\[link\] f(\d+) \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)')


def schedule(route):
    frame = route['start_frame']
    legs = []
    for loop in range(route['loops']):
        for leg in route['legs']:
            end = frame + leg['frames'] + route['pause_frames']
            legs.append(dict(leg, loop=loop + 1, start=frame, end=end,
                             checkpoint_frame=end - 60))
            frame = end
    return legs


def summarize(edges, target_fps=60):
    values = sorted((b - a) / 1e6 for a, b in edges)
    if not values:
        return {'intervals': 0}
    def percentile(p):
        x = (len(values) - 1) * p
        lo, hi = math.floor(x), math.ceil(x)
        return values[lo] + (values[hi] - values[lo]) * (x - lo)
    budget_ms = 1000 / target_fps
    late_threshold_ms = budget_ms + max(.5, budget_ms * .05)
    return dict(intervals=len(values), covered_s=sum(values) / 1000,
                target_fps=target_fps, budget_ms=budget_ms, late_threshold_ms=late_threshold_ms,
                late_pct=100 * sum(v > late_threshold_ms for v in values) / len(values),
                fps=1000 * len(values) / sum(values), p50_ms=percentile(.5),
                p95_ms=percentile(.95), p99_ms=percentile(.99), max_ms=max(values),
                over20_pct=100 * sum(v > 20 for v in values) / len(values))


def run_benchmark(args, route, legs):
    def stats(edges):
        return summarize(edges, args.target_fps)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    def adb(*cmd, binary=False, check=True):
        for attempt in range(3 if cmd[0] == 'push' else 1):
            r = subprocess.run([args.adb, '-s', args.serial, *cmd], capture_output=True, timeout=120)
            if not check or r.returncode == 0:
                return r.stdout if binary else r.stdout.decode(errors='replace')
            message = f'{cmd[0]} failed ({r.returncode}): ' + (r.stdout + r.stderr).decode(errors='replace')
            with (out / 'adb-errors.txt').open('a', encoding='utf-8') as errors:
                errors.write(message + '\n')
            if cmd[0] != 'push' or attempt == 2:
                raise RuntimeError(message)
            time.sleep(.5)
    def digest(data):
        return hashlib.sha256(data).hexdigest()
    if adb('shell', 'pidof', PKG, check=False).strip():
        raise RuntimeError('App is open. Save and close it before running the benchmark.')
    snapshot = args.snapshot.resolve()
    if not (snapshot / 'save_0' / 'savedata3.bin').is_file():
        raise RuntimeError('--snapshot must contain save_0/savedata3.bin at the calibrated village position.')
    installed = adb('shell', 'pm', 'path', PKG).strip().removeprefix('package:')
    if '\n' in installed or not installed.startswith('/data/app/'):
        raise RuntimeError('Expected a single installed APK.')
    adb('pull', installed, str(out / 'installed-before.apk'))
    config = adb('exec-out', 'cat', BASE + '/larecomp.ini', binary=True)
    (out / 'settings-before.ini').write_bytes(config)
    adb('pull', BASE + '/save', str(out / 'save-before'))
    shutil.copytree(snapshot, out / 'route-snapshot')
    (out / 'route.json').write_text(json.dumps(route, indent=2), encoding='utf-8')
    scratch = BASE + '/benchmark-' + out.name
    if not re.fullmatch(r'[A-Za-z0-9_-]+', out.name):
        raise RuntimeError('Output directory name must use letters, digits, hyphens or underscores.')
    markers = sorted({frame for leg in legs for frame in
                      (leg['start'], leg['start'] + leg['frames'], leg['end'])})
    spec = TITLE + ''.join(f",{leg['start']}:{leg['stick']}:{leg['frames']}" for leg in legs)
    env = (f'LARECOMP_SAVE_DIR={scratch},LARECOMP_INPUT={spec},LARECOMP_NO_USER_INPUT=1,'
           'LARECOMP_LOG_LINK=60,LARECOMP_TEXTURE_TRACE=1,LARECOMP_FPS_INTERVAL=2,LARECOMP_BENCHMARK_MARKERS='
           + ','.join(map(str, markers)))
    if args.cpu_bc:
        env += ',LARECOMP_NO_BC=1'
    if VARIANTS[args.variant]:
        env += ',' + VARIANTS[args.variant]
    manifest = dict(route=route['id'], diagnostic_apk_sha256=digest(args.apk.read_bytes()),
                    installed_apk_sha256=digest((out / 'installed-before.apk').read_bytes()),
                    config_sha256=digest(config), cpu_bc=args.cpu_bc, variant=args.variant,
                    measurement_target_fps=args.target_fps, env=env,
                    snapshot={str(f.relative_to(snapshot)): digest(f.read_bytes())
                              for f in snapshot.rglob('*') if f.is_file()})
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    logger = None
    try:
        (out / 'thermal-before.txt').write_text(adb('shell', 'dumpsys', 'thermalservice'), encoding='utf-8')
        adb('shell', 'mkdir', '-p', scratch)
        adb('push', str(out / 'route-snapshot' / 'save_0'), scratch + '/')
        adb('install', '-r', str(args.apk.resolve()))
        launch = adb('shell', 'am', 'start', '-n', PKG + '/com.lasrecomp.LASRecompActivity', '--es', 'env', env)
        (out / 'launch.txt').write_text(launch, encoding='utf-8')
        pid = ''
        for _ in range(40):
            pid = adb('shell', 'pidof', PKG, check=False).strip()
            if pid:
                break
            time.sleep(.5)
        if not pid:
            raise RuntimeError('App did not start; inspect launch.txt.')
        with (out / 'logcat.txt').open('wb') as log, (out / 'surface.jsonl').open('w') as raw:
            logger = subprocess.Popen([args.adb, '-s', args.serial, 'logcat', '-v', 'epoch',
                                       '--pid=' + pid, '-s', 'LASRecomp'], stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + args.timeout
            layer = None
            last_present, stale_samples = None, 0
            edges, frames = set(), set()
            while time.monotonic() < deadline:
                if layer is None:
                    layers = adb('shell', 'dumpsys', 'SurfaceFlinger', '--list')
                    candidates = re.findall(r'RequestedLayerState\{([^\n]+?\(BLAST\)#\d+)',
                                            '\n'.join(x for x in layers.splitlines() if 'SurfaceView[' + PKG in x))
                    found = max(candidates, key=lambda name: int(name.rsplit('#', 1)[1]), default=None)
                    if found:
                        layer = found
                        with (out / 'layers.jsonl').open('a', encoding='utf-8') as history:
                            history.write(json.dumps(dict(layer=layer, host_monotonic=time.monotonic())) + '\n')
                if layer:
                    sample = adb('shell', f'dumpsys SurfaceFlinger --latency "{layer}"')
                    raw.write(json.dumps(sample) + '\n')
                    observed = sorted({int(row.split()[1]) for row in sample.splitlines()[1:]
                                       if len(row.split()) == 3 and row.split()[1].isdigit()
                                       and 0 < int(row.split()[1]) < 9223372036854775807})
                    frames.update(observed)
                    edges.update(zip(observed, observed[1:]))
                    if observed and observed[-1] != last_present:
                        last_present, stale_samples = observed[-1], 0
                    else:
                        stale_samples += 1
                        if stale_samples >= 3:
                            # SDL/Android may replace the startup surface. Rediscover instead of measuring its old layer.
                            layer, last_present, stale_samples = None, None, 0
                text = (out / 'logcat.txt').read_text(encoding='utf-8', errors='replace')
                if re.search(r'FATAL|\*\*\* signal', text):
                    raise RuntimeError('Runtime failure; inspect logcat.txt.')
                if any(int(m[1]) == markers[-1] for m in MARKER.findall(text)):
                    break
                if not adb('shell', 'pidof', PKG, check=False).strip():
                    raise RuntimeError('App exited during the route.')
                time.sleep(.8)
            else:
                raise RuntimeError('Route timed out; check screen lock, scene and marker support.')
            time.sleep(.5)
            logger.terminate(); logger.wait(timeout=10); logger = None
        text = (out / 'logcat.txt').read_text(encoding='utf-8', errors='replace')
        times = {int(requested): int(ns) for frame, requested, ns in MARKER.findall(text)
                 if frame == requested}
        positions = {int(f): [float(x), float(y), float(z)] for f, x, y, z in POSITION.findall(text)}
        errors = []
        if any(frame not in times for frame in markers):
            raise RuntimeError('Missing or late CPU frame markers.')
        wrong_levels = [(int(frame), level) for frame, level in re.findall(r'\] t=[\d.]+ frame (\d+).*? level (\S+)', text)
                        if markers[0] <= int(frame) <= markers[-1] and level != route['level']]
        checks = [(route['start_frame'], route['start_position'])] + [
            (leg['checkpoint_frame'], leg['checkpoint']) for leg in legs]
        for frame, expected in checks:
            actual = positions.get(frame)
            if actual is None or math.dist(actual, expected) > route['position_tolerance']:
                errors.append(dict(frame=frame, expected=expected, actual=actual))
        sections = []
        for leg in legs:
            lo, hi = times[leg['start']], times[leg['end']]
            sections.append(dict(loop=leg['loop'], name=leg['name'], **stats(
                {(a, b) for a, b in edges if lo <= a < b <= hi})))
        lo, hi = times[markers[0]], times[markers[-1]]
        valid_edges = {(a, b) for a, b in edges if lo <= a < b <= hi}
        valid_frames = {f for f in frames if lo <= f <= hi}
        gaps = max(0, len(valid_frames) - 1 - len(valid_edges))
        loops = []
        for loop in range(1, route['loops'] + 1):
            lap = [leg for leg in legs if leg['loop'] == loop]
            a, b = times[lap[0]['start']], times[lap[-1]['end']]
            loops.append(dict(loop=loop, **stats({(x, y) for x, y in edges if a <= x < y <= b})))
        phases = {}
        for name in ('moving', 'paused'):
            phase_edges = set()
            for leg in legs:
                split = leg['start'] + leg['frames']
                a, b = (leg['start'], split) if name == 'moving' else (split, leg['end'])
                phase_edges.update((x, y) for x, y in edges if times[a] <= x < y <= times[b])
            phases[name] = stats(phase_edges)
        result = dict(valid_route=not errors and not wrong_levels and not gaps and bool(valid_edges),
                      checkpoint_errors=errors, unexpected_levels=wrong_levels, history_gaps=gaps, sections=sections,
                      loops=loops, phases=phases,
                      **stats(valid_edges))
        (out / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        (out / 'finish.png').write_bytes(adb('exec-out', 'screencap', '-p', binary=True))
        print(json.dumps({k: v for k, v in result.items() if k not in ('sections', 'loops', 'phases')}, indent=2), flush=True)
        if not result['valid_route']:
            raise RuntimeError('Invalid route/capture; inspect result.json before comparing performance.')
    finally:
        if logger:
            logger.terminate(); logger.wait(timeout=10)
        try:
            (out / 'thermal-after.txt').write_text(adb('shell', 'dumpsys', 'thermalservice'), encoding='utf-8')
        except (subprocess.SubprocessError, OSError) as error:
            (out / 'thermal-after-error.txt').write_text(str(error), encoding='utf-8')
        adb('shell', 'am', 'force-stop', PKG)
        adb('install', '-r', str(out / 'installed-before.apk'))
        adb('push', str(out / 'settings-before.ini'), BASE + '/larecomp.ini')
        for f in (out / 'save-before').rglob('*'):
            if f.is_file() and digest(adb('exec-out', 'cat', BASE + '/save/' + f.relative_to(out / 'save-before').as_posix(), binary=True)) != digest(f.read_bytes()):
                raise RuntimeError('Original save changed; backup preserved at ' + str(out / 'save-before'))
        installed = adb('shell', 'pm', 'path', PKG).strip().removeprefix('package:')
        actual = adb('shell', 'sha256sum', installed).split()[0]
        if actual != manifest['installed_apk_sha256'] or adb('exec-out', 'cat', BASE + '/larecomp.ini', binary=True) != config:
            raise RuntimeError('APK/config restoration verification failed.')
        (out / 'restored.txt').write_text('Original APK/config restored; original save hashes unchanged. App closed.\n')
        print('Original APK/config restored; saves unchanged; app closed.', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('command', choices=['plan', 'run'])
    p.add_argument('--route', type=Path, default=ROOT / 'tools/benchmarks/village-route.json')
    p.add_argument('--adb', default='adb')
    p.add_argument('--serial')
    p.add_argument('--apk', type=Path)
    p.add_argument('--snapshot', type=Path, help='Private immutable snapshot containing save_0/')
    p.add_argument('--out', type=Path)
    p.add_argument('--timeout', type=int, default=600)
    p.add_argument('--cpu-bc', action='store_true')
    p.add_argument('--target-fps', type=int, choices=(30, 60, 120), default=60,
                   help='Reporting budget only; does not change game speed, FPS setting, display or ADPF.')
    p.add_argument('--variant', choices=VARIANTS, default='control',
                   help='One experimental renderer variant; ADPF is unchanged.')
    args = p.parse_args()
    if args.cpu_bc and args.variant != 'control':
        p.error('--cpu-bc cannot be mixed with renderer variants')
    route = json.loads(args.route.read_text(encoding='utf-8'))
    legs = schedule(route)
    if args.command == 'plan':
        print(json.dumps(dict(route=route['id'], nominal_seconds_at_60fps=(legs[-1]['end'] - legs[0]['start']) / 60,
                              legs=legs), indent=2))
    else:
        if not all((args.serial, args.apk, args.snapshot, args.out)):
            p.error('run requires --serial, --apk, --snapshot and --out')
        run_benchmark(args, route, legs)


if __name__ == '__main__':
    main()
