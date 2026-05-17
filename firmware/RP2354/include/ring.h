#ifndef RING_H
#define RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "project_config.h"

typedef struct {
    uint16_t samples[POINT_CHANNELS];
} point8_t;

typedef struct {
    uint8_t raw[INGRESS_BLOCK_BYTES];
    uint32_t bytes_used;
    uint32_t point_count;
} ingress_block_t;

typedef struct {
    ingress_block_t blocks[INGRESS_BLOCK_COUNT];
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint32_t count;
    volatile uint32_t overruns;
} ingress_ring_t;

void ingress_ring_init(ingress_ring_t *ring);
ingress_block_t *ingress_ring_get_write_block(ingress_ring_t *ring);
bool ingress_ring_commit_write(ingress_ring_t *ring, uint32_t bytes_used);
ingress_block_t *ingress_ring_peek_read_block(ingress_ring_t *ring);
void ingress_ring_release_read_block(ingress_ring_t *ring);

static inline void decode_point_le(const uint8_t *src, point8_t *dst) {
    for (uint32_t ch = 0; ch < POINT_CHANNELS; ++ch) {
        dst->samples[ch] = (uint16_t)src[ch * 2u] | ((uint16_t)src[ch * 2u + 1u] << 8u);
    }
}

#endif
