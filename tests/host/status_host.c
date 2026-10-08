// Host harness for main/hw_facts.c and main/host_name.c, driven by tests/host/test_status_parts.py:
//   status_host hw <chip or -> <revision> <cores> <flash> <psram> <heap free> <heap total> <buffer size>
//   status_host hwmax
//   status_host host <Host header> <hostname> <station address>
// hw prints the JSON piece, or NOFIT with the length it left; host prints ours or foreign.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_name.h"
#include "hw_facts.h"

int main(int argc, char **argv)
{
    if (argc == 10 && strcmp(argv[1], "hw") == 0) {
        hw_facts_t facts = {
            .chip = strcmp(argv[2], "-") == 0 ? NULL : argv[2],
            .revision = atoi(argv[3]),
            .cores = atoi(argv[4]),
            .flash_bytes = atoll(argv[5]),
            .psram_bytes = atoll(argv[6]),
            .heap_free_bytes = atoll(argv[7]),
            .heap_total_bytes = atoll(argv[8]),
        };
        size_t size = strtoul(argv[9], NULL, 10);
        char *buf = malloc(size);
        size_t len = 0;
        if (!hw_facts_json(buf, size, &len, &facts)) {
            printf("NOFIT %zu\n", len);
        } else {
            printf("%.*s\n", (int)len, buf);
        }
        free(buf);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "hwmax") == 0) {
        printf("%d\n", HW_FACTS_JSON_MAX);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "name") == 0) {
        uint8_t mac[6];
        unsigned m[6];
        if (sscanf(argv[2], "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
            return 2;
        }
        for (int i = 0; i < 6; i++) {
            mac[i] = (uint8_t)m[i];
        }
        size_t size = strtoul(argv[3], NULL, 10);
        char *buf = calloc(size ? size : 1, 1);
        if (board_name(buf, size, mac)) {
            printf("%s\n", buf);
        } else {
            printf("NOFIT '%s'\n", size ? buf : "");
        }
        free(buf);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "host") == 0) {
        puts(host_names_board(argv[2], argv[3], argv[4]) ? "ours" : "foreign");
        return 0;
    }
    fprintf(stderr, "usage: status_host hw|hwmax|host ...\n");
    return 2;
}
