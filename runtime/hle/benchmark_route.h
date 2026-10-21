// Opt-in benchmark controller. Reads positions, sends stick input, never writes guest memory.
// TSV: loops warmup_seconds pause_seconds tolerance; then x y z waypoints (first is start).
#ifdef __ANDROID__
#include "../platform.h"
#include <stdio.h>

static void benchmark_route_log(const char *event, int loop, int leg, uint64_t frame, const float *p) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    LOG("[route] %s loop %d leg %d frame %llu ns %llu pos %.3f %.3f %.3f\n", event, loop, leg,
        (unsigned long long)frame, (unsigned long long)t.tv_sec * 1000000000ull + t.tv_nsec,
        p[0], p[1], p[2]);
}

static void benchmark_route(uint64_t frame, int32_t *lx, int32_t *ly, uint64_t *buttons) {
    static int init, count, loops, loop, leg, state; // 0 waiting, 1 warmup, 2 moving, 3 paused, 4 done
    static float points[32][3], warmup, pause, tolerance;
    static uint64_t since, telemetry;
    if (!init) {
        init = 1;
        const char *path = getenv("LARECOMP_BENCHMARK_ROUTE");
        if (!path) return;
        FILE *f = fopen(path, "r");
        if (!f) { LOG("[route-error] cannot open route\n"); state = 4; return; }
        int n = fscanf(f, "%d %f %f %f", &loops, &warmup, &pause, &tolerance);
        while (count < 32 && fscanf(f, "%f %f %f", &points[count][0], &points[count][1], &points[count][2]) == 3) count++;
        fclose(f);
        if (n != 4 || loops < 1 || loops > 8 || count < 2 || !isfinite(warmup) || !isfinite(pause) ||
            !isfinite(tolerance) || warmup < 0 || warmup > 120 || pause < 0 || pause > 10 || tolerance < .1 || tolerance > .3) {
            LOG("[route-error] invalid route header\n"); state = 4; count = 0;
        }
        for (int i = 0; i < count; i++) for (int j = 0; j < 3; j++) if (!isfinite(points[i][j])) {
            LOG("[route-error] nonfinite waypoint\n"); state = 4; count = 0;
        }
    }
    if (!count) return;
    if (state == 4) { *lx = *ly = 0; *buttons = 0; return; }
    if (frame < 2400) return; // title script is finished before taking control
    uint64_t ps = LD64(MAIN_BASE + 0x1CC1368);
    if (!ps || strcmp((const char *)(uintptr_t)ps, "Field")) {
        if (state >= 2) { LOG("[route-error] left Field\n"); state = 4; }
        return;
    }
    extern uint64_t doorjump_player(uint64_t frame);
    uint64_t pc = doorjump_player(frame);
    if (!pc) return;
    const float *p = (const float *)(uintptr_t)(pc + 0x80);
    uint64_t now = SDL_GetTicksNS();
    *lx = *ly = 0; *buttons = 0;
    if (now >= telemetry) {
        PlatDeviceStats d; PlatAdpfStats a;
        plat_device_stats(&d); plat_adpf_stats(&a);
        LOG("[telemetry] frame %llu cpu-pct %.2f rss-mb %u avail-mb %u soc-c %.2f battery-c %.2f battery-pct %.2f watts %.3f thermal %d headroom %.3f adpf %d threads %d errors %d target-ms %.3f work-ms %.3f game-ms %.3f render-ms %.3f game-core %d render-core %d clusters %d mhz %u %u %u %u\n",
            (unsigned long long)frame, d.cpu_pct, d.rss_mb, d.avail_mb, d.soc_temp_c, d.batt_temp_c, d.batt_pct,
            d.watts, d.thermal, d.headroom, a.state, a.threads, a.errors, a.target_ms, a.work_ms, a.game_ms,
            a.render_ms, a.game_cpu, a.render_cpu, d.ncluster, d.cluster_mhz[0], d.cluster_mhz[1], d.cluster_mhz[2], d.cluster_mhz[3]);
        telemetry = now + 2000000000ull;
    }
    if (!state) { since = now; state = 1; benchmark_route_log("warmup", 0, -1, frame, p); }
    if (state == 1) {
        if ((now - since) / 1e9 < warmup) return;
        double dx = p[0] - points[0][0], dy = p[1] - points[0][1], dz = p[2] - points[0][2];
        if (dx*dx + dy*dy + dz*dz > 1) { LOG("[route-error] wrong start position\n"); state = 4; return; }
        loop = 1; leg = 1; state = 2; since = now;
        benchmark_route_log("start", loop, leg, frame, p);
        benchmark_route_log("move", loop, leg, frame, p);
    }
    if (state == 3) {
        if ((now - since) / 1e9 < pause) return;
        leg++;
        if (leg == count) { leg = 1; loop++; }
        if (loop > loops) { benchmark_route_log("done", loops, count-1, frame, p); state = 4; return; }
        state = 2; since = now;
        benchmark_route_log("move", loop, leg, frame, p);
    }
    if (state == 2) {
        double dx = points[leg][0] - p[0], dz = points[leg][2] - p[2];
        if (fabs(dx) <= tolerance && fabs(dz) <= tolerance && fabs(points[leg][1] - p[1]) <= .5) {
            benchmark_route_log("arrive", loop, leg, frame, p); state = 3; since = now; return;
        }
        if ((now - since) / 1e9 > 120) { LOG("[route-error] waypoint timeout\n"); state = 4; return; }
        if (fabs(dx) >= fabs(dz)) *lx = dx > 0 ? 32767 : -32767;
        else *ly = dz < 0 ? 32767 : -32767;
    }
}
#endif
