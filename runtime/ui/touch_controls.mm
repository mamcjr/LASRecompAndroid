// On-screen controls on Android (docs/ANDROID-PORT.md, Fase 4): a left stick, A / B / X / Y, L / R / ZL / ZR,
// Minus / Plus and a button for the in-game menu, drawn over the game with ImGui (ui/menu.mm menu_render) while no
// controller is connected. Fingers come from SDL's touch events (ui_run, runtime/nvn/vulkan/device.c); what they hold
// joins the controller's buttons and stick in runtime/hle/input.c (g_touch_buttons, g_touch_lx / ly). The stick is
// floating: it starts where a finger lands in the left part of the screen. With the menu or the name entry up,
// touches stay mouse clicks for the menu. The Android back button opens the menu too.
// The layout is the player's (the Controls tab, ui/controls_ui.mm): each control's place (g_settings.touch_pos, a
// share of the screen; negative: the default), their size and opacity. Editing it (touch_edit_begin) closes the menu
// and lets the controls be dragged; a bar on the screen saves, resets or cancels.
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "imgui.h"

extern "C" {
#include "settings.h"
bool menu_input_captured(void);  // ui/menu.mm
void menu_test_open(void);       // ui/menu.mm: opens the in-game menu
void menu_request_close(void);   // ui/menu.mm
_Atomic uint64_t g_touch_buttons;  // (runtime/hle/input.c) Switch button bits held on the screen
_Atomic int32_t g_touch_lx, g_touch_ly;  // left stick, -32767..32767, y up
}

// the Switch button bits (runtime/hle/input.c)
enum { TB_A = 0, TB_B, TB_X, TB_Y, TB_LSTICK, TB_RSTICK, TB_L, TB_R, TB_ZL, TB_ZR, TB_PLUS, TB_MINUS };
enum { C_NONE = -1, C_STICK = 100, C_MENU = 101, C_DONE = 102, C_RESET = 103, C_CANCEL = 104 };
// the controls, in g_settings.touch_pos order (TOUCH_NCTL entries)
enum { K_A, K_B, K_X, K_Y, K_L, K_ZL, K_R, K_ZR, K_MINUS, K_PLUS, K_MENU, K_STICK, K_N };
_Static_assert(K_N == TOUCH_NCTL, "settings.h TOUCH_NCTL");

typedef struct { float x, y, r; int bit; const char *label; bool rect; float w, h; int k; } Ctl;

static pthread_mutex_t t_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { SDL_FingerID id; int ctl; float x, y, dx, dy; } fingers[10];
static int nfingers;
static float stick_cx, stick_cy, stick_kx, stick_ky;  // where the stick started and where its knob is (pixels)
static bool stick_on;
static float disp_w = 1, disp_h = 1;  // the window in pixels (as last drawn)
static atomic_bool used;  // a touch since the last controller: show the controls
static atomic_bool hide_auto;  // a controller is connected (as of the last event or check)
static atomic_bool editing;
static float edit_pos[K_N][2];  // (editing) the layout being edited
static _Atomic uint64_t last_draw_ms;

static void default_pos(int k, float *x, float *y) {  // in pixels
    float w = disp_w, h = disp_h, u = h / 100.0f, gap = 13.0f * u, fx = w - 20 * u, fy = h - 27 * u, sw = 15 * u;
    switch (k) {
    case K_A: *x = fx + gap, *y = fy; break;  // face buttons: a diamond, Switch layout (A east)
    case K_B: *x = fx, *y = fy + gap; break;
    case K_X: *x = fx, *y = fy - gap; break;
    case K_Y: *x = fx - gap, *y = fy; break;
    case K_L: *x = 4 * u + sw / 2, *y = 20 * u; break;  // shoulders: L / ZL top left, R / ZR top right
    case K_ZL: *x = 4 * u + sw / 2, *y = 10 * u; break;
    case K_R: *x = w - 4 * u - sw / 2, *y = 20 * u; break;
    case K_ZR: *x = w - 4 * u - sw / 2, *y = 10 * u; break;
    case K_MINUS: *x = w / 2 - 9 * u, *y = h - 7 * u; break;
    case K_PLUS: *x = w / 2 + 9 * u, *y = h - 7 * u; break;
    case K_MENU: *x = w / 2, *y = 6 * u; break;
    default: *x = 20 * u, *y = h - 27 * u; break;  // the stick's resting place
    }
}

