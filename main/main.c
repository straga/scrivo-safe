// scrivo-safe: recovery image that lives in the factory partition.
// Always raises its own open access point with a captive portal; joins the first saved Wi-Fi
// network that answers; answers as <its access point name>.local. Accepts a new main firmware image over
// HTTP, checks it before writing anything, writes it to ota_0 and boots it.

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "dns_server.h"

#include "app_fs.h"
#include "body.h"
#include "files_http.h"
#include "form.h"
#include "host_name.h"
#include "hw_facts.h"
#include "ptable.h"
#include "session.h"
#include "status_led.h"

static const char *TAG = "scrivo-safe";

// Contract with the MicroPython image, which reads and writes the same keys via esp32.NVS.
// MicroPython has no string type in NVS, so values are blobs without a terminating NUL.
// Slot 0 is wifi_ssid / wifi_pass, slot N is wifi_ssidN / wifi_passN; slot 0 is tried first.
// web_user / web_pass are the sign-in of the portal (a form on the page, session.c); without them
// the network form and the boot button stay open, firmware uploads and files are refused.
#define NVS_NAMESPACE "scrivo"
#define WIFI_SLOTS    5

#define SSID_MAX_LEN 32
#define PASS_MAX_LEN 64
#define PASS_MIN_LEN 8     // WPA2 minimum; an empty password means an open network
#define FORM_MAX_LEN 256
#define WEB_USER_MAX_LEN 32
#define WEB_PASS_MAX_LEN 64
// A password set through the one open window, where anyone who reached the board can set it: eight is
// the shortest that does not read as a placeholder. wled1234 from the industry survey is what a lower
// bar looks like (docs/REF_Iot_Industry_Patterns).
#define WEB_PASS_MIN_LEN 8
#define STR_(x) #x
#define STR(x) STR_(x)

#define JOIN_TIMEOUT_MS  40000   // the router has been seen refusing the first attempts for ~30 s
#define RETRY_PAUSE_MS   30000

// The main firmware is told by the project name its image carries in esp_app_desc_t; the name is a Kconfig
// setting (SCRIVO_SAFE_MAIN_PROJECT, "micropython" by default).
#define FIRMWARE_PROJECT CONFIG_SCRIVO_SAFE_MAIN_PROJECT
#define UPLOAD_CHUNK     4096

// Nobody uses the portal this long - the board goes back to the main firmware (stage 3.10, step 6).
// nvs key idle_return_min (i32, minutes) next to the networks; no key or a value outside the range
// means the default. An open page polls /status and so keeps the board here; a minute after it is
// closed the board goes back.
#define IDLE_RETURN_KEY         "idle_return_min"
#define IDLE_RETURN_DEFAULT_MIN 1
#define IDLE_RETURN_MAX_MIN     (24 * 60)

#define AP_URL     "http://192.168.4.1/"   // default address of the IDF access point netif

#define STA_GOT_IP_BIT       BIT0
#define STA_DISCONNECTED_BIT BIT1
#define SCAN_REQUEST_BIT     BIT2   // the page asks for a scan; the join task, the one owner of Wi-Fi, runs it
#define SCAN_DONE_BIT        BIT3

#define SCAN_MAX_NETWORKS 20
#define SCAN_WAIT_MS      15000   // a join attempt in progress is not interrupted; the page is told to retry
#define SCAN_CONNECT_GAP_MS 5000  // a scan right before esp_wifi_connect made the router drop the association

typedef struct {
    char ssid[SSID_MAX_LEN + 1];
    char pass[PASS_MAX_LEN + 1];
} wifi_net_t;

static char s_board_prefix[33];   // "<board>-" taken from our own version
static wifi_net_t s_nets[WIFI_SLOTS];
static int s_net_count;
static EventGroupHandle_t s_sta_events;
static char s_sta_ip[16];
static uint8_t s_last_reason;
static char s_sta_ssid[SSID_MAX_LEN + 1];   // the saved network the station has an address from
// scrivo-safe-<last two bytes of the station MAC>, set by start_network before anything that reads it starts. The
// access point, mdns and the router's client list all show it, with the digits of the MAC the router lists.
static char s_ap_ssid[33];
static esp_timer_handle_t s_idle_timer;
// Guards the idle count between actions on the httpd task and idle_task: the moment the count runs out
// (INT64_MAX while it is stopped for an operation or held by "stay"), and whether the board has committed
// to going back to ota_0.
static SemaphoreHandle_t s_idle_lock;
static int64_t s_idle_deadline = INT64_MAX;
static bool s_leaving;
static int32_t s_idle_return_min = IDLE_RETURN_DEFAULT_MIN;
// "Stay here until I let go" from the page: RAM only, so a reset or a power cut brings back the rule
// above. A board left in the recovery image for good would be found out only on site.
static bool s_stay;

#define PTABLE_PREV_KEY "ptable_prev"   // nvs blob: the table before the last rewrite, for going back

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool open;
} scan_net_t;

static scan_net_t s_scan[SCAN_MAX_NETWORKS];   // written by the join task, read by the HTTP task after SCAN_DONE_BIT
static int s_scan_count;
static esp_err_t s_scan_err;

static void describe_self(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s running from partition %s at 0x%" PRIx32,
             app->project_name, app->version, running->label, running->address);

    // An image for this board has a version that starts with the same "<board>-".
    const char *dash = strchr(app->version, '-');
    size_t len = dash ? (size_t)(dash - app->version + 1) : 0;
    memcpy(s_board_prefix, app->version, len);
    s_board_prefix[len] = '\0';
}

// ---- nvs ----

static void slot_keys(int slot, char *ssid_key, char *pass_key)
{
    if (slot == 0) {
        strcpy(ssid_key, "wifi_ssid");
        strcpy(pass_key, "wifi_pass");
    } else {
        sprintf(ssid_key, "wifi_ssid%d", slot);
        sprintf(pass_key, "wifi_pass%d", slot);
    }
}

// Reads a blob into buf and terminates it; a stored value longer than buf_size - 1 is rejected.
static esp_err_t read_blob_str(nvs_handle_t handle, const char *key, char *buf, size_t buf_size)
{
    size_t len = buf_size - 1;
    esp_err_t err = nvs_get_blob(handle, key, buf, &len);
    if (err != ESP_OK) {
        return err;
    }
    buf[len] = '\0';
    return ESP_OK;
}

// Fills nets from the slots in order, skipping empty or unreadable ones; returns how many.
static int load_networks(wifi_net_t *nets)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no saved networks: namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(err));
        return 0;
    }
    int count = 0;
    for (int slot = 0; slot < WIFI_SLOTS; slot++) {
        char ssid_key[16], pass_key[16];
        slot_keys(slot, ssid_key, pass_key);
        wifi_net_t *net = &nets[count];
        if (read_blob_str(handle, ssid_key, net->ssid, sizeof(net->ssid)) != ESP_OK || net->ssid[0] == '\0') {
            continue;
        }
        err = read_blob_str(handle, pass_key, net->pass, sizeof(net->pass));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "saved network '%s' in slot %d has no usable %s: %s", net->ssid, slot, pass_key,
                     esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "saved network %d: '%s'", count, net->ssid);
        count++;
    }
    nvs_close(handle);
    if (count == 0) {
        ESP_LOGW(TAG, "no saved networks in '%s'", NVS_NAMESPACE);
    }
    return count;
}

static esp_err_t write_networks(const wifi_net_t *nets, int count);

// Puts the network first, drops an older entry with the same name and whatever no longer fits.
static esp_err_t save_network(const char *ssid, const char *pass)
{
    wifi_net_t old[WIFI_SLOTS];
    int old_count = load_networks(old);

    wifi_net_t nets[WIFI_SLOTS];
    int count = 0;
    strlcpy(nets[count].ssid, ssid, sizeof(nets[count].ssid));
    strlcpy(nets[count].pass, pass, sizeof(nets[count].pass));
    count++;
    for (int i = 0; i < old_count && count < WIFI_SLOTS; i++) {
        if (strcmp(old[i].ssid, ssid) != 0) {
            nets[count++] = old[i];
        }
    }
    esp_err_t err = write_networks(nets, count);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved network '%s' first, %d network(s) in the list", ssid, count);
    }
    return err;
}

