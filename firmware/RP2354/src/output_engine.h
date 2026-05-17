#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ring_buffer.h"

typedef struct {
    bool running;
    uint32_t sample_rate_hz;
    uint32_t consumed_samples;
    uint32_t underruns;
    bool mirror_x;
    bool mirror_y;
    int16_t last_output[RP_SAMPLE_CHANNELS];
} output_engine_status_t;

void output_engine_init(void);

void output_engine_start(void);
void output_engine_stop_safe(void);
void output_engine_pause_for_flash(void);
void output_engine_resume_after_flash(void);
void output_engine_set_rate(uint32_t rate_hz);
void output_engine_set_mirror(bool mirror_x, bool mirror_y);
void output_engine_get_mirror(bool *mirror_x, bool *mirror_y);

void output_engine_get_status(output_engine_status_t *status);
