// Native game code (Android, experimental): the game's own ARM64 code runs on the phone's CPU instead of the game code
// built at setup (lasgame.so) - no build of it. From BOTWRecompAndroid (runtime/native.c), after the approach of
// MacNativeGames and d2r-mobile's patch_nso.py.
//
// The modules are loaded and relocated at their guest addresses as always (loader.c); guest addresses are host
// addresses, so the game's code can run there as it is. Only what can't run outside the Switch is rewritten, in place:
//   svc #n              -> a stub (in a patch area after the module) that calls native_svc_gate: the registers go into
//                          the thread's Cpu, rt_svc runs on the host stack, the registers come back
//   mrs/msr tpidr_el0   -> the guest's TPIDR_EL0 in its Cpu (bionic's TLS stays in the real register)
//   mrs tpidrro_el0     -> the guest's TPIDRRO_EL0 (its ThreadLocalRegion) from its Cpu
//   mrs cntpct_el0      -> the virtual counter (readable by apps): as it is where it runs at the Switch's 19.2 MHz
//                          (Snapdragon), scaled to it elsewhere (MediaTek, Exynos, Tensor); mrs cntfrq_el0 -> 19.2 MHz
//   tools/patches.py    -> its fixes and hooks at the same addresses (widescreen, frame pacing, the randomizer, the
//                          crash fixes), as stubs into native_patch_gate: the same C, on the Cpu
// x18 is left alone: the game uses it as an ordinary register, and Linux keeps it across context switches; the gates
// give the host its own x18 back while C runs (bionic's shadow call stack lives there).
// Host functions (HLE: NVN, rtld's callbacks, imports) keep their fake addresses at HLE_BASE, now real code: each
// 16-byte slot jumps to native_hle_gate with its index. A stub finds the thread's Cpu from TPIDR_EL0 through
// g_native_tab (native_gate.S). Everything the runtime does with a Cpu (kernel.c, services, NVN) is unchanged.
#include "rt.h"

#if defined(__aarch64__) && defined(__ANDROID__)
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "game_addresses.h"
#include "hle.h"
#include "kernel.h"
#include "settings.h"

typedef struct { uint64_t tp; Cpu *cpu; } NativeTabEnt;
// Open addressing from the hashed slot (native_gate.S: the same hash; up to 64 probes, past the end: no wrap). tp 0:
// never used, 1: a thread that ended (probing goes on past it).
#define TAB_N 4096
#define TAB_PROBES 64
__attribute__((visibility("hidden"), aligned(64))) NativeTabEnt g_native_tab[TAB_N + TAB_PROBES];
static pthread_mutex_t tab_lock = PTHREAD_MUTEX_INITIALIZER;

void native_enter(Cpu *c, uint64_t addr);  // native_gate.S
extern char native_svc_gate[], native_hle_gate[], native_patch_gate[];

// Chosen per boot (main.c boot): the setting (Native game code), or LARECOMP_NATIVE=0/1 over it
static bool g_on;
bool native_wanted(void) {
    const char *e = getenv("LARECOMP_NATIVE");
    return e ? atoi(e) != 0 : g_settings.native_code;
}
void native_begin_boot(void) { g_on = native_wanted(); }
bool native_on(void) { return g_on; }

static uint64_t tp_now(void) { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static unsigned tab_idx(uint64_t tp) { return (unsigned)(((tp ^ (tp >> 12)) >> 12) & 4095); }

// The calling host thread runs guest code with this Cpu (its slot: the thread's TPIDR_EL0)
static NativeTabEnt *tab_find(uint64_t tp) {
    NativeTabEnt *e = &g_native_tab[tab_idx(tp)];
    for (int k = 0; k < TAB_PROBES && e[k].tp; k++)
        if (e[k].tp == tp) return &e[k];
    return NULL;
}
// The thread's slot now holds c; returns the Cpu it held (NULL: none)
static Cpu *native_bind(Cpu *c) {
    uint64_t tp = tp_now();
    NativeTabEnt *e = tab_find(tp);
    if (e && e->cpu == c) return c;
    Cpu *was = e ? e->cpu : NULL;
    pthread_mutex_lock(&tab_lock);
    if (!e) {
        NativeTabEnt *s = &g_native_tab[tab_idx(tp)];
        for (int k = 0; k < TAB_PROBES && !e; k++)
            if (s[k].tp <= 1) e = &s[k];  // (never used, or a thread that ended)
        if (!e) FATAL("[native] no free slot for a thread (%d probes)\n", TAB_PROBES);
    }
    __atomic_store_n(&e->cpu, c, __ATOMIC_RELEASE);
    __atomic_store_n(&e->tp, tp, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&tab_lock);
    return was;
}
void native_thread_end(void) {
    if (!native_on()) return;
    pthread_mutex_lock(&tab_lock);
    NativeTabEnt *e = tab_find(tp_now());
    if (e) __atomic_store_n(&e->tp, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&tab_lock);
}

// guest_call's native path. x16 / x17 come back as they were too: a gate takes the guest's from the Cpu after its C,
// and a call into the game from there (a fix's handler) would leave that code's. A call with another Cpu than the
// thread's (a copy) gives the slot back to the thread's own afterwards.
void native_call(Cpu *c, uint64_t addr) {
    Cpu *was = native_bind(c);
    uint64_t lr = c->x[30], x16 = c->x[16], x17 = c->x[17];
    native_enter(c, addr);
    c->x[30] = lr, c->x[16] = x16, c->x[17] = x17;
    if (was && was != c) native_bind(was);
}

// The host's x18 back on a thread that runs the game's code (a signal arrives with the game's in it; native_gate.S
// keeps the host's in the Cpu)
void native_host_x18(void) {
    NativeTabEnt *e = tab_find(tp_now());
    if (!e || !e->cpu) return;
    uint64_t v;
    memcpy(&v, (const char *)e->cpu + 344, 8);  // (native_gate.S C_HOST_X18)
    if (v) __asm__ volatile("mov x18, %0" ::"r"(v));  // (x18 is reserved: the compiler never uses it)
}

// ---- the gates' C side (on the host stack; c holds the guest's registers)
void native_svc_c(Cpu *c, uint32_t imm) { rt_svc(c, imm, c->x[30]); }

HostFn hle_slot_fn(uint64_t pc);  // dispatch.c
void native_hle_c(Cpu *c, uint32_t i) {
    rt_unwind_point();
    HostFn f = hle_slot_fn(HLE_BASE + 16ull * i);
    f(c);
}

// ---- the counter: the Switch's ticks are 19.2 MHz. Where the phone's isn't, a read is cntvct * g_tick_mul >> 32.
static uint64_t g_tick_mul;  // 0: the phone's counter is 19.2 MHz
uint64_t native_ticks(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return g_tick_mul ? (uint64_t)(((__uint128_t)v * g_tick_mul) >> 32) : v;
}

// ---- the rewriter
static uint32_t enc_b(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)(to - from);
    if (d & 3 || d < -(1ll << 27) || d >= (1ll << 27))
        FATAL("[native] branch %llx -> %llx out of range\n", (unsigned long long)from, (unsigned long long)to);
    return 0x14000000u | ((uint32_t)(d >> 2) & 0x3FFFFFFu);
}
static uint32_t enc_ldr_lit(int rt, uint64_t from, uint64_t to) { return 0x58000000u | (((uint32_t)((to - from) >> 2) & 0x7FFFF) << 5) | rt; }
static uint32_t enc_stp_pre(int t1, int t2, int n, int off) { return 0xA9800000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1; }
static uint32_t enc_ldp_post(int t1, int t2, int n, int off) { return 0xA8C00000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1; }
static uint32_t enc_stp(int t1, int t2, int n, int off) { return 0xA9000000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1; }
static uint32_t enc_ldp(int t1, int t2, int n, int off) { return 0xA9400000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1; }
static uint32_t enc_str(int t, int n, int off) { return 0xF9000000u | ((uint32_t)(off / 8) << 10) | (n << 5) | t; }
static uint32_t enc_ldr(int t, int n, int off) { return 0xF9400000u | ((uint32_t)(off / 8) << 10) | (n << 5) | t; }
#define SP 31
#define MRS_TPIDR 0xD53BD040u
#define MRS_TPIDRRO 0xD53BD060u
#define MSR_TPIDR 0xD51BD040u
#define MRS_CNTFRQ 0xD53BE000u
#define MRS_CNTPCT 0xD53BE020u
#define MRS_CNTVCT 0xD53BE040u
#define BRK(n) (0xD4200000u | ((n) << 5))

