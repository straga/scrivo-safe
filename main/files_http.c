#include "files_http.h"

#include <stdio.h>
#include <string.h>

#include "app_fs.h"
#include "body.h"
#include "esp_log.h"
#include "esp_system.h"
#include "files_zip.h"
#include "form.h"

static const char *TAG = "files";

#define PATH_BUF   3 * 128 + 1   // a percent-encoded path is up to three times longer
#define CHUNK      1024
#define PART_SUFFIX ".part"

// Reads the path= query value, decoded and checked; sends 400 and returns false when it is unusable.
static bool query_path(httpd_req_t *req, char *path, size_t size)
{
    char query[PATH_BUF + 8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", path, size) != ESP_OK || !form_decode(path) || !app_fs_path_ok(path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path must be absolute, without empty, . or .. parts");
        return false;
    }
    return true;
}

static lfs2_t *begin_or_fail(httpd_req_t *req)
{
    lfs2_t *fs = app_fs_begin();
    if (fs == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the MicroPython filesystem in vfs does not mount");
    }
    return fs;
}

// Appends text to a chunked reply through a small buffer; an empty chunk would end the reply.
typedef struct {
    httpd_req_t *req;
    char buf[512];
    size_t len;
    bool failed;
} reply_t;

static void reply_put(reply_t *r, const char *text)
{
    size_t n = strlen(text);
    if (r->len + n > sizeof(r->buf)) {
        r->failed |= r->len && httpd_resp_send_chunk(r->req, r->buf, r->len) != ESP_OK;
        r->len = 0;
    }
    if (n > sizeof(r->buf)) {
        r->failed |= httpd_resp_send_chunk(r->req, text, n) != ESP_OK;
        return;
    }
    memcpy(r->buf + r->len, text, n);
    r->len += n;
}

// Puts s as the inside of a JSON string. Paths are plain names, but a quote or a control
// character must not break the reply.
static void reply_put_json(reply_t *r, const char *s)
{
    char piece[8];
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            snprintf(piece, sizeof(piece), "\\%c", c);
        } else if (c < 0x20) {
            snprintf(piece, sizeof(piece), "\\u%04x", c);
        } else {
            piece[0] = (char)c;
            piece[1] = '\0';
        }
        reply_put(r, piece);
    }
}

typedef struct {
    reply_t *r;
    bool first;
} listing_t;

static int list_entry(void *ctx, const char *path, const struct lfs2_info *info)
{
    listing_t *l = ctx;
    bool is_dir = info->type == LFS2_TYPE_DIR;
    char tail[64];
    snprintf(tail, sizeof(tail), "\",\"dir\":%s,\"size\":%u}", is_dir ? "true" : "false", is_dir ? 0 : (unsigned)info->size);
    reply_put(l->r, l->first ? "{\"path\":\"" : ",{\"path\":\"");
    l->first = false;
    reply_put_json(l->r, path);
    reply_put(l->r, tail);
    return 0;
}

