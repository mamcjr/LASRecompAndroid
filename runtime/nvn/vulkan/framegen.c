// 60 fps (generated): the game runs at its own 30 frames a second, and after each real frame an in-between one is
// made on the GPU and shown one refresh later. Adapted from LASRecomp-Android-Plus by Nycolazs
// (https://github.com/Nycolazs/LASRecomp-Android-Plus, runtime/nvn/vulkan/framegen.c at ef11b469; MIT, same upstream
// and license as this project), itself after the WWHD port's runtime/src/vk/reproj.cpp.
// The in-between frame is the real one reprojected with the scene's depth to the camera moved on by half its last
// step (the camera's view-projection, each frame: runtime/hle/widescreen.c). The HUD stays where it is: at the
// scene's composite into the presented picture (exec.c, as mirror mode finds it) the picture is copied aside (a
// quarter of its size), and pixels where the finished picture differs from that copy are HUD. The in-between frame is
// drawn from the picture the real one was presented from (exec.c vk_last_present_src) into the next swapchain image,
// the window's overlays (touch controls, the FPS overlay) drawn again on top, and presented right after: FIFO shows it
// one refresh later. Link is held where the real frame has him; other characters' own movement isn't reprojected
// (it stays at 30 a second) - unless LARECOMP_FRAMEGEN_FLOW=1 (experimental): block matching against the previous
// real frame moves what moved on its own by half its step too (kFlow). Android only (this fork's scope): Video > Frame generation; LARECOMP_FRAMEGEN=0/1.
#ifdef __ANDROID__
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <string.h>

#include "../../rt.h"
#include "../../settings.h"
#include "vk.h"

// ---- the frame's inputs (render thread)
static TexBackend g_scene;        // the presented picture right after the scene's composite (no HUD yet)
static bool g_scene_ok;           // captured this frame
static NvnTexture *g_depth;       // the scene's depth (the main pass's)
static float g_depth_scale[2];    // its content's share of the depth image (the game's dynamic resolution)
static float g_vp[16], g_vp_prev[16];
static bool g_have_vp, g_have_prev;
static uint64_t g_vp_frame;
static TexBackend g_comp;         // the window picture of the real frame
static bool g_comp_ok;

static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
static VkPipeline g_pipe;
static VkFormat g_pipe_fmt;

