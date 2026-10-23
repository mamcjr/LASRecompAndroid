// Shader preparation (Android, from LASRecomp beta_5): the game's shader variants are translated, compiled and built
// into pipelines before the game needs them, so a first visit anywhere has no compile hitch or pop-in (a cold run into
// Mabe Village built ~150 programs and pipelines in its first 6 s there, ~16 700 draws skipped meanwhile). The
// approach follows SuperMarioWonderRecomp's ahead-of-time shaders (github.com/BelmanteGu/SuperMarioWonderRecomp).
//
// What ships is only a list of variant records (runtime/nvn/shader_variants.txt, recorded by upstream on the desktop -
// they are in NVN terms, the same on a phone): pipeline / compute state (formats, vertex layout, blend state, ...)
// plus each shader's identity as a one-way hash of its code bytes. No shader code ships in any form: the code is found
// by that hash in the player's own game files (the BNSH shader containers in the romfs' shader archives, effects and
// UI archives - here inside extracted/romfs.bin - and in the main module's data), and vulkan/exec.c builds the
// variants through the same code its draws use. The objects are kept for the game, which finds them by content at
// first use (code addresses differ from run to run). Anything the list misses still compiles at first use
// (asynchronously, as before) and is logged as a first visit ("[shader] first visit"). The player's own list
// (shader_seen.c) adds every variant met once to the next preparation.
//
// When (runtime/ui/launcher.mm): on the start screen, in the background, only when the shader caches are cold - the
// SPIR-V cache and the driver's pipeline cache make a warm start compile nothing anyway, so a preparation is due when
// the build, the translator, the GPU / driver or the global memory mode changed since the last one (the tag below).
// PLAY waits for it (a progress line).
//
// LARECOMP_RECORD_KEYS=<file> (dev): every program / pipeline / compute variant the game builds is appended to <file>
// (one line each, deduplicated). LARECOMP_SHADER_LIST=<file>: that list instead of the shipped one.
// LARECOMP_SHADER_PREP=0: never prepare; =2: prepare on every start (tests).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// (SR_NO_DS_WRITE: this fork's, recorded by the player's own list - copy mode's fragment key ShaderKey.no_ds_write,
// the draw writes no depth / stencil; the shipped list can't say, so a record without a depth target implies it there)
enum { SR_FLIP_Y = 1, SR_DEPTH_MINUS_ONE = 2, SR_FLIP_X = 4, SR_NO_DS_WRITE = 8 };

typedef struct {
    char kind;               // 'G' graphics pipeline, 'C' compute program
    uint64_t vs, fs;         // code hashes (shader_code_hash); compute: vs
    uint8_t flags;           // SR_*: lower-left window origin, NVN depth mode [-1, 1], mirrored composite
    uint8_t nattrib, nstream, ncolor;
    uint32_t attrib[16];     // NVN vertex attribute state: format | stream << 8 | offset << 13
    uint32_t stream[16][2];  // vertex stream stride, divisor
    uint32_t color[8];       // render target texture formats (0: no target)
    uint32_t cview[8];       // render target view formats (0: the texture's own)
    uint32_t depth;          // depth / stencil target format (0: none)
    uint32_t samples;        // samples of the targets (1: single-sampled)
    uint32_t channel_mask;   // color write masks, 4 bits per target
    uint8_t blend_on;        // blending enabled, 1 bit per target
    uint8_t blend[8][8];     // NVN blend state per target (zero where blending is off)
    uint8_t a2c;             // alpha to coverage
    uint8_t topo;            // primitive class: 0 points, 1 lines, 2 triangles
    uint8_t poly_fill;       // filled polygons (0: lines)
    uint32_t local[3];       // compute: local size
} ShaderRec;

// The record's text form (one line, no newline) and back
void shader_rec_format(const ShaderRec *r, char *out, size_t n);
bool shader_rec_parse(const char *line, ShaderRec *r);