static void pos_of(int k, float *x, float *y) {
    const float *p = editing ? edit_pos[k] : g_settings.touch_pos[k];
    if (p[0] >= 0 && p[1] >= 0) *x = p[0] * disp_w, *y = p[1] * disp_h;
    else default_pos(k, x, y);
}

static float size_scale(void) { return g_settings.touch_scale > 0.2f ? g_settings.touch_scale : 1.0f; }
static float stick_radius(void) { return disp_h * 0.13f * size_scale(); }

static int layout(Ctl *c) {  // in pixels of the window (landscape)
    float u = disp_h / 100.0f, z = size_scale();
    float br = 7.5f * u * z, sw = 15 * u * z, sh = 7 * u * z, sr = 4.5f * u * z;
    static const struct { int k, bit; const char *label; int shape; } T[] = {
        {K_A, TB_A, "A", 0}, {K_B, TB_B, "B", 0}, {K_X, TB_X, "X", 0}, {K_Y, TB_Y, "Y", 0},
        {K_L, TB_L, "L", 1}, {K_ZL, TB_ZL, "ZL", 1}, {K_R, TB_R, "R", 1}, {K_ZR, TB_ZR, "ZR", 1},
        {K_MINUS, TB_MINUS, "-", 2}, {K_PLUS, TB_PLUS, "+", 2}, {K_MENU, C_MENU, "=", 2}};
    int n = 0;
    for (const auto &t : T) {
        float x, y;
        pos_of(t.k, &x, &y);
        c[n++] = t.shape == 1 ? (Ctl){x, y, 0, t.bit, t.label, true, sw, sh, t.k}
                              : (Ctl){x, y, t.shape == 2 ? sr : br, t.bit, t.label, false, 0, 0, t.k};
    }
    if (editing) {  // the bar: save, defaults, cancel
        float bw = 16 * u, bh = 8 * u, y = 30 * u;
        c[n++] = (Ctl){disp_w / 2 - 18 * u, y, 0, C_DONE, "Done", true, bw, bh, -1};
        c[n++] = (Ctl){disp_w / 2, y, 0, C_RESET, "Default", true, bw, bh, -1};
        c[n++] = (Ctl){disp_w / 2 + 18 * u, y, 0, C_CANCEL, "Cancel", true, bw, bh, -1};
    }
    return n;
}

static bool in_ctl(const Ctl *c, float x, float y) {
    float u = disp_h / 100.0f;
    if (c->rect) return fabsf(x - c->x) <= c->w / 2 + u && fabsf(y - c->y) <= c->h / 2 + u;
    float dx = x - c->x, dy = y - c->y, r = c->r * 1.25f;  // (a bit larger than drawn: thumbs)
    return dx * dx + dy * dy <= r * r;
}

static int hit(float x, float y, int *k) {  // the control under (x, y) (*k: its layout entry)
    Ctl c[20];
    int n = layout(c);
    for (int i = n; i-- > 0;)  // (the bar first)
        if (in_ctl(&c[i], x, y)) { *k = c[i].k; return c[i].bit; }
    *k = K_STICK;
    if (editing) {
        float sx, sy, R = stick_radius();
        pos_of(K_STICK, &sx, &sy);
        return (x - sx) * (x - sx) + (y - sy) * (y - sy) <= R * R ? C_STICK : C_NONE;
    }
    if (x < disp_w * 0.45f && y > disp_h * 0.30f) return C_STICK;
    return C_NONE;
}