static const char kWarp[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D comp;\n"   // the window picture
    "layout(set = 0, binding = 1) uniform sampler2D scene;\n"  // the presented picture before the HUD
    "layout(set = 0, binding = 2) uniform sampler2D fin;\n"    // the presented picture (with the HUD)
    "layout(set = 0, binding = 3) uniform sampler2D depthT;\n" // the scene's depth
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    // m0..m3: rows of (generated view-projection) x inverse(this frame's); rect: the game picture in the window (uv);
    // crop: the part of the presented picture shown there; a: flags (1 y flip: game NDC up = the image's first row, 2 depth in
    // [-1, 1], 4 present flip, 8 comp in the game's layout, 16 sRGB-encoded picture, 32 objects' own movement), Link's radius (uv, 0 = none), his depth, the mode; b: Link (uv), the depth's scale (zw)
    "layout(push_constant) uniform P { vec4 m0; vec4 m1; vec4 m2; vec4 m3; vec4 rect; vec4 crop; vec4 a; vec4 b; } p;\n"
    "bool flag(uint f) { return (uint(p.a.x) & f) != 0u; }\n"
    // (flag 8: comp is the presented picture in the game's texture layout, sampled at game uv; else the window picture)
    "vec2 toGame(vec2 w) { vec2 q = (w - p.rect.xy) / p.rect.zw; if (flag(4u)) q.y = 1.0 - q.y; return p.crop.xy + q * p.crop.zw; }\n"
    "vec2 toWin(vec2 g);\n"
    "vec4 compAt(vec2 g) { return flag(8u) ? textureLod(comp, g, 0.0) : textureLod(comp, toWin(g), 0.0); }\n"
    "vec2 toWin(vec2 g) { vec2 q = (g - p.crop.xy) / p.crop.zw; if (flag(4u)) q.y = 1.0 - q.y; return p.rect.xy + q * p.rect.zw; }\n"
    // the scene before the HUD as the presented picture has it: that copy is the composite's linear (HDR) colour, the
    // presented picture sRGB-encoded (sampled raw: exec.c raw_view). Pasted in linear it was much darker - a dark line
    // under the hearts where the scenery moved out from under them
    "vec3 sceneAt(vec2 g) {\n"
    "  vec3 c = clamp(textureLod(scene, g, 0.0).rgb, 0.0, 1.0);\n"
    "  if (!flag(16u)) return c;\n"
    "  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));\n"
    "}\n"
    "layout(set = 0, binding = 4) uniform sampler2D hudMask;\n"  // where the HUD is: a quarter of the presented picture
    // how much of the HUD's neighbourhood g is in (0..1): the mask a quarter texel further around (the faint edge of
    // the hearts' soft shadow is under the mask's threshold - moved with the scenery it left a dark line under them)
    "float hudW(vec2 g) {\n"
    "  vec2 t = 1.0 / vec2(textureSize(hudMask, 0));\n"
    "  float w = textureLod(hudMask, g, 0.0).r;\n"
    "  for (int i = 0; i < 4; i++) w = max(w, textureLod(hudMask, g + t * vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0), 0.0).r);\n"
    "  return clamp(w * 2.0, 0.0, 1.0);\n"
    "}\n"
    // where the scene point at g lands in the generated picture
    "vec2 moved(vec2 g) {\n"
    "  vec2 n = g;\n"
    // the depth as an average over ~9x9 pixels (five plain fetches, as view distance): the exact depth tore thin
    // layered things (tall grass, plants) into rows when the camera moved - neighbouring pixels near and far
    "  vec2 o = 4.0 / vec2(textureSize(depthT, 0));\n"
    "  float z = 0.0;\n"
    "  for (int i = 0; i < 5; i++) {\n"
    "    vec2 q = n * p.b.zw + o * (i == 0 ? vec2(0.0) : vec2((i & 1) == 1 ? -1.0 : 1.0, (i & 2) == 2 ? -1.0 : 1.0));\n"
    "    z += 1.0 / max(1.0 - textureLod(depthT, q, 0.0).r, 1e-6);\n"
    "  }\n"
    "  float d = 1.0 - 5.0 / z;\n"
    "  vec4 c = vec4(n.x * 2.0 - 1.0, flag(1u) ? 1.0 - 2.0 * n.y : 2.0 * n.y - 1.0, flag(2u) ? d * 2.0 - 1.0 : d, 1.0);\n"
    "  vec4 h = vec4(dot(p.m0, c), dot(p.m1, c), dot(p.m2, c), dot(p.m3, c));\n"
    "  if (h.w <= 1e-5) return g;\n"
    "  vec2 e = h.xy / h.w;\n"
    "  vec2 n2 = vec2(e.x * 0.5 + 0.5, flag(1u) ? 0.5 - e.y * 0.5 : e.y * 0.5 + 0.5);\n"
    "  return n2;\n"
    "}\n"
    // Link (the player) moves on his own and the camera follows him: kept where the real frame has him instead of
    // being moved with the scenery (he shook back and forth). How much (1 = held): around his projected position,
    // at his depth, easing into the scenery's movement towards the edge (a continuous bend: no seam, no second Link)
    "float linkW(vec2 g) {\n"
    "  if (p.a.y <= 0.0) return 0.0;\n"
    "  vec2 ts = vec2(textureSize(depthT, 0));\n"
    "  vec2 d = (g - p.b.xy) / vec2(p.a.y, p.a.y * ts.x * p.b.z / (ts.y * p.b.w));\n"
    "  float r2 = dot(d, d);\n"
    "  if (r2 >= 1.0) return 0.0;\n"
    "  float z = 1.0 / max(1.0 - textureLod(depthT, g * p.b.zw, 0.0).r, 1e-6);\n"
    "  return (1.0 - smoothstep(0.35, 1.0, sqrt(r2))) * (1.0 - smoothstep(0.05, 0.1, abs(z * p.a.z - 1.0)));\n"
    "}\n"
    "layout(set = 0, binding = 5) uniform sampler2D flowT;\n"  // objects' own movement per 16x16 block (kFlow; flag 32)
    // half the movement of the object at g beyond the camera's, where g's depth is its block's
    "vec2 objMove(vec2 g) {\n"
    "  if (!flag(32u)) return vec2(0.0);\n"
    "  vec4 f = textureLod(flowT, g, 0.0);\n"
    "  if (f.w < 0.5) return vec2(0.0);\n"
    "  float z = 1.0 / max(1.0 - textureLod(depthT, g * p.b.zw, 0.0).r, 1e-6);\n"
    "  return abs(z - f.z) > 0.15 * f.z ? vec2(0.0) : f.xy * 0.5;\n"
    "}\n"
    "void main() {\n"
    // (outside the game picture first: the plain copy below, sampled there, stretched the picture's last column of
    // pixels across the black bars - every in-between frame with the camera still)
    "  if (any(lessThan(uv, p.rect.xy)) || any(greaterThan(uv, p.rect.xy + p.rect.zw))) { o = flag(8u) ? vec4(0.0, 0.0, 0.0, 1.0) : textureLod(comp, uv, 0.0); return; }\n"
    "  if (p.a.w > 1.5 && p.a.w < 2.5) { vec2 n = toGame(uv); float d = textureLod(depthT, n * p.b.zw, 0.0).r; o = vec4(vec3(clamp((1.0 - d) * 20.0, 0.0, 1.0)), 1.0); return; }\n"  // (LARECOMP_FRAMEGEN_DEBUG=depth)
    "  if (p.a.w > 0.5 && p.a.w < 1.5) { o = flag(8u) ? compAt(toGame(uv)) : textureLod(comp, uv, 0.0); return; }\n"  // (LARECOMP_FRAMEGEN_PLAIN / a still camera: the real picture again)
    "  vec2 g = toGame(uv);\n"
    "  if (p.a.w > 5.5) { o = vec4(sceneAt(g), 1.0); return; }\n"  // (=scene: the scene before the HUD)
    "  if (p.a.w > 4.5) { o = vec4(compAt(g).rgb * (1.0 - 0.6 * hudW(g)) + vec3(0.6 * hudW(g), 0.0, 0.0), 1.0); return; }\n"  // (=mask)
    "  if (p.a.w > 3.5) { o = vec4(abs(compAt(g).rgb - sceneAt(g)) * 4.0, 1.0); return; }\n"  // (=diff)
    "  if (p.a.w > 2.5) { o = compAt(g); o.r = max(o.r, linkW(g)); return; }\n"  // (LARECOMP_FRAMEGEN_DEBUG=link)
    "  float w = hudW(g);\n"
    "  if (w >= 1.0) { o = compAt(g); return; }\n"
    "  vec2 lo = p.crop.xy, hi = p.crop.xy + p.crop.zw;\n"
    "  vec2 s = g;\n"
    "  for (int i = 0; i < 2; i++) s = clamp(g - (moved(s) - s) - objMove(s), lo, hi);\n"  // (the source pixel that lands here)
    // (Link held only where his block's own movement wasn't found: found, it already keeps him with the camera)
    "  s = mix(s, g, any(notEqual(objMove(g), vec2(0.0))) ? 0.0 : linkW(g));\n"
    // the scenery: where the source is near the HUD, from the scene before it (soft: a quarter of the size)
    "  vec3 bg = mix(compAt(s).rgb, sceneAt(s), hudW(s));\n"
    // the HUD and its surroundings stay as in the real frame, fading into the moved scenery over a quarter texel
    "  o = vec4(mix(bg, compAt(g).rgb, w), 1.0);\n"
    "}\n";

// the HUD mask: each texel covers 4x4 pixels of the presented picture, set where it differs from the scene's copy
static const char kMask[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D scene;\n"
    "layout(set = 0, binding = 1) uniform sampler2D fin;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 t = 1.0 / vec2(textureSize(fin, 0));\n"
    "  float m = 0.0;\n"
    "  for (int i = 0; i < 4; i++) {\n"  // (bilinear taps between the 2x2 quads of the block: all 16 pixels)
    "    vec2 q = uv + t * vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0);\n"
    "    vec3 d = abs(textureLod(fin, q, 0.0).rgb - textureLod(scene, q, 0.0).rgb);\n"
    "    m = max(m, step(0.01, max(d.r, max(d.g, d.b))));\n"
    "  }\n"
    "  o = vec4(m);\n"
    "}\n";
// The same with the scene kept at a quarter of the size (fg_sceneq): the scene's 4x4 average (two 2:1 linear blits)
// against the finished picture's (the four bilinear taps of the block)
static const char kMaskQ[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D scene;\n"
    "layout(set = 0, binding = 1) uniform sampler2D fin;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 t = 1.0 / vec2(textureSize(fin, 0));\n"
    "  vec3 f = vec3(0.0);\n"
    "  for (int i = 0; i < 4; i++) f += textureLod(fin, uv + t * vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0), 0.0).rgb;\n"
    "  vec3 d = abs(f * 0.25 - textureLod(scene, uv, 0.0).rgb);\n"
    // (0.05: the two sides round differently - 8-bit sRGB blits against filtered taps - by up to a few levels)
    "  o = vec4(step(0.05, max(d.r, max(d.g, d.b))));\n"
    "}\n";

// ---- objects' own movement (LARECOMP_FRAMEGEN_FLOW=1; after LASRecomp-Android-Plus's c751b6c2, from the WWHD port):
// with the camera's movement alone characters stood still in the in-between frame and then jumped. Each 16x16 block of
// the scene is looked for in the previous real frame; blocks that clearly moved on their own (and agree with their
// neighbours) move by half that. Plain fetches only: their version gathered (textureGather) from the depth buffer and
// the images, and the phone froze and hard-reset when it switched on - they took it out; it's off here until checked.
// A quarter-size brightness picture of the scene before the HUD (g_scene: already a quarter of the presented picture,
// or the whole one with LARECOMP_FG_SCENEQ=0 - four taps cover both)
static const char kDown[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D scene;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 t = 0.25 * abs(vec2(dFdx(uv.x), dFdy(uv.y)));\n"  // (a quarter of this picture's texel: bilinear taps)
    "  vec3 c = vec3(0.0);\n"
    "  for (int i = 0; i < 4; i++) c += textureLod(scene, uv + t * vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0), 0.0).rgb;\n"
    "  o = vec4(dot(c * 0.25, vec3(0.299, 0.587, 0.114)));\n"
    "}\n";
// One value per 16x16 block (4x4 texels of the quarter picture): the block is looked for in the previous frame's
// picture around where the camera's movement alone puts it, and around the same place on the screen (what the camera
// follows: Link). Out: the movement beyond the camera's, previous -> this frame (uv), the block's nearest depth
// (1 / (1 - d): view distance up to a factor), valid. m0..m3: rows of previous view-projection x inverse(this one's);
// a: y flip (the warp's flag 1), depth in [-1, 1]; b.zw: the depth's content scale
static const char kFlow[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D curImg;\n"
    "layout(set = 0, binding = 1) uniform sampler2D prevImg;\n"
    "layout(set = 0, binding = 2) uniform sampler2D depthT;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "layout(push_constant) uniform P { vec4 m0; vec4 m1; vec4 m2; vec4 m3; vec4 a; vec4 b; } p;\n"
    "void block(sampler2D img, vec2 at, vec2 ts, out float v[16]) {\n"  // (16 plain fetches at the texels' centres)
    "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) v[j * 4 + i] = textureLod(img, at + (vec2(i, j) - 1.5) * ts, 0.0).r;\n"
    "}\n"
    "float sad(float a[16], float b[16]) { float s = 0.0; for (int i = 0; i < 16; i++) s += abs(a[i] - b[i]); return s; }\n"
    "void main() {\n"
    "  o = vec4(0.0);\n"
    "  vec2 ts = 1.0 / vec2(textureSize(curImg, 0));\n"
    "  float cur[16]; block(curImg, uv, ts, cur);\n"
    "  float mean = 0.0; for (int i = 0; i < 16; i++) mean += cur[i];\n"
    "  mean *= 1.0 / 16.0;\n"
    "  float dv = 0.0; for (int i = 0; i < 16; i++) dv += abs(cur[i] - mean);\n"
    "  vec2 dq = uv * p.b.zw, dt = 1.0 / vec2(textureSize(depthT, 0));\n"  // (the block's nearest depth: four fetches)
    "  float d = 1.0;\n"
    "  for (int i = 0; i < 4; i++) d = min(d, textureLod(depthT, dq + vec2((i & 1) == 0 ? -4.0 : 4.0, (i & 2) == 0 ? -4.0 : 4.0) * dt, 0.0).r);\n"
    "  o.z = min(1.0 / max(1.0 - d, 1e-6), 60000.0);\n"
    "  if (dv < 0.12) return;\n"  // (little detail: no reliable match)
    "  vec4 c = vec4(uv.x * 2.0 - 1.0, p.a.x > 0.5 ? 1.0 - 2.0 * uv.y : 2.0 * uv.y - 1.0, p.a.y > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
    "  vec4 h = vec4(dot(p.m0, c), dot(p.m1, c), dot(p.m2, c), dot(p.m3, c));\n"
    "  bool camOk = h.w > 1e-5;\n"
    "  vec2 e = camOk ? h.xy / h.w : vec2(0.0);\n"
    "  vec2 pp = camOk ? vec2(e.x * 0.5 + 0.5, p.a.x > 0.5 ? 0.5 - e.y * 0.5 : e.y * 0.5 + 0.5) : uv;\n"
    "  camOk = camOk && all(greaterThan(pp, vec2(0.01))) && all(lessThan(pp, vec2(0.99)));\n"
    "  float best = 1e9, atCam = 1e9; vec2 bp = uv; float b[16];\n"
    "  for (int k = 0; k < 2; k++) {\n"  // (around pp, and around the same place on the screen; +-6 texels, then refined)
    "    vec2 c0 = k == 0 ? pp : uv;\n"
    "    if (k == 0 && !camOk) continue;\n"
    "    for (int oy = -6; oy <= 6; oy += 2) for (int ox = -6; ox <= 6; ox += 2) {\n"
    "      vec2 at = c0 + vec2(ox, oy) * ts;\n"
    "      if (any(lessThan(at, vec2(0.01))) || any(greaterThan(at, vec2(0.99)))) continue;\n"
    "      block(prevImg, at, ts, b); float sd = sad(cur, b);\n"
    "      if (k == 0 && ox == 0 && oy == 0) atCam = sd;\n"
    "      if (sd < best) { best = sd; bp = at; }\n"
    "    }\n"
    "  }\n"
    "  vec2 c1 = bp;\n"
    "  for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {\n"
    "    if (ox == 0 && oy == 0) continue;\n"
    "    vec2 at = c1 + vec2(ox, oy) * ts;\n"
    "    block(prevImg, at, ts, b); float sd = sad(cur, b);\n"
    "    if (sd < best) { best = sd; bp = at; }\n"
    "  }\n"
    "  if (best > 1.2 || atCam <= best * 1.5 + 0.05) return;\n"  // (no clear match, or the camera explains it)
    "  o = vec4(pp - bp, o.z, 1.0);\n"
    "}\n";
// a block keeps its movement only when at least 2 of its 8 neighbours moved the same way (their average); lone
// blocks (water, flicker) are dropped
static const char kFilter[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D flowIn;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec4 c = textureLod(flowIn, uv, 0.0);\n"
    "  o = vec4(0.0, 0.0, c.z, 0.0);\n"
    "  if (c.w < 0.5) return;\n"
    "  vec2 ts = 1.0 / vec2(textureSize(flowIn, 0));\n"
    "  vec2 sum = c.xy; float n = 0.0;\n"
    "  for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {\n"
    "    if (i == 0 && j == 0) continue;\n"
    "    vec4 q = textureLod(flowIn, uv + vec2(i, j) * ts, 0.0);\n"
    "    if (q.w > 0.5 && all(lessThan(abs(q.xy - c.xy), ts * 0.25))) { sum += q.xy; n += 1.0; }\n"
    "  }\n"
    "  if (n >= 2.0) o = vec4(sum / (n + 1.0), c.z, 1.0);\n"
    "}\n";
static TexBackend g_q[2], g_flow_raw, g_flow;  // brightness pictures (this frame's: g_q[g_qi]), block movement
static int g_qi;
static bool g_q_prev, g_flow_ok;  // the previous frame's picture is there; this frame's movement is
static void build_flow(VkCommandBuffer cb);

static TexBackend g_mask;
static TexBackend *g_fin;
static bool build_mask(VkCommandBuffer cb);
static VkDescriptorSetLayout g_mdsl;
static VkPipelineLayout g_mlayout;
static VkPipeline g_mpipe;

// The in-between frame is drawn from the picture the real frame was presented from (post-processed, the game's
// texture layout) with the window's overlays drawn again on top (device.c), instead of a copy of the finished window
// picture: 2400x1080 copied every real frame was ~0.7 ms of a Galaxy S20 FE's GPU (their measurement). LARECOMP_FG_WINCOPY=1: the copy.
static bool fg_wincopy(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_FG_WINCOPY") ? atoi(getenv("LARECOMP_FG_WINCOPY")) != 0 : 0;
    return on;
}
bool fg_draws_overlays(void) { return !fg_wincopy(); }

// The scene before the HUD kept at a quarter of its size (two 2:1 linear blits) instead of copied whole: it is only
// compared block by block for the HUD mask, and sampled where the HUD covered the scenery (the in-between frame shows
// it there a little soft). The whole copy was ~0.9 ms of a Galaxy S20 FE's GPU at 1080p (theirs). LARECOMP_FG_SCENEQ=0: the copy.
static bool fg_sceneq(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_FG_SCENEQ") ? atoi(getenv("LARECOMP_FG_SCENEQ")) != 0 : 1;
    return on;
}
static TexBackend g_scene_half;

// LARECOMP_FG_TEST=<bits> (measuring only, the in-between frame comes out wrong): 1 no scene copy, 2 no window copy,
// 4 no HUD mask, 8 no full barriers around the scene copy
static int fg_test(void) {
    static int t = -1;
    if (t < 0) t = getenv("LARECOMP_FG_TEST") ? atoi(getenv("LARECOMP_FG_TEST")) : 0;
    return t;
}

static int fg_setting(void) {
    static int env = -2;
    if (env == -2) env = getenv("LARECOMP_FRAMEGEN") ? atoi(getenv("LARECOMP_FRAMEGEN")) : -1;
    return env >= 0 ? env : g_settings.frame_gen;
}

bool fg_enabled(void) { return fg_setting() != 0; }

static bool make_image(TexBackend *t, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    if (t->image && t->width == w && t->height == h && t->format == f) return true;
    if (t->image) vk_views_forget(t), vk_defer_image(t->image, t->mem);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *t = (TexBackend){0};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &t->image, &t->mem, NULL) != VK_SUCCESS) { t->image = VK_NULL_HANDLE; return false; }
    vk_image_init_layout(vk_upload_cmd(), t->image, VK_IMAGE_ASPECT_COLOR_BIT);
    t->format = f, t->width = w, t->height = h, t->depth = 1, t->levels = t->layers = t->samples = 1;
    t->aspect = VK_IMAGE_ASPECT_COLOR_BIT, t->base_type = VK_IMAGE_VIEW_TYPE_2D, t->gpu_owned = true;
    return true;
}

// exec.c draw_: a draw writing depth. The scene's depth is the one most draws of the frame write (the main pass's;
// not the shadow map's, a square one, nor the HUD's)
static struct { NvnTexture *t; int n; float sx, sy; } g_dc[8];
void fg_note_draw(NvnTexture *depth, uint32_t pass_w, uint32_t pass_h) {
    if (!depth || depth->width == depth->height) return;
    int i = 0;
    while (i < 8 && g_dc[i].t && g_dc[i].t != depth) i++;
    if (i == 8) return;
    if (!g_dc[i].t) {
        TexBackend *b = depth->backend;
        g_dc[i].t = depth;
        g_dc[i].sx = b && b->width ? (float)pass_w / b->width : 1;
        g_dc[i].sy = b && b->height ? (float)pass_h / b->height : 1;
    }
    g_dc[i].n++;
}
static void pick_depth(void) {
    int best = -1;
    for (int i = 0; i < 8 && g_dc[i].t; i++) if (best < 0 || g_dc[i].n > g_dc[best].n) best = i;
    g_depth = best >= 0 ? g_dc[best].t : NULL;
    if (best >= 0) g_depth_scale[0] = g_dc[best].sx, g_depth_scale[1] = g_dc[best].sy;
    memset(g_dc, 0, sizeof g_dc);
}

// exec.c draw_, right after the scene's composite into the presented picture (outside any pass)
static _Atomic bool g_paused;  // (fg_frame_wanted: in-between frames paused - the GPU full)
void fg_scene_composited(VkCommandBuffer cb, TexBackend *rt) {
    // (paused: no copy either - its full barriers mid-frame cost the driver it paused for: Qualcomm's on the POCO F7
    // Ultra, 8% of real frames late paused against 2.9% with frame generation off; the first frame after a pause goes
    // without an in-between one)
    if (!fg_enabled() || !rt || !rt->image || rt->samples != 1 || g_paused) return;
    if (fg_sceneq()) {
        uint32_t hw = (rt->width + 1) / 2, hh = (rt->height + 1) / 2, qw = (rt->width + 3) / 4, qh = (rt->height + 3) / 4;
        VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!make_image(&g_scene_half, rt->format, hw, hh, u) || !make_image(&g_scene, rt->format, qw, qh, u)) return;
        vk_full_barrier(cb);
        VkImageBlit b1 = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)rt->width, (int32_t)rt->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)hw, (int32_t)hh, 1}}};
        vkCmdBlitImage(cb, rt->image, VK_IMAGE_LAYOUT_GENERAL, g_scene_half.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b1, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
        VkImageBlit b2 = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)hw, (int32_t)hh, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)qw, (int32_t)qh, 1}}};
        vkCmdBlitImage(cb, g_scene_half.image, VK_IMAGE_LAYOUT_GENERAL, g_scene.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b2, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
        g_scene_ok = true;
        return;
    }
    if (!make_image(&g_scene, rt->format, rt->width, rt->height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return;
    if (!(fg_test() & 8)) vk_full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {rt->width, rt->height, 1}};
    if (!(fg_test() & 1)) vkCmdCopyImage(cb, rt->image, VK_IMAGE_LAYOUT_GENERAL, g_scene.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    if (!(fg_test() & 8)) vk_full_barrier(cb);
    g_scene_ok = true;
}

static void mat_mul(const float *a, const float *b, float *o) {  // rows: o = a b
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
}

static bool mat_inv(const float *m, float *o) {
    float inv[16], det;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-20f) return false;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return true;
}

