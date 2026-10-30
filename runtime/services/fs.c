// fsp-srv: RomFS as a raw storage image, save data and SD card backed by host directories.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#undef IN   // (windows.h's empty parameter markers; ipc.h defines its own)
#undef OUT
#endif

#include "../hle.h"
#include "../mods.h"
#include "../rando.h"
#include "../ipc.h"
#include "../game_addresses.h"
#include "../romfs_build.h"

#define FS_PATH_NOT_FOUND MAKE_RESULT(2, 1)
#define FS_PATH_EXISTS MAKE_RESULT(2, 2)
#define FS_TARGET_LOCKED MAKE_RESULT(2, 7)
#define FS_NOT_EMPTY MAKE_RESULT(2, 8)

const char *g_save_dir = "save";
static int romfs_fd = -1;

#ifdef __ANDROID__
// The game files used in place (main.c g_romfs_link: <root>/romfs.link names the player's own romfs/ folder): no
// romfs.bin, its image made up from that folder (romfs_build.h romfs_virtual) - romfs_fd is then VIRT_FD, and every
// read of the image goes through img_pread. Kept over a reboot (the same files). From BOTWRecompAndroid (f1d15ba).
#define VIRT_FD (-2)
static RomfsVirtual *romfs_virt;
static ssize_t img_pread(int fd, void *dst, size_t n, uint64_t off) {
    if (fd == VIRT_FD) return romfs_virt ? (ssize_t)romfs_virtual_read(romfs_virt, dst, n, off) : -1;
    return pread(fd, dst, n, (off_t)off);
}
static uint64_t img_size(int fd) {
    if (fd == VIRT_FD) return romfs_virt ? romfs_virtual_size(romfs_virt) : 0;
    struct stat sb;
    return fstat(fd, &sb) ? 0 : (uint64_t)sb.st_size;
}
#else
#define img_pread(fd, dst, n, off) pread(fd, dst, n, (off_t)(off))
static uint64_t img_size(int fd) {
    struct stat sb;
    fstat(fd, &sb);
    return (uint64_t)sb.st_size;
}
#endif

// ------------------------------------------------------------------ RomFS file index (offset -> path)
typedef struct { uint64_t start, size; char *path; int fd; const uint8_t *mem; } Overlay;
// in-image patches (copies of RomFS tables with changes): header, dir meta, file hash, file meta
typedef struct { uint64_t off, size; uint8_t *data; } Patch;
static Patch patches[8];
static int npatches;
static Overlay *ov;
static int ov_n, ov_cap;
static uint8_t *ov_meta;
static uint64_t ov_meta_off, ov_meta_size, ov_end;
// Lets the runtime see which game files are being streamed (e.g. which level is loading).
typedef struct { uint64_t off, size; char *path; uint32_t meta; } RomFile;
static RomFile *rom_files;
static int rom_nfiles;
static uint64_t rom_data_off;

// Reboot: the next seed builds its own index / overlay when the game opens the RomFS again.
RT_ON_RESET(fs_reset) {
    for (int i = 0; i < ov_n; i++) {
        if (ov[i].fd >= 0) close(ov[i].fd);
        free(ov[i].path);
    }
    for (int i = 0; i < npatches; i++) free(patches[i].data);  // (ov_meta and the extended table are among these)
    for (int i = 0; i < rom_nfiles; i++) free(rom_files[i].path);
    free(ov), free(rom_files);
    ov = NULL, ov_n = ov_cap = 0;
    ov_meta = NULL, ov_meta_off = ov_meta_size = ov_end = 0;
    npatches = 0;
    rom_files = NULL, rom_nfiles = 0, rom_data_off = 0;
    if (romfs_fd >= 0) close(romfs_fd);
    romfs_fd = -1;  // (Android: romfs_virt stays - the same files next boot)
    g_save_dir = "save";
}

static int rom_cmp(const void *a, const void *b) {
    const RomFile *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static void rom_dir_path(const uint8_t *dmeta, uint32_t dsize, uint32_t dir, char *out, size_t cap) {
    char parts[16][256];
    int n = 0;
    while (dir != 0 && dir < dsize && n < 16) {  // root (offset 0) has an empty name
        uint32_t parent, namelen;
        memcpy(&parent, dmeta + dir, 4);
        memcpy(&namelen, dmeta + dir + 0x14, 4);
        if (namelen > 255) namelen = 255;
        memcpy(parts[n], dmeta + dir + 0x18, namelen);
        parts[n][namelen] = 0;
        n++;
        dir = parent;
    }
    out[0] = 0;
    for (int i = n - 1; i >= 0; i--) { strlcat(out, parts[i], cap); strlcat(out, "/", cap); }
}

// ---- files a seed ADDS (names the game's RomFS doesn't have: generated dungeon cells...). The game parses the
// RomFS tables itself, so they're extended: a copy of the file-meta table with the new entries appended (linked
// into their directory's file list and the name hash buckets) is placed past the end of the image, the header,
// directory table and hash table are patched to match. New files must go into existing directories.
static uint32_t romfs_hash(uint32_t parent, const char *name, uint32_t len) {
    uint32_t hash = parent ^ 123456789;
    for (uint32_t i = 0; i < len; i++) { hash = (hash >> 5) | (hash << 27); hash ^= (uint8_t)name[i]; }
    return hash;
}

static int path_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct { char rel[1024]; char host[1200]; uint64_t size; } NewFile;

static void walk_new(const char *root, const char *rel, char **known, int nknown, NewFile **out, int *n, int *cap) {
    char dirp[1200];
    snprintf(dirp, sizeof dirp, "%s/%s", root, rel);
    DIR *d = opendir(dirp);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char r2[1024], h2[1200];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(h2, sizeof h2, "%s/%s", root, r2);
        struct stat sb;
        if (stat(h2, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { walk_new(root, r2, known, nknown, out, n, cap); continue; }
        const char *key = r2;
        if (bsearch(&key, known, nknown, sizeof *known, path_cmp)) continue;  // replaces an existing file
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, sizeof **out * *cap); }
        strlcpy((*out)[*n].rel, r2, sizeof (*out)[*n].rel);
        strlcpy((*out)[*n].host, h2, sizeof (*out)[*n].host);
        (*out)[*n].size = (uint64_t)sb.st_size;
        (*n)++;
    }
    closedir(d);
}

