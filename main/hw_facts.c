#include "hw_facts.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// A count as JSON: null for what the chip did not say.
static void count_json(char *out, size_t size, int64_t value)
{
    if (value < 0) {
        snprintf(out, size, "null");
    } else {
        snprintf(out, size, "%" PRId64, value);
    }
}

bool hw_facts_json(char *buf, size_t size, size_t *len, const hw_facts_t *facts)
{
    // Chip names come from a table in hw_facts_chip.c: nothing in them to escape.
    char chip[32] = "null";
    char revision[16] = "null";
    char cores[16], flash[24], psram[24], heap[24], heap_total[24];
    if (facts->chip != NULL) {
        snprintf(chip, sizeof(chip), "\"%.24s\"", facts->chip);
    }
    if (facts->revision >= 0) {
        snprintf(revision, sizeof(revision), "\"%d.%d\"", facts->revision / 100, facts->revision % 100);
    }
    count_json(cores, sizeof(cores), facts->cores);
    count_json(flash, sizeof(flash), facts->flash_bytes);
    count_json(psram, sizeof(psram), facts->psram_bytes);
    count_json(heap, sizeof(heap), facts->heap_free_bytes);
    count_json(heap_total, sizeof(heap_total), facts->heap_total_bytes);

    char piece[HW_FACTS_JSON_MAX + 1];
    int n = snprintf(piece, sizeof(piece),
                     ",\"chip\":%s,\"chip_rev\":%s,\"cores\":%s,\"flash\":%s,\"psram\":%s,\"heap_free\":%s,\"heap_total\":%s",
                     chip, revision, cores, flash, psram, heap, heap_total);
    if (n < 0 || (size_t)n >= sizeof(piece) || *len + (size_t)n >= size) {
        return false;
    }
    memcpy(buf + *len, piece, (size_t)n);
    *len += (size_t)n;
    return true;
}