// Two or three temporaries for a stub, none of them xN
static void temps(int n, int *t, int k) {
    for (int r = 14, i = 0; r <= 17 && i < k; r++)
        if (r != n) t[i++] = r;
}

// One stub per site, 80 bytes (16-aligned: the literal at +40 / +72). The svc's frame: [sp] x16, x17, [sp + 16] x30,
// [sp + 24] the svc number, [sp + 32] free (native_gate.S GATE_IN).
static uint64_t stub_svc(uint32_t *s, uint64_t at, uint64_t site, uint32_t imm) {
    s[0] = 0xD100C3FFu;                    // sub sp, sp, #48
    s[1] = enc_stp(16, 17, SP, 0);         // stp x16, x17, [sp]
    s[2] = enc_str(30, SP, 16);            // str x30, [sp, #16]
    s[3] = 0xD2800011u | (imm << 5);       // movz x17, #imm
    s[4] = enc_ldr_lit(16, at + 16, at + 40);
    s[5] = 0xD63F0200u;                    // blr x16
    s[6] = enc_ldp(16, 17, SP, 0);         // ldp x16, x17, [sp]
    s[7] = enc_ldr(30, SP, 16);            // ldr x30, [sp, #16]
    s[8] = 0x9100C3FFu;                    // add sp, sp, #48
    s[9] = enc_b(at + 36, site + 4);
    memcpy(&s[10], &(uint64_t){(uint64_t)(uintptr_t)native_svc_gate}, 8);
    return 80;
}
// mrs xN, tpidr_el0 / tpidrro_el0 (load) or msr tpidr_el0, xN (store): xN <-> [Cpu + off], the Cpu from g_native_tab
static uint64_t stub_tls(uint32_t *s, uint64_t at, uint64_t site, int n, int off, bool store) {
    int t[3];
    temps(n, t, 3);
    int a = t[0], b = t[1], c = t[2];
    s[0] = enc_stp_pre(a, b, SP, -32);
    s[1] = enc_str(c, SP, 16);
    s[2] = MRS_TPIDR | a;                                        // mrs xa, tpidr_el0 (the host's: the key)
    s[3] = 0xCA400000u | (a << 16) | (12 << 10) | (a << 5) | b;  // eor xb, xa, xa, lsr #12
    s[4] = 0xD3400000u | (12 << 16) | (23 << 10) | (b << 5) | b;  // ubfx xb, xb, #12, #12
    s[5] = enc_ldr_lit(c, at + 20, at + 72);                    // ldr xc, =g_native_tab
    s[6] = 0x8B000000u | (b << 16) | (4 << 10) | (c << 5) | b;   // add xb, xc, xb, lsl #4
    s[7] = enc_ldr(c, b, 0);                                    // 1: ldr xc, [xb] (the slot's thread)
    s[8] = 0xCA000000u | (a << 16) | (c << 5) | c;              // eor xc, xc, xa
    s[9] = 0xB4000000u | (3 << 5) | c;                          // cbz xc, 2f (+12)
    s[10] = 0x91004000u | (b << 5) | b;                         // add xb, xb, #16
    s[11] = 0x17FFFFFCu;                                        // b 1b (-16)
    s[12] = enc_ldr(a, b, 8);                                   // 2: ldr xa, [xb, #8] (the Cpu)
    s[13] = store ? enc_str(n, a, off) : enc_ldr(n, a, off);
    s[14] = enc_ldr(c, SP, 16);
    s[15] = enc_ldp_post(a, b, SP, 32);
    s[16] = enc_b(at + 64, site + 4);
    s[17] = 0xD503201Fu;                                        // nop (the literal 8-aligned)
    memcpy(&s[18], &(uint64_t){(uint64_t)(uintptr_t)g_native_tab}, 8);
    return 80;
}
// mrs xN, cntpct_el0 on a counter that isn't 19.2 MHz: xN = cntvct * g_tick_mul >> 32 (native_ticks), 48 bytes
static uint64_t stub_ticks(uint32_t *s, uint64_t at, uint64_t site, int n) {
    int t[2];
    temps(n, t, 2);
    int a = t[0], b = t[1];
    s[0] = enc_stp_pre(a, b, SP, -16);
    s[1] = MRS_CNTVCT | a;
    s[2] = enc_ldr_lit(b, at + 8, at + 32);                     // ldr xb, =g_tick_mul
    s[3] = 0x9B007C00u | (b << 16) | (a << 5) | n;              // mul xn, xa, xb (the low half)
    s[4] = 0x9BC07C00u | (b << 16) | (a << 5) | a;              // umulh xa, xa, xb (the high half)
    s[5] = 0x93C00000u | (n << 16) | (32 << 10) | (a << 5) | n;  // extr xn, xa, xn, #32
    s[6] = enc_ldp_post(a, b, SP, 16);
    s[7] = enc_b(at + 28, site + 4);
    memcpy(&s[8], &g_tick_mul, 8);
    s[10] = s[11] = 0xD503201Fu;
    return 48;
}
// mrs xN, cntfrq_el0 there: xN = 19.2 MHz, the frequency of the ticks the game reads (16 bytes)
static uint64_t stub_freq(uint32_t *s, uint64_t at, uint64_t site, int n) {
    s[0] = 0xD2800000u | (0xF800u << 5) | n;                    // movz xn, #0xf800
    s[1] = 0xF2A00000u | (0x124u << 5) | n;                     // movk xn, #0x124, lsl #16 (19,200,000)
    s[2] = enc_b(at + 8, site + 4);
    s[3] = 0xD503201Fu;
    return 16;
}