// Removes the network from the saved list, the rest keeping their order. ESP_ERR_NOT_FOUND when it
// is not saved.
static esp_err_t delete_network(const char *ssid)
{
    wifi_net_t nets[WIFI_SLOTS];
    int old_count = load_networks(nets);
    int count = 0;
    for (int i = 0; i < old_count; i++) {
        if (strcmp(nets[i].ssid, ssid) != 0) {
            nets[count++] = nets[i];
        }
    }
    if (count == old_count) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = write_networks(nets, count);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "deleted network '%s', %d network(s) left", ssid, count);
    }
    return err;
}

// Writes the list into the slots in order and erases the slots after it.
static esp_err_t write_networks(const wifi_net_t *nets, int count)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    for (int slot = 0; slot < WIFI_SLOTS && err == ESP_OK; slot++) {
        char ssid_key[16], pass_key[16];
        slot_keys(slot, ssid_key, pass_key);
        if (slot < count) {
            err = nvs_set_blob(handle, ssid_key, nets[slot].ssid, strlen(nets[slot].ssid));
            if (err == ESP_OK) {
                err = nvs_set_blob(handle, pass_key, nets[slot].pass, strlen(nets[slot].pass));
            }
        } else {
            esp_err_t e1 = nvs_erase_key(handle, ssid_key);
            esp_err_t e2 = nvs_erase_key(handle, pass_key);
            err = (e1 == ESP_OK || e1 == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : e1;
            if (err == ESP_OK && e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) {
                err = e2;
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void load_web_credentials(void)
{
    char user[WEB_USER_MAX_LEN + 1] = "";
    char pass[WEB_PASS_MAX_LEN + 1] = "";
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (read_blob_str(handle, "web_user", user, sizeof(user)) != ESP_OK ||
            read_blob_str(handle, "web_pass", pass, sizeof(pass)) != ESP_OK) {
            user[0] = pass[0] = '\0';
        }
        nvs_close(handle);
    }
    session_set_credentials(user, pass);
    if (session_credentials_set()) {
        ESP_LOGI(TAG, "web credentials from nvs, user '%s'", user);
    } else {
        ESP_LOGW(TAG, "no web credentials: sign-in impossible, firmware uploads and files refused");
    }
}

static void load_idle_return(void)
{
    int32_t minutes = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_i32(handle, IDLE_RETURN_KEY, &minutes);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no %s in nvs: back to ota_0 after %d min without actions", IDLE_RETURN_KEY, IDLE_RETURN_DEFAULT_MIN);
    } else if (minutes < 1 || minutes > IDLE_RETURN_MAX_MIN) {
        ESP_LOGW(TAG, "%s = %" PRId32 " is outside 1..%d: using %d min", IDLE_RETURN_KEY, minutes, IDLE_RETURN_MAX_MIN,
                 IDLE_RETURN_DEFAULT_MIN);
    } else {
        s_idle_return_min = minutes;
        ESP_LOGI(TAG, "%s = %" PRId32 ": back to ota_0 after that without actions", IDLE_RETURN_KEY, minutes);
    }
}

// ---- wifi ----

static void on_network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "client " MACSTR " joined the access point", MAC2STR(event->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "no address from router: network '%.32s' dropped or refused, reason %d",
                 (char *)event->ssid, event->reason);
        s_sta_ip[0] = '\0';
        s_last_reason = event->reason;
        xEventGroupSetBits(s_sta_events, STA_DISCONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "address from router: %s (gateway " IPSTR "), page at http://%s/ and http://%s.local/",
                 s_sta_ip, IP2STR(&event->ip_info.gw), s_sta_ip, s_ap_ssid);
        xEventGroupSetBits(s_sta_events, STA_GOT_IP_BIT);
    }
}

// The driver searches for the network itself on connect and answers with one of these when it is
// not on the air (or not usable), so there is nothing to wait for.
static bool network_absent(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

// Tries one network until it gets an address, turns out absent, or JOIN_TIMEOUT_MS runs out.
// No scan of our own before connecting: a scan right before esp_wifi_connect made the router drop every
// association (checked with control runs on a bench router). The driver's own search inside the
// connect goes over every channel and takes the strongest access point of the network: left at
// zero it is WIFI_FAST_SCAN, which stops at the first one found - 27.09 the far one on channel 13.
static bool try_join(const wifi_net_t *net)
{
    wifi_config_t sta = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi = -127,
        },
    };
    memcpy(sta.sta.ssid, net->ssid, strnlen(net->ssid, sizeof(sta.sta.ssid)));
    memcpy(sta.sta.password, net->pass, strnlen(net->pass, sizeof(sta.sta.password)));
    esp_wifi_set_config(WIFI_IF_STA, &sta);

    ESP_LOGI(TAG, "joining network '%s'", net->ssid);
    TickType_t start = xTaskGetTickCount();
    xEventGroupClearBits(s_sta_events, STA_GOT_IP_BIT | STA_DISCONNECTED_BIT);
    esp_wifi_connect();
    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= pdMS_TO_TICKS(JOIN_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "network '%s': no address in %d s, giving up on it", net->ssid, JOIN_TIMEOUT_MS / 1000);
            esp_wifi_disconnect();
            return false;
        }
        EventBits_t bits = xEventGroupWaitBits(s_sta_events, STA_GOT_IP_BIT | STA_DISCONNECTED_BIT, pdTRUE,
                                               pdFALSE, pdMS_TO_TICKS(JOIN_TIMEOUT_MS) - elapsed);
        if (bits & STA_GOT_IP_BIT) {
            snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", net->ssid);
            ESP_LOGI(TAG, "network '%s' joined after %" PRIu32 " ms", net->ssid,
                     (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - start));
            return true;
        }
        if (bits & STA_DISCONNECTED_BIT) {
            if (network_absent(s_last_reason)) {
                ESP_LOGW(TAG, "network '%s' is not on the air (reason %d), skipped", net->ssid, s_last_reason);
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    }
}

// Runs one blocking scan and keeps the networks on the air, each name once with its strongest
// signal, strongest first. Called only from the join task, between join attempts.
static void run_scan(void)
{
    static wifi_ap_record_t records[SCAN_MAX_NETWORKS];
    wifi_scan_config_t config = {.show_hidden = false};
    uint16_t found = SCAN_MAX_NETWORKS;
    s_scan_count = 0;
    s_scan_err = esp_wifi_scan_start(&config, true);
    if (s_scan_err == ESP_OK) {
        s_scan_err = esp_wifi_scan_get_ap_records(&found, records);
    } else {
        esp_wifi_clear_ap_list();
    }
    for (int i = 0; s_scan_err == ESP_OK && i < found; i++) {
        const char *name = (const char *)records[i].ssid;
        if (name[0] == '\0') {
            continue;
        }
        int at = 0;
        while (at < s_scan_count && strcmp(s_scan[at].ssid, name) != 0) {
            at++;
        }
        if (at == s_scan_count) {
            s_scan_count++;
        } else if (s_scan[at].rssi >= records[i].rssi) {
            continue;
        }
        snprintf(s_scan[at].ssid, sizeof(s_scan[at].ssid), "%s", name);
        s_scan[at].rssi = records[i].rssi;
        s_scan[at].channel = records[i].primary;
        s_scan[at].open = records[i].authmode == WIFI_AUTH_OPEN;
    }
    for (int i = 1; i < s_scan_count; i++) {
        for (int j = i; j > 0 && s_scan[j].rssi > s_scan[j - 1].rssi; j--) {
            scan_net_t t = s_scan[j];
            s_scan[j] = s_scan[j - 1];
            s_scan[j - 1] = t;
        }
    }
    ESP_LOGI(TAG, "scan: %d network(s) on the air (%s)", s_scan_count, esp_err_to_name(s_scan_err));
    xEventGroupSetBits(s_sta_events, SCAN_DONE_BIT);
}

// Waits up to ms for the link to drop (only when joined) or for a scan request, running the scans;
// returns true when the link dropped. A scan leaves at least SCAN_CONNECT_GAP_MS before a join.
static bool wait_serving_scans(uint32_t ms, bool joined)
{
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t left = joined ? portMAX_DELAY : (end > now ? end - now : 0);
        if (!joined && left == 0) {
            return false;
        }
        EventBits_t wanted = SCAN_REQUEST_BIT | (joined ? STA_DISCONNECTED_BIT : 0);
        EventBits_t bits = xEventGroupWaitBits(s_sta_events, wanted, pdFALSE, pdFALSE, left);
        if (joined && (bits & STA_DISCONNECTED_BIT)) {
            xEventGroupClearBits(s_sta_events, STA_DISCONNECTED_BIT);
            return true;
        }
        if (bits & SCAN_REQUEST_BIT) {
            xEventGroupClearBits(s_sta_events, SCAN_REQUEST_BIT);
            run_scan();
            TickType_t gap = xTaskGetTickCount() + pdMS_TO_TICKS(SCAN_CONNECT_GAP_MS);
            if (!joined && end < gap) {
                end = gap;
            }
        }
    }
}

