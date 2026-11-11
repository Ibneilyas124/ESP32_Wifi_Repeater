/*
 * ESP32 WiFi Repeater / Internet Sharing Router (NAPT)
 * Board: ESP32 DevKit V1        Built by Sarfraz Qureshi - Ibn e Ilyas Technologies
 *
 * - STA connects to the main router, AP re-shares the internet through NAT
 * - DNS handed to clients is configurable (default 8.8.8.8) - fixes "connected, no internet"
 * - Name shown in the main router's client list is configurable (DHCP hostname)
 * - Styled login page (fixed user "Sarfraz", default password "admin", password changeable)
 * - Advanced settings table (all editable, all with defaults), MAC whitelist with device names
 * - Hardware factory reset: hold the BOOT button ~8 s, release when the LED blinks fast
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_private/wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_sntp.h"
#include <time.h>
#include "esp_timer.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "lwip/ip4_addr.h"
#include "esp_ota_ops.h"

static const char *TAG = "repeater";

#define NVS_NS            "repeater"
#define ADMIN_USER        "Sarfraz"        /* fixed, not changeable */
#define WL_BUF            1024             /* whitelist storage buffer */
#define RESET_BTN_GPIO    GPIO_NUM_0       /* BOOT button on DevKit V1 */
#define LED_GPIO          GPIO_NUM_2       /* onboard blue LED on DevKit V1 */
#define RESET_HOLD_MS     8000

/* HTML pages embedded from login.html / admin.html (see main/CMakeLists.txt) */
extern const char login_html_start[] asm("_binary_login_html_start");
extern const char admin_html_start[] asm("_binary_admin_html_start");
extern const char tools_html_start[] asm("_binary_tools_html_start");

#define FW_VERSION "2.1.0"

static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_sta_connected = false;
static char s_sta_ip[16] = "0.0.0.0";
static int64_t s_boot_time_us = 0;
static char s_session_token[40] = "";
static uint32_t s_subnet_base = 0, s_subnet_mask = 0;
static volatile bool s_guest_filter_on = true;

/* ---- deauth monitor ring buffer ---- */
#define DEAUTH_RING 24
typedef struct { int64_t t; uint8_t type; uint8_t src[6]; uint8_t dst[6]; int8_t rssi; bool ours; } deauth_evt_t;
static deauth_evt_t s_deauth_ring[DEAUTH_RING];
static volatile int s_deauth_head = 0, s_deauth_count = 0, s_deauth_total = 0;

/* ---- upstream connection stability log ---- */
#define DISCON_RING 10
typedef struct { int64_t t; uint8_t reason; } discon_evt_t;
static discon_evt_t s_discon_ring[DISCON_RING];
static volatile int s_discon_head = 0, s_discon_count = 0, s_discon_total = 0;
static int64_t s_sta_since_us = 0;   /* time of last successful upstream connect */

/* ---- per-device usage tracking + optional uplink rate limit (whitelisted devices only) ---- */
#define TRACK_MAX 16
typedef struct {
    uint8_t mac[6];
    bool used;
    uint64_t total_bytes;
    uint32_t week_bytes;  int week_no;
    uint32_t month_bytes; int month_no;
    uint32_t bucket_tokens;
    int64_t bucket_last_us;
} usage_entry_t;
static usage_entry_t s_usage[TRACK_MAX];
static bool s_time_synced = false;

/* ---- repeater-wide (all devices combined) download tracking + rate cap.
 * Unlike per-device usage above, this is measured on the upstream (STA)
 * side, so it is the one number in this firmware that genuinely reflects
 * real internet usage in BOTH directions for the whole repeater - not just
 * uploads. It cannot be split back out per device (no public API exposes
 * the NAT table), so it is reported and capped in aggregate only. */
static usage_entry_t s_down_usage;           /* .mac unused - whole-repeater totals */
static uint32_t s_down_bucket_tokens = 0;
static int64_t s_down_bucket_last_us = 0;
static uint32_t s_up_bucket_tokens = 0;       /* overall (all-clients) uplink bucket */
static int64_t s_up_bucket_last_us = 0;

/* ---- speed-history: periodic latency samples to 1.1.1.1 (every ~15 min) ---- */
#define SPEEDHIST_MAX 48
typedef struct { int64_t t; int ms; bool ok; } speedhist_t;
static speedhist_t s_speedhist[SPEEDHIST_MAX];
static int s_speedhist_head = 0, s_speedhist_count = 0;

/* ---- background scan / diagnose state ---- */
static volatile int s_scan_state = 0;    /* 0 idle/done, 1 running, 2 error */
static volatile int s_diag_state = 0;    /* 0 idle, 1 running, 2 done */
static cJSON *s_diag_result = NULL;

/* ================= Settings table (single source of truth) ================= */

typedef struct {
    const char *key;
    const char *label;
    const char *def;     /* default value */
    const char *type;    /* text | password | password_keep | number | bool */
    const char *hint;
    const char *group;   /* which collapsible section this appears under in the dashboard */
} setting_t;

static const setting_t SETTINGS[] = {
    {"ap_ssid",    "Repeater WiFi name (SSID)",        "Sarfraz",                  "text",          "1-32 characters", "wifi"},
    {"ap_pass",    "Repeater WiFi password",           "Sarfraz1",                 "password",      "8-63 characters. Blank = open network (no password)", "wifi"},
    {"ap_hidden",  "Hide repeater WiFi name",          "0",                        "bool",          "Hidden networks must be added manually on phones", "wifi"},
    {"ap_channel", "Repeater channel",                 "1",                        "number",        "1-13. Automatically follows the main router while connected", "wifi"},
    {"ap_max_conn","Max connected devices",            "8",                        "number",        "1-10", "wifi"},
    {"ap_ip",      "Repeater IP address",              "192.168.4.1",              "text",          "Change only if it clashes with the main router's network", "wifi"},
    {"dns1",       "DNS server given to clients",      "8.8.8.8",                  "text",          "If sites do not open try 8.8.8.8, 8.8.4.4, 1.1.1.1 or your ISP DNS", "wifi"},
    {"hostname",   "Name shown in main router",        "Ibn-e-Ilyas-Technologies", "text",          "1-32 characters. Letters, digits and hyphen work best on routers", "wifi"},
    {"tx_power",   "Transmit power (dBm)",             "20",                       "number",        "2-20. Lower it if the device overheats", "wifi"},
    {"guest_filter","Guests: connect but no internet",  "1",                        "bool",          "1 = wrong-listed devices can join & reach this page but get no internet. 0 = they get disconnected immediately", "security"},
    {"deauth_monitor","Deauth attack monitor",          "1",                        "bool",          "Passively watches for deauth/disassoc floods nearby. See WiFi Tools > Deauth", "security"},
    {"deauth_alert","Deauth alert threshold",           "10",                       "number",        "Frames in 10s that count as an attack (1-50)", "security"},
    {"admin_pass", "Admin password",                   "admin",                    "password_keep", "4-32 characters. Leave blank to keep the current one", "security"},
    {"sta_mac",    "Custom MAC toward main router",    "",                         "text",          "Blank = factory MAC. Format AA:BB:CC:DD:EE:FF, first byte must be even", "network"},
    {"sta_static_ip","Remote-management IP (static)",   "",                         "text",          "Blank = automatic (from main router). Set this to stop it changing, e.g. if you run 2+ repeaters", "network"},
    {"sta_gateway", "Static IP: main router's address", "",                         "text",          "Required only if Remote-management IP above is set. Usually the main router's own IP", "network"},
    {"sta_netmask", "Static IP: subnet mask",            "255.255.255.0",            "text",          "Required only if Remote-management IP above is set. Leave default unless you know why", "network"},
    {"auto_reboot_hours","Auto-restart every N hours",   "0",                        "number",        "0 = never. Otherwise the repeater reboots itself every N hours (1-168) to stay fresh", "network"},
    {"overall_up_kbps",  "Overall upload cap (all devices)", "0",                    "number",        "0 = unlimited. Total upload speed shared by every connected device combined, in Kbps", "bandwidth"},
    {"overall_down_kbps","Overall download cap (all devices)","0",                   "number",        "0 = unlimited. Total download/streaming speed shared by every connected device combined, in Kbps", "bandwidth"},
    {"tz_offset",  "Time zone offset (hours from UTC)", "5",                      "number",        "Used for whitelist time windows. Pakistan = 5. Range -12 to 14", "network"},
};
#define N_SETTINGS (sizeof(SETTINGS) / sizeof(SETTINGS[0]))

/* ================= NVS helpers ================= */

static void nvs_get_string(const char *key, char *out, size_t out_len, const char *def) {
    nvs_handle_t h;
    size_t len = out_len;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, key, out, &len) == ESP_OK) {
            nvs_close(h);
            return;
        }
        nvs_close(h);
    }
    strncpy(out, def, out_len - 1);
    out[out_len - 1] = '\0';
}

static void nvs_set_string(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Separate, never-wiped namespace, used only to PROVE a factory reset really
 * happened (a reset counter) - since some default values (e.g. the WiFi
 * name/password) look identical before and after a reset if they were never
 * customised, which was confusing to verify otherwise. */
#define DIAG_NS "repeater_diag"

static int bump_reset_counter(void) {
    nvs_handle_t h;
    int32_t count = 0;
    if (nvs_open(DIAG_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_i32(h, "resets", &count);
        count++;
        nvs_set_i32(h, "resets", count);
        nvs_commit(h);
        nvs_close(h);
    }
    return count;
}

static int get_reset_counter(void) {
    nvs_handle_t h;
    int32_t count = 0;
    if (nvs_open(DIAG_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "resets", &count);
        nvs_close(h);
    }
    return count;
}

static void wipe_settings(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    bump_reset_counter();
}

static const setting_t *find_setting(const char *key) {
    for (size_t i = 0; i < N_SETTINGS; i++) {
        if (strcmp(SETTINGS[i].key, key) == 0) return &SETTINGS[i];
    }
    return NULL;
}

static void get_setting(const char *key, char *out, size_t out_len) {
    const setting_t *s = find_setting(key);
    nvs_get_string(key, out, out_len, s ? s->def : "");
}

static int get_setting_int(const char *key, int lo, int hi, int fallback) {
    char b[16];
    get_setting(key, b, sizeof(b));
    if (b[0] == '\0') return fallback;
    int v = atoi(b);
    if (v < lo || v > hi) return fallback;
    return v;
}

/* ================= Validation ================= */

static bool valid_ipv4_any(const char *s) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

static bool valid_host_ipv4(const char *s) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a < 1 || a > 223 || b > 255 || c > 255 || d < 1 || d > 254) return false;
    return true;
}

static bool parse_mac(const char *s, uint8_t out[6]) {
    unsigned v[6];
    char extra;
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &extra) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

static bool int_in_range(const char *v, int lo, int hi) {
    if (*v == '\0' || strlen(v) > 4) return false;
    for (const char *p = v; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    int x = atoi(v);
    return x >= lo && x <= hi;
}

static bool validate_setting(const char *key, const char *v, char *err, size_t n) {
    size_t len = strlen(v);
    if (strcmp(key, "ap_ssid") == 0) {
        if (len < 1 || len > 32) { snprintf(err, n, "must be 1-32 characters"); return false; }
    } else if (strcmp(key, "ap_pass") == 0) {
        if (len != 0 && (len < 8 || len > 63)) { snprintf(err, n, "must be 8-63 characters (or blank for open network)"); return false; }
    } else if (strcmp(key, "ap_hidden") == 0) {
        if (strcmp(v, "0") != 0 && strcmp(v, "1") != 0) { snprintf(err, n, "must be 0 or 1"); return false; }
    } else if (strcmp(key, "ap_channel") == 0) {
        if (!int_in_range(v, 1, 13)) { snprintf(err, n, "must be 1-13"); return false; }
    } else if (strcmp(key, "ap_max_conn") == 0) {
        if (!int_in_range(v, 1, 10)) { snprintf(err, n, "must be 1-10"); return false; }
    } else if (strcmp(key, "ap_ip") == 0 || strcmp(key, "dns1") == 0) {
        if (!valid_host_ipv4(v)) { snprintf(err, n, "must be a valid IPv4 address like 192.168.4.1"); return false; }
    } else if (strcmp(key, "hostname") == 0) {
        if (len < 1 || len > 32) { snprintf(err, n, "must be 1-32 characters"); return false; }
        for (size_t i = 0; i < len; i++) {
            if ((unsigned char)v[i] < 0x20) { snprintf(err, n, "contains invalid characters"); return false; }
        }
    } else if (strcmp(key, "sta_mac") == 0) {
        uint8_t m[6];
        if (len != 0 && (!parse_mac(v, m) || (m[0] & 1))) {
            snprintf(err, n, "must look like AA:BB:CC:DD:EE:FF with an even first byte");
            return false;
        }
    } else if (strcmp(key, "tx_power") == 0) {
        if (!int_in_range(v, 2, 20)) { snprintf(err, n, "must be 2-20"); return false; }
    } else if (strcmp(key, "sta_static_ip") == 0 || strcmp(key, "sta_gateway") == 0) {
        if (len != 0 && !valid_ipv4_any(v)) { snprintf(err, n, "must be a valid IPv4 address, or blank"); return false; }
    } else if (strcmp(key, "sta_netmask") == 0) {
        if (!valid_ipv4_any(v)) { snprintf(err, n, "must be a valid IPv4 mask like 255.255.255.0"); return false; }
    } else if (strcmp(key, "auto_reboot_hours") == 0) {
        if (!int_in_range(v, 0, 168)) { snprintf(err, n, "must be 0-168"); return false; }
    } else if (strcmp(key, "overall_up_kbps") == 0 || strcmp(key, "overall_down_kbps") == 0) {
        if (!int_in_range(v, 0, 9999)) { snprintf(err, n, "must be 0-9999 Kbps (0 = unlimited)"); return false; }
    } else if (strcmp(key, "guest_filter") == 0 || strcmp(key, "deauth_monitor") == 0) {
        if (strcmp(v, "0") != 0 && strcmp(v, "1") != 0) { snprintf(err, n, "must be 0 or 1"); return false; }
    } else if (strcmp(key, "deauth_alert") == 0) {
        if (!int_in_range(v, 1, 50)) { snprintf(err, n, "must be 1-50"); return false; }
    } else if (strcmp(key, "tz_offset") == 0) {
        int tzv = atoi(v);
        if (len < 1 || len > 3 || tzv < -12 || tzv > 14) { snprintf(err, n, "must be -12 to 14"); return false; }
    } else if (strcmp(key, "admin_pass") == 0) {
        if (len < 4 || len > 32) { snprintf(err, n, "must be 4-32 characters"); return false; }
    }
    return true;
}

/* ================= MAC whitelist: "MAC|Name,MAC|Name,..." (empty = allow all) ================= */

static void whitelist_get(char *out, size_t out_len) {
    nvs_get_string("whitelist", out, out_len, "");
}

static void whitelist_set(const char *csv) {
    nvs_set_string("whitelist", csv);
}

static bool mac_str_eq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static void split_entry3(const char *entry, char *mac_out, size_t mac_len, char *name_out, size_t name_len, int *limit_out) {
    const char *bar1 = strchr(entry, '|');
    if (!bar1) {
        strncpy(mac_out, entry, mac_len - 1); mac_out[mac_len - 1] = '\0';
        name_out[0] = '\0'; if (limit_out) *limit_out = 0;
        return;
    }
    size_t mlen = (size_t)(bar1 - entry);
    if (mlen >= mac_len) mlen = mac_len - 1;
    memcpy(mac_out, entry, mlen);
    mac_out[mlen] = '\0';

    const char *bar2 = strchr(bar1 + 1, '|');
    if (bar2) {
        size_t nlen = (size_t)(bar2 - (bar1 + 1));
        if (nlen >= name_len) nlen = name_len - 1;
        memcpy(name_out, bar1 + 1, nlen);
        name_out[nlen] = '\0';
        if (limit_out) *limit_out = atoi(bar2 + 1);
    } else {
        strncpy(name_out, bar1 + 1, name_len - 1);
        name_out[name_len - 1] = '\0';
        if (limit_out) *limit_out = 0;
    }
}

/* kept for the 2 call sites that only ever need mac+name */
static void split_entry(const char *entry, char *mac_out, size_t mac_len, char *name_out, size_t name_len) {
    split_entry3(entry, mac_out, mac_len, name_out, name_len, NULL);
}

static bool whitelist_contains(const char *mac) {
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    if (strlen(list) == 0) return true;
    char *tok = strtok(list, ",");
    bool found = false;
    while (tok) {
        while (*tok == ' ') tok++;
        char m[20], n[64];
        split_entry(tok, m, sizeof(m), n, sizeof(n));
        if (mac_str_eq(m, mac)) { found = true; break; }
        tok = strtok(NULL, ",");
    }
    return found;
}

/* ---- Device labels: admin ka diya naam, MAC se NVS "labels" mein ("MAC=naam|MAC=naam|"). Whitelist se alag, access par asar nahi. ---- */
#define LBL_BUF 1024
static bool label_get(const char *mac, char *out, size_t n) {
    static char list[LBL_BUF];
    nvs_get_string("labels", list, sizeof(list), "");
    size_t ml = strlen(mac);
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, '|');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > ml + 1 && p[ml] == '=' && strncasecmp(p, mac, ml) == 0) {
            size_t vl = len - ml - 1;
            if (vl >= n) vl = n - 1;
            memcpy(out, p + ml + 1, vl);
            out[vl] = 0;
            return true;
        }
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static bool label_set(const char *mac, const char *name) {
    static char list[LBL_BUF];
    static char out[LBL_BUF + 64];
    nvs_get_string("labels", list, sizeof(list), "");
    size_t ml = strlen(mac), ol = 0;
    out[0] = 0;
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, '|');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        bool same = (len > ml && p[ml] == '=' && strncasecmp(p, mac, ml) == 0);
        if (!same && len > 0 && ol + len + 1 < sizeof(out)) {
            memcpy(out + ol, p, len);
            ol += len;
            out[ol++] = '|';
            out[ol] = 0;
        }
        if (!e) break;
        p = e + 1;
    }
    if (name[0]) {
        size_t nl = strlen(name);
        if (ol + ml + 1 + nl + 1 >= LBL_BUF) return false;
        memcpy(out + ol, mac, ml); ol += ml;
        out[ol++] = '=';
        memcpy(out + ol, name, nl); ol += nl;
        out[ol++] = '|';
        out[ol] = 0;
    }
    nvs_set_string("labels", out);
    return true;
}