static void stick_update(void) {  // (t_lock held)
    float R = stick_radius();
    float dx = stick_kx - stick_cx, dy = stick_ky - stick_cy, d = sqrtf(dx * dx + dy * dy);
    if (d > R) { dx *= R / d, dy *= R / d; stick_kx = stick_cx + dx, stick_ky = stick_cy + dy; }
    g_touch_lx = (int32_t)(dx / R * 32767);
    g_touch_ly = (int32_t)(-dy / R * 32767);
}

static void recompute(void) {  // (t_lock held)
    uint64_t b = 0;
    for (int i = 0; i < nfingers && !editing; i++)
        if (fingers[i].ctl >= 0 && fingers[i].ctl < 16) b |= 1ull << fingers[i].ctl;
    g_touch_buttons = b;
}

static void release_all(void) {
    pthread_mutex_lock(&t_lock);
    nfingers = 0;
    stick_on = false;
    g_touch_buttons = 0, g_touch_lx = 0, g_touch_ly = 0;
    pthread_mutex_unlock(&t_lock);
}

// ---- layout editing (the Controls tab)
extern "C" bool touch_edit_available(void) {  // the overlay is up (in game): there is something to drag
    return SDL_GetTicks() - last_draw_ms < 500 && g_settings.touch_controls;
}

extern "C" void touch_edit_begin(void) {
    memcpy(edit_pos, g_settings.touch_pos, sizeof edit_pos);
    release_all();
    editing = true;
    used = true;
    menu_request_close();
}

static void edit_end(int how) {  // C_DONE / C_RESET / C_CANCEL
    if (how == C_RESET) {
        for (int k = 0; k < K_N; k++) edit_pos[k][0] = edit_pos[k][1] = -1;
        return;  // (still editing: the defaults on the screen)
    }
    if (how == C_DONE) {
        memcpy(g_settings.touch_pos, edit_pos, sizeof edit_pos);
        settings_save();
    }
    release_all();
    editing = false;
}

extern "C" void touch_layout_reset(void) {
    for (int k = 0; k < K_N; k++) g_settings.touch_pos[k][0] = g_settings.touch_pos[k][1] = -1;
    settings_save();
}

extern "C" bool touch_controls_visible(void) {
    // a controller connected before the game started (its ADDED event went to the start screen): checked here too,
    // a few times a second
    static _Atomic uint64_t checked_ms;
    uint64_t now = SDL_GetTicks();
    if (now - checked_ms > 250) {
        checked_ms = now;
        bool pad = SDL_HasGamepad();
        if (pad && !hide_auto) release_all();
        hide_auto = pad;
    }
    return editing || (!hide_auto && !menu_input_captured() && g_settings.touch_controls);
}

