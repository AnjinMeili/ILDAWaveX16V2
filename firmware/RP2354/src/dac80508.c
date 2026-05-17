#include <string.h>

#include "pico/stdlib.h"
#include "pico/sync.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"

#include "dac80508_tx.pio.h"

#include "dac80508.h"

/* DAC80508 pins */
#define DAC_MISO_GPIO 0
#define DAC_CS_GPIO   1
#define DAC_SCK_GPIO  2
#define DAC_MOSI_GPIO 3

#define DAC_REG_SYNC     0x02
#define DAC_REG_GAIN     0x04
#define DAC_REG_TRIGGER  0x05
#define DAC_REG_DAC_BASE 0x08

#define DAC_CH_X  6
#define DAC_CH_Y  7
#define DAC_CH_R  5
#define DAC_CH_G  4
#define DAC_CH_B  3
#define DAC_CH_I  2
#define DAC_CH_U1 1
#define DAC_CH_U2 0

static PIO s_dac_pio = pio1;
static uint s_dac_sm = 0;
static uint s_dac_offset = 0;
static int s_dac_dma_chan = -1;

static uint32_t s_dac_words[DAC_BATCH_MAX_SAMPLES * 9];

static critical_section_t s_dac_cs;

static uint8_t s_active_mask = DAC_DEFAULT_ACTIVE_MASK;
static uint32_t s_sample_rate_hz = 1000;

static inline uint16_t signed_to_dac(int16_t v)
{
    return (uint16_t)((int32_t)v + 32768);
}

static uint32_t active_channel_count(void)
{
    uint32_t n = 0;

    for (int i = 0; i < RP_SAMPLE_CHANNELS; i++) {
        if (s_active_mask & (1u << i)) {
            n++;
        }
    }

    if (n == 0) {
        n = 1;
    }

    return n;
}

static inline uint32_t dac_make_word(uint8_t reg, uint16_t value)
{
    uint32_t cmd = ((uint32_t)reg << 16) | value;
    return cmd << 8;
}

static inline void dac_put_word(uint32_t *buf, uint32_t *index, uint8_t reg, uint16_t value)
{
    buf[*index] = dac_make_word(reg, value);
    (*index)++;
}

static uint32_t dac_build_words_for_sample(uint32_t *buf, const int16_t sample[RP_SAMPLE_CHANNELS])
{
    uint32_t i = 0;

    if (s_active_mask & (1u << 0)) {
        dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_X, signed_to_dac(sample[0]));
    }

    if (s_active_mask & (1u << 1)) {
        dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_Y, signed_to_dac(sample[1]));
    }

    if (s_active_mask & (1u << 2)) {
        dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_R, signed_to_dac(sample[2]));
    }

    if (s_active_mask & (1u << 3)) {
        dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_G, signed_to_dac(sample[3]));
    }

    if (s_active_mask & (1u << 4)) {
        dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_B, signed_to_dac(sample[4]));
    }

    if (s_active_mask & (1u << 5)) {
        dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_I, signed_to_dac(sample[5]));
    }

    if (s_active_mask & (1u << 6)) {
        dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_U1, signed_to_dac(sample[6]));
    }

    if (s_active_mask & (1u << 7)) {
        dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_U2, signed_to_dac(sample[7]));
    }

    dac_put_word(buf, &i, DAC_REG_TRIGGER, 0x0010);

    return i;
}

static uint32_t dac_build_words_for_all_raw_zero(uint32_t *buf)
{
    uint32_t i = 0;

    /*
       Hard stopped/off state:
       all physical DAC channels raw 0x0000.
    */
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_X,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_Y,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_R,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_G,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_B,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_I,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_U1, 0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_U2, 0x0000);

    dac_put_word(buf, &i, DAC_REG_TRIGGER, 0x0010);

    return i;
}

static uint32_t dac_build_words_for_blanked_xy(uint32_t *buf, int16_t x, int16_t y)
{
    uint32_t i = 0;

    /*
       Active underrun state:
       keep scanner position sane, blank laser outputs.
       X/Y use signed sample conversion.
       R/G/B/I/U1/U2 are forced to raw 0x0000.
    */
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_X,  signed_to_dac(x));
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_Y,  signed_to_dac(y));
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_R,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_G,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_B,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_I,  0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_U1, 0x0000);
    dac_put_word(buf, &i, DAC_REG_DAC_BASE + DAC_CH_U2, 0x0000);

    dac_put_word(buf, &i, DAC_REG_TRIGGER, 0x0010);

    return i;
}

static void dac_apply_timing_locked(void)
{
    uint32_t rate = s_sample_rate_hz;

    if (rate < 1000) {
        rate = 1000;
    }

    if (rate > 200000) {
        rate = 200000;
    }

    uint32_t frames_per_sample = active_channel_count() + 1;
    double pio_cycles_per_sample = (double)frames_per_sample * 24.0 * 3.0;
    double target_pio_hz = (double)rate * pio_cycles_per_sample;

    uint32_t sys_hz = clock_get_hz(clk_sys);
    float div = (float)((double)sys_hz / target_pio_hz);

    if (div < 1.0f) {
        div = 1.0f;
    }

    if (div > 65535.0f) {
        div = 65535.0f;
    }

    pio_sm_set_clkdiv(s_dac_pio, s_dac_sm, div);
}

