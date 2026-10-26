// The game-address tables of every supported game version and their selection by build ID (game_addresses.h).
#include "game_addresses.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define GA_ADDR(name, value) .name = UINT64_C(value),
static const GameAddrs ga_v1_0_0 = {
#include "game_addresses/patches_v1_0_0.h"
#include "game_addresses/runtime_v1_0_0.h"
};
static const GameAddrs ga_v1_0_1 = {
#include "game_addresses/patches_v1_0_1.h"
#include "game_addresses/runtime_v1_0_1.h"
};
#undef GA_ADDR

static const GameVersion versions[] = {
#define GA_VERSION(key, label, build_id) {#key, label, build_id, &ga_##key},
#define GA_RELEASE(key, label, npdm_sha256)
#define GA_MOVED(key, from, to, delta)
#include "game_addresses/versions.h"
#undef GA_VERSION
#undef GA_RELEASE
#undef GA_MOVED
};

static const struct { const char *key, *label, *npdm_sha256; } releases[] = {
#define GA_VERSION(key, label, build_id)
#define GA_RELEASE(key, label, npdm_sha256) {#key, label, npdm_sha256},
#define GA_MOVED(key, from, to, delta)
#include "game_addresses/versions.h"
#undef GA_VERSION
#undef GA_RELEASE
#undef GA_MOVED
};

static const struct { const char *key; uint64_t from, to, delta; } moves[] = {
#define GA_VERSION(key, label, build_id)
#define GA_RELEASE(key, label, npdm_sha256)
#define GA_MOVED(key, from, to, delta) {#key, UINT64_C(from), UINT64_C(to), UINT64_C(delta)},
#include "game_addresses/versions.h"
#undef GA_VERSION
#undef GA_RELEASE
#undef GA_MOVED
};

const GameAddrs *g_ga;
static const GameVersion *g_version;
#ifdef __ANDROID__
static GameAddrs g_moved;  // the selected table with its guest addresses in the guest window (rt.h GUEST_LO)
#endif

static int hexval(char c) { return c <= '9' ? c - '0' : c - 'A' + 10; }

const GameVersion *game_version_by_build_id(const uint8_t id[20]) {
    for (size_t i = 0; i < sizeof versions / sizeof versions[0]; i++) {
        const char *h = versions[i].build_id;
        int k = 0;
        while (k < 20 && id[k] == (uint8_t)(hexval(h[2 * k]) << 4 | hexval(h[2 * k + 1]))) k++;
        if (k == 20) return &versions[i];
    }
    return NULL;
}

// The main program's build ID (0x40 in the file). false: missing or not a program.
static bool read_build_id(const char *path, uint8_t id[20]) {
    uint8_t hdr[0x60];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4);
    fclose(f);
    if (ok) memcpy(id, hdr + 0x40, 20);
    return ok;
}

const GameVersion *game_version_of_main(const char *main_path) {
    uint8_t id[20];
    return read_build_id(main_path, id) ? game_version_by_build_id(id) : NULL;
}

const GameVersion *game_version_of(const char *game_dir) {
    char path[1100];
    snprintf(path, sizeof path, "%s/exefs/main", game_dir);
    return game_version_of_main(path);
}

// ---- SHA-256 (FIPS 180-4), for main.npdm only: the release label
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

// The SHA-256 of a file as 64 lower-case hex digits; false when it is missing or larger than an npdm can be.
static bool file_sha256(const char *path, char hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[128];
    uint64_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, 64, f)) == 64 && (total += 64) <= (1u << 20)) sha256_block(h, buf);
    bool ok = !ferror(f) && total <= (1u << 20);
    fclose(f);
    if (!ok) return false;
    total += n;
    buf[n++] = 0x80;
    size_t end = n <= 56 ? 64 : 128;
    memset(buf + n, 0, end - n);
    for (int i = 0; i < 8; i++) buf[end - 1 - i] = (uint8_t)((total * 8) >> (8 * i));
    sha256_block(h, buf);
    if (end == 128) sha256_block(h, buf + 64);
    for (int i = 0; i < 8; i++) snprintf(hex + 8 * i, 9, "%08x", h[i]);
    return true;
}

const char *game_release_label(const char *exefs_dir, char *buf, size_t size) {
    char path[1100];
    snprintf(path, sizeof path, "%s/main", exefs_dir);
    const GameVersion *v = game_version_of_main(path);
    if (!v) return NULL;
    snprintf(buf, size, "%s", v->label);
    bool shared = false;
    for (size_t i = 0; i < sizeof releases / sizeof releases[0]; i++) shared |= !strcmp(releases[i].key, v->key);
    char sha[65];
    snprintf(path, sizeof path, "%s/main.npdm", exefs_dir);
    if (!shared || !file_sha256(path, sha)) return buf;
    for (size_t i = 0; i < sizeof releases / sizeof releases[0]; i++)
        if (!strcmp(releases[i].key, v->key) && !strcmp(sha, releases[i].npdm_sha256)) snprintf(buf, size, "%s", releases[i].label);
    return buf;
}