static bool whitelist_get_name(const char *mac, char *name_out, size_t name_len) {
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    if (strlen(list) == 0) return false;
    char *copy = strdup(list);
    if (!copy) return false;
    char *tok = strtok(copy, ",");
    bool found = false;
    while (tok) {
        while (*tok == ' ') tok++;
        char m[20], n[64];
        split_entry(tok, m, sizeof(m), n, sizeof(n));
        if (mac_str_eq(m, mac)) {
            strncpy(name_out, n, name_len - 1);
            name_out[name_len - 1] = '\0';
            found = true;
            break;
        }
        tok = strtok(NULL, ",");
    }
    free(copy);
    return found;
}

static int whitelist_get_limit(const char *mac) {
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    if (strlen(list) == 0) return 0;
    char *tok = strtok(list, ",");
    int limit = 0;
    while (tok) {
        while (*tok == ' ') tok++;
        char m[20], n[64]; int lim = 0;
        split_entry3(tok, m, sizeof(m), n, sizeof(n), &lim);
        if (mac_str_eq(m, mac)) { limit = lim; break; }
        tok = strtok(NULL, ",");
    }
    return limit;
}

static void whitelist_remove(const char *mac) {
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    char out[WL_BUF] = "";
    char *copy = strdup(list);
    if (!copy) return;
    char *tok = strtok(copy, ",");
    bool first = true;
    while (tok) {
        while (*tok == ' ') tok++;
        char m[20], n[64]; int lim2;
        split_entry3(tok, m, sizeof(m), n, sizeof(n), &lim2);
        if (!mac_str_eq(m, mac)) {
            if (!first) strncat(out, ",", sizeof(out) - strlen(out) - 1);
            strncat(out, tok, sizeof(out) - strlen(out) - 1);
            first = false;
        }
        tok = strtok(NULL, ",");
    }
    free(copy);
    whitelist_set(out);
}

/* Adds or renames an entry (limit_kbps 0 = unlimited). Returns false when the list is full. */
static bool whitelist_add(const char *mac, const char *name, int limit_kbps, const char *sched) {
    whitelist_remove(mac);
    char clean[48];
    size_t j = 0;
    for (size_t i = 0; name && name[i] && j < sizeof(clean) - 1; i++) {
        clean[j++] = (name[i] == ',' || name[i] == '|') ? ' ' : name[i];
    }
    clean[j] = '\0';
    if (limit_kbps < 0) limit_kbps = 0;
    if (limit_kbps > 100000) limit_kbps = 100000;

    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    char entry[112];
    if (sched && sched[0]) snprintf(entry, sizeof(entry), "%.20s|%.47s|%d|%.9s", mac, clean, limit_kbps, sched);
    else snprintf(entry, sizeof(entry), "%.20s|%.47s|%d", mac, clean, limit_kbps);
    if (strlen(list) + strlen(entry) + 2 >= WL_BUF) return false;
    if (strlen(list) == 0) {
        whitelist_set(entry);
    } else {
        char buf[WL_BUF + 144];
        snprintf(buf, sizeof(buf), "%s,%s", list, entry);
        whitelist_set(buf);
    }
    return true;
}

/* ---- Whitelist schedule: entry "MAC|Name|limit|HHMM-HHMM"; khali = hamesha allowed ---- */
static bool sched_parse(const char *sc, int *a_min, int *b_min) {
    if (strlen(sc) != 9 || sc[4] != '-') return false;
    for (int i = 0; i < 9; i++) {
        if (i == 4) continue;
        if (sc[i] < '0' || sc[i] > '9') return false;
    }
    int ah = (sc[0] - '0') * 10 + (sc[1] - '0'), am = (sc[2] - '0') * 10 + (sc[3] - '0');
    int bh = (sc[5] - '0') * 10 + (sc[6] - '0'), bm = (sc[7] - '0') * 10 + (sc[8] - '0');
    if (ah > 23 || bh > 23 || am > 59 || bm > 59) return false;
    *a_min = ah * 60 + am;
    *b_min = bh * 60 + bm;
    return true;
}