// directory entry offset for "a/b/c" (NULL-terminated walk from the root, offset 0), -1 if missing
static int64_t dir_lookup(const uint8_t *dm, uint64_t dsz, const char *path) {
    uint32_t cur = 0;
    char buf[1024];
    strlcpy(buf, path, sizeof buf);
    for (char *tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/")) {
        uint32_t child;
        memcpy(&child, dm + cur + 8, 4);
        while (child != 0xFFFFFFFFu && child + 0x18 <= dsz) {
            uint32_t nl;
            memcpy(&nl, dm + child + 0x14, 4);
            if (nl == strlen(tok) && !memcmp(dm + child + 0x18, tok, nl)) break;
            memcpy(&child, dm + child + 4, 4);
        }
        if (child == 0xFFFFFFFFu || child + 0x18 > dsz) return -1;
        cur = child;
    }
    return cur;
}

static void add_new_files(int fd, const uint64_t *h, const uint8_t *dmeta, uint64_t dsz, uint64_t fsz) {
    const char *seed = rando_seed_dir();
    if (!seed) return;
    char root[1100];
    snprintf(root, sizeof root, "%s/romfs", seed);
    char **known = malloc(sizeof *known * rom_nfiles);
    for (int i = 0; i < rom_nfiles; i++) known[i] = rom_files[i].path;
    qsort(known, rom_nfiles, sizeof *known, path_cmp);
    NewFile *nf = NULL;
    int n = 0, cap = 0;
    walk_new(root, "", known, rom_nfiles, &nf, &n, &cap);
    free(known);
    if (!n) return;
    rom_files = realloc(rom_files, sizeof(RomFile) * (rom_nfiles + n));
    uint64_t hoff = h[5], hsz = h[6];
    uint8_t *dcopy = malloc(dsz), *hcopy = malloc(hsz), *head = malloc(0x50);
    memcpy(dcopy, dmeta, dsz);
    if (img_pread(fd, hcopy, hsz, hoff) != (ssize_t)hsz || img_pread(fd, head, 0x50, 0) != 0x50) return;
    // the file-meta table to extend: the override-patched copy if there is one
    const uint8_t *base_meta = ov_meta;
    uint8_t *tmp = NULL;
    if (!base_meta) { tmp = malloc(fsz); img_pread(fd, tmp, fsz, h[7]); base_meta = tmp; }
    uint64_t extra = 0;
    for (int i = 0; i < n; i++) {
        const char *name = strrchr(nf[i].rel, '/');
        name = name ? name + 1 : nf[i].rel;
        extra += 0x20 + ((strlen(name) + 3) & ~3u);
    }
    uint64_t nsz = fsz + extra;
    uint8_t *meta = calloc(1, nsz);
    memcpy(meta, base_meta, fsz);
    free(tmp);
    if (!ov_end) ov_end = (img_size(fd) + 0xF) & ~0xFull;
    uint64_t meta_at = ov_end;  // the new table first, then the new files' data
    uint64_t data_at = (meta_at + nsz + 0xF) & ~0xFull;
    uint32_t nb = (uint32_t)(hsz / 4);
    uint64_t o = fsz;
    int added = 0;
    for (int i = 0; i < n; i++) {
        char dirp[1024];
        strlcpy(dirp, nf[i].rel, sizeof dirp);
        char *slash = strrchr(dirp, '/');
        const char *name = slash ? slash + 1 : nf[i].rel;
        if (slash) *slash = 0; else dirp[0] = 0;
        int64_t dir = dir_lookup(dcopy, dsz, dirp);
        if (dir < 0) { LOG("[fs] seed file %s: directory not in the game's RomFS, skipped\n", nf[i].rel); continue; }
        uint32_t nl = (uint32_t)strlen(name), d32 = (uint32_t)dir, entry = (uint32_t)o, child;
        memcpy(&child, dcopy + dir + 0xC, 4);  // dir's first file -> becomes our sibling
        uint64_t rel = data_at - rom_data_off, size = nf[i].size;
        uint32_t bucket = romfs_hash(d32, name, nl) % nb, prev;
        memcpy(&prev, hcopy + 4 * bucket, 4);
        memcpy(meta + o, &d32, 4);
        memcpy(meta + o + 4, &child, 4);
        memcpy(meta + o + 8, &rel, 8);
        memcpy(meta + o + 0x10, &size, 8);
        memcpy(meta + o + 0x18, &prev, 4);
        memcpy(meta + o + 0x1C, &nl, 4);
        memcpy(meta + o + 0x20, name, nl);
        memcpy(dcopy + dir + 0xC, &entry, 4);
        memcpy(hcopy + 4 * bucket, &entry, 4);
        o += 0x20 + ((nl + 3) & ~3u);
        if (ov_n == ov_cap) { ov_cap = ov_cap ? ov_cap * 2 : 256; ov = realloc(ov, sizeof *ov * ov_cap); }
        ov[ov_n++] = (Overlay){data_at, size, strdup(nf[i].host), -1, NULL};
        rom_files[rom_nfiles++] = (RomFile){data_at, size, strdup(nf[i].rel), entry};
        data_at = (data_at + size + 0xF) & ~0xFull;
        added++;
    }
    // the new table goes before the new data (keep the overlay list sorted by start)
    if (ov_n == ov_cap) { ov_cap = ov_cap ? ov_cap * 2 : 256; ov = realloc(ov, sizeof *ov * ov_cap); }
    memmove(ov + (ov_n - added) + 1, ov + (ov_n - added), sizeof *ov * added);
    ov[ov_n - added] = (Overlay){meta_at, nsz, NULL, -1, meta};
    ov_n++;
    ov_end = data_at;
    uint64_t fmeta_off = meta_at, fmeta_size = o;
    memcpy(head + 7 * 8, &fmeta_off, 8);
    memcpy(head + 8 * 8, &fmeta_size, 8);
    patches[npatches++] = (Patch){0, 0x50, head};
    patches[npatches++] = (Patch){h[3], dsz, dcopy};
    patches[npatches++] = (Patch){hoff, hsz, hcopy};
    LOG("[fs] randomizer: %d new romfs files\n", added);
}

