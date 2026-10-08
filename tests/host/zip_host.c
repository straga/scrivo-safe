// Host stand-in for the board around main/files_zip.c, driven by tests/host/test_files_zip.py.
//
// The image is a LittleFS v2 filesystem kept in a file and configured as the recovery image mounts vfs
// (main/app_fs.c): block 4096, read and prog size 32, cache 128, lookahead 32. One command per run:
//
//   zip_host IMAGE format BLOCKS
//   zip_host IMAGE put BOARD_PATH HOST_FILE      missing directories are made
//   zip_host IMAGE mkdir BOARD_PATH
//   zip_host IMAGE export HOST_DIR               every directory and file, as a tree
//   zip_host IMAGE zip HOST_FILE [--sink-fails-after BYTES]
//   zip_host IMAGE check BOARD_PATH
//   zip_host IMAGE unpack BOARD_PATH [--power-cut-after WRITES]
//
// zip, check and unpack print one JSON object. --power-cut-after lets that many program and erase calls
// reach the image and fails every later one, which is how flash looks once the power is gone. Programming
// ANDs bits in, as NOR flash does, so a write without an erase is not hidden.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_fs.h"
#include "files_zip.h"

#define BLOCK 4096
#define CACHE 128
#define LOOKAHEAD 32

static uint8_t *s_image;
static size_t s_blocks;
static long s_writes;
static long s_cut = -1;

static uint8_t s_read_buf[CACHE], s_prog_buf[CACHE], s_lookahead_buf[LOOKAHEAD];
static uint8_t s_file_buf[2][CACHE];
static struct lfs2_file_config s_file_cfg[2] = {{.buffer = s_file_buf[0]}, {.buffer = s_file_buf[1]}};

static bool power_on(void)
{
    if (s_cut >= 0 && s_writes >= s_cut) {
        return false;
    }
    s_writes++;
    return true;
}

static int dev_read(const struct lfs2_config *c, lfs2_block_t block, lfs2_off_t off, void *buffer, lfs2_size_t size)
{
    (void)c;
    memcpy(buffer, s_image + (size_t)block * BLOCK + off, size);
    return 0;
}

static int dev_prog(const struct lfs2_config *c, lfs2_block_t block, lfs2_off_t off, const void *buffer, lfs2_size_t size)
{
    (void)c;
    if (!power_on()) {
        return LFS2_ERR_IO;
    }
    const uint8_t *src = buffer;
    uint8_t *dst = s_image + (size_t)block * BLOCK + off;
    for (lfs2_size_t i = 0; i < size; i++) {
        dst[i] &= src[i];
    }
    return 0;
}

static int dev_erase(const struct lfs2_config *c, lfs2_block_t block)
{
    (void)c;
    if (!power_on()) {
        return LFS2_ERR_IO;
    }
    memset(s_image + (size_t)block * BLOCK, 0xff, BLOCK);
    return 0;
}

static int dev_sync(const struct lfs2_config *c)
{
    (void)c;
    return 0;
}

static struct lfs2_config config(void)
{
    return (struct lfs2_config){
        .read = dev_read, .prog = dev_prog, .erase = dev_erase, .sync = dev_sync,
        .read_size = 32, .prog_size = 32, .block_size = BLOCK, .block_count = s_blocks,
        .block_cycles = 100, .cache_size = CACHE, .lookahead_size = LOOKAHEAD,
        .read_buffer = s_read_buf, .prog_buffer = s_prog_buf, .lookahead_buffer = s_lookahead_buf,
    };
}

static void fail(const char *what, const char *detail)
{
    fprintf(stderr, "zip_host: %s: %s\n", what, detail);
    exit(2);
}

static void save(const char *image)
{
    FILE *f = fopen(image, "wb");
    if (f == NULL || fwrite(s_image, BLOCK, s_blocks, f) != s_blocks || fclose(f) != 0) {
        fail("cannot write the image", image);
    }
}

