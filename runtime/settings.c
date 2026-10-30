#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"
#ifdef __ANDROID__
#include <stdatomic.h>
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#endif

#define SETTINGS_FILE settings_file()

// LARECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's larecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("LARECOMP_SETTINGS");
    return e && *e ? e : "larecomp.ini";
}

Settings g_settings = {
    .aspect = ASPECT_UNLOCKED,
    .fullscreen = false,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
#ifdef __ANDROID__
    .fps_mode = 1,       // compatibility-first startup: fixed 30, unless the saved configuration chooses otherwise
    .res_mode = RES_Q50,
    .show_fps = true,  // (the performance overlay, ui/perf_overlay.mm: on until turned off in the menu)
    .touch_controls = true,
    .touch_pos = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
                  {-1, -1}, {-1, -1}},
    .touch_scale = 1.0f,
    .touch_opacity = 1.0f,
    .touch_context = true,
    .overlay_pos = {-1, -1},
    .gpu_heap = -1,
    .turnip_lrz = -1,
#else
    .show_fps = false,
#endif
    .inv_keys = true,
    .tracker = true,
    .chaos_letters = CHAOS_LETTERS_MAP,
    .diff_damage = 1.0f,
    .upscaler = UPSCALE_SHARP,  // (MetalFX on macOS, AMD FSR 1 on Windows)
};

// The game's own text languages: its RomFS carries these per region (regionUS/USen|USes|USfr, regionEU/EUen|EUes|...,
// regionJP/JPja, regionKR/KRkr, regionTW/TWch, regionCN/CNch), and the game picks one from the system language it
// reads back (runtime/services/am.c), exactly as it does on the console. Codes as the Switch reports them.
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                   "zh-CN", "zh-TW"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "Spanish", "French", "German",
                                                   "Italian", "Dutch", "Russian", "Korean", "Chinese (Simplified)",
                                                   "Chinese (Traditional)"};

int settings_host_language(void) {
#ifdef __ANDROID__
    static int lang = -1;
    if (lang < 0) {
        static const char *const codes[] = {"es", "fr", "de", "it", "nl"};
        const char *e = getenv("LARECOMP_HOST_LANG"), *code = e && *e ? e : NULL;
        SDL_Locale **l = NULL;
        if (!code) {  // (the phone's first preferred language, through SDL)
            int n = 0;
            l = SDL_GetPreferredLocales(&n);
            code = l && n > 0 ? l[0]->language : NULL;
        }
        lang = 0;
        for (int i = 0; i < 5 && code; i++)
            if (!strncmp(code, codes[i], 2)) lang = i + 1;
        SDL_free(l);
    }
    return lang;
#else
    return 0;
#endif
}

bool settings_host_portuguese(void) {
#ifdef __ANDROID__
    static int pt = -1;
    if (pt < 0) {
        const char *e = getenv("LARECOMP_HOST_LANG");
        if (e && *e) pt = !strncmp(e, "pt", 2);
        else {  // (the phone's first preferred language, through SDL)
            int n = 0;
            SDL_Locale **l = SDL_GetPreferredLocales(&n);
            pt = l && n > 0 && l[0]->language && !strcmp(l[0]->language, "pt");
            SDL_free(l);
        }
    }
    return pt;
#else
    return false;
#endif
}

// LANG_AUTO: the host's own language (LANG=es_ES.UTF-8 -> es), en-US when it is none of the game's. Linux only:
// macOS / Windows apps get no locale variables from the system, so Auto is English there (pick one in settings).
static const char *host_language(void) {
#ifndef __linux__
    return "en-US";
#endif
    const char *e = getenv("LC_ALL");
    if (!e || !*e) e = getenv("LC_MESSAGES");
    if (!e || !*e) e = getenv("LANG");
    if (!e || !*e || !strcmp(e, "C") || !strcmp(e, "POSIX")) return "en-US";
    char l[8] = {0};
    size_t n = 0;
    for (const char *p = e; *p && *p != '.' && *p != '@' && *p != '_' && n < sizeof l - 1; p++) l[n++] = *p;
    static const struct { const char *loc, *code; } map[] = {{"ja", "ja"}, {"ko", "ko"}, {"ru", "ru"}, {"nl", "nl"},
                                                             {"de", "de"}, {"it", "it"}, {"fr", "fr"}, {"es", "es"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(l, map[i].loc)) return map[i].code;
    if (!strcmp(l, "zh")) return e[n] == '_' && e[n + 1] == 'T' ? "zh-TW" : "zh-CN";  // (TW / HK: traditional)
    return "en-US";
}

const char *settings_language(void) {
    int i = g_settings.language;
    return i > LANG_AUTO && i < N_LANGUAGES ? g_language_codes[i] : host_language();
}

const char *settings_language_name(void) {
    const char *c = settings_language();
    for (int i = 0; i < N_LANGUAGES; i++)
        if (!strcmp(c, g_language_codes[i])) return g_language_names[i];
    return "English";
}

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
#ifdef __ANDROID__
    char line[640];  // (touch_layout: 24 numbers)
#else
    char line[256];
#endif
    bool tip_key = false, aspect_key = false;
    while (fgets(line, sizeof line, f)) {
        char key[64];
#ifdef __ANDROID__
        char val[512] = "";  // (an empty value - mods_off = - leaves it untouched)
        if (sscanf(line, " %63[^= ] = %511[^\n]", key, val) < 1) continue;
#else
        char val[128];
        if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) < 1) continue;
