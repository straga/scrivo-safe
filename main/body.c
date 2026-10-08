#include "body.h"

#include "esp_log.h"

static const char *TAG = "body";

int body_recv(httpd_req_t *req, char *buf, size_t len)
{
    for (int timeouts = 0; timeouts < BODY_STALL_TIMEOUTS; timeouts++) {
        int got = httpd_req_recv(req, buf, len);
        if (got != HTTPD_SOCK_ERR_TIMEOUT) {
            return got;
        }
    }
    ESP_LOGW(TAG, "no data for %d timeouts in a row on %s: the upload is given up", BODY_STALL_TIMEOUTS, req->uri);
    return BODY_STALLED;
}