static void rom_index(int fd) {
#ifdef __ANDROID__
    mods_scan();  // (game mods: runtime/mods.c - their files replace the game's below, after a randomizer seed's)
#endif
    uint64_t h[10];
    if (img_pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h) return;
    // header: size, dir hash (off,size), dir meta (off,size), file hash (off,size), file meta (off,size), data off
    uint64_t doff = h[3], dsz = h[4], foff = h[7], fsz = h[8];
    rom_data_off = h[9];
    uint8_t *dmeta = malloc(dsz), *fmeta = malloc(fsz);
    if (img_pread(fd, dmeta, dsz, doff) != (ssize_t)dsz || img_pread(fd, fmeta, fsz, foff) != (ssize_t)fsz) return;
    int cap = 4096;
    rom_files = malloc(sizeof(RomFile) * cap);
    for (uint64_t o = 0; o + 0x20 <= fsz;) {
        uint32_t parent, namelen;
        uint64_t data, size;
        memcpy(&parent, fmeta + o, 4);
        memcpy(&data, fmeta + o + 8, 8);
        memcpy(&size, fmeta + o + 0x10, 8);
        memcpy(&namelen, fmeta + o + 0x1C, 4);
        char dir[1024], name[256];
        rom_dir_path(dmeta, (uint32_t)dsz, parent, dir, sizeof dir);
        uint32_t nl = namelen > 255 ? 255 : namelen;
        memcpy(name, fmeta + o + 0x20, nl);
        name[nl] = 0;
        if (rom_nfiles == cap) { cap *= 2; rom_files = realloc(rom_files, sizeof(RomFile) * cap); }
        char *path = malloc(strlen(dir) + nl + 1);
        strcpy(path, dir);
        strcat(path, name);
        rom_files[rom_nfiles++] = (RomFile){rom_data_off + data, size, path, (uint32_t)o};
        o += 0x20 + ((namelen + 3) & ~3u);
    }
#ifdef __ANDROID__
    {   // (from upstream LASRecomp beta_5.3: its issues #391 / #389) the game's files by name and size against its own
        // (game_addresses.c game_romfs_check_*), and their total: one damaged, renamed or missing made the game crash at
        // boot with no hint. The log names the odd files (the start screen says so too: launcher.mm romfs_check). The
        // game code's six files some copies have at the romfs root count when exefs/ has them at the same size.
        enum { LOGGED = 20 };
        GameRomfsCheck *check = game_romfs_check_begin(game_version());
        int odd = 0, copies = 0;
        uint64_t bytes = 0;
        for (int i = 0; i < rom_nfiles; i++) {
            const char *p = rom_files[i].path;
            char exe[1200];
            struct stat es;
            size_t rl = g_romfs_dir ? strlen(g_romfs_dir) : 0;  // (<root>/romfs: its exefs/ beside it)
            if (!strchr(p, '/') && game_romfs_exefs_name(p) && rl > 5 &&
                snprintf(exe, sizeof exe, "%.*sexefs/%s", (int)(rl - 5), g_romfs_dir, p) < (int)sizeof exe &&
                stat(exe, &es) == 0 && (uint64_t)es.st_size == rom_files[i].size) {
                copies++;
                continue;
            }
            bytes += rom_files[i].size;
            if (!game_romfs_check_file(check, p, rom_files[i].size) && odd++ < LOGGED)
                LOG("[fs] romfs: not one of the game's files: %s (%llu bytes)\n", p, (unsigned long long)rom_files[i].size);
        }
        if (odd > LOGGED) LOG("[fs] romfs: %d more files that aren't the game's\n", odd - LOGGED);
        if (copies) LOG("[fs] romfs: %d copies of the game code at the root, ignored\n", copies);
        int missing = game_romfs_check_end(check);
        if (missing) LOG("[fs] romfs: %d of the game's files missing or changed\n", missing);
        uint64_t want = game_romfs_bytes(game_version());
        if (want && bytes != want)
            LOG("[fs] romfs: %llu bytes in %d files, this version's are %llu: damaged or changed game files\n",
                (unsigned long long)bytes, rom_nfiles - copies, (unsigned long long)want);
    }
#endif
    // Randomizer seed: replacement files are served from the seed folder. The file-meta table is patched
    // (in a copy) to point them at data appended past the end of the image; reads there go to host files.
    for (int i = 0; i < rom_nfiles; i++) {
        char host[1200];
        if (!rando_romfs_override(rom_files[i].path, host, sizeof host) &&
            !mods_romfs_override(rom_files[i].path, host, sizeof host))
            continue;
        struct stat sb;
        if (stat(host, &sb)) continue;
        if (!ov_meta) {
            ov_meta = malloc(fsz);
            memcpy(ov_meta, fmeta, fsz);
            ov_meta_off = foff;
            ov_meta_size = fsz;
            ov_end = (img_size(fd) + 0xF) & ~0xFull;
        }
        uint64_t rel = ov_end - rom_data_off, size = (uint64_t)sb.st_size;
        memcpy(ov_meta + rom_files[i].meta + 8, &rel, 8);
        memcpy(ov_meta + rom_files[i].meta + 0x10, &size, 8);
        if (ov_n == ov_cap) { ov_cap = ov_cap ? ov_cap * 2 : 256; ov = realloc(ov, sizeof *ov * ov_cap); }
        ov[ov_n].start = ov_end;
        ov[ov_n].size = size;
        ov[ov_n].path = strdup(host);
        ov[ov_n].fd = -1;
        ov[ov_n].mem = NULL;
        ov_n++;
        ov_end = (ov_end + size + 0xF) & ~0xFull;
    }
#ifdef __ANDROID__
    if (ov_n) LOG("[fs] %d romfs files replaced (%s)\n", ov_n, rando_seed_dir() ? "randomizer / mods" : "mods");
#else
    if (ov_n) LOG("[fs] randomizer: %d romfs files replaced\n", ov_n);
#endif
    if (ov_meta) { patches[npatches++] = (Patch){ov_meta_off, ov_meta_size, ov_meta}; }
    add_new_files(fd, h, dmeta, dsz, fsz);
    qsort(rom_files, rom_nfiles, sizeof *rom_files, rom_cmp);
    free(dmeta);
    free(fmeta);
    LOG("[fs] romfs index: %d files\n", rom_nfiles);
}

