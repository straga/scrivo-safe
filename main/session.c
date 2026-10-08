#include "session.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"

#define SESSION_SLOTS     4
#define SESSION_TOKEN_LEN 16                        // random bytes
#define SESSION_HEX_LEN   (2 * SESSION_TOKEN_LEN)
#define CRED_MAX_LEN      64

typedef struct {
    char token[SESSION_HEX_LEN + 1];
    unsigned long last_used;   // order of use, to replace the least recent session
} session_t;

// The HTTP server runs every handler on its one task, so the table needs no lock.
static session_t s_sessions[SESSION_SLOTS];
static unsigned long s_clock;
static char s_user[CRED_MAX_LEN + 1];
static char s_pass[CRED_MAX_LEN + 1];
// Set-Cookie must outlive the handler call that sets it, until the response is sent.
static char s_cookie_header[96];

void session_set_credentials(const char *user, const char *pass)
{
    snprintf(s_user, sizeof(s_user), "%s", user ? user : "");
    snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
    memset(s_sessions, 0, sizeof(s_sessions));
}

bool session_credentials_set(void)
{
    return s_user[0] != '\0' && s_pass[0] != '\0';
}

// Compares without stopping at the first difference, so the time taken does not tell how much matched.
static bool same_secret(const char *a, const char *b)
{
    size_t a_len = strlen(a), b_len = strlen(b);
    unsigned char diff = a_len != b_len;
    size_t n = a_len < b_len ? a_len : b_len;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool session_credentials_match(const char *user, const char *pass)
{
    // both are compared always, so a wrong name takes as long as a wrong password
    bool user_ok = same_secret(user, s_user);
    bool pass_ok = same_secret(pass, s_pass);
    return session_credentials_set() && user_ok && pass_ok;
}

void session_open(httpd_req_t *req)
{
    session_t *slot = &s_sessions[0];
    for (int i = 1; i < SESSION_SLOTS; i++) {
        if (s_sessions[i].last_used < slot->last_used) {
            slot = &s_sessions[i];
        }
    }
    uint8_t random[SESSION_TOKEN_LEN];
    esp_fill_random(random, sizeof(random));
    for (int i = 0; i < SESSION_TOKEN_LEN; i++) {
        sprintf(slot->token + 2 * i, "%02x", random[i]);
    }
    slot->last_used = ++s_clock;
    snprintf(s_cookie_header, sizeof(s_cookie_header), "sid=%s; Path=/; HttpOnly; SameSite=Strict", slot->token);
    httpd_resp_set_hdr(req, "Set-Cookie", s_cookie_header);
}

static session_t *find(httpd_req_t *req)
{
    char token[SESSION_HEX_LEN + 1];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, "sid", token, &len) != ESP_OK || strlen(token) != SESSION_HEX_LEN) {
        return NULL;
    }
    for (int i = 0; i < SESSION_SLOTS; i++) {
        if (s_sessions[i].token[0] != '\0' && same_secret(token, s_sessions[i].token)) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

void session_close(httpd_req_t *req)
{
    session_t *s = find(req);
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
}

bool session_valid(httpd_req_t *req)
{
    session_t *s = find(req);
    if (s == NULL) {
        return false;
    }
    s->last_used = ++s_clock;
    return true;
}
