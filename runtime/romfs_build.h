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

// An image made elsewhere (a romfs.bin the player brings, read through a descriptor: the file picker's on Android).
// romfs_image_fd_ok: its header, and the file long enough for it. romfs_image_files: each file's path ("a/b/c"), offset
// in the image and size, from its tables. romfs_image_extract: the files keep(rel) picks, written under keep_dir/rel.
// false = why says what's wrong.
bool romfs_image_fd_ok(int fd, char *why, size_t why_size);
bool romfs_image_files(int fd, void (*each)(void *ud, const char *path, uint64_t off, uint64_t size), void *ud,
                       char *why, size_t why_size);
bool romfs_image_extract(int fd, const char *keep_dir, bool (*keep)(const char *rel), char *why, size_t why_size);

// The image of the folder `dir` without writing it: its header and tables in memory, each file's data read from the
// folder where it lies (Android: the game files used in place, no copy - runtime/services/fs.c, romfs.link).
// romfs_virtual_read reads image bytes [off, off + n) as romfs_build's image would hold them; the files are opened as
// they're first read (a few kept open). NULL = failed (why). From BOTWRecompAndroid (f1d15ba).
typedef struct RomfsVirtual RomfsVirtual;
RomfsVirtual *romfs_virtual_open(const char *dir, char *why, size_t why_size);
int64_t romfs_virtual_read(RomfsVirtual *v, void *dst, uint64_t n, uint64_t off);
uint64_t romfs_virtual_size(const RomfsVirtual *v);
void romfs_virtual_close(RomfsVirtual *v);

// <root>/romfs.link (Android: the game files used in place): the path of the player's own romfs/ folder or romfs.bin
// it names, into out. false: there's none (the files are copied).
bool romfs_link_read(const char *root, char *out, size_t size);

// Bytes of file data under dir (the image is about this plus its tables).
uint64_t romfs_dir_bytes(const char *dir);

// Copies the folder tree src to dst (hard links where the file system allows: no second copy of 6 GB). progress as
// above. false = failed.
bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));

#ifdef __cplusplus
}
#endif
