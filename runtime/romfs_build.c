// RomFS image builder (romfs_build.h). Layout as the game's own image: a 0x50-byte header, file data
// from 0x200 (each file 16-byte aligned), then the directory hash table, directory entries, file hash table and file
// entries. Entries are found by hash (the game's RomFS code walks the tables itself), so the order inside them only
// has to be consistent: directories and files are sorted by name, children before siblings' children.
#include "romfs_build.h"

#include "platform.h"  // (Windows: POSIX mkdir)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#define EMPTY 0xFFFFFFFFu
#define DATA_START 0x200u

typedef struct Dir Dir;
typedef struct {
    char *name, *path;  // path: on disk (romfs_build), or relative to the image's root (romfs_build_entries)
    uint64_t size, data_off;
    uint32_t off;  // entry offset in the file table
    int index;     // (romfs_build_entries) the entry's index, for its open callback
    Dir *parent;
} File;
struct Dir {
    char *name, *path;
    Dir *parent;
    Dir **dirs;
    File **files;
    int ndirs, nfiles;
    uint32_t off;  // entry offset in the directory table
};

static int by_name_dir(const void *a, const void *b) { return strcmp((*(Dir *const *)a)->name, (*(Dir *const *)b)->name); }
static int by_name_file(const void *a, const void *b) { return strcmp((*(File *const *)a)->name, (*(File *const *)b)->name); }

static Dir *scan(const char *path, const char *name, Dir *parent, int depth) {
    if (depth > 32) return NULL;
    DIR *d = opendir(path);
    if (!d) return NULL;
    Dir *dir = calloc(1, sizeof *dir);
    dir->name = strdup(name), dir->path = strdup(path), dir->parent = parent;
    int cd = 0, cf = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;  // (".", "..", and hidden files such as .DS_Store aren't the game's)
        size_t n = strlen(path) + strlen(e->d_name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) { free(p); continue; }
        if (S_ISDIR(st.st_mode)) {
            Dir *c = scan(p, e->d_name, dir, depth + 1);
            free(p);
            if (!c) continue;
            if (dir->ndirs == cd) dir->dirs = realloc(dir->dirs, sizeof *dir->dirs * (cd = cd ? cd * 2 : 8));
            dir->dirs[dir->ndirs++] = c;
        } else if (S_ISREG(st.st_mode)) {
            File *f = calloc(1, sizeof *f);
            f->name = strdup(e->d_name), f->path = p, f->size = (uint64_t)st.st_size, f->parent = dir;
            if (dir->nfiles == cf) dir->files = realloc(dir->files, sizeof *dir->files * (cf = cf ? cf * 2 : 16));
            dir->files[dir->nfiles++] = f;
        } else {
            free(p);
        }
    }
    closedir(d);
    if (dir->ndirs) qsort(dir->dirs, dir->ndirs, sizeof *dir->dirs, by_name_dir);
    if (dir->nfiles) qsort(dir->files, dir->nfiles, sizeof *dir->files, by_name_file);
    return dir;
}

static void free_tree(Dir *d) {
    if (!d) return;
    for (int i = 0; i < d->ndirs; i++) free_tree(d->dirs[i]);
    for (int i = 0; i < d->nfiles; i++) free(d->files[i]->name), free(d->files[i]->path), free(d->files[i]);
    free(d->dirs), free(d->files), free(d->name), free(d->path), free(d);
}

// flat lists, in table order
typedef struct { Dir **dirs; File **files; int nd, nf, cd, cf; } Lists;
static void flatten(Dir *d, Lists *l) {
    for (int i = 0; i < d->nfiles; i++) {
        if (l->nf == l->cf) l->files = realloc(l->files, sizeof *l->files * (l->cf = l->cf ? l->cf * 2 : 256));
        l->files[l->nf++] = d->files[i];
    }
    for (int i = 0; i < d->ndirs; i++) {
        if (l->nd == l->cd) l->dirs = realloc(l->dirs, sizeof *l->dirs * (l->cd = l->cd ? l->cd * 2 : 64));
        l->dirs[l->nd++] = d->dirs[i];
    }
    for (int i = 0; i < d->ndirs; i++) flatten(d->dirs[i], l);
}

// the RomFS name hash and bucket count (as the game's RomFS code computes them)
static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = ((h >> 5) | (h << 27)) ^ *p;
    return h;
}
static uint32_t bucket_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint32_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}