static void join_task(void *arg)
{
    for (;;) {
        bool joined = false;
        for (int i = 0; i < s_net_count && !joined; i++) {
            joined = try_join(&s_nets[i]);
        }
        if (joined) {
            // Stay until the link drops, then go through the list again.
            wait_serving_scans(0, true);
            continue;
        }
        if (s_net_count > 0) {
            ESP_LOGW(TAG, "none of %d saved network(s) joined, access point only; next try in %d s",
                     s_net_count, RETRY_PAUSE_MS / 1000);
        }
        wait_serving_scans(RETRY_PAUSE_MS, false);
    }
}

static void start_network(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    // Keep the driver's own keys out of the nvs partition shared with MicroPython.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_network_event,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_network_event,
                                                        NULL, NULL));

    uint8_t mac[6];
    // The station MAC: the address the router lists for the board. The access point has its own, one higher.
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    wifi_config_t ap = {
        .ap = {
            .max_connection = 4,
            // Open on purpose: a password would have to be baked into the build. The page itself
            // is guarded by web_user / web_pass from nvs.
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    board_name(s_ap_ssid, sizeof(s_ap_ssid), mac);
    ap.ap.ssid_len = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
    // The router lists a client by the hostname it asks its address with; left alone, every board reads "espressif".
    esp_err_t name_err = esp_netif_set_hostname(sta_netif, s_ap_ssid);
    if (name_err != ESP_OK) {
        ESP_LOGW(TAG, "hostname for the router not set: %s", esp_err_to_name(name_err));
    }

    // The station half is always present so the board can join a saved network while the access
    // point stays up.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    // DHCP option 114 tells phones the portal address directly (examples/protocols/http_server/captive_portal).
    esp_netif_dhcps_stop(ap_netif);
    esp_err_t err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           (void *)AP_URL, strlen(AP_URL));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcp captive portal option not set: %s", esp_err_to_name(err));
    }
    esp_netif_dhcps_start(ap_netif);

    esp_netif_ip_info_t ip;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip));
    ESP_LOGI(TAG, "access point '%s' open, page at http://" IPSTR "/", ap.ap.ssid, IP2STR(&ip.ip));
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        err = mdns_hostname_set(s_ap_ssid);
    }
    if (err == ESP_OK) {
        err = mdns_instance_name_set("scrivo-safe recovery image");
    }
    if (err == ESP_OK) {
        err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns not started: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "mdns name %s.local", s_ap_ssid);
}

// ---- http ----

static void restart_task(void *arg)
{
    // Give the HTTP reply time to leave before the reset.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void restart_soon(void)
{
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
}

static bool activity(void);

// The board has committed to going back to ota_0: nothing new starts, so the restart cuts nothing off.
static bool refuse_leaving(httpd_req_t *req)
{
    ESP_LOGW(TAG, "refused %s: going back to ota_0", req->uri);
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "The board is going back to the main firmware; nothing was started.\n");
    return false;
}

// The state is open to everyone; actions need a session from the form on the page (session.c).
// Until web credentials exist in nvs the network form and the boot button stay open, so a fresh
// board can be set up through the access point without a cable. No response ever carries
// WWW-Authenticate: a browser pop-up is exactly what the phone's captive mini-browser lacks.
static bool action_allowed(httpd_req_t *req)
{
    if (!session_credentials_set() || session_valid(req)) {
        return activity() || refuse_leaving(req);
    }
    ESP_LOGW(TAG, "no session for %s %s", req->method == HTTP_POST ? "POST" : "GET", req->uri);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Sign in on the page first.\n");
    return false;
}

// Firmware uploads and files need credentials to exist at all; sends 403 and returns false otherwise.
static bool credentials_required(httpd_req_t *req)
{
    if (session_credentials_set()) {
        return true;
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "refused until web_user and web_pass are set in nvs");
    return false;
}

// Runs a long operation - a firmware, a file, an archive, a partition table - with the idle count
// stopped, and starts the count again when it ends, refused or not. Restarting the count once at the
// start is not enough: an operation longer than the return time would still be cut off, and the
// return time is the person's own setting, down to one minute.
static esp_err_t while_count_stopped(httpd_req_t *req, esp_err_t (*operation)(httpd_req_t *req))
{
    if (!credentials_required(req) || !action_allowed(req)) {
        return ESP_OK;
    }
    xSemaphoreTake(s_idle_lock, portMAX_DELAY);
    bool leaving = s_leaving;
    if (!leaving && s_idle_timer != NULL) {
        esp_timer_stop(s_idle_timer);
        s_idle_deadline = INT64_MAX;
    }
    xSemaphoreGive(s_idle_lock);
    if (leaving) {
        refuse_leaving(req);
        return ESP_OK;
    }
    esp_err_t result = operation(req);
    activity();
    return result;
}

// web/index.html, inlined, checked and compressed at build time by tools/build_web.py and embedded
// by target_add_binary_data in CMakeLists.txt. The length comes from the linker symbols, never by hand.
extern const uint8_t web_page_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t web_page_gz_end[] asm("_binary_index_html_gz_end");

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // the page changes only with the firmware, but a phone must not keep the one from before an update
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)web_page_gz_start, web_page_gz_end - web_page_gz_start);
}

// Appends s to buf at *len as the inside of a JSON string; returns false when it does not fit.
static bool json_append_escaped(char *buf, size_t size, size_t *len, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char piece[7];
        size_t n;
        if (c == '"' || c == '\\') {
            piece[0] = '\\';
            piece[1] = (char)c;
            n = 2;
        } else if (c < 0x20) {
            n = (size_t)snprintf(piece, sizeof(piece), "\\u%04x", c);
        } else {
            piece[0] = (char)c;
            n = 1;
        }
        if (*len + n >= size) {
            return false;
        }
        memcpy(buf + *len, piece, n);
        *len += n;
    }
    buf[*len] = '\0';
    return true;
}

static bool json_append_raw(char *buf, size_t size, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (*len + n >= size) {
        return false;
    }
    memcpy(buf + *len, s, n + 1);
    *len += n;
    return true;
}

// What changes on the board, for the static page, open to everyone: which image runs, its version,
// the network and address, the access point, the saved networks, the size of ota_0, whether this
// request is signed in, and how long until the board goes back to the main firmware by itself.
// The longest /status reply, counted rather than hoped for: every string escaped at its worst, six bytes for a
// byte (\u00XX) - the running partition label, the version, the joined network, the address, the access point
// name, a quoted name per saved network - then the tail of numbers and the hardware piece.
#define JSON_ESCAPED_MAX(bytes) (6 * (bytes))
#define STATUS_TAIL_SIZE 256
#define STATUS_BODY_SIZE 2560
#define STATUS_BODY_MAX                                                                                          \
    (sizeof("{\"running\":\"\",\"version\":\"\",\"network\":\"\",\"ip\":\"\",\"ap\":\"\",\"networks\":[") - 1 + \
     JSON_ESCAPED_MAX(16 + 31 + SSID_MAX_LEN + 15 + 32) +                                                         \
     WIFI_SLOTS * (sizeof(",\"\"") - 1 + JSON_ESCAPED_MAX(SSID_MAX_LEN)) + STATUS_TAIL_SIZE - 1 + HW_FACTS_JSON_MAX + \
     sizeof("}"))
_Static_assert(STATUS_BODY_MAX <= STATUS_BODY_SIZE, "the longest /status reply does not fit its buffer");

