#!/usr/bin/env python3
"""PC-only tests of the actual ImGui touch-scroll helper with a simulated ImGui context.
Usage: python tools/test_touch_scroll.py <Android-NDK>/toolchains/llvm/prebuilt/windows-x86_64/bin
Requires Node.js; does not use adb, Android or Vulkan.
"""
from pathlib import Path
import subprocess
import sys
root = Path(__file__).resolve().parents[1]
out = root / 'build' / 'touch-ui-tests'
out.mkdir(parents=True, exist_ok=True)
bin = Path(sys.argv[1])
src = (root / 'runtime/ui/option_rows.mm').read_text()
helper = src[src.index('void ui_touch_scroll() {'):src.index('\n#ifdef __ANDROID__\nstatic void touch_begin')]
preamble = r"""
#define __ANDROID__ 1
typedef unsigned ImGuiID;
struct ImGuiContext { int unused; };
struct ImVec2 { float x, y; };
struct ImGuiIO { int MouseSource; ImVec2 MousePos; bool MouseDown[1]; };
struct ImGuiWindow { ImGuiID ID; ImVec2 Scroll, ScrollMax; int Flags; };
enum { ImGuiMouseSource_Mouse, ImGuiMouseSource_TouchScreen };
enum { ImGuiHoveredFlags_AllowWhenBlockedByActiveItem = 1, ImGuiPopupFlags_AnyPopupId = 1, ImGuiWindowFlags_Popup = 1 };
float fabsf(float f) { return __builtin_fabsf(f); }
namespace ImGui {
static ImGuiContext contexts[2];
static ImGuiContext *current = contexts;
static ImGuiIO io;
static ImGuiWindow window = {1, {0,100}, {0,500}, 0};
static bool clicked, hovered = true, popup;
static int cleared;
static float requested = -999;
ImGuiIO &GetIO() { return io; }
ImGuiWindow *GetCurrentWindow() { return &window; }
ImGuiContext *GetCurrentContext() { return current; }
bool IsMouseClicked(int) { return clicked; }
bool IsWindowHovered(int) { return hovered; }
bool IsPopupOpen(const char *, int) { return popup; }
float GetFontSize() { return 20; }
void ClearActiveID() { cleared++; }
void SetScrollY(float y) { requested = y; }
}
"""
tests = r"""
#define CHECK(c) do { count++; if (!(c)) return count; } while (0)
extern "C" int test() {
    using namespace ImGui;
    int count = 0;
    io.MouseSource = ImGuiMouseSource_TouchScreen;
    io.MouseDown[0] = true; io.MousePos = {100,100}; clicked = true;
    ui_touch_scroll();
    CHECK(cleared == 0);
    clicked = false; io.MousePos = {100,106}; ui_touch_scroll();
    CHECK(cleared == 0);
    io.MousePos = {100,130}; ui_touch_scroll();
    CHECK(cleared == 1 && requested == 70);
    io.MouseDown[0] = false; ui_touch_scroll();
    CHECK(cleared == 2); // release is cancelled too, rather than activating a button
    ui_touch_scroll(); CHECK(cleared == 2);

    clicked = true; io.MouseDown[0] = true; io.MousePos = {100,100}; ui_touch_scroll();
    clicked = false; io.MousePos = {160,110}; ui_touch_scroll();
    CHECK(cleared == 2); // horizontal slider gesture is not a vertical swipe
    io.MouseDown[0] = false; ui_touch_scroll();

    clicked = true; io.MouseDown[0] = true; io.MousePos = {100,100}; ui_touch_scroll();
    clicked = false; window.ID = 2; io.MousePos = {100,150}; ui_touch_scroll();
    CHECK(cleared == 2); // a different child cannot steal the gesture
    window.ID = 1; current = &contexts[1]; ui_touch_scroll();
    CHECK(cleared == 2); // same window id in another context cannot steal it either
    current = contexts; io.MouseDown[0] = false; ui_touch_scroll();

    int before = cleared;
    io.MouseSource = ImGuiMouseSource_Mouse; clicked = true; io.MouseDown[0] = true;
    ui_touch_scroll(); io.MousePos = {100,200}; clicked = false; ui_touch_scroll();
    CHECK(cleared == before);
    io.MouseSource = ImGuiMouseSource_TouchScreen;
    clicked = true; io.MousePos = {100,100}; popup = true; ui_touch_scroll();
    clicked = false; io.MousePos = {100,150}; ui_touch_scroll();
    CHECK(cleared == before); // parent must not scroll underneath a popup
    window.Flags = ImGuiWindowFlags_Popup;
    clicked = true; io.MousePos = {100,100}; ui_touch_scroll();
    clicked = false; io.MousePos = {100,150}; ui_touch_scroll();
    CHECK(cleared == before + 1 && requested == 50);
    io.MouseDown[0] = false; ui_touch_scroll();
    popup = false; window.Flags = 0; window.ScrollMax.y = 0;
    before = cleared; clicked = true; io.MouseDown[0] = true; io.MousePos = {100,100}; ui_touch_scroll();
    clicked = false; io.MousePos = {100,200}; ui_touch_scroll();
    CHECK(cleared == before); // no scrolling/cancellation on a page that already fits
    return 0;
}
"""
source = out / 'touch_scroll.cpp'
source.write_text(preamble + helper + tests)
subprocess.run([str(bin / 'clang++.exe'), '-target', 'wasm32', '-ffreestanding', '-fno-exceptions', '-fno-rtti', '-O2', '-c', str(source), '-o', str(out / 'touch_scroll.o')], check=True)
subprocess.run([str(bin / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=test', str(out / 'touch_scroll.o'), '-o', str(out / 'touch_scroll.wasm')], check=True)
runner = out / 'test.cjs'
runner.write_text("const fs=require('fs'); WebAssembly.instantiate(fs.readFileSync(process.argv[2]),{}).then(({instance})=>{const n=instance.exports.test(); if(n) throw Error('Touch check '+n+' failed'); console.log('Touch scroll: 12 checks passed with simulated ImGui context');}).catch(e=>{console.error(e);process.exitCode=1;});")
subprocess.run(['node', str(runner), str(out / 'touch_scroll.wasm')], check=True)