esp_err_t files_list_get(httpd_req_t *req)
{
    lfs2_t *fs = begin_or_fail(req);
    if (fs == NULL) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    reply_t r = {.req = req};
    listing_t listing = {.r = &r, .first = true};
    reply_put(&r, "{\"entries\":[");
    int err = app_fs_walk(fs, "/", list_entry, &listing);
    app_fs_usage_t usage;
    bool counted = app_fs_usage(fs, &usage);
    app_fs_end();
    char tail[128];
    snprintf(tail, sizeof(tail), "],\"fs\":{\"block_size\":%u,\"blocks\":%u,\"used_blocks\":%d},\"error\":%d}",
             (unsigned)usage.block_size, (unsigned)usage.blocks, counted ? (int)usage.used_blocks : -1, err < 0 ? err : 0);
    reply_put(&r, tail);
    if (r.len) {
        r.failed |= httpd_resp_send_chunk(req, r.buf, r.len) != ESP_OK;
    }
    if (err < 0) {
        ESP_LOGW(TAG, "listing stopped: %d", err);
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t file_get(httpd_req_t *req)
{
    char path[PATH_BUF];
    if (!query_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    lfs2_t *fs = begin_or_fail(req);
    if (fs == NULL) {
        return ESP_OK;
    }
    lfs2_file_t file;
    int err = lfs2_file_opencfg(fs, &file, path, LFS2_O_RDONLY, app_fs_file_config(0));
    if (err < 0) {
        app_fs_end();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
    }
    const char *name = strrchr(path, '/') + 1;
    char disposition[200];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    char buf[CHUNK];
    lfs2_ssize_t n;
    esp_err_t sent = ESP_OK;
    while (sent == ESP_OK && (n = lfs2_file_read(fs, &file, buf, sizeof(buf))) > 0) {
        sent = httpd_resp_send_chunk(req, buf, n);
    }
    lfs2_file_close(fs, &file);
    app_fs_end();
    return sent == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : sent;
}

esp_err_t file_post(httpd_req_t *req)
{
    char path[PATH_BUF];
    if (!query_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    char part[PATH_BUF + sizeof(PART_SUFFIX)];
    snprintf(part, sizeof(part), "%s" PART_SUFFIX, path);
    lfs2_t *fs = begin_or_fail(req);
    if (fs == NULL) {
        return ESP_OK;
    }
    const char *refusal = NULL;
    int err = app_fs_make_parents(fs, path);
    lfs2_file_t file;
    bool open = false;
    if (err == 0) {
        err = lfs2_file_opencfg(fs, &file, part, LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_TRUNC, app_fs_file_config(0));
        open = err == 0;
    }
    char buf[CHUNK];
    size_t written = 0;
    while (err == 0 && written < req->content_len) {
        int got = body_recv(req, buf, sizeof(buf));
        if (got <= 0) {
            refusal = got == BODY_STALLED ? "upload stalled: no data for 30 s; the old file is left as it was"
                                          : "upload interrupted; the old file is left as it was";
            break;
        }
        lfs2_ssize_t n = lfs2_file_write(fs, &file, buf, got);
        if (n < 0) {
            err = n;
            break;
        }
        written += (size_t)got;
    }
    if (open) {
        int close_err = lfs2_file_close(fs, &file);
        err = err ? err : close_err;
    }
    if (err == 0 && refusal == NULL) {
        err = lfs2_rename(fs, part, path);
    }
    if (err != 0 || refusal != NULL) {
        lfs2_remove(fs, part);
    }
    app_fs_end();
    if (refusal != NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, refusal);
    }
    if (err != 0) {
        ESP_LOGW(TAG, "writing %s failed: %d", path, err);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   err == LFS2_ERR_NOSPC ? "no space left in vfs" : "writing the file failed");
    }
    ESP_LOGI(TAG, "wrote %s, %u bytes", path, (unsigned)written);
    char reply[PATH_BUF + 48];
    snprintf(reply, sizeof(reply), "Saved %u bytes to %s.\n", (unsigned)written, path);
    return httpd_resp_sendstr(req, reply);
}

esp_err_t file_delete_post(httpd_req_t *req)
{
    char path[PATH_BUF];
    if (!query_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    lfs2_t *fs = begin_or_fail(req);
    if (fs == NULL) {
        return ESP_OK;
    }
    int err = lfs2_remove(fs, path);
    app_fs_end();
    if (err == LFS2_ERR_NOENT) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
    }
    if (err == LFS2_ERR_NOTEMPTY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "the directory is not empty");
    }
    if (err != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "deleting failed");
    }
    ESP_LOGI(TAG, "deleted %s", path);
    return httpd_resp_sendstr(req, "Deleted.\n");
}

// Lowest free heap seen at a progress line of the running archive job: the inflate buffers and the
// archive directory are all allocated while those lines are written.
static size_t s_heap_lowest;

static void zip_log(const char *line)
{
    size_t free_now = esp_get_free_heap_size();
    s_heap_lowest = free_now < s_heap_lowest ? free_now : s_heap_lowest;
    ESP_LOGI(TAG, "%s; heap free %u", line, (unsigned)free_now);
}

static files_zip_fs_t zip_fs(lfs2_t *fs)
{
    return (files_zip_fs_t){.fs = fs, .file_cfg = {app_fs_file_config(0), app_fs_file_config(1)}, .log = zip_log};
}

static esp_err_t zip_refused(httpd_req_t *req, files_zip_result_t result, const char *why)
{
    ESP_LOGW(TAG, "%s", why);
    return httpd_resp_send_err(req, result == FILES_ZIP_NOT_FOUND ? HTTPD_404_NOT_FOUND
                                    : result == FILES_ZIP_REFUSED ? HTTPD_400_BAD_REQUEST
                                                                  : HTTPD_500_INTERNAL_SERVER_ERROR, why);
}

