#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the page says about the hardware under it. A number of -1 and a NULL chip mean the chip did not say.
typedef struct {
    const char *chip;          // "ESP32-S3"
    int revision;              // major * 100 + minor, as esp_chip_info() gives it
    int cores;
    int64_t flash_bytes;       // as the flash chip reports its size
    int64_t psram_bytes;       // inside the chip package; 0 is none there
    int64_t heap_free_bytes;
    int64_t heap_total_bytes;  // the heap the free bytes are part of: the chip's own RAM the image did not take
} hw_facts_t;

// The longest piece hw_facts_json() writes, for the /status reply to count its buffer: the keys and punctuation
// take 78, a chip name 24 in quotes, a revision up to "21474836.47", cores up to 10 digits, four int64 of 19.
#define HW_FACTS_JSON_MAX 200

// Appends ,"chip":…,"chip_rev":…,"cores":…,"flash":…,"psram":…,"heap_free":… to buf at *len, null for what the
// chip did not say. False, with *len unchanged, when it does not fit.
bool hw_facts_json(char *buf, size_t size, size_t *len, const hw_facts_t *facts);

// Reads the facts of the running chip (main/hw_facts_chip.c).
void hw_facts_read(hw_facts_t *facts);