// SDL events (the UI thread): true when the event was for the on-screen controls.
extern "C" bool touch_controls_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: hide_auto = true; release_all(); return false;
    case SDL_EVENT_GAMEPAD_REMOVED: hide_auto = SDL_HasGamepad(); return false;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_AC_BACK && !e->key.repeat) {
            if (editing) { edit_end(C_CANCEL); return true; }
            if (!menu_input_captured()) { menu_test_open(); return true; }
        }
        return false;
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: break;
    default: return false;
    }
    if (!editing && (menu_input_captured() || !g_settings.touch_controls || hide_auto)) { release_all(); return false; }  // (the menu's clicks)
    used = true;
    float x = e->tfinger.x * disp_w, y = e->tfinger.y * disp_h;
    pthread_mutex_lock(&t_lock);
    int f = -1;
    for (int i = 0; i < nfingers; i++) if (fingers[i].id == e->tfinger.fingerID) f = i;
    int act = C_NONE;  // (after the lock: menu / the edit bar)
    if (e->type == SDL_EVENT_FINGER_DOWN && f < 0 && nfingers < 10) {
        int k, ctl = hit(x, y, &k);
        if (ctl == C_STICK && stick_on && !editing) ctl = C_NONE;  // (one stick)
        float cx, cy;
        if (k >= 0) pos_of(k, &cx, &cy); else cx = x, cy = y;
        fingers[nfingers] = {e->tfinger.fingerID, ctl, x, y, cx - x, cy - y};
        if (editing && k >= 0 && ctl != C_NONE && ctl < C_DONE) fingers[nfingers].ctl = 200 + k;  // (dragging layout entry k)
        nfingers++;
        if (!editing && ctl == C_STICK) { stick_on = true; stick_cx = stick_kx = x, stick_cy = stick_ky = y; stick_update(); }
        if (ctl == C_MENU && !editing) act = C_MENU;
        if (ctl >= C_DONE && ctl <= C_CANCEL) act = ctl;
    } else if (e->type == SDL_EVENT_FINGER_MOTION && f >= 0) {
        fingers[f].x = x, fingers[f].y = y;
        if (fingers[f].ctl >= 200) {  // (editing) the control follows the finger, kept on the screen
            int k = fingers[f].ctl - 200;
            float nx = (x + fingers[f].dx) / disp_w, ny = (y + fingers[f].dy) / disp_h;
            edit_pos[k][0] = nx < 0.02f ? 0.02f : nx > 0.98f ? 0.98f : nx;
            edit_pos[k][1] = ny < 0.03f ? 0.03f : ny > 0.97f ? 0.97f : ny;
        } else if (fingers[f].ctl == C_STICK && !editing) { stick_kx = x, stick_ky = y; stick_update(); }
    } else if (e->type == SDL_EVENT_FINGER_UP && f >= 0) {
        if (fingers[f].ctl == C_STICK && !editing) { stick_on = false; g_touch_lx = 0, g_touch_ly = 0; }
        fingers[f] = fingers[--nfingers];
    }
    recompute();
    pthread_mutex_unlock(&t_lock);
    if (act == C_MENU) { release_all(); menu_test_open(); }
    else if (act >= C_DONE) edit_end(act);
    return true;
}