// device.c: whether this frame gets an in-between one (asked before the real frame is presented)
bool fg_frame_wanted(uint64_t frame, TexBackend *fin) {
    extern int g_present_interval;  // runtime/nvn/objects.c: the game's swap interval (2: its 30 fps mode)
    extern bool menu_is_open(void);
    extern bool widescreen_camera(uint64_t frame, float vp[16]);
    if (!fg_enabled()) return false;
    pick_depth();
    float vp[16];
    bool have = widescreen_camera(frame, vp);
    if (have) {
        if (g_have_vp) memcpy(g_vp_prev, g_vp, sizeof g_vp), g_have_prev = true;
        memcpy(g_vp, vp, sizeof vp), g_have_vp = true, g_vp_frame = frame;
    } else {
        g_have_vp = g_have_prev = false;
    }
    // Only while the game holds its 30 on this GPU: real frames late (the GPU full) get no in-between ones - their GPU
    // time goes back to the game. With FIFO a real frame takes two refreshes (33.3 ms) or, late, three (50 ms), so late
    // frames are counted rather than averaged: 6 of the last 60 (2 s) pause it. Then it tries again after a wait that
    // doubles at each try that doesn't hold 20 s (5 s .. 80 s), back to 5 s once one has. (An average with a 34.5 /
    // 36.5 ms band before: Qualcomm's driver on the POCO F7 Ultra ran at 34-37 ms with in-between frames and paused and
    // resumed every second or two - half the frames generated, flickering between 30 and 60.)
    // (Late frames without in-between ones too - the same driver walking through the village: 6-12% - count as the
    // base: only what in-between frames add beyond it pauses them. Measured while paused, a slow moving average.)
    // (Not counted for 1.5 s after a change - in-between frames starting or pausing, the game going to its 30: going
    // from one to two pictures a frame queues them for a moment, and those few late frames alone paused every try.)
    static double last, paused_at, trial_at, wait_ms = 5000, base, grace_until;
    static uint8_t late[60];
    static int idx, nlate;
    static bool paused, counting;
    double now = SDL_GetTicksNS() / 1e6;
    bool countable = have && g_present_interval == 2 && !menu_is_open();
    if (countable != counting) counting = countable, grace_until = now + 1500;
    if (countable && now >= grace_until && last > 0 && now - last < 200) {
        uint8_t l = now - last > 41.7;  // (more than two and a half refreshes: a third one)
        nlate += l - late[idx], late[idx] = l, idx = (idx + 1) % 60;
        if (paused) base += (l - base) * 0.01;  // (~100 frames)
    }
    last = now;
    if (!paused && nlate >= 6 + (int)(base * 60 + 0.5)) {
        if (trial_at > 0) wait_ms = fmin(wait_ms * 2, 80000);  // (the last try didn't hold)
        paused = true, paused_at = now, trial_at = 0;
        LOG("[framegen] paused: %d of the last 60 real frames late (base %.0f%%: the GPU full); next try in %.0f s\n", nlate,
            base * 100, wait_ms / 1000);
        memset(late, 0, sizeof late), nlate = 0, grace_until = now + 1500;
    } else if (paused && now - paused_at >= wait_ms) {
        paused = false, trial_at = now;
        memset(late, 0, sizeof late), nlate = 0, grace_until = now + 1500;
        LOG("[framegen] trying again\n");
    } else if (!paused && trial_at > 0 && now - trial_at >= 20000) {
        trial_at = 0, wait_ms = 5000;  // (it held)
    }
    g_paused = paused;
    bool ok = have && g_have_prev && g_scene_ok && g_depth && g_depth->backend && g_present_interval == 2 && !menu_is_open() && !paused;
    static int logwhy = -1;  // LARECOMP_LOG_FRAMEGEN=1: why frames went without an in-between one, every 2 s
    if (logwhy < 0) logwhy = getenv("LARECOMP_LOG_FRAMEGEN") != NULL;
    if (logwhy) {
        static int n, why[6];
        static double at;
        n++;
        why[0] += !have || !g_have_prev, why[1] += !g_scene_ok, why[2] += !g_depth || !g_depth->backend;
        why[3] += g_present_interval != 2, why[4] += menu_is_open(), why[5] += paused;
        if (now - at > 2000) {
            if (why[0] + why[1] + why[2] + why[3] + why[5])
                LOG("[framegen] of %d frames without: camera %d, scene %d, depth %d, interval %d, menu %d, paused %d (late %d of 60)\n", n,
                    why[0], why[1], why[2], why[3], why[4], why[5], nlate);
            n = 0, memset(why, 0, sizeof why), at = now;
        }
    }
    if (!ok) g_scene_ok = false, g_depth = NULL;
    g_fin = fin;
    return ok;
}