static bool wl_sched_get(const char *mac, char *out, size_t n) {
    out[0] = '\0';
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    if (!list[0]) return false;
    char *copy = strdup(list);
    if (!copy) return false;
    bool found = false;
    char *save = NULL;
    char *tok = strtok_r(copy, ",", &save);
    while (tok) {
        while (*tok == ' ') tok++;
        char m[20], nm[64];
        split_entry(tok, m, sizeof(m), nm, sizeof(nm));
        if (mac_str_eq(m, mac)) {
            const char *b1 = strchr(tok, '|');
            const char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
            const char *b3 = b2 ? strchr(b2 + 1, '|') : NULL;
            if (b3) snprintf(out, n, "%.9s", b3 + 1);
            found = true;
            break;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    free(copy);
    return found;
}

/* true = abhi internet allowed. Time sync se pehle ya schedule na ho to hamesha true. 4 MAC ka 1 second ka cache */
static bool wl_in_schedule(const char *mac) {
    time_t now = time(NULL);
    if (now < 1700000000) return true;
    static int64_t c_t[4];
    static char c_m[4][18];
    static bool c_r[4];
    static int c_next = 0;
    int64_t us = esp_timer_get_time();
    for (int i = 0; i < 4; i++) {
        if (c_t[i] && us - c_t[i] < 1000000 && strcmp(c_m[i], mac) == 0) return c_r[i];
    }
    bool res = true;
    char sc[12];
    int a_min, b_min;
    if (wl_sched_get(mac, sc, sizeof(sc)) && sc[0] && sched_parse(sc, &a_min, &b_min) && a_min != b_min) {
        int off = get_setting_int("tz_offset", -12, 14, 5);
        int64_t loc = (int64_t)now + (int64_t)off * 3600;
        int cur = (int)(((loc % 86400) + 86400) % 86400 / 60);
        if (a_min < b_min) res = (cur >= a_min && cur < b_min);
        else res = (cur >= a_min || cur < b_min);
    }
    int sl = c_next; c_next = (c_next + 1) % 4;
    c_t[sl] = us; c_r[sl] = res;
    snprintf(c_m[sl], sizeof(c_m[sl]), "%s", mac);
    return res;
}

/* ================= Session auth ================= */

static void generate_session_token(char *out, size_t len) {
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        memcpy(bytes + i * 4, &r, 4);
    }
    size_t pos = 0;
    for (int i = 0; i < 16 && pos + 2 < len; i++) {
        out[pos++] = hex[(bytes[i] >> 4) & 0xF];
        out[pos++] = hex[bytes[i] & 0xF];
    }
    out[pos] = '\0';
}

static bool check_session(httpd_req_t *req) {
    if (strlen(s_session_token) == 0) return false;
    char cookie[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    char needle[64];
    snprintf(needle, sizeof(needle), "session=%s", s_session_token);
    return strstr(cookie, needle) != NULL;
}

static esp_err_t require_session_api(httpd_req_t *req) {
    if (check_session(req)) return ESP_OK;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

/* compares without leaking where the first mismatch is */
static bool secure_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la != lb);
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

/* ================= WiFi ================= */

/* ---- Guest packet filter: unauthorized clients keep local access (DHCP,
 * ARP, the admin/tools page at the repeater's own IP) but every packet bound
 * OUTSIDE the local subnet - i.e. anything that would be NAT'd to the
 * internet - is dropped before it ever reaches the IP stack. Whitelisted
 * clients are untouched and behave exactly as before. This only runs when
 * "guest_filter" is on AND the whitelist is not empty (an empty whitelist
 * still means "allow everyone fully", unchanged from earlier versions). */

/* ---- usage tracking + rate limit helpers ---- */

static void usage_key(const uint8_t mac[6], char *out, size_t n) {
    snprintf(out, n, "u%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void usage_load(usage_entry_t *e) {
    char key[16]; usage_key(e->mac, key, sizeof(key));
    char v[96] = "";
    nvs_get_string(key, v, sizeof(v), "");
    unsigned long long tb = 0; unsigned wb = 0; int wn = 0; unsigned mb = 0; int mn = 0;
    if (sscanf(v, "%llu,%u,%d,%u,%d", &tb, &wb, &wn, &mb, &mn) == 5) {
        e->total_bytes = tb; e->week_bytes = wb; e->week_no = wn; e->month_bytes = mb; e->month_no = mn;
    }
}

static void usage_save(usage_entry_t *e) {
    char key[16]; usage_key(e->mac, key, sizeof(key));
    char v[96];
    snprintf(v, sizeof(v), "%llu,%lu,%d,%lu,%d", (unsigned long long)e->total_bytes, (unsigned long)e->week_bytes, e->week_no, (unsigned long)e->month_bytes, e->month_no);
    nvs_set_string(key, v);
}

/* whole-repeater download totals - persisted under a fixed NVS key */
static void down_usage_load(void) {
    char v[96] = "";
    nvs_get_string("down_total", v, sizeof(v), "");
    unsigned long long tb = 0; unsigned wb = 0; int wn = 0; unsigned mb = 0; int mn = 0;
    if (sscanf(v, "%llu,%u,%d,%u,%d", &tb, &wb, &wn, &mb, &mn) == 5) {
        s_down_usage.total_bytes = tb; s_down_usage.week_bytes = wb; s_down_usage.week_no = wn;
        s_down_usage.month_bytes = mb; s_down_usage.month_no = mn;
    }
}

static void down_usage_save(void) {
    char v[96];
    snprintf(v, sizeof(v), "%llu,%lu,%d,%lu,%d", (unsigned long long)s_down_usage.total_bytes,
             (unsigned long)s_down_usage.week_bytes, s_down_usage.week_no, (unsigned long)s_down_usage.month_bytes, s_down_usage.month_no);
    nvs_set_string("down_total", v);
}

/* finds a tracked device, creating a new slot (loaded from NVS) the first time it is seen */
static usage_entry_t *usage_find_or_add(const uint8_t mac[6]) {
    for (int i = 0; i < TRACK_MAX; i++) {
        if (s_usage[i].used && memcmp(s_usage[i].mac, mac, 6) == 0) return &s_usage[i];
    }
    for (int i = 0; i < TRACK_MAX; i++) {
        if (!s_usage[i].used) {
            memset(&s_usage[i], 0, sizeof(usage_entry_t));
            memcpy(s_usage[i].mac, mac, 6);
            s_usage[i].used = true;
            usage_load(&s_usage[i]);
            return &s_usage[i];
        }
    }
    return NULL; /* table full - that device just won't be tracked */
}

/* call once/minute or so: rolls week/month counters over using real time (needs SNTP) */
static void usage_housekeeping(void) {
    time_t now = time(NULL);
    if (now < 1700000000) return; /* clock not synced yet - do not reset anything on guesswork */
    int week_no = (int)(now / 604800);
    struct tm tmv; localtime_r(&now, &tmv);
    int month_no = tmv.tm_year * 12 + tmv.tm_mon;
    bool any_dirty = false;
    for (int i = 0; i < TRACK_MAX; i++) {
        if (!s_usage[i].used) continue;
        bool dirty = false;
        if (s_usage[i].week_no != week_no) { s_usage[i].week_bytes = 0; s_usage[i].week_no = week_no; dirty = true; }
        if (s_usage[i].month_no != month_no) { s_usage[i].month_bytes = 0; s_usage[i].month_no = month_no; dirty = true; }
        if (dirty) { usage_save(&s_usage[i]); any_dirty = true; }
    }
    if (s_down_usage.week_no != week_no) { s_down_usage.week_bytes = 0; s_down_usage.week_no = week_no; any_dirty = true; }
    if (s_down_usage.month_no != month_no) { s_down_usage.month_bytes = 0; s_down_usage.month_no = month_no; any_dirty = true; }
    if (any_dirty) down_usage_save();
}

static void usage_persist_all(void) {
    for (int i = 0; i < TRACK_MAX; i++) {
        if (s_usage[i].used) usage_save(&s_usage[i]);
    }
    down_usage_save();
}

static void update_subnet_cache(void) {
    char ip[16];
    get_setting("ap_ip", ip, sizeof(ip));
    s_subnet_mask = esp_ip4addr_aton("255.255.255.0");
    s_subnet_base = esp_ip4addr_aton(ip) & s_subnet_mask;
}

/* ---- DHCP hostname (option 12) sniffing: phone ka naam Connected Devices ke liye.
 * Sirf padhta hai, packet kabhi nahi badalta; ap_rx_filter ki pehli line se chalta hai
 * (guest_filter on ho ya off). Table sirf RAM mein, boot par khali. ---- */
#define DHCP_HOST_MAX 16
typedef struct { uint8_t mac[6]; char name[32]; bool used; } dhcp_host_t;
static dhcp_host_t s_dhcp_host[DHCP_HOST_MAX];
static int s_dhcp_host_next = 0;

/* name hamesha 32 bytes ka, NUL-terminated array hona chahiye */
static void dhcp_host_store(const uint8_t mac[6], const char name[32]) {
    int slot = -1;
    for (int i = 0; i < DHCP_HOST_MAX; i++) {
        if (s_dhcp_host[i].used && memcmp(s_dhcp_host[i].mac, mac, 6) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < DHCP_HOST_MAX; i++) {
            if (!s_dhcp_host[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) {
        slot = s_dhcp_host_next;
        s_dhcp_host_next = (s_dhcp_host_next + 1) % DHCP_HOST_MAX;
    }
    s_dhcp_host[slot].used = false;
    memcpy(s_dhcp_host[slot].mac, mac, 6);
    memcpy(s_dhcp_host[slot].name, name, 32);
    s_dhcp_host[slot].used = true;
}

static bool dhcp_host_lookup(const uint8_t mac[6], char *out, size_t n) {
    for (int i = 0; i < DHCP_HOST_MAX; i++) {
        if (s_dhcp_host[i].used && memcmp(s_dhcp_host[i].mac, mac, 6) == 0) {
            snprintf(out, n, "%s", s_dhcp_host[i].name);
            return true;
        }
    }
    return false;
}

/* ---- Phone model/OS: HTTP User-Agent (port 80 ki request sniff, MAC se; ya repeater ke apne pages se, IP se)
 * aur DHCP option 60 (vendor class, MAC se). Sirf RAM, boot par khali. Packet kabhi nahi badalta. ---- */
#define DEVINFO_MAX 16
typedef struct { uint8_t mac[6]; char vc[32]; bool used; } dhcp_vc_t;
static dhcp_vc_t s_dhcp_vc[DEVINFO_MAX];
static int s_dhcp_vc_next = 0;

/* vc hamesha 32 bytes ka, NUL-terminated array hona chahiye */
static void dhcp_vc_store(const uint8_t mac[6], const char vc[32]) {
    int slot = -1;
    for (int i = 0; i < DEVINFO_MAX; i++) {
        if (s_dhcp_vc[i].used && memcmp(s_dhcp_vc[i].mac, mac, 6) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < DEVINFO_MAX; i++) {
            if (!s_dhcp_vc[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) {
        slot = s_dhcp_vc_next;
        s_dhcp_vc_next = (s_dhcp_vc_next + 1) % DEVINFO_MAX;
    }
    s_dhcp_vc[slot].used = false;
    memcpy(s_dhcp_vc[slot].mac, mac, 6);
    memcpy(s_dhcp_vc[slot].vc, vc, 32);
    s_dhcp_vc[slot].used = true;
}

static bool dhcp_vc_lookup(const uint8_t mac[6], char *out, size_t n) {
    for (int i = 0; i < DEVINFO_MAX; i++) {
        if (s_dhcp_vc[i].used && memcmp(s_dhcp_vc[i].mac, mac, 6) == 0) {
            snprintf(out, n, "%s", s_dhcp_vc[i].vc);
            return true;
        }
    }
    return false;
}

/* kind: 1 = key MAC hai (sniff), 2 = key IP hai (pehle 4 bytes, baqi 0) */
typedef struct { uint8_t kind; uint8_t key[6]; char model[32]; char os[20]; } ua_info_t;
static ua_info_t s_ua[DEVINFO_MAX];
static int s_ua_next = 0;
static char s_ua_buf[200];   /* sirf WiFi task (http_sniff) istemal karta hai */

#include "model_table.h"

/* code (jaise SM-A325F) ka marketing naam, Google ki list se; brand optional */
static const char *model_lookup(const char *code, const char **brand) {
    char k[40];
    size_t n = strlen(code);
    if (n == 0 || n >= sizeof(k)) return NULL;
    for (size_t i = 0; i <= n; i++) {
        char c = code[i];
        k[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    int lo = 0, hi = MODEL_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *e = MODEL_BLOB + MODEL_OFF[mid];
        int c = strcmp(e + 1, k);
        if (c == 0) {
            if (brand) *brand = MODEL_BRANDS[(uint8_t)e[0]];
            return e + 1 + strlen(e + 1) + 1;
        }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static const char *model_friendly(const char *code) { return model_lookup(code, NULL); }

static const char *model_brand(const char *code) {
    const char *b = NULL;
    model_lookup(code, &b);
    return b ? b : "";
}

static const char *brand_from_model(const char *m) {
    if (strncmp(m, "SM-", 3) == 0) return "Samsung";
    if (strncasecmp(m, "Redmi", 5) == 0 || strncasecmp(m, "POCO", 4) == 0 ||
        strncasecmp(m, "Xiaomi", 6) == 0 || strncasecmp(m, "Mi ", 3) == 0) return "Xiaomi";
    if (strncasecmp(m, "Infinix", 7) == 0) return "Infinix";
    if (strncasecmp(m, "TECNO", 5) == 0) return "Tecno";
    if (strncasecmp(m, "itel", 4) == 0) return "itel";
    if (strncmp(m, "CPH", 3) == 0) return "OPPO";
    if (strncmp(m, "RMX", 3) == 0) return "realme";
    if (strncasecmp(m, "vivo", 4) == 0) return "vivo";
    if (strncasecmp(m, "Pixel", 5) == 0) return "Google";
    if (strncmp(m, "iPhone", 6) == 0) return "Apple";
    if (strncasecmp(m, "moto", 4) == 0) return "Motorola";
    if (strncasecmp(m, "Galaxy", 6) == 0) return "Samsung";
    return "";
}

/* User-Agent se model code aur OS; kuch na mile to khali string. Chrome ka reduced "K" model ignore */
static void ua_parse(const char *ua, char *model, size_t mn, char *os, size_t on) {
    model[0] = 0; os[0] = 0;
    const char *a = strstr(ua, "Android ");
    if (a) {
        const char *v = a + 8;
        int k = 0;
        while (v[k] && v[k] != ';' && v[k] != ')' && k < 10) k++;
        snprintf(os, on, "Android %.*s", k, v);
        if (v[k] == ';') {
            const char *m = v + k + 1;
            while (*m == ' ') m++;
            int l = 0;
            while (m[l] && m[l] != ';' && m[l] != ')' && l < 60) l++;
            for (int z = 0; z + 7 <= l; z++) {
                if (strncmp(m + z, " Build/", 7) == 0) { l = z; break; }
            }
            while (l > 0 && m[l - 1] == ' ') l--;
            if (l >= 2 && !(l == 2 && m[0] == 'w' && m[1] == 'v')) {
                size_t q = 0;
                for (int z = 0; z < l && q + 1 < mn; z++) {
                    uint8_t ch = (uint8_t)m[z];
                    if (ch >= 0x20 && ch <= 0x7E) model[q++] = (char)ch;
                }
                model[q] = 0;
            }
        }
    } else if (strstr(ua, "iPhone")) {
        snprintf(model, mn, "iPhone");
        const char *p = strstr(ua, "iPhone OS ");
        if (p) {
            p += 10;
            char vb[12];
            int k = 0;
            while (p[k] && p[k] != ' ' && p[k] != ')' && k < 8) { vb[k] = (p[k] == '_') ? '.' : p[k]; k++; }
            vb[k] = 0;
            snprintf(os, on, "iOS %s", vb);
        }
    } else if (strstr(ua, "Windows NT")) {
        snprintf(os, on, "Windows");
    } else if (strstr(ua, "Macintosh")) {
        snprintf(os, on, "macOS");
    }
}

static void ua_remember(uint8_t kind, const uint8_t key[6], const char *ua) {
    char model[32] = {0};
    char os[20] = {0};
    ua_parse(ua, model, sizeof(model), os, sizeof(os));
    if (!model[0] && !os[0]) return;
    int slot = -1;
    for (int i = 0; i < DEVINFO_MAX; i++) {
        if (s_ua[i].kind == kind && memcmp(s_ua[i].key, key, 6) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < DEVINFO_MAX; i++) {
            if (s_ua[i].kind == 0) { slot = i; break; }
        }
        if (slot < 0) {
            slot = s_ua_next;
            s_ua_next = (s_ua_next + 1) % DEVINFO_MAX;
        }
        memset(&s_ua[slot], 0, sizeof(s_ua[slot]));
        s_ua[slot].kind = kind;
        memcpy(s_ua[slot].key, key, 6);
    }
    if (model[0]) memcpy(s_ua[slot].model, model, 32);
    if (os[0]) memcpy(s_ua[slot].os, os, 20);
}

/* Client ki plain HTTP request (port 80) se User-Agent: Android ka connectivity-check bhi isi mein aata hai */
static void http_sniff(const uint8_t *f, uint16_t len) {
    if (len < 14 + 20 + 20 + 30) return;
    if (f[12] != 0x08 || f[13] != 0x00) return;          /* IPv4 */
    if ((f[14] >> 4) != 4) return;
    int ihl = (f[14] & 0x0F) * 4;
    if (ihl < 20 || f[23] != 6) return;                  /* TCP */
    int t = 14 + ihl;
    if (len < t + 20) return;
    if (f[t + 2] != 0 || f[t + 3] != 80) return;         /* dest port 80 */
    int thl = (f[t + 12] >> 4) * 4;
    if (thl < 20) return;
    int p = t + thl;
    if (len < p + 30) return;
    const char *pl = (const char *)f + p;
    int pn = len - p;
    if (memcmp(pl, "GET ", 4) != 0 && memcmp(pl, "POST", 4) != 0 && memcmp(pl, "HEAD", 4) != 0) return;
    for (int i = 0; i + 12 < pn; i++) {
        if (pl[i] == '\n' && strncasecmp(pl + i + 1, "User-Agent:", 11) == 0) {
            int s = i + 12;
            while (s < pn && pl[s] == ' ') s++;
            int k = 0;
            while (s < pn && pl[s] != '\r' && pl[s] != '\n' && k < (int)sizeof(s_ua_buf) - 1) s_ua_buf[k++] = pl[s++];
            s_ua_buf[k] = 0;
            ua_remember(1, f + 6, s_ua_buf);
            return;
        }
    }
}

/* Repeater ke apne pages par aane wali request ka User-Agent, peer IP se */
static void ua_capture(httpd_req_t *req) {
    char ua[256];
    if (httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua)) != ESP_OK) return;
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return;
    if (ss.ss_family != AF_INET) return;
    uint8_t key[6] = {0};
    memcpy(key, &((struct sockaddr_in *)&ss)->sin_addr.s_addr, 4);
    ua_remember(2, key, ua);
}

/* UA se model na mile to DHCP hostname (jaise moto-g-5G-2023) se model: sirf jaane-pehchane brand ke naam se shuru hone par */
static bool host_model_guess(const char *hn, char *out, size_t n) {
    static const char *pf[] = {"moto", "galaxy", "samsung", "redmi", "poco", "xiaomi", "pixel", "infinix",
                               "tecno", "itel", "realme", "oppo", "vivo", "oneplus", "nokia", "huawei", "honor", "iphone"};
    bool ok = false;
    for (size_t i = 0; i < sizeof(pf) / sizeof(pf[0]); i++) {
        if (strncasecmp(hn, pf[i], strlen(pf[i])) == 0) { ok = true; break; }
    }
    if (!ok || n < 4) return false;
    size_t q = 0;
    for (const char *p = hn; *p && q + 1 < n; p++) {
        uint8_t ch = (uint8_t)*p;
        if (ch == '-' || ch == '_') ch = ' ';
        if (ch < 0x20 || ch > 0x7E) continue;
        if (ch == ' ' && (q == 0 || out[q - 1] == ' ')) continue;
        out[q++] = (char)ch;
    }
    while (q > 0 && out[q - 1] == ' ') q--;
    out[q] = 0;
    return q >= 3;
}

/* MAC (pehle) aur IP (baad mein) se jo mila, model/os/brand mein bhar do; kuch na mile to khali */
static void devinfo_get(const uint8_t mac[6], const uint8_t ip4[4],
                        char *model, size_t mn, char *os, size_t on, char *brand, size_t bn) {
    model[0] = 0; os[0] = 0; brand[0] = 0;
    char raw[32] = {0};
    char ros[20] = {0};
    for (int pass = 0; pass < 2; pass++) {
        uint8_t key[6] = {0};
        if (pass == 0) {
            memcpy(key, mac, 6);
        } else {
            if (ip4[0] == 0 && ip4[1] == 0 && ip4[2] == 0 && ip4[3] == 0) break;
            memcpy(key, ip4, 4);
        }
        for (int i = 0; i < DEVINFO_MAX; i++) {
            if (s_ua[i].kind == (pass == 0 ? 1 : 2) && memcmp(s_ua[i].key, key, 6) == 0) {
                if (!raw[0]) { memcpy(raw, s_ua[i].model, 32); raw[31] = 0; }
                if (!ros[0]) { memcpy(ros, s_ua[i].os, 20); ros[19] = 0; }
            }
        }
    }
    char vc[32];
    if (dhcp_vc_lookup(mac, vc, sizeof(vc))) {
        if (strncmp(vc, "android-dhcp-", 13) == 0) snprintf(ros, sizeof(ros), "Android %.8s", vc + 13);
        else if (!ros[0] && strncmp(vc, "MSFT", 4) == 0) snprintf(ros, sizeof(ros), "Windows");
    }
    snprintf(os, on, "%.19s", ros);
    if (raw[0]) {
        const char *fr = model_friendly(raw);
        if (fr) snprintf(model, mn, "%.30s (%.31s)", fr, raw);
        else snprintf(model, mn, "%.31s", raw);
        { const char *tb = model_brand(raw); snprintf(brand, bn, "%s", tb[0] ? tb : brand_from_model(raw)); }
    }
}

/* ---- Device first-seen / connected-since (RAM only, boot par khali, 16 devices) ---- */
#define SEEN_MAX 16
typedef struct { uint8_t mac[6]; bool used; int64_t first_us; int64_t conn_us; } seen_t;
static seen_t s_seen[SEEN_MAX];
static int s_seen_next = 0;

static void seen_touch(const uint8_t mac[6]) {
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i].used && memcmp(s_seen[i].mac, mac, 6) == 0) { s_seen[i].conn_us = now; return; }
    }
    int slot = -1;
    for (int i = 0; i < SEEN_MAX; i++) {
        if (!s_seen[i].used) { slot = i; break; }
    }
    if (slot < 0) { slot = s_seen_next; s_seen_next = (s_seen_next + 1) % SEEN_MAX; }
    s_seen[slot].used = false;
    memcpy(s_seen[slot].mac, mac, 6);
    s_seen[slot].first_us = now;
    s_seen[slot].conn_us = now;
    s_seen[slot].used = true;
}

/* seconds since first seen / since this connection; -1 agar record nahi */
static void seen_get(const uint8_t mac[6], int *first_ago_s, int *conn_s) {
    *first_ago_s = -1; *conn_s = -1;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i].used && memcmp(s_seen[i].mac, mac, 6) == 0) {
            *first_ago_s = (int)((now - s_seen[i].first_us) / 1000000);
            *conn_s = (int)((now - s_seen[i].conn_us) / 1000000);
            return;
        }
    }
}

#include "oui_table.h"

/* MAC ke pehle 3 bytes se company; locally-administered (private/random) MAC ki company nahi hoti */
static const char *oui_vendor_lookup(const uint8_t mac[6]) {
    if (mac[0] & 0x02) return "Private MAC";
    uint32_t key = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    int lo = 0, hi = OUI_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (OUI_KEYS[mid] == key) return OUI_NAMES[OUI_VEN[mid]];
        if (OUI_KEYS[mid] < key) lo = mid + 1; else hi = mid - 1;
    }
    return "";
}

static void dhcp_sniff(const uint8_t *f, uint16_t len) {
    if (len < 14 + 20 + 8 + 240) return;
    if (f[12] != 0x08 || f[13] != 0x00) return;          /* IPv4 */
    if ((f[14] >> 4) != 4) return;
    int ihl = (f[14] & 0x0F) * 4;
    if (ihl < 20 || f[23] != 17) return;                 /* UDP */
    int u = 14 + ihl;
    if (len < u + 8 + 240) return;
    if (f[u + 2] != 0 || f[u + 3] != 67) return;         /* dest port 67 (DHCP server) */
    const uint8_t *d = f + u + 8;
    int n = len - (u + 8);
    if (d[0] != 1) return;                               /* BOOTREQUEST */
    if (d[236] != 0x63 || d[237] != 0x82 || d[238] != 0x53 || d[239] != 0x63) return;
    int i = 240;
    while (i < n) {
        uint8_t code = d[i];
        if (code == 255) break;
        if (code == 0) { i++; continue; }
        if (i + 1 >= n) break;
        int l = d[i + 1];
        if (i + 2 + l > n) break;
        if (code == 12 && l > 0) {
            char nm[32];
            int k = 0;
            for (int j = 0; j < l && k < 31; j++) {
                uint8_t ch = d[i + 2 + j];
                if (ch >= 0x20 && ch <= 0x7E) nm[k++] = (char)ch;
            }
            nm[k] = 0;
            for (int z = k + 1; z < 32; z++) nm[z] = 0;
            if (k > 0) dhcp_host_store(f + 6, nm);
        }
        if (code == 60 && l > 0) {
            char vcn[32];
            int q = 0;
            for (int j = 0; j < l && q < 31; j++) {
                uint8_t ch = d[i + 2 + j];
                if (ch >= 0x20 && ch <= 0x7E) vcn[q++] = (char)ch;
            }
            vcn[q] = 0;
            for (int z = q + 1; z < 32; z++) vcn[z] = 0;
            if (q > 0) dhcp_vc_store(f + 6, vcn);
        }
        i += 2 + l;
    }
}

/* ---- Website block list: UDP port 53 ki DNS query ka domain (ends-with match) list mein ho to packet drop.
 * List NVS "blocklist" (comma separated) mein, RAM cache se filter chalta hai. DoT/DoH aur TCP DNS is se nahi rukte. ---- */
#define BL_MAX 20
#define BL_LEN 40
static char s_bl[BL_MAX][BL_LEN + 1];
static volatile int s_bl_n = 0;

/* txt (comma/newline/space) se domains nikalta hai; count lautata hai, ghalti par -1 aur *err set */
static int blocklist_parse(const char *txt, char out[][BL_LEN + 1], const char **err) {
    int n = 0;
    char tok[BL_LEN + 2];
    int tl = 0;
    bool toolong = false;
    for (const char *p = txt; ; p++) {
        char c = *p;
        bool sep = (c == 0 || c == ',' || c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == ';');
        if (sep) {
            if (toolong) { *err = "domain too long (max 40 characters)"; return -1; }
            if (tl > 0) {
                if (n >= BL_MAX) { *err = "too many domains (max 20)"; return -1; }
                tok[tl] = 0;
                memcpy(out[n], tok, (size_t)tl + 1);
                n++;
            }
            tl = 0;
            if (c == 0) break;
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!ok) { *err = "only letters, digits, dot and hyphen are allowed in domains"; return -1; }
        if (c == '.' && tl == 0) continue;
        if (tl < BL_LEN) tok[tl++] = c; else toolong = true;
    }
    return n;
}

static void blocklist_apply(char in[][BL_LEN + 1], int n) {
    s_bl_n = 0;
    for (int i = 0; i < n; i++) memcpy(s_bl[i], in[i], BL_LEN + 1);
    s_bl_n = n;
}

static void blocklist_load(void) {
    static char v[BL_MAX * (BL_LEN + 1) + 2];
    static char tmp[BL_MAX][BL_LEN + 1];
    nvs_get_string("blocklist", v, sizeof(v), "");
    const char *err = NULL;
    int n = blocklist_parse(v, tmp, &err);
    if (n > 0) blocklist_apply(tmp, n);
}

static bool dns_blocked(const uint8_t *f, uint16_t len) {
    int bn = s_bl_n;
    if (bn <= 0 || len < 14 + 20 + 8 + 12 + 5) return false;
    if (f[12] != 0x08 || f[13] != 0x00) return false;     /* IPv4 */
    if ((f[14] >> 4) != 4) return false;
    int ihl = (f[14] & 0x0F) * 4;
    if (ihl < 20 || f[23] != 17) return false;            /* UDP */
    int u = 14 + ihl;
    if (len < u + 8 + 12 + 5) return false;
    if (f[u + 2] != 0 || f[u + 3] != 53) return false;    /* dest port 53 */
    const uint8_t *d = f + u + 8;
    int n = len - (u + 8);
    if (d[2] & 0x80) return false;                        /* sirf query, response nahi */
    if (d[4] == 0 && d[5] == 0) return false;            /* QDCOUNT 0 */
    char name[100];
    int nl = 0;
    int i = 12;
    while (i < n) {
        int l = d[i];
        if (l == 0) break;
        if (l > 63 || i + 1 + l > n) return false;
        if (nl + l + 2 >= (int)sizeof(name)) return false;
        if (nl > 0) name[nl++] = '.';
        for (int k = 0; k < l; k++) {
            char c = (char)d[i + 1 + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            name[nl++] = c;
        }
        i += 1 + l;
    }
    name[nl] = 0;
    if (nl == 0) return false;
    for (int b = 0; b < bn; b++) {
        int bl = (int)strlen(s_bl[b]);
        if (bl <= 0 || bl > nl) continue;
        if (strcmp(name + nl - bl, s_bl[b]) != 0) continue;
        if (nl == bl || name[nl - bl - 1] == '.') return true;
    }
    return false;
}

static esp_err_t ap_rx_filter(void *buffer, uint16_t len, void *eb) {
    dhcp_sniff((const uint8_t *)buffer, len);
    http_sniff((const uint8_t *)buffer, len);
    if (dns_blocked((const uint8_t *)buffer, len)) {
        esp_wifi_internal_free_rx_buffer(eb);   /* blocked website ki DNS query - jawab nahi milega */
        return ESP_OK;
    }
    if (!s_guest_filter_on || len < 14) {
        esp_netif_receive(s_ap_netif, buffer, len, eb);
        return ESP_OK;
    }
    uint8_t *f = (uint8_t *)buffer;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", f[6], f[7], f[8], f[9], f[10], f[11]);
    if (whitelist_contains(mac) && wl_in_schedule(mac)) {
        usage_entry_t *ue = usage_find_or_add(f + 6);
        if (ue) {
            ue->total_bytes += len;
            ue->week_bytes += len;
            ue->month_bytes += len;

            int limit_kbps = whitelist_get_limit(mac);
            if (limit_kbps > 0) {
                int64_t now = esp_timer_get_time();
                if (ue->bucket_last_us == 0) ue->bucket_last_us = now;
                int64_t dt = now - ue->bucket_last_us;
                if (dt > 0) {
                    uint64_t refill = (uint64_t)dt * (uint32_t)limit_kbps * 1000ULL / 8ULL / 1000000ULL;
                    uint32_t cap = (uint32_t)limit_kbps * 1000 / 8;  /* ~1 second burst */
                    if (cap < 1600) cap = 1600;   /* kam az kam aik poora packet */
                    uint64_t nt = (uint64_t)ue->bucket_tokens + refill;
                    ue->bucket_tokens = nt > cap ? cap : (uint32_t)nt;
                    ue->bucket_last_us = now;
                }
                if (ue->bucket_tokens < len) {
                    esp_wifi_internal_free_rx_buffer(eb);   /* over the uplink limit - drop this packet */
                    return ESP_OK;
                }
                ue->bucket_tokens -= len;
            }

            int overall_up = get_setting_int("overall_up_kbps", 0, 9999, 0);
            if (overall_up > 0) {
                int64_t now2 = esp_timer_get_time();
                if (s_up_bucket_last_us == 0) s_up_bucket_last_us = now2;
                int64_t dt2 = now2 - s_up_bucket_last_us;
                if (dt2 > 0) {
                    uint64_t refill2 = (uint64_t)dt2 * (uint32_t)overall_up * 1000ULL / 8ULL / 1000000ULL;
                    uint32_t cap2 = (uint32_t)overall_up * 1000 / 8;
                    if (cap2 < 1600) cap2 = 1600;  /* kam az kam aik poora packet */
                    uint64_t nt2 = (uint64_t)s_up_bucket_tokens + refill2;
                    s_up_bucket_tokens = nt2 > cap2 ? cap2 : (uint32_t)nt2;
                    s_up_bucket_last_us = now2;
                }
                if (s_up_bucket_tokens < len) {
                    esp_wifi_internal_free_rx_buffer(eb);  /* over the shared overall uplink cap */
                    return ESP_OK;
                }
                s_up_bucket_tokens -= len;
            }
        }
        esp_netif_receive(s_ap_netif, buffer, len, eb);
        return ESP_OK;
    }
    uint16_t ethertype = ((uint16_t)f[12] << 8) | f[13];
    bool allow = false;
    if (ethertype == 0x0806) {
        allow = true; /* ARP - keeps local networking alive, never reaches the internet */
    } else if (ethertype == 0x0800 && len >= 34) {
        uint32_t dest;
        memcpy(&dest, f + 30, 4); /* IPv4 destination address */
        if (dest == 0xFFFFFFFFu || (dest & s_subnet_mask) == s_subnet_base) allow = true;
    }
    if (allow) {
        esp_netif_receive(s_ap_netif, buffer, len, eb);
    } else {
        esp_wifi_internal_free_rx_buffer(eb);
    }
    return ESP_OK;
}

/* ---- Whole-repeater download counter + optional overall download cap.
 * Registered on the STA (upstream) interface. Unlike ap_rx_filter, this
 * ALWAYS forwards every packet unchanged when no cap is set (default) - it
 * only ever drops packets when the admin has explicitly set a download cap
 * greater than 0, so normal internet sharing is completely unaffected
 * unless this is turned on. Must be re-armed after every STA (re)connect,
 * because ESP-IDF's own networking glue re-registers its own default
 * receive callback each time the STA reconnects. */
static esp_err_t sta_rx_counter(void *buffer, uint16_t len, void *eb) {
    s_down_usage.total_bytes += len;
    s_down_usage.week_bytes += len;
    s_down_usage.month_bytes += len;

    int overall_down = get_setting_int("overall_down_kbps", 0, 9999, 0);
    if (overall_down > 0) {
        int64_t now = esp_timer_get_time();
        if (s_down_bucket_last_us == 0) s_down_bucket_last_us = now;
        int64_t dt = now - s_down_bucket_last_us;
        if (dt > 0) {
            uint64_t refill = (uint64_t)dt * (uint32_t)overall_down * 1000ULL / 8ULL / 1000000ULL;
            uint32_t cap = (uint32_t)overall_down * 1000 / 8;
            if (cap < 1600) cap = 1600;   /* kam az kam aik poora packet */
            uint64_t nt = (uint64_t)s_down_bucket_tokens + refill;
            s_down_bucket_tokens = nt > cap ? cap : (uint32_t)nt;
            s_down_bucket_last_us = now;
        }
        if (s_down_bucket_tokens < len) {
            esp_wifi_internal_free_rx_buffer(eb);   /* over the overall download cap */
            return ESP_OK;
        }
        s_down_bucket_tokens -= len;
    }
    esp_netif_receive(s_sta_netif, buffer, len, eb);
    return ESP_OK;
}

/* ---- Deauth / disassoc monitor: passive promiscuous management-frame sniff.
 * Adds counters only, never transmits anything itself. */

static void wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;
    uint8_t *p = pkt->payload;
    uint8_t fc0 = p[0];
    uint8_t ftype = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    if (ftype != 0 || (subtype != 0x0C && subtype != 0x0A)) return; /* mgmt: deauth=0xC, disassoc=0xA */

    uint8_t *addr1 = p + 4, *addr2 = p + 10, *addr3 = p + 16;
    uint8_t self_ap[6] = {0}, self_sta[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, self_ap);
    esp_wifi_get_mac(WIFI_IF_STA, self_sta);
    bool ours = (memcmp(addr1, self_ap, 6) == 0) || (memcmp(addr3, self_ap, 6) == 0) ||
                (memcmp(addr1, self_sta, 6) == 0) || (memcmp(addr3, self_sta, 6) == 0);

    int idx = s_deauth_head;
    s_deauth_ring[idx].t = esp_timer_get_time();
    s_deauth_ring[idx].type = subtype;
    memcpy(s_deauth_ring[idx].src, addr2, 6);
    memcpy(s_deauth_ring[idx].dst, addr1, 6);
    s_deauth_ring[idx].rssi = pkt->rx_ctrl.rssi;
    s_deauth_ring[idx].ours = ours;
    s_deauth_head = (s_deauth_head + 1) % DEAUTH_RING;
    if (s_deauth_count < DEAUTH_RING) s_deauth_count++;
    s_deauth_total++;
}

static void deauth_monitor_apply(bool on) {
    if (on) {
        wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
        esp_wifi_set_promiscuous_filter(&filt);
        esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb);
        esp_wifi_set_promiscuous(true);
    } else {
        esp_wifi_set_promiscuous(false);
    }
}


/* ---- upstream failover: primary (sta_ssid/sta_pass) <-> backup (sta_ssid2/sta_pass2) ---- */
#define STA_FAILOVER_FAILS 5
static int s_sta_fail = 0;
static bool s_on_backup = false;

/* Sirf event task se chalta hai, is liye static buffers (event task ka stack chhota hai). */
static bool sta_apply_network(bool backup) {
    static char ssid[33], pass[65];
    static wifi_config_t c;
    nvs_get_string(backup ? "sta_ssid2" : "sta_ssid", ssid, sizeof(ssid), "");
    nvs_get_string(backup ? "sta_pass2" : "sta_pass", pass, sizeof(pass), "");
    if (ssid[0] == 0) return false;
    memset(&c, 0, sizeof(c));
    strncpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid));
    strncpy((char *)c.sta.password, pass, sizeof(c.sta.password));
    return esp_wifi_set_config(WIFI_IF_STA, &c) == ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            /* ESP-IDF's own netif glue re-registers its default STA receive
             * callback on every connect, silently overwriting ours - so we
             * must re-arm our download counter/cap right after, every time. */
            esp_err_t re = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, sta_rx_counter);
            ESP_LOGI(TAG, "Download counter armed: %s", esp_err_to_name(re));
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *dc = (wifi_event_sta_disconnected_t *)data;
            s_sta_connected = false;
            strcpy(s_sta_ip, "0.0.0.0");
            int idx = s_discon_head;
            s_discon_ring[idx].t = esp_timer_get_time();
            s_discon_ring[idx].reason = dc ? dc->reason : 0;
            s_discon_head = (s_discon_head + 1) % DISCON_RING;
            if (s_discon_count < DISCON_RING) s_discon_count++;
            s_discon_total++;
            ESP_LOGW(TAG, "Upstream WiFi disconnected (reason %d), retrying...", dc ? dc->reason : 0);
            if (++s_sta_fail >= STA_FAILOVER_FAILS) {
                s_sta_fail = 0;
                if (sta_apply_network(!s_on_backup)) {
                    s_on_backup = !s_on_backup;
                    ESP_LOGW(TAG, "Upstream failover: switching to %s network", s_on_backup ? "BACKUP" : "PRIMARY");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
            seen_touch(ev->mac);
            char mac[18];
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     ev->mac[0], ev->mac[1], ev->mac[2], ev->mac[3], ev->mac[4], ev->mac[5]);
            if (!whitelist_contains(mac)) {
                if (s_guest_filter_on) {
                    ESP_LOGI(TAG, "Guest connected (local access only, no internet): %s", mac);
                } else {
                    ESP_LOGW(TAG, "Rejecting non-whitelisted client %s", mac);
                    esp_wifi_deauth_sta(ev->aid);
                }
            } else {
                ESP_LOGI(TAG, "Client connected: %s", mac);
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        s_sta_since_us = esp_timer_get_time();
        s_sta_fail = 0;
        ESP_LOGI(TAG, "Upstream IP %s - enabling NAT", s_sta_ip);
        esp_netif_napt_enable(s_ap_netif);

        if (!s_time_synced) {
            s_time_synced = true; /* only ever try once per boot */
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP time sync started (needed for weekly/monthly usage totals)");
        }
    }
}