static esp_err_t status_get(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    uint64_t expiry = 0;
    long idle_left = -1;
    // -1 while "stay here" holds the board. A stopped one-shot timer still reports its old expiry
    // (seen as -45 s), so the count is read only from a running timer.
    if (!s_stay && s_idle_timer != NULL && esp_timer_is_active(s_idle_timer) &&
        esp_timer_get_expiry_time(s_idle_timer, &expiry) == ESP_OK) {
        idle_left = (long)(((int64_t)expiry - esp_timer_get_time()) / 1000000);
    }
    // Reading the state does not restart the count. The page polls it every 5 s while open, so a
    // forgotten tab in someone's browser would hold the board here for good (10.6 h seen with a 5 min
    // return). Actions restart it through action_allowed(); a person reading the page holds it with "stay".
    bool joined = s_sta_ip[0] != '\0';
    wifi_ap_record_t ap_info;
    char rssi[8] = "null";
    // which access point of the network: two of them share one name
    char bssid[24] = "null";
    char channel[8] = "null";
    if (joined && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snprintf(rssi, sizeof(rssi), "%d", ap_info.rssi);
        snprintf(bssid, sizeof(bssid), "\"%02x:%02x:%02x:%02x:%02x:%02x\"", MAC2STR(ap_info.bssid));
        snprintf(channel, sizeof(channel), "%u", ap_info.primary);
    }
    char body[STATUS_BODY_SIZE];
    size_t len = 0;
    bool fits = json_append_raw(body, sizeof(body), &len, "{\"running\":\"") &&
                json_append_escaped(body, sizeof(body), &len, running->label) &&
                json_append_raw(body, sizeof(body), &len, "\",\"version\":\"") &&
                json_append_escaped(body, sizeof(body), &len, esp_app_get_description()->version) &&
                json_append_raw(body, sizeof(body), &len, "\",\"network\":\"") &&
                json_append_escaped(body, sizeof(body), &len, joined ? s_sta_ssid : "") &&
                json_append_raw(body, sizeof(body), &len, "\",\"ip\":\"") &&
                json_append_escaped(body, sizeof(body), &len, s_sta_ip) &&
                json_append_raw(body, sizeof(body), &len, "\",\"ap\":\"") &&
                json_append_escaped(body, sizeof(body), &len, s_ap_ssid) &&
                json_append_raw(body, sizeof(body), &len, "\",\"networks\":[");
    for (int i = 0; fits && i < s_net_count; i++) {
        fits = json_append_raw(body, sizeof(body), &len, i > 0 ? ",\"" : "\"") &&
               json_append_escaped(body, sizeof(body), &len, s_nets[i].ssid) &&
               json_append_raw(body, sizeof(body), &len, "\"");
    }
    char tail[STATUS_TAIL_SIZE];
    snprintf(tail, sizeof(tail),
             "],\"ota0_size\":%u,\"signed_in\":%s,\"credentials_set\":%s,\"idle_left_s\":%ld,"
             "\"idle_return_min\":%d,\"stay\":%s,\"leaving\":%s,\"rssi\":%s,\"bssid\":%s,\"channel\":%s",
             ota0 ? (unsigned)ota0->size : 0, session_valid(req) ? "true" : "false",
             session_credentials_set() ? "true" : "false", idle_left, (int)s_idle_return_min,
             s_stay ? "true" : "false", s_leaving ? "true" : "false", rssi, bssid, channel);
    hw_facts_t hw;
    hw_facts_read(&hw);
    fits = fits && json_append_raw(body, sizeof(body), &len, tail) && hw_facts_json(body, sizeof(body), &len, &hw) &&
           json_append_raw(body, sizeof(body), &len, "}");
    if (!fits) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status does not fit the reply buffer");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, (ssize_t)len);
}

// GET /scan: {"networks": [{"ssid", "rssi", "channel", "open"}, ...]}, strongest first. The join task
// runs the scan; while it is in a join attempt the answer is 503 and the page asks to retry.
static esp_err_t scan_get(httpd_req_t *req)
{
    if (!action_allowed(req)) {
        return ESP_OK;
    }
    xEventGroupClearBits(s_sta_events, SCAN_DONE_BIT);
    xEventGroupSetBits(s_sta_events, SCAN_REQUEST_BIT);
    EventBits_t bits = xEventGroupWaitBits(s_sta_events, SCAN_DONE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(SCAN_WAIT_MS));
    if (!(bits & SCAN_DONE_BIT)) {
        xEventGroupClearBits(s_sta_events, SCAN_REQUEST_BIT);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "The board is joining a network; scan again in a few seconds.\n");
    }
    if (s_scan_err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(s_scan_err));
    }
    char body[2048];
    size_t len = 0;
    bool fits = json_append_raw(body, sizeof(body), &len, "{\"networks\":[");
    for (int i = 0; fits && i < s_scan_count; i++) {
        char tail[64];
        snprintf(tail, sizeof(tail), "\",\"rssi\":%d,\"channel\":%u,\"open\":%s}", s_scan[i].rssi,
                 s_scan[i].channel, s_scan[i].open ? "true" : "false");
        fits = json_append_raw(body, sizeof(body), &len, i > 0 ? ",{\"ssid\":\"" : "{\"ssid\":\"") &&
               json_append_escaped(body, sizeof(body), &len, s_scan[i].ssid) &&
               json_append_raw(body, sizeof(body), &len, tail);
    }
    fits = fits && json_append_raw(body, sizeof(body), &len, "]}");
    if (!fits) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan list too long");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, len);
}

// Reads a whole application/x-www-form-urlencoded body of at most FORM_MAX_LEN bytes; false when
// it is empty, too long or cut off.
static bool recv_form(httpd_req_t *req, char *body)
{
    if (req->content_len == 0 || req->content_len > FORM_MAX_LEN) {
        return false;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int ret = body_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            return false;
        }
        received += ret;
    }
    body[received] = '\0';
    return true;
}

// POST /login from the form on the page: user, password. Answers with a redirect to the page, so the
// plain form works in any browser, the phone's captive mini-browser included. The password is never
// logged.
static esp_err_t login_post(httpd_req_t *req)
{
    char body[FORM_MAX_LEN + 1];
    char user[3 * WEB_USER_MAX_LEN + 1] = "";
    char pass[3 * WEB_PASS_MAX_LEN + 1] = "";
    bool ok = recv_form(req, body) &&
              httpd_query_key_value(body, "user", user, sizeof(user)) == ESP_OK && form_decode(user) &&
              httpd_query_key_value(body, "password", pass, sizeof(pass)) == ESP_OK && form_decode(pass) &&
              session_credentials_match(user, pass);
    memset(pass, 0, sizeof(pass));
    memset(body, 0, sizeof(body));
    if (ok) {
        activity();
        session_open(req);
        ESP_LOGI(TAG, "signed in");
    } else {
        ESP_LOGW(TAG, "sign-in refused");
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", ok ? "/" : "/?login=failed");
    return httpd_resp_sendstr(req, ok ? "Signed in.\n" : "Wrong name or password.\n");
}

// POST /credentials from the form a board shows while it has none: user, password, password2.
// Open ONLY while nvs holds no credentials - the one window in the life of a board, from the first
// flash of this image to the first setup. Once they are set the answer is a refusal, for a signed-in
// browser as well: changing a known password is another thing entirely and wants the old one asked
// for, which this route does not do.
//
// A second request cannot slip in while the first is writing: the server runs handlers one after
// another (HTTPD_DEFAULT_CONFIG, one thread), and ESP-IDF ships a separate async_handlers example
// precisely because parallel handling has to be built with a queue and worker tasks. There are none
// here, so the next POST starts after nvs_commit of this one.
static esp_err_t credentials_post(httpd_req_t *req)
{
    if (session_credentials_set()) {
        ESP_LOGW(TAG, "sign-in setup refused: credentials already exist");
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                            "sign-in is already set up; changing it is not done here");
        return ESP_OK;
    }

    char body[FORM_MAX_LEN + 1];
    char user[3 * WEB_USER_MAX_LEN + 1] = "";
    char pass[3 * WEB_PASS_MAX_LEN + 1] = "";
    char again[3 * WEB_PASS_MAX_LEN + 1] = "";
    const char *refusal = NULL;
    if (!recv_form(req, body) ||
        httpd_query_key_value(body, "user", user, sizeof(user)) != ESP_OK || !form_decode(user) ||
        httpd_query_key_value(body, "password", pass, sizeof(pass)) != ESP_OK || !form_decode(pass) ||
        httpd_query_key_value(body, "password2", again, sizeof(again)) != ESP_OK || !form_decode(again)) {
        refusal = "name, password and password2 are all needed";
    } else if (user[0] == '\0') {
        refusal = "the name cannot be empty";
    } else if (strlen(user) > WEB_USER_MAX_LEN) {
        refusal = "the name is too long";
    } else if (strlen(pass) < WEB_PASS_MIN_LEN) {
        refusal = "the password is shorter than " STR(WEB_PASS_MIN_LEN) " characters";
    } else if (strlen(pass) > WEB_PASS_MAX_LEN) {
        refusal = "the password is too long";
    } else if (strcmp(pass, again) != 0) {
        refusal = "the two passwords are not the same";
    }

    esp_err_t err = ESP_OK;
    if (refusal == NULL) {
        nvs_handle_t handle;
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, "web_user", user, strlen(user));
            if (err == ESP_OK) {
                err = nvs_set_blob(handle, "web_pass", pass, strlen(pass));
            }
            if (err == ESP_OK) {
                err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
        if (err == ESP_OK) {
            // takes effect at once and drops every open session: whoever set it signs in with it now
            session_set_credentials(user, pass);
            ESP_LOGI(TAG, "sign-in set up, user '%s'", user);
        } else {
            ESP_LOGE(TAG, "sign-in not written to nvs: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "sign-in setup refused: %s", refusal);
    }
    memset(pass, 0, sizeof(pass));
    memset(again, 0, sizeof(again));
    memset(body, 0, sizeof(body));

    if (refusal != NULL) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/?setup=failed");
        return httpd_resp_sendstr(req, refusal);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sign-in not written to nvs");
        return ESP_OK;
    }
    activity();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?setup=done");
    return httpd_resp_sendstr(req, "Sign-in is set up. Sign in with it now.\n");
}

// POST /logout: ends the session of this browser and goes back to the page.
static esp_err_t logout_post(httpd_req_t *req)
{
    session_close(req);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, "Signed out.\n");
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!action_allowed(req)) {
        return ESP_OK;
    }
    char body[FORM_MAX_LEN + 1];
    if (!recv_form(req, body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form is empty, too long or cut off");
    }

    // Encoded values can be up to three times longer than decoded ones.
    char ssid[3 * SSID_MAX_LEN + 1] = "";
    char pass[3 * PASS_MAX_LEN + 1] = "";
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || !form_decode(ssid)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "network name missing or malformed");
    }
    esp_err_t pass_err = httpd_query_key_value(body, "pass", pass, sizeof(pass));
    if ((pass_err != ESP_OK && pass_err != ESP_ERR_NOT_FOUND) || !form_decode(pass)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password malformed");
    }
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len > SSID_MAX_LEN) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "network name must be 1 to 32 bytes");
    }
    if (pass_len > PASS_MAX_LEN || (pass_len > 0 && pass_len < PASS_MIN_LEN)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password must be empty or 8 to 64 bytes");
    }

    esp_err_t err = save_network(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving network failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    httpd_resp_sendstr(req, "Saved. The board restarts and joins the network.\n");
    restart_soon();
    return ESP_OK;
}

