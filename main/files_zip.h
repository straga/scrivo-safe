// Zip archives of the MicroPython files in vfs: every file out as one archive, and one archive laid
// back in place of every file. Works on a mounted filesystem (app_fs.h) and knows nothing of HTTP, so
// the same code builds for the board and for the host tests (tests/host).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lfs2.h"

// Where an unpack keeps the files it takes out of the archive until every one of them is checked.
#define FILES_ZIP_STAGE "/.unpack"

typedef struct {
    lfs2_t *fs;
    // LFS2_NO_MALLOC leaves file caches to the caller. An unpack reads the archive and writes a file
    // at the same time, so it needs two.
    struct lfs2_file_config *file_cfg[2];
    // One progress line at a time for the monitor; may be NULL.
    void (*log)(const char *line);
} files_zip_fs_t;

typedef enum {
    FILES_ZIP_OK,
    FILES_ZIP_NOT_FOUND,   // no file at the archive path
    FILES_ZIP_REFUSED,     // the archive cannot be laid onto the board; why says what is wrong with it
    FILES_ZIP_FAILED,      // the filesystem or the receiver failed; why says at which step
} files_zip_result_t;

typedef struct {
    unsigned files;          // files the archive puts on the board
    unsigned dirs;           // directories it names or implies
    unsigned skipped;        // entries under __MACOSX/, which Finder adds and which are not board files
    unsigned removed;        // board files and directories that are not in the archive
    uint32_t bytes;          // file bytes after unpacking
    uint32_t blocks_needed;  // flash blocks the unpacked files take while the old ones are still there
    uint32_t blocks_free;
} files_zip_summary_t;

// Receives each board path that is not in the archive, a directory after the files in it.
typedef void (*files_zip_path_fn)(void *ctx, const char *path, bool dir);

// Reads the archive at `archive` and checks it against the board; writes nothing. `removed` hears what
// files_zip_unpack would remove.
files_zip_result_t files_zip_check(files_zip_fs_t *z, const char *archive, files_zip_summary_t *sum,
                                   files_zip_path_fn removed, void *ctx, char *why, size_t why_size);

// Makes vfs hold exactly the archive's files. Every file is taken out under FILES_ZIP_STAGE and its
// CRC-32 checked twice, as it is inflated and as it is read back from flash; only then do the names
// move into place, board paths missing from the archive are removed and the archive is deleted. A
// refusal or failure before the names move leaves every board file and the archive as they were.
// `removed` hears what was removed.
files_zip_result_t files_zip_unpack(files_zip_fs_t *z, const char *archive, files_zip_summary_t *sum,
                                    files_zip_path_fn removed, void *ctx, char *why, size_t why_size);

// Receives the next bytes of an archive being written; returning false stops the writing.
typedef bool (*files_zip_sink_t)(void *ctx, const void *data, size_t len);

// Writes every file and directory in vfs, in the order GET /files lists them, as one zip with stored
// (uncompressed) entries. FILES_ZIP_STAGE, left behind by an unpack the power cut, is not written.
files_zip_result_t files_zip_write(files_zip_fs_t *z, files_zip_sink_t sink, void *ctx, char *why, size_t why_size);
