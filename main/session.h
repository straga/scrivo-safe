// Signed-in sessions of the portal. A person signs in with a form on the page, the board answers
// with a random cookie, and every action checks it. No browser pop-up is ever asked for: the
// captive mini-browser a phone opens has none.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"

// Remembers the credentials read from nvs; both empty means nobody can sign in.
void session_set_credentials(const char *user, const char *pass);

// True when credentials exist, so signing in is possible at all.
bool session_credentials_set(void);

// Compares a submitted name and password with the stored ones in constant time.
bool session_credentials_match(const char *user, const char *pass);

// Starts a session for this request: puts its cookie into the response headers.
void session_open(httpd_req_t *req);

// Ends the session the request carries, if any, and tells the browser to drop the cookie.
void session_close(httpd_req_t *req);

// True when the request carries the cookie of a live session.
bool session_valid(httpd_req_t *req);
