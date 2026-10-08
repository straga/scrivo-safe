// HTTP handlers for the MicroPython files in vfs (app_fs.h). They check nothing about the person:
// main.c lets a request through only with a session.
#pragma once

#include "esp_http_server.h"

// GET /files: {"entries": [{"path": "/lib", "dir": true, "size": 0}, ...], "fs": {"block_size",
// "blocks", "used_blocks"}}, every file and directory, depth first.
esp_err_t files_list_get(httpd_req_t *req);

// GET /file?path=/main.py: the file's bytes; 404 when there is no such file.
esp_err_t file_get(httpd_req_t *req);

// POST /file?path=/main.py with the new bytes as the body: written to <path>.part, then renamed
// over the old file, so a broken upload leaves the old file in place. Missing directories are made.
esp_err_t file_post(httpd_req_t *req);

// POST /file/delete?path=/main.py: removes a file or an empty directory.
esp_err_t file_delete_post(httpd_req_t *req);

// GET /files/zip: every file and directory as one zip with stored entries (files_zip.h), saved as
// board-files.zip. A failure after the first bytes closes the connection, so a broken download
// shows as broken rather than as a shorter archive.
esp_err_t files_archive_get(httpd_req_t *req);

// POST /files/unpack/check?path=/a.zip: reads the archive and checks it against the board, writes
// nothing. {"remove": [{"path", "dir"}, ...], "files", "dirs", "skipped", "bytes", "blocks_needed",
// "blocks_free"}: remove is what POST /files/unpack takes off the board. 400 with the reason when
// the archive cannot be unpacked, 404 when there is no such file.
esp_err_t files_unpack_check_post(httpd_req_t *req);

// POST /files/unpack?path=/a.zip: vfs becomes exactly the archive, then the archive is deleted
// (files_zip_unpack). {"files", "dirs", "skipped", "bytes", "blocks_needed", "blocks_free",
// "removed", "heap_free_before", "heap_free_lowest", "heap_free_after"}. 400 and 404 as the check;
// 500 when flash fails, and the text says whether the names had started to move.
esp_err_t files_unpack_post(httpd_req_t *req);
