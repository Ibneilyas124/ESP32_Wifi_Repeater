#ifndef QOS_MANAGER_H
#define QOS_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_CLIENTS 16

typedef struct {
    uint8_t mac[6];
    char name[32];
    uint32_t ip;
    uint64_t weekly_bytes;
    uint64_t monthly_bytes;
    uint32_t speed_limit_mbps; // 0 = Unlimited
    uint32_t quota_limit_mb;   // 0 = Unlimited
    bool is_blocked;
    
    // Token Bucket internals
    uint32_t tokens;
    uint32_t last_token_update;
} client_node_t;

void qos_init();
void qos_process_packet(uint8_t *mac, uint32_t packet_len, bool is_tx);
client_node_t* qos_get_clients(size_t *count);
void qos_set_client_limit(uint8_t *mac, uint32_t speed_mbps, uint32_t quota_mb);
void qos_block_client(uint8_t *mac, bool block);
uint64_t qos_get_total_monthly_bytes();
uint64_t qos_get_total_weekly_bytes();

#endif
