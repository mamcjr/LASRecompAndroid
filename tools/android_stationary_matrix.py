#!/usr/bin/env python3
"""Stationary full-factorial benchmark. Keeps raw telemetry private; restores the APK in finally."""
import argparse
import hashlib
import importlib.util
import itertools
import json
import re
import subprocess
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PKG = 'com.lasrecomp.android'
BASE = '/sdcard/Android/data/' + PKG + '/files'
TITLE = '400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6'
RESOLUTIONS = [('50%', 12), ('75%', 10), ('1x', 1), ('Dynamic', 9), ('Match Display', 0)]
ACK = re.compile(r'\[matrix\] applied (\d+) frame (\d+) monotonic-ns (\d+) fps (\d+) res (\d+) upscaler (\d+) aa (\d+) aspect (\d+)')
POSITION = re.compile(r'\[link\] f\d+ \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)')
spec = importlib.util.spec_from_file_location('village', ROOT / 'tools/android_village_benchmark.py')
village = importlib.util.module_from_spec(spec)
spec.loader.exec_module(village)


def profiles(include_2x=False):
    resolutions = RESOLUTIONS + ([('2x', 2)] if include_2x else [])
    return [dict(id=i, resolution=res[0], res_mode=res[1], aspect=aspect[0], aspect_mode=aspect[1],
                 upscaler=up[0], upscaler_mode=up[1], aa=aa[0], aa_mode=aa[1], frame_rate='auto')
            for i, (res, aspect, up, aa) in enumerate(itertools.product(
                resolutions, [('16:9', 1), ('Unlocked', 0)], [('Bilinear', 0), ('FSR1', 2)],
                [('Off', 0), ('SMAA', 1)]), 1)]


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def run_matrix(args):
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    variants = profiles(args.include_2x)
    command = BASE + '/stationary-matrix.command'
    settings = BASE + '/stationary-matrix.ini'
    scratch = BASE + '/stationary-matrix-save'
    stop = threading.Event()
    processes = []
    streams = []
    backup = out / 'installed-before.apk'
    changed_apk = False
    old_stayon = None

    def adb(*cmd, check=True, timeout=30):
        result = subprocess.run([args.adb, '-s', args.serial, *cmd], capture_output=True, timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(f'adb {cmd[0]}: ' + (result.stdout + result.stderr).decode(errors='replace'))
        return result.stdout.decode(errors='replace')

    def save_text(path, text):
        path.write_text(text, encoding='utf-8')

    def shell(text):
        return adb('shell', text)

    def screenshot(path):
        result = subprocess.run([args.adb, '-s', args.serial, 'exec-out', 'screencap', '-p'], capture_output=True, timeout=20, check=True)
        path.write_bytes(result.stdout)

    def app_alive(pid):
        return pid in adb('shell', 'pidof', PKG, check=False).split()

    def snapshot(folder, suffix):
        save_text(folder / f'thermal-{suffix}.txt', adb('shell', 'dumpsys', 'thermalservice'))
        save_text(folder / f'memory-{suffix}.txt', adb('shell', 'dumpsys', 'meminfo', PKG))
        save_text(folder / f'battery-{suffix}.txt', adb('shell', 'dumpsys', 'battery'))

    def telemetry(folder, end):
        # Lightweight reads only, at 5 s intervals. Detailed dumps are outside timed windows.
        script = ('echo MEMORY; cat /proc/meminfo; echo LOAD; cat /proc/loadavg; '
                  'echo CPU; for p in /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq; do '
                  'echo "$p"; cat "$p"; done; echo GPU; '
                  'for p in /sys/class/kgsl/kgsl-3d0/gpuclk /sys/class/kgsl/kgsl-3d0/gpubusy '
                  '/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq; do echo "$p"; cat "$p"; done')
        with (folder / 'telemetry.jsonl').open('a', encoding='utf-8', buffering=1) as f:
            while not end.is_set():
                try:
                    r = subprocess.run([args.adb, '-s', args.serial, 'shell', script], capture_output=True, timeout=8)
                    f.write(json.dumps(dict(host_time=time.time(), stdout=r.stdout.decode(errors='replace'),
                                            stderr=r.stderr.decode(errors='replace'), returncode=r.returncode)) + '\n')
                except Exception as e:
                    f.write(json.dumps(dict(host_time=time.time(), error=str(e))) + '\n')
                end.wait(5)

    def capture(folder, pid, log_path):
        layer = None
        last = None
        stale = 0
        floor = None
        frames, edges = set(), set()
        start = time.monotonic()
        next_alive = start
        with (folder / 'surface.jsonl').open('w', encoding='utf-8', buffering=1) as f:
            while time.monotonic() - start < args.measure:
                if stop.is_set():
                    raise KeyboardInterrupt
                if time.monotonic() >= next_alive:
                    if not app_alive(pid):
                        raise RuntimeError('App exited during measurement')
                    next_alive = time.monotonic() + 5
                if layer is None:
                    names = re.findall(r'RequestedLayerState\{([^\n]+?\(BLAST\)#\d+)',
                                       '\n'.join(x for x in adb('shell', 'dumpsys', 'SurfaceFlinger', '--list').splitlines()
                                                 if 'SurfaceView[' + PKG in x))
                    layer = max(names, key=lambda x: int(x.rsplit('#', 1)[1]), default=None)
                if layer:
                    data = shell(f'dumpsys SurfaceFlinger --latency "{layer}"')
                    f.write(json.dumps(dict(host_time=time.time(), layer=layer, data=data)) + '\n')
                    observed = sorted({int(x.split()[1]) for x in data.splitlines()[1:]
                                       if len(x.split()) == 3 and x.split()[1].isdigit()
                                       and 0 < int(x.split()[1]) < 9223372036854775807})
                    if floor is None and observed:
                        floor = observed[-1]
                    current = [x for x in observed if floor is not None and x > floor]
                    frames.update(current)
                    edges.update(zip(current, current[1:]))
                    if observed and observed[-1] != last:
                        last, stale = observed[-1], 0
                    else:
                        stale += 1
                        if stale >= 3:
                            layer, last, stale = None, None, 0
                time.sleep(.65)
        stats = village.summarize(edges, 60)
        stats.update(history_gaps=max(0, len(frames) - 1 - len(edges)),
                     first_present_ns=min(frames) if frames else None,
                     last_present_ns=max(frames) if frames else None,
                     host_duration_s=time.monotonic() - start)
        stats['valid'] = stats.get('intervals', 0) > 1 and stats['history_gaps'] == 0
        return stats

    try:
        if args.settle < 0 or args.measure < 10:
            raise RuntimeError('Use nonnegative settling and at least 10 seconds of measurement')
        source = args.snapshot.resolve() / 'save_0'
        if not (source / 'savedata3.bin').is_file():
            raise RuntimeError('Snapshot must contain save_0/savedata3.bin')
        screenshot(out / 'screen-before.png')
        policy = adb('shell', 'dumpsys', 'window', 'policy')
        save_text(out / 'window-before.txt', policy)
        if re.search(r'\bshowing=true\b', policy):
            raise RuntimeError('Device is locked; unlock before testing')
        installed = adb('shell', 'pm', 'path', PKG).strip().removeprefix('package:')
        if '\n' in installed or not installed.startswith('/data/app/'):
            raise RuntimeError('Expected one installed APK')
        adb('pull', installed, str(backup), timeout=90)
        manifest = dict(serial=args.serial, created=time.strftime('%Y-%m-%dT%H:%M:%S%z'),
                        settle_s=args.settle, measure_s=args.measure, profiles=variants,
                        memory_path='LARECOMP_VK_NO_PLACED=1 for both drivers', adpf='unchanged',
                        installed_before_sha256=digest(backup), test_apk_sha256=digest(args.apk),
                        snapshot_sha256={x.name: digest(x) for x in source.iterdir() if x.is_file()},
                        device=adb('shell', 'getprop', 'ro.product.model').strip(),
                        android=adb('shell', 'getprop', 'ro.build.version.release').strip())
        save_text(out / 'manifest.json', json.dumps(manifest, indent=2))
        old_stayon = adb('shell', 'settings', 'get', 'global', 'stay_on_while_plugged_in').strip()
        adb('shell', 'svc', 'power', 'stayon', 'usb')
        adb('shell', 'am', 'force-stop', PKG)
        changed_apk = True  # restore even if install fails halfway
        adb('install', '-r', str(args.apk.resolve()), timeout=120)
        config = out / 'initial.ini'
        config.write_bytes(b'frame_rate = auto\nresolution = 50%\nupscaler = bilinear\nantialias = off\naspect = original\nshow_fps = 1\ntouch_controls = 1\n')
        adb('push', str(config), settings)
        for driver in ['turnip', 'system']:
            folder = out / driver
            folder.mkdir()
            adb('shell', 'am', 'force-stop', PKG)
            adb('shell', 'mkdir', '-p', scratch)
            adb('push', str(source), scratch + '/', timeout=60)
            empty = out / 'empty-command'
            empty.write_bytes(b'')
            adb('push', str(empty), command)
            env = (f'LARECOMP_SETTINGS={settings},LARECOMP_SAVE_DIR={scratch},LARECOMP_INPUT={TITLE},'
                   f'LARECOMP_NO_USER_INPUT=1,LARECOMP_VK_NO_PLACED=1,LARECOMP_BENCHMARK_CONFIG={command},'
                   'LARECOMP_LOG_LINK=120,LARECOMP_TEXTURE_TRACE=1,LARECOMP_FPS_INTERVAL=2,LARECOMP_TIMING_TRACE=1')
            if driver == 'system':
                env += ',LARECOMP_VK_DRIVER='
            print(driver + ': starting Auto / isolated save / ADPF unchanged', flush=True)
            adb('shell', 'am', 'start', '-n', PKG + '/com.lasrecomp.LASRecompActivity', '--es', 'env', env)
            time.sleep(1)
            pid = adb('shell', 'pidof', PKG).strip()
            if not pid.isdigit():
                raise RuntimeError('App did not start')
            log_path = folder / 'logcat.txt'
            stream = log_path.open('wb')
            streams.append(stream)
            proc = subprocess.Popen([args.adb, '-s', args.serial, 'logcat', '-v', 'epoch', '--pid=' + pid], stdout=stream, stderr=subprocess.STDOUT)
            processes.append(proc)
            deadline = time.monotonic() + 240
            while time.monotonic() < deadline:
                text = log_path.read_text(encoding='utf-8', errors='replace')
                if not app_alive(pid):
                    save_text(folder / 'exit-info.txt', adb('shell', 'dumpsys', 'activity', 'exit-info', PKG))
                    raise RuntimeError(driver + ': exited before the village')
                if 'level Field' in text and '[link]' in text:
                    break
                time.sleep(2)
            else:
                raise RuntimeError(driver + ': village not reached')
            text = log_path.read_text(encoding='utf-8', errors='replace')
            actual = re.findall(r'\[vk\] using ([^\r\n]+)', text)
            if not actual or (('Turnip' in actual[-1]) != (driver == 'turnip')):
                raise RuntimeError('Actual driver does not match requested driver: ' + repr(actual))
            save_text(folder / 'driver.json', json.dumps(dict(pid=pid, actual=actual[-1]), indent=2))
            print(driver + ': village reached; warming 30 s', flush=True)
            time.sleep(30)
            end = threading.Event()
            worker = threading.Thread(target=telemetry, args=(folder, end), daemon=True)
            worker.start()
            try:
                for profile in variants:
                    stage = folder / f'{profile["id"]:02d}'
                    stage.mkdir()
                    local = stage / 'command.txt'
                    values = [profile['id'], 0, profile['res_mode'], profile['upscaler_mode'], profile['aa_mode'], profile['aspect_mode']]
                    local.write_bytes((' '.join(map(str, values)) + '\n').encode())
                    adb('push', str(local), command + '.tmp')
                    shell(f'mv "{command}.tmp" "{command}"')
                    deadline = time.monotonic() + 20
                    ack = None
                    while time.monotonic() < deadline:
                        text = log_path.read_text(encoding='utf-8', errors='replace')
                        ack = next((x for x in ACK.finditer(text) if int(x[1]) == profile['id']), None)
                        if ack:
                            actual_values = [int(ack[1]), int(ack[4]), int(ack[5]), int(ack[6]), int(ack[7]), int(ack[8])]
                            if actual_values != values:
                                raise RuntimeError('Settings acknowledgement differs from requested settings')
                            break
                        if not app_alive(pid):
                            raise RuntimeError('App exited while applying settings')
                        time.sleep(.3)
                    if not ack:
                        raise RuntimeError('No settings acknowledgement')
                    save_text(stage / 'profile.json', json.dumps(dict(profile, acknowledgement=ack.group(0)), indent=2))
                    time.sleep(args.settle)
                    snapshot(stage, 'before')
                    offset = log_path.stat().st_size
                    stats = capture(stage, pid, log_path)
                    with log_path.open('rb') as logs:
                        logs.seek(offset)
                        window_log = logs.read().decode(errors='replace')
                    save_text(stage / 'measurement-log.txt', window_log)
                    positions = [tuple(map(float, x)) for x in POSITION.findall(window_log)]
                    stats['positions'] = positions
                    stats['position_valid'] = bool(positions) and all(max(abs(a - b) for a, b in zip(x, (30.75, 10.5, 131.25))) < .05 for x in positions)
                    stats['valid'] = stats['valid'] and stats['position_valid']
                    stats.update(profile=profile, driver=driver)
                    save_text(stage / 'result.json', json.dumps(stats, indent=2))
                    snapshot(stage, 'after')
                    screenshot(stage / 'finish.png')
                    print(f'{driver} {profile["id"]}/{len(variants)} {profile["resolution"]} {profile["aspect"]} '
                          f'{profile["upscaler"]} AA={profile["aa"]}: {stats.get("fps", 0):.2f} FPS valid={stats["valid"]}', flush=True)
            finally:
                end.set()
                worker.join(timeout=10)
            proc.terminate()
            proc.wait(timeout=10)
            stream.close()
        save_text(out / 'complete.json', json.dumps(dict(complete=True, profiles_per_driver=len(variants))))
    except BaseException as e:
        save_text(out / 'error.txt', repr(e))
        try:
            save_text(out / 'exit-info.txt', adb('shell', 'dumpsys', 'activity', 'exit-info', PKG))
        except Exception:
            pass
        raise
    finally:
        stop.set()
        for proc in processes:
            if proc.poll() is None:
                proc.terminate()
        for stream in streams:
            if not stream.closed:
                stream.close()
        restoration = {}
        if changed_apk:
            try:
                adb('shell', 'am', 'force-stop', PKG)
                restoration['install'] = adb('install', '-r', str(backup), timeout=120)
                restoration['launcher'] = adb('shell', 'am', 'start', '-n', PKG + '/com.lasrecomp.LASRecompActivity')
            except Exception as e:
                restoration['error'] = str(e)
        if old_stayon is not None:
            try:
                if old_stayon == 'null':
                    adb('shell', 'settings', 'delete', 'global', 'stay_on_while_plugged_in')
                else:
                    adb('shell', 'settings', 'put', 'global', 'stay_on_while_plugged_in', old_stayon)
                restoration['stay_awake'] = 'restored'
            except Exception as e:
                restoration['stay_awake_error'] = str(e)
        save_text(out / 'restoration.json', json.dumps(restoration, indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['plan', 'run'])
    p.add_argument('--include-2x', action='store_true')
    p.add_argument('--adb', default=r'C:\Program Files\platform-tools\adb.exe')
    p.add_argument('--serial')
    p.add_argument('--apk', type=Path, default=ROOT / 'android/lasrecomp/app/build/outputs/apk/release/app-release.apk')
    p.add_argument('--snapshot', type=Path, default=ROOT / 'build/compat-simulation-20261021/save-before')
    p.add_argument('--out', type=Path)
    p.add_argument('--settle', type=float, default=10)
    p.add_argument('--measure', type=float, default=30)
    args = p.parse_args()
    if args.action == 'plan':
        print(json.dumps(dict(profiles_per_driver=len(profiles(args.include_2x)),
                              nominal_minutes=2 * len(profiles(args.include_2x)) * (args.settle + args.measure) / 60,
                              profiles=profiles(args.include_2x)), indent=2))
    elif not args.serial or not args.out:
        p.error('run requires --serial and --out')
    else:
        run_matrix(args)


if __name__ == '__main__':
    main()
