// The parts of app_fs.h that work on a mounted tree and need no partition, so the host tests
// (tests/host) build them with the code that uses them.
#include "app_fs.h"

#include <stdio.h>
#include <string.h>

#define PATH_MAX_LEN 128

bool app_fs_path_ok(const char *path)
{
    size_t len = strlen(path);
    if (len < 2 || len > PATH_MAX_LEN || path[0] != '/' || path[len - 1] == '/') {
        return false;
    }
    const char *part = path + 1;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) {
            return false;
        }
        part += n + (end ? 1 : 0);
    }
    return true;
}

int app_fs_make_parents(lfs2_t *fs, const char *path)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *slash = strchr(dir + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        int err = lfs2_mkdir(fs, dir);
        *slash = '/';
        if (err < 0 && err != LFS2_ERR_EXIST) {
            return err;
        }
    }
    return 0;
}

int app_fs_walk(lfs2_t *fs, const char *dir, app_fs_walk_fn fn, void *ctx)
{
    lfs2_dir_t d;
    int err = lfs2_dir_open(fs, &d, dir);
    if (err < 0) {
        return err;
    }
    struct lfs2_info info;
    while ((err = lfs2_dir_read(fs, &d, &info)) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        char path[256];
        int n = snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, info.name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        if ((err = fn(ctx, path, &info)) < 0) {
            break;
        }
        if (info.type == LFS2_TYPE_DIR && (err = app_fs_walk(fs, path, fn, ctx)) < 0) {
            break;
        }
    }
    lfs2_dir_close(fs, &d);
    return err;
}
