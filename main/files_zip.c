#include "files_zip.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_fs.h"
#include "miniz.h"

#define MAX_ENTRIES   256
#define MAX_DIRECTORY 16384   // central directory bytes held in memory for one unpack
#define IO_CHUNK      1024
#define PATH_BUF      130     // app_fs_path_ok takes up to 128 bytes
#define NAME_SHOWN    100     // an archive name quoted in a refusal is cut to this

#define SIG_LOCAL         0x04034b50
#define SIG_CENTRAL       0x02014b50
#define SIG_END           0x06054b50
#define SIG_ZIP64_LOCATOR 0x07064b50
#define LOCAL_LEN   30
#define CENTRAL_LEN 46
#define END_LEN     22
#define DOS_DATE_1980 0x0021   // LittleFS keeps no times; every entry says 1 January 1980

#define STOP (-10000)          // a walk callback stopping on purpose, below every LittleFS error

enum { KIND_SKIP, KIND_FILE, KIND_DIR };

static const char STAGE[] = FILES_ZIP_STAGE;

// ---------------------------------------------------------------------------------------------
// Reading an archive

typedef struct {
    files_zip_fs_t *z;
    const char *archive;
    files_zip_summary_t *sum;
    char *why;
    size_t why_size;
    lfs2_file_t file;          // the archive, open in file_cfg[0]
    bool open;
    lfs2_off_t size;
    lfs2_off_t directory_at;
    uint8_t *directory;
    unsigned entries;
    uint16_t at[MAX_ENTRIES];  // where each central record starts in directory
    uint8_t kind[MAX_ENTRIES];
    // Board paths that are not in the archive, in walk order: 'f' or 'd', the path, NUL.
    char *removed;
    size_t removed_len, removed_cap;
    int walk_err;
    uint8_t io[IO_CHUNK];
    tinfl_decompressor *inflator;
    uint8_t *window;
} job_t;

typedef struct {
    uint16_t made_by, flags, method, name_len, extra_len;
    uint32_t crc, csize, usize, attr, local;
    const char *name;
} record_t;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint8_t *put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    return put16(put16(p, (uint16_t)v), (uint16_t)(v >> 16));
}

static uint32_t crc32_add(uint32_t crc, const uint8_t *data, size_t len)
{
    return (uint32_t)mz_crc32(crc, data, len);
}

static files_zip_result_t say(char *why, size_t why_size, files_zip_result_t result, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, why_size, fmt, ap);
    va_end(ap);
    return result;
}

#define REFUSE(j, ...) say((j)->why, (j)->why_size, FILES_ZIP_REFUSED, __VA_ARGS__)
#define FAIL(j, ...) say((j)->why, (j)->why_size, FILES_ZIP_FAILED, __VA_ARGS__)

static void note(files_zip_fs_t *z, const char *fmt, ...)
{
    if (z->log == NULL) {
        return;
    }
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    z->log(line);
}

static void record(const job_t *j, unsigned i, record_t *r)
{
    const uint8_t *p = j->directory + j->at[i];
    r->made_by = le16(p + 4);
    r->flags = le16(p + 8);
    r->method = le16(p + 10);
    r->crc = le32(p + 16);
    r->csize = le32(p + 20);
    r->usize = le32(p + 24);
    r->name_len = le16(p + 28);
    r->extra_len = le16(p + 30);
    r->attr = le32(p + 38);
    r->local = le32(p + 42);
    r->name = (const char *)p + CENTRAL_LEN;
}

// The name without the slash that marks a directory.
static size_t stem(const record_t *r)
{
    return r->name_len && r->name[r->name_len - 1] == '/' ? r->name_len - 1u : r->name_len;
}

static int shown(const record_t *r)
{
    return r->name_len < NAME_SHOWN ? r->name_len : NAME_SHOWN;
}

// "/" + the name, when it makes a path the board can hold.
static bool board_path(const record_t *r, char *path)
{
    size_t n = stem(r);
    if (n + 2 > PATH_BUF || memchr(r->name, '\0', n) || memchr(r->name, '\\', n)) {
        return false;
    }
    path[0] = '/';
    memcpy(path + 1, r->name, n);
    path[n + 1] = '\0';
    return app_fs_path_ok(path);
}

