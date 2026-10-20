# Android touch settings

Android settings now use a category rail with Video, Audio, Controls and Saves & files (before launch). More options exposes the remaining pages, preserving their existing availability rules. Both the launcher and the in-game settings use the same category/page dispatcher. The desktop keeps its tab bar and two-pane option pages.

Android option pages use full-width controls with inline descriptions, larger dropdown entries, native checkboxes and numeric sliders. The touch-controls layout editor also uses full-width actions and sliders. A persistent Back to start / Back to game button sits below the scrolling content. The settings window uses 94% of the screen width.

Vertical touch drags scroll the category rail, option list, regular pages and dropdowns. After the movement threshold is exceeded, the active control is cancelled, including on release. Horizontal slider gestures are retained. Touch source information is preserved through the in-game event queue; the launcher already receives it through the SDL ImGui backend.

PC validation:

```text
python tools/test_touch_scroll.py <Android-NDK>/toolchains/llvm/prebuilt/windows-x86_64/bin
```

The test compiles the actual gesture helper to WebAssembly and runs it in Node.js against a simulated ImGui context. It covers 12 checks: taps, drag threshold, release cancellation, horizontal movement, child-window/context ownership, mouse input, popup blocking/scrolling and non-scrollable content. This does not validate the rendered layout or real SDL touch delivery.

The Android release build is checked on the PC. Device use, physical target sizing, visual inspection, controller navigation and interaction with the Android keyboard still need validation. No device was accessed for this change.