static void load(const char *image)
{
    FILE *f = fopen(image, "rb");
    struct stat st;
    if (f == NULL || stat(image, &st) != 0 || st.st_size % BLOCK != 0) {
        fail("cannot read the image", image);
    }
    s_blocks = (size_t)st.st_size / BLOCK;
    s_image = malloc((size_t)st.st_size);
    if (s_image == NULL || fread(s_image, BLOCK, s_blocks, f) != s_blocks) {
        fail("cannot read the image", image);
    }
    fclose(f);
}

static void json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (c < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

static void put(lfs2_t *fs, const char *path, const char *host_file)
{
    FILE *in = fopen(host_file, "rb");
    if (in == NULL) {
        fail("cannot open", host_file);
    }
    app_fs_make_parents(fs, path);
    lfs2_file_t file;
    if (lfs2_file_opencfg(fs, &file, path, LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_TRUNC, &s_file_cfg[0]) < 0) {
        fail("cannot create", path);
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (lfs2_file_write(fs, &file, buf, n) != (lfs2_ssize_t)n) {
            fail("cannot write", path);
        }
    }
    if (lfs2_file_close(fs, &file) < 0) {
        fail("cannot close", path);
    }
    fclose(in);
}

typedef struct {
    lfs2_t *fs;
    const char *host_dir;
} export_t;

// The walk passes a directory before what is in it, so every host directory exists before its files.
static int export_entry(void *ctx, const char *path, const struct lfs2_info *info)
{
    export_t *e = ctx;
    char host_path[1024];
    snprintf(host_path, sizeof(host_path), "%s%s", e->host_dir, path);
    if (info->type == LFS2_TYPE_DIR) {
        mkdir(host_path, 0755);
        return 0;
    }
    lfs2_file_t file;
    FILE *out = fopen(host_path, "wb");
    if (out == NULL || lfs2_file_opencfg(e->fs, &file, path, LFS2_O_RDONLY, &s_file_cfg[0]) < 0) {
        fail("cannot export", path);
    }
    char buf[4096];
    lfs2_ssize_t n;
    while ((n = lfs2_file_read(e->fs, &file, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, out);
    }
    lfs2_file_close(e->fs, &file);
    fclose(out);
    return 0;
}

// What check and unpack report, kept until the JSON is printed.
#define MAX_LINES 1024
static struct {
    char *path;
    bool dir;
} s_removed[MAX_LINES];
static size_t s_removed_n;
static struct {
    char *line;
    long writes;
} s_log[MAX_LINES];
static size_t s_log_n;

static void on_removed(void *ctx, const char *path, bool dir)
{
    (void)ctx;
    if (s_removed_n < MAX_LINES) {
        s_removed[s_removed_n].path = strdup(path);
        s_removed[s_removed_n++].dir = dir;
    }
}

static void on_log(const char *line)
{
    if (s_log_n < MAX_LINES) {
        s_log[s_log_n].line = strdup(line);
        s_log[s_log_n++].writes = s_writes;
    }
}

static const char *result_name(files_zip_result_t r)
{
    switch (r) {
    case FILES_ZIP_OK: return "OK";
    case FILES_ZIP_NOT_FOUND: return "NOT_FOUND";
    case FILES_ZIP_REFUSED: return "REFUSED";
    case FILES_ZIP_FAILED: return "FAILED";
    }
    return "?";
}

static void print_result(files_zip_result_t r, const char *why, const files_zip_summary_t *sum)
{
    printf("{\"result\":\"%s\",\"why\":", result_name(r));
    json_str(r == FILES_ZIP_OK ? "" : why);
    if (sum != NULL) {
        printf(",\"summary\":{\"files\":%u,\"dirs\":%u,\"skipped\":%u,\"removed\":%u,\"bytes\":%u,"
               "\"blocks_needed\":%u,\"blocks_free\":%u}",
               sum->files, sum->dirs, sum->skipped, sum->removed, (unsigned)sum->bytes,
               (unsigned)sum->blocks_needed, (unsigned)sum->blocks_free);
    }
    printf(",\"removed\":[");
    for (size_t i = 0; i < s_removed_n; i++) {
        printf("%s{\"path\":", i ? "," : "");
        json_str(s_removed[i].path);
        printf(",\"dir\":%s}", s_removed[i].dir ? "true" : "false");
    }
    printf("],\"log\":[");
    for (size_t i = 0; i < s_log_n; i++) {
        printf("%s{\"line\":", i ? "," : "");
        json_str(s_log[i].line);
        printf(",\"writes\":%ld}", s_log[i].writes);
    }
    printf("],\"writes\":%ld}\n", s_writes);
}

typedef struct {
    FILE *out;
    long left;
} sink_t;

static bool to_file(void *ctx, const void *data, size_t len)
{
    sink_t *s = ctx;
    if (s->left >= 0 && (long)len > s->left) {
        return false;
    }
    if (s->left >= 0) {
        s->left -= (long)len;
    }
    return fwrite(data, 1, len, s->out) == len;
}

static long option(int argc, char **argv, const char *name)
{
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return atol(argv[i + 1]);
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fail("usage", "zip_host IMAGE format|put|mkdir|export|zip|check|unpack ...");
    }
    const char *image = argv[1], *cmd = argv[2];
    if (strcmp(cmd, "format") == 0) {
        s_blocks = (size_t)atol(argv[3]);
        s_image = malloc(s_blocks * BLOCK);
        memset(s_image, 0xff, s_blocks * BLOCK);
        struct lfs2_config cfg = config();
        lfs2_t fs;
        if (lfs2_format(&fs, &cfg) != 0) {
            fail("format failed", image);
        }
        save(image);
        return 0;
    }
    load(image);
    struct lfs2_config cfg = config();
    lfs2_t fs;
    if (lfs2_mount(&fs, &cfg) != 0) {
        fail("the image does not mount", image);
    }
    s_writes = 0;
    s_cut = option(argc, argv, "--power-cut-after");
    files_zip_fs_t z = {.fs = &fs, .file_cfg = {&s_file_cfg[0], &s_file_cfg[1]}, .log = on_log};
    char why[256] = "";
    files_zip_summary_t sum = {0};
    if (strcmp(cmd, "put") == 0 && argc >= 5) {
        put(&fs, argv[3], argv[4]);
    } else if (strcmp(cmd, "mkdir") == 0) {
        app_fs_make_parents(&fs, argv[3]);
        lfs2_mkdir(&fs, argv[3]);
    } else if (strcmp(cmd, "export") == 0) {
        export_t e = {.fs = &fs, .host_dir = argv[3]};
        if (app_fs_walk(&fs, "/", export_entry, &e) < 0) {
            fail("cannot export", argv[3]);
        }
    } else if (strcmp(cmd, "zip") == 0) {
        sink_t sink = {.out = fopen(argv[3], "wb"), .left = option(argc, argv, "--sink-fails-after")};
        if (sink.out == NULL) {
            fail("cannot create", argv[3]);
        }
        files_zip_result_t r = files_zip_write(&z, to_file, &sink, why, sizeof(why));
        fclose(sink.out);
        print_result(r, why, NULL);
    } else if (strcmp(cmd, "check") == 0) {
        files_zip_result_t r = files_zip_check(&z, argv[3], &sum, on_removed, NULL, why, sizeof(why));
        print_result(r, why, &sum);
    } else if (strcmp(cmd, "unpack") == 0) {
        files_zip_result_t r = files_zip_unpack(&z, argv[3], &sum, on_removed, NULL, why, sizeof(why));
        print_result(r, why, &sum);
    } else {
        fail("unknown command", cmd);
    }
    s_cut = -1;   // unmount reads only; the image keeps whatever reached flash before the cut
    lfs2_unmount(&fs);
    save(image);
    return 0;
}
