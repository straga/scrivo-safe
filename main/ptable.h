// The partition table at CONFIG_PARTITION_TABLE_OFFSET: read it, check a table a firmware brings
// against it, write it. Only the border between ota_0 and vfs may move; every other entry must stay
// byte for byte, so nvs, otadata, phy_init and factory (the running image) keep their places.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_flash_partitions.h"

#define PTABLE_LEN ESP_PARTITION_TABLE_MAX_LEN   // 0xC00: what gen_esp32part.py writes

typedef struct {
    uint32_t address;
    uint32_t size;
} ptable_span_t;

typedef struct {
    bool same;             // byte-equal to the board's table: nothing to write
    ptable_span_t ota0_from, ota0_to;
    ptable_span_t vfs_from, vfs_to;
    bool vfs_moves;        // the files MicroPython keeps in vfs are lost
} ptable_diff_t;

// Reads the table the board booted with into buf (PTABLE_LEN bytes).
esp_err_t ptable_read(uint8_t *buf);

// Checks table (len bytes) against current; fills diff. Returns NULL when the table may be written,
// otherwise why not, in words for the page.
const char *ptable_check(const uint8_t *table, size_t len, const uint8_t *current, ptable_diff_t *diff);

// Erases the table's sector and writes table in one piece, then reads it back. ESP_ERR_INVALID_CRC
// when the read-back differs. The board must restart right after: the running image keeps the old
// list of partitions until then.
esp_err_t ptable_write(const uint8_t *table);