static bool to_reply(void *ctx, const void *data, size_t len)
{
    return httpd_resp_send_chunk(ctx, data, len) == ESP_OK;
}

esp_err_t files_archive_get(httpd_req_t *req)
{
    lfs2_t *fs = begin_or_fail(req);
    if (fs == NULL) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/zip");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"board-files.zip\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    files_zip_fs_t z = zip_fs(fs);
    char why[200];
    files_zip_result_t result = files_zip_write(&z, to_reply, req, why, sizeof(why));
    app_fs_end();
    if (result != FILES_ZIP_OK) {
        ESP_LOGW(TAG, "archive not sent: %s", why);
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

typedef struct {
    reply_t r;
    bool first;
} removed_reply_t;

static void removed_put(void *ctx, const char *path, bool dir)
{
    removed_reply_t *p = ctx;
    reply_put(&p->r, p->first ? "{\"remove\":[{\"path\":\"" : ",{\"path\":\"");
    p->first = false;
    reply_put_json(&p->r, path);
    reply_put(&p->r, dir ? "\",\"dir\":true}" : "\",\"dir\":false}");
}

static void summary_put(reply_t *r, const files_zip_summary_t *sum)
{
    char text[200];
    snprintf(text, sizeof(text), "\"files\":%u,\"dirs\":%u,\"skipped\":%u,\"bytes\":%u,\"blocks_needed\":%u,\"blocks_free\":%u",
             sum->files, sum->dirs, sum->skipped, (unsigned)sum->bytes, (unsigned)sum->blocks_needed,
             (unsigned)sum->blocks_free);
    reply_put(r, text);
}

esp_err_t files_unpack_check_post(httpd_req_t *req)
{
    char path[PATH_BUF];
    if (!query_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    lfs2_t *fs = begin_or_fail(req);
    if (fs == NULL) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    files_zip_fs_t z = zip_fs(fs);
    files_zip_summary_t sum;
    // files_zip_check names what would be removed only after the archive has passed, so a refusal
    // finds nothing sent yet and can still answer with its status.
    removed_reply_t reply = {.r = {.req = req}, .first = true};
    char why[256];
    files_zip_result_t result = files_zip_check(&z, path, &sum, removed_put, &reply, why, sizeof(why));
    app_fs_end();
    if (result != FILES_ZIP_OK) {
        return zip_refused(req, result, why);
    }
    if (reply.first) {
        reply_put(&reply.r, "{\"remove\":[");
    }
    reply_put(&reply.r, "],");
    summary_put(&reply.r, &sum);
    reply_put(&reply.r, "}");
    if (reply.r.len) {
        reply.r.failed |= httpd_resp_send_chunk(req, reply.r.buf, reply.r.len) != ESP_OK;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t files_unpack_post(httpd_req_t *req)
{
    char path[PATH_BUF];
    if (!query_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    lfs2_t *fs = begin_or_fail(req);
    if (fs == NULL) {
        return ESP_OK;
    }
    size_t heap_before = esp_get_free_heap_size();
    s_heap_lowest = heap_before;
    files_zip_fs_t z = zip_fs(fs);
    files_zip_summary_t sum;
    char why[256];
    files_zip_result_t result = files_zip_unpack(&z, path, &sum, NULL, NULL, why, sizeof(why));
    app_fs_end();
    size_t heap_after = esp_get_free_heap_size();
    ESP_LOGI(TAG, "unpack %s: heap free %u before, %u lowest, %u after", path, (unsigned)heap_before,
             (unsigned)s_heap_lowest, (unsigned)heap_after);
    if (result != FILES_ZIP_OK) {
        return zip_refused(req, result, why);
    }
    httpd_resp_set_type(req, "application/json");
    reply_t r = {.req = req};
    reply_put(&r, "{");
    summary_put(&r, &sum);
    char tail[160];
    snprintf(tail, sizeof(tail), ",\"removed\":%u,\"heap_free_before\":%u,\"heap_free_lowest\":%u,\"heap_free_after\":%u}",
             sum.removed, (unsigned)heap_before, (unsigned)s_heap_lowest, (unsigned)heap_after);
    reply_put(&r, tail);
    if (r.len) {
        r.failed |= httpd_resp_send_chunk(req, r.buf, r.len) != ESP_OK;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}
