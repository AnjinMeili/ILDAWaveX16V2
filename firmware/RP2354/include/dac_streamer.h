#ifndef DAC_STREAMER_H
#define DAC_STREAMER_H

#include <stdbool.h>
#include <stdint.h>
#include "hardware/pio.h"
#include "ring.h"

typedef struct {
    PIO pio;
    uint sm;
    uint offset;
    int dma_tx;
    volatile bool dma_busy;
    volatile uint32_t frames_sent;
    volatile uint32_t points_sent;
    volatile uint32_t underruns;
    volatile bool started;

    uint32_t tx_words[2][TX_WORD_COUNT];
    uint32_t tx_word_count[2];
    uint8_t fill_index;
    uint8_t send_index;
    uint8_t queued_buffers;

    ingress_block_t *current_block;
    uint32_t current_block_point_index;
} dac_streamer_t;

void dac_streamer_init(dac_streamer_t *ctx);
void dac_streamer_set_zero_outputs(dac_streamer_t *ctx);
void dac_streamer_service(dac_streamer_t *ctx, ingress_ring_t *ring);

#endif
