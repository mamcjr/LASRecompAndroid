// See mods.h. Android only (this fork's scope): elsewhere there are none.
#include "mods.h"

#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "hle.h"
#include "settings.h"

static bool is_off(const char *name) {  // (mods_off: names separated by '|')
    size_t n = strlen(name);
    for (const char *s = g_settings.mods_off; *s;) {
        const char *e = strchr(s, '|');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == n && !strncmp(s, name, n)) return true;
        if (!e) break;
        s = e + 1;
    }
    return false;
}

static int by_name(const void *a, const void *b) { return strcmp(((const ModInfo *)a)->name, ((const ModInfo *)b)->name); }

int mods_list(ModInfo *out, int max) {
    DIR *d = opendir("mods");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof out->name) continue;
        char p[256];
        struct stat sb;
        snprintf(p, sizeof p, "mods/%s/romfs", e->d_name);
        if (stat(p, &sb) || !S_ISDIR(sb.st_mode)) continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
        out[n].on = !is_off(e->d_name);
        n++;
    }
    closedir(d);
    qsort(out, n, sizeof *out, by_name);
    return n;
}

void mods_set(const char *name, bool on) {
    char list[sizeof g_settings.mods_off] = "";
    for (const char *s = g_settings.mods_off; *s;) {  // the others as they were
        const char *e = strchr(s, '|');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l && !(l == strlen(name) && !strncmp(s, name, l))) {
            size_t k = strlen(list);
            if (k + l + 2 < sizeof list) snprintf(list + k, sizeof list - k, "%s%.*s", k ? "|" : "", (int)l, s);
        }
        if (!e) break;
        s = e + 1;
    }
    size_t k = strlen(list);
    if (!on && k + strlen(name) + 2 < sizeof list) snprintf(list + k, sizeof list - k, "%s%s", k ? "|" : "", name);
    snprintf(g_settings.mods_off, sizeof g_settings.mods_off, "%s", list);
}

// ---- the files of the mods that are on: (path in the RomFS, host path), sorted by path; the first mod wins
typedef struct { char *rel, *host; int rank; } ModFile;  // (rank: the mod's place by name)
static ModFile *files;
static int nfiles, capfiles;

static void walk(const char *root, const char *rel, int rank) {
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
        if (S_ISDIR(sb.st_mode)) { walk(root, r2, rank); continue; }
        if (nfiles == capfiles) { capfiles = capfiles ? capfiles * 2 : 64; files = realloc(files, sizeof *files * capfiles); }
        files[nfiles++] = (ModFile){strdup(r2), strdup(h2), rank};
    }
    closedir(d);
}

static int by_rel(const void *a, const void *b) {
    int c = strcmp(((const ModFile *)a)->rel, ((const ModFile *)b)->rel);
    return c ? c : ((const ModFile *)a)->rank - ((const ModFile *)b)->rank;
}

void mods_scan(void) {
    for (int i = 0; i < nfiles; i++) free(files[i].rel), free(files[i].host);
    nfiles = 0;
    ModInfo m[MODS_MAX];
    int n = mods_list(m, MODS_MAX);
    for (int i = 0; i < n; i++) {
        if (!m[i].on) { LOG("[mods] %s: off\n", m[i].name); continue; }
        int before = nfiles;
        char root[256];
        snprintf(root, sizeof root, "mods/%s/romfs", m[i].name);
        walk(root, "", i);
        LOG("[mods] %s: %d files\n", m[i].name, nfiles - before);
    }
    qsort(files, nfiles, sizeof *files, by_rel);
}

bool mods_romfs_override(const char *path, char *out, size_t cap) {
    int lo = 0, hi = nfiles - 1, at = -1;
    while (lo <= hi) {  // (the first entry with this path: the lowest-ranked mod)
        int mid = (lo + hi) / 2, c = strcmp(path, files[mid].rel);
        if (c <= 0) { if (!c) at = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    if (at < 0) return false;
    snprintf(out, cap, "%s", files[at].host);
    return true;
}

const char *mods_note(const char *name, char *buf, size_t cap) {
    buf[0] = 0;
    char dirp[256], pick[512] = "";
    snprintf(dirp, sizeof dirp, "mods/%s", name);
    DIR *d = opendir(dirp);
    if (!d) return buf;
    struct dirent *e;
    int best = -1;
    while ((e = readdir(d))) {  // a .txt; the one that looks like the mod's own note first
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".txt")) continue;
        char low[256];
        snprintf(low, sizeof low, "%s", e->d_name);
        for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
        int score = strstr(low, "sobre") || strstr(low, "about") || strstr(low, "readme") || strstr(low, "credit") ? 1 : 0;
        if (score > best) best = score, snprintf(pick, sizeof pick, "%s/%s", dirp, e->d_name);
    }
    closedir(d);
    if (!pick[0]) return buf;
    FILE *f = fopen(pick, "rb");
    if (!f) return buf;
    char raw[2048];
    size_t n = fread(raw, 1, sizeof raw - 1, f), o = 0;
    fclose(f);
    for (size_t i = 0; i < n && o + 3 < cap;) {  // ASCII and U+0080..U+00FF only (the menu font's)
        unsigned char c = (unsigned char)raw[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + len > n) break;
        if (c < 0x80) { if (c != '\r') buf[o++] = (char)c; }
        else if (len == 2 && (c == 0xC2 || c == 0xC3)) buf[o++] = (char)c, buf[o++] = raw[i + 1];
        i += len;
    }
    buf[o] = 0;
    // (symbols dropped at a line's start leave a space: trim them)
    char *w = buf;
    for (char *r = buf; *r;) {
        if ((r == buf || r[-1] == '\n') && *r == ' ') { r++; continue; }
        *w++ = *r++;
    }
    *w = 0;
    return buf;
}
#else
int mods_list(ModInfo *out, int max) { (void)out, (void)max; return 0; }
void mods_set(const char *name, bool on) { (void)name, (void)on; }
void mods_scan(void) {}
bool mods_romfs_override(const char *path, char *out, size_t cap) { (void)path, (void)out, (void)cap; return false; }
const char *mods_note(const char *name, char *buf, size_t cap) { (void)name; if (cap) buf[0] = 0; return buf; }
#endif