// ---- the game's fixes and hooks: tools/patches.py's, at the same addresses (there they are C in place of one
// instruction of the generated code). A stub calls native_patch_gate with the fix's number; its handler works on the
// Cpu - the same calls as patches.py's C - and says how to go on:
#define ORIG 0  // run the original instruction (from the stub) and go on after it
#define ALT 1   // the fix's other address (patches.py's goto)
#define RET 2   // return from the function (x30 as the handler left it)
#define ON 3    // go on after the instruction without running it
typedef struct Fix Fix;
typedef int (*FixFn)(Cpu *c, const Fix *f, uint32_t w);
struct Fix {
    size_t site, alt;     // GA_FIELD()s (alt NO_ALT: none)
    uint32_t word, mask;  // the original instruction, checked before it's replaced (word & mask)
    FixFn fn;
    int arg;              // the handler's own (a site id, a register, an offset)
    const char *what;
};
#define NO_ALT ((size_t)-1)

float rt_aspect_site(int site);
float rt_cull_aspect(float orig);
float rt_skysea_margin(void);
float rt_fovy(float orig);
float rt_indoor_aspect(void);
void rt_warp_request_obj(uint64_t r);
int rt_crashout_skip(void);
uint32_t rt_gohma_heading(uint64_t gohma, uint32_t track);
int rt_gohma_free(void);
void rt_level_request(Cpu *c);
int rt_adv_exit_choice(void);
int32_t rt_damage(int32_t dmg, int32_t shift);
int rt_hero_mode(void);
int rt_hide_item_explanation(void);
int rt_add_item(Cpu *c);
void rt_add_prize(Cpu *c);
void rt_items_page_tick(void);
void rt_map_page_tick(Cpu *c);
void rt_dgnmap_tick(Cpu *c);
void rt_log_resname(uint64_t p);
void rt_mb_seen(uint64_t o);
uint32_t rt_frame_steps(uint32_t interval);
void rt_dig_hole(Cpu *c);
uint64_t rt_hole_cap(void);
int rt_bottle3_use(void);
uint64_t rt_actor_create_fn(uint64_t fn, uint32_t type, uint64_t info);
uint32_t rt_hookshot_attr(Cpu *c, uint32_t attr);
int rt_snap_dir(Cpu *c);
int rt_buff_replace(int allow);
void rt_tilt_obj(uint64_t obj);
int rt_tunic_upgrades(void);
int rt_underwater_items(void);
int rt_underwater_skip_owned(uint64_t actor);
float rt_prompt_x(uint64_t pane, float x);
void rt_physx_no_sim(uint64_t shape, uint32_t flags, uint64_t fp, uint64_t sp);
void rt_empty_handler(void);
int rt_delegate_null(Cpu *c, int slot);
void rt_frame_job_kickoff(uint64_t owner);
void rt_frame_job_finish(uint64_t job);
void rt_owner_call_notify(Cpu *c, uint64_t obj);

static float vf(const Cpu *c, int r) { float f; memcpy(&f, &c->v[r], 4); return f; }
static void set_vf(Cpu *c, int r, float f) { c->v[r] = 0; memcpy(&c->v[r], &f, 4); }  // (a scalar write clears the rest)
static float ldf(uint64_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }

