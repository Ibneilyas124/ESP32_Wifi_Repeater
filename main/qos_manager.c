#include "qos_manager.h"
#include <string.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

static client_node_t clients[MAX_CLIENTS];
static size_t client_count = 0;
static uint64_t total_monthly_bytes = 0;
static uint64_t total_weekly_bytes = 0;

static int find_client_index(uint8_t *mac) {
    for (size_t i = 0; i < client_count; i++) {
        if (memcmp(clients[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

void qos_init() {
    memset(clients, 0, sizeof(clients));
    client_count = 0;
}

void qos_process_packet(uint8_t *mac, uint32_t packet_len, bool is_tx) {
    int idx = find_client_index(mac);
    if (idx < 0) {
        if (client_count < MAX_CLIENTS) {
            idx = client_count++;
            memcpy(clients[idx].mac, mac, 6);
            snprintf(clients[idx].name, sizeof(clients[idx].name), "Client-%02X%02X", mac[4], mac[5]);
            clients[idx].speed_limit_mbps = 0;
            clients[idx].quota_limit_mb = 0;
            clients[idx].is_blocked = false;
        } else {
            return;
        }
    }

    // Check if client is blocked
    if (clients[idx].is_blocked) return;

    // Check Quota Limit
    if (clients[idx].quota_limit_mb > 0) {
        uint64_t limit_bytes = (uint64_t)clients[idx].quota_limit_mb * 1024 * 1024;
        if (clients[idx].monthly_bytes >= limit_bytes) {
            return; // Drop packet (Quota exceeded)
        }
    }

    // Token Bucket Rate Limiting (Speed Cap)
    if (clients[idx].speed_limit_mbps > 0) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000); // ms
        uint32_t elapsed = now - clients[idx].last_token_update;
        clients[idx].last_token_update = now;

        // Rate = Speed Limit in Bytes per ms
        uint32_t rate_bytes_per_ms = (clients[idx].speed_limit_mbps * 1000000) / (8 * 1000);
        clients[idx].tokens += elapsed * rate_bytes_per_ms;

        uint32_t max_bucket = rate_bytes_per_ms * 1000; // 1 sec burst capacity
        if (clients[idx].tokens > max_bucket) clients[idx].tokens = max_bucket;

        if (clients[idx].tokens < packet_len) {
            return; // Drop packet to throttle bandwidth speed
        }
        clients[idx].tokens -= packet_len;
    }

    // Update Counters
    clients[idx].weekly_bytes += packet_len;
    clients[idx].monthly_bytes += packet_len;
    total_weekly_bytes += packet_len;
    total_monthly_bytes += packet_len;
}

client_node_t* qos_get_clients(size_t *count) {
    *count = client_count;
    return clients;
}

void qos_set_client_limit(uint8_t *mac, uint32_t speed_mbps, uint32_t quota_mb) {
    int idx = find_client_index(mac);
    if (idx >= 0) {
        clients[idx].speed_limit_mbps = speed_mbps;
        clients[idx].quota_limit_mb = quota_mb;
    }
}

void qos_block_client(uint8_t *mac, bool block) {
    int idx = find_client_index(mac);
    if (idx >= 0) {
        clients[idx].is_blocked = block;
    }
}

uint64_t qos_get_total_monthly_bytes() { return total_monthly_bytes; }
uint64_t qos_get_total_weekly_bytes() { return total_weekly_bytes; }
