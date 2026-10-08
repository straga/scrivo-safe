#include "host_name.h"

#include <stdio.h>
#include <string.h>

bool board_name(char *buf, size_t size, const uint8_t sta_mac[6])
{
    int n = snprintf(buf, size, "scrivo-safe-%02x%02x", sta_mac[4], sta_mac[5]);
    if (n < 0 || (size_t)n >= size) {
        if (size > 0) {
            buf[0] = '\0';
        }
        return false;
    }
    return true;
}

bool host_names_board(const char *host, const char *hostname, const char *sta_ip)
{
    size_t n = strlen(hostname);
    if (n > 0 && strncmp(host, hostname, n) == 0 && strcmp(host + n, ".local") == 0) {
        return true;
    }
    return sta_ip[0] != '\0' && strcmp(host, sta_ip) == 0;
}