// Virtual RomFS image read: original bytes, with the patched file table and appended replacement files.
static ssize_t romfs_read(int fd, uint8_t *dst, uint64_t size, uint64_t off) {
    if (!ov_n) return img_pread(fd, dst, size, off);
    uint64_t done = 0;
    while (done < size) {
        uint64_t o = off + done, n = size - done;
        if (o >= ov[0].start) {  // appended region
            int i = 0;
            while (i < ov_n && !(o >= ov[i].start && o < ov[i].start + ((ov[i].size + 0xF) & ~0xFull))) i++;
            if (i == ov_n) { memset(dst + done, 0, n); done += n; break; }
            uint64_t in = o - ov[i].start, avail = ((ov[i].size + 0xF) & ~0xFull) - in;
            if (n > avail) n = avail;
            ssize_t r;
            if (ov[i].mem) {  // in-memory blob (the extended file table)
                r = in < ov[i].size ? (ssize_t)(n < ov[i].size - in ? n : ov[i].size - in) : 0;
                memcpy(dst + done, ov[i].mem + in, (size_t)r);
            } else {
                if (ov[i].fd < 0) ov[i].fd = open(ov[i].path, O_RDONLY);
                r = in < ov[i].size ? pread(ov[i].fd, dst + done, n < ov[i].size - in ? n : ov[i].size - in, (off_t)in) : 0;
            }
            if (r < 0) r = 0;
            if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
        } else {
            if (o + n > ov[0].start) n = ov[0].start - o;
            ssize_t r = img_pread(fd, dst + done, n, o);
            if (r < 0) r = 0;
            if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
            // overlay the patched tables (file meta, and with added files: header, dir meta, file hash)
            for (int k = 0; k < npatches; k++) {
                uint64_t a = o > patches[k].off ? o : patches[k].off;
                uint64_t b = o + n < patches[k].off + patches[k].size ? o + n : patches[k].off + patches[k].size;
                if (a < b) memcpy(dst + done + (a - o), patches[k].data + (a - patches[k].off), b - a);
            }
        }
        done += n;
    }
    return (ssize_t)done;
}

// Path of the RomFS file containing image offset `off` (NULL if none).
const char *romfs_file_at(uint64_t off) {
    int lo = 0, hi = rom_nfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (off < rom_files[mid].off) hi = mid - 1;
        else if (off >= rom_files[mid].off + rom_files[mid].size) lo = mid + 1;
        else return rom_files[mid].path;
    }
    return NULL;
}

void romfs_file_read_hook(const char *path);

// ------------------------------------------------------------------ IStorage (raw RomFS image)
typedef struct { int fd; uint64_t size; } Storage;

static void storage(Service *s, IpcReq *req, IpcResp *resp) {
    Storage *st = s->state;
    switch (req->cmd) {
    case 0: {  // Read(offset, size) -> B buffer
        uint64_t off = IN(uint64_t, 0), size = IN(uint64_t, 8);
        IpcBuf *b = out_buf(req, 0);
        if (!b) { resp->result = MAKE_RESULT(2, 6001); break; }
        if (size > b->size) size = b->size;
        plat_write_prepare(b->addr, size);  // (a read into pages write tracking made read-only: EFAULT)
        ssize_t n = romfs_read(st->fd, (uint8_t *)(uintptr_t)b->addr, size, off);
        if (n < 0) LOG("[fs] storage read failed at %llx: %s\n", off, strerror(errno));
        {
            const char *path = romfs_file_at(off);
            static const char *last;
            if (path && path != last) {
                last = path;
                if (getenv("LARECOMP_LOG_FILES")) LOG("[fs] read %s\n", path);
                romfs_file_read_hook(path);
            }
        }
        static uint64_t reads, bytes;
        reads++;
        bytes += size;
        if ((reads & (reads - 1)) == 0) LOG("[fs] romfs reads: %llu (%llu KB)\n", reads, bytes >> 10);
        break;
    }
    case 4: OUT(uint64_t, 0) = ov_n ? ov_end : st->size; resp_size(resp, 8); break;  // GetSize
    default: LOG("[fs] IStorage cmd %u (ok)\n", req->cmd);
    }
}

