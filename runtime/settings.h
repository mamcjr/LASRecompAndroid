// User settings (larecomp.ini next to the executable's working directory), edited from the in-game menu.
#pragma once
#include <stdbool.h>

enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12,
       RES_X125 = 13, RES_X150 = 14 /* Android: 1.25x / 1.5x (from LASRecomp-Android-Plus) */ };
// (11 was 67% of the display: larecomp.ini "67%" now loads as RES_Q75)
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */,
       UPSCALE_SGSR1 = 3 /* Android: Qualcomm Snapdragon Game Super Resolution 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { PROMPTS_AUTO = 0, PROMPTS_NINTENDO = 1, PROMPTS_XBOX = 2, PROMPTS_PLAYSTATION = 3 };
enum { CHAOS_LETTERS_OFF = 0, CHAOS_LETTERS_EXPLORED = 1, CHAOS_LETTERS_MAP = 2 /* explored, all with the Map */ };
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, N_LANGUAGES = 12 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ..., "Spanish")

typedef struct {
    int aspect;          // ASPECT_*: unlocked = fill the window (wider/taller views), original = 16:9 with bars
    bool fullscreen;
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool show_fps;
#ifdef __ANDROID__
    bool touch_controls;  // on-screen controls while no controller is connected (ui/touch_controls.mm)
    int fps_mode;         // 0: the game's own choice (30 / 60, the frame pacer), 1: 30 fps, 2: 60 fps
    bool shader_int32;    // shader address math in 32-bit words even where the driver offers 64-bit (vulkan/exec.c)
    bool frame_gen;       // 60 on the screen from the game's 30: an in-between frame by reprojection (vulkan/framegen.c)
    bool frame_gen_flow;  // ... characters' own movement too, by block matching (experimental; framegen.c build_flow)
    bool frame_gen_120;   // ... with the display at 120 Hz: the game's 60 become 120 (off: 60 Hz, its 30 become 60)
    int overlay_style;    // Show FPS's overlay (ui/perf_overlay.mm): 0 full, 1 compact (one line)
    float overlay_pos[2]; // its top left, a share of the screen, dragged there with a finger (negative: the corner)
    char mods_off[512];   // the game mods turned off in the menu (runtime/mods.c), their folder names separated by '|'
    // Driver compatibility (the start screen's Saves & files tab, ui/launcher.mm), for GPUs where a Turnip path draws
    // wrong: read when the game starts (gpu_heap) or when the app starts (turnip_lrz, log_file). -1 Automatic: on for
    // the Adreno 8xx GPUs these paths were made on, off elsewhere (vulkan/device.c vk_adreno_8xx); 1 on, 0 off.
    int gpu_heap;         // Turnip: the game's memory mapped as GPU memory (vulkan/exec.c vk_heap_place); off: CPU copies
    int turnip_lrz;       // Turnip's low-resolution Z; off: TU_DEBUG=nolrz before the driver loads (vulkan/device.c)
    bool log_file;        // the log in a file as well (platform/android.c), to send with a report
    char gpu_model[64];   // the GPU of the last start ("Adreno (TM) 830v1"): turnip_lrz's Automatic, before the driver
#define TOUCH_NCTL 12
    float touch_pos[TOUCH_NCTL][2];  // each on-screen control's place, a share of the screen (negative: default)
    float touch_scale, touch_opacity;
    bool touch_stick_fixed;  // the touch stick stays at its place (else its base comes to the thumb)
    bool touch_context;      // only the buttons the game uses now (B, R, L, X / Y once Link has their items; no ZL / ZR)
#endif
    bool dpad_move;      // the d-pad (and arrow keys) move Link like the stick
    bool move_360;       // analog movement in any direction (the game snaps the stick to 8): runtime/hle/qol.c
    bool blur_removal;   // no tilt-shift blur at the top / bottom of the screen
    bool hide_hero_icon; // no Hero Mode icon on the HUD
    bool mirror_mode;    // the world mirrored left to right (Gameplay tab): renderers + runtime/hle/input.c
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int prompts;         // PROMPTS_*: button glyphs (auto = follow the controller SDL reports): runtime/nvn/texpatch.c
    bool inv_keys;       // randomizer: show every dungeon's keys on the pause menu's Items page
    bool tracker;        // randomizer: check tracker on the pause menu's Map page
    int chaos_letters;   // CHAOS_LETTERS_*: room-shuffle doorway letters on the dungeon map
    bool aspect_asked;   // the start screen's one-time "authentic 16:9 or unlocked?" question was answered
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
    // cheats (runtime/ui/cheats.mm), applied every frame while a save is loaded
    bool cheat_hearts;   // health kept full
    bool cheat_ammo;     // bombs / arrows / magic powder kept full (for the bags Link owns)
    bool cheat_rupees;   // rupees kept at 999
    bool cheat_hookshot; // hookshot latches onto any surface (Gameplay tab; runtime/hle/hookshot.c; seeds made with it: always)
    bool no_buff_drops;  // no Guardian Acorns / Pieces of Power (Gameplay tab; runtime/hle/qol.c; seeds made with it: always)
    // difficulty (runtime/ui/difficulty.mm)
    float diff_damage;   // damage taken multiplier (1 = normal)
    bool diff_ohko;      // any hit kills
    bool diff_hero;      // Hero Mode flag forced on in the loaded save (2x damage, no recovery hearts)
    // cosmetics (runtime/ui/cosmetics.mm; applied live by runtime/nvn/texpatch.c), never part of a seed
    int tunic;           // index into g_tunic_colours (0 = Default: the game's / the seed's own)
    int hearts;          // index into g_heart_colours (0 = Default red)
    char rando_seed[128];  // active randomizer seed (rando/<name>/), empty = vanilla
} Settings;

enum { N_TUNIC_COLOURS = 13, N_HEART_COLOURS = 9 };
extern const char *const g_tunic_colours[N_TUNIC_COLOURS];
extern const char *const g_heart_colours[N_HEART_COLOURS];

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);
// Android: the phone's language is Portuguese (the menus' own text: ui/i18n.mm). LARECOMP_HOST_LANG=pt|en (tests)
bool settings_host_portuguese(void);
