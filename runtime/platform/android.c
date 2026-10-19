// Android host layer (runtime/platform.h), arm64. Based on linux.c; what differs:
// - the guest window goes wherever the app process has room (android/common/guest_window.h) instead of a fixed
//   address: an app process is forked from the zygote, with its libraries already mapped, and can't re-exec itself;
// - stdout / stderr go to logcat (tag LASRecomp);
// - no execinfo: backtraces through the unwinder; no re-exec.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include "../platform.h"
#include "../rt.h"
#include "../../android/common/guest_window.h"

uint64_t g_guest_lo;  // the window's start (rt.h GUEST_LO), set by plat_reserve_guest before main()

// ---- logcat: the runtime logs with fprintf(stderr), as on the desktop; a thread forwards the lines
static void *log_pump(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[4096];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', (size_t)(buf + n - start)))) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, "LASRecomp", start);
            start = nl + 1;
        }
        n = (size_t)(buf + n - start);
        memmove(buf, start, n);
        if (n == sizeof buf - 1) {  // (a line longer than the buffer: in pieces)
            buf[n] = 0;
            __android_log_write(ANDROID_LOG_INFO, "LASRecomp", buf);
            n = 0;
        }
    }
    return NULL;
}

static void log_to_logcat(void) {
    static bool done;
    if (done) return;
    done = true;
    int p[2];
    if (pipe(p) != 0) return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(p[1], STDOUT_FILENO);
    dup2(p[1], STDERR_FILENO);
    close(p[1]);
    pthread_t t;
    if (pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0]) == 0) pthread_detach(t);
}

// ---- guest address space
// Called by runtime/main.c before main() with GUEST_LO / GUEST_HI, while the window has no place yet (GUEST_LO = 0):
// picks one (the lowest 4 GB-aligned free hole above 4 GB and below 2^39) and sets g_guest_lo. Called with a set
// window (it never moves while the process lives), it claims exactly that range, as on Linux.
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    log_to_logcat();
    if (!g_guest_lo) {
        uint64_t base = gw_reserve(hi - lo, GW_ALIGN, GW_FLOOR, GW_VA_LIMIT);
        if (!base) {
            fprintf(stderr, "FATAL: no room for the guest window (%#llx bytes below 2^39)\n", (unsigned long long)(hi - lo));
            return false;
        }
        g_guest_lo = base;
        fprintf(stderr, "[android] guest window %#llx-%#llx\n", (unsigned long long)base, (unsigned long long)(base + hi - lo));
        return true;
    }
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Apps can't have real-time scheduling: the display priority Android gives its own frame threads instead.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -8);  // (THREAD_PRIORITY_DISPLAY; fails quietly if not allowed)
}

// ---- process
void plat_init(void) { log_to_logcat(); }

// "The executable": a file in the folder the app copies its shipped resources to (internal storage, files/res: the
// Baloo font, the icon, ...), so runtime/paths.c finds them next to it as on the desktop. Native libraries stay in
// nativeLibraryDir (LARECOMP_NATIVE_LIB_DIR, set by the activity).
bool plat_exe_path(char *buf, size_t size) {
    const char *files = SDL_GetAndroidInternalStoragePath();
    if (!files) return false;
    return (size_t)snprintf(buf, size, "%s/res/lasrecomp", files) < size;
}

void plat_exec(char *const argv[]) { (void)argv; }  // (an app process can't replace itself: the caller reports it)

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

void plat_fatal_notice(void) {
    fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LAS Recomp", "LAS Recomp stopped because of an error.\n\n"
                             "Details: logcat, tag LASRecomp.", NULL);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

typedef struct { void **pcs; int n, max; } Trace;
static _Unwind_Reason_Code trace_step(struct _Unwind_Context *ctx, void *arg) {
    Trace *t = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && t->n < t->max) t->pcs[t->n++] = (void *)pc;
    return t->n < t->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void plat_print_backtrace(void) {
    void *bt[64];
    Trace t = {bt, 0, 64};
    _Unwind_Backtrace(trace_step, &t);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < t.n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s+%llx)\n", i, (unsigned long long)pc, s, (unsigned long long)off,
                       mod, (unsigned long long)moff);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

