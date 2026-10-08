// The MicroPython filesystem in the vfs partition, opened by the recovery image.
//
// MicroPython formats vfs as LittleFS v2 (ports/esp32/modules/inisetup.py, vfs.VfsLfs2) with the
// VfsLfs2 defaults: read and prog size 32, lookahead 32, block cycles 100, cache 128, block size
// 4096 from the partition (extmod/vfs_lfs.c, extmod/vfs_lfsx.c). The same lfs2.c from
// MicroPython's lib/littlefs is built here with the same configuration, so both images read and
// write the same blocks.
//
// The filesystem is mounted for one request at a time and unmounted after it: no cached state
// outlives a request, so nothing is left half-written for MicroPython to find. An image that owns
// vfs alone for as long as it runs keeps it mounted instead (app_fs_keep_mounted): a mount walks
// every directory of the tree, and one per request held the Rust app's network for seconds.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "lfs2.h"

typedef struct {
    lfs2_size_t block_size;
    lfs2_size_t blocks;
    lfs2_ssize_t used_blocks;
} app_fs_usage_t;

// Mounts vfs; returns the filesystem or NULL (and logs why). Pair every success with app_fs_end().
lfs2_t *app_fs_begin(void);

// Unmounts vfs and lets the next request in.
void app_fs_end(void);

// From now on app_fs_begin() mounts only the first time and app_fs_end() leaves vfs mounted: the
// pair only takes and gives the lock. Call it before the first app_fs_begin(). Files are opened and
// closed within one begin/end all the same - another request may change the tree in between. The
// cache is 1 KB then, 4 KB of internal heap taken here; what lands on the flash is the same, since
// files inlined in their directory stay at 128 bytes, as MicroPython writes them.
void app_fs_keep_mounted(void);

// Fills usage for a mounted filesystem; false when the count fails.
bool app_fs_usage(lfs2_t *fs, app_fs_usage_t *usage);

// A buffer for lfs2_file_opencfg: LFS2_NO_MALLOC leaves file caches to the caller. slot 0 or 1: two
// files can be open at a time, one in each slot.
struct lfs2_file_config *app_fs_file_config(int slot);

// The rest works on any mounted tree and lives in app_fs_tree.c.

// True for an absolute path without empty, "." or ".." parts, short enough for LittleFS.
bool app_fs_path_ok(const char *path);

// Makes every missing directory above path; 0 or a LittleFS error.
int app_fs_make_parents(lfs2_t *fs, const char *path);

// Hears one entry of app_fs_walk: its full path and what LittleFS says about it. A negative return
// stops the walk.
typedef int (*app_fs_walk_fn)(void *ctx, const char *path, const struct lfs2_info *info);

// Calls fn for every entry below dir, depth first, a directory before what is in it; dir itself is
// not passed. The one order for everything that goes through the files: GET /files, the zip writer,
// the unpack. Returns 0 when every entry was seen, or the first negative value from fn or LittleFS.
// A path longer than 255 bytes is skipped.
int app_fs_walk(lfs2_t *fs, const char *dir, app_fs_walk_fn fn, void *ctx);