/* AP address + the DNS server that the AP's DHCP server hands to clients.
 * Without an explicit DNS offer the AP advertises itself as DNS, but the ESP32
 * runs no resolver - that is the classic "connected, no internet" symptom. */
static void configure_ap_network(void) {
    update_subnet_cache();
    char ip[16], dns[16];
    get_setting("ap_ip", ip, sizeof(ip));
    get_setting("dns1", dns, sizeof(dns));

    esp_netif_ip_info_t info = { 0 };
    info.ip.addr = esp_ip4addr_aton(ip);
    info.gw.addr = info.ip.addr;
    info.netmask.addr = esp_ip4addr_aton("255.255.255.0");

    esp_netif_dns_info_t dnsinfo = { 0 };
    dnsinfo.ip.u_addr.ip4.addr = esp_ip4addr_aton(dns);
    dnsinfo.ip.type = ESP_IPADDR_TYPE_V4;

    uint8_t offer_dns = 0x02;   /* OFFER_DNS bit of the (private) lwip dhcps option enum */

    esp_netif_dhcps_stop(s_ap_netif);
    esp_err_t e1 = esp_netif_set_ip_info(s_ap_netif, &info);
    esp_err_t e2 = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns));
    esp_err_t e3 = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dnsinfo);
    esp_err_t e4 = esp_netif_dhcps_start(s_ap_netif);
    ESP_LOGI(TAG, "AP %s, DNS %s (ip:%s opt:%s dns:%s start:%s)", ip, dns,
             esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3), esp_err_to_name(e4));
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    char hostname[40];
    get_setting("hostname", hostname, sizeof(hostname));
    esp_netif_set_hostname(s_sta_netif, hostname);

    char sip[16], sgw[16], smask[16];
    get_setting("sta_static_ip", sip, sizeof(sip));
    get_setting("sta_gateway", sgw, sizeof(sgw));
    get_setting("sta_netmask", smask, sizeof(smask));
    if (sip[0] != '\0' && sgw[0] != '\0') {
        esp_netif_dhcpc_stop(s_sta_netif);
        esp_netif_ip_info_t sinfo = { 0 };
        sinfo.ip.addr = esp_ip4addr_aton(sip);
        sinfo.gw.addr = esp_ip4addr_aton(sgw);
        sinfo.netmask.addr = esp_ip4addr_aton(smask[0] ? smask : "255.255.255.0");
        esp_err_t se = esp_netif_set_ip_info(s_sta_netif, &sinfo);
        ESP_LOGI(TAG, "Static upstream IP %s via gw %s: %s", sip, sgw, esp_err_to_name(se));
    } else {
        ESP_LOGI(TAG, "Upstream IP: automatic (DHCP)");
    }

    configure_ap_network();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    char ap_ssid[33], ap_pass[65], sta_ssid[33], sta_pass[65], sta_mac[20];
    get_setting("ap_ssid", ap_ssid, sizeof(ap_ssid));
    get_setting("ap_pass", ap_pass, sizeof(ap_pass));
    get_setting("sta_mac", sta_mac, sizeof(sta_mac));
    nvs_get_string("sta_ssid", sta_ssid, sizeof(sta_ssid), "");
    nvs_get_string("sta_pass", sta_pass, sizeof(sta_pass), "");

    wifi_config_t ap_config = { 0 };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    strncpy((char *)ap_config.ap.password, ap_pass, sizeof(ap_config.ap.password));
    ap_config.ap.max_connection = get_setting_int("ap_max_conn", 1, 10, 8);
    ap_config.ap.channel = get_setting_int("ap_channel", 1, 13, 1);
    ap_config.ap.ssid_hidden = get_setting_int("ap_hidden", 0, 1, 0);
    ap_config.ap.authmode = strlen(ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    wifi_config_t sta_config = { 0 };
    strncpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, sta_pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    uint8_t mac[6];
    if (sta_mac[0] != '\0' && parse_mac(sta_mac, mac) && !(mac[0] & 1)) {
        esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, mac);
        ESP_LOGI(TAG, "Custom STA MAC %s: %s", sta_mac, esp_err_to_name(e));
    }

    if (esp_wifi_set_config(WIFI_IF_AP, &ap_config) != ESP_OK) {
        ESP_LOGW(TAG, "AP config rejected - falling back to an open network");
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }
    if (strlen(sta_ssid) > 0) {
        if (esp_wifi_set_config(WIFI_IF_STA, &sta_config) != ESP_OK) ESP_LOGW(TAG, "Upstream WiFi config rejected - fix it in Upstream WiFi");
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    int dbm = get_setting_int("tx_power", 2, 20, 20);
    esp_wifi_set_max_tx_power((int8_t)(dbm * 4));

    char gf[4]; get_setting("guest_filter", gf, sizeof(gf));
    s_guest_filter_on = (gf[0] == '1');
    esp_err_t rxe = esp_wifi_internal_reg_rxcb(WIFI_IF_AP, ap_rx_filter);
    ESP_LOGI(TAG, "Guest filter %s (rxcb: %s)", s_guest_filter_on ? "ON" : "OFF", esp_err_to_name(rxe));

    char dm[4]; get_setting("deauth_monitor", dm, sizeof(dm));
    deauth_monitor_apply(dm[0] == '1');

    ESP_LOGI(TAG, "AP up: SSID=%s hostname=%s", ap_ssid, hostname);
}