static int fx_aspect(Cpu *c, const Fix *f, uint32_t w) { set_vf(c, w & 31, rt_aspect_site(f->arg)); return ON; }
static int fx_cull(Cpu *c, const Fix *f, uint32_t w) { set_vf(c, 9, rt_cull_aspect(ldf(c->x[27] + 0x570))); return ON; }
static int fx_fovy(Cpu *c, const Fix *f, uint32_t w) { set_vf(c, w & 31, rt_fovy(ldf(c->x[f->arg] + 0x564))); return ON; }
static int fx_skysea(Cpu *c, const Fix *f, uint32_t w) {  // arg: the box bound's offset (0x2e0 the max, 0x2d0 the min)
    float b[4];
    memcpy(b, (const void *)(uintptr_t)(c->x[23] + f->arg), 16);
    b[0] += (f->arg == 0x2e0 ? 1.0f : -1.0f) * rt_skysea_margin();
    memcpy(&c->v[w & 31], b, 16);
    return ON;
}
static int fx_indoor(Cpu *c, const Fix *f, uint32_t w) { set_vf(c, w & 31, rt_indoor_aspect()); return ON; }
static int fx_warp_request(Cpu *c, const Fix *f, uint32_t w) { rt_warp_request_obj(c->x[f->arg]); return ORIG; }
static int fx_crashout(Cpu *c, const Fix *f, uint32_t w) {
    ST32(c->x[0] + 0xb8, 0);
    return rt_crashout_skip() ? ALT : ON;
}
static int fx_gohma_heading(Cpu *c, const Fix *f, uint32_t w) {
    c->x[8] = rt_gohma_heading(c->x[19], (uint32_t)c->x[8]);
    return ORIG;
}
static int fx_gohma_arrived(Cpu *c, const Fix *f, uint32_t w) {
    if (!rt_gohma_free()) return ORIG;
    c->x[0] = 0;
    return ALT;
}
static int fx_mstalfos_body(Cpu *c, const Fix *f, uint32_t w) { c->x[8] = rt_gohma_free() ? 0 : 1; return ON; }
static int fx_mstalfos_jump(Cpu *c, const Fix *f, uint32_t w) {
    float a = vf(c, 0), b = vf(c, 1);
    if (rt_gohma_free()) a += ldf(LD64(c->x[19] + 0x1160) + 0x28c);
    set_vf(c, 0, a - b);
    return ON;
}
static int fx_mchief_story(Cpu *c, const Fix *f, uint32_t w) { return rt_gohma_free() || ((c->x[8] >> 7) & 1) ? ALT : ON; }
static int fx_level_request(Cpu *c, const Fix *f, uint32_t w) { rt_level_request(c); return ORIG; }
static int fx_adv_exit_dialog(Cpu *c, const Fix *f, uint32_t w) { return rt_adv_exit_choice() ? ON : ORIG; }
static int fx_damage_lsl(Cpu *c, const Fix *f, uint32_t w) {
    int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    c->x[rd] = (uint32_t)rt_damage((int32_t)c->x[rn], (int32_t)(c->x[rm] & 31));
    return ON;
}
static int fx_damage_fixed(Cpu *c, const Fix *f, uint32_t w) { c->x[20] = (uint32_t)(-rt_damage(2, rt_hero_mode())); return ON; }
static int fx_item_explanation(Cpu *c, const Fix *f, uint32_t w) {
    c->x[22] = rt_hide_item_explanation() ? 0 : LD64(c->sp + 0xf0);
    return ON;
}
static int fx_item_name(Cpu *c, const Fix *f, uint32_t w) { return rt_hide_item_explanation() ? ON : ORIG; }
static int fx_add_item(Cpu *c, const Fix *f, uint32_t w) { return rt_add_item(c) ? RET : ORIG; }
static int fx_prize_add(Cpu *c, const Fix *f, uint32_t w) {
    c->x[2] = (uint32_t)c->x[20];
    rt_add_prize(c);
    return ON;
}
static int fx_items_page(Cpu *c, const Fix *f, uint32_t w) { rt_items_page_tick(); return ORIG; }
static int fx_map_page(Cpu *c, const Fix *f, uint32_t w) { rt_map_page_tick(c); return ORIG; }
static int fx_dgn_map(Cpu *c, const Fix *f, uint32_t w) { rt_dgnmap_tick(c); return ORIG; }
static int fx_res_name(Cpu *c, const Fix *f, uint32_t w) { rt_log_resname(c->x[2]); return ORIG; }
static int fx_mb_seen(Cpu *c, const Fix *f, uint32_t w) { rt_mb_seen(c->x[21]); return ORIG; }
static int fx_frame_steps(Cpu *c, const Fix *f, uint32_t w) { c->x[8] = rt_frame_steps(LD32(c->x[8] + c->x[9])); return ON; }
static int fx_dig_hole(Cpu *c, const Fix *f, uint32_t w) { rt_dig_hole(c); return ORIG; }
static int fx_hole_cap(Cpu *c, const Fix *f, uint32_t w) {  // arg: what the original compares with less the cap (0 / 1)
    (void)subs64(c, c->x[8], rt_hole_cap() - (uint64_t)f->arg);
    return ON;
}
static int fx_bottle3_use(Cpu *c, const Fix *f, uint32_t w) {
    c->x[0] = COND_NE(c) ? 0 : 1;
    if (c->x[0] && rt_bottle3_use()) c->x[0] = 0;
    return ON;
}
static int fx_boss_spawn(Cpu *c, const Fix *f, uint32_t w) {
    c->x[24] = rt_actor_create_fn(LD64(c->x[8]), (uint32_t)c->x[23], c->x[2]);
    return ON;
}
static int fx_hookshot_attr(Cpu *c, const Fix *f, uint32_t w) { c->x[23] = rt_hookshot_attr(c, LD32(c->sp + 0x38)); return ON; }
static int fx_snap_dir(Cpu *c, const Fix *f, uint32_t w) { return rt_snap_dir(c) ? RET : ORIG; }
static int fx_buff_replace(Cpu *c, const Fix *f, uint32_t w) { c->x[8] = (uint32_t)rt_buff_replace(LD8(c->x[24] + 0x38)); return ON; }
static int fx_tilt_init(Cpu *c, const Fix *f, uint32_t w) { rt_tilt_obj(c->x[0]); return ORIG; }
static int fx_missing_string(Cpu *c, const Fix *f, uint32_t w) {  // "" (callers strlen() it), no throw
    ST64(c->sp + 0x18, GA(EMPTY_STRING));
    ST32(c->sp + 0x20, 0);
    return ALT;
}
static int fx_missing_float(Cpu *c, const Fix *f, uint32_t w) {  // 0.0, then the function's own epilogue
    c->v[0] = 0;
    c->x[29] = LD64(c->sp + 48), c->x[30] = LD64(c->sp + 56);
    c->x[20] = LD64(c->sp + 32), c->x[19] = LD64(c->sp + 40);
    c->sp += 0x40;
    return RET;
}
static int fx_tunic_clear(Cpu *c, const Fix *f, uint32_t w) { return rt_tunic_upgrades() ? ON : ORIG; }
static int fx_underwater_owned(Cpu *c, const Fix *f, uint32_t w) {
    if (!rt_underwater_skip_owned(c->x[20])) return ORIG;
    c->x[0] = 0;
    return ON;
}
static int fx_underwater_view(Cpu *c, const Fix *f, uint32_t w) {
    c->x[27] = (rt_underwater_items() || COND_NE(c)) ? 1u : 0u;
    return ON;
}
static int fx_underwater_pickup(Cpu *c, const Fix *f, uint32_t w) { return rt_underwater_skip_owned(c->x[19]) ? ON : ORIG; }
// Event actions with no handler (patches.py NO_HANDLER_SITES): the throw goes straight into its catch block, whose
// begin / end catch are skipped then - only then: here the game's own throws can reach that catch too.
static __thread bool t_no_handler;
static int fx_no_handler(Cpu *c, const Fix *f, uint32_t w) {
    if (f->arg == 0) { t_no_handler = true; return ALT; }
    if (!t_no_handler) return ORIG;
    if (f->arg == 2) t_no_handler = false;
    return ON;
}
static int fx_prompt_pos(Cpu *c, const Fix *f, uint32_t w) {
    float px = rt_prompt_x(c->x[8], vf(c, 2)), py = vf(c, 0);
    memcpy((void *)(uintptr_t)(c->x[8] + 0x30), &px, 4);
    memcpy((void *)(uintptr_t)(c->x[8] + 0x34), &py, 4);
    return ON;
}
static int fx_shape_sim(Cpu *c, const Fix *f, uint32_t w) {  // arg: the register the original sets (x20 / x1)
    if (c->x[0] != 1) return ORIG;
    rt_physx_no_sim(c->x[1], f->arg == 20 ? LD32(c->x[19]) : 0u, c->x[29], c->sp);
    return ALT;
}
static int fx_empty_handler(Cpu *c, const Fix *f, uint32_t w) {
    c->x[1] = c->sp + 0x18;
    if (c->x[0]) return ON;
    rt_empty_handler();
    return ALT;
}
static int fx_delegate_forwarder(Cpu *c, const Fix *f, uint32_t w) { return rt_delegate_null(c, f->arg) ? RET : ORIG; }
static int fx_frame_job_kickoff(Cpu *c, const Fix *f, uint32_t w) { rt_frame_job_kickoff(c->x[f->arg]); return ORIG; }
static int fx_frame_job_finish(Cpu *c, const Fix *f, uint32_t w) { rt_frame_job_finish(c->x[0]); return ORIG; }
static int fx_owner_call_notify(Cpu *c, const Fix *f, uint32_t w) {
    rt_owner_call_notify(c, c->x[21]);
    c->x[0] = c->x[21] + 0x38;
    return ON;
}

