// Receiving request bodies. Every handler that reads a body goes through here.
#pragma once

#include <stddef.h>
#include "esp_http_server.h"

// Socket timeouts in a row after which a body is given up: recv_wait_timeout is 5 s (not overridden in
// main.c), so six make 30 s without a byte. This limit is the other half of stopping the idle count during
// a long operation (while_count_stopped in main.c): without it a sender gone without closing the connection
// keeps the receive - and the stopped count - waiting for good, the forgotten tab again from the other side.
// A live link does not go silent for half a minute, not even a slow phone.
#define BODY_STALL_TIMEOUTS 6

// body_recv returned it: the sender went silent for the whole stall limit.
#define BODY_STALLED (-100)

// Receives the next bytes of the body into buf: the count (> 0), 0 when the sender closed the connection,
// BODY_STALLED, or another negative httpd error. Timeouts shorter than the limit are waited through.
int body_recv(httpd_req_t *req, char *buf, size_t len);