static Service *open_romfs_storage(void) {
#ifdef __ANDROID__
    struct stat lst;  // (the game files used in place: their own romfs.bin, or their romfs/ folder)
    bool link_file = g_romfs_link && stat(g_romfs_link, &lst) == 0 && S_ISREG(lst.st_mode);
    if (romfs_fd == -1 && g_romfs_link && !link_file) {
        if (!romfs_virt) {
            uint64_t t0 = plat_now_ns();
            char why[256];
            romfs_virt = romfs_virtual_open(g_romfs_link, why, sizeof why);
            if (!romfs_virt)
                FATAL("The game files folder can't be read: %s. If it was moved or deleted, set up the game files again "
                      "on the start screen.\n", why);
            LOG("[fs] game data from %s in place: %.1f GB, its index made in %llu ms\n", g_romfs_link,
                romfs_virtual_size(romfs_virt) / 1e9, (unsigned long long)((plat_now_ns() - t0) / 1000000));
        }
        romfs_fd = VIRT_FD;
        rom_index(romfs_fd);
    }
#endif
    if (romfs_fd == -1) {  // (not VIRT_FD)
        char path[1024];
        snprintf(path, sizeof path, "%s.bin", g_romfs_dir);  // extracted/romfs.bin
#ifdef __ANDROID__
        if (link_file) snprintf(path, sizeof path, "%s", g_romfs_link), LOG("[fs] game data from %s in place\n", path);
#endif
        romfs_fd = open(path, O_RDONLY);
        if (romfs_fd < 0) FATAL("cannot open %s (set up the game files on the start screen)\n", path);
        rom_index(romfs_fd);
    }
    Storage *st = calloc(1, sizeof *st);
    st->fd = romfs_fd;
    st->size = img_size(romfs_fd);
    return service_new("IStorage(romfs)", storage, st);
}

// ------------------------------------------------------------------ host-directory filesystem
// Save data is all-or-nothing, as on the Switch (its save storage commits a mount's changes at once): a file opened
// for writing on a save mount works on a copy, <file>.pending, which the mount's Commit syncs and renames over the
// file - so a crash or power loss mid-save leaves the last committed save, never a half-written one (the save
// backups PR #55 was closed in favour of this). Reads see the pending copy, as the Switch's do before a commit. A
// copy left by a crash is dropped at the next mount (uncommitted, as on the Switch). The SD card mount writes in place.
// A save that can't be written (save folder not writable, disk full, ...) is dropped whole and the committed save
// stays as it was, but the game is never told: its SDK aborts on any failed OpenFile / Write / Flush / Commit (the
// crash dialog), and it has no "could not save" path anyway. The player gets one warning (save_failed) instead.
#define PENDING_MAX 16
typedef struct {
    char root[1024];
    bool journaled;
    int npending;
    char pending[PENDING_MAX][1100];
    bool failed[PENDING_MAX];  // a write to this working copy failed: dropped at the Commit, never published
} HostFs;
typedef struct {
    int fd;
    char path[1100];
    HostFs *fs;    // a save file opened for writing (journaled mount), else NULL
    int slot;      // its working copy in fs->pending (-1: a scratch copy)
    bool scratch;  // staging failed: the writes go to a temporary file outside the save folder, never committed
} HostFile;

// Saving failed: say so loudly in the log, and once per run in a dialog (the game itself still says "Game saved").
static void save_failed(HostFs *fs, const char *what, const char *path, int error) {
    LOG("[save] SAVE FAILED: %s %s (%s) - not saved, the last committed save is kept\n", what, path,
        error ? strerror(error) : "earlier error");
    static bool told;
    if (told) return;
    told = true;
    char text[1400];
    snprintf(text, sizeof text, "Your game could not be saved (the game may still say it was). Your previous save "
             "is unchanged.\n\nMake sure this folder can be written to and the disk has free space:\n%s", fs->root);
    plat_notice(text);
}

static void sync_dir_of(const char *path) {
    char d[1100];
    snprintf(d, sizeof d, "%s", path);
    char *slash = strrchr(d, '/');
    if (!slash) return;
    *slash = 0;
#ifndef _WIN32
    int fd = open(d, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
#endif
}

// the working copy over the file in one step (Windows' rename doesn't replace an existing file; with the UTF-8
// code page the A function takes our paths as they are)
static int replace_file(const char *from, const char *to) {
#ifdef _WIN32
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
    LOG("[save] replace %s failed (Windows error %lu)\n", to, (unsigned long)GetLastError());
    errno = EIO;
    return -1;
#else
    return rename(from, to);
#endif
}

// in -> out from the start (errno set on failure)
static bool copy_fd(int in, int out) {
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { if (!w) errno = EIO; return false; }
            done += w;
        }
    return n == 0;
}

static bool copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY);
    if (in < 0) return false;
    int out = open(to, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    bool ok = out >= 0 && copy_fd(in, out);
    int error = errno;
    close(in);
    if (out >= 0 && close(out)) { error = errno; ok = false; }
    errno = error;
    return ok;
}

// A copy of the save file outside the save folder, deleted when closed: what the game writes when its working copy
// can't be made. Writing over the committed file instead would put the only copy at risk (a crash, a full disk or a
// failed write mid-save leaves it half-written, and a rando slot's flags sidecar would no longer match it).
static int scratch_copy(const char *from) {
    int in = open(from, O_RDONLY);
    if (in < 0) return -1;
#ifdef _WIN32
    char dir[MAX_PATH], name[MAX_PATH];
    int fd = -1;
    if (GetTempPathA(sizeof dir, dir) && GetTempFileNameA(dir, "las", 0, name))
        fd = _open(name, _O_RDWR | _O_BINARY | _O_TEMPORARY);
#else
    char name[1100];
    const char *dir = getenv("TMPDIR");
    snprintf(name, sizeof name, "%s/lasrecomp-save-XXXXXX", dir && *dir ? dir : "/tmp");
    int fd = mkstemp(name);
    if (fd >= 0) unlink(name);
#endif
    if (fd >= 0 && !copy_fd(in, fd)) { close(fd); fd = -1; }
    close(in);
    return fd;
}

