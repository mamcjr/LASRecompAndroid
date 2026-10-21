// Opt-in CPU-side diagnostics using the GPU queries already issued for the game's counters.
#ifdef __ANDROID__
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "../../platform.h"
#include "../../rt.h"
#include "timing_trace_pair.h"

bool vk_timing_trace_on(void) {
    static _Atomic int enabled = -1;
    int on = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (on < 0) {
        const char *e = getenv("LARECOMP_TIMING_TRACE");
        on = e && atoi(e) != 0;
        atomic_store_explicit(&enabled, on, memory_order_relaxed);
    }
    return on;
}

typedef struct { _Atomic uint64_t ns, n, max; } Wait;
static Wait waits[VK_TIMING_WAITS];
static _Atomic uint64_t fence_calls, fence_clamped;
void vk_timing_trace_fence(uint64_t requested, uint64_t submitted) {
    if (!vk_timing_trace_on()) return;
    atomic_fetch_add_explicit(&fence_calls, 1, memory_order_relaxed);
    if (requested > submitted) atomic_fetch_add_explicit(&fence_clamped, 1, memory_order_relaxed);
}
void vk_timing_trace_wait(int kind, uint64_t ns) {
    if (!vk_timing_trace_on() || kind < 0 || kind >= VK_TIMING_WAITS) return;
    Wait *w = &waits[kind];
    atomic_fetch_add_explicit(&w->ns, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&w->n, 1, memory_order_relaxed);
    uint64_t m = atomic_load_explicit(&w->max, memory_order_relaxed);
    while (m < ns && !atomic_compare_exchange_weak_explicit(&w->max, &m, ns, memory_order_relaxed, memory_order_relaxed)) {}
}

typedef struct {
    unsigned n, unmatched, legacy_miss, cross_frame, invalid, clamped, failures, lost, buckets[5];
    uint64_t first_frame, last_frame, first_seq, last_seq;
    double gpu, delivered, raw, idle, host, max_gpu;
    uint64_t game_ns, present_ns, game_n, present_n;
} Counters;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static VkTimingHistory history;
static Counters counters;

void vk_timing_trace_submission(uint64_t ns, bool presentation) {
    if (!vk_timing_trace_on()) return;
    pthread_mutex_lock(&lock);
    if (presentation) { counters.present_ns += ns; counters.present_n++; }
    else { counters.game_ns += ns; counters.game_n++; }
    pthread_mutex_unlock(&lock);
}

void vk_timing_trace_counter(const VkTimingCounter *s) {
    if (!vk_timing_trace_on()) return;
    pthread_mutex_lock(&lock);
    VkTimingCounter start;
    if (vk_timing_pair(&history, s, &start)) {
        Counters *c = &counters;
        if (!c->n) { c->first_frame = s->frame; c->first_seq = s->seq; }
        c->last_frame = s->frame; c->last_seq = s->seq;
        c->n++;
        c->legacy_miss += !s->idle_matched;
        c->cross_frame += start.frame != s->frame;
        c->clamped += s->delivered_ticks != s->gpu_ticks;
        if (s->gpu_ticks < start.gpu_ticks || s->raw_ns < start.raw_ns || s->host_ns < start.host_ns ||
            s->idle_ns < start.idle_ns || s->delivered_ticks < start.delivered_ticks) c->invalid++;
        else {
            double gpu = (s->gpu_ticks - start.gpu_ticks) * (625.0 / 384) / 1e6;
            c->gpu += gpu;
            c->delivered += (s->delivered_ticks - start.delivered_ticks) * (625.0 / 384) / 1e6;
            c->raw += (s->raw_ns - start.raw_ns) / 1e6;
            c->idle += (s->idle_ns - start.idle_ns) / 1e6;
            c->host += (s->host_ns - start.host_ns) / 1e6;
            if (gpu > c->max_gpu) c->max_gpu = gpu;
            c->buckets[gpu < 8 ? 0 : gpu < 12 ? 1 : gpu < 16 ? 2 : gpu < 20 ? 3 : 4]++;
        }
    } else counters.unmatched++;
    pthread_mutex_unlock(&lock);
}

void vk_timing_trace_query_failure(int result, unsigned n, uint64_t seq) {
    if (!vk_timing_trace_on()) return;
    (void)result; (void)seq;
    pthread_mutex_lock(&lock);
    counters.failures++; counters.lost += n;
    pthread_mutex_unlock(&lock);
}

void vk_timing_trace_frame(uint64_t frame) {
    if (!vk_timing_trace_on()) return;
    static uint64_t since, previous_frame;
    uint64_t now = plat_now_ns();
    if (!since) { since = now; previous_frame = frame; return; }
    if (now - since < 2000000000ull) return;
    uint64_t nf = frame - previous_frame;
    since = now; previous_frame = frame;
    pthread_mutex_lock(&lock);
    Counters c = counters;
    counters = (Counters){0};
    pthread_mutex_unlock(&lock);
    unsigned valid = c.n - c.invalid;
    double d = valid ? valid : 1;
    LOG("[gpu-pairs] frames %llu-%llu seq %llu-%llu pairs %u starts %u legacy-miss %u cross %u invalid %u failures %u lost %u gpu %.3f max %.3f delivered %.3f raw %.3f idle %.3f host %.3f clamp %u buckets %u/%u/%u/%u/%u\n",
        (unsigned long long)c.first_frame, (unsigned long long)c.last_frame, (unsigned long long)c.first_seq,
        (unsigned long long)c.last_seq, c.n, c.unmatched, c.legacy_miss, c.cross_frame, c.invalid, c.failures, c.lost,
        c.gpu / d, c.max_gpu, c.delivered / d, c.raw / d, c.idle / d, c.host / d, c.clamped,
        c.buckets[0], c.buckets[1], c.buckets[2], c.buckets[3], c.buckets[4]);
    LOG("[gpu-submissions] frame %llu frames %llu other-count %llu present-count %llu other-per-frame %.3f present-per-frame %.3f total-per-frame %.3f ms\n",
        (unsigned long long)frame, (unsigned long long)nf, (unsigned long long)c.game_n, (unsigned long long)c.present_n,
        nf ? c.game_ns / 1e6 / nf : 0, nf ? c.present_ns / 1e6 / nf : 0, nf ? (c.game_ns + c.present_ns) / 1e6 / nf : 0);
    LOG("[gpu-fences] frame %llu waits %llu clamped-to-submitted %llu\n", (unsigned long long)frame,
        (unsigned long long)atomic_exchange_explicit(&fence_calls, 0, memory_order_relaxed),
        (unsigned long long)atomic_exchange_explicit(&fence_clamped, 0, memory_order_relaxed));
    static const char *names[VK_TIMING_WAITS] = {"timeline", "slot", "acquire", "present", "submit"};
    for (int i = 0; i < VK_TIMING_WAITS; i++) {
        uint64_t ns = atomic_exchange_explicit(&waits[i].ns, 0, memory_order_relaxed);
        uint64_t n = atomic_exchange_explicit(&waits[i].n, 0, memory_order_relaxed);
        uint64_t max = atomic_exchange_explicit(&waits[i].max, 0, memory_order_relaxed);
        LOG("[present-waits] frame %llu frames %llu %s calls %llu total %.3f per-frame %.3f max %.3f ms\n",
            (unsigned long long)frame, (unsigned long long)nf, names[i], (unsigned long long)n,
            ns / 1e6, nf ? ns / 1e6 / nf : 0, max / 1e6);
    }
}
#else
typedef int vk_timing_trace_unused;
#endif