// (installed through ART's signal chain: ART still handles its own faults first)
static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes (none of the desktop's helpers exist on Android yet: docs/ANDROID-PORT.md, Fases 6-7)
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

void plat_terminate(void) { _exit(0); }

// ---- device state for the performance overlay (runtime/ui/perf_overlay.mm)
#include <dirent.h>
#include <jni.h>
#include <stdatomic.h>

// What the activity reports once a second (LASRecompActivity.reportDeviceStats: public Android APIs; apps can't read
// the battery or most thermal sensors in /sys).
static pthread_mutex_t g_ds_lock = PTHREAD_MUTEX_INITIALIZER;
static PlatDeviceStats g_ds = {.batt_temp_c = -1, .batt_pct = -1, .thermal = -1, .headroom = -1};

JNIEXPORT void JNICALL Java_com_lasrecomp_LASRecompActivity_nativeDeviceStats(JNIEnv *env, jclass cls, jfloat batt_temp_c,
                                                                            jfloat batt_pct, jfloat watts, jint thermal,
                                                                            jfloat headroom) {
    (void)env, (void)cls;
    pthread_mutex_lock(&g_ds_lock);
    g_ds.batt_temp_c = batt_temp_c;
    g_ds.batt_pct = batt_pct;
    g_ds.watts = watts;
    g_ds.thermal = thermal;
    g_ds.headroom = headroom;
    pthread_mutex_unlock(&g_ds_lock);
}

static uint64_t read_u64(const char *path) {
    char buf[64] = {0};
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    return n > 0 ? strtoull(buf, NULL, 10) : 0;
}

// The CPU clusters (cpufreq policies) and the thermal zones an app may read, found once.
static char g_policy[8][96];
static int g_npolicy = -1;
static char g_tzone[96];  // a readable SoC / CPU temperature (most aren't readable by apps)

static void find_sources(void) {
    if (g_npolicy >= 0) return;
    g_npolicy = 0;
    DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
    if (d) {
        char names[8][32];
        int n = 0;
        for (struct dirent *e; (e = readdir(d)) && n < 8;)
            if (!strncmp(e->d_name, "policy", 6)) snprintf(names[n++], sizeof names[0], "%s", e->d_name);
        closedir(d);
        for (int i = 0; i < n; i++)  // (in core order: policy0, policy2, policy6 ...)
            for (int j = i + 1; j < n; j++)
                if (atoi(names[j] + 6) < atoi(names[i] + 6)) { char t[32]; memcpy(t, names[i], 32); memcpy(names[i], names[j], 32); memcpy(names[j], t, 32); }
        for (int i = 0; i < n; i++)
            snprintf(g_policy[g_npolicy++], sizeof g_policy[0], "/sys/devices/system/cpu/cpufreq/%s/scaling_cur_freq", names[i]);
    }
    int best = -1;  // (prefer CPU / SoC zones; any readable one beats none)
    for (int z = 0; z < 128; z++) {
        char tp[96], type[64] = {0}, path[96];
        snprintf(tp, sizeof tp, "/sys/class/thermal/thermal_zone%d/type", z);
        int fd = open(tp, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, type, sizeof type - 1);
        close(fd);
        if (n <= 0) continue;
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        uint64_t v = read_u64(path);
        if (!v || v > 200000) continue;
        int score = strstr(type, "cpu") ? 3 : strstr(type, "soc") || strstr(type, "aoss") ? 2 : 1;
        if (score > best) { best = score; snprintf(g_tzone, sizeof g_tzone, "%s", path); }
    }
    fprintf(stderr, "[perf] %d CPU clusters, temperature from %s\n", g_npolicy, g_tzone[0] ? g_tzone : "(none readable)");
}

void plat_device_stats(PlatDeviceStats *out) {
    find_sources();
    pthread_mutex_lock(&g_ds_lock);
    *out = g_ds;
    pthread_mutex_unlock(&g_ds_lock);
    out->ncluster = g_npolicy;
    for (int i = 0; i < g_npolicy; i++) out->cluster_mhz[i] = (uint32_t)(read_u64(g_policy[i]) / 1000);
    out->soc_temp_c = g_tzone[0] ? (float)read_u64(g_tzone) / 1000.0f : -1;
    // this process's CPU time since the last call, as a share of every core
    static uint64_t last_ticks, last_ns;
    char buf[1024] = {0};
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        const char *p = n > 0 ? strrchr(buf, ')') : NULL;  // (fields after the command name: utime = 14th, stime = 15th)
        unsigned long long ut = 0, st = 0;
        if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) {
            uint64_t ticks = ut + st, ns = plat_now_ns();
            long hz = sysconf(_SC_CLK_TCK), cores = sysconf(_SC_NPROCESSORS_ONLN);
            if (last_ns && ns > last_ns && hz > 0 && cores > 0)
                out->cpu_pct = (float)((double)(ticks - last_ticks) / hz / ((ns - last_ns) / 1e9) / cores * 100.0);
            last_ticks = ticks, last_ns = ns;
        }
    }
    // memory: this process's resident set, and what the system has available
    out->rss_mb = out->avail_mb = 0;
    FILE *f = fopen("/proc/self/status", "re");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) if (!strncmp(line, "VmRSS:", 6)) out->rss_mb = (uint32_t)(atoll(line + 6) / 1024);
        fclose(f);
    }
    if ((f = fopen("/proc/meminfo", "re"))) {
        char line[256];
        while (fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) out->avail_mb = (uint32_t)(atoll(line + 13) / 1024);
        fclose(f);
    }
}

