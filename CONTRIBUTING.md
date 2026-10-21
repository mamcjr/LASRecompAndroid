# Contributing to LAS Recomp for Android

This repository maintains the Android port of [LAS Recomp](https://github.com/peiikai/LASRecomp) by Peiikai.
Android bug reports, touch UI improvements, documentation, testing, randomizer fixes and code contributions
are welcome. Reports and discussions in **Portuguese or English** are welcome.

> [!NOTE]
> This project is developed mostly with AI (see the README). Contributions written by hand, with AI assistance or
> anything in between are equally welcome - what matters is that a change works, is tested, and follows the rules
> below.

## The one hard rule: no Nintendo files

Nothing owned by Nintendo may ever be committed or attached to an issue or pull request:

- no copies of the game in any form
- no extracted game files (`extracted/`), no generated code (`generated/`), no fonts (`.bfttf` / Switch `.ttf`)
- no game assets of any kind: textures, models, music, text, level files - also not "just one file for testing"
- no randomizer seed folders (`rando/`) - they contain modified game files; share the **seed ID** instead

Pull requests that include Nintendo files can't be merged. Screenshots and short clips of the game in bug reports are fine.

## Reporting bugs

Search the [existing issues](https://github.com/mamcjr/LASRecompAndroid/issues), then open a report in this
fork's issue tracker. The upstream project's Discord and issue tracker are not this fork's support channels.
For desktop builds and upstream development, see the [original repository](https://github.com/peiikai/LASRecomp).
Please include:

- app version, or commit and build type if you built it yourself
- phone model, Android version, SoC/GPU and Vulkan driver name/version (including whether it is the system driver)
- internal resolution, frame-rate mode (Auto / 30 / 60), antialiasing, upscaler and relevant diagnostic flags
- what you did, what happened, and what you expected
- for randomizer problems: the **seed ID** (Randomizer tab, Copy button), the settings, and where you got stuck
  (area / room, which items you had). A seed ID reproduces the exact seed on the same LAS Recomp version.
- a screenshot or short clip if it's visual
- for crashes: the relevant Android log excerpt, with personal paths, device identifiers and credentials removed

Do not upload save archives unless a maintainer requests them and you have reviewed their contents. Never
commit signing keys, keystore passwords or local SDK configuration.

## Building

Follow the Android dependency and SDK setup in the [English README](README.md#building-from-source) or
[Portuguese README](README.pt-BR.md). Scripts under `android/deps/` prepare the native dependencies and the
on-device game builder. From `android/lasrecomp/`, build a development APK:

```powershell
.\gradlew.bat :app:assembleDebug
```

On a Unix shell, use `./gradlew :app:assembleDebug`. Use your own legally obtained v1.0.0 game files for setup.
Release signing keys are not needed to contribute. Back up your saves before replacing an installed app with
a build signed by a different key.

## Project layout

- `android/lasrecomp/` - Gradle project, Android activity and APK packaging.
- `android/deps/` - native dependencies and on-device builder scripts.
- `runtime/platform/android.c` - Android integration and performance hints.
- `tools/gen.py` - the recompiler: translates the game's executables into C (`generated/`, never committed).
  `tools/patches.py` holds code patches applied during generation; every patch site checks the original instruction
  word, so a wrong address fails loudly instead of silently.
- `runtime/` - the host side:
  - `kernel.c`, `loader.c`, `services/` - the Switch OS replacement
  - `hle/` - replacements and features (door jumps, keysanity, pots / bushes, souls, warps, input, ...)
  - `nvn/` - the graphics layer: command recording (`cmdbuf.c`), Metal (`renderer.m`, `metal.m`) and Vulkan
    (`vulkan/`) backends, the shader translator (`maxwell/translate.c`)
  - `ui/` - the ImGui overlay menu and the start screen
- `tools/rando/` - randomizer tooling around LAS-Randomizer: the headless driver (`las_headless.py`), dungeon room
  shuffle (`dungeon_gen.py`, `room_logic.py`), entrances, pots / bushes / digs, shop, souls, pets.
- `third_party/LAS-Randomizer` - the vendored randomizer (MIT, with local changes marked "LAS Recomp" in comments).
- `tools/test_*.py` - focused PC checks; each script documents its requirements.
- `docs/` - Android implementation notes, measurements and feature documentation.

## Testing your change

- Build the affected Android code and run relevant focused checks. If you touch shared rendering code, identify
  the platforms/backends built and tested, and what remains unverified.
- Agree device installation, game restarts and gameplay tests with the device owner. Preserve unsaved progress,
  back up settings and use an isolated save directory (`LARECOMP_SAVE_DIR`) for automated runs.
- Validate the in-game result when device testing is available and authorized. State clearly when a change has
  only been checked on the PC; do not describe that as device validation.
- Android test hooks use `LARECOMP_*` environment variables, documented where they are defined. Keep copies of
  the settings and saves for scripted runs, and restore the original configuration afterward. Desktop helper
  scripts such as `tools/shot.sh` do not replace Android device validation.
- Randomizer changes: generate several seeds with different settings and check they complete (every location
  reachable). For dungeon room shuffle, `tools/doorjump_test.sh <seed>` walks every mismatched doorway.
- State that belongs to a seed must be reset when the player switches seeds in place: use an `RT_ON_RESET` hook
  (see `runtime/hle.h`).

## Performance changes

Preserve the validated ADPF baseline while isolating other variables. Compare the same scene, resolution,
driver and frame-rate settings, with stated warm-up time, capture duration and thermal conditions if known.
Report frametime percentiles and long frames when available; average FPS alone does not prove smoother play.
The overlay's GPU-wait percentage is not GPU utilization.

Diagnostic logs and GPU profiling can change timings. Record enabled flags and keep profiling captures separate
from normal frametime measurements. Leave unproven experiments disabled by default. See the
[Android optimization notes](docs/ANDROID-OPTIMIZATIONS.md).

## Code style

- Match the surrounding code: its naming, structure and comment density. Comments explain *why* (what the game does,
  what an address is, which seed or bug showed the problem), not what the next line does.
- Keep randomizer and game-facing text in the game's own style: pickup messages go through the game's message box,
  not custom pop-ups; settings get short labels, not paragraphs.
- New randomizer options must keep older seed IDs loading (see the seed ID notes in `runtime/ui/rando_tab.mm`).
- Keep Android-specific code behind the appropriate platform guards. Preserve settings and save compatibility,
  and keep touch controls usable on a phone.

## Pull requests

- Keep each pull request focused on one change; small is easier to review.
- Describe what changed, why, and how you tested it (platform, seeds, screenshots).
- Don't include personal paths or information in code, comments or commit messages.

Submit pull requests to [mamcjr/LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid/pulls).
Review AI-assisted changes and name the tools used when they materially contributed to the work.
An APK release is a separate maintainer action; documentation changes and experimental work do not by themselves
authorize publishing a new build.

## License

Original LAS Recomp code retains Peiikai's copyright and attribution. Android fork contributions are identified
separately in [LICENSE](LICENSE). By contributing original code, you agree to license that contribution under
the project's MIT License. Preserve existing notices and the separate licenses of third-party code, fonts and
artwork; Nintendo's game files are not covered by this project's license.