#define F(site, alt, ...) {GA_FIELD(site), alt, __VA_ARGS__}  // (word, mask, fn, arg, what)
#define A(name) GA_FIELD(name)
#define ANY 0xFFFFFFFFu
#define LDR_BA4 0xBD4BA400u, 0xFFFFFC00u  // ldr sN, [xM, #0xba4]
static const Fix fixes[] = {
    F(ASPECT_SITES_0, NO_ALT, LDR_BA4, fx_aspect, 0, "aspect"),
    F(ASPECT_SITES_1, NO_ALT, LDR_BA4, fx_aspect, 1, "aspect"),
    F(ASPECT_SITES_2, NO_ALT, LDR_BA4, fx_aspect, 2, "aspect"),
    F(ASPECT_SITES_3, NO_ALT, LDR_BA4, fx_aspect, 3, "aspect"),
    F(ASPECT_SITES_4, NO_ALT, LDR_BA4, fx_aspect, 4, "aspect"),
    F(ASPECT_SITES_5, NO_ALT, LDR_BA4, fx_aspect, 5, "aspect"),
    F(CULL_ASPECT, NO_ALT, 0xbd457369, ANY, fx_cull, 0, "culling aspect"),
    F(FOVY_SITES_0, NO_ALT, 0xbd4566e0, ANY, fx_fovy, 23, "fovy"),
    F(FOVY_SITES_1, NO_ALT, 0xbd456768, ANY, fx_fovy, 27, "fovy"),
    F(SKYSEA_BOX_MAX_SITE, NO_ALT, 0x3dc0bae1, ANY, fx_skysea, 0x2e0, "sky / sea box"),
    F(SKYSEA_BOX_MIN_SITE, NO_ALT, 0x3dc0b6e2, ANY, fx_skysea, 0x2d0, "sky / sea box"),
    F(INDOOR_SITE, NO_ALT, LDR_BA4, fx_indoor, 0, "indoor shadow"),
    F(WARP_REQUEST_SITES_0, NO_ALT, 0xf900e100 | 20, ANY, fx_warp_request, 20, "warp request"),
    F(WARP_REQUEST_SITES_1, NO_ALT, 0xf900e100 | 19, ANY, fx_warp_request, 19, "warp request"),
    F(CRASHOUT_SITE, A(CRASHOUT_SKIP), 0xb900b81f, ANY, fx_crashout, 0, "Blaino knock-out"),
    F(GOHMA_HEADING_SITES_0, NO_ALT, 0xb915ee68, ANY, fx_gohma_heading, 0, "Gohma heading"),
    F(GOHMA_HEADING_SITES_1, NO_ALT, 0xb915ee68, ANY, fx_gohma_heading, 0, "Gohma heading"),
    F(GOHMA_ARRIVED_SITE, A(GOHMA_ARRIVED_SKIP), 0x3dc58260, ANY, fx_gohma_arrived, 0, "Gohma track end"),
    F(MSTALFOS_BODY_SITE, NO_ALT, 0x320003e8, ANY, fx_mstalfos_body, 0, "Master Stalfos body"),
    F(MSTALFOS_JUMP_SITE, NO_ALT, 0x1e213800, ANY, fx_mstalfos_jump, 0, "Master Stalfos jump"),
    F(MCHIEF_STORY_SITE, A(MCHIEF_STORY_SKIP), 0x37380148, ANY, fx_mchief_story, 0, "Moblin Chief story"),
    F(LEVEL_REQUEST_SITE, NO_ALT, 0xd10203ff, ANY, fx_level_request, 0, "level request"),
    F(ADV_EXIT_DIALOG_SITE, NO_ALT, 0x94000000, 0xFC000000u, fx_adv_exit_dialog, 0, "chamber exit prompt"),
    F(DAMAGE_LSL_SITES_0, NO_ALT, 0x1ac02000, 0xffe0fc00, fx_damage_lsl, 0, "damage"),
    F(DAMAGE_LSL_SITES_1, NO_ALT, 0x1ac02000, 0xffe0fc00, fx_damage_lsl, 0, "damage"),
    F(DAMAGE_LSL_SITES_2, NO_ALT, 0x1ac02000, 0xffe0fc00, fx_damage_lsl, 0, "damage"),
    F(DAMAGE_FIXED_SITE, NO_ALT, 0x1a800014, 0xffe00c1f, fx_damage_fixed, 0, "fixed damage"),
    F(ITEM_EXPLANATION_SITE, NO_ALT, 0xf9407bf6, ANY, fx_item_explanation, 0, "item explanation"),
    F(ITEM_NAME_SITE, NO_ALT, 0x94035896, ANY, fx_item_name, 0, "item name"),
    F(ADD_ITEM_SITE, NO_ALT, 0xd10143ff, ANY, fx_add_item, 0, "AddItem"),
    F(PRIZE_ADD_SITE, NO_ALT, 0x2a1403e2, ANY, fx_prize_add, 0, "AddPrize"),
    F(ITEMS_PAGE_SITE, NO_ALT, 0xd10243ff, ANY, fx_items_page, 0, "items page"),
    F(MAP_PAGE_SITE, NO_ALT, 0xd10203ff, ANY, fx_map_page, 0, "map page"),
    F(DGN_MAP_SITE, NO_ALT, 0xd10243ff, ANY, fx_dgn_map, 0, "dungeon map page"),
    F(RES_NAME_SITE, NO_ALT, 0xd10503ff, ANY, fx_res_name, 0, "resource by name"),
    F(MB_SEEN_SITE, NO_ALT, 0xf94002a8, ANY, fx_mb_seen, 0, "actor seen"),
    F(FRAME_STEPS_SITE, NO_ALT, 0xb8696908, ANY, fx_frame_steps, 0, "frame steps"),
    F(DIG_HOLE_SITE, NO_ALT, 0xf94007f6, ANY, fx_dig_hole, 0, "hole dug"),
    F(HOLE_CAP_SITES_0, NO_ALT, 0xf101451f, ANY, fx_hole_cap, 0, "dug-hole limit"),
    F(HOLE_CAP_SITES_1, NO_ALT, 0xf101411f, ANY, fx_hole_cap, 1, "dug-hole limit"),
    F(BOTTLE3_USE_SITE, NO_ALT, 0x1a9f17e0, ANY, fx_bottle3_use, 0, "third bottle"),
    F(BOSS_SPAWN_SITE, NO_ALT, 0xf9400118, ANY, fx_boss_spawn, 0, "actor create"),
    F(HOOKSHOT_ATTR_SITE, NO_ALT, 0xb9403bf7, ANY, fx_hookshot_attr, 0, "hookshot hit"),
    F(SNAP_DIR_SITE, NO_ALT, 0xd10103ff, ANY, fx_snap_dir, 0, "8-way stick snap"),
    F(BUFF_REPLACE_SITE, NO_ALT, 0x3940e308, ANY, fx_buff_replace, 0, "buff drops"),
    F(TILT_INIT_SITE, NO_ALT, 0xd10243ff, ANY, fx_tilt_init, 0, "tilt-shift set-up"),
    F(MISSING_STRING_SITE, A(MISSING_STRING_RETURN), 0x321c03e0, ANY, fx_missing_string, 0, "missing String parameter"),
    F(MISSING_FLOAT_SITE, NO_ALT, 0x321c03e0, ANY, fx_missing_float, 0, "missing Float parameter"),
    F(TUNIC_CLEAR_SITES_0, NO_ALT, 0x12197929, ANY, fx_tunic_clear, 0, "tunic clear"),
    F(TUNIC_CLEAR_SITES_1, NO_ALT, 0x12001929, ANY, fx_tunic_clear, 0, "tunic clear"),
    F(UNDERWATER_OWNED_SITE, NO_ALT, 0x9400351e, ANY, fx_underwater_owned, 0, "sunken items owned"),
    F(UNDERWATER_VIEW_SITE, NO_ALT, 0x1a9f07fb, ANY, fx_underwater_view, 0, "sunken items view"),
    F(UNDERWATER_PICKUP_SITE, NO_ALT, 0x9420589f, ANY, fx_underwater_pickup, 0, "sunken items pickup"),
    F(NO_HANDLER_SITES_0, A(NO_HANDLER_CATCH), 0x321d03e0, ANY, fx_no_handler, 0, "no handler"),
    F(NO_HANDLER_SITES_1, NO_ALT, 0x940e8868, ANY, fx_no_handler, 1, "no handler: begin catch"),
    F(NO_HANDLER_SITES_2, NO_ALT, 0x940e887b, ANY, fx_no_handler, 2, "no handler: end catch"),
    F(PROMPT_POS_SITE, NO_ALT, 0x2d060102, ANY, fx_prompt_pos, 0, "action prompt spot"),
    F(SHAPE_SIM_SITES_0, A(SHAPE_SIM_SKIP_0), 0xaa0003f4, ANY, fx_shape_sim, 20, "PhysX shape change"),
    F(SHAPE_SIM_SITES_1, A(SHAPE_SIM_SKIP_1), 0xaa0003e1, ANY, fx_shape_sim, 1, "PhysX shape change"),
    F(EMPTY_HANDLER_SITE, A(EMPTY_HANDLER_CATCH), 0x910063e1, ANY, fx_empty_handler, 0, "empty event handler"),
    F(DELEGATE_FORWARDER_SITES_0, NO_ALT, 0xaa0003e1, ANY, fx_delegate_forwarder, 0x28, "delegate forwarder"),
    F(DELEGATE_FORWARDER_SITES_1, NO_ALT, 0xaa0003e1, ANY, fx_delegate_forwarder, 0x30, "delegate forwarder"),
    F(FRAME_JOB_KICKOFF_SITES_0, NO_ALT, 0x91078013 | 22 << 5, ANY, fx_frame_job_kickoff, 22, "frame job kick-off"),
    F(FRAME_JOB_KICKOFF_SITES_1, NO_ALT, 0x91078013 | 22 << 5, ANY, fx_frame_job_kickoff, 22, "frame job kick-off"),
    F(FRAME_JOB_KICKOFF_SITES_2, NO_ALT, 0x91078013 | 22 << 5, ANY, fx_frame_job_kickoff, 22, "frame job kick-off"),
    F(FRAME_JOB_KICKOFF_SITES_3, NO_ALT, 0x91078013 | 21 << 5, ANY, fx_frame_job_kickoff, 21, "frame job kick-off"),
    F(FRAME_JOB_FINISH_SITE, NO_ALT, 0xf9400008, ANY, fx_frame_job_finish, 0, "frame job finish"),
    F(OWNER_CALL_NOTIFY_SITE, NO_ALT, 0x9100e2a0, ANY, fx_owner_call_notify, 0, "owner-thread call notify"),
};
#define NFIX (sizeof fixes / sizeof *fixes)
static uint32_t fix_word[NFIX];  // (each site's instruction as found)
int native_patch_c(Cpu *c, uint32_t i) { return fixes[i].fn(c, &fixes[i], fix_word[i]); }