// Reads the header and segment headers of the image stored in a partition and checks that every
// segment lies inside the partition. esp_ota_set_boot_partition verifies the image with
// esp_image_verify, which bounds a segment length only by ESP_IMAGE_MAX_FLASH_ADDR_SIZE: an ota_0
// whose new headers lead into the tail of an older image (an upload cut off before its abort ran)
// hung the board on the boot button. Returns NULL when the bounds hold, otherwise the reason.
// Also gives, through image_len when not NULL, where the image ends: segments, the checksum padded to
// 16 bytes and the appended SHA-256.
static const char *stored_image_span(const esp_partition_t *part, size_t *image_len)
{
    esp_image_header_t image;
    if (image_len != NULL) {
        *image_len = 0;
    }
    if (esp_partition_read(part, 0, &image, sizeof(image)) != ESP_OK) {
        return "cannot read ota_0";
    }
    if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.segment_count == 0 ||
        image.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
        return "ota_0 holds no valid image";
    }
    size_t offset = sizeof(image);
    for (int i = 0; i < image.segment_count; i++) {
        esp_image_segment_header_t segment;
        if (offset + sizeof(segment) > part->size ||
            esp_partition_read(part, offset, &segment, sizeof(segment)) != ESP_OK) {
            return "ota_0 holds an incomplete image";
        }
        offset += sizeof(segment) + segment.data_len;
        if ((segment.data_len & 3) != 0 || segment.data_len > part->size || offset > part->size) {
            return "ota_0 holds an incomplete image";
        }
    }
    size_t end = (offset | 15) + 1 + (image.hash_appended ? 32 : 0);
    if (image_len != NULL && end <= part->size) {
        *image_len = end;
    }
    return NULL;
}

static const char *check_stored_image_bounds(const esp_partition_t *part)
{
    return stored_image_span(part, NULL);
}

// Switches boot to ota_0 when it holds an image that fits the partition and passes verification;
// returns NULL, or why not. The image boots unconfirmed, so one that fails comes back to factory.
static const char *boot_ota0(void)
{
    const esp_partition_t *app = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                          ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (app == NULL) {
        return "no ota_0 partition";
    }
    const char *bad = check_stored_image_bounds(app);
    if (bad != NULL) {
        return bad;
    }
    // Validates the image in ota_0 first and refuses an empty or broken one.
    esp_err_t err = esp_ota_set_boot_partition(app);
    return err == ESP_OK ? NULL : esp_err_to_name(err);
}

// The board leaves the recovery image: the LED goes dark so the change is visible, then the restart.
static void leave_recovery(void)
{
    status_led_show(STATUS_LED_LEAVING);
    restart_soon();
}

static esp_err_t boot_app_post(httpd_req_t *req)
{
    if (!action_allowed(req)) {
        return ESP_OK;
    }
    const char *bad = boot_ota0();
    if (bad != NULL) {
        ESP_LOGW(TAG, "boot ota_0 refused: %s", bad);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, bad);
    }
    ESP_LOGI(TAG, "boot partition set to ota_0, restarting");
    httpd_resp_sendstr(req, "Boot partition set to ota_0, restarting.\n");
    leave_recovery();
    return ESP_OK;
}

// POST /restart: the board restarts and comes back in this image. The boot partition is not touched: this image
// moves it only right before a restart of its own - up to ota_0 (boot_ota0, update_receive) or down to factory
// (apply_table) - so the bootloader brings the board back where it runs now.
static esp_err_t restart_post(httpd_req_t *req)
{
    if (!action_allowed(req)) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "restart asked from the page, boot partition untouched");
    httpd_resp_sendstr(req, "Restarting in the recovery image. This page comes back by itself; sign in again when it asks.\n");
    restart_soon();
    return ESP_OK;
}

// ---- back to the main firmware when nobody uses the portal ----

// The timer firing only says the time ran out then. Before this task runs, an action may start the count
// again or stop it for an operation, and the board must not restart under that action. The deadline, read
// under the lock every action takes, says whether the return is still due; once it is, the board commits
// to going back first, and actions that come after are refused in words rather than cut off.
static void idle_task(void *arg)
{
    xSemaphoreTake(s_idle_lock, portMAX_DELAY);
    bool due = !s_leaving && esp_timer_get_time() >= s_idle_deadline;
    if (due) {
        s_leaving = true;
    }
    xSemaphoreGive(s_idle_lock);
    if (!due) {
        ESP_LOGI(TAG, "the return time ran out as an action came in: staying");
    } else {
        const char *bad = boot_ota0();
        if (bad == NULL) {
            ESP_LOGI(TAG, "no action for %d min, going back to ota_0", (int)s_idle_return_min);
            leave_recovery();
        } else {
            // nothing to go back to: stay here and look again after the same time
            ESP_LOGW(TAG, "no action for %d min, but ota_0 is not bootable (%s): staying", (int)s_idle_return_min, bad);
            xSemaphoreTake(s_idle_lock, portMAX_DELAY);
            s_leaving = false;
            xSemaphoreGive(s_idle_lock);
            activity();
        }
    }
    vTaskDelete(NULL);
}

// Runs on the esp_timer task, which must not wait on flash: the switch happens on a task of its own.
static void idle_expired(void *arg)
{
    xTaskCreate(idle_task, "idle-return", 4096, NULL, 5, NULL);
}