// device.c vk_frame_end, outside rendering: the real window picture, before it goes to the presentation engine
void fg_copy_window(VkCommandBuffer cb, VkImage sc, VkFormat fmt, uint32_t w, uint32_t h) {
    g_comp_ok = false;
    if (!fg_wincopy()) {  // (only the HUD mask: the in-between frame draws from the presented picture)
        g_comp_ok = build_mask(cb);
        if (g_comp_ok) build_flow(cb);
        return;
    }
    if (!make_image(&g_comp, fmt, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return;
    VkImageMemoryBarrier2 b[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .image = sc, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .image = g_comp.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = b};
    vkCmdPipelineBarrier2(cb, &di);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {w, h, 1}};
    if (!(fg_test() & 2)) vkCmdCopyImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_comp.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT, b[0].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b[1].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, b[1].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    vkCmdPipelineBarrier2(cb, &di);
    g_comp_ok = (fg_test() & 4) ? true : build_mask(cb);
    if (g_comp_ok) build_flow(cb);
}

// a fullscreen triangle, uv = position (top-left 0, 0)
static const char kTri[] =
    "#version 450\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static VkPipeline fs_pipeline(VkShaderModule fs, VkFormat fmt, VkPipelineLayout layout) {
    static VkShaderModule tri;
    if (!tri) tri = vk_compile_glsl(kTri, STAGE_VERTEX, "framegen.vert");
    if (!tri) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = tri, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                               .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = layout};
    VkPipeline p;
    return vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