/* ================= Hardware reset button (BOOT = GPIO0) =================
 * Hold ~8 s -> LED starts blinking fast -> release -> settings erased, reboot.
 * We MUST wait for release before restarting: GPIO0 low at reset means
 * "enter download mode" and the firmware would not start. */

static int tcp_probe(const char *ip, int port, int timeout_ms); /* defined later, used here for speed-history sampling */

/* ---- backup par chalte hue har ~5 min baad primary wapas try, sirf agar primary scan mein dikhe ---- */
static volatile bool s_pr_busy = false;
static void primary_return_task(void *arg) {
    static char ssid[33];
    nvs_get_string("sta_ssid", ssid, sizeof(ssid), "");
    if (ssid[0] && s_on_backup && s_sta_connected && s_scan_state != 1) {
        s_scan_state = 1;                       /* admin ke scan se takrao se bachao */
        wifi_scan_config_t sc = { 0 };
        sc.ssid = (uint8_t *)ssid;
        esp_err_t e = esp_wifi_scan_start(&sc, true);
        uint16_t num = 0;
        if (e == ESP_OK) esp_wifi_scan_get_ap_num(&num);
        esp_wifi_clear_ap_list();
        s_scan_state = 0;
        if (e == ESP_OK && num > 0 && s_on_backup && s_sta_connected && sta_apply_network(false)) {
            s_on_backup = false;
            s_sta_fail = 0;
            ESP_LOGW(TAG, "Primary upstream visible again - switching back from BACKUP");
            esp_wifi_disconnect();              /* DISCONNECTED handler dobara connect karta hai */
        }
    }
    s_pr_busy = false;
    vTaskDelete(NULL);
}

static void button_task(void *arg) {
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << RESET_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led);
    gpio_set_level(LED_GPIO, 0);

    int held_ms = 0;
    uint32_t tick = 0;
    while (1) {
        tick++;
        if (tick % 600 == 0) {          /* every ~60s */
            usage_housekeeping();
            int hrs = get_setting_int("auto_reboot_hours", 0, 168, 0);
            if (hrs > 0 && (esp_timer_get_time() - s_boot_time_us) >= (int64_t)hrs * 3600LL * 1000000LL) {
                ESP_LOGW(TAG, "Scheduled auto-restart (%d h) reached", hrs);
                esp_restart();
            }
        }
        if (tick % 3000 == 0) {         /* every ~5 min */
            usage_persist_all();
        }
        if (tick % 3000 == 0 && s_on_backup && s_sta_connected && !s_pr_busy) {   /* every ~5 min, only while on backup */
            s_pr_busy = true;
            if (xTaskCreate(primary_return_task, "primret", 4096, NULL, 4, NULL) != pdPASS) s_pr_busy = false;
        }
        if (tick % 9000 == 0 && s_sta_connected) {   /* every ~15 min */
            int ms = tcp_probe("1.1.1.1", 443, 2000);
            int idx = s_speedhist_head;
            s_speedhist[idx].t = esp_timer_get_time();
            s_speedhist[idx].ms = ms >= 0 ? ms : 0;
            s_speedhist[idx].ok = ms >= 0;
            s_speedhist_head = (s_speedhist_head + 1) % SPEEDHIST_MAX;
            if (s_speedhist_count < SPEEDHIST_MAX) s_speedhist_count++;
        }
        if (gpio_get_level(RESET_BTN_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= RESET_HOLD_MS) {
                ESP_LOGW(TAG, "Factory reset armed - release the button now");
                int level = 0;
                while (gpio_get_level(RESET_BTN_GPIO) == 0) {
                    level = !level;
                    gpio_set_level(LED_GPIO, level);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(300));
                wipe_settings();
                ESP_LOGW(TAG, "Settings erased - restarting with defaults");
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ================= HTTP helpers ================= */

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len) {
    int total = 0;
    while (total < (int)buf_len - 1) {
        int r = httpd_req_recv(req, buf + total, buf_len - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
    free(out);
    cJSON_Delete(root);
    return r;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *err) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", ok);
    if (!ok) cJSON_AddStringToObject(r, "error", err ? err : "error");
    return send_json(req, r);
}

static void restart_soon(void) {
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
}

/* ================= HTTP handlers ================= */

static esp_err_t root_get_handler(httpd_req_t *req) {
    ua_capture(req);
    if (check_session(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/admin");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, login_html_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t admin_get_handler(httpd_req_t *req) {
    ua_capture(req);
    if (!check_session(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, admin_html_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t login_post_handler(httpd_req_t *req) {
    char buf[256];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    bool ok = false;
    if (j) {
        cJSON *user = cJSON_GetObjectItem(j, "username");
        cJSON *pass = cJSON_GetObjectItem(j, "password");
        char admin_pass[40];
        get_setting("admin_pass", admin_pass, sizeof(admin_pass));
        if (cJSON_IsString(user) && cJSON_IsString(pass) &&
            secure_equal(user->valuestring, ADMIN_USER) &&
            secure_equal(pass->valuestring, admin_pass)) {
            ok = true;
        }
        cJSON_Delete(j);
    }
    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(1000));   /* slows down password guessing */
        httpd_resp_set_status(req, "401 Unauthorized");
        return send_result(req, false, "wrong password");
    }
    generate_session_token(s_session_token, sizeof(s_session_token));
    char cookie_hdr[96];
    snprintf(cookie_hdr, sizeof(cookie_hdr), "session=%s; Path=/; HttpOnly", s_session_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_hdr);
    return send_result(req, true, NULL);
}

static esp_err_t logout_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    s_session_token[0] = '\0';
    httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; Max-Age=0");
    return send_result(req, true, NULL);
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    ua_capture(req);
    if (require_session_api(req) != ESP_OK) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "fw", FW_VERSION);
    cJSON_AddBoolToObject(root, "sta_connected", s_sta_connected);
    cJSON_AddStringToObject(root, "sta_ip", s_sta_ip);
    cJSON_AddNumberToObject(root, "uptime_s", (double)((esp_timer_get_time() - s_boot_time_us) / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());

    char v[40];
    nvs_get_string("sta_ssid", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_ssid", v);
    cJSON_AddBoolToObject(root, "on_backup", s_on_backup);
    nvs_get_string("sta_ssid2", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_ssid2", v);
    get_setting("ap_ip", v, sizeof(v));
    cJSON_AddStringToObject(root, "ap_ip", v);

    char wlfull[WL_BUF];
    whitelist_get(wlfull, sizeof(wlfull));
    cJSON_AddBoolToObject(root, "wl_active", strlen(wlfull) > 0);

    cJSON *clients = cJSON_CreateArray();
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        esp_netif_pair_mac_ip_t pairs[10] = { 0 };
        int count = sta_list.num > 10 ? 10 : sta_list.num;
        for (int i = 0; i < count; i++) {
            memcpy(pairs[i].mac, sta_list.sta[i].mac, 6);
        }
        esp_netif_dhcps_get_clients_by_mac(s_ap_netif, count, pairs);

        for (int i = 0; i < count; i++) {
            char mac[18];
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     sta_list.sta[i].mac[0], sta_list.sta[i].mac[1], sta_list.sta[i].mac[2],
                     sta_list.sta[i].mac[3], sta_list.sta[i].mac[4], sta_list.sta[i].mac[5]);
            char ipstr[16];
            snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&pairs[i].ip));
            char name[64] = "";
            whitelist_get_name(mac, name, sizeof(name));
            if (!name[0]) label_get(mac, name, sizeof(name));
            if (!name[0]) dhcp_host_lookup(sta_list.sta[i].mac, name, sizeof(name));

            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "mac", mac);
            cJSON_AddStringToObject(c, "ip", ipstr);
            cJSON_AddStringToObject(c, "name", name);
            cJSON_AddStringToObject(c, "vendor", oui_vendor_lookup(sta_list.sta[i].mac));
            uint8_t ip4[4];
            memcpy(ip4, &pairs[i].ip.addr, 4);
            char dm[80], dos[24], dbr[16];
            devinfo_get(sta_list.sta[i].mac, ip4, dm, sizeof(dm), dos, sizeof(dos), dbr, sizeof(dbr));
            if (!dm[0]) {
                char hn[64] = "";
                if (dhcp_host_lookup(sta_list.sta[i].mac, hn, sizeof(hn)) && host_model_guess(hn, dm, sizeof(dm))) {
                    if (!dbr[0]) snprintf(dbr, sizeof(dbr), "%s", brand_from_model(dm));
                }
            }
            cJSON_AddStringToObject(c, "model", dm);
            int sfa, scn;
            seen_get(sta_list.sta[i].mac, &sfa, &scn);
            cJSON_AddNumberToObject(c, "first_ago_s", sfa);
            cJSON_AddNumberToObject(c, "conn_s", scn);
            cJSON_AddStringToObject(c, "os", dos);
            cJSON_AddStringToObject(c, "brand", dbr);
            cJSON_AddNumberToObject(c, "rssi", sta_list.sta[i].rssi);
            cJSON_AddBoolToObject(c, "authorized", whitelist_contains(mac) && wl_in_schedule(mac));
            cJSON_AddItemToArray(clients, c);
        }
    }
    cJSON_AddItemToObject(root, "clients", clients);

    cJSON *wl = cJSON_CreateArray();
    char list[WL_BUF];
    whitelist_get(list, sizeof(list));
    if (strlen(list) > 0) {
        char *copy = strdup(list);
        if (copy) {
            char *tok = strtok(copy, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                char m[20], n[64]; int lim = 0;
                split_entry3(tok, m, sizeof(m), n, sizeof(n), &lim);
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "mac", m);
                cJSON_AddStringToObject(e, "name", n);
                cJSON_AddNumberToObject(e, "limit_kbps", lim);
                const char *sb1 = strchr(tok, '|');
                const char *sb2 = sb1 ? strchr(sb1 + 1, '|') : NULL;
                const char *sb3 = sb2 ? strchr(sb2 + 1, '|') : NULL;
                cJSON_AddStringToObject(e, "sched", sb3 ? sb3 + 1 : "");
                cJSON_AddItemToArray(wl, e);
                tok = strtok(NULL, ",");
            }
            free(copy);
        }
    }
    cJSON_AddItemToObject(root, "whitelist", wl);

    cJSON_AddNumberToObject(root, "session_uptime_s", s_sta_connected ? (double)((esp_timer_get_time() - s_sta_since_us) / 1000000) : 0);
    cJSON_AddNumberToObject(root, "discon_total", s_discon_total);
    cJSON *dlog = cJSON_CreateArray();
    int dn = s_discon_count;
    int64_t now2 = esp_timer_get_time();
    for (int i = 0; i < dn; i++) {
        int idx = ((s_discon_head - 1 - i) % DISCON_RING + DISCON_RING) % DISCON_RING;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ago_s", (double)((now2 - s_discon_ring[idx].t) / 1000000));
        cJSON_AddNumberToObject(o, "reason", s_discon_ring[idx].reason);
        cJSON_AddItemToArray(dlog, o);
    }
    cJSON_AddItemToObject(root, "discon_log", dlog);
    return send_json(req, root);
}

static esp_err_t whitelist_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[320];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (!j) return send_result(req, false, "invalid data");

    cJSON *action = cJSON_GetObjectItem(j, "action");
    cJSON *mac = cJSON_GetObjectItem(j, "mac");
    cJSON *name = cJSON_GetObjectItem(j, "name");
    bool ok = true;
    const char *err = NULL;

    uint8_t m[6];
    if (!cJSON_IsString(action) || !cJSON_IsString(mac) || !parse_mac(mac->valuestring, m)) {
        ok = false;
        err = "MAC must look like AA:BB:CC:DD:EE:FF";
    } else {
        char norm[18];
        snprintf(norm, sizeof(norm), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        if (strcmp(action->valuestring, "add") == 0) {
            cJSON *limit = cJSON_GetObjectItem(j, "limit_kbps");
            int lim = cJSON_IsNumber(limit) ? limit->valueint : 0;
            cJSON *sc = cJSON_GetObjectItem(j, "sched");
            const char *scs = cJSON_IsString(sc) ? sc->valuestring : "";
            int sa, sb;
            if (scs[0] && !sched_parse(scs, &sa, &sb)) {
                ok = false;
                err = "time window must look like 2000-2300";
            } else if (!whitelist_add(norm, cJSON_IsString(name) ? name->valuestring : "", lim, scs)) {
                ok = false;
                err = "whitelist is full";
            }
        } else if (strcmp(action->valuestring, "remove") == 0) {
            whitelist_remove(norm);
        } else if (strcmp(action->valuestring, "label") == 0) {
            char cl[32];
            size_t cq = 0;
            const char *raw = cJSON_IsString(name) ? name->valuestring : "";
            for (const char *q = raw; *q && cq < 31; q++) {
                uint8_t ch = (uint8_t)*q;
                if (ch < 0x20 || ch > 0x7E) continue;
                if (ch == '|' || ch == '=' || ch == ',') ch = ' ';
                if (ch == ' ' && (cq == 0 || cl[cq - 1] == ' ')) continue;
                cl[cq++] = (char)ch;
            }
            while (cq > 0 && cl[cq - 1] == ' ') cq--;
            cl[cq] = 0;
            if (!label_set(norm, cl)) { ok = false; err = "name list is full"; }
        }
    }
    cJSON_Delete(j);
    return send_result(req, ok, err);
}

static esp_err_t wifi_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    static char buf[600];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (!j) return send_result(req, false, "invalid data");

    cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
    cJSON *pass = cJSON_GetObjectItem(j, "password");
    bool ok = true;
    const char *err = NULL;
    if (cJSON_IsString(ssid) && strlen(ssid->valuestring) > 32) { ok = false; err = "WiFi name is too long (max 32)"; }
    if (ok && cJSON_IsString(pass)) {
        size_t pl = strlen(pass->valuestring);
        if (pl != 0 && (pl < 8 || pl > 63)) { ok = false; err = "password must be 8-63 characters (blank = open network)"; }
    }
    cJSON *ssid2 = cJSON_GetObjectItem(j, "ssid2");
    cJSON *pass2 = cJSON_GetObjectItem(j, "password2");
    if (ok && cJSON_IsString(ssid2) && strlen(ssid2->valuestring) > 32) { ok = false; err = "Backup WiFi name is too long (max 32)"; }
    if (ok && cJSON_IsString(pass2)) {
        size_t pl2 = strlen(pass2->valuestring);
        if (pl2 != 0 && (pl2 < 8 || pl2 > 63)) { ok = false; err = "backup password must be 8-63 characters (blank = open network)"; }
    }
    if (ok) {
        if (cJSON_IsString(ssid2)) nvs_set_string("sta_ssid2", ssid2->valuestring);
        if (cJSON_IsString(pass2)) nvs_set_string("sta_pass2", pass2->valuestring);
        if (cJSON_IsString(ssid)) nvs_set_string("sta_ssid", ssid->valuestring);
        if (cJSON_IsString(pass)) nvs_set_string("sta_pass", pass->valuestring);
    }
    cJSON_Delete(j);
    send_result(req, ok, err);
    if (ok) restart_soon();
    return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    for (size_t i = 0; i < N_SETTINGS; i++) {
        char v[136];   /* generous margin for the longest allowed setting value */
        if (strcmp(SETTINGS[i].type, "password_keep") == 0) v[0] = '\0';
        else get_setting(SETTINGS[i].key, v, sizeof(v));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", SETTINGS[i].key);
        cJSON_AddStringToObject(o, "label", SETTINGS[i].label);
        cJSON_AddStringToObject(o, "type", SETTINGS[i].type);
        cJSON_AddStringToObject(o, "value", v);
        cJSON_AddStringToObject(o, "def", SETTINGS[i].def);
        cJSON_AddStringToObject(o, "hint", SETTINGS[i].hint);
        cJSON_AddStringToObject(o, "group", SETTINGS[i].group);
        cJSON_AddItemToArray(items, o);
    }
    cJSON_AddItemToObject(root, "items", items);
    return send_json(req, root);
}