// the working copy of a save file (made on its first open for writing): its index in fs->pending; -1 for the SD
// card mount, a file not being saved, or (errno set) when the copy can't be made
static int pending_of(HostFs *fs, const char *p, bool create, char *out, size_t n) {
    if (!fs->journaled) return -1;
    snprintf(out, n, "%s.pending", p);
    for (int i = 0; i < fs->npending; i++) if (!strcmp(fs->pending[i], p)) return i;
    if (!create) return -1;
    if (fs->npending == PENDING_MAX) { errno = ENOSPC; return -1; }
    if (strlen(p) >= sizeof fs->pending[0]) { errno = ENAMETOOLONG; return -1; }
    if (!copy_file(p, out)) { int error = errno; unlink(out); errno = error; return -1; }
    fs->failed[fs->npending] = false;
    snprintf(fs->pending[fs->npending], sizeof fs->pending[0], "%s", p);
    return fs->npending++;
}

static bool sync_save_file(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    int error = errno;
    if (close(fd)) return false;
    if (!ok) errno = error;
    return ok;
}
static bool commit_slot(const char *path) {
    char pending[1200], flags[1200], flags_pending[1200];
    snprintf(pending, sizeof pending, "%s.pending", path);
    snprintf(flags, sizeof flags, "%s.flagsx", path);
    snprintf(flags_pending, sizeof flags_pending, "%s.flagsx.pending", path);
    struct stat st;
    bool has_flags = stat(flags_pending, &st) == 0;
    if (!has_flags && errno != ENOENT) return false;
    if (has_flags && !sync_save_file(flags_pending)) return false;
    if (!stat(pending, &st)) {
        if (!sync_save_file(pending) || replace_file(pending, path)) return false;
        sync_dir_of(path);
    } else if (errno != ENOENT || !has_flags) return false;
    // A missing working copy with pending flags means the data rename already succeeded. Finish that pair
    // on retry; never discard its flags or let a fresh mount read the new data with the old flags.
    if (has_flags) {
        if (replace_file(flags_pending, flags)) return false;
        sync_dir_of(flags);
    }
    return true;
}
// a save that won't be committed: its working copies go (the committed file and sidecar stay as they are)
static void drop_slot(const char *path) {
    char pending[1200], flags_pending[1250];
    snprintf(pending, sizeof pending, "%s.pending", path);
    snprintf(flags_pending, sizeof flags_pending, "%s.flagsx.pending", path);
    unlink(pending);
    unlink(flags_pending);
}
// Commit always succeeds for the game (see save_failed). A save that failed is dropped whole. If only the sidecar's
// rename failed (the data is already replaced), its working copy stays for the next mount to finish the pair.
static void commit(HostFs *fs) {
    for (int i = 0; i < fs->npending; i++) {
        const char *path = fs->pending[i];
        if (fs->failed[i]) {
            drop_slot(path);
            save_failed(fs, "dropped the failed save", path, 0);
        } else if (!commit_slot(path)) {
            int error = errno;
            char pending[1200];
            struct stat st;
            snprintf(pending, sizeof pending, "%s.pending", path);
            if (!stat(pending, &st)) {
                drop_slot(path);
                save_failed(fs, "could not commit", path, error);
            } else LOG("[save] committed %s, but not its flags sidecar yet (%s): the next mount finishes it\n", path,
                       strerror(error));
        } else LOG("[save] committed %s\n", path);
    }
    fs->npending = 0;
}
// Recover pairs before dropping abandoned working copies: deleting the data copy first would make an
// uncommitted sidecar look committed. No format change: existing .flagsx files still contain just flag bytes.
static bool recover_save_flags(const char *root) {
    DIR *d = opendir(root);
    if (!d) return false;
    struct dirent *e;
    int error = 0;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n <= 15 || strcmp(e->d_name + n - 15, ".flagsx.pending")) continue;
        char path[1200], pending[1250], flags_pending[1250];
        snprintf(path, sizeof path, "%s/%.*s", root, (int)(n - 15), e->d_name);
        snprintf(pending, sizeof pending, "%s.pending", path);
        snprintf(flags_pending, sizeof flags_pending, "%s.flagsx.pending", path);
        struct stat st;
        if (!stat(pending, &st)) {
            if (unlink(flags_pending)) { error = errno; break; }
        } else if (errno != ENOENT || !commit_slot(path)) { error = errno; break; }
    }
    closedir(d);
    if (error) errno = error;
    return !error;
}
typedef struct { DIR *d; char path[1024]; int mode; } HostDir;

// The game closed the file / directory: close the host one too. They used to stay open for the whole run - a leak
// everywhere, and on Windows the open working copy blocked the Commit's replace, so saves never landed (a file can't
// be replaced while it's open there; the game closes its files before it commits, as nn::fs requires).
static void file_close(Service *s) {
    HostFile *f = s->state;
    if (f) { close(f->fd); free(f); s->state = NULL; }
}
static void dir_close(Service *s) {
    HostDir *d = s->state;
    if (d) { if (d->d) closedir(d->d); free(d); s->state = NULL; }
}

static const char *in_path(IpcReq *req, int i) {
    if (i < req->nx) return (const char *)(uintptr_t)req->x[i].addr;
    if (i < req->na) return (const char *)(uintptr_t)req->a[i].addr;
    return "";
}

static void host_path(HostFs *fs, const char *p, char *out, size_t n) {
    snprintf(out, n, "%s%s%s", fs->root, p[0] == '/' ? "" : "/", p);
}

static uint32_t errno_result(void) {
    if (errno == ENOENT || errno == ENOTDIR) return FS_PATH_NOT_FOUND;
    if (errno == EEXIST) return FS_PATH_EXISTS;
    if (errno == ENOTEMPTY) return FS_NOT_EMPTY;
    return MAKE_RESULT(2, 4000);
}

// a save file's write / flush / resize failed: that save is dropped at the Commit (the game is told it worked)
static void file_failed(HostFile *f, const char *what, int error) {
    if (f->slot >= 0) f->fs->failed[f->slot] = true;
    save_failed(f->fs, what, f->path, error);
}