// Signing in and every allowed action start the idle count again. Refused requests and polls of
// /status do not. False when the board has already committed to going back: the action is refused.
static bool activity(void)
{
    xSemaphoreTake(s_idle_lock, portMAX_DELAY);
    bool leaving = s_leaving;
    if (!leaving && s_idle_timer != NULL) {
        esp_timer_stop(s_idle_timer);
        s_idle_deadline = INT64_MAX;
        if (!s_stay) {
            uint64_t us = (uint64_t)s_idle_return_min * 60 * 1000000;
            s_idle_deadline = esp_timer_get_time() + (int64_t)us;
            esp_timer_start_once(s_idle_timer, us);
        }
    }
    xSemaphoreGive(s_idle_lock);
    return !leaving;
}

static esp_err_t files_list_route(httpd_req_t *req)
{
    return credentials_required(req) && action_allowed(req) ? files_list_get(req) : ESP_OK;
}

static esp_err_t file_get_route(httpd_req_t *req)
{
    return credentials_required(req) && action_allowed(req) ? file_get(req) : ESP_OK;
}

static esp_err_t file_post_route(httpd_req_t *req)
{
    return while_count_stopped(req, file_post);
}

static esp_err_t file_delete_route(httpd_req_t *req)
{
    return credentials_required(req) && action_allowed(req) ? file_delete_post(req) : ESP_OK;
}

static esp_err_t files_archive_route(httpd_req_t *req)
{
    return credentials_required(req) && action_allowed(req) ? files_archive_get(req) : ESP_OK;
}

static esp_err_t files_unpack_check_route(httpd_req_t *req)
{
    return credentials_required(req) && action_allowed(req) ? files_unpack_check_post(req) : ESP_OK;
}

static esp_err_t files_unpack_route(httpd_req_t *req)
{
    return while_count_stopped(req, files_unpack_post);
}

// Reads exactly len bytes of the request body; false when the sender closed, failed or went silent for
// the stall limit (body.h).
static bool recv_exact(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int ret = body_recv(req, buf + got, len - got);
        if (ret <= 0) {
            return false;
        }
        got += ret;
    }
    return true;
}

// Checks the start of an uploaded image before anything is written: ESP image magic, segment count,
// target chip, application description magic, project name and the board prefix of the version.
// Returns NULL when the image is acceptable, otherwise the reason.
static const char *check_image_head(const char *head, char *version, size_t version_size)
{
    const esp_image_header_t *image = (const esp_image_header_t *)head;
    const esp_app_desc_t *desc =
        (const esp_app_desc_t *)(head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));

    if (image->magic != ESP_IMAGE_HEADER_MAGIC) {
        return "not an ESP firmware image (bad image magic)";
    }
    if (image->segment_count == 0 || image->segment_count > ESP_IMAGE_MAX_SEGMENTS) {
        return "image has an invalid segment count";
    }
    if (image->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return "image is built for another chip";
    }
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "image has no application description";
    }
    strlcpy(version, desc->version, version_size);
    if (strncmp(desc->project_name, FIRMWARE_PROJECT, sizeof(desc->project_name)) != 0) {
        return "image is not the main firmware (project name is not " FIRMWARE_PROJECT ")";
    }
    if (s_board_prefix[0] == '\0' || strncmp(desc->version, s_board_prefix, strlen(s_board_prefix)) != 0) {
        return "image is built for another board (version prefix does not match)";
    }
    return NULL;
}

// Follows the segment headers of an image while it arrives, so that esp_ota_end only ever verifies
// an image whose segments all lie inside the uploaded bytes. IDF bounds a segment length only by
// ESP_IMAGE_MAX_FLASH_ADDR_SIZE (UINT32_MAX in an app, esp_image_format.c:53): a truncated upload
// once left a 1.3 GB length from the old tail of ota_0, and verifying it hung the board for minutes.
typedef struct {
    size_t total;                                          // bytes the client announced
    uint8_t segments;                                      // from esp_image_header_t
    bool hash_appended;
    uint8_t seen;                                          // segment headers read so far
    size_t next_header;                                    // upload offset of the next segment header
    uint8_t header[sizeof(esp_image_segment_header_t)];    // a header may straddle two chunks
    size_t header_have;
} image_walk_t;

static void walk_start(image_walk_t *walk, const esp_image_header_t *image, size_t total)
{
    *walk = (image_walk_t){
        .total = total,
        .segments = image->segment_count,
        .hash_appended = image->hash_appended == 1,
        .next_header = sizeof(esp_image_header_t),
    };
}

// Takes the bytes that sit at upload offset `offset`, in upload order. Returns the reason when a
// segment runs past the uploaded data, otherwise NULL.
static const char *walk_feed(image_walk_t *walk, size_t offset, const char *data, size_t len)
{
    while (walk->seen < walk->segments) {
        size_t wanted = walk->next_header + walk->header_have;
        if (wanted >= offset + len) {
            break;
        }
        size_t take = sizeof(walk->header) - walk->header_have;
        if (take > offset + len - wanted) {
            take = offset + len - wanted;
        }
        memcpy(walk->header + walk->header_have, data + (wanted - offset), take);
        walk->header_have += take;
        if (walk->header_have < sizeof(walk->header)) {
            break;
        }
        const esp_image_segment_header_t *segment = (const esp_image_segment_header_t *)walk->header;
        size_t end = walk->next_header + sizeof(walk->header) + segment->data_len;
        if ((segment->data_len & 3) != 0 || segment->data_len > walk->total || end > walk->total) {
            return "image is truncated or corrupt: a segment runs past the uploaded data";
        }
        walk->next_header = end;
        walk->seen++;
        walk->header_have = 0;
    }
    return NULL;
}

// After the last byte: all segment headers arrived, and the checksum byte (padded to a 16-byte
// boundary) and the appended SHA-256 fit in the upload, the way esp_image_format.c computes the
// image length.
static const char *walk_finish(const image_walk_t *walk)
{
    if (walk->seen < walk->segments) {
        return "image is truncated: not all segments arrived";
    }
    size_t end = ((walk->next_header + 1 + 15) & ~(size_t)15) + (walk->hash_appended ? ESP_IMAGE_HASH_LEN : 0);
    if (end > walk->total) {
        return "image is truncated: checksum or hash missing";
    }
    return NULL;
}

// Receives a main firmware image as the raw request body and writes it to ota_0 as it arrives.
// Order after ElegantOTA: everything that can be refused is refused before the partition is
// opened; then write chunk by chunk while the segment walk checks that the image fits; esp_ota_end
// verifies the whole image (its appended SHA-256) before the boot partition is switched.
static esp_err_t update_receive(httpd_req_t *req)
{
    const esp_partition_t *target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                             ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (target == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota_0 partition");
    }
    const size_t head_len = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    size_t total = req->content_len;
    if (total < head_len) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file is too short to be a firmware image");
    }
    if (total > target->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image is larger than ota_0");
    }

    char *buf = malloc(UPLOAD_CHUNK);
    if (buf == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    esp_ota_handle_t ota = 0;
    bool ota_open = false;
    const char *refusal = NULL;
    httpd_err_code_t refusal_code = HTTPD_400_BAD_REQUEST;
    char version[sizeof(((esp_app_desc_t *)0)->version) + 1] = "";
    image_walk_t walk;

    if (!recv_exact(req, buf, head_len)) {
        refusal = "upload interrupted or silent for 30 s";
        goto done;
    }
    refusal = check_image_head(buf, version, sizeof(version));
    if (refusal == NULL) {
        walk_start(&walk, (const esp_image_header_t *)buf, total);
        refusal = walk_feed(&walk, 0, buf, head_len);
    }
    if (refusal != NULL) {
        ESP_LOGW(TAG, "upload refused before writing: %s", refusal);
        goto done;
    }

    ESP_LOGI(TAG, "upload of %u bytes, version '%s', writing to %s", (unsigned)total, version, target->label);
    esp_err_t err = esp_ota_begin(target, total, &ota);
    if (err != ESP_OK) {
        refusal = esp_err_to_name(err);
        refusal_code = HTTPD_500_INTERNAL_SERVER_ERROR;
        goto done;
    }
    ota_open = true;

    size_t written = head_len;
    err = esp_ota_write(ota, buf, head_len);
    while (err == ESP_OK && written < total) {
        size_t want = total - written < UPLOAD_CHUNK ? total - written : UPLOAD_CHUNK;
        if (!recv_exact(req, buf, want)) {
            refusal = "upload interrupted or silent for 30 s; ota_0 is incomplete and the board keeps booting factory";
            goto done;
        }
        refusal = walk_feed(&walk, written, buf, want);
        if (refusal != NULL) {
            ESP_LOGW(TAG, "upload refused at byte %u: %s", (unsigned)written, refusal);
            goto done;
        }
        err = esp_ota_write(ota, buf, want);
        written += want;
    }
    if (err != ESP_OK) {
        refusal = esp_err_to_name(err);
        refusal_code = HTTPD_500_INTERNAL_SERVER_ERROR;
        goto done;
    }
    refusal = walk_finish(&walk);
    if (refusal != NULL) {
        ESP_LOGW(TAG, "upload refused after the last byte: %s", refusal);
        goto done;
    }

    ota_open = false;
    err = esp_ota_end(ota);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        refusal = "image failed verification; ota_0 is overwritten and the board keeps booting factory";
        goto done;
    }
    if (err != ESP_OK) {
        refusal = esp_err_to_name(err);
        refusal_code = HTTPD_500_INTERNAL_SERVER_ERROR;
        goto done;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        refusal = esp_err_to_name(err);
        refusal_code = HTTPD_500_INTERNAL_SERVER_ERROR;
        goto done;
    }

    ESP_LOGI(TAG, "image '%s' written to %s (%u bytes), booting it", version, target->label, (unsigned)written);
    char reply[128];
    snprintf(reply, sizeof(reply), "Written %u bytes of %s to ota_0. Restarting into it.\n", (unsigned)written, version);
    httpd_resp_sendstr(req, reply);
    free(buf);
    leave_recovery();
    return ESP_OK;