static esp_err_t settings_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char *buf = malloc(1536);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
    read_body(req, buf, 1536);
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return send_result(req, false, "invalid data");

    char err[160] = "";
    bool ok = true;
    for (size_t i = 0; i < N_SETTINGS && ok; i++) {
        cJSON *it = cJSON_GetObjectItem(j, SETTINGS[i].key);
        if (!cJSON_IsString(it)) continue;
        if (strcmp(SETTINGS[i].key, "admin_pass") == 0 && it->valuestring[0] == '\0') continue;
        char why[80] = "";
        if (!validate_setting(SETTINGS[i].key, it->valuestring, why, sizeof(why))) {
            snprintf(err, sizeof(err), "%.50s: %.80s", SETTINGS[i].label, why);
            ok = false;
        }
    }
    if (ok) {
        for (size_t i = 0; i < N_SETTINGS; i++) {
            cJSON *it = cJSON_GetObjectItem(j, SETTINGS[i].key);
            if (!cJSON_IsString(it)) continue;
            if (strcmp(SETTINGS[i].key, "admin_pass") == 0 && it->valuestring[0] == '\0') continue;
            nvs_set_string(SETTINGS[i].key, it->valuestring);
        }
    }
    cJSON_Delete(j);
    send_result(req, ok, err);
    if (ok) restart_soon();
    return ESP_OK;
}

static esp_err_t config_export_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    char v[WL_BUF];
    for (size_t i = 0; i < N_SETTINGS; i++) {
        get_setting(SETTINGS[i].key, v, sizeof(v));
        cJSON_AddStringToObject(root, SETTINGS[i].key, v);
    }
    nvs_get_string("sta_ssid", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_ssid", v);
    nvs_get_string("sta_pass", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_pass", v);
    nvs_get_string("sta_ssid2", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_ssid2", v);
    nvs_get_string("sta_pass2", v, sizeof(v), "");
    cJSON_AddStringToObject(root, "sta_pass2", v);
    whitelist_get(v, sizeof(v));
    cJSON_AddStringToObject(root, "whitelist", v);
    static char bl_exp[BL_MAX * (BL_LEN + 1) + 2];
    nvs_get_string("blocklist", bl_exp, sizeof(bl_exp), "");
    cJSON_AddStringToObject(root, "blocklist", bl_exp);

    char *out = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t config_import_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char *buf = malloc(4096);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
    read_body(req, buf, 4096);
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return send_result(req, false, "not valid JSON");

    char err[160] = "";
    bool ok = true;
    for (size_t i = 0; i < N_SETTINGS && ok; i++) {
        cJSON *it = cJSON_GetObjectItem(j, SETTINGS[i].key);
        if (!cJSON_IsString(it)) continue;
        char why[80] = "";
        if (!validate_setting(SETTINGS[i].key, it->valuestring, why, sizeof(why))) {
            snprintf(err, sizeof(err), "%.50s: %.80s", SETTINGS[i].label, why);
            ok = false;
        }
    }
    cJSON *wl = cJSON_GetObjectItem(j, "whitelist");
    if (ok && cJSON_IsString(wl) && strlen(wl->valuestring) >= WL_BUF) { ok = false; snprintf(err, sizeof(err), "whitelist too long"); }
    static char bl_tmp[BL_MAX][BL_LEN + 1];
    int bl_n = -1;
    cJSON *blj = cJSON_GetObjectItem(j, "blocklist");
    if (ok && cJSON_IsString(blj)) {
        const char *berr = NULL;
        bl_n = blocklist_parse(blj->valuestring, bl_tmp, &berr);
        if (bl_n < 0) { ok = false; snprintf(err, sizeof(err), "blocklist: %.100s", berr ? berr : "invalid list"); }
    }
    if (ok) {
        for (size_t i = 0; i < N_SETTINGS; i++) {
            cJSON *it = cJSON_GetObjectItem(j, SETTINGS[i].key);
            if (cJSON_IsString(it)) nvs_set_string(SETTINGS[i].key, it->valuestring);
        }
        const char *extra[] = {"sta_ssid", "sta_pass", "whitelist"};
        for (int i = 0; i < 3; i++) {
            cJSON *it = cJSON_GetObjectItem(j, extra[i]);
            if (cJSON_IsString(it) && strlen(it->valuestring) < (size_t)(i == 0 ? 33 : i == 1 ? 64 : WL_BUF) && !(i == 1 && it->valuestring[0] && strlen(it->valuestring) < 8)) nvs_set_string(extra[i], it->valuestring);
        }
        const char *extra2[] = {"sta_ssid2", "sta_pass2"};
        for (int i = 0; i < 2; i++) {
            cJSON *it = cJSON_GetObjectItem(j, extra2[i]);
            if (cJSON_IsString(it) && strlen(it->valuestring) < (size_t)(i == 0 ? 33 : 64) && !(i == 1 && it->valuestring[0] && strlen(it->valuestring) < 8)) nvs_set_string(extra2[i], it->valuestring);
        }
        if (bl_n >= 0) {
            static char bl_csv[BL_MAX * (BL_LEN + 1) + 2];
            bl_csv[0] = 0;
            for (int i = 0; i < bl_n; i++) {
                if (i) strncat(bl_csv, ",", sizeof(bl_csv) - strlen(bl_csv) - 1);
                strncat(bl_csv, bl_tmp[i], sizeof(bl_csv) - strlen(bl_csv) - 1);
            }
            nvs_set_string("blocklist", bl_csv);   /* import ke baad reboot, blocklist_load() boot par uthata hai */
        }
    }
    cJSON_Delete(j);
    send_result(req, ok, err);
    if (ok) restart_soon();
    return ESP_OK;
}

/* ================= Guest "Request access": guest naam bhejta hai, admin Approve/Reject karta hai ================= */
#define REQ_MAX 8
typedef struct { uint8_t mac[6]; char name[32]; bool used; } access_req_t;
static access_req_t s_req[REQ_MAX];   /* sirf RAM, boot par khali */

/* request bhejne wale ka MAC: peer IP se DHCP lease table mein dhoondh kar (guest se MAC nahi manga jata) */
static int s_peer_dbg, s_peer_fam, s_peer_n;
static bool peer_mac(httpd_req_t *req, uint8_t mac_out[6]) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return false;
    struct sockaddr_storage ss;
    socklen_t sln = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sln) != 0) { s_peer_dbg = 1; return false; }
    s_peer_fam = ss.ss_family;
    uint32_t ip = 0;
    if (ss.ss_family == AF_INET) {
        ip = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
#if LWIP_IPV6
    } else if (ss.ss_family == AF_INET6) {
        const uint8_t *a6 = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        bool mapped = (a6[10] == 0xff && a6[11] == 0xff);
        for (int q = 0; q < 10; q++) if (a6[q] != 0) mapped = false;
        if (mapped) memcpy(&ip, a6 + 12, 4);
#endif
    }
    if (ip == 0) { s_peer_dbg = 2; return false; }
    
    wifi_sta_list_t stl;
    if (esp_wifi_ap_get_sta_list(&stl) != ESP_OK) { s_peer_dbg = 3; return false; }
    s_peer_n = stl.num;
    esp_netif_pair_mac_ip_t pairs[10] = { 0 };
    int count = stl.num > 10 ? 10 : stl.num;
    for (int i = 0; i < count; i++) memcpy(pairs[i].mac, stl.sta[i].mac, 6);
    esp_netif_dhcps_get_clients_by_mac(s_ap_netif, count, pairs);
    for (int i = 0; i < count; i++) {
        if (pairs[i].ip.addr != 0 && pairs[i].ip.addr == ip) { memcpy(mac_out, stl.sta[i].mac, 6); return true; }
    }
    s_peer_dbg = 4;
    return false;
}

static esp_err_t request_post_handler(httpd_req_t *req) {
    char buf[160];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (!j) return send_result(req, false, "invalid data");
    cJSON *nm = cJSON_GetObjectItem(j, "name");
    char name[32];
    int k = 0;
    if (cJSON_IsString(nm)) {
        for (const char *p = nm->valuestring; *p && k < 31; p++) {
            uint8_t ch = (uint8_t)*p;
            if (ch >= 0x20 && ch <= 0x7E && ch != ',' && ch != '|') name[k++] = (char)ch;
        }
    }
    name[k] = 0;
    cJSON_Delete(j);
    if (k == 0) return send_result(req, false, "please enter your name");
    uint8_t m[6];
    if (!peer_mac(req, m)) {
        ESP_LOGW(TAG, "request: peer_mac failed (code %d, family %d, stations %d)", s_peer_dbg, s_peer_fam, s_peer_n);
        return send_result(req, false, "could not identify your device - connect to the repeater's WiFi and try again");
    }
    char norm[18];
    snprintf(norm, sizeof(norm), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    if (whitelist_contains(norm)) return send_result(req, false, "this device already has internet access");
    int slot = -1;
    for (int i = 0; i < REQ_MAX; i++) {
        if (s_req[i].used && memcmp(s_req[i].mac, m, 6) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < REQ_MAX; i++) {
            if (!s_req[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) return send_result(req, false, "too many pending requests, please try later");
    s_req[slot].used = false;
    memcpy(s_req[slot].mac, m, 6);
    memset(s_req[slot].name, 0, sizeof(s_req[slot].name));
    memcpy(s_req[slot].name, name, k + 1);
    s_req[slot].used = true;
    return send_result(req, true, NULL);
}

static esp_err_t requests_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < REQ_MAX; i++) {
        if (!s_req[i].used) continue;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_req[i].mac[0], s_req[i].mac[1], s_req[i].mac[2], s_req[i].mac[3], s_req[i].mac[4], s_req[i].mac[5]);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", mac);
        cJSON_AddStringToObject(o, "name", s_req[i].name);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "requests", arr);
    return send_json(req, root);
}

static esp_err_t requests_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[128];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (!j) return send_result(req, false, "invalid data");
    cJSON *action = cJSON_GetObjectItem(j, "action");
    cJSON *mac = cJSON_GetObjectItem(j, "mac");
    bool ok = false;
    const char *err = "invalid data";
    uint8_t m[6];
    if (cJSON_IsString(action) && cJSON_IsString(mac) && parse_mac(mac->valuestring, m)) {
        int slot = -1;
        for (int i = 0; i < REQ_MAX; i++) {
            if (s_req[i].used && memcmp(s_req[i].mac, m, 6) == 0) { slot = i; break; }
        }
        if (slot < 0) {
            err = "request not found";
        } else if (strcmp(action->valuestring, "approve") == 0) {
            char norm[18];
            snprintf(norm, sizeof(norm), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
            if (whitelist_add(norm, s_req[slot].name, 0, "")) { s_req[slot].used = false; ok = true; }
            else err = "whitelist is full";
        } else if (strcmp(action->valuestring, "reject") == 0) {
            s_req[slot].used = false;
            ok = true;
        }
    }
    cJSON_Delete(j);
    return send_result(req, ok, ok ? NULL : err);
}

/* ================= Website block list API ================= */

static esp_err_t blocklist_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char out[BL_MAX * (BL_LEN + 1) + 2];
    out[0] = 0;
    int n = s_bl_n;
    for (int i = 0; i < n; i++) {
        if (i) strncat(out, "\n", sizeof(out) - strlen(out) - 1);
        strncat(out, s_bl[i], sizeof(out) - strlen(out) - 1);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "list", out);
    return send_json(req, r);
}

