#include "app_fs.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app-fs";

#define READ_SIZE  32
#define PROG_SIZE  32
#define LOOKAHEAD  32
#define CACHE_SIZE 128    // MIN(block_size, 4 * MAX(READ_SIZE, PROG_SIZE)), as extmod/vfs_lfsx.c
// The cache of a vfs kept mounted: opening a file reads its directory's block through the cache, and at 128 bytes that
// is dozens of reads of the flash - each one stops the cache of both cores. A piece of a page file cost the Rust app's
// network loop 40-45 ms of its core on `.30` (6.6, 26.09).
#define KEPT_CACHE_SIZE 1024
// The largest file kept inside its directory: MicroPython's default with its cache of 128 bytes. A bigger cache would
// raise LittleFS's default with it and write inline files MicroPython's own mount does not write.
#define INLINE_MAX 128

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;
static bool s_keep;       // app_fs_keep_mounted: mount on the first request and stay mounted
static bool s_mounted;
static lfs2_t s_fs;
static struct lfs2_config s_cfg;
static uint8_t s_read_buf[CACHE_SIZE];
static uint8_t s_prog_buf[CACHE_SIZE];
static uint8_t s_lookahead_buf[LOOKAHEAD];
static uint8_t s_file_buf[2][CACHE_SIZE];
static struct lfs2_file_config s_file_cfg[2] = {{.buffer = s_file_buf[0]}, {.buffer = s_file_buf[1]}};
// The cache in use and its buffers: the static ones above, or KEPT_CACHE_SIZE from the heap for a vfs kept mounted.
static lfs2_size_t s_cache_size = CACHE_SIZE;
static uint8_t *s_read = s_read_buf;
static uint8_t *s_prog = s_prog_buf;

static int dev_read(const struct lfs2_config *c, lfs2_block_t block, lfs2_off_t off, void *buffer, lfs2_size_t size)
{
    return esp_partition_read(s_part, block * c->block_size + off, buffer, size) == ESP_OK ? 0 : LFS2_ERR_IO;
}

static int dev_prog(const struct lfs2_config *c, lfs2_block_t block, lfs2_off_t off, const void *buffer, lfs2_size_t size)
{
    return esp_partition_write(s_part, block * c->block_size + off, buffer, size) == ESP_OK ? 0 : LFS2_ERR_IO;
}

static int dev_erase(const struct lfs2_config *c, lfs2_block_t block)
{
    return esp_partition_erase_range(s_part, block * c->block_size, c->block_size) == ESP_OK ? 0 : LFS2_ERR_IO;
}

static int dev_sync(const struct lfs2_config *c)
{
    return 0;
}

lfs2_t *app_fs_begin(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "vfs");
    }
    if (s_part == NULL) {
        ESP_LOGW(TAG, "no vfs partition");
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mounted) {
        return &s_fs;
    }
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.read = dev_read;
    s_cfg.prog = dev_prog;
    s_cfg.erase = dev_erase;
    s_cfg.sync = dev_sync;
    s_cfg.read_size = READ_SIZE;
    s_cfg.prog_size = PROG_SIZE;
    s_cfg.block_size = 4096;
    s_cfg.block_count = s_part->size / 4096;
    s_cfg.block_cycles = 100;
    s_cfg.cache_size = s_cache_size;
    s_cfg.inline_max = INLINE_MAX;
    s_cfg.lookahead_size = LOOKAHEAD;
    s_cfg.read_buffer = s_read;
    s_cfg.prog_buffer = s_prog;
    s_cfg.lookahead_buffer = s_lookahead_buf;
    int err = lfs2_mount(&s_fs, &s_cfg);
    if (err != 0) {
        // never formats: an unreadable filesystem is MicroPython's data, not ours to wipe
        ESP_LOGW(TAG, "vfs does not mount as LittleFS v2: %d", err);
        xSemaphoreGive(s_lock);
        return NULL;
    }
    s_mounted = s_keep;
    return &s_fs;
}

void app_fs_end(void)
{
    if (!s_mounted) {
        lfs2_unmount(&s_fs);
    }
    xSemaphoreGive(s_lock);
}

void app_fs_keep_mounted(void)
{
    s_keep = true;
    // four buffers of the bigger cache, inside: the flash is read into them while the cache - and PSRAM behind it - is off
    uint8_t *bufs = heap_caps_malloc(4 * KEPT_CACHE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (bufs == NULL) {
        ESP_LOGW(TAG, "no %d bytes for the cache of a vfs kept mounted: it keeps %d", 4 * KEPT_CACHE_SIZE, CACHE_SIZE);
        return;
    }
    s_cache_size = KEPT_CACHE_SIZE;
    s_read = bufs;
    s_prog = bufs + KEPT_CACHE_SIZE;
    s_file_cfg[0].buffer = bufs + 2 * KEPT_CACHE_SIZE;
    s_file_cfg[1].buffer = bufs + 3 * KEPT_CACHE_SIZE;
}

bool app_fs_usage(lfs2_t *fs, app_fs_usage_t *usage)
{
    usage->block_size = s_cfg.block_size;
    usage->blocks = s_cfg.block_count;
    usage->used_blocks = lfs2_fs_size(fs);
    return usage->used_blocks >= 0;
}

struct lfs2_file_config *app_fs_file_config(int slot)
{
    return &s_file_cfg[slot ? 1 : 0];
}