static VkPipelineLayout set_layout(int nbind, uint32_t push, VkDescriptorSetLayout *dsl) {
    VkDescriptorSetLayoutBinding bd[8];
    for (int i = 0; i < nbind; i++)
        bd[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nbind,
                                          .pBindings = bd};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, dsl));
    VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, push};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = dsl,
                                     .pushConstantRangeCount = push ? 1 : 0, .pPushConstantRanges = push ? &r : NULL};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static VkPipeline warp_pipeline(VkFormat fmt) {
    if (g_pipe && g_pipe_fmt == fmt) return g_pipe;
    if (!g_layout) g_layout = set_layout(6, 128, &g_dsl);
    static VkShaderModule fs;
    if (!fs) fs = vk_compile_glsl(kWarp, STAGE_FRAGMENT, "framegen.frag");
    VkPipeline p = fs ? fs_pipeline(fs, fmt, g_layout) : VK_NULL_HANDLE;
    if (!p) return VK_NULL_HANDLE;
    g_pipe = p, g_pipe_fmt = fmt;  // (a format change is a new swapchain: the old one is left to the device's teardown)
    LOG("[framegen] 60 fps (generated): in-between frames by reprojection\n");
    return p;
}

static void bind_images(VkCommandBuffer cb, VkPipelineLayout layout, VkDescriptorSetLayout dsl, int n, VkImageView *v, const bool *linear) {
    VkDescriptorSet set = vk_alloc_set(dsl);
    VkDescriptorImageInfo ii[8];
    VkWriteDescriptorSet w[8];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(linear[i]), v[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
}

static VkImageView color_view(TexBackend *t) {
    VkComponentMapping ident = {0};
    return vk_image_view(t, VK_IMAGE_VIEW_TYPE_2D, t->format, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_COLOR_BIT);
}

// the HUD mask (outside any rendering: fg_copy_window)
static bool build_mask(VkCommandBuffer cb) {
    TexBackend *fin = g_fin;
    if (!fin || !fin->image || !g_scene.image) return false;
    uint32_t mw = (fin->width + 3) / 4, mh = (fin->height + 3) / 4;
    if (!make_image(&g_mask, VK_FORMAT_R8_UNORM, mw, mh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return false;
    if (!g_mlayout) g_mlayout = set_layout(2, 0, &g_mdsl);
    static int mq = -1;
    if (g_mpipe && mq != (int)fg_sceneq()) g_mpipe = VK_NULL_HANDLE;  // (the toggle read once: never in practice)
    mq = fg_sceneq();
    if (!g_mpipe) {
        VkShaderModule fs = mq ? vk_compile_glsl(kMaskQ, STAGE_FRAGMENT, "framegen_maskq.frag")
                               : vk_compile_glsl(kMask, STAGE_FRAGMENT, "framegen_mask.frag");
        g_mpipe = fs ? fs_pipeline(fs, VK_FORMAT_R8_UNORM, g_mlayout) : VK_NULL_HANDLE;
        if (!g_mpipe) return false;
    }
    VkImageView mv = color_view(&g_mask), v[2] = {color_view(&g_scene), color_view(fin)};
    if (!mv || !v[0] || !v[1]) return false;
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = mv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {mw, mh}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mpipe);
    static const bool lin[2] = {true, true};
    bind_images(cb, g_mlayout, g_mdsl, 2, v, lin);
    VkViewport vp = {0, 0, (float)mw, (float)mh, 0, 1};
    VkRect2D sc = {{0, 0}, {mw, mh}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    return true;
}

// a fullscreen pass of fs into target (outside any rendering); pc: push constants (size 0: none)
static bool run_pass(VkCommandBuffer cb, TexBackend *target, VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSetLayout dsl,
                     int n, VkImageView *v, const bool *linear, const void *pc, uint32_t pcsize) {
    VkImageView tv = color_view(target);
    if (!tv || !pipe) return false;
    for (int i = 0; i < n; i++) if (!v[i]) return false;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {target->width, target->height}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    bind_images(cb, layout, dsl, n, v, linear);
    if (pcsize) vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcsize, pc);
    VkViewport vp = {0, 0, (float)target->width, (float)target->height, 0, 1};
    VkRect2D sc = {{0, 0}, {target->width, target->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    return true;
}

// objects' own movement (fg_copy_window, after the HUD mask): this frame's brightness picture, then - with the
// previous real frame's there (the one right before: a gap would match across two steps) - the block search and its
// neighbour filter. LARECOMP_FRAMEGEN_FLOW=1: on (off by default until checked on phones)
static void build_flow(VkCommandBuffer cb) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_FRAMEGEN_FLOW") ? atoi(getenv("LARECOMP_FRAMEGEN_FLOW")) != 0 : 0;
    static uint64_t q_frame = ~0ull;
    g_flow_ok = false;
    NvnTexture *dt = g_depth;
    if (!on || !g_scene.image || !dt || !dt->backend) { g_q_prev = false; return; }
    if (q_frame + 1 != g_vp_frame) g_q_prev = false;
    q_frame = g_vp_frame;
    static VkDescriptorSetLayout dsl1, dsl3;
    static VkPipelineLayout l1, l3;
    static VkPipeline pdown, pflow, pfilt;
    if (!l1) l1 = set_layout(1, 0, &dsl1), l3 = set_layout(3, 96, &dsl3);
    if (!pdown) {
        VkShaderModule a = vk_compile_glsl(kDown, STAGE_FRAGMENT, "framegen_down.frag"),
                       b = vk_compile_glsl(kFlow, STAGE_FRAGMENT, "framegen_flow.frag"),
                       c = vk_compile_glsl(kFilter, STAGE_FRAGMENT, "framegen_filter.frag");
        if (!a || !b || !c) { on = 0; return; }
        pdown = fs_pipeline(a, VK_FORMAT_R8_UNORM, l1);
        pflow = fs_pipeline(b, VK_FORMAT_R16G16B16A16_SFLOAT, l3);
        pfilt = fs_pipeline(c, VK_FORMAT_R16G16B16A16_SFLOAT, l1);
        if (!pdown || !pflow || !pfilt) { on = 0; return; }
        LOG("[framegen] objects' own movement by block matching (LARECOMP_FRAMEGEN_FLOW)\n");
    }
    // (g_scene is a quarter of the presented picture already, unless LARECOMP_FG_SCENEQ=0)
    uint32_t qw = fg_sceneq() ? g_scene.width : (g_scene.width + 3) / 4, qh = fg_sceneq() ? g_scene.height : (g_scene.height + 3) / 4;
    uint32_t fw = (qw + 3) / 4, fh = (qh + 3) / 4;
    VkImageUsageFlags use = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    g_qi ^= 1;
    TexBackend *cur = &g_q[g_qi], *prev = &g_q[g_qi ^ 1];
    if (prev->width != qw || prev->height != qh) g_q_prev = false;
    if (!make_image(cur, VK_FORMAT_R8_UNORM, qw, qh, use) || !make_image(&g_flow_raw, VK_FORMAT_R16G16B16A16_SFLOAT, fw, fh, use) ||
        !make_image(&g_flow, VK_FORMAT_R16G16B16A16_SFLOAT, fw, fh, use)) { g_q_prev = false; return; }
    static const bool lin1[1] = {true}, nearest1[1] = {false}, lin3[3] = {false, false, false};
    VkImageView sv = color_view(&g_scene);
    bool made = run_pass(cb, cur, pdown, l1, dsl1, 1, &sv, lin1, NULL, 0);
    if (made && g_q_prev && g_have_prev) {
        float inv[16], mp[16];
        if (mat_inv(g_vp, inv)) {
            mat_mul(g_vp_prev, inv, mp);
            extern uint32_t nvn_window_origin(void);
            extern uint32_t nvn_depth_mode(void);
            TexBackend *db = dt->backend;
            VkComponentMapping ident = {0};
            struct { float m[16], a[4], b[4]; } pc = {{0}};
            memcpy(pc.m, mp, sizeof mp);
            pc.a[0] = nvn_window_origin() != 0 ? 1.0f : 0.0f;  // (as the warp's flag 1)
            pc.a[1] = nvn_depth_mode() == 0 ? 1.0f : 0.0f;
            pc.b[2] = g_depth_scale[0], pc.b[3] = g_depth_scale[1];
            VkImageView v[3] = {color_view(cur), color_view(prev),
                                vk_image_view(db, VK_IMAGE_VIEW_TYPE_2D, db->format, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_DEPTH_BIT)};
            VkImageView fv = color_view(&g_flow_raw);
            g_flow_ok = run_pass(cb, &g_flow_raw, pflow, l3, dsl3, 3, v, lin3, &pc, sizeof pc) &&
                        run_pass(cb, &g_flow, pfilt, l1, dsl1, 1, &fv, nearest1, NULL, 0);
        }
    }
    g_q_prev = made;
}

// Link's place in the real frame (game uv), the radius around it holding him (uv, horizontal) and the depth buffer's
// value there (1 - d, as the warp compares view distances); nothing when he isn't known (runtime/hle/doorjump.c)
static void link_on_screen(float flags, float uvo[2], float *radius, float *depth) {
    extern bool doorjump_link_pos(float p[3]);
    static int on = -1;  // LARECOMP_FRAMEGEN_LINK=0/1
    if (on < 0) on = getenv("LARECOMP_FRAMEGEN_LINK") ? atoi(getenv("LARECOMP_FRAMEGEN_LINK")) : 1;
    float w[3];
    if (!on || !doorjump_link_pos(w)) return;
    float c[2][4];
    for (int k = 0; k < 2; k++) {  // his middle, and 1.1 to the side of it
        float q[4] = {w[0] + (k ? 1.1f : 0), w[1] + 0.7f, w[2], 1};
        for (int r = 0; r < 4; r++) c[k][r] = g_vp[r * 4] * q[0] + g_vp[r * 4 + 1] * q[1] + g_vp[r * 4 + 2] * q[2] + g_vp[r * 4 + 3] * q[3];
        if (c[k][3] <= 1e-3f) return;
    }
    bool yflip = ((int)flags & 1) != 0, dneg = ((int)flags & 2) != 0;
    float x0 = c[0][0] / c[0][3] * 0.5f + 0.5f, y0 = c[0][1] / c[0][3];
    y0 = yflip ? 0.5f - y0 * 0.5f : y0 * 0.5f + 0.5f;
    float x1 = c[1][0] / c[1][3] * 0.5f + 0.5f;
    float z = c[0][2] / c[0][3];
    if (dneg) z = z * 0.5f + 0.5f;
    if (x0 < -0.2f || x0 > 1.2f || y0 < -0.2f || y0 > 1.2f || z >= 1) return;
    uvo[0] = x0, uvo[1] = y0;
    *radius = fabsf(x1 - x0);
    *depth = 1 - z;
    static int nlog;
    if (getenv("LARECOMP_FRAMEGEN_DEBUG") && nlog++ % 60 == 0)
        LOG("[framegen] Link (%.2f %.2f %.2f) at uv %.3f %.3f r %.3f depth %.5f\n", w[0], w[1], w[2], x0, y0, *radius, z);
}

// In-between frames drawn so far (render thread): ui/perf_overlay.mm's FPS with frame generation
static uint64_t g_generated;
uint64_t fg_generated_frames(void) { return g_generated; }

// device.c, inside the in-between frame's rendering into the swapchain image: draws it. fin: the presented picture;
// rect: where it lies in the window (pixels), crop: the part of it shown; flip_present: lower-left origin
bool fg_draw(VkCommandBuffer cb, VkFormat fmt, uint32_t ww, uint32_t wh, TexBackend *fin, const float rect[4], const float crop[4],
             bool flip_present) {
    extern uint32_t nvn_window_origin(void);
    extern uint32_t nvn_depth_mode(void);
    g_scene_ok = false;
    NvnTexture *dt = g_depth;
    g_depth = NULL;
    extern VkImageView vk_last_present_src(void);
    VkImageView psrc = fg_wincopy() ? VK_NULL_HANDLE : vk_last_present_src();
    if (!g_comp_ok || !fin || !fin->image || !dt || !dt->backend || (!fg_wincopy() && !psrc)) return false;
    TexBackend *db = dt->backend;
    VkPipeline pipe = warp_pipeline(fmt);
    if (!pipe) return false;
    // generated camera: half a step on (rows); M = VPg x inverse(VP)
    float vpg[16], inv[16], m[16];
    static float step = -1;  // LARECOMP_FRAMEGEN_T=<t>: how far on (tests: 1 = where the next real frame should be)
    if (step < 0) step = getenv("LARECOMP_FRAMEGEN_T") ? (float)atof(getenv("LARECOMP_FRAMEGEN_T")) : 0.5f;
    for (int i = 0; i < 16; i++) vpg[i] = g_vp[i] + step * (g_vp[i] - g_vp_prev[i]);
    if (!mat_inv(g_vp, inv)) return false;
    mat_mul(vpg, inv, m);
    VkComponentMapping ident = {0};
    VkImageView v[6] = {psrc ? psrc : color_view(&g_comp), color_view(&g_scene), color_view(fin),
                        vk_image_view(db, VK_IMAGE_VIEW_TYPE_2D, db->format, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_DEPTH_BIT), color_view(&g_mask),
                        g_flow_ok ? color_view(&g_flow) : color_view(&g_mask)};  // (no movement: a stand-in, unread)
    for (int i = 0; i < 6; i++) if (!v[i]) return false;
    struct { float m[16], rect[4], crop[4], a[4], b[4]; } pc;
    memcpy(pc.m, m, sizeof m);
    pc.rect[0] = rect[0] / ww, pc.rect[1] = rect[1] / wh, pc.rect[2] = rect[2] / ww, pc.rect[3] = rect[3] / wh;
    memcpy(pc.crop, crop, sizeof pc.crop);
    // (flag 1: the game's lower-left origin is drawn flipped and presented flipped back, so in the presented picture's and
    // the depth's own layout NDC up is the image's bottom row - no y flip. With it the in-between frame moved the
    // scenery backwards when the camera moved vertically: -5 / +17 pixels per shown frame instead of +6 / +6, S20 FE)
    bool srgb = fin->format == VK_FORMAT_R8G8B8A8_SRGB || fin->format == VK_FORMAT_B8G8R8A8_SRGB;
    pc.a[0] = (float)((nvn_window_origin() != 0 ? 1 : 0) | (nvn_depth_mode() == 0 ? 2 : 0) | (flip_present ? 4 : 0) | (psrc ? 8 : 0) |
                      (srgb ? 16 : 0) | (g_flow_ok ? 32 : 0));
    pc.a[1] = 0, pc.a[2] = 0, pc.b[0] = pc.b[1] = 0;
    link_on_screen(pc.a[0], pc.b, &pc.a[1], &pc.a[2]);
    // a camera that didn't move: the real picture again, without the per-pixel reprojection
    float still = 0;
    for (int i = 0; i < 16; i++) still = fmaxf(still, fabsf(g_vp[i] - g_vp_prev[i]) / (fabsf(g_vp[i]) + 1e-3f));
    const char *dbg = getenv("LARECOMP_FRAMEGEN_DEBUG");
    // (LARECOMP_FRAMEGEN_DEBUG=depth / link / diff: the picture against the scene before the HUD / mask: the HUD's /
    // scene: the scene before the HUD)
    static const char *const modes[] = {"link", "diff", "mask", "scene"};
    pc.a[3] = dbg ? 2.0f : getenv("LARECOMP_FRAMEGEN_PLAIN") || still < 1e-5f ? 1.0f : 0.0f;
    for (int i = 0; dbg && i < 4; i++) if (!strcmp(dbg, modes[i])) pc.a[3] = 3.0f + i;
    static int nlog;
    if (getenv("LARECOMP_FRAMEGEN_DEBUG") && nlog++ < 3)
        LOG("[framegen] depth %ux%u scale %.3f %.3f, picture %ux%u, rect %.0f %.0f %.0f %.0f crop %.3f %.3f %.3f %.3f\n", db->width, db->height,
            g_depth_scale[0], g_depth_scale[1], fin->width, fin->height, rect[0], rect[1], rect[2], rect[3], crop[0], crop[1], crop[2], crop[3]);
    pc.b[2] = g_depth_scale[0], pc.b[3] = g_depth_scale[1];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    static const bool lin[6] = {true, true, true, false, true, false};
    bind_images(cb, g_layout, g_dsl, 6, v, lin);
    vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport vp = {0, 0, (float)ww, (float)wh, 0, 1};
    VkRect2D sc = {{0, 0}, {ww, wh}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    g_generated++;
    return true;
}
#else
typedef int framegen_unused;  // (Android only)
#endif