static esp_err_t blocklist_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char *buf = malloc(1200);
    if (!buf) return send_result(req, false, "out of memory");
    read_body(req, buf, 1200);
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return send_result(req, false, "invalid data");
    cJSON *l = cJSON_GetObjectItem(j, "list");
    if (!cJSON_IsString(l)) { cJSON_Delete(j); return send_result(req, false, "invalid data"); }
    char tmp[BL_MAX][BL_LEN + 1];
    const char *err = NULL;
    int n = blocklist_parse(l->valuestring, tmp, &err);
    cJSON_Delete(j);
    if (n < 0) return send_result(req, false, err ? err : "invalid list");
    char csv[BL_MAX * (BL_LEN + 1) + 2];
    csv[0] = 0;
    for (int i = 0; i < n; i++) {
        if (i) strncat(csv, ",", sizeof(csv) - strlen(csv) - 1);
        strncat(csv, tmp[i], sizeof(csv) - strlen(csv) - 1);
    }
    nvs_set_string("blocklist", csv);
    blocklist_apply(tmp, n);
    return send_result(req, true, NULL);
}

/* ================= Kick / identity ================= */

static esp_err_t kick_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[64];
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    bool ok = false;
    const char *err = "invalid data";
    if (j) {
        cJSON *mac = cJSON_GetObjectItem(j, "mac");
        uint8_t m[6];
        if (cJSON_IsString(mac) && parse_mac(mac->valuestring, m)) {
            uint16_t aid = 0;
            if (esp_wifi_ap_get_sta_aid(m, &aid) == ESP_OK && aid > 0) {
                esp_wifi_deauth_sta(aid);
                ok = true;
            } else {
                err = "device is not currently connected";
            }
        } else {
            err = "MAC must look like AA:BB:CC:DD:EE:FF";
        }
        cJSON_Delete(j);
    }
    return send_result(req, ok, ok ? NULL : err);
}

static esp_err_t me_get_handler(httpd_req_t *req) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "admin", check_session(req));
    cJSON_AddStringToObject(r, "fw", FW_VERSION);
    cJSON_AddNumberToObject(r, "reset_count", get_reset_counter());
    return send_json(req, r);
}

static esp_err_t tools_get_handler(httpd_req_t *req) {
    ua_capture(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, tools_html_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================= WiFi Analyzer (scan) ================= */

static void scan_task(void *arg) {
    wifi_scan_config_t cfg = { .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_err_t e = esp_wifi_scan_start(&cfg, true);
    s_scan_state = (e == ESP_OK) ? 0 : 2;
    vTaskDelete(NULL);
}

static esp_err_t scan_start_post_handler(httpd_req_t *req) {
    if (s_scan_state != 1) {
        s_scan_state = 1;
        if (xTaskCreate(scan_task, "scan", 4096, NULL, 4, NULL) != pdPASS) s_scan_state = 2;   /* task na bane to state atke nahi, GET error dikhata hai */
    }
    return send_result(req, true, NULL);
}

static const char *auth_name(wifi_auth_mode_t a) {
    switch (a) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "WPA2+";
    }
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    if (s_scan_state == 1) { cJSON *r = cJSON_CreateObject(); cJSON_AddStringToObject(r, "state", "scanning"); return send_json(req, r); }
    if (s_scan_state == 2) { cJSON *r = cJSON_CreateObject(); cJSON_AddStringToObject(r, "state", "error"); cJSON_AddStringToObject(r, "error", "scan failed - try again"); return send_json(req, r); }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 40) num = 40;
    wifi_ap_record_t *recs = num ? calloc(num, sizeof(wifi_ap_record_t)) : NULL;
    if (num && !recs) { httpd_resp_send_500(req); return ESP_OK; }
    if (num) esp_wifi_scan_get_ap_records(&num, recs);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", "done");
    uint8_t cur_ch = 0; wifi_second_chan_t sc;
    esp_wifi_get_channel(&cur_ch, &sc);
    cJSON_AddNumberToObject(root, "cur_ch", cur_ch);

    char apssid[33]; get_setting("ap_ssid", apssid, sizeof(apssid));
    cJSON_AddStringToObject(root, "ap_ssid", apssid);
    uint8_t apmac[6] = {0}; esp_wifi_get_mac(WIFI_IF_AP, apmac);
    char apmacs[18]; snprintf(apmacs, sizeof(apmacs), "%02X:%02X:%02X:%02X:%02X:%02X", apmac[0], apmac[1], apmac[2], apmac[3], apmac[4], apmac[5]);
    cJSON_AddStringToObject(root, "ap_bssid", apmacs);
    if (s_sta_connected) {
        wifi_ap_record_t up;
        if (esp_wifi_sta_get_ap_info(&up) == ESP_OK) {
            char ubssid[18]; snprintf(ubssid, sizeof(ubssid), "%02X:%02X:%02X:%02X:%02X:%02X", up.bssid[0], up.bssid[1], up.bssid[2], up.bssid[3], up.bssid[4], up.bssid[5]);
            cJSON_AddStringToObject(root, "up_bssid", ubssid);
        }
    }

    cJSON *nets = cJSON_CreateArray();
    for (int i = 0; i < num; i++) {
        wifi_ap_record_t *r = &recs[i];
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", (const char *)r->ssid);
        char bssid[18]; snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X", r->bssid[0], r->bssid[1], r->bssid[2], r->bssid[3], r->bssid[4], r->bssid[5]);
        cJSON_AddStringToObject(n, "bssid", bssid);
        cJSON_AddNumberToObject(n, "ch", r->primary);
        cJSON_AddNumberToObject(n, "rssi", r->rssi);
        cJSON_AddStringToObject(n, "auth", auth_name(r->authmode));
        cJSON_AddNumberToObject(n, "bw", r->second != WIFI_SECOND_CHAN_NONE ? 40 : 20);
        cJSON_AddBoolToObject(n, "b", r->phy_11b);
        cJSON_AddBoolToObject(n, "g", r->phy_11g);
        cJSON_AddBoolToObject(n, "n", r->phy_11n);
        cJSON_AddBoolToObject(n, "lr", r->phy_lr);
        cJSON_AddBoolToObject(n, "wps", r->wps);
        if (r->country.cc[0]) { char cc[3] = { r->country.cc[0], r->country.cc[1], 0 }; cJSON_AddStringToObject(n, "cc", cc); }
        cJSON_AddItemToArray(nets, n);
    }
    cJSON_AddItemToObject(root, "nets", nets);
    free(recs);
    return send_json(req, root);
}

/* ================= Deauth monitor readout ================= */

static esp_err_t deauth_get_handler(httpd_req_t *req) {
    char v[4]; get_setting("deauth_monitor", v, sizeof(v));
    bool enabled = (v[0] == '1');
    int thr = get_setting_int("deauth_alert", 1, 50, 10);
    int64_t now = esp_timer_get_time();
    int c10 = 0, b10 = 0, o10 = 0;
    int n = s_deauth_count;

    cJSON *events = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        int idx = ((s_deauth_head - 1 - i) % DEAUTH_RING + DEAUTH_RING) % DEAUTH_RING;
        deauth_evt_t *e = &s_deauth_ring[idx];
        double age = (double)(now - e->t) / 1e6;
        if (age <= 10.0) {
            c10++;
            if (memcmp(e->dst, "\xff\xff\xff\xff\xff\xff", 6) == 0) b10++;
            if (e->ours) o10++;
        }
        if (i < 15) {
            char sm[18], dm[18];
            snprintf(sm, sizeof(sm), "%02X:%02X:%02X:%02X:%02X:%02X", e->src[0], e->src[1], e->src[2], e->src[3], e->src[4], e->src[5]);
            snprintf(dm, sizeof(dm), "%02X:%02X:%02X:%02X:%02X:%02X", e->dst[0], e->dst[1], e->dst[2], e->dst[3], e->dst[4], e->dst[5]);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "age", (int)age);
            cJSON_AddStringToObject(o, "type", e->type == 0x0C ? "deauth" : "disassoc");
            cJSON_AddStringToObject(o, "src", sm);
            cJSON_AddStringToObject(o, "dst", dm);
            cJSON_AddNumberToObject(o, "rssi", e->rssi);
            cJSON_AddBoolToObject(o, "ours", e->ours);
            cJSON_AddItemToArray(events, o);
        }
    }

    const char *level = "ok";
    if (enabled) {
        if (c10 >= thr) level = "attack";
        else if (c10 >= (thr / 3 + 1)) level = "watch";
    }

    uint8_t ch = 0; wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddStringToObject(root, "level", level);
    cJSON_AddNumberToObject(root, "count10", c10);
    cJSON_AddNumberToObject(root, "bcast10", b10);
    cJSON_AddNumberToObject(root, "ours10", o10);
    cJSON_AddBoolToObject(root, "targets_us", o10 > 0);
    cJSON_AddNumberToObject(root, "total", s_deauth_total);
    cJSON_AddNumberToObject(root, "thr", thr);
    cJSON_AddNumberToObject(root, "channel", ch);
    cJSON_AddItemToObject(root, "events", events);
    return send_json(req, root);
}

/* ================= Speed test ================= */

static esp_err_t ping_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "pong", 4);
    return ESP_OK;
}

static esp_err_t speed_get_handler(httpd_req_t *req) {
    char qs[64] = "", kbs[16] = "256";
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        httpd_query_key_value(qs, "kb", kbs, sizeof(kbs));
    }
    int kb = atoi(kbs);
    if (kb <= 0 || kb > 4096) kb = 256;

    static char chunk[16384];
    memset(chunk, 'A', sizeof(chunk));
    httpd_resp_set_type(req, "application/octet-stream");
    int remaining = kb * 1024;
    while (remaining > 0) {
        int n = remaining > (int)sizeof(chunk) ? (int)sizeof(chunk) : remaining;
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) break;
        remaining -= n;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t upspeed_post_handler(httpd_req_t *req) {
    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining);
        if (n <= 0) break;
        remaining -= n;
    }
    return send_result(req, true, NULL);
}

/* ================= Diagnose ================= */

#include <fcntl.h>
#include <sys/poll.h>
#include <errno.h>
static int tcp_probe(const char *ip, int port, int timeout_ms) {
    int64_t t0 = esp_timer_get_time();
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);
    int fl = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, fl | O_NONBLOCK);
    int r = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (r != 0 && errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = sock, .events = POLLOUT, .revents = 0 };
        int pr = poll(&pfd, 1, timeout_ms);
        int soerr = 1;
        socklen_t sl = sizeof(soerr);
        if (pr > 0) getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        r = (pr > 0 && soerr == 0) ? 0 : -1;
    }
    close(sock);
    if (r != 0) return -1;
    return (int)((esp_timer_get_time() - t0) / 1000);
}

static void diag_task(void *arg) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", s_sta_connected);
    if (s_sta_connected) {
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            cJSON_AddStringToObject(root, "ssid", (const char *)info.ssid);
            cJSON_AddNumberToObject(root, "rssi", info.rssi);
            cJSON_AddNumberToObject(root, "channel", info.primary);
        }
        cJSON_AddStringToObject(root, "ip", s_sta_ip);

        esp_netif_ip_info_t ipi = { 0 };
        esp_netif_get_ip_info(s_sta_netif, &ipi);
        char gw[16]; snprintf(gw, sizeof(gw), IPSTR, IP2STR(&ipi.gw));
        cJSON_AddStringToObject(root, "gw_ip", gw);
        int gwms = tcp_probe(gw, 80, 800);
        cJSON_AddStringToObject(root, "gw", gwms >= 0 ? "ok" : "refused");
        cJSON_AddNumberToObject(root, "gw_ms", gwms >= 0 ? gwms : 0);

        cJSON *inet = cJSON_CreateArray();
        const char *hosts[2] = { "1.1.1.1", "8.8.8.8" };
        for (int i = 0; i < 2; i++) {
            int ms = tcp_probe(hosts[i], 443, 1500);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "host", hosts[i]);
            cJSON_AddBoolToObject(o, "ok", ms >= 0);
            cJSON_AddNumberToObject(o, "ms", ms >= 0 ? ms : 0);
            cJSON_AddItemToArray(inet, o);
        }
        cJSON_AddItemToObject(root, "inet", inet);

        int64_t d0 = esp_timer_get_time();
        struct hostent *he = gethostbyname("google.com");
        int dms = (int)((esp_timer_get_time() - d0) / 1000);
        if (he && he->h_addr_list && he->h_addr_list[0]) {
            uint8_t *a = (uint8_t *)he->h_addr_list[0];
            char dip[16]; snprintf(dip, sizeof(dip), "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
            cJSON_AddBoolToObject(root, "dns_ok", true);
            cJSON_AddStringToObject(root, "dns_ip", dip);
            cJSON_AddNumberToObject(root, "dns_ms", dms);
        } else {
            cJSON_AddBoolToObject(root, "dns_ok", false);
        }
    }
    char dns[16]; get_setting("dns1", dns, sizeof(dns));
    cJSON_AddStringToObject(root, "client_dns", dns);
    cJSON_AddStringToObject(root, "state", "done");

    if (s_diag_result) cJSON_Delete(s_diag_result);
    s_diag_result = root;
    s_diag_state = 2;
    vTaskDelete(NULL);
}

static esp_err_t diag_start_post_handler(httpd_req_t *req) {
    if (s_diag_state != 1) {
        s_diag_state = 1;
        if (xTaskCreate(diag_task, "diag", 6144, NULL, 4, NULL) != pdPASS) s_diag_state = 0;   /* task na bane to state atke nahi, dobara try ho sake */
    }
    return send_result(req, true, NULL);
}

static esp_err_t diag_get_handler(httpd_req_t *req) {
    if (s_diag_state != 2 || !s_diag_result) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "state", s_diag_state == 1 ? "running" : "idle");
        return send_json(req, r);
    }
    char *out = cJSON_PrintUnformatted(s_diag_result);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t usage_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "time_synced", time(NULL) >= 1700000000);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < TRACK_MAX; i++) {
        if (!s_usage[i].used) continue;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_usage[i].mac[0], s_usage[i].mac[1], s_usage[i].mac[2], s_usage[i].mac[3], s_usage[i].mac[4], s_usage[i].mac[5]);
        char name[64] = "";
        whitelist_get_name(mac, name, sizeof(name));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", mac);
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddNumberToObject(o, "total_bytes", (double)s_usage[i].total_bytes);
        cJSON_AddNumberToObject(o, "week_bytes", s_usage[i].week_bytes);
        cJSON_AddNumberToObject(o, "month_bytes", s_usage[i].month_bytes);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "devices", arr);

    /* Whole-repeater totals (both directions). This is the only figure here
     * that includes downloads/streaming - per-device figures above are
     * uploads only (see FEATURES.md for why). */
    double up_total = 0, up_week = 0, up_month = 0;
    for (int i = 0; i < TRACK_MAX; i++) {
        if (!s_usage[i].used) continue;
        up_total += (double)s_usage[i].total_bytes;
        up_week += s_usage[i].week_bytes;
        up_month += s_usage[i].month_bytes;
    }
    cJSON *overall = cJSON_CreateObject();
    cJSON_AddNumberToObject(overall, "upload_total_bytes", up_total);
    cJSON_AddNumberToObject(overall, "upload_week_bytes", up_week);
    cJSON_AddNumberToObject(overall, "upload_month_bytes", up_month);
    cJSON_AddNumberToObject(overall, "download_total_bytes", (double)s_down_usage.total_bytes);
    cJSON_AddNumberToObject(overall, "download_week_bytes", s_down_usage.week_bytes);
    cJSON_AddNumberToObject(overall, "download_month_bytes", s_down_usage.month_bytes);
    cJSON_AddItemToObject(root, "overall", overall);

    return send_json(req, root);
}

