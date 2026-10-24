#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ring_buffer.h"

typedef enum {
    OUTPUT_MODE_PARKED,          /* stopped: X/Y at centre, laser channels at 0 V */
    OUTPUT_MODE_START_BLANK,     /* STARTed, waiting for the first sample */
    OUTPUT_MODE_STREAMING,
    OUTPUT_MODE_UNDERRUN_BLANK,  /* ring empty: holding X/Y, laser channels at 0 V */
    OUTPUT_MODE_FAILSAFE_BLANK,
} output_engine_mode_t;

typedef struct {
    bool running;
    uint32_t sample_rate_hz;      /* rate actually applied */
    uint32_t requested_rate_hz;   /* last RATE asked for, before the limit */
    uint32_t max_rate_hz;         /* RATE is clamped to this; saved in flash */
    uint32_t consumed_samples;
    uint32_t underruns;
    uint32_t failsafe_blank_events;
    uint32_t mode_transitions;
    output_engine_mode_t mode;
    bool mirror_x;
    bool mirror_y;
    int16_t last_output[RP_SAMPLE_CHANNELS];

    /* Scanner load since START: the largest X/Y step between consecutive lit
       samples, and how many steps of 1/8 full scale or more were played. */
    uint32_t max_lit_step;
    uint32_t lit_big_steps;
    uint32_t blank_big_steps;
} output_engine_status_t;

void output_engine_init(void);

void output_engine_start(void);
void output_engine_stop_safe(void);
void output_engine_pause_for_flash(void);
void output_engine_resume_after_flash(void);
void output_engine_set_rate(uint32_t rate_hz);
void output_engine_set_max_rate(uint32_t max_rate_hz);
uint32_t output_engine_get_max_rate(void);
void output_engine_set_mirror(bool mirror_x, bool mirror_y);
void output_engine_get_mirror(bool *mirror_x, bool *mirror_y);

void output_engine_get_status(output_engine_status_t *status);