// The original instruction, to run from the stub at `at`: b / bl aimed from there; any other pc-relative one can't
// move (its handlers never answer ORIG): brk, to stop there loudly if one did
static uint32_t moved(uint32_t w, uint64_t site, uint64_t at) {
    if ((w & 0x7C000000u) == 0x14000000u) {  // b / bl
        int64_t d = (int64_t)((uint64_t)(w & 0x3FFFFFFu) << 38) >> 36;
        return (w & 0x80000000u) | (enc_b(at, site + d) & 0x7FFFFFFFu);
    }
    if ((w & 0x1F000000u) == 0x10000000u || (w & 0x3B000000u) == 0x18000000u || (w & 0x7E000000u) == 0x34000000u ||
        (w & 0x7E000000u) == 0x36000000u || (w & 0xFF000010u) == 0x54000000u)
        return BRK(0xf1);  // (adr / adrp, ldr literal, cbz / cbnz, tbz / tbnz, b.cond)
    return w;
}

// 128 bytes: the svc's frame, native_patch_gate, then the way on its answer says
static uint64_t stub_fix(uint32_t *s, uint64_t at, uint64_t site, uint32_t id, uint32_t orig, uint64_t alt) {
    const uint32_t pop[3] = {enc_ldp(16, 17, SP, 0), enc_ldr(30, SP, 16), 0x9100C3FFu};  // (the frame back, add sp #48)
    s[0] = 0xD100C3FFu;                         // sub sp, sp, #48
    s[1] = enc_stp(16, 17, SP, 0);
    s[2] = enc_str(30, SP, 16);
    s[3] = 0xD2800011u | (id << 5);             // movz x17, #id
    s[4] = enc_ldr_lit(16, at + 16, at + 112);
    s[5] = 0xD63F0200u;                         // blr x16 (native_patch_gate: the way on at [sp + 24])
    s[6] = enc_ldr(16, SP, 24);
    s[7] = 0xB4000000u | (15 << 5) | 16;        // cbz x16, 22 (+60)
    s[8] = 0x37080000u | (5 << 5) | 16;         // tbnz w16, #1, 13 (+20)
    memcpy(&s[9], pop, 12);                     // 1 (ALT): the fix's other address
    s[12] = alt ? enc_b(at + 48, alt) : BRK(0xf2);
    s[13] = 0x37000000u | (5 << 5) | 16;        // tbnz w16, #0, 18 (+20)
    memcpy(&s[14], pop, 12);                    // 2 (RET): return
    s[17] = 0xD65F03C0u;                        // ret
    memcpy(&s[18], pop, 12);                    // 3 (ON): on, without the instruction
    s[21] = enc_b(at + 84, site + 4);
    memcpy(&s[22], pop, 12);                    // 0 (ORIG): the instruction, then on
    s[25] = moved(orig, site, at + 100);
    s[26] = enc_b(at + 104, site + 4);
    s[27] = 0xD503201Fu;                        // nop (the literal 8-aligned)
    memcpy(&s[28], &(uint64_t){(uint64_t)(uintptr_t)native_patch_gate}, 8);
    return 128;
}

