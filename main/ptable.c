#include "ptable.h"

#include <string.h>

#include "esp_flash.h"
#include "esp_flash_internal.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "ptable";

#define ENTRY_LEN      sizeof(esp_partition_info_t)   // 32
#define MAX_ENTRIES    (PTABLE_LEN / ENTRY_LEN - 1)   // the last slot is kept for the MD5 entry
#define SECTOR         0x1000
#define MIN_OTA0_SIZE  0x100000   // 1 MB: a MicroPython image is 1.6 MB, a smaller slot is a mistake
#define MIN_VFS_SIZE   0x10000    // 16 LittleFS blocks

esp_err_t ptable_read(uint8_t *buf)
{
    return esp_flash_read(esp_flash_default_chip, buf, CONFIG_PARTITION_TABLE_OFFSET, PTABLE_LEN);
}

// Counts the partition entries and requires the MD5 entry right after them and 0xFF to the end,
// the shape gen_esp32part.py writes. -1 when the table is not that.
static int count_entries(const uint8_t *table)
{
    int n = 0;
    while (n < MAX_ENTRIES && ((const esp_partition_info_t *)(table + n * ENTRY_LEN))->magic == ESP_PARTITION_MAGIC) {
        n++;
    }
    const esp_partition_info_t *md5 = (const esp_partition_info_t *)(table + n * ENTRY_LEN);
    if (n == 0 || md5->magic != ESP_PARTITION_MAGIC_MD5) {
        return -1;
    }
    for (size_t i = (n + 1) * ENTRY_LEN; i < PTABLE_LEN; i++) {
        if (table[i] != 0xFF) {
            return -1;
        }
    }
    return n;
}

static int find_label(const uint8_t *table, int n, const char *label)
{
    for (int i = 0; i < n; i++) {
        const esp_partition_info_t *p = (const esp_partition_info_t *)(table + i * ENTRY_LEN);
        if (strncmp((const char *)p->label, label, sizeof(p->label)) == 0) {
            return i;
        }
    }
    return -1;
}

// The same partition with only its address and size free to differ.
static bool same_but_span(const esp_partition_info_t *a, const esp_partition_info_t *b)
{
    return a->magic == b->magic && a->type == b->type && a->subtype == b->subtype &&
           memcmp(a->label, b->label, sizeof(a->label)) == 0 && a->flags == b->flags;
}

const char *ptable_check(const uint8_t *table, size_t len, const uint8_t *current, ptable_diff_t *diff)
{
    memset(diff, 0, sizeof(*diff));
    if (len != PTABLE_LEN) {
        return "not a partition table: 3072 bytes expected, as gen_esp32part.py writes them";
    }
    int n = count_entries(table);
    if (n < 0) {
        return "not a partition table: partition entries, an MD5 entry and 0xFF to the end expected";
    }
    int n_verified = 0;
    if (esp_partition_table_verify((const esp_partition_info_t *)table, false, &n_verified) != ESP_OK) {
        return "the partition table is damaged: its MD5 or an entry does not check out";
    }
    int cur_n = count_entries(current);
    int ota0 = find_label(current, cur_n, "ota_0");
    int vfs = find_label(current, cur_n, "vfs");
    if (cur_n < 0 || ota0 < 0 || vfs < 0) {
        return "the board's own table has no ota_0 or vfs: nothing here may be moved";
    }
    if (n != cur_n) {
        return "a different set of partitions: this firmware is built for another layout or another board";
    }
    for (int i = 0; i < n; i++) {
        const uint8_t *a = table + i * ENTRY_LEN;
        const uint8_t *b = current + i * ENTRY_LEN;
        if (i != ota0 && i != vfs && memcmp(a, b, ENTRY_LEN) != 0) {
            return "nvs, otadata, phy_init or factory differ from the board's: only the ota_0 / vfs border may move";
        }
    }
    const esp_partition_info_t *new_ota0 = (const esp_partition_info_t *)(table + ota0 * ENTRY_LEN);
    const esp_partition_info_t *new_vfs = (const esp_partition_info_t *)(table + vfs * ENTRY_LEN);
    const esp_partition_info_t *cur_ota0 = (const esp_partition_info_t *)(current + ota0 * ENTRY_LEN);
    const esp_partition_info_t *cur_vfs = (const esp_partition_info_t *)(current + vfs * ENTRY_LEN);
    if (!same_but_span(new_ota0, cur_ota0) || !same_but_span(new_vfs, cur_vfs)) {
        return "ota_0 or vfs changed its name, type or flags: only the border between them may move";
    }
    if (new_ota0->pos.offset != cur_ota0->pos.offset) {
        return "ota_0 must start where it starts now";
    }
    if (new_vfs->pos.offset != new_ota0->pos.offset + new_ota0->pos.size) {
        return "vfs must start right where ota_0 ends";
    }
    if (new_vfs->pos.offset + new_vfs->pos.size != cur_vfs->pos.offset + cur_vfs->pos.size) {
        return "vfs must end where it ends now";
    }
    if (new_ota0->pos.size % SECTOR || new_vfs->pos.size % SECTOR) {
        return "ota_0 and vfs sizes must be whole 4 KB sectors";
    }
    if (new_ota0->pos.size < MIN_OTA0_SIZE || new_vfs->pos.size < MIN_VFS_SIZE) {
        return "ota_0 under 1 MB or vfs under 64 KB: this layout leaves no room for the firmware or its files";
    }
    diff->same = memcmp(table, current, PTABLE_LEN) == 0;
    diff->ota0_from = (ptable_span_t){cur_ota0->pos.offset, cur_ota0->pos.size};
    diff->ota0_to = (ptable_span_t){new_ota0->pos.offset, new_ota0->pos.size};
    diff->vfs_from = (ptable_span_t){cur_vfs->pos.offset, cur_vfs->pos.size};
    diff->vfs_to = (ptable_span_t){new_vfs->pos.offset, new_vfs->pos.size};
    diff->vfs_moves = new_vfs->pos.offset != cur_vfs->pos.offset;
    return NULL;
}

esp_err_t ptable_write(const uint8_t *table)
{
    static uint8_t back[PTABLE_LEN];
    // The table region is refused by the flash driver unless asked for, the way esp_ota_begin does it
    // for a partition table update; the permission is taken back right after.
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false);
    esp_err_t err = esp_flash_erase_region(esp_flash_default_chip, CONFIG_PARTITION_TABLE_OFFSET, SECTOR);
    if (err == ESP_OK) {
        err = esp_flash_write(esp_flash_default_chip, table, CONFIG_PARTITION_TABLE_OFFSET, PTABLE_LEN);
    }
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
    if (err == ESP_OK) {
        err = ptable_read(back);
    }
    if (err == ESP_OK && memcmp(back, table, PTABLE_LEN) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }
    ESP_LOGW(TAG, "partition table written at 0x%x: %s", CONFIG_PARTITION_TABLE_OFFSET, esp_err_to_name(err));
    return err;
}