static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }
static uint64_t align16(uint64_t x) { return (x + 15) & ~15ull; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

uint64_t romfs_dir_bytes(const char *dir) {
    Dir *t = scan(dir, "", NULL, 0);
    if (!t) return 0;
    Lists l = {0};
    flatten(t, &l);
    uint64_t n = 0;
    for (int i = 0; i < l.nf; i++) n += l.files[i]->size;
    free(l.dirs), free(l.files);
    free_tree(t);
    return n;
}

// Where the file data comes from (romfs_build: paths on disk; romfs_build_entries: descriptors from a callback), and
// which files also get a loose copy under keep_dir.
typedef struct {
    int (*open_fd)(void *ctx, int index);  // NULL: fopen(File.path)
    void *ctx;
    const char *keep_dir;
    bool (*keep)(const char *rel);
} Source;

static bool mkdirs_for(const char *file) {  // the folders a file's path needs
    char p[2048];
    snprintf(p, sizeof p, "%s", file);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    return true;
}

static bool copy_data(FILE *out, const File *f, const Source *src, uint8_t *buf, size_t bufsize, uint64_t *done,
                      uint64_t total, void (*progress)(uint64_t, uint64_t)) {
    FILE *in = NULL;
    if (src && src->open_fd) {
        int fd = src->open_fd(src->ctx, f->index);
        in = fd >= 0 ? fdopen(fd, "rb") : NULL;
        if (!in && fd >= 0) close(fd);
    } else {
        in = fopen(f->path, "rb");
    }
    if (!in) return false;
    FILE *copy = NULL;
    if (src && src->keep && src->keep(f->path)) {
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", src->keep_dir, f->path);
        mkdirs_for(p);
        if (!(copy = fopen(p, "wb"))) { fclose(in); return false; }
    }
    uint64_t left = f->size;
    bool ok = true;
    while (left && ok) {
        size_t want = left < bufsize ? (size_t)left : bufsize;
        size_t got = fread(buf, 1, want, in);
        ok = got == want && fwrite(buf, 1, got, out) == got && (!copy || fwrite(buf, 1, got, copy) == got);
        left -= got;
        *done += got;
        if (progress) progress(*done, total);
    }
    if (ok && src && src->open_fd && fgetc(in) != EOF) ok = false;  // (larger than listed: not the file it was)
    fclose(in);
    if (copy && fclose(copy) != 0) ok = false;
    return ok;
}

static bool write_image(Dir *root, const Source *src, const char *out_path, void (*progress)(uint64_t, uint64_t),
                        char *why, size_t why_size);

#ifdef __ANDROID__  // (the Android setup: launcher.mm nativeUseRomfsImage)
// ---- an image made elsewhere (a romfs.bin the player brings: hactool's --romfs, or ours from another phone). From
// BOTWRecompAndroid (47270e8). Its file tables walked from the root directory: each file's path ("a/b/c"), offset in
// the image and size.
static bool read_full(int fd, void *dst, uint64_t n, uint64_t off) {
    for (uint64_t done = 0; done < n;) {
        ssize_t r = pread(fd, (uint8_t *)dst + done, n - done, (off_t)(off + done));
        if (r <= 0) return false;
        done += (uint64_t)r;
    }
    return true;
}

typedef struct {
    const uint8_t *dt, *ft;  // directory and file entry tables
    uint32_t dsize, fsize;
    uint64_t data;  // where file data starts
    void (*each)(void *ud, const char *path, uint64_t off, uint64_t size);
    void *ud;
    long budget;  // (entries left to visit: a damaged table's loop ends)
} Walk;

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static bool walk_dir(Walk *w, uint32_t doff, char *path, size_t len, int depth) {
    if (depth > 64 || doff > w->dsize - 0x18) return false;
    const uint8_t *d = w->dt + doff;
    for (uint32_t f = get32(d + 12); f != EMPTY; ) {  // its files
        if (--w->budget < 0 || f > w->fsize - 0x20) return false;
        const uint8_t *e = w->ft + f;
        uint32_t nl = get32(e + 28);
        if (nl > w->fsize - f - 0x20 || len + nl + 2 > 4096) return false;
        memcpy(path + len, e + 0x20, nl);
        path[len + nl] = 0;
        w->each(w->ud, path, w->data + get64(e + 8), get64(e + 16));
        f = get32(e + 4);
    }
    for (uint32_t c = get32(d + 8); c != EMPTY; ) {  // its folders
        if (--w->budget < 0 || c > w->dsize - 0x18) return false;
        const uint8_t *e = w->dt + c;
        uint32_t nl = get32(e + 20);
        if (nl > w->dsize - c - 0x18 || len + nl + 2 > 4096) return false;
        memcpy(path + len, e + 0x18, nl);
        path[len + nl] = '/';
        if (!walk_dir(w, c, path, len + nl + 1, depth + 1)) return false;
        c = get32(e + 4);
    }
    return true;
}

bool romfs_image_fd_ok(int fd, char *why, size_t why_size) {
    uint64_t h[10];
    struct stat st;
    if (!read_full(fd, h, sizeof h, 0) || fstat(fd, &st) != 0 || h[0] != 0x50) {
        if (why) snprintf(why, why_size, "It isn't a RomFS image (no 0x50 header).");
        return false;
    }
    uint64_t end = h[9];
    for (int i = 1; i < 9; i += 2) {
        if (h[i] > (1ull << 40) || h[i + 1] > (1ull << 40)) {
            if (why) snprintf(why, why_size, "Its header is damaged.");
            return false;
        }
        if (h[i] + h[i + 1] > end) end = h[i] + h[i + 1];
    }
    if ((uint64_t)st.st_size < end) {
        if (why) snprintf(why, why_size, "It has %.2f of its %.2f GB: copy it again.", st.st_size / 1e9, end / 1e9);
        return false;
    }
    return true;
}

bool romfs_image_files(int fd, void (*each)(void *ud, const char *path, uint64_t off, uint64_t size), void *ud,
                       char *why, size_t why_size) {
    uint64_t h[10];
    if (!romfs_image_fd_ok(fd, why, why_size) || !read_full(fd, h, sizeof h, 0)) return false;
    if (h[4] < 0x18 || h[8] < 0x20 || h[4] > (256u << 20) || h[8] > (256u << 20)) {
        if (why) snprintf(why, why_size, "Its tables are damaged.");
        return false;
    }
    uint8_t *dt = malloc(h[4]), *ft = malloc(h[8]);
    bool ok = dt && ft && read_full(fd, dt, h[4], h[3]) && read_full(fd, ft, h[8], h[7]);
    if (ok) {
        Walk w = {dt, ft, (uint32_t)h[4], (uint32_t)h[8], h[9], each, ud, (long)(h[4] / 0x18 + h[8] / 0x20)};
        char path[4096];
        ok = walk_dir(&w, 0, path, 0, 0);
    }
    if (!ok && why) snprintf(why, why_size, "Its file tables can't be read.");
    free(dt), free(ft);
    return ok;
}

// The files keep(path) picks, out of the image into keep_dir/path (the few the runtime opens by path)
typedef struct { int fd; const char *dir; bool (*keep)(const char *); bool ok; char *why; size_t why_size; } ImgOut;
static void img_out_one(void *ud, const char *path, uint64_t off, uint64_t size) {
    ImgOut *o = ud;
    if (!o->ok || !o->keep(path)) return;
    char dst[4096];
    snprintf(dst, sizeof dst, "%s/%s", o->dir, path);
    mkdirs_for(dst);
    FILE *f = fopen(dst, "wb");
    static uint8_t buf[1 << 20];  // (the setup's one thread)
    bool good = f != NULL;
    for (uint64_t done = 0; good && done < size;) {
        uint64_t k = size - done < sizeof buf ? size - done : sizeof buf;
        good = read_full(o->fd, buf, k, off + done) && fwrite(buf, 1, k, f) == k;
        done += k;
    }
    if (f && fclose(f) != 0) good = false;
    if (good) return;
    o->ok = false;
    if (o->why) snprintf(o->why, o->why_size, "Can't write %s.", dst);
}

bool romfs_image_extract(int fd, const char *keep_dir, bool (*keep)(const char *rel), char *why, size_t why_size) {
    ImgOut o = {fd, keep_dir, keep, true, why, why_size};
    return romfs_image_files(fd, img_out_one, &o, why, why_size) && o.ok;
}
#endif

bool romfs_build(const char *dir, const char *out_path, void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    Dir *root = scan(dir, "", NULL, 0);
    if (!root) {
        if (why) snprintf(why, why_size, "can't read %s", dir);
        return false;
    }
    bool ok = write_image(root, NULL, out_path, progress, why, why_size);
    free_tree(root);
    return ok;
}

// The tree of romfs_build_entries' relative paths ("a/b/c"), sorted as scan() sorts a folder.
static Dir *tree_child(Dir *d, const char *name) {
    for (int i = 0; i < d->ndirs; i++)
        if (!strcmp(d->dirs[i]->name, name)) return d->dirs[i];
    Dir *c = calloc(1, sizeof *c);
    c->name = strdup(name), c->path = strdup(""), c->parent = d;
    d->dirs = realloc(d->dirs, sizeof *d->dirs * (d->ndirs + 1));
    d->dirs[d->ndirs++] = c;
    return c;
}
static void tree_sort(Dir *d) {
    if (d->ndirs) qsort(d->dirs, d->ndirs, sizeof *d->dirs, by_name_dir);
    if (d->nfiles) qsort(d->files, d->nfiles, sizeof *d->files, by_name_file);
    for (int i = 0; i < d->ndirs; i++) tree_sort(d->dirs[i]);
}

bool romfs_build_entries(const RomfsEntry *entries, int n, int (*open_fd)(void *ctx, int index), void *ctx,
                         const char *out_path, const char *keep_dir, bool (*keep)(const char *rel),
                         void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    Dir *root = calloc(1, sizeof *root);
    root->name = strdup(""), root->path = strdup("");
    for (int i = 0; i < n; i++) {
        char rel[1024];
        snprintf(rel, sizeof rel, "%s", entries[i].path);
        Dir *d = root;
        char *part = rel, *slash;
        bool hidden = false;
        for (char *s = rel; *s; s++)  // (as scan(): hidden files and folders aren't the game's - nor their folders)
            if ((s == rel || s[-1] == '/') && *s == '.') hidden = true;
        if (hidden) continue;
        while ((slash = strchr(part, '/'))) {
            *slash = 0;
            if (*part) d = tree_child(d, part);
            part = slash + 1;
        }
        if (!*part) continue;
        File *f = calloc(1, sizeof *f);
        f->name = strdup(part), f->path = strdup(entries[i].path), f->size = entries[i].size, f->index = i, f->parent = d;
        d->files = realloc(d->files, sizeof *d->files * (d->nfiles + 1));
        d->files[d->nfiles++] = f;
    }
    tree_sort(root);
    Source src = {open_fd, ctx, keep_dir, keep};
    bool ok = write_image(root, &src, out_path, progress, why, why_size);
    free_tree(root);
    return ok;
}

// An image's tables (write_image writes them, romfs_virtual_open keeps them): l lists the directories and files in table
// order, each file with its data_off (from DATA_START); the header's offsets go to hv
typedef struct {
    Lists l;
    uint32_t dsize, fsize, nbd, nbf;
    uint8_t *dtab, *ftab;
    uint32_t *dbuckets, *fbuckets;
    uint64_t data, total, hv[10];
} Tables;

static void tables_free(Tables *t) {
    free(t->dtab), free(t->ftab), free(t->dbuckets), free(t->fbuckets), free(t->l.dirs), free(t->l.files);
    memset(t, 0, sizeof *t);
}

static bool make_tables(Dir *root, Tables *t) {
    memset(t, 0, sizeof *t);
    Lists l = {0};
    // table order: root, then each directory's subdirectories as flatten() lists them; files likewise
    l.dirs = realloc(l.dirs, sizeof *l.dirs * (l.cd = 64));
    l.dirs[l.nd++] = root;
    flatten(root, &l);
    t->l = l;
    if (!l.nf) return false;

    // entry offsets
    uint32_t dsize = 0, fsize = 0;
    for (int i = 0; i < l.nd; i++) l.dirs[i]->off = dsize, dsize += 0x18 + align4((uint32_t)strlen(l.dirs[i]->name));
    for (int i = 0; i < l.nf; i++) l.files[i]->off = fsize, fsize += 0x20 + align4((uint32_t)strlen(l.files[i]->name));
    // file data offsets (relative to the data start), each 16-byte aligned
    uint64_t data = 0, total = 0;
    for (int i = 0; i < l.nf; i++) {
        data = align16(data);
        l.files[i]->data_off = data;
        data += l.files[i]->size;
        total += l.files[i]->size;
    }

    uint32_t nbd = bucket_count((uint32_t)l.nd), nbf = bucket_count((uint32_t)l.nf);
    uint32_t *dbuckets = malloc(4 * nbd), *fbuckets = malloc(4 * nbf);
    uint8_t *dtab, *ftab;
    for (uint32_t i = 0; i < nbd; i++) dbuckets[i] = EMPTY;
    for (uint32_t i = 0; i < nbf; i++) fbuckets[i] = EMPTY;
    dtab = calloc(1, dsize), ftab = calloc(1, fsize);
    for (int i = 0; i < l.nd; i++) {
        Dir *d = l.dirs[i];
        uint8_t *e = dtab + d->off;
        uint32_t parent = d->parent ? d->parent->off : 0, nl = (uint32_t)strlen(d->name);
        uint32_t sib = EMPTY;
        if (d->parent)
            for (int k = 0; k + 1 < d->parent->ndirs; k++)
                if (d->parent->dirs[k] == d) sib = d->parent->dirs[k + 1]->off;
        uint32_t h = name_hash(parent, d->name) % nbd;
        put32(e, parent);
        put32(e + 4, sib);
        put32(e + 8, d->ndirs ? d->dirs[0]->off : EMPTY);
        put32(e + 12, d->nfiles ? d->files[0]->off : EMPTY);
        put32(e + 16, dbuckets[h]);
        put32(e + 20, nl);
        memcpy(e + 0x18, d->name, nl);
        dbuckets[h] = d->off;
    }
    for (int i = 0; i < l.nf; i++) {
        File *f = l.files[i];
        uint8_t *e = ftab + f->off;
        uint32_t nl = (uint32_t)strlen(f->name), sib = EMPTY;
        for (int k = 0; k + 1 < f->parent->nfiles; k++)
            if (f->parent->files[k] == f) sib = f->parent->files[k + 1]->off;
        uint32_t h = name_hash(f->parent->off, f->name) % nbf;
        put32(e, f->parent->off);
        put32(e + 4, sib);
        put64(e + 8, f->data_off);
        put64(e + 16, f->size);
        put32(e + 24, fbuckets[h]);
        put32(e + 28, nl);
        memcpy(e + 0x20, f->name, nl);
        fbuckets[h] = f->off;
    }

    // header: tables after the file data (4-byte aligned)
    uint64_t dhash_off = (DATA_START + data + 3) & ~3ull;
    uint64_t dmeta_off = dhash_off + 4ull * nbd, fhash_off = dmeta_off + dsize, fmeta_off = fhash_off + 4ull * nbf;
    uint64_t hv[10] = {0x50, dhash_off, 4ull * nbd, dmeta_off, dsize, fhash_off, 4ull * nbf, fmeta_off, fsize, DATA_START};
    memcpy(t->hv, hv, sizeof hv);
    t->dsize = dsize, t->fsize = fsize, t->nbd = nbd, t->nbf = nbf, t->dtab = dtab, t->ftab = ftab;
    t->dbuckets = dbuckets, t->fbuckets = fbuckets, t->data = data, t->total = total;
    return true;
}

static bool write_image(Dir *root, const Source *src, const char *out_path, void (*progress)(uint64_t, uint64_t),
                        char *why, size_t why_size) {
    #define FAIL(...) do { if (why) snprintf(why, why_size, __VA_ARGS__); goto fail; } while (0)
    bool ok = false;
    FILE *out = NULL;
    uint8_t *buf = NULL;
    Tables t;
    if (!make_tables(root, &t)) FAIL("no game files found");
    Lists l = t.l;
    uint64_t total = t.total, dhash_off = t.hv[1];
    uint32_t nbd = t.nbd, nbf = t.nbf, dsize = t.dsize, fsize = t.fsize;
    uint8_t hdr[DATA_START] = {0};
    memcpy(hdr, t.hv, sizeof t.hv);

    out = fopen(out_path, "wb");
    if (!out) FAIL("can't write %s", out_path);
    if (fwrite(hdr, 1, DATA_START, out) != DATA_START) FAIL("write failed (disk full?)");
    size_t bufsize = 4u << 20;
    buf = malloc(bufsize);
    uint64_t pos = 0, done = 0;
    static const uint8_t zero[16];
    for (int i = 0; i < l.nf; i++) {
        File *f = l.files[i];
        if (f->data_off > pos && fwrite(zero, 1, (size_t)(f->data_off - pos), out) != f->data_off - pos)
            FAIL("write failed (disk full?)");
        if (!copy_data(out, f, src, buf, bufsize, &done, total, progress)) FAIL("can't read %s", f->path);
        pos = f->data_off + f->size;
    }
    uint64_t tail = dhash_off - DATA_START - pos;
    if (tail && fwrite(zero, 1, (size_t)tail, out) != tail) FAIL("write failed (disk full?)");
    if (fwrite(t.dbuckets, 4, nbd, out) != nbd || fwrite(t.dtab, 1, dsize, out) != dsize ||
        fwrite(t.fbuckets, 4, nbf, out) != nbf || fwrite(t.ftab, 1, fsize, out) != fsize)
        FAIL("write failed (disk full?)");
    if (fclose(out) != 0) { out = NULL; FAIL("write failed (disk full?)"); }
    out = NULL;
    ok = true;
fail:
    if (out) fclose(out);
    if (!ok) remove(out_path);
    free(buf);
    tables_free(&t);
    return ok;
    #undef FAIL
}

// ------------------------------------------------------------------ the image of a folder, unwritten (romfs_build.h)
#ifdef __ANDROID__  // (the game files used in place: Android's setup)
#define VFD_SLOTS 16
struct RomfsVirtual {
    Dir *root;
    Tables t;
    uint8_t *head, *tail;  // [0, DATA_START) and the tables, [hv[1], size)
    uint64_t size;
    struct { int file, fd; uint64_t used; } fds[VFD_SLOTS];  // (the files open lately)
    uint64_t clock;
    pthread_mutex_t lock;  // (reads from several threads: the open files are shared)
};

RomfsVirtual *romfs_virtual_open(const char *dir, char *why, size_t why_size) {
    Dir *root = scan(dir, "", NULL, 0);
    if (!root) {
        if (why) snprintf(why, why_size, "can't read %s", dir);
        return NULL;
    }
    RomfsVirtual *v = calloc(1, sizeof *v);
    v->root = root;
    if (!make_tables(root, &v->t)) {
        if (why) snprintf(why, why_size, "no game files found in %s", dir);
        romfs_virtual_close(v);
        return NULL;
    }
    const uint64_t *hv = v->t.hv;
    v->head = calloc(1, DATA_START);
    memcpy(v->head, hv, sizeof v->t.hv);
    v->size = hv[7] + hv[8];
    uint64_t tl = v->size - hv[1];
    v->tail = calloc(1, tl);
    memcpy(v->tail, v->t.dbuckets, 4ull * v->t.nbd);
    memcpy(v->tail + (hv[3] - hv[1]), v->t.dtab, v->t.dsize);
    memcpy(v->tail + (hv[5] - hv[1]), v->t.fbuckets, 4ull * v->t.nbf);
    memcpy(v->tail + (hv[7] - hv[1]), v->t.ftab, v->t.fsize);
    for (int i = 0; i < VFD_SLOTS; i++) v->fds[i].file = -1, v->fds[i].fd = -1;
    pthread_mutex_init(&v->lock, NULL);
    return v;
}

uint64_t romfs_virtual_size(const RomfsVirtual *v) { return v->size; }

void romfs_virtual_close(RomfsVirtual *v) {
    if (!v) return;
    for (int i = 0; i < VFD_SLOTS; i++)
        if (v->fds[i].fd >= 0) close(v->fds[i].fd);
    tables_free(&v->t);
    free_tree(v->root);
    free(v->head), free(v->tail), free(v);
}

static int virtual_fd(RomfsVirtual *v, int file) {
    int lru = 0;
    for (int i = 0; i < VFD_SLOTS; i++) {
        if (v->fds[i].file == file) { v->fds[i].used = ++v->clock; return v->fds[i].fd; }
        if (v->fds[i].used < v->fds[lru].used) lru = i;
    }
    if (v->fds[lru].fd >= 0) close(v->fds[lru].fd);
    v->fds[lru].file = file;
    v->fds[lru].fd = open(v->t.l.files[file]->path, O_RDONLY);
    v->fds[lru].used = ++v->clock;
    return v->fds[lru].fd;
}

static int64_t virtual_read(RomfsVirtual *v, void *dst, uint64_t n, uint64_t off);
int64_t romfs_virtual_read(RomfsVirtual *v, void *dst, uint64_t n, uint64_t off) {
    pthread_mutex_lock(&v->lock);
    int64_t r = virtual_read(v, dst, n, off);
    pthread_mutex_unlock(&v->lock);
    return r;
}

static int64_t virtual_read(RomfsVirtual *v, void *dst, uint64_t n, uint64_t off) {
    uint8_t *d = dst;
    uint64_t done = 0, tables = v->t.hv[1];
    while (done < n) {
        uint64_t o = off + done, left = n - done;
        if (o >= v->size) { memset(d + done, 0, left); break; }
        if (o < DATA_START) {
            uint64_t k = DATA_START - o < left ? DATA_START - o : left;
            memcpy(d + done, v->head + o, k);
            done += k;
            continue;
        }
        if (o >= tables) {
            uint64_t k = v->size - o < left ? v->size - o : left;
            memcpy(d + done, v->tail + (o - tables), k);
            done += k;
            continue;
        }
        // file data: the last file starting at or before o (files are in data order)
        uint64_t rel = o - DATA_START;
        int lo = 0, hi = v->t.l.nf - 1, at = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (v->t.l.files[mid]->data_off <= rel) at = mid, lo = mid + 1;
            else hi = mid - 1;
        }
        File *f = at >= 0 ? v->t.l.files[at] : NULL;
        uint64_t next = at + 1 < v->t.l.nf ? DATA_START + v->t.l.files[at + 1]->data_off : tables;
        if (!f || rel >= f->data_off + f->size) {  // (alignment padding between files)
            uint64_t k = next - o < left ? next - o : left;
            memset(d + done, 0, k);
            done += k;
            continue;
        }
        uint64_t in = rel - f->data_off, k = f->size - in < left ? f->size - in : left;
        int fd = virtual_fd(v, at);
        ssize_t r = fd >= 0 ? pread(fd, d + done, k, (off_t)in) : -1;
        if (r <= 0) return -1;
        done += (uint64_t)r;
    }
    return (int64_t)done;
}