uint64_t game_version_from_v1_0_0(const GameVersion *v, uint64_t a) {
    for (size_t i = 0; v && i < sizeof moves / sizeof moves[0]; i++)
        if (!strcmp(moves[i].key, v->key) && a >= moves[i].from && a < moves[i].to) return a + moves[i].delta;
    return a;
}

bool game_version_select(const char *game_dir, char *why, size_t why_size) {
    uint8_t id[20];
    char exefs[1100], path[1100];
    snprintf(exefs, sizeof exefs, "%s/exefs", game_dir);
    snprintf(path, sizeof path, "%s/main", exefs);
    if (!read_build_id(path, id)) {
        snprintf(why, why_size, "the game files' exefs/main is missing or unreadable");
        return false;
    }
    const GameVersion *v = game_version_by_build_id(id);
    if (!v) {
        snprintf(why, why_size, "these game files are an unsupported version of the game (main build ID "
                 "%02X%02X%02X%02X%02X%02X%02X%02X): LAS Recomp needs version 1.0.0, 1.0.1 or 1.1.0",
                 id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
        return false;
    }
    g_version = v;
    g_ga = v->addrs;
#ifdef __ANDROID__
    // (the tables hold the desktop's addresses, GUEST_CANON..: the game is loaded at GUEST_LO here; sizes stay)
    g_moved = *v->addrs;
    for (uint64_t *p = (uint64_t *)&g_moved; p < (uint64_t *)(&g_moved + 1); p++)
        if (*p >= GUEST_CANON && *p < GUEST_CANON + (GUEST_HI - GUEST_LO)) *p += GUEST_LO - GUEST_CANON;
    g_ga = &g_moved;
#endif
    char label[32];
    LOG("[game] version %s (main build ID %.16s, address table %s)\n",
        game_release_label(exefs, label, sizeof label), v->build_id, v->key);
    return true;
}

const GameVersion *game_version(void) { return g_version; }

// (from upstream LASRecomp beta_5.3: 14c1f9b, 1f93424) The game's RomFS by its totals and file by file.
uint64_t game_romfs_bytes(const GameVersion *v) {
    if (!v) return 0;
    if (!strcmp(v->key, "v1_0_0")) return 6232003695ull;
    if (!strcmp(v->key, "v1_0_1")) return 6232003903ull;  // (1.0.1 and 1.1.0 have the same RomFS)
    return 0;
}

static const struct { uint64_t hash; uint8_t versions; } romfs_files[] = {
#include "game_addresses/romfs_files.h"
};
enum { ROMFS_ENTRIES = sizeof romfs_files / sizeof *romfs_files };
struct GameRomfsCheck {
    uint8_t version;              // romfs_files' bit for the version
    uint8_t seen[ROMFS_ENTRIES];  // (a file can be passed once: a folder has one file of a name)
};

GameRomfsCheck *game_romfs_check_begin(const GameVersion *v) {
    uint8_t bit = !v ? 0 : !strcmp(v->key, "v1_0_0") ? 1 : !strcmp(v->key, "v1_0_1") ? 2 : 0;
    if (!bit) return NULL;
    GameRomfsCheck *c = calloc(1, sizeof *c);
    if (c) c->version = bit;
    return c;
}

uint64_t game_romfs_hash(const char *rel, uint64_t size) {
    char digits[24];
    snprintf(digits, sizeof digits, "%llu", (unsigned long long)size);
    uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a 64 of "<rel>\0<size>", as tools/romfs_files.py
    for (const unsigned char *p = (const unsigned char *)rel; ; p++) {
        h = (h ^ *p) * 0x100000001b3ull;
        if (!*p) break;
    }
    for (const char *p = digits; *p; p++) h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
    return h;
}

bool game_romfs_check_file(GameRomfsCheck *c, const char *rel, uint64_t size) {
    if (!c) return true;
    uint64_t h = game_romfs_hash(rel, size);
    size_t lo = 0, hi = ROMFS_ENTRIES;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (romfs_files[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    if (lo == ROMFS_ENTRIES || romfs_files[lo].hash != h || !(romfs_files[lo].versions & c->version)) return false;
    c->seen[lo] = 1;
    return true;
}

int game_romfs_check_end(GameRomfsCheck *c) {
    if (!c) return 0;
    int missing = 0;
    for (size_t i = 0; i < ROMFS_ENTRIES; i++) missing += (romfs_files[i].versions & c->version) && !c->seen[i];
    free(c);
    return missing;
}

bool game_romfs_exefs_name(const char *rel) {
    static const char *const names[] = {"main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(rel, names[i])) return true;
    return false;
}