static void dac_pio_dma_write_words_blocking(const uint32_t *words, uint32_t count)
{
    if (count == 0) {
        return;
    }

    critical_section_enter_blocking(&s_dac_cs);

    dma_channel_config cfg = dma_channel_get_default_config(s_dac_dma_chan);

    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(s_dac_pio, s_dac_sm, true));

    dma_channel_configure(
        s_dac_dma_chan,
        &cfg,
        &s_dac_pio->txf[s_dac_sm],
        words,
        count,
        true
    );

    dma_channel_wait_for_finish_blocking(s_dac_dma_chan);

    critical_section_exit(&s_dac_cs);
}

static void dac_write_24(uint8_t reg, uint16_t value)
{
    uint32_t word = dac_make_word(reg, value);
    dac_pio_dma_write_words_blocking(&word, 1);
}

void dac_set_sample_rate(uint32_t sample_rate_hz)
{
    critical_section_enter_blocking(&s_dac_cs);

    if (sample_rate_hz < 1) {
        sample_rate_hz = 1;
    }

    if (sample_rate_hz > 200000) {
        sample_rate_hz = 200000;
    }

    s_sample_rate_hz = sample_rate_hz;
    dac_apply_timing_locked();

    critical_section_exit(&s_dac_cs);
}

void dac_set_active_mask(uint8_t mask)
{
    if (mask == 0) {
        mask = 0x03; /* X/Y minimum */
    }

    critical_section_enter_blocking(&s_dac_cs);

    s_active_mask = mask;
    dac_apply_timing_locked();

    critical_section_exit(&s_dac_cs);
}

uint8_t dac_get_active_mask(void)
{
    return s_active_mask;
}

void dac_write_all_zero(void)
{
    uint32_t count = dac_build_words_for_all_raw_zero(s_dac_words);
    dac_pio_dma_write_words_blocking(s_dac_words, count);
}

void dac_write_blanked_xy(int16_t x, int16_t y)
{
    uint32_t count = dac_build_words_for_blanked_xy(s_dac_words, x, y);
    dac_pio_dma_write_words_blocking(s_dac_words, count);
}

void dac_write_sample(const int16_t sample[RP_SAMPLE_CHANNELS])
{
    uint32_t count = dac_build_words_for_sample(s_dac_words, sample);
    dac_pio_dma_write_words_blocking(s_dac_words, count);
}

void dac_write_sample_batch(const int16_t samples[][RP_SAMPLE_CHANNELS], uint32_t count)
{
    if (count == 0) {
        return;
    }

    if (count > DAC_BATCH_MAX_SAMPLES) {
        count = DAC_BATCH_MAX_SAMPLES;
    }

    uint32_t wi = 0;

    for (uint32_t s = 0; s < count; s++) {
        wi += dac_build_words_for_sample(&s_dac_words[wi], samples[s]);
    }

    dac_pio_dma_write_words_blocking(s_dac_words, wi);
}

void dac_init(void)
{
    critical_section_init(&s_dac_cs);

    gpio_init(DAC_CS_GPIO);
    gpio_set_dir(DAC_CS_GPIO, GPIO_OUT);
    gpio_put(DAC_CS_GPIO, 1);

    gpio_init(DAC_SCK_GPIO);
    gpio_set_dir(DAC_SCK_GPIO, GPIO_OUT);
    gpio_put(DAC_SCK_GPIO, 0);

    gpio_init(DAC_MOSI_GPIO);
    gpio_set_dir(DAC_MOSI_GPIO, GPIO_OUT);
    gpio_put(DAC_MOSI_GPIO, 0);

    s_dac_offset = pio_add_program(s_dac_pio, &dac80508_tx_program);

    pio_sm_config c = dac80508_tx_program_get_default_config(s_dac_offset);

    sm_config_set_out_pins(&c, DAC_MOSI_GPIO, 1);
    sm_config_set_sideset_pins(&c, DAC_CS_GPIO);
    sm_config_set_out_shift(&c, false, false, 24);

    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(s_dac_pio, s_dac_sm, s_dac_offset, &c);

    pio_gpio_init(s_dac_pio, DAC_MOSI_GPIO);
    pio_gpio_init(s_dac_pio, DAC_CS_GPIO);
    pio_gpio_init(s_dac_pio, DAC_SCK_GPIO);

    pio_sm_set_consecutive_pindirs(s_dac_pio, s_dac_sm, DAC_MOSI_GPIO, 1, true);
    pio_sm_set_consecutive_pindirs(s_dac_pio, s_dac_sm, DAC_CS_GPIO, 2, true);

    pio_sm_set_enabled(s_dac_pio, s_dac_sm, true);

    s_dac_dma_chan = dma_claim_unused_channel(true);

    dac_set_active_mask(DAC_DEFAULT_ACTIVE_MASK);
    dac_set_sample_rate(1000);

    sleep_ms(10);

    dac_write_24(DAC_REG_TRIGGER, 0x000A);
    sleep_ms(10);

    dac_write_24(DAC_REG_GAIN, 0x00FF);
    dac_write_24(DAC_REG_SYNC, 0xFFFF);

    /*
       Boot safety:
       all physical DAC outputs raw 0x0000.
    */
    dac_write_all_zero();
}