bool romfs_link_read(const char *root, char *out, size_t size) {
    char p[1100];
    snprintf(p, sizeof p, "%s/romfs.link", root);
    FILE *f = fopen(p, "rb");
    if (!f || !size) {
        if (f) fclose(f);
        return false;
    }
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return n > 0;
}
#endif

// ------------------------------------------------------------------ copying the player's folder
static bool copy_file(const char *src, const char *dst, uint64_t size, uint64_t *done, uint64_t total,
                      void (*progress)(uint64_t, uint64_t)) {
    remove(dst);
#ifdef __APPLE__
    if (clonefile(src, dst, 0) == 0) goto linked;  // (APFS: a copy-on-write clone, instant)
#endif
#ifdef _WIN32
    if (CreateHardLinkA(dst, src, NULL)) goto linked;  // (same NTFS volume: a hard link)
#else
    if (link(src, dst) == 0) goto linked;  // (same volume: a hard link)
#endif
    {
        FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
        bool ok = in && out;
        static uint8_t buf[1 << 20];
        while (ok) {
            size_t n = fread(buf, 1, sizeof buf, in);
            if (!n) break;
            if (fwrite(buf, 1, n, out) != n) ok = false;
            *done += n;
            if (progress) progress(*done, total);
        }
        if (in) fclose(in);
        if (out && fclose(out) != 0) ok = false;
        return ok;
    }
linked:
    *done += size;
    if (progress) progress(*done, total);
    return true;
}

static bool copy_dir(Dir *d, const char *dst, uint64_t *done, uint64_t total, void (*progress)(uint64_t, uint64_t)) {
    mkdir(dst, 0755);
    for (int i = 0; i < d->nfiles; i++) {
        size_t n = strlen(dst) + strlen(d->files[i]->name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", dst, d->files[i]->name);
        bool ok = copy_file(d->files[i]->path, p, d->files[i]->size, done, total, progress);
        free(p);
        if (!ok) return false;
    }
    for (int i = 0; i < d->ndirs; i++) {
        size_t n = strlen(dst) + strlen(d->dirs[i]->name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", dst, d->dirs[i]->name);
        bool ok = copy_dir(d->dirs[i], p, done, total, progress);
        free(p);
        if (!ok) return false;
    }
    return true;
}

bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t, uint64_t)) {
    Dir *t = scan(src, "", NULL, 0);
    if (!t) return false;
    Lists l = {0};
    flatten(t, &l);
    uint64_t total = 0, done = 0;
    for (int i = 0; i < l.nf; i++) total += l.files[i]->size;
    bool ok = copy_dir(t, dst, &done, total, progress);
    free(l.dirs), free(l.files);
    free_tree(t);
    return ok;
}