#endif
        if (!strcmp(key, "rando_seed")) {
#ifndef __ANDROID__  // (Android has no randomizer: a seed saved by an earlier version stays unused)
            if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) != 2) val[0] = 0;
            strlcpy(g_settings.rando_seed, val, sizeof g_settings.rando_seed);
#endif
            continue;
        }
        if (!strcmp(key, "aspect")) g_settings.aspect = !strcmp(val, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;
        else if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
#ifdef __ANDROID__
        else if (!strcmp(key, "touch_controls")) g_settings.touch_controls = atoi(val) != 0;
        else if (!strcmp(key, "frame_rate")) g_settings.fps_mode = !strcmp(val, "30") ? 1 : !strcmp(val, "60") ? 2 : 0;
        else if (!strcmp(key, "shader_int32")) g_settings.shader_int32 = atoi(val) != 0;
        else if (!strcmp(key, "frame_gen")) g_settings.frame_gen = atoi(val) != 0;
        else if (!strcmp(key, "frame_gen_flow")) g_settings.frame_gen_flow = atoi(val) != 0;
        else if (!strcmp(key, "frame_gen_120")) g_settings.frame_gen_120 = atoi(val) != 0;
        else if (!strcmp(key, "native_code")) g_settings.native_code = atoi(val) != 0;
        else if (!strcmp(key, "files_in_place")) g_settings.files_in_place = atoi(val) != 0;
        else if (!strcmp(key, "overlay")) g_settings.overlay_style = !strcmp(val, "compact");
        else if (!strcmp(key, "overlay_pos")) sscanf(val, "%f,%f", &g_settings.overlay_pos[0], &g_settings.overlay_pos[1]);
        else if (!strcmp(key, "mods_off")) snprintf(g_settings.mods_off, sizeof g_settings.mods_off, "%s", val);
        else if (!strcmp(key, "gpu_heap")) g_settings.gpu_heap = !strcmp(val, "auto") ? -1 : atoi(val) != 0;
        else if (!strcmp(key, "turnip_lrz")) g_settings.turnip_lrz = !strcmp(val, "auto") ? -1 : atoi(val) != 0;
        else if (!strcmp(key, "log_file")) g_settings.log_file = atoi(val) != 0;
        else if (!strcmp(key, "gpu_model")) snprintf(g_settings.gpu_model, sizeof g_settings.gpu_model, "%s", val);
        else if (!strcmp(key, "touch_scale")) g_settings.touch_scale = (float)atof(val);
        else if (!strcmp(key, "touch_opacity")) g_settings.touch_opacity = (float)atof(val);
        else if (!strcmp(key, "touch_stick_fixed")) g_settings.touch_stick_fixed = atoi(val) != 0;
        else if (!strcmp(key, "touch_context")) g_settings.touch_context = atoi(val) != 0;
        else if (!strcmp(key, "touch_icons")) g_settings.touch_icons = atoi(val) != 0;
        else if (!strcmp(key, "black_bars")) g_settings.black_bars = atoi(val) != 0;
        else if (!strcmp(key, "touch_layout")) {  // x,y per control (a share of the screen; negative: default)
            char *q = val;
            for (int i = 0; i < TOUCH_NCTL * 2 && *q; i++) {
                g_settings.touch_pos[i / 2][i % 2] = strtof(q, &q);
                while (*q == ',' || *q == ' ') q++;
            }
        }
#endif
        else if (!strcmp(key, "resolution")) {
            int n = atoi(val);
#ifdef __ANDROID__
            if (!strcmp(val, "1.25x") || !strcmp(val, "1.5x")) { g_settings.res_mode = val[2] == '2' ? RES_X125 : RES_X150; continue; }
#endif
            g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                                  !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                                  (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;  // (5x-8x were removed)
        }
#ifdef __ANDROID__
        else if (!strcmp(key, "upscaler") && !strcmp(val, "sgsr1")) g_settings.upscaler = UPSCALE_SGSR1;
#endif
        else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;  // (metalfx / fsr1: this OS's)
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : AA_SMAA;  // ("fxaa": SMAA now)
        else if (!strcmp(key, "inv_keys")) g_settings.inv_keys = atoi(val) != 0;
        else if (!strcmp(key, "dpad_move")) g_settings.dpad_move = atoi(val) != 0;
        else if (!strcmp(key, "move_360")) g_settings.move_360 = atoi(val) != 0;
        else if (!strcmp(key, "blur_removal")) g_settings.blur_removal = atoi(val) != 0;
        else if (!strcmp(key, "hide_hero_icon")) g_settings.hide_hero_icon = atoi(val) != 0;
        else if (!strcmp(key, "mirror_mode")) g_settings.mirror_mode = atoi(val) != 0;
        else if (!strcmp(key, "prompts")) g_settings.prompts = !strcmp(val, "nintendo") ? PROMPTS_NINTENDO : !strcmp(val, "xbox") ?
                                                              PROMPTS_XBOX : !strcmp(val, "playstation") ? PROMPTS_PLAYSTATION : PROMPTS_AUTO;
        else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
            g_settings.language = LANG_AUTO;
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(val, g_language_codes[i])) g_settings.language = i;
        }
        else if (!strcmp(key, "tracker")) g_settings.tracker = atoi(val) != 0;
        else if (!strcmp(key, "menu_tip_seen")) g_settings.menu_tip_seen = atoi(val) != 0, tip_key = true;
        else if (!strcmp(key, "aspect_asked")) g_settings.aspect_asked = atoi(val) != 0, aspect_key = true;
        else if (!strcmp(key, "chaos_letters"))
            g_settings.chaos_letters = !strcmp(val, "off") ? CHAOS_LETTERS_OFF : !strcmp(val, "explored") ? CHAOS_LETTERS_EXPLORED : CHAOS_LETTERS_MAP;
        else if (!strcmp(key, "cheat_hearts")) g_settings.cheat_hearts = atoi(val) != 0;
        else if (!strcmp(key, "cheat_ammo")) g_settings.cheat_ammo = atoi(val) != 0;
        else if (!strcmp(key, "cheat_rupees")) g_settings.cheat_rupees = atoi(val) != 0;
        else if (!strcmp(key, "cheat_hookshot")) g_settings.cheat_hookshot = atoi(val) != 0;
        else if (!strcmp(key, "no_buff_drops")) g_settings.no_buff_drops = atoi(val) != 0;
        else if (!strcmp(key, "diff_damage")) g_settings.diff_damage = (float)atof(val);
        else if (!strcmp(key, "diff_ohko")) g_settings.diff_ohko = atoi(val) != 0;
        else if (!strcmp(key, "diff_hero")) g_settings.diff_hero = atoi(val) != 0;
        else if (!strcmp(key, "tunic")) { for (int i = 0; i < N_TUNIC_COLOURS; i++) if (!strcmp(val, g_tunic_colours[i])) g_settings.tunic = i; }
        else if (!strcmp(key, "hearts")) { for (int i = 0; i < N_HEART_COLOURS; i++) if (!strcmp(val, g_heart_colours[i])) g_settings.hearts = i; }
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
    }
    fclose(f);
    if (!tip_key) g_settings.menu_tip_seen = true;  // (a settings file from before the note: not a first launch)
    if (!aspect_key) g_settings.aspect_asked = true;  // (likewise: the aspect ratio was chosen already)
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
}