static bool in_stage(const char *path)
{
    size_t n = sizeof(STAGE) - 1;
    return strncmp(path, STAGE, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static bool has_extra(const record_t *r, uint16_t id)
{
    const uint8_t *p = (const uint8_t *)r->name + r->name_len, *end = p + r->extra_len;
    while (p + 4 <= end) {
        if (le16(p) == id) {
            return true;
        }
        p += 4 + le16(p + 2);
    }
    return false;
}

// Reads exactly n bytes at off: 0, 1 when the archive ends first, or a LittleFS error.
static int read_at(job_t *j, lfs2_off_t off, void *buf, lfs2_size_t n)
{
    if ((uint64_t)off + n > j->size) {
        return 1;
    }
    lfs2_soff_t pos = lfs2_file_seek(j->z->fs, &j->file, (lfs2_soff_t)off, LFS2_SEEK_SET);
    if (pos < 0) {
        return (int)pos;
    }
    lfs2_ssize_t got = lfs2_file_read(j->z->fs, &j->file, buf, n);
    if (got < 0) {
        return (int)got;
    }
    return (lfs2_size_t)got == n ? 0 : 1;
}

static files_zip_result_t not_zip(job_t *j)
{
    return REFUSE(j, "%s is not a zip archive: it has no end-of-directory record", j->archive);
}

static files_zip_result_t read_failed(job_t *j, int err)
{
    return FAIL(j, "reading %s failed: %d", j->archive, err);
}

static files_zip_result_t open_archive(job_t *j)
{
    lfs2_t *fs = j->z->fs;
    if (in_stage(j->archive)) {
        return REFUSE(j, "%s lies in %s, which the unpack clears", j->archive, STAGE);
    }
    struct lfs2_info info;
    int err = lfs2_stat(fs, j->archive, &info);
    if (err == LFS2_ERR_NOENT) {
        return say(j->why, j->why_size, FILES_ZIP_NOT_FOUND, "no such file: %s", j->archive);
    }
    if (err < 0) {
        return read_failed(j, err);
    }
    if (info.type == LFS2_TYPE_DIR) {
        return REFUSE(j, "%s is a directory, not a zip archive", j->archive);
    }
    err = lfs2_file_opencfg(fs, &j->file, j->archive, LFS2_O_RDONLY, j->z->file_cfg[0]);
    if (err < 0) {
        return read_failed(j, err);
    }
    j->open = true;
    j->size = info.size;
    return FILES_ZIP_OK;
}

// The end record is the last thing in a zip, followed only by a comment of up to 65535 bytes. The
// one that counts is the one whose comment reaches exactly to the end of the file.
static files_zip_result_t find_end(job_t *j, lfs2_off_t *end)
{
    if (j->size < END_LEN) {
        return not_zip(j);
    }
    lfs2_off_t lowest = j->size > END_LEN + 0xFFFFu ? j->size - END_LEN - 0xFFFFu : 0;
    lfs2_off_t top = j->size - END_LEN + 1;   // candidates below top
    const lfs2_off_t span = IO_CHUNK - END_LEN + 1;
    while (top > lowest) {
        lfs2_off_t from = top - lowest > span ? top - span : lowest;
        int err = read_at(j, from, j->io, top - from + END_LEN - 1);
        if (err < 0) {
            return read_failed(j, err);
        }
        if (err > 0) {
            return not_zip(j);
        }
        for (lfs2_off_t p = top; p-- > from;) {
            const uint8_t *q = j->io + (p - from);
            if (le32(q) == SIG_END && (uint64_t)p + END_LEN + le16(q + 20) == j->size) {
                *end = p;
                return FILES_ZIP_OK;
            }
        }
        top = from;
    }
    return not_zip(j);
}

static files_zip_result_t zip64(job_t *j)
{
    return REFUSE(j, "%s is a zip64 archive; zip64 is not supported", j->archive);
}

static files_zip_result_t read_directory(job_t *j)
{
    lfs2_off_t end;
    files_zip_result_t result = find_end(j, &end);
    if (result != FILES_ZIP_OK) {
        return result;
    }
    uint8_t rec[END_LEN];
    int err = read_at(j, end, rec, END_LEN);
    if (err != 0) {
        return err < 0 ? read_failed(j, err) : not_zip(j);
    }
    uint16_t disk = le16(rec + 4), directory_disk = le16(rec + 6), on_disk = le16(rec + 8), total = le16(rec + 10);
    uint32_t directory_size = le32(rec + 12), directory_at = le32(rec + 16);
    if (total == 0xFFFF || directory_size == 0xFFFFFFFF || directory_at == 0xFFFFFFFF) {
        return zip64(j);
    }
    if (end >= 20) {
        uint8_t locator[4];
        err = read_at(j, end - 20, locator, sizeof(locator));
        if (err < 0) {
            return read_failed(j, err);
        }
        if (err == 0 && le32(locator) == SIG_ZIP64_LOCATOR) {
            return zip64(j);
        }
    }
    if (disk != 0 || directory_disk != 0 || on_disk != total) {
        return REFUSE(j, "%s is split across several files; only a single-file archive can be unpacked", j->archive);
    }
    if (total > MAX_ENTRIES) {
        return REFUSE(j, "%s has %u entries; more than %u entries are not unpacked", j->archive, total, MAX_ENTRIES);
    }
    if (directory_size > MAX_DIRECTORY) {
        return REFUSE(j, "the directory of %s takes %u bytes; more than %u are not unpacked", j->archive,
                      (unsigned)directory_size, MAX_DIRECTORY);
    }
    if ((uint64_t)directory_at + directory_size > end) {
        return REFUSE(j, "%s is damaged: its directory runs past its end record", j->archive);
    }
    j->directory = malloc(directory_size ? directory_size : 1);
    if (j->directory == NULL) {
        return FAIL(j, "no memory for the directory of %s (%u bytes)", j->archive, (unsigned)directory_size);
    }
    err = read_at(j, directory_at, j->directory, directory_size);
    if (err != 0) {
        return err < 0 ? read_failed(j, err) : REFUSE(j, "%s is damaged: its directory is cut short", j->archive);
    }
    j->directory_at = directory_at;
    j->entries = total;
    size_t pos = 0;
    for (unsigned i = 0; i < total; i++) {
        const uint8_t *p = j->directory + pos;
        if (pos + CENTRAL_LEN > directory_size || le32(p) != SIG_CENTRAL) {
            return REFUSE(j, "%s is damaged: entry %u of its directory is not where it should be", j->archive, i);
        }
        size_t len = CENTRAL_LEN + (size_t)le16(p + 28) + le16(p + 30) + le16(p + 32);
        if (pos + len > directory_size) {
            return REFUSE(j, "%s is damaged: entry %u runs past its directory", j->archive, i);
        }
        j->at[i] = (uint16_t)pos;
        pos += len;
    }
    if (pos != directory_size) {
        return REFUSE(j, "%s is damaged: its directory holds more than its %u entries", j->archive, total);
    }
    return FILES_ZIP_OK;
}

// True when entry b, of kind, puts a directory at the first n bytes of name.
static bool makes_dir(const record_t *b, int kind, const char *name, size_t n)
{
    size_t bn = stem(b);
    if (memcmp(b->name, name, n < bn ? n : bn) != 0) {
        return false;
    }
    return (bn > n && b->name[n] == '/') || (kind == KIND_DIR && bn == n);
}

static files_zip_result_t check_entries(job_t *j)
{
    files_zip_summary_t *s = j->sum;
    lfs2_size_t block = j->z->fs->cfg->block_size;
    char path[PATH_BUF];
    for (unsigned i = 0; i < j->entries; i++) {
        record_t r;
        record(j, i, &r);
        if (r.name_len >= 9 && memcmp(r.name, "__MACOSX/", 9) == 0) {
            j->kind[i] = KIND_SKIP;
            s->skipped++;
            continue;
        }
        if ((r.flags & 0x0041) || r.method == 99) {
            return REFUSE(j, "%.*s is encrypted; encrypted archives are not supported", shown(&r), r.name);
        }
        if (r.method != 0 && r.method != 8) {
            return REFUSE(j, "%.*s uses compression method %u; only stored (0) and deflated (8) are supported",
                          shown(&r), r.name, r.method);
        }
        if (r.csize == 0xFFFFFFFF || r.usize == 0xFFFFFFFF || r.local == 0xFFFFFFFF || has_extra(&r, 0x0001)) {
            return zip64(j);
        }
        if ((r.made_by >> 8) == 3 && ((r.attr >> 16) & 0170000) == 0120000) {
            return REFUSE(j, "%.*s is a symbolic link; symbolic links are not supported", shown(&r), r.name);
        }
        if (!board_path(&r, path)) {
            return REFUSE(j, "\"%.*s\" is not a usable path on the board: it is empty, absolute, has . or .. or empty "
                             "parts, a backslash, or is longer than 128 bytes", shown(&r), r.name);
        }
        if (in_stage(path)) {
            return REFUSE(j, "%s is reserved for the unpack itself", path);
        }
        if (strcmp(path, j->archive) == 0) {
            return REFUSE(j, "%s is the name of the archive itself", path);
        }
        j->kind[i] = r.name[r.name_len - 1] == '/' ? KIND_DIR : KIND_FILE;
        if (j->kind[i] == KIND_FILE) {
            if (r.method == 0 && r.csize != r.usize) {
                return REFUSE(j, "%s is damaged: stored with two different sizes", path);
            }
            s->files++;
            s->bytes += r.usize;
            s->blocks_needed += (r.usize + block - 1) / block;
        }
    }

    for (unsigned i = 0; i < j->entries; i++) {
        if (j->kind[i] == KIND_SKIP) {
            continue;
        }
        record_t a;
        record(j, i, &a);
        size_t an = stem(&a);
        for (unsigned k = 0; k < i; k++) {
            if (j->kind[k] == KIND_SKIP) {
                continue;
            }
            record_t b;
            record(j, k, &b);
            size_t bn = stem(&b);
            if (an == bn && memcmp(a.name, b.name, an) == 0) {
                if (j->kind[i] != j->kind[k]) {
                    return REFUSE(j, "/%.*s is both a file and a directory in the archive", (int)an, a.name);
                }
                if (j->kind[i] == KIND_FILE) {
                    return REFUSE(j, "/%.*s is in the archive twice", (int)an, a.name);
                }
            }
            if ((j->kind[i] == KIND_FILE && makes_dir(&b, KIND_FILE, a.name, an) && bn > an) ||
                (j->kind[k] == KIND_FILE && makes_dir(&a, KIND_FILE, b.name, bn) && an > bn)) {
                const record_t *file = j->kind[i] == KIND_FILE && bn > an ? &a : &b;
                return REFUSE(j, "/%.*s is both a file and a directory in the archive", (int)stem(file), file->name);
            }
        }
        // every directory this entry names or implies, counted once
        for (size_t n = 1; n <= an; n++) {
            bool is_dir = (n < an && a.name[n] == '/') || (n == an && j->kind[i] == KIND_DIR);
            if (!is_dir) {
                continue;
            }
            bool seen = false;
            for (unsigned k = 0; k < i && !seen; k++) {
                record_t b;
                record(j, k, &b);
                seen = j->kind[k] != KIND_SKIP && makes_dir(&b, j->kind[k], a.name, n);
            }
            s->dirs += !seen;
        }
    }
    s->blocks_needed += 2 * (s->dirs + 1);   // a metadata pair per directory, and one for the stage
    return FILES_ZIP_OK;
}

static files_zip_result_t check_against_board(job_t *j)
{
    lfs2_t *fs = j->z->fs;
    char path[PATH_BUF];
    struct lfs2_info info;
    for (unsigned i = 0; i < j->entries; i++) {
        if (j->kind[i] == KIND_SKIP) {
            continue;
        }
        record_t r;
        record(j, i, &r);
        board_path(&r, path);
        for (char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
            *slash = '\0';
            int err = lfs2_stat(fs, path, &info);
            if (err == 0 && info.type == LFS2_TYPE_REG) {
                return REFUSE(j, "%s is a file on the board and a directory in the archive", path);
            }
            *slash = '/';
            if (err < 0 && err != LFS2_ERR_NOENT) {
                return read_failed(j, err);
            }
        }
        int err = lfs2_stat(fs, path, &info);
        if (err == 0 && j->kind[i] == KIND_FILE && info.type == LFS2_TYPE_DIR) {
            return REFUSE(j, "%s is a directory on the board and a file in the archive", path);
        }
        if (err == 0 && j->kind[i] == KIND_DIR && info.type == LFS2_TYPE_REG) {
            return REFUSE(j, "%s is a file on the board and a directory in the archive", path);
        }
        if (err < 0 && err != LFS2_ERR_NOENT) {
            return read_failed(j, err);
        }
    }
    lfs2_ssize_t used = lfs2_fs_size(fs);
    if (used < 0) {
        return FAIL(j, "counting the used blocks failed: %d", (int)used);
    }
    j->sum->blocks_free = fs->cfg->block_count - (lfs2_size_t)used;
    if (j->sum->blocks_needed > j->sum->blocks_free) {
        return REFUSE(j, "the files in %s need %u blocks of flash and %u are free", j->archive,
                      (unsigned)j->sum->blocks_needed, (unsigned)j->sum->blocks_free);
    }
    return FILES_ZIP_OK;
}

static bool in_archive(const job_t *j, const char *path, bool dir)
{
    const char *name = path + 1;
    size_t n = strlen(name);
    for (unsigned i = 0; i < j->entries; i++) {
        if (j->kind[i] == KIND_SKIP) {
            continue;
        }
        record_t r;
        record(j, i, &r);
        if (dir ? makes_dir(&r, j->kind[i], name, n)
                : j->kind[i] == KIND_FILE && stem(&r) == n && memcmp(r.name, name, n) == 0) {
            return true;
        }
    }
    return false;
}

static int collect_removed(void *ctx, const char *path, const struct lfs2_info *info)
{
    job_t *j = ctx;
    bool dir = info->type == LFS2_TYPE_DIR;
    if (strcmp(path, j->archive) == 0 || in_stage(path) || in_archive(j, path, dir)) {
        return 0;
    }
    size_t need = strlen(path) + 2;
    if (j->removed_len + need > j->removed_cap) {
        size_t cap = j->removed_cap ? j->removed_cap * 2 : 512;
        char *grown = cap <= 65536 ? realloc(j->removed, cap) : NULL;
        if (grown == NULL) {
            j->walk_err = STOP;
            return STOP;
        }
        j->removed = grown;
        j->removed_cap = cap;
    }
    j->removed[j->removed_len] = dir ? 'd' : 'f';
    memcpy(j->removed + j->removed_len + 1, path, need - 1);
    j->removed_len += need;
    j->sum->removed++;
    return 0;
}

// Steps back through the removed list: the entry before *pos, or NULL at the start.
static const char *removed_before(const job_t *j, size_t *pos)
{
    if (*pos == 0) {
        return NULL;
    }
    size_t start = *pos - 1;
    while (start > 0 && j->removed[start - 1] != '\0') {
        start--;
    }
    *pos = start;
    return j->removed + start;
}

static files_zip_result_t prepare(job_t *j)
{
    files_zip_result_t result = open_archive(j);
    if (result == FILES_ZIP_OK) {
        result = read_directory(j);
    }
    if (result == FILES_ZIP_OK) {
        result = check_entries(j);
    }
    if (result == FILES_ZIP_OK) {
        result = check_against_board(j);
    }
    if (result != FILES_ZIP_OK) {
        return result;
    }
    int err = app_fs_walk(j->z->fs, "/", collect_removed, j);
    if (j->walk_err == STOP) {
        return FAIL(j, "the board holds more files than can be compared with %s", j->archive);
    }
    if (err < 0) {
        return FAIL(j, "listing the board's files failed: %d", err);
    }
    return FILES_ZIP_OK;
}

static job_t *start(files_zip_fs_t *z, const char *archive, files_zip_summary_t *sum, char *why, size_t why_size)
{
    memset(sum, 0, sizeof(*sum));
    snprintf(why, why_size, "no memory for the unpack");
    job_t *j = calloc(1, sizeof(job_t));
    if (j != NULL) {
        *j = (job_t){.z = z, .archive = archive, .sum = sum, .why = why, .why_size = why_size};
        why[0] = '\0';
    }
    return j;
}

static void finish(job_t *j)
{
    if (j->open) {
        lfs2_file_close(j->z->fs, &j->file);
    }
    free(j->directory);
    free(j->removed);
    free(j->inflator);
    free(j->window);
    free(j);
}

files_zip_result_t files_zip_check(files_zip_fs_t *z, const char *archive, files_zip_summary_t *sum,
                                   files_zip_path_fn removed, void *ctx, char *why, size_t why_size)
{
    job_t *j = start(z, archive, sum, why, why_size);
    if (j == NULL) {
        return FILES_ZIP_FAILED;
    }
    files_zip_result_t result = prepare(j);
    if (result == FILES_ZIP_OK && removed != NULL) {
        size_t pos = j->removed_len;
        for (const char *e; (e = removed_before(j, &pos)) != NULL;) {
            removed(ctx, e + 1, e[0] == 'd');
        }
    }
    finish(j);
    return result;
}

// ---------------------------------------------------------------------------------------------
// Unpacking

// Removes path and everything under it; a missing path is not an error.
static int remove_tree(lfs2_t *fs, const char *path)
{
    struct lfs2_info info;
    int err = lfs2_stat(fs, path, &info);
    if (err < 0) {
        return err == LFS2_ERR_NOENT ? 0 : err;
    }
    while (info.type == LFS2_TYPE_DIR) {
        char child[256];
        bool found = false;
        lfs2_dir_t d;
        if ((err = lfs2_dir_open(fs, &d, path)) < 0) {
            return err;
        }
        struct lfs2_info e;
        while ((err = lfs2_dir_read(fs, &d, &e)) > 0) {
            if (strcmp(e.name, ".") != 0 && strcmp(e.name, "..") != 0) {
                found = snprintf(child, sizeof(child), "%s/%s", path, e.name) < (int)sizeof(child);
                break;
            }
        }
        lfs2_dir_close(fs, &d);
        if (err < 0) {
            return err;
        }
        if (!found) {
            break;
        }
        if ((err = remove_tree(fs, child)) < 0) {
            return err;
        }
    }
    return lfs2_remove(fs, path);
}

static files_zip_result_t write_failed(job_t *j, int err, const char *path)
{
    return FAIL(j, err == LFS2_ERR_NOSPC ? "no space left in vfs while writing %s" : "writing %s failed: %d", path, err);
}

static files_zip_result_t cut_short(job_t *j, const char *path)
{
    return REFUSE(j, "%s is damaged: the archive ends inside its data", path);
}

static files_zip_result_t put_out(job_t *j, lfs2_file_t *out, const uint8_t *data, size_t len, uint32_t *crc,
                                  uint32_t *count, const char *path)
{
    lfs2_ssize_t n = lfs2_file_write(j->z->fs, out, data, len);
    if (n < 0) {
        return write_failed(j, (int)n, path);
    }
    *crc = crc32_add(*crc, data, len);
    *count += (uint32_t)len;
    return FILES_ZIP_OK;
}

static files_zip_result_t copy_stored(job_t *j, const record_t *r, lfs2_file_t *out, uint32_t *crc, uint32_t *count,
                                      const char *path)
{
    for (uint32_t left = r->csize; left > 0;) {
        lfs2_size_t want = left < IO_CHUNK ? left : IO_CHUNK;
        lfs2_ssize_t got = lfs2_file_read(j->z->fs, &j->file, j->io, want);
        if (got < 0) {
            return read_failed(j, (int)got);
        }
        if ((lfs2_size_t)got != want) {
            return cut_short(j, path);
        }
        files_zip_result_t result = put_out(j, out, j->io, want, crc, count, path);
        if (result != FILES_ZIP_OK) {
            return result;
        }
        left -= want;
    }
    return FILES_ZIP_OK;
}

// Raw deflate through tinfl (ROM on the board) with a wrapping 32 KB window as the output buffer.
static files_zip_result_t inflate(job_t *j, const record_t *r, lfs2_file_t *out, uint32_t *crc, uint32_t *count,
                                  const char *path)
{
    if (j->inflator == NULL) {
        j->inflator = malloc(sizeof(tinfl_decompressor));
        j->window = malloc(TINFL_LZ_DICT_SIZE);
        if (j->inflator == NULL || j->window == NULL) {
            return FAIL(j, "no memory to inflate (%u bytes)", (unsigned)(sizeof(tinfl_decompressor) + TINFL_LZ_DICT_SIZE));
        }
    }
    tinfl_init(j->inflator);
    uint32_t left = r->csize;
    size_t in_len = 0, in_at = 0, out_at = 0;
    for (;;) {
        if (in_at == in_len && left > 0) {
            lfs2_size_t want = left < IO_CHUNK ? left : IO_CHUNK;
            lfs2_ssize_t got = lfs2_file_read(j->z->fs, &j->file, j->io, want);
            if (got < 0) {
                return read_failed(j, (int)got);
            }
            if ((lfs2_size_t)got != want) {
                return cut_short(j, path);
            }
            in_len = want;
            in_at = 0;
            left -= want;
        }
        size_t in_n = in_len - in_at, out_n = TINFL_LZ_DICT_SIZE - out_at;
        tinfl_status status = tinfl_decompress(j->inflator, j->io + in_at, &in_n, j->window, j->window + out_at, &out_n,
                                               left > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        in_at += in_n;
        if (out_n > 0) {
            if ((uint64_t)*count + out_n > r->usize) {
                return REFUSE(j, "%s does not inflate to the %u bytes the archive says", path, (unsigned)r->usize);
            }
            files_zip_result_t result = put_out(j, out, j->window + out_at, out_n, crc, count, path);
            if (result != FILES_ZIP_OK) {
                return result;
            }
            out_at = (out_at + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        }
        if (status == TINFL_STATUS_DONE) {
            return FILES_ZIP_OK;
        }
        if (status < 0) {
            return REFUSE(j, "%s does not inflate: its deflate data is damaged", path);
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && left == 0 && in_at == in_len) {
            return REFUSE(j, "%s does not inflate: its data ends before the deflate stream does", path);
        }
    }
}

// Entry i out of the archive into FILES_ZIP_STAGE/<i>, with its CRC-32 checked as it is written and
// again as flash gives it back.
static files_zip_result_t take_out_one(job_t *j, unsigned i)
{
    lfs2_t *fs = j->z->fs;
    record_t r;
    record(j, i, &r);
    char path[PATH_BUF], temp[sizeof(STAGE) + 12];
    board_path(&r, path);
    snprintf(temp, sizeof(temp), "%s/%u", STAGE, i);

    uint8_t local[LOCAL_LEN];
    int err = read_at(j, r.local, local, LOCAL_LEN);
    if (err < 0) {
        return read_failed(j, err);
    }
    if (err > 0 || le32(local) != SIG_LOCAL) {
        return REFUSE(j, "%s is damaged: its header is not where the directory says", path);
    }
    uint64_t data = (uint64_t)r.local + LOCAL_LEN + le16(local + 26) + le16(local + 28);
    if (data + r.csize > j->directory_at) {
        return REFUSE(j, "%s is damaged: its data runs into the directory", path);
    }
    lfs2_soff_t pos = lfs2_file_seek(fs, &j->file, (lfs2_soff_t)data, LFS2_SEEK_SET);
    if (pos < 0) {
        return read_failed(j, (int)pos);
    }

    lfs2_file_t out;
    err = lfs2_file_opencfg(fs, &out, temp, LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_TRUNC, j->z->file_cfg[1]);
    if (err < 0) {
        return write_failed(j, err, path);
    }
    uint32_t crc = 0, count = 0;
    files_zip_result_t result = r.method == 0 ? copy_stored(j, &r, &out, &crc, &count, path)
                                              : inflate(j, &r, &out, &crc, &count, path);
    err = lfs2_file_close(fs, &out);
    if (result != FILES_ZIP_OK) {
        return result;
    }
    if (err < 0) {
        return write_failed(j, err, path);
    }
    if (crc != r.crc || count != r.usize) {
        return REFUSE(j, "%s is damaged: CRC-32 %08x over %u bytes, the archive says %08x over %u", path,
                      (unsigned)crc, (unsigned)count, (unsigned)r.crc, (unsigned)r.usize);
    }

    err = lfs2_file_opencfg(fs, &out, temp, LFS2_O_RDONLY, j->z->file_cfg[1]);
    if (err < 0) {
        return FAIL(j, "reading back %s failed: %d", path, err);
    }
    uint32_t back = 0, back_count = 0;
    lfs2_ssize_t n;
    while ((n = lfs2_file_read(fs, &out, j->io, IO_CHUNK)) > 0) {
        back = crc32_add(back, j->io, (size_t)n);
        back_count += (uint32_t)n;
    }
    lfs2_file_close(fs, &out);
    if (n < 0) {
        return FAIL(j, "reading back %s failed: %d", path, (int)n);
    }
    if (back != r.crc || back_count != r.usize) {
        return FAIL(j, "%s read back from flash differs from what was written: CRC-32 %08x over %u bytes", path,
                    (unsigned)back, (unsigned)back_count);
    }
    return FILES_ZIP_OK;
}

static files_zip_result_t take_out(job_t *j)
{
    lfs2_t *fs = j->z->fs;
    note(j->z, "unpack %s: %u files, %u bytes, %u blocks needed of %u free", j->archive, j->sum->files,
         (unsigned)j->sum->bytes, (unsigned)j->sum->blocks_needed, (unsigned)j->sum->blocks_free);
    int err = remove_tree(fs, STAGE);
    if (err >= 0) {
        err = lfs2_mkdir(fs, STAGE);
    }
    if (err < 0) {
        return FAIL(j, "making %s failed: %d", STAGE, err);
    }
    files_zip_result_t result = FILES_ZIP_OK;
    for (unsigned i = 0; i < j->entries && result == FILES_ZIP_OK; i++) {
        if (j->kind[i] == KIND_FILE) {
            result = take_out_one(j, i);
        }
    }
    lfs2_file_close(fs, &j->file);
    j->open = false;
    if (result != FILES_ZIP_OK) {
        remove_tree(fs, STAGE);
        return result;
    }
    note(j->z, "unpack %s: %u files taken out to %s and checked, moving names", j->archive, j->sum->files, STAGE);
    return FILES_ZIP_OK;
}

static files_zip_result_t remove_listed(job_t *j, char kind, files_zip_path_fn removed, void *ctx)
{
    size_t pos = j->removed_len;
    for (const char *e; (e = removed_before(j, &pos)) != NULL;) {
        if (e[0] != kind) {
            continue;
        }
        int err = lfs2_remove(j->z->fs, e + 1);
        if (err < 0 && err != LFS2_ERR_NOENT) {
            return FAIL(j, "removing %s failed (%d); the archive's files are in place", e + 1, err);
        }
        if (removed != NULL) {
            removed(ctx, e + 1, kind == 'd');
        }
    }
    return FILES_ZIP_OK;
}

static files_zip_result_t move_into_place(job_t *j, files_zip_path_fn removed, void *ctx)
{
    lfs2_t *fs = j->z->fs;
    char path[PATH_BUF], temp[sizeof(STAGE) + 12];
    for (unsigned i = 0; i < j->entries; i++) {
        if (j->kind[i] == KIND_SKIP) {
            continue;
        }
        record_t r;
        record(j, i, &r);
        board_path(&r, path);
        int err = app_fs_make_parents(fs, path);
        if (err >= 0 && j->kind[i] == KIND_DIR) {
            err = lfs2_mkdir(fs, path);
            err = err == LFS2_ERR_EXIST ? 0 : err;
        }
        if (err >= 0 && j->kind[i] == KIND_FILE) {
            snprintf(temp, sizeof(temp), "%s/%u", STAGE, i);
            err = lfs2_rename(fs, temp, path);
        }
        if (err < 0) {
            return FAIL(j, "moving %s into place failed (%d); the board holds part of the archive, unpack it again",
                        path, err);
        }
    }
    files_zip_result_t result = remove_listed(j, 'f', removed, ctx);
    if (result != FILES_ZIP_OK) {
        return result;
    }
    int err = lfs2_remove(fs, j->archive);
    if (err < 0 && err != LFS2_ERR_NOENT) {
        return FAIL(j, "removing %s failed (%d); its files are in place", j->archive, err);
    }
    result = remove_listed(j, 'd', removed, ctx);   // children before parents: the list runs backwards
    if (result != FILES_ZIP_OK) {
        return result;
    }
    err = remove_tree(fs, STAGE);
    if (err < 0) {
        return FAIL(j, "removing %s failed (%d); the archive's files are in place", STAGE, err);
    }
    note(j->z, "unpack %s: done, %u files in place, %u removed", j->archive, j->sum->files, j->sum->removed);
    return FILES_ZIP_OK;
}

files_zip_result_t files_zip_unpack(files_zip_fs_t *z, const char *archive, files_zip_summary_t *sum,
                                    files_zip_path_fn removed, void *ctx, char *why, size_t why_size)
{
    job_t *j = start(z, archive, sum, why, why_size);
    if (j == NULL) {
        return FILES_ZIP_FAILED;
    }
    files_zip_result_t result = prepare(j);
    if (result == FILES_ZIP_OK) {
        result = take_out(j);
    }
    if (result == FILES_ZIP_OK) {
        result = move_into_place(j, removed, ctx);
    }
    finish(j);
    return result;
}

// ---------------------------------------------------------------------------------------------
// Writing an archive

typedef struct {
    files_zip_fs_t *z;
    files_zip_sink_t sink;
    void *ctx;
    uint8_t *directory;
    size_t directory_len, directory_cap;
    unsigned entries;
    uint32_t offset;
    uint8_t out[IO_CHUNK];
    size_t out_len;
    bool stopped;
    uint64_t sent;
    int err;
    char failed_path[256];
    uint8_t io[IO_CHUNK];
} writer_t;

static void emit(writer_t *w, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0 && !w->stopped) {
        size_t n = IO_CHUNK - w->out_len < len ? IO_CHUNK - w->out_len : len;
        memcpy(w->out + w->out_len, p, n);
        w->out_len += n;
        p += n;
        len -= n;
        if (w->out_len == IO_CHUNK) {
            w->stopped = !w->sink(w->ctx, w->out, IO_CHUNK);
            w->sent += w->stopped ? 0 : IO_CHUNK;
            w->out_len = 0;
        }
    }
}

static void flush(writer_t *w)
{
    if (w->out_len > 0 && !w->stopped) {
        w->stopped = !w->sink(w->ctx, w->out, w->out_len);
        w->sent += w->stopped ? 0 : w->out_len;
    }
    w->out_len = 0;
}

typedef struct {
    size_t directory_len;
    unsigned entries;
} measure_t;

static int measure(void *ctx, const char *path, const struct lfs2_info *info)
{
    measure_t *m = ctx;
    if (!in_stage(path)) {
        m->directory_len += CENTRAL_LEN + strlen(path) - 1 + (info->type == LFS2_TYPE_DIR);
        m->entries++;
    }
    return 0;
}

static int add_entry(void *ctx, const char *path, const struct lfs2_info *info)
{
    writer_t *w = ctx;
    lfs2_t *fs = w->z->fs;
    if (in_stage(path)) {
        return 0;
    }
    bool dir = info->type == LFS2_TYPE_DIR;
    char name[258];
    uint16_t name_len = (uint16_t)snprintf(name, sizeof(name), "%s%s", path + 1, dir ? "/" : "");
    uint32_t crc = 0, size = 0;
    lfs2_file_t file;
    if (!dir) {
        int err = lfs2_file_opencfg(fs, &file, path, LFS2_O_RDONLY, w->z->file_cfg[0]);
        lfs2_ssize_t n = err;
        while (err == 0 && (n = lfs2_file_read(fs, &file, w->io, IO_CHUNK)) > 0) {
            crc = crc32_add(crc, w->io, (size_t)n);
            size += (uint32_t)n;
        }
        if (err == 0 && n == 0) {
            n = lfs2_file_seek(fs, &file, 0, LFS2_SEEK_SET);
        }
        if (n < 0) {
            if (err == 0) {
                lfs2_file_close(fs, &file);
            }
            w->err = (int)n;
            snprintf(w->failed_path, sizeof(w->failed_path), "%s", path);
            return STOP;
        }
    }
    uint16_t needed = dir ? 20 : 10;
    uint8_t h[CENTRAL_LEN], *p = h;
    p = put32(p, SIG_LOCAL);
    p = put16(p, needed);
    p = put16(p, 0x0800);   // names in UTF-8
    p = put16(p, 0);        // stored
    p = put16(p, 0);
    p = put16(p, DOS_DATE_1980);
    p = put32(p, crc);
    p = put32(p, size);
    p = put32(p, size);
    p = put16(p, name_len);
    p = put16(p, 0);
    emit(w, h, LOCAL_LEN);
    emit(w, name, name_len);
    if (!dir) {
        lfs2_ssize_t n;
        while (!w->stopped && (n = lfs2_file_read(fs, &file, w->io, IO_CHUNK)) > 0) {
            emit(w, w->io, (size_t)n);
        }
        lfs2_file_close(fs, &file);
    }

    if (w->directory_len + CENTRAL_LEN + name_len > w->directory_cap) {
        w->err = LFS2_ERR_INVAL;   // the tree changed between the two walks; the mount lock rules it out
        snprintf(w->failed_path, sizeof(w->failed_path), "%s", path);
        return STOP;
    }
    p = w->directory + w->directory_len;
    p = put32(p, SIG_CENTRAL);
    p = put16(p, 0x031E);   // made by Unix, zip 3.0: the mode below is read as Unix permissions
    p = put16(p, needed);
    p = put16(p, 0x0800);
    p = put16(p, 0);
    p = put16(p, 0);
    p = put16(p, DOS_DATE_1980);
    p = put32(p, crc);
    p = put32(p, size);
    p = put32(p, size);
    p = put16(p, name_len);
    p = put16(p, 0);
    p = put16(p, 0);
    p = put16(p, 0);
    p = put16(p, 0);
    p = put32(p, dir ? (040755u << 16) | 0x10 : 0100644u << 16);
    p = put32(p, w->offset);
    memcpy(p, name, name_len);
    w->directory_len += CENTRAL_LEN + name_len;
    w->entries++;
    w->offset += LOCAL_LEN + name_len + size;
    return w->stopped ? STOP : 0;
}

files_zip_result_t files_zip_write(files_zip_fs_t *z, files_zip_sink_t sink, void *ctx, char *why, size_t why_size)
{
    measure_t m = {0};
    int err = app_fs_walk(z->fs, "/", measure, &m);
    if (err < 0) {
        return say(why, why_size, FILES_ZIP_FAILED, "listing the files failed: %d", err);
    }
    if (m.entries > 0xFFFF) {
        return say(why, why_size, FILES_ZIP_FAILED, "%u entries do not fit a zip without zip64", m.entries);
    }
    writer_t *w = calloc(1, sizeof(writer_t));
    uint8_t *directory = malloc(m.directory_len ? m.directory_len : 1);
    if (w == NULL || directory == NULL) {
        free(w);
        free(directory);
        return say(why, why_size, FILES_ZIP_FAILED, "no memory for the archive directory (%u bytes)",
                   (unsigned)m.directory_len);
    }
    *w = (writer_t){.z = z, .sink = sink, .ctx = ctx, .directory = directory, .directory_cap = m.directory_len};
    err = app_fs_walk(z->fs, "/", add_entry, w);
    files_zip_result_t result = FILES_ZIP_OK;
    if (w->err < 0) {
        result = say(why, why_size, FILES_ZIP_FAILED, "reading %s failed: %d", w->failed_path, w->err);
    } else if (err < 0 && !w->stopped) {
        result = say(why, why_size, FILES_ZIP_FAILED, "listing the files failed: %d", err);
    } else {
        uint8_t end[END_LEN], *p = end;
        p = put32(p, SIG_END);
        p = put16(p, 0);
        p = put16(p, 0);
        p = put16(p, (uint16_t)w->entries);
        p = put16(p, (uint16_t)w->entries);
        p = put32(p, (uint32_t)w->directory_len);
        p = put32(p, w->offset);
        put16(p, 0);
        emit(w, w->directory, w->directory_len);
        emit(w, end, END_LEN);
        flush(w);
    }
    if (result == FILES_ZIP_OK && w->stopped) {
        result = say(why, why_size, FILES_ZIP_FAILED, "sending the archive stopped after %llu bytes",
                     (unsigned long long)w->sent);
    }
    if (result == FILES_ZIP_OK) {
        note(z, "zip: %u entries, %u bytes", w->entries, (unsigned)(w->offset + w->directory_len + END_LEN));
    }
    free(w->directory);
    free(w);
    return result;
}