// The seed's code words (rando.c): written into the game's code itself (the generated code asks rando_word instead)
int rando_code_word(int i, uint64_t *addr, uint32_t *word);

static uint64_t page_up(uint64_t v) { uint64_t p = (uint64_t)sysconf(_SC_PAGESIZE); return (v + p - 1) & ~(p - 1); }

static void prepare_module(Module *m) {
    uint32_t *text = (uint32_t *)(uintptr_t)(m->base + m->text_off);
    uint64_t nw = m->text_size / 4, room = 0;
    bool is_main = m->base == MAIN_BASE;
    if (is_main) {  // (first: a seed's word is the instruction there now)
        uint64_t a;
        uint32_t w;
        int n = 0;
        for (int i = 0; rando_code_word(i, &a, &w); i++, n++) *(uint32_t *)(uintptr_t)a = w;
        if (n) LOG("[native] %d code words of the seed written\n", n);
    }
    for (uint64_t i = 0; i < nw; i++) {
        uint32_t w = text[i];
        if ((w & 0xFFE0001Fu) == 0xD4000001u || (w & ~31u) == MRS_TPIDR || (w & ~31u) == MRS_TPIDRRO || (w & ~31u) == MSR_TPIDR)
            room += 80;
        else if ((w & ~31u) == MRS_CNTPCT) room += 48;
        else if ((w & ~31u) == MRS_CNTFRQ) room += 16;
    }
    if (is_main) room += NFIX * 128;
    uint64_t area = (m->base + m->size + 0xFFFF) & ~0xFFFFull, asize = page_up(room ? room : 1);
    plat_commit(area, asize);
    uint64_t at = area;
    unsigned nsvc = 0, ntls = 0, ncnt = 0;
    for (uint64_t i = 0; i < nw; i++) {
        uint32_t w = text[i];
        uint64_t site = (uint64_t)(uintptr_t)&text[i];
        uint64_t len = 0;
        int n = w & 31;
        if ((w & 0xFFE0001Fu) == 0xD4000001u) len = stub_svc((uint32_t *)(uintptr_t)at, at, site, (w >> 5) & 0xFFFF), nsvc++;
        else if ((w & ~31u) == MRS_TPIDRRO) len = stub_tls((uint32_t *)(uintptr_t)at, at, site, n, 288, false), ntls++;
        else if ((w & ~31u) == MRS_TPIDR) len = stub_tls((uint32_t *)(uintptr_t)at, at, site, n, 280, false), ntls++;
        else if ((w & ~31u) == MSR_TPIDR) len = stub_tls((uint32_t *)(uintptr_t)at, at, site, n, 280, true), ntls++;
        else if ((w & ~31u) == MRS_CNTPCT) {
            ncnt++;
            if (!g_tick_mul || n == 31) { text[i] = MRS_CNTVCT | n; continue; }
            len = stub_ticks((uint32_t *)(uintptr_t)at, at, site, n);
        } else if ((w & ~31u) == MRS_CNTFRQ) {
            ncnt++;
            if (!g_tick_mul || n == 31) continue;
            len = stub_freq((uint32_t *)(uintptr_t)at, at, site, n);
        }
        if (!len) continue;
        text[i] = enc_b(site, at);
        at += len;
    }
    unsigned nfixed = 0;
    for (uint32_t f = 0; is_main && f < NFIX; f++) {
        uint64_t site = GA_AT(fixes[f].site), alt = fixes[f].alt == NO_ALT ? 0 : GA_AT(fixes[f].alt);
        uint32_t *w = (uint32_t *)(uintptr_t)site;
        if ((*w & fixes[f].mask) != fixes[f].word)
            FATAL("[native] fix '%s' at main+%llx: %08x, not %08x\n", fixes[f].what,
                  (unsigned long long)(site - MAIN_BASE), *w, fixes[f].word);
        fix_word[f] = *w;
        at += stub_fix((uint32_t *)(uintptr_t)at, at, site, f, *w, alt);
        *w = enc_b(site, at - 128);
        nfixed++;
    }
    uint64_t t0 = m->base + m->text_off, tsz = page_up(m->text_size);
    __builtin___clear_cache((char *)(uintptr_t)t0, (char *)(uintptr_t)(t0 + tsz));
    __builtin___clear_cache((char *)(uintptr_t)area, (char *)(uintptr_t)(area + asize));
    if (mprotect((void *)(uintptr_t)t0, tsz, PROT_READ | PROT_EXEC) || mprotect((void *)(uintptr_t)area, asize, PROT_READ | PROT_EXEC))
        FATAL("[native] %s: the code can't be made executable (errno %d)\n", m->name, errno);
    LOG("[native] %s: %u svc, %u TLS, %u counter reads rewritten, %u fixes; stubs at %llx (%llu KB)\n", m->name, nsvc,
        ntls, ncnt, nfixed, (unsigned long long)area, (unsigned long long)(asize >> 10));
}