void settings_save(void) {
#ifdef __ANDROID__
    // Written to larecomp.ini.tmp, then put in its place: a phone ends apps in the background at any moment (the app
    // saves then too: vulkan/device.c), and a file cut short there lost every setting. One writer at a time. (From
    // LASRecomp-Android-Plus, MIT.)
    static atomic_flag lock = ATOMIC_FLAG_INIT;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", SETTINGS_FILE);
    while (atomic_flag_test_and_set(&lock)) {}
    FILE *f = fopen(tmp, "w");
    if (!f) { LOG("[settings] cannot write %s\n", tmp); atomic_flag_clear(&lock); return; }
#else
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
#endif
    fprintf(f, "# LAS Recomp settings (edited from the in-game menu: F1, Esc, or Minus+Plus on a controller)\n");
    fprintf(f, "aspect = %s\n", g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
#ifdef __ANDROID__
    fprintf(f, "touch_controls = %d\n", g_settings.touch_controls);
    fprintf(f, "frame_rate = %s\n", g_settings.fps_mode == 1 ? "30" : g_settings.fps_mode == 2 ? "60" : "auto");
    fprintf(f, "shader_int32 = %d\n", g_settings.shader_int32);
    fprintf(f, "frame_gen = %d\n", g_settings.frame_gen);
    fprintf(f, "frame_gen_flow = %d\n", g_settings.frame_gen_flow);
    fprintf(f, "frame_gen_120 = %d\n", g_settings.frame_gen_120);
    fprintf(f, "native_code = %d\n", g_settings.native_code);
    fprintf(f, "files_in_place = %d\n", g_settings.files_in_place);
    fprintf(f, "overlay = %s\n", g_settings.overlay_style == 1 ? "compact" : "full");
    fprintf(f, "overlay_pos = %.4f,%.4f\n", g_settings.overlay_pos[0], g_settings.overlay_pos[1]);
    fprintf(f, "mods_off = %s\n", g_settings.mods_off);
    static const char *const tri[] = {"auto", "0", "1"};  // (-1 / 0 / 1)
    fprintf(f, "gpu_heap = %s\nturnip_lrz = %s\nlog_file = %d\ngpu_model = %s\n", tri[g_settings.gpu_heap < 0 ? 0 : g_settings.gpu_heap ? 2 : 1],
            tri[g_settings.turnip_lrz < 0 ? 0 : g_settings.turnip_lrz ? 2 : 1], g_settings.log_file, g_settings.gpu_model);
    fprintf(f, "touch_stick_fixed = %d\ntouch_context = %d\ntouch_icons = %d\nblack_bars = %d\n", g_settings.touch_stick_fixed,
            g_settings.touch_context, g_settings.touch_icons, g_settings.black_bars);
    fprintf(f, "touch_scale = %.2f\ntouch_opacity = %.2f\ntouch_layout = ", g_settings.touch_scale, g_settings.touch_opacity);
    for (int i = 0; i < TOUCH_NCTL; i++)
        fprintf(f, "%s%.4f,%.4f", i ? "," : "", g_settings.touch_pos[i][0], g_settings.touch_pos[i][1]);
    fprintf(f, "\n");
#endif
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_METALFX ? "metalfx" : g_settings.upscaler == UPSCALE_FSR1 ? "fsr1"
                                  : g_settings.upscaler == UPSCALE_SGSR1 ? "sgsr1" : "bilinear");
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : "off");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode == RES_X125) fprintf(f, "resolution = 1.25x\n");
    else if (g_settings.res_mode == RES_X150) fprintf(f, "resolution = 1.5x\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "inv_keys = %d\n", g_settings.inv_keys);
    fprintf(f, "dpad_move = %d\n", g_settings.dpad_move);
    fprintf(f, "move_360 = %d\nblur_removal = %d\nhide_hero_icon = %d\n", g_settings.move_360, g_settings.blur_removal,
            g_settings.hide_hero_icon);
    static const char *const prompts[] = {"auto", "nintendo", "xbox", "playstation"};
    fprintf(f, "prompts = %s\n", prompts[g_settings.prompts & 3]);
    fprintf(f, "mirror_mode = %d\n", g_settings.mirror_mode);
    fprintf(f, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    fprintf(f, "tracker = %d\n", g_settings.tracker);
    fprintf(f, "aspect_asked = %d\n", g_settings.aspect_asked);
    fprintf(f, "menu_tip_seen = %d\n", g_settings.menu_tip_seen);
    static const char *const letters[] = {"off", "explored", "map"};
    fprintf(f, "chaos_letters = %s\n", letters[g_settings.chaos_letters]);
    fprintf(f, "cheat_hearts = %d\ncheat_ammo = %d\ncheat_rupees = %d\n", g_settings.cheat_hearts, g_settings.cheat_ammo,
            g_settings.cheat_rupees);
    fprintf(f, "cheat_hookshot = %d\n", g_settings.cheat_hookshot);
    fprintf(f, "no_buff_drops = %d\n", g_settings.no_buff_drops);
    fprintf(f, "diff_damage = %.2f\ndiff_ohko = %d\ndiff_hero = %d\n", g_settings.diff_damage, g_settings.diff_ohko,
            g_settings.diff_hero);
    fprintf(f, "tunic = %s\nhearts = %s\n", g_tunic_colours[g_settings.tunic], g_heart_colours[g_settings.hearts]);
    bind_settings_save(f);
    fprintf(f, "rando_seed = %s\n", g_settings.rando_seed);
#ifdef __ANDROID__
    if (fclose(f) != 0 || rename(tmp, SETTINGS_FILE) != 0) LOG("[settings] cannot write %s\n", SETTINGS_FILE);
    atomic_flag_clear(&lock);
#else
    fclose(f);
#endif
}