static void file(Service *s, IpcReq *req, IpcResp *resp) {
    HostFile *f = s->state;
    switch (req->cmd) {
    case 0: {  // Read(option, offset, size) -> B
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *b = out_buf(req, 0);
        if (!b || off < 0 || size < 0) { resp->result = MAKE_RESULT(2, 6001); break; }
        if ((uint64_t)size > b->size) size = (int64_t)b->size;
        plat_write_prepare(b->addr, size);
        ssize_t n = pread(f->fd, (void *)(uintptr_t)b->addr, (size_t)size, off);
        if (n < 0) resp->result = errno_result();
        if (n > 0 && !f->scratch && strstr(f->path, "savedata")) {
            LOG("[save] read %s off %lld size %lld -> guest %llx\n", f->path, (long long)off, (long long)size, (unsigned long long)b->addr);
            extern void save_editor_file_read(const char *path, uint64_t addr, int64_t off, int64_t size);
            save_editor_file_read(f->path, b->addr, off, n);
            extern void flags_ext_save_read(const char *path, uint64_t addr, int64_t off, int64_t size);
            flags_ext_save_read(f->path, b->addr, off, n);
        }
        OUT(int64_t, 0) = n < 0 ? 0 : n;
        resp_size(resp, 8);
        break;
    }
    case 1: {  // Write(option, offset, size) <- A
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *a = in_buf(req, 0);
        if (strstr(f->path, "savedata"))
            LOG("[save] write %s off %lld size %lld <- guest %llx\n", f->path, (long long)off, (long long)size, (unsigned long long)a->addr);
        int error = 0;
        for (int64_t done = 0; done < size;) {  // (a short write goes on from where it stopped)
            ssize_t n = pwrite(f->fd, (void *)(uintptr_t)(a->addr + done), (size_t)(size - done), off + done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { error = n ? errno : EIO; break; }
            done += n;
        }
        if (!error && !f->scratch && strstr(f->path, "savedata")) {
            extern bool flags_ext_save_written(const char *path, uint64_t addr, int64_t off, int64_t size);
            errno = 0;
            if (!flags_ext_save_written(f->path, a->addr, off, size)) error = errno ? errno : EIO;
        }
        if (error && f->fs) file_failed(f, "write", error);
        else if (error) { errno = error; resp->result = errno_result(); }
        break;
    }
    case 2:  // Flush
        if (fsync(f->fd) && f->fs) file_failed(f, "flush", errno);
        break;
    case 3:  // SetSize
        if (!ftruncate(f->fd, IN(int64_t, 0))) break;
        if (f->fs) file_failed(f, "resize", errno);
        else resp->result = errno_result();
        break;
    case 4: {
        struct stat sb;
        fstat(f->fd, &sb);
        OUT(int64_t, 0) = sb.st_size;
        resp_size(resp, 8);
        break;
    }
    default: LOG("[fs] IFile cmd %u (ok)\n", req->cmd);
    }
}

static void directory(Service *s, IpcReq *req, IpcResp *resp) {
    HostDir *d = s->state;
    switch (req->cmd) {
    case 0: {  // Read -> B buffer of 0x310-byte entries
        IpcBuf *b = out_buf(req, 0);
        uint8_t *out = (uint8_t *)(uintptr_t)b->addr;
        int64_t max = (int64_t)(b->size / 0x310), n = 0;
        struct dirent *e;
        while (n < max && (e = readdir(d->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
#ifdef _WIN32  // no d_type
            char full[2048];
            struct stat es;
            snprintf(full, sizeof full, "%s/%s", d->path, e->d_name);
            bool isdir = !stat(full, &es) && S_ISDIR(es.st_mode);
#else
            bool isdir = e->d_type == DT_DIR;
#endif
            if ((isdir && !(d->mode & 1)) || (!isdir && !(d->mode & 2))) continue;
            uint8_t *ent = out + n * 0x310;
            memset(ent, 0, 0x310);
            strncpy((char *)ent, e->d_name, 0x300);
            ent[0x304] = isdir ? 0 : 1;
            if (!isdir) {
                char p[2048];
                struct stat sb;
                snprintf(p, sizeof p, "%s/%s", d->path, e->d_name);
                if (!stat(p, &sb)) *(int64_t *)(ent + 0x308) = sb.st_size;
            }
            n++;
        }
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    case 1: {  // GetEntryCount
        long pos = telldir(d->d);
        rewinddir(d->d);
        int64_t n = 0;
        struct dirent *e;
        while ((e = readdir(d->d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
        seekdir(d->d, pos);
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    }
}

static int rm_rf(const char *p, bool keep_root) {
    DIR *d = opendir(p);
    if (!d) return unlink(p);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[2048];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        rm_rf(c, false);
    }
    closedir(d);
    return keep_root ? 0 : rmdir(p);
}

static void filesystem(Service *s, IpcReq *req, IpcResp *resp) {
    HostFs *fs = s->state;
    char p[2048], p2[2048];
    host_path(fs, in_path(req, 0), p, sizeof p);
    switch (req->cmd) {
    case 0: {  // CreateFile(option, size)
        int fd = open(p, O_CREAT | O_EXCL | O_RDWR, 0644);
        if (fd < 0) { resp->result = errno_result(); break; }
        if (ftruncate(fd, IN(int64_t, 8))) resp->result = errno_result();
        close(fd);
        break;
    }
    case 1: if (unlink(p)) resp->result = errno_result(); break;
    case 2: if (mkdir(p, 0755)) resp->result = errno_result(); break;
    case 3: if (rmdir(p)) resp->result = errno_result(); break;
    case 4: if (rm_rf(p, false)) resp->result = errno_result(); break;
    case 5: case 6:
        host_path(fs, in_path(req, 1), p2, sizeof p2);
        if (rename(p, p2)) resp->result = errno_result();
        break;
    case 7: {  // GetEntryType
        struct stat sb;
        if (stat(p, &sb)) { resp->result = FS_PATH_NOT_FOUND; break; }
        OUT(uint32_t, 0) = S_ISDIR(sb.st_mode) ? 0 : 1;
        resp_size(resp, 4);
        break;
    }
    case 8: {  // OpenFile(mode)
        uint32_t mode = IN(uint32_t, 0);
        bool save = fs->journaled && (mode & 2);
        char work[2100];
        struct stat sb;
        int slot = -1, fd = -1;
        bool scratch = false;
        bool exists = !stat(p, &sb);
        if (exists) slot = pending_of(fs, p, save, work, sizeof work);
        if (slot >= 0) {
            snprintf(p, sizeof p, "%s", work);
        } else if (save && exists) {  // (its working copy can't be made)
            save_failed(fs, "could not stage", p, errno);
            fd = scratch_copy(p);
            scratch = true;
            if (fd < 0) fd = open(p, O_RDONLY);  // (no scratch copy either: the writes fail and are dropped)
        }
        if (fd < 0 && !scratch) fd = open(p, (mode & 2) ? O_RDWR : O_RDONLY);
        if (fd < 0) { resp->result = errno_result(); break; }
        HostFile *f = calloc(1, sizeof *f);
        f->fd = fd;
        snprintf(f->path, sizeof f->path, "%s%s", p, scratch ? " (scratch copy)" : "");
        if (save) { f->fs = fs; f->slot = scratch ? -1 : slot; f->scratch = scratch; }
        Service *fsvc = service_new("IFile", file, f);
        fsvc->on_close = file_close;
        ipc_return_object(resp, fsvc);
        break;
    }
    case 9: {  // OpenDirectory(mode)
        DIR *d = opendir(p);
        if (!d) { resp->result = errno_result(); break; }
        HostDir *hd = calloc(1, sizeof *hd);
        hd->d = d;
        hd->mode = (int)IN(uint32_t, 0);
        snprintf(hd->path, sizeof hd->path, "%s", p);
        Service *dsvc = service_new("IDirectory", directory, hd);
        dsvc->on_close = dir_close;
        ipc_return_object(resp, dsvc);
        break;
    }
    case 10: commit(fs); break;  // Commit (always succeeds for the game: see save_failed)
    case 11: case 12: OUT(int64_t, 0) = 0x40000000; resp_size(resp, 8); break;
    case 13: rm_rf(p, true); break;
    case 14: memset(resp->data, 0, 0x20); resp_size(resp, 0x20); break;
    default: LOG("[fs] IFileSystem cmd %u (ok)\n", req->cmd);
    }
}

static Service *open_host_fs(const char *sub) {
    HostFs *fs = calloc(1, sizeof *fs);
    if (getenv("LARECOMP_SAVE_DIR")) g_save_dir = getenv("LARECOMP_SAVE_DIR");  // test runs use a scratch copy
    else if (rando_seed_dir()) {  // each randomizer seed has its own save slots
        static char seed_save[600];
        snprintf(seed_save, sizeof seed_save, "%s/save", rando_seed_dir());
        g_save_dir = seed_save;
    }
    snprintf(fs->root, sizeof fs->root, "%s/%s", g_save_dir, sub);
#ifdef __ANDROID__
    // Save import swaps directories before PLAY. Recover if Android killed the process between the two renames.
    if (!strcmp(sub, "save_0")) {
        char previous[1200];
        snprintf(previous, sizeof previous, "%s.previous", fs->root);
        struct stat st;
        if (stat(fs->root, &st) != 0 && errno == ENOENT && stat(previous, &st) == 0) {
            if (rename(previous, fs->root) != 0) {
                LOG("[fs] cannot restore save backup %s: %s\n", previous, strerror(errno));
                abort();  // do not silently create empty saves over an unrecovered import
            }
        }
    }
#endif
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "%s", fs->root);
    for (char *q = cmd + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(cmd, 0755); *q = '/'; }
    mkdir(cmd, 0755);
    fs->journaled = strcmp(sub, "sdcard") != 0;
    if (fs->journaled) {  // (a working copy left by a crash: never committed - dropped, as on the Switch)
        if (!recover_save_flags(fs->root)) { int error = errno; free(fs); errno = error; return NULL; }
        DIR *d = opendir(fs->root);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 8 && !strcmp(e->d_name + l - 8, ".pending")) {
                char stale[1300];
                snprintf(stale, sizeof stale, "%s/%s", fs->root, e->d_name);
                LOG("[save] dropping uncommitted %s\n", stale);
                unlink(stale);
            }
        }
        if (d) closedir(d);
    }
    LOG("[fs] mounted host directory %s\n", fs->root);
    return service_new("IFileSystem", filesystem, fs);
}

// ------------------------------------------------------------------ fsp-srv
SERVICE("fsp-srv", fsp_srv) {
    (void)s;
    switch (req->cmd) {
    case 1: break;  // SetCurrentProcess
    case 18: ipc_return_object(resp, open_host_fs("sdcard")); break;
    case 22: break;  // CreateSaveDataFileSystem
    case 51: case 53: {  // OpenSaveDataFileSystem(space, attribute)
        uint8_t type = IN(uint8_t, 0x8 + 0x18);  // attribute.type
        char sub[64];
        snprintf(sub, sizeof sub, "save_%u", type);
        Service *mount = open_host_fs(sub);
        if (mount) ipc_return_object(resp, mount);
        else resp->result = errno_result();
        break;
    }
    case 200: ipc_return_object(resp, open_romfs_storage()); break;  // OpenDataStorageByCurrentProcess
    case 202:  // OpenDataStorageByDataId (system data archives)
        LOG("[fs] OpenDataStorageByDataId %llx (not available)\n", IN(uint64_t, 8));
        resp->result = MAKE_RESULT(2, 1001);
        break;
    case 1005: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetGlobalAccessLogMode
    case 1003: case 1004: case 1006: break;
    case 1011: memset(resp->data, 0, 0x10); resp_size(resp, 0x10); break;
    default: LOG("[fs] fsp-srv cmd %u (ok)\n", req->cmd);
    }
}