// The HLE slots as code: slot i = movz x17, #i; b trampoline (-> native_hle_gate)
static void prepare_hle(void) {
    uint64_t n = 4096, tramp = HLE_BASE + 16 * n, size = page_up(16 * n + 16);
    if (size > HLE_SIZE) FATAL("[native] HLE slots: %llu bytes\n", (unsigned long long)size);
    plat_commit(HLE_BASE, size);
    for (uint64_t i = 0; i < n; i++) {
        uint32_t *s = (uint32_t *)(uintptr_t)(HLE_BASE + 16 * i);
        s[0] = 0xD2800011u | ((uint32_t)i << 5);
        s[1] = enc_b(HLE_BASE + 16 * i + 4, tramp);
        s[2] = s[3] = BRK(0);
    }
    uint32_t *t = (uint32_t *)(uintptr_t)tramp;
    t[0] = enc_ldr_lit(16, tramp, tramp + 8);
    t[1] = 0xD61F0200u;  // br x16
    memcpy(&t[2], &(uint64_t){(uint64_t)(uintptr_t)native_hle_gate}, 8);
    __builtin___clear_cache((char *)(uintptr_t)HLE_BASE, (char *)(uintptr_t)(HLE_BASE + size));
    if (mprotect((void *)(uintptr_t)HLE_BASE, size, PROT_READ | PROT_EXEC))
        FATAL("[native] the HLE slots can't be made executable (errno %d)\n", errno);
}

void native_prepare(void) {
    if (!native_on()) return;
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    g_tick_mul = f && f != 19200000 ? (uint64_t)(((__uint128_t)19200000 << 32) / f) : 0;
    if (!g_tick_mul && getenv("LARECOMP_NATIVE_SCALED_TICKS")) g_tick_mul = 1ull << 32;  // (test: the scaled reads' stubs, x1.0)
    LOG("[native] the game's code runs on the CPU (counter %llu Hz%s)\n", (unsigned long long)f,
        g_tick_mul ? ": its reads scaled to the Switch's 19.2 MHz" : ", as the Switch's");
    memset(g_native_tab, 0, sizeof g_native_tab);
    for (int i = 0; i < g_nmodules; i++) prepare_module(&g_modules[i]);
    prepare_hle();
}
#elif defined(__ANDROID__)
bool native_wanted(void) { return false; }
void native_begin_boot(void) {}
bool native_on(void) { return false; }
void native_prepare(void) {}
void native_call(Cpu *c, uint64_t addr) { (void)c; (void)addr; }
void native_thread_end(void) {}
void native_host_x18(void) {}
uint64_t native_ticks(void) { return 0; }
#endif
