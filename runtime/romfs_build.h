// RomFS image from a folder of game files (runtime/romfs_build.c). The game reads its data through one raw RomFS
// image (romfs.bin, runtime/services/fs.c); players bring their own game files, and the start screen builds the image
// from their romfs/ folder.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes the RomFS image of the folder `dir` to `out`. progress (may be NULL): bytes of file data written so far and
// the total, now and then. false = failed (why: a short message).
bool romfs_build(const char *dir, const char *out, void (*progress)(uint64_t done, uint64_t total), char *why,
                 size_t why_size);

// The same image from a list of files (relative paths "a/b/c" and sizes) whose data open_fd(ctx, index) hands over as
// a readable descriptor (closed here) - Android: straight from the picked folder, no copy of it first. The files
// keep(rel) picks are also written loose under keep_dir/rel (the few the runtime opens by path). keep may be NULL.
typedef struct {
    const char *path;
    uint64_t size;
} RomfsEntry;
bool romfs_build_entries(const RomfsEntry *entries, int n, int (*open_fd)(void *ctx, int index), void *ctx,
                         const char *out, const char *keep_dir, bool (*keep)(const char *rel),
                         void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size);

// Bytes of file data under dir (the image is about this plus its tables).
uint64_t romfs_dir_bytes(const char *dir);

// Copies the folder tree src to dst (hard links where the file system allows: no second copy of 6 GB). progress as
// above. false = failed.
bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));

#ifdef __cplusplus
}
#endif