// Drawn with the in-game overlay (ui/menu.mm menu_render), between ImGui's NewFrame and Render.
extern "C" void touch_controls_draw(ImVec2 display, ImFont *font) {
    disp_w = display.x, disp_h = display.y;
    last_draw_ms = SDL_GetTicks();
    if (!touch_controls_visible()) return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    Ctl c[20];
    int n = layout(c);
    uint64_t held = g_touch_buttons;
    float u = disp_h / 100.0f;
    float op = g_settings.touch_opacity > 0.05f ? g_settings.touch_opacity : 1.0f;
    float alpha = (used || editing ? 1.0f : 0.6f) * op;
    // Island inventory palette, kept translucent over the game. Every layer follows
    // the player's opacity; pressed controls use green as well as a brighter rim.
    ImU32 fill = IM_COL32(244, 235, 207, (int)(60 * alpha));
    ImU32 fill_on = IM_COL32(55, 104, 65, (int)(210 * op));
    ImU32 line = IM_COL32(126, 96, 57, (int)(205 * alpha));
    ImU32 trim = IM_COL32(238, 216, 164, (int)(180 * alpha));
    ImU32 text = IM_COL32(255, 247, 219, (int)(235 * alpha));
    ImU32 shadow = IM_COL32(31, 47, 32, (int)(220 * alpha));
    if (editing) {
        dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 90));
        const char *tip = "Drag the controls to move them";
        float fs = 4.5f * u;
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, tip);
        ImVec2 tp(disp_w / 2 - ts.x / 2, 20 * u);
        dl->AddText(font, fs, ImVec2(tp.x + 0.2f * u, tp.y + 0.2f * u), IM_COL32(31, 47, 32, 230), tip);
        dl->AddText(font, fs, tp, IM_COL32(255, 247, 219, 255), tip);
    }
    for (int i = 0; i < n; i++) {
        bool bar = c[i].bit >= C_DONE && c[i].bit <= C_CANCEL;
        bool on = c[i].bit >= 0 && c[i].bit < 16 && (held >> c[i].bit & 1);
        float fs = (c[i].rect ? c[i].h * (bar ? 0.45f : 0.6f) : c[i].r * 0.9f);
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, c[i].label);
        ImU32 f = bar ? IM_COL32(55, 86, 53, 240) : on ? fill_on : fill;
        ImU32 l = bar ? IM_COL32(190, 162, 107, 255) : line;
        ImU32 rim = bar ? IM_COL32(238, 216, 164, 255) : on ? IM_COL32(255, 237, 187, (int)(240 * op)) : trim;
        ImU32 tx = bar ? IM_COL32(255, 247, 219, 255) : text;
        if (c[i].rect) {
            ImVec2 a(c[i].x - c[i].w / 2, c[i].y - c[i].h / 2), b(c[i].x + c[i].w / 2, c[i].y + c[i].h / 2);
            dl->AddRectFilled(a, b, f, 1.5f * u);
            dl->AddRect(a, b, l, 1.5f * u, 0, 0.3f * u);
            float inset = 0.55f * u;
            dl->AddRect(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), rim, 1.1f * u, 0, 0.15f * u);
        } else {
            dl->AddCircleFilled(ImVec2(c[i].x, c[i].y), c[i].r, f, 32);
            dl->AddCircle(ImVec2(c[i].x, c[i].y), c[i].r, l, 32, 0.3f * u);
            dl->AddCircle(ImVec2(c[i].x, c[i].y), c[i].r - 0.55f * u, rim, 32, 0.15f * u);
        }
        if (c[i].bit == C_MENU) {  // three bars
            for (int k = -1; k <= 1; k++) {
                dl->AddLine(ImVec2(c[i].x - 2 * u, c[i].y + k * 1.3f * u + 0.2f * u),
                            ImVec2(c[i].x + 2 * u, c[i].y + k * 1.3f * u + 0.2f * u), shadow, 0.65f * u);
                dl->AddLine(ImVec2(c[i].x - 2 * u, c[i].y + k * 1.3f * u), ImVec2(c[i].x + 2 * u, c[i].y + k * 1.3f * u), tx, 0.4f * u);
            }
        } else {
            dl->AddText(font, fs, ImVec2(c[i].x - ts.x / 2 + 0.2f * u, c[i].y - ts.y / 2 + 0.2f * u), shadow, c[i].label);
            dl->AddText(font, fs, ImVec2(c[i].x - ts.x / 2, c[i].y - ts.y / 2), tx, c[i].label);
        }
    }
    // the stick: where it started (or its resting place) and the knob
    pthread_mutex_lock(&t_lock);
    bool on = stick_on;
    float rx, ry;
    pos_of(K_STICK, &rx, &ry);
    float cx = on ? stick_cx : rx, cy = on ? stick_cy : ry, kx = on ? stick_kx : cx, ky = on ? stick_ky : cy;
    pthread_mutex_unlock(&t_lock);
    float R = stick_radius();
    dl->AddCircleFilled(ImVec2(cx, cy), R, fill, 48);
    dl->AddCircle(ImVec2(cx, cy), R, line, 48, 0.3f * u);
    dl->AddCircle(ImVec2(cx, cy), R - 0.55f * u, trim, 48, 0.15f * u);
    float knob = R * 0.45f;
    dl->AddCircleFilled(ImVec2(kx, ky), knob, on ? fill_on : IM_COL32(244, 235, 207, (int)(95 * alpha)), 32);
    dl->AddCircle(ImVec2(kx, ky), knob, line, 32, 0.3f * u);
    dl->AddCircle(ImVec2(kx, ky), knob - 0.55f * u,
                  on ? IM_COL32(255, 237, 187, (int)(240 * op)) : trim, 32, 0.15f * u);
}
#endif
