#pragma once

#include <stdint.h>

#include "ring_buffer.h"

#define DAC_DEFAULT_ACTIVE_MASK 0xFFu
#define DAC_BATCH_MAX_SAMPLES 32

void dac_init(void);

void dac_set_sample_rate(uint32_t sample_rate_hz);
void dac_set_active_mask(uint8_t mask);
uint8_t dac_get_active_mask(void);

void dac_write_sample(const int16_t sample[RP_SAMPLE_CHANNELS]);
void dac_write_sample_batch(const int16_t samples[][RP_SAMPLE_CHANNELS], uint32_t count);

/*
   Hard safety/off:
   forces raw DAC code 0x0000 on every physical DAC output channel.
   Use this for boot, STOP, CLEAR, and fully stopped state.
*/
void dac_write_all_zero(void);

/*
   Active-stream underrun safety:
   keeps X/Y at the supplied signed sample values,
   but forces R/G/B/I/U1/U2 to raw DAC 0x0000.
   Use this while output is running but samples temporarily starve.
*/
void dac_write_blanked_xy(int16_t x, int16_t y);