done:
    if (ota_open) {
        esp_ota_abort(ota);
    }
    free(buf);
    ESP_LOGW(TAG, "upload failed: %s", refusal);
    return httpd_resp_send_err(req, refusal_code, refusal);
}

// POST /update. An idle return that fires during the upload would not cut it off - boot_ota0 refuses
// the half-written ota_0 and the board stays - but it would read and hash up to the whole partition
// while the upload writes into it, once every return time. The count is stopped instead.
static esp_err_t update_post(httpd_req_t *req)
{
    return while_count_stopped(req, update_receive);
}

// Unknown paths lead to the page, which is what makes phones open the portal on their own.
// A request addressed to the board keeps its host; any other host (a phone's connectivity
// check resolved by our DNS) is sent to the access point address, as esp-at does.
static esp_err_t redirect_to_page(httpd_req_t *req, httpd_err_code_t err)
{
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    bool ours = host_names_board(host, s_ap_ssid, s_sta_ip) || strcmp(host, "192.168.4.1") == 0;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", ours ? "/" : AP_URL);
    // iOS needs a body to recognise the portal.
    return httpd_resp_sendstr(req, "Redirect to the scrivo-safe page");
}

// ---- settings of the return, saved networks ----

// POST /settings: idle_return_min=N (1..1440, kept in nvs) and/or stay=1|0 (RAM only, see s_stay).
static esp_err_t settings_post(httpd_req_t *req)
{
    if (!action_allowed(req)) {
        return ESP_OK;
    }
    char body[FORM_MAX_LEN + 1];
    if (!recv_form(req, body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form is empty, too long or cut off");
    }
    char value[16];
    if (httpd_query_key_value(body, "idle_return_min", value, sizeof(value)) == ESP_OK) {
        char *end = NULL;
        long minutes = strtol(value, &end, 10);
        if (value[0] == '\0' || *end != '\0' || minutes < 1 || minutes > IDLE_RETURN_MAX_MIN) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "the return time must be 1 to 1440 minutes");
        }
        nvs_handle_t handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_i32(handle, IDLE_RETURN_KEY, (int32_t)minutes);
            if (err == ESP_OK) {
                err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        }
        s_idle_return_min = (int32_t)minutes;
        ESP_LOGI(TAG, "%s = %ld from the page", IDLE_RETURN_KEY, minutes);
    }
    if (httpd_query_key_value(body, "stay", value, sizeof(value)) == ESP_OK) {
        if (strcmp(value, "1") != 0 && strcmp(value, "0") != 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "stay must be 1 or 0");
        }
        s_stay = value[0] == '1';
        ESP_LOGI(TAG, "%s", s_stay ? "stay here: no return to ota_0 until the next restart"
                                   : "let go: back to ota_0 after the return time without actions");
    }
    activity();
    char reply[64];
    snprintf(reply, sizeof(reply), "{\"idle_return_min\":%d,\"stay\":%s}", (int)s_idle_return_min, s_stay ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, reply);
}

// POST /wifi/delete: ssid=NAME. The board restarts, as after saving a network.
static esp_err_t wifi_delete_post(httpd_req_t *req)
{
    if (!action_allowed(req)) {
        return ESP_OK;
    }
    char body[FORM_MAX_LEN + 1];
    char ssid[3 * SSID_MAX_LEN + 1] = "";
    if (!recv_form(req, body) || httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || !form_decode(ssid)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "network name missing or malformed");
    }
    esp_err_t err = delete_network(ssid);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such saved network");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    httpd_resp_sendstr(req, "Deleted. The board restarts.\n");
    restart_soon();
    return ESP_OK;
}

// ---- partition table ----

static uint8_t s_table_new[PTABLE_LEN];   // the httpd task is the only user
static uint8_t s_table_cur[PTABLE_LEN];

static bool previous_table(uint8_t *table)
{
    nvs_handle_t handle;
    size_t len = PTABLE_LEN;
    bool found = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        found = nvs_get_blob(handle, PTABLE_PREV_KEY, table, &len) == ESP_OK && len == PTABLE_LEN;
        nvs_close(handle);
    }
    return found;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_refused(httpd_req_t *req, const char *reason)
{
    char body[256];
    size_t len = 0;
    bool fits = json_append_raw(body, sizeof(body), &len, "{\"refused\":\"") &&
                json_append_escaped(body, sizeof(body), &len, reason) && json_append_raw(body, sizeof(body), &len, "\"}");
    return send_json(req, "400 Bad Request", fits ? body : "{\"refused\":\"refused\"}");
}

static void diff_json(char *out, size_t size, const ptable_diff_t *d)
{
    snprintf(out, size,
             "{\"same\":%s,\"vfs_moves\":%s,\"changes\":[{\"label\":\"ota_0\",\"from\":{\"address\":%lu,\"size\":%lu},"
             "\"to\":{\"address\":%lu,\"size\":%lu}},{\"label\":\"vfs\",\"from\":{\"address\":%lu,\"size\":%lu},"
             "\"to\":{\"address\":%lu,\"size\":%lu}}]}",
             d->same ? "true" : "false", d->vfs_moves ? "true" : "false",
             (unsigned long)d->ota0_from.address, (unsigned long)d->ota0_from.size,
             (unsigned long)d->ota0_to.address, (unsigned long)d->ota0_to.size,
             (unsigned long)d->vfs_from.address, (unsigned long)d->vfs_from.size,
             (unsigned long)d->vfs_to.address, (unsigned long)d->vfs_to.size);
    if (d->same) {
        snprintf(out, size, "{\"same\":true,\"vfs_moves\":false,\"changes\":[]}");
    }
}

// GET /partitions, open to everyone: every partition with the bytes it uses - the image in an app
// partition, the files in vfs; -1 where there is nothing to count - and whether a previous table
// is kept for going back.
static esp_err_t partitions_get(httpd_req_t *req)
{
    char body[1024];
    int len = snprintf(body, sizeof(body), "{\"table\":[");
    bool first = true;
    for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
         it != NULL && len < (int)sizeof(body); it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        long used = -1;
        if (p->type == ESP_PARTITION_TYPE_APP) {
            size_t image = 0;
            stored_image_span(p, &image);
            used = (long)image;
        } else if (strcmp(p->label, "vfs") == 0) {
            lfs2_t *fs = app_fs_begin();
            app_fs_usage_t usage;
            if (fs != NULL && app_fs_usage(fs, &usage)) {
                used = (long)usage.used_blocks * (long)usage.block_size;
            }
            if (fs != NULL) {
                app_fs_end();
            }
        }
        len += snprintf(body + len, sizeof(body) - len,
                        "%s{\"label\":\"%s\",\"type\":%d,\"subtype\":%d,\"address\":%lu,\"size\":%lu,\"used\":%ld}",
                        first ? "" : ",", p->label, p->type, p->subtype, (unsigned long)p->address,
                        (unsigned long)p->size, used);
        first = false;
    }
    bool previous = previous_table(s_table_new);
    len += snprintf(body + len, sizeof(body) - len, "],\"previous\":%s}", previous ? "true" : "false");
    if (len >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "partition list does not fit the reply buffer");
    }
    return send_json(req, "200 OK", body);
}

