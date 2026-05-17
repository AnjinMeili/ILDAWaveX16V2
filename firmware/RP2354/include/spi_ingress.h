#ifndef SPI_INGRESS_H
#define SPI_INGRESS_H

#include <stdbool.h>
#include <stdint.h>
#include "ring.h"

typedef struct {
    ingress_ring_t *ring;
    int dma_rx;
    volatile bool dma_armed;
    volatile bool cs_active;

    uint8_t packet_dma_buffer[INGRESS_PACKET_BUFFER_COUNT][INGRESS_PACKET_MAX_BYTES];
    volatile uint8_t dma_fill_index;
    volatile uint8_t ready_mask;
    volatile uint8_t busy_mask;
    volatile uint32_t ready_bytes[INGRESS_PACKET_BUFFER_COUNT];
    volatile bool packet_overflow;

    volatile uint32_t packets_seen;
    volatile uint32_t packets_dropped;
    volatile uint32_t packets_truncated;
    volatile uint32_t bytes_last_packet;

    volatile uint32_t frames_ok;
    volatile uint32_t frames_bad_crc;
    volatile uint32_t frames_bad_sync;
    volatile uint32_t frames_dropped;
    volatile uint32_t seq_errors;
    volatile uint8_t next_sequence;
    volatile bool sequence_locked;
} spi_ingress_t;

void spi_ingress_init(spi_ingress_t *ctx, ingress_ring_t *ring);
void spi_ingress_poll(spi_ingress_t *ctx);

#endif