// One-way hash of a shader's code bytes: what identifies a shader in the records
uint64_t shader_code_hash(const uint8_t *code, size_t n);
struct NvnShaderStage;
uint64_t shader_stage_hash(const struct NvnShaderStage *s);  // its code up to the end of its constants; 0: unknown
uint8_t shader_topo_class(uint32_t mode);                     // NVN draw mode -> ShaderRec.topo

// (LARECOMP_RECORD_KEYS, the player's own list) note a variant the game builds; cheap when recording is off
bool shader_rec_recording(void);
void shader_rec_note(const ShaderRec *r);

// ---- the player's own list (runtime/nvn/shader_seen.c): cache/shader_seen.txt in the data folder, in the shipped
// list's format. Every variant the game builds that the shipped list doesn't have is added (queued by
// shader_rec_note, written by a background thread in batches; at most 20000 records), and the next preparation does
// them after the shipped list ("[shader] prepared N + M seen before"). LARECOMP_SHADER_SEEN=0 turns it off.
bool shader_seen_on(void);
void shader_seen_note(const ShaderRec *r);
bool shader_seen_shipped(const char *line);  // the record (its text form) is in the shipped list
// (preparation) the file's text (NULL: none) and the game id in its header (0: none)
char *shader_seen_read(uint64_t *file_game);
// (preparation) game: the id of the game files' shaders; keep (not NULL: the file was of another game): the records
// whose shaders this game has, to replace the file's records with
void shader_seen_prep_done(uint64_t game, const char *keep);
const char *shader_prep_shipped_list(void);  // the shipped list's text
bool shader_cache_write(const char *path, const void *data, size_t n);  // temp file + rename: never half-written

// First visits: a program / pipeline / compute program the game had to build itself (preparation didn't cover it).
// what: "program", "pipeline", "compute"; a, b: its shaders' hashes; id: what it is (logged once per process)
void shader_first_visit(const char *what, uint64_t a, uint64_t b, uint64_t id);

// ---- preparation
// Shader code found in the game files for a record's hash
typedef struct {
    const uint8_t *code;     // the stage's code (graphics: with the 0x30-byte NVN header and the program header)
    uint32_t size;           // bytes up to the end of its constants
    const uint8_t *control;  // its control section (NULL: not needed - the record holds the compute local size)
    uint32_t const_offset, const_size;
} ShaderBlob;

// Backend (runtime/nvn/vulkan/exec.c): build what the record describes through the draw path's own code and keep it
// for the game. Called on one thread, while no game runs; builds finish later (renderer_prepare_pending). The code
// blobs are only valid during the call. false: not built (the translation failed).
bool renderer_prepare_begin(void);
bool renderer_prepare(const ShaderRec *r, const ShaderBlob *vs, const ShaderBlob *fs);
int renderer_prepare_pending(void);
int renderer_prepare_queued(void);  // (builds queued so far, finished or not: the progress)
void renderer_prepare_end(void);  // all builds done (keeps the pipeline cache)
// The tag of what the caches were built with (this build's translator, the GPU / driver, the global memory mode the
// game will use): a preparation is due when the last one's differs
void renderer_prepare_tag(char *out, size_t n);

// Prepares every record of the shipped list (LARECOMP_SHADER_LIST=<file>: that list instead) from the game files
// (root: the extracted game, <root>/romfs.bin and <root>/exefs/main), then the player's own list (shader_seen_*).
// progress(done, total) is called now and then; *cancel (any thread) stops it early (builds already started finish
// in the background). Logs "[shader] prepared N + M seen before, not found K". Returns false when it couldn't start
// (no list, no GPU device).
bool shader_prep_run(const char *root, void (*progress)(int done, int total), const volatile int *cancel);
// Whether a preparation is due: the tag of the last finished one (cache/shader_prep.tag) differs from tag
bool shader_prep_due(const char *tag);
void shader_prep_done(const char *tag);  // (after a run that wasn't cancelled)
// (LARECOMP_SHADER_PREP=1 when due, =2 always; tests, which boot without the start screen) the same at game start,
// before the game's first frame: runtime/nvn/objects.c. romfs: the extracted game's romfs path (<root>/romfs)
void shader_prep_boot(const char *romfs);