// Receives a table from the request body into s_table_new and checks it against the board's, read
// into s_table_cur. True when the table may be written; otherwise the refusal is already sent.
static bool receive_and_check(httpd_req_t *req, ptable_diff_t *diff)
{
    if (req->content_len == 0 || req->content_len > PTABLE_LEN) {
        send_refused(req, "not a partition table: 3072 bytes expected, as gen_esp32part.py writes them");
        return false;
    }
    if (!recv_exact(req, (char *)s_table_new, req->content_len)) {
        send_refused(req, "the table did not arrive whole");
        return false;
    }
    if (ptable_read(s_table_cur) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the board's own table could not be read");
        return false;
    }
    const char *bad = ptable_check(s_table_new, req->content_len, s_table_cur, diff);
    if (bad != NULL) {
        ESP_LOGW(TAG, "partition table refused: %s", bad);
        send_refused(req, bad);
        return false;
    }
    return true;
}

// POST /partitions/check: what the table in the body would change, or why it is refused. Writes nothing.
static esp_err_t partitions_check_post(httpd_req_t *req)
{
    if (!credentials_required(req) || !action_allowed(req)) {
        return ESP_OK;
    }
    ptable_diff_t diff;
    if (!receive_and_check(req, &diff)) {
        return ESP_OK;
    }
    char json[512];
    diff_json(json, sizeof(json), &diff);
    return send_json(req, "200 OK", json);
}

// Writes s_table_new over the board's table s_table_cur, already checked: the board's table goes to
// nvs for going back, boot is pointed at factory so the board comes up here, the sector is written
// and read back, the board restarts. A failed write puts the old table back before answering.
static esp_err_t apply_table(httpd_req_t *req, const ptable_diff_t *diff)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, PTABLE_PREV_KEY, s_table_cur, PTABLE_LEN);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "the board's table could not be kept for going back; nothing written");
    }
    const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    err = factory != NULL ? esp_ota_set_boot_partition(factory) : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "boot could not be pointed at factory; nothing written");
    }
    err = ptable_write(s_table_new);
    if (err != ESP_OK) {
        esp_err_t back = ptable_write(s_table_cur);
        ESP_LOGE(TAG, "partition table write failed (%s), old table put back: %s", esp_err_to_name(err), esp_err_to_name(back));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   back == ESP_OK ? "writing the new table failed; the old table is back in place"
                                                  : "writing failed and the old table could not be put back: keep the power on and connect the board by wire");
    }
    ESP_LOGW(TAG, "partition table rewritten: ota_0 %lu -> %lu bytes, vfs 0x%lx -> 0x%lx; restarting",
             (unsigned long)diff->ota0_from.size, (unsigned long)diff->ota0_to.size,
             (unsigned long)diff->vfs_from.address, (unsigned long)diff->vfs_to.address);
    char json[512];
    diff_json(json, sizeof(json), diff);
    send_json(req, "200 OK", json);
    restart_soon();
    return ESP_OK;
}

// POST /partitions/apply: the table in the body replaces the board's, when it passes the check and
// differs; a table equal to the board's writes nothing.
static esp_err_t partitions_apply_receive(httpd_req_t *req)
{
    ptable_diff_t diff;
    if (!receive_and_check(req, &diff)) {
        return ESP_OK;
    }
    if (diff.same) {
        return send_json(req, "200 OK", "{\"same\":true,\"vfs_moves\":false,\"changes\":[]}");
    }
    return apply_table(req, &diff);
}

static esp_err_t partitions_apply_post(httpd_req_t *req)
{
    return while_count_stopped(req, partitions_apply_receive);
}

// GET /partitions/previous: the table kept before the last rewrite, as the file gen_esp32part.py writes.
static esp_err_t partitions_previous_get(httpd_req_t *req)
{
    if (!credentials_required(req) || !action_allowed(req)) {
        return ESP_OK;
    }
    if (!previous_table(s_table_new)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no previous partition table is kept");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"partition-table-previous.bin\"");
    return httpd_resp_send(req, (const char *)s_table_new, PTABLE_LEN);
}

// POST /partitions/restore: the kept table goes back through the same check and the same write; the
// table it replaces is kept in its turn.
static esp_err_t partitions_restore_write(httpd_req_t *req)
{
    if (!previous_table(s_table_new)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no previous partition table is kept");
    }
    if (ptable_read(s_table_cur) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the board's own table could not be read");
    }
    ptable_diff_t diff;
    const char *bad = ptable_check(s_table_new, PTABLE_LEN, s_table_cur, &diff);
    if (bad != NULL) {
        return send_refused(req, bad);
    }
    if (diff.same) {
        return send_json(req, "200 OK", "{\"same\":true,\"vfs_moves\":false,\"changes\":[]}");
    }
    return apply_table(req, &diff);
}

static esp_err_t partitions_restore_post(httpd_req_t *req)
{
    return while_count_stopped(req, partitions_restore_write);
}

static void start_http(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Every handler runs on the httpd task. Saving a network (form buffers, two network lists,
    // nvs calls) overflowed the default 4096-byte stack and rebooted the board.
    config.stack_size = 8192;
    // Phones open many connections while probing a portal; the oldest ones are dropped.
    // Seven leaves sockets for the DNS server and mDNS in CONFIG_LWIP_MAX_SOCKETS (as esp-at).
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 24;   // twenty-three routes today
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/login", .method = HTTP_POST, .handler = login_post},
        {.uri = "/credentials", .method = HTTP_POST, .handler = credentials_post},
        {.uri = "/logout", .method = HTTP_POST, .handler = logout_post},
        {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/wifi/delete", .method = HTTP_POST, .handler = wifi_delete_post},
        {.uri = "/settings", .method = HTTP_POST, .handler = settings_post},
        {.uri = "/partitions", .method = HTTP_GET, .handler = partitions_get},
        {.uri = "/partitions/check", .method = HTTP_POST, .handler = partitions_check_post},
        {.uri = "/partitions/apply", .method = HTTP_POST, .handler = partitions_apply_post},
        {.uri = "/partitions/previous", .method = HTTP_GET, .handler = partitions_previous_get},
        {.uri = "/partitions/restore", .method = HTTP_POST, .handler = partitions_restore_post},
        {.uri = "/boot/app", .method = HTTP_POST, .handler = boot_app_post},
        {.uri = "/restart", .method = HTTP_POST, .handler = restart_post},
        {.uri = "/update", .method = HTTP_POST, .handler = update_post},
        {.uri = "/files", .method = HTTP_GET, .handler = files_list_route},
        {.uri = "/file", .method = HTTP_GET, .handler = file_get_route},
        {.uri = "/file", .method = HTTP_POST, .handler = file_post_route},
        {.uri = "/file/delete", .method = HTTP_POST, .handler = file_delete_route},
        {.uri = "/files/zip", .method = HTTP_GET, .handler = files_archive_route},
        {.uri = "/files/unpack/check", .method = HTTP_POST, .handler = files_unpack_check_route},
        {.uri = "/files/unpack", .method = HTTP_POST, .handler = files_unpack_route},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_page);
    httpd_register_err_handler(server, HTTPD_405_METHOD_NOT_ALLOWED, redirect_to_page);
    httpd_register_err_handler(server, HTTPD_414_URI_TOO_LONG, redirect_to_page);
}

void app_main(void)
{
    describe_self();
    status_led_show(STATUS_LED_RECOVERY);

    // Wi-Fi needs nvs initialised. The partition belongs to both images, so unlike the IDF
    // examples it is never erased on an init error: that would wipe MicroPython's data.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s, staying offline", esp_err_to_name(err));
        return;
    }

    s_sta_events = xEventGroupCreate();
    s_net_count = load_networks(s_nets);
    load_web_credentials();
    load_idle_return();

    // Portal traffic is mostly noise from phones probing the internet.
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    start_network();
    dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    start_dns_server(&dns);
    start_mdns();
    s_idle_lock = xSemaphoreCreateMutex();   // before the first request can take it
    start_http();
    const esp_timer_create_args_t idle = {.callback = idle_expired, .name = "idle-return"};
    if (esp_timer_create(&idle, &s_idle_timer) == ESP_OK) {
        activity();
    }
    xTaskCreate(join_task, "join", 4096, NULL, 5, NULL);
}