static esp_err_t speedhistory_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();
    int n = s_speedhist_count;
    for (int i = 0; i < n; i++) {
        int idx = ((s_speedhist_head - 1 - i) % SPEEDHIST_MAX + SPEEDHIST_MAX) % SPEEDHIST_MAX;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ago_s", (double)((now - s_speedhist[idx].t) / 1000000));
        cJSON_AddNumberToObject(o, "ms", s_speedhist[idx].ms);
        cJSON_AddBoolToObject(o, "ok", s_speedhist[idx].ok);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "samples", arr);
    return send_json(req, root);
}

static esp_err_t ota_fail(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

/* Web firmware update: raw .bin body (esp32-wifi-repeater.bin), login required */
static esp_err_t ota_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return ota_fail(req, "500 Internal Server Error", "No OTA partition (flash the new partition table first)");
    size_t total = req->content_len;
    if (total < 100000 || total > part->size) return ota_fail(req, "400 Bad Request", "Bad file size");
    char *buf = malloc(2048);
    if (!buf) return ota_fail(req, "500 Internal Server Error", "Out of memory");
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) { free(buf); return ota_fail(req, "500 Internal Server Error", "OTA begin failed"); }
    size_t got = 0;
    int timeouts = 0;
    bool first = true;
    while (got < total) {
        size_t want = total - got;
        if (want > 2048) want = 2048;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 10) r = -1; else continue;
        }
        if (r <= 0) { esp_ota_abort(h); free(buf); return ota_fail(req, "500 Internal Server Error", "Upload interrupted"); }
        timeouts = 0;
        if (first) {
            if ((uint8_t)buf[0] != 0xE9) { esp_ota_abort(h); free(buf); return ota_fail(req, "400 Bad Request", "Not a firmware app image"); }
            first = false;
        }
        err = esp_ota_write(h, buf, r);
        if (err != ESP_OK) { esp_ota_abort(h); free(buf); return ota_fail(req, "500 Internal Server Error", "Flash write failed"); }
        got += r;
    }
    free(buf);
    err = esp_ota_end(h);
    if (err != ESP_OK) return ota_fail(req, "400 Bad Request", "Image validation failed");
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) return ota_fail(req, "500 Internal Server Error", "Could not set boot partition");
    send_result(req, true, NULL);
    restart_soon();
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    send_result(req, true, NULL);
    restart_soon();
    return ESP_OK;
}

static esp_err_t factory_reset_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    wipe_settings();
    send_result(req, true, NULL);
    restart_soon();
    return ESP_OK;
}

/* ================= Online update (GitHub releases): admin "Check for update" / "Update now" ================= */
#include <time.h>
#include <strings.h>
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "build_num.h"

#define UPD_BASE_URL "https://github.com/Ibneilyas124/ESP32_Wifi_Repeater/releases/"
/* state: 0 idle, 1 checking, 2 update available, 3 up to date, 4 installing, 5 done (restarting), 6 error */
static volatile int s_upd_state = 0;
static volatile int s_upd_action = 0;   /* 0 check, 1 install */
static volatile int s_upd_latest = 0;
static volatile int s_upd_pct = 0;
static char s_upd_msg[160];
static char s_upd_loc[300];
static char s_upd_url[160];

static esp_err_t upd_http_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        strcasecmp(e->header_key, "Location") == 0) {
        snprintf(s_upd_loc, sizeof(s_upd_loc), "%s", e->header_value);
    }
    return ESP_OK;
}

static void upd_check(void) {
    s_upd_loc[0] = 0;
    esp_http_client_config_t c = {0};
    c.url = UPD_BASE_URL "latest";
    c.event_handler = upd_http_evt;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.disable_auto_redirect = true;
    c.timeout_ms = 15000;
    c.buffer_size = 1024;
    esp_http_client_handle_t cl = esp_http_client_init(&c);
    if (!cl) { snprintf(s_upd_msg, sizeof(s_upd_msg), "http init failed"); s_upd_state = 6; return; }
    esp_err_t e = esp_http_client_perform(cl);
    int code = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);
    if (e != ESP_OK) {
        snprintf(s_upd_msg, sizeof(s_upd_msg), "could not reach GitHub (%s)", esp_err_to_name(e));
        s_upd_state = 6; return;
    }
    const char *p = strstr(s_upd_loc, "/tag/build-");
    int n = p ? atoi(p + 11) : 0;
    if (n <= 0) {
        snprintf(s_upd_msg, sizeof(s_upd_msg), "unexpected reply from GitHub (HTTP %d)", code);
        s_upd_state = 6; return;
    }
    s_upd_latest = n;
    s_upd_state = (n > FW_BUILD) ? 2 : 3;
}

static void upd_install(void) {
    snprintf(s_upd_url, sizeof(s_upd_url), UPD_BASE_URL "download/build-%d/esp32-wifi-repeater.bin", s_upd_latest);
    esp_http_client_config_t hc = {0};
    hc.url = s_upd_url;
    hc.crt_bundle_attach = esp_crt_bundle_attach;
    hc.timeout_ms = 20000;
    hc.buffer_size = 2048;
    hc.buffer_size_tx = 1024;
    hc.keep_alive_enable = true;
    esp_https_ota_config_t oc = {0};
    oc.http_config = &hc;
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&oc, &h);
    if (err != ESP_OK) {
        snprintf(s_upd_msg, sizeof(s_upd_msg), "download could not start (%s)", esp_err_to_name(err));
        s_upd_state = 6; return;
    }
    int total = esp_https_ota_get_image_size(h);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (total > 0) s_upd_pct = (int)((int64_t)esp_https_ota_get_image_len_read(h) * 100 / total);
    }
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);
        if (err == ESP_OK) { s_upd_pct = 100; s_upd_state = 5; restart_soon(); return; }
        snprintf(s_upd_msg, sizeof(s_upd_msg), "image check failed (%s)", esp_err_to_name(err));
    } else {
        esp_https_ota_abort(h);
        snprintf(s_upd_msg, sizeof(s_upd_msg), "download failed (%s)", esp_err_to_name(err));
    }
    s_upd_state = 6;
}

/* Clock HTTP "Date" header se (UDP/NTP band ho tab bhi chalta hai) */
#include <sys/time.h>
static char s_time_err[64];
static char s_date_hdr[48];
static esp_err_t upd_date_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        strcasecmp(e->header_key, "Date") == 0) {
        snprintf(s_date_hdr, sizeof(s_date_hdr), "%s", e->header_value);
    }
    return ESP_OK;
}
static bool upd_parse_date(const char *v, time_t *out) {
    static const char *mons[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int d, y, hh, mm, ss;
    char mon[4] = {0};
    if (sscanf(v, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return false;
    int m = 0;
    for (int i = 0; i < 12; i++) if (strcmp(mon, mons[i]) == 0) m = i + 1;
    if (m == 0 || y < 2024 || d < 1 || d > 31) return false;
    int yy = y - (m <= 2);
    int era = yy / 400;
    int yoe = yy - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    *out = (time_t)(days * 86400L + hh * 3600L + mm * 60L + ss);
    return true;
}
static void upd_http_time(void) {
    const char *urls[2] = {"http://connectivitycheck.gstatic.com/generate_204", "http://www.google.com/"};
    s_time_err[0] = 0;
    for (int k = 0; k < 2; k++) {
        s_date_hdr[0] = 0;
        {
            struct hostent *hh = gethostbyname(k == 0 ? "connectivitycheck.gstatic.com" : "www.google.com");
            if (!hh || !hh->h_addr_list || !hh->h_addr_list[0]) {
                snprintf(s_time_err, sizeof(s_time_err), "DNS fail (host %d)", k + 1);
                continue;
            }
        }
        esp_http_client_config_t c = {0};
        c.url = urls[k];
        c.event_handler = upd_date_evt;
        c.disable_auto_redirect = true;
        c.timeout_ms = 8000;
        esp_http_client_handle_t cl = esp_http_client_init(&c);
        if (!cl) continue;
        esp_err_t e = esp_http_client_perform(cl);
        esp_http_client_cleanup(cl);
        if (e != ESP_OK) { snprintf(s_time_err, sizeof(s_time_err), "DNS ok, connect: %s", esp_err_to_name(e)); continue; }
        time_t t;
        if (s_date_hdr[0] && upd_parse_date(s_date_hdr, &t)) {
            struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            return;
        }
        snprintf(s_time_err, sizeof(s_time_err), "no Date header");
    }
}

/* clock sync na ho to SNTP dobara shuru karo (pool.ntp.org, phir time.google.com), har koshish 12 sec */
static void upd_sync_clock(void) {
    const char *srv[2] = {"pool.ntp.org", "time.google.com"};
    for (int k = 0; k < 2; k++) {
        if (time(NULL) >= 1700000000) return;
        esp_sntp_stop();
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, srv[k]);
        esp_sntp_init();
        for (int i = 0; i < 12; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (time(NULL) >= 1700000000) return;
        }
    }
}

static void upd_task(void *arg) {
    {
        esp_netif_dns_info_t fb = { 0 };
        fb.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
        fb.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_FALLBACK, &fb);
    }
    if (time(NULL) < 1700000000) upd_http_time();
    upd_sync_clock();
    if (time(NULL) < 1700000000) {
        snprintf(s_upd_msg, sizeof(s_upd_msg), "clock not synced (NTP and HTTP time failed: %s) - check upstream internet/DNS", s_time_err[0] ? s_time_err : "unknown");
        s_upd_state = 6;
    } else if (s_upd_action == 0) {
        upd_check();
    } else {
        upd_install();
    }
    vTaskDelete(NULL);
}

static esp_err_t update_get_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "state", s_upd_state);
    cJSON_AddNumberToObject(root, "cur", FW_BUILD);
    cJSON_AddNumberToObject(root, "latest", s_upd_latest);
    cJSON_AddNumberToObject(root, "pct", s_upd_pct);
    cJSON_AddStringToObject(root, "msg", s_upd_msg);
    return send_json(req, root);
}

static esp_err_t update_start_post_handler(httpd_req_t *req) {
    if (require_session_api(req) != ESP_OK) return ESP_OK;
    char buf[64] = {0};
    read_body(req, buf, sizeof(buf));
    cJSON *j = cJSON_Parse(buf);
    if (!j) return send_result(req, false, "invalid data");
    cJSON *a = cJSON_GetObjectItem(j, "action");
    int act = -1;
    if (cJSON_IsString(a)) {
        if (strcmp(a->valuestring, "check") == 0) act = 0;
        else if (strcmp(a->valuestring, "install") == 0) act = 1;
    }
    cJSON_Delete(j);
    if (act < 0) return send_result(req, false, "invalid data");
    if (s_upd_state == 1 || s_upd_state == 4) return send_result(req, false, "already running");
    if (!s_sta_connected) return send_result(req, false, "repeater is not connected to the upstream WiFi");
    if (act == 1 && s_upd_state != 2) return send_result(req, false, "check for an update first");
    size_t fr = esp_get_free_heap_size();
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (fr < 60000 || big < 24000) {
        char em[96];
        snprintf(em, sizeof(em), "not enough free memory (free %u, largest block %u)", (unsigned)fr, (unsigned)big);
        return send_result(req, false, em);
    }
    s_upd_action = act;
    s_upd_pct = 0;
    s_upd_msg[0] = 0;
    s_upd_state = act ? 4 : 1;
    if (xTaskCreate(upd_task, "upd", 10240, NULL, 4, NULL) != pdPASS) {
        s_upd_state = 0;
        return send_result(req, false, "could not start update task (low memory)");
    }
    return send_result(req, true, NULL);
}

static void start_webserver(void) {
    blocklist_load();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 36;
    config.stack_size = 8192;
    config.lru_purge_enable = true;   /* sockets bhar jayen to sab se purana idle socket band: admin page hang fix */
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }
    httpd_uri_t uris[] = {
        {.uri = "/",                   .method = HTTP_GET,  .handler = root_get_handler},
        {.uri = "/admin",              .method = HTTP_GET,  .handler = admin_get_handler},
        {.uri = "/api/login",          .method = HTTP_POST, .handler = login_post_handler},
        {.uri = "/api/logout",         .method = HTTP_POST, .handler = logout_post_handler},
        {.uri = "/api/status",         .method = HTTP_GET,  .handler = status_get_handler},
        {.uri = "/api/whitelist",      .method = HTTP_POST, .handler = whitelist_post_handler},
        {.uri = "/api/wifi",           .method = HTTP_POST, .handler = wifi_post_handler},
        {.uri = "/api/settings",       .method = HTTP_GET,  .handler = settings_get_handler},
        {.uri = "/api/settings",       .method = HTTP_POST, .handler = settings_post_handler},
        {.uri = "/api/config/export",  .method = HTTP_GET,  .handler = config_export_get_handler},
        {.uri = "/api/config/import",  .method = HTTP_POST, .handler = config_import_post_handler},
        {.uri = "/api/reboot",         .method = HTTP_POST, .handler = reboot_post_handler},
        {.uri = "/api/ota",            .method = HTTP_POST, .handler = ota_post_handler},
        {.uri = "/api/factory_reset",  .method = HTTP_POST, .handler = factory_reset_post_handler},
        {.uri = "/api/kick",           .method = HTTP_POST, .handler = kick_post_handler},
        {.uri = "/api/request",        .method = HTTP_POST, .handler = request_post_handler},
        {.uri = "/api/requests",       .method = HTTP_GET,  .handler = requests_get_handler},
        {.uri = "/api/requests",       .method = HTTP_POST, .handler = requests_post_handler},
        {.uri = "/api/blocklist",      .method = HTTP_GET,  .handler = blocklist_get_handler},
        {.uri = "/api/blocklist",      .method = HTTP_POST, .handler = blocklist_post_handler},
        {.uri = "/api/me",             .method = HTTP_GET,  .handler = me_get_handler},
        {.uri = "/tools",              .method = HTTP_GET,  .handler = tools_get_handler},
        {.uri = "/api/scan/start",     .method = HTTP_POST, .handler = scan_start_post_handler},
        {.uri = "/api/scan",           .method = HTTP_GET,  .handler = scan_get_handler},
        {.uri = "/api/deauth",         .method = HTTP_GET,  .handler = deauth_get_handler},
        {.uri = "/api/ping",           .method = HTTP_GET,  .handler = ping_get_handler},
        {.uri = "/api/speed",          .method = HTTP_GET,  .handler = speed_get_handler},
        {.uri = "/api/upspeed",        .method = HTTP_POST, .handler = upspeed_post_handler},
        {.uri = "/api/diag/start",     .method = HTTP_POST, .handler = diag_start_post_handler},
        {.uri = "/api/diag",           .method = HTTP_GET,  .handler = diag_get_handler},
        {.uri = "/api/usage",          .method = HTTP_GET,  .handler = usage_get_handler},
        {.uri = "/api/speedhistory",   .method = HTTP_GET,  .handler = speedhistory_get_handler},
        {.uri = "/api/update/start",   .method = HTTP_POST, .handler = update_start_post_handler},
        {.uri = "/api/update",         .method = HTTP_GET,  .handler = update_get_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "Web dashboard started on port 80");
    /* Web server is up: confirm this image is good (rollback cancelled) */
    esp_ota_mark_app_valid_cancel_rollback();
}

void app_main(void) {
    s_boot_time_us = esp_timer_get_time();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    down_usage_load();

    wifi_init();
    start_webserver();
    xTaskCreate(button_task, "reset_btn", 3072, NULL, 5, NULL);
}
