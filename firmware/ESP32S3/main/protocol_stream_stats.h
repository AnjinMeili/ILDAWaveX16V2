#pragma once

#include <stdint.h>

typedef enum {
    PROTOCOL_STREAM_SD_ILDA = 0,
    PROTOCOL_STREAM_IDN,
    PROTOCOL_STREAM_ETHERDREAM,
    PROTOCOL_STREAM_ILW,
} protocol_stream_source_t;

typedef struct {
    uint32_t sd_ilda_samples;
    uint32_t idn_samples;
    uint32_t etherdream_samples;
    uint32_t ilw_samples;
} protocol_stream_stats_t;

void protocol_stream_stats_add_samples(protocol_stream_source_t source, uint32_t samples);
void protocol_stream_stats_get(protocol_stream_stats_t *stats);
