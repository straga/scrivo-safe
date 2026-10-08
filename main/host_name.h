#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Writes the board's name, scrivo-safe-<last two bytes of the station MAC>, into buf: the access point, mdns and the
// router's client table all show it, and its digits are the ones the router lists as the board's MAC. False, with
// buf left empty, when size cannot hold the whole name.
bool board_name(char *buf, size_t size, const uint8_t sta_mac[6]);

// True when the Host header of a request names this board: its mdns name with ".local", or its address from the
// router. An empty name or address matches nothing, so a request that comes before either is set is foreign.
bool host_names_board(const char *host, const char *hostname, const char *sta_ip);