// ---- ADPF (Android Dynamic Performance Framework, API 33+): the presenting thread's work per frame against its
// target. Android's governor otherwise keeps a thread that works ~65% of each 60 fps frame on a middling clock (or
// core), and a frame with more of the game's work than usual (2-3x in places) overran: the overworld fell back to its
// 30 fps mode. Loaded at run time (minSdk 30). LARECOMP_NO_ADPF=1: off.
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;

// LARECOMP_PIN=1 (experiment, off by default): the presenting thread (the game's main thread, which also records the
// frame) on the cores with the highest clock. Left to the scheduler it stays on the middle cluster (cores 0-5 at
// ~2.4 GHz on a Snapdragon 8 Elite) while the two prime cores idle at 1 GHz. But pinned, it crashed the game within a
// minute or two every time (2026-10-16, four runs): a job worker called into a job the main thread had already moved
// on from - a race in the game's job system that a main thread much faster than its workers wins (on the Switch every
// core runs at the same clock).
static cpu_set_t g_all_cores;
static bool g_pinned;

void plat_thread_unpin(void) {
    if (g_pinned) sched_setaffinity(0, sizeof g_all_cores, &g_all_cores);
}

static void pin_to_fastest_cores(void) {
    if (!getenv("LARECOMP_PIN")) return;
    long best = 0, mhz[CPU_SETSIZE] = {0};
    int n = 0;
    for (int c = 0; c < CPU_SETSIZE && c < 64; c++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        if (fscanf(f, "%ld", &mhz[c]) != 1) mhz[c] = 0;
        fclose(f);
        if (mhz[c] > best) best = mhz[c];
        n = c + 1;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    int k = 0;
    for (int c = 0; c < n; c++)
        if (mhz[c] == best) {
            CPU_SET(c, &set);
            k++;
        }
    if (!best || k == 0 || k == n) return;  // (one cluster: nothing to pick)
    if (sched_getaffinity(0, sizeof g_all_cores, &g_all_cores)) return;
    g_pinned = true;  // (before: a thread started from here from now on goes back to g_all_cores)
    int r = sched_setaffinity(0, sizeof set, &set);
    LOG("[android] presenting thread on the %d fastest cores (%ld MHz): %s\n", k, best / 1000, r ? strerror(errno) : "ok");
}

// One session for both threads by default (the presenting thread and the render thread; LARECOMP_ADPF_SPLIT=1 for
// two separate sessions). The presenting thread reports the frame's critical path (max of both threads' busy times).
typedef struct AWorkDuration AWorkDuration;
static int loaded;  // 0 untried, 1 on, -1 off, -2 disabled
static int s_split = -1;
static int s_errors, s_last_error, s_recreated;
static int32_t s_presenting_tid, s_render_tid;
static int s_session_threads;
static float s_target_ms, s_work_ms, s_game_ms, s_render_ms;
static int s_game_cpu = -1, s_render_cpu = -1;

static void adpf_work(int slot, uint64_t work_ns, uint64_t target_ns, uint64_t gpu_ns, uint64_t game_ns) {
    static APerformanceHintManager *(*get_manager)(void);
    static APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    static int (*report)(APerformanceHintSession *, int64_t);
    static int (*retarget)(APerformanceHintSession *, int64_t);
    static int (*set_threads)(APerformanceHintSession *, const int32_t *, size_t);
    static void (*close_session)(APerformanceHintSession *);
    static int state[2];  // per slot: 0 untried, 1 on, -1 off
    static APerformanceHintSession *session[2];
    static uint64_t target[2];
    // (API 35+: a work duration with the GPU's time - the governor then raises the GPU's clock too)
    static AWorkDuration *(*wd_create)(void);
    static void (*wd_start)(AWorkDuration *, int64_t), (*wd_total)(AWorkDuration *, int64_t), (*wd_cpu)(AWorkDuration *, int64_t),
        (*wd_gpu)(AWorkDuration *, int64_t);
    static int (*report2)(APerformanceHintSession *, AWorkDuration *);
    static AWorkDuration *wd;
    if (loaded < 0 || state[slot] < 0) return;
    if (!loaded) {
        if (getenv("LARECOMP_NO_ADPF")) { loaded = -2; return; }
        loaded = -1;
        void *lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return;
        get_manager = (APerformanceHintManager * (*)(void)) dlsym(lib, "APerformanceHint_getManager");
        create = (APerformanceHintSession * (*)(APerformanceHintManager *, const int32_t *, size_t, int64_t))
            dlsym(lib, "APerformanceHint_createSession");
        report = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
        retarget = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
        set_threads = (int (*)(APerformanceHintSession *, const int32_t *, size_t))dlsym(lib, "APerformanceHint_setThreads");
        close_session = (void (*)(APerformanceHintSession *))dlsym(lib, "APerformanceHint_closeSession");
        if (!get_manager || !create || !report || !retarget) return;
        wd_create = (AWorkDuration * (*)(void)) dlsym(lib, "AWorkDuration_create");
        wd_start = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos");
        wd_total = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setActualTotalDurationNanos");
        wd_cpu = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setActualCpuDurationNanos");
        wd_gpu = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setActualGpuDurationNanos");
        report2 = (int (*)(APerformanceHintSession *, AWorkDuration *))dlsym(lib, "APerformanceHint_reportActualWorkDuration2");
        if (wd_create && wd_start && wd_total && wd_cpu && wd_gpu && report2) wd = wd_create();
        LOG("[android] performance hints with the GPU's time: %s\n", wd ? "yes" : "no (API < 35)");
        loaded = 1;
    }
    if (s_split < 0) s_split = getenv("LARECOMP_ADPF_SPLIT") != NULL;

    // Target duration adjustment: LARECOMP_ADPF_TARGET_US or LARECOMP_ADPF_MARGIN (pct)
    static int target_override_us = -1, margin_pct = -1;
    if (target_override_us < 0) {
        const char *to = getenv("LARECOMP_ADPF_TARGET_US");
        target_override_us = to ? atoi(to) : 0;
        const char *mp = getenv("LARECOMP_ADPF_MARGIN");
        margin_pct = mp ? atoi(mp) : 0;
    }
    if (target_override_us > 0) target_ns = (uint64_t)target_override_us * 1000ull;
    else if (margin_pct > 0 && margin_pct < 100) target_ns = target_ns * (100 - margin_pct) / 100;

    if (slot == 0) {  // Presenting thread (game's main loop)
        s_presenting_tid = (int32_t)gettid();
        s_game_cpu = sched_getcpu();
        float gms = (float)(game_ns ? game_ns : work_ns) / 1e6f;
        s_game_ms = s_game_ms > 0 ? s_game_ms * 0.9f + gms * 0.1f : gms;
        s_work_ms = (float)work_ns / 1e6f;
        s_target_ms = (float)target_ns / 1e6f;
    } else {          // Render thread (Vulkan command translation)
        s_render_tid = (int32_t)gettid();
        s_render_cpu = sched_getcpu();
        float rms = (float)work_ns / 1e6f;
        s_render_ms = s_render_ms > 0 ? s_render_ms * 0.9f + rms * 0.1f : rms;
    }

    APerformanceHintManager *m = get_manager();
    if (!m) return;

    if (!s_split) {
        // --- Single session mode (both threads in session[0]) ---
        if (!session[0]) {
            if (slot == 0) pin_to_fastest_cores();
            int32_t tids[2];
            int ntids = 0;
            if (s_presenting_tid > 0) tids[ntids++] = s_presenting_tid;
            if (s_render_tid > 0) tids[ntids++] = s_render_tid;
            if (ntids > 0) {
                session[0] = create(m, tids, (size_t)ntids, (int64_t)target_ns);
                if (session[0]) {
                    s_session_threads = ntids;
                    target[0] = target_ns;
                    state[0] = 1;
                    LOG("[android] performance hint single session (%d thread%s): on\n", ntids, ntids > 1 ? "s" : "");
                } else {
                    s_errors++;
                    s_last_error = errno;
                }
            }
        } else if (s_session_threads < 2 && s_presenting_tid > 0 && s_render_tid > 0) {
            // Add the second thread to the active session
            int32_t tids[2] = {s_presenting_tid, s_render_tid};
            int ok = 0;
            if (set_threads) {
                int r = set_threads(session[0], tids, 2);
                if (r == 0) { ok = 1; s_session_threads = 2; }
                else { s_errors++; s_last_error = r; }
            }
            if (!ok) {
                if (close_session) close_session(session[0]);
                session[0] = create(m, tids, 2, (int64_t)target_ns);
                if (session[0]) {
                    s_session_threads = 2;
                    s_recreated++;
                    LOG("[android] performance hint session recreated with 2 threads: on\n");
                } else {
                    s_errors++;
                    s_last_error = errno;
                }
            }
        }
        if (slot != 0 || !session[0]) return;  // Render thread doesn't report separately in single-session mode

        if (target_ns != target[0]) {
            int r = retarget(session[0], (int64_t)target_ns);
            if (r != 0) { s_errors++; s_last_error = r; }
            target[0] = target_ns;
        }
        if (gpu_ns && wd) {
            int64_t now = (int64_t)plat_now_ns(), cpu = (int64_t)(work_ns ? work_ns : 1), gpu = (int64_t)gpu_ns;
            wd_start(wd, now - (cpu > gpu ? cpu : gpu));
            wd_total(wd, cpu > gpu ? cpu : gpu);
            wd_cpu(wd, cpu);
            wd_gpu(wd, gpu);
            int r = report2(session[0], wd);
            if (r != 0) {
                s_errors++;
                s_last_error = r;
                static bool told;
                if (!told) { told = true; LOG("[android] performance hint with GPU: error %d\n", r); }
            } else {
                return;
            }
        }
        if (work_ns) {
            int r = report(session[0], (int64_t)work_ns);
            if (r != 0) {
                s_errors++;
                s_last_error = r;
                static bool told;
                if (!told) { told = true; LOG("[android] performance hint report error %d\n", r); }
            }
        }
    } else {
        // --- Split session mode (one session per thread) ---
        if (!state[slot]) {
            state[slot] = -1;
            if (slot == 0) pin_to_fastest_cores();
            int32_t tid = (int32_t)gettid();
            session[slot] = create(m, &tid, 1, (int64_t)target_ns);
            LOG("[android] performance hint session (%s thread): %s\n", slot ? "render" : "presenting",
                session[slot] ? "on" : "not available");
            if (!session[slot]) { s_errors++; s_last_error = errno; return; }
            target[slot] = target_ns;
            state[slot] = 1;
            s_session_threads = (session[0] ? 1 : 0) + (session[1] ? 1 : 0);
        }
        if (!session[slot]) return;
        if (target_ns != target[slot]) {
            int r = retarget(session[slot], (int64_t)target_ns);
            if (r != 0) { s_errors++; s_last_error = r; }
            target[slot] = target_ns;
        }
        if (slot == 0 && gpu_ns && wd) {
            int64_t now = (int64_t)plat_now_ns(), cpu = (int64_t)(work_ns ? work_ns : 1), gpu = (int64_t)gpu_ns;
            wd_start(wd, now - (cpu > gpu ? cpu : gpu));
            wd_total(wd, cpu > gpu ? cpu : gpu);
            wd_cpu(wd, cpu);
            wd_gpu(wd, gpu);
            int r = report2(session[slot], wd);
            if (r != 0) { s_errors++; s_last_error = r; }
            else return;
        }
        if (work_ns) {
            int r = report(session[slot], (int64_t)work_ns);
            if (r != 0) { s_errors++; s_last_error = r; }
        }
    }
}

void plat_frame_work(uint64_t work_ns, uint64_t target_ns) { adpf_work(0, work_ns, target_ns, 0, work_ns); }
void plat_frame_work_gpu(uint64_t cpu_ns, uint64_t gpu_ns, uint64_t target_ns, uint64_t game_ns) {
    adpf_work(0, cpu_ns, target_ns, gpu_ns, game_ns);
}
void plat_render_work(uint64_t work_ns, uint64_t target_ns) { adpf_work(1, work_ns, target_ns, 0, 0); }

void plat_adpf_stats(PlatAdpfStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state = loaded;
    out->sessions = s_split ? 2 : 1;
    out->threads = s_session_threads;
    out->errors = s_errors;
    out->last_error = s_last_error;
    out->recreated = s_recreated;
    out->target_ms = s_target_ms;
    out->work_ms = s_work_ms;
    out->game_ms = s_game_ms;
    out->render_ms = s_render_ms;
    out->game_cpu = s_game_cpu;
    out->render_cpu = s_render_cpu;
}
