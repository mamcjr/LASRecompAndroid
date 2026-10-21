// Opt-in live settings for a stationary Android benchmark. The controller publishes one
// complete command with rename, then waits for the acknowledgement before timing it.
#ifdef __ANDROID__
static void benchmark_settings(uint64_t frame) {
    static const char *path;
    static bool initialized;
    static uint64_t next_poll;
    static unsigned applied;
    if (!initialized) { path = getenv("LARECOMP_BENCHMARK_CONFIG"); initialized = true; }
    if (!path || !*path) return;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return;
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (now < next_poll) return;
    next_poll = now + 250000000ull;
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned id;
    int fps, res, upscale, aa, aspect;
    char extra;
    int n = fscanf(f, "%u %d %d %d %d %d %c", &id, &fps, &res, &upscale, &aa, &aspect, &extra);
    fclose(f);
    if (n != 6 || id <= applied || fps != 0 ||
        !(res == RES_MATCH || res == 1 || res == 2 || res == RES_DYNAMIC || res == RES_Q75 || res == RES_Q50) ||
        !(upscale == UPSCALE_BILINEAR || upscale == UPSCALE_FSR1) ||
        !(aa == AA_OFF || aa == AA_SMAA) || !(aspect == ASPECT_UNLOCKED || aspect == ASPECT_ORIGINAL)) return;
    extern void vk_async_drain(void);
    vk_async_drain();  // mutate only after queued rendering has stopped reading the old settings
    g_settings.fps_mode = fps;
    g_settings.res_mode = res;
    g_settings.upscaler = upscale;
    g_settings.antialias = aa;
    g_settings.aspect = aspect;
    applied = id;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    LOG("[matrix] applied %u frame %llu monotonic-ns %llu fps %d res %d upscaler %d aa %d aspect %d\n",
        id, (unsigned long long)frame,
        (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec,
        fps, res, upscale, aa, aspect);
}
#endif
