#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t payload_errors;
    uint32_t received_blocks;
    uint32_t received_samples;
    uint32_t stream_packets;
    uint32_t stream_bad_packets;
    uint32_t stream_dropped_packets;
    uint32_t stream_rx_timeouts;
    uint32_t stream_stale_packets;   /* received across a STOP/CLEAR and discarded */
} stream_rx_status_t;

void stream_rx_init(void);

bool stream_rx_receive_packet(uint32_t timeout_ms);
void stream_rx_process_packet(void);

void stream_rx_get_status(stream_rx_status_t *status);