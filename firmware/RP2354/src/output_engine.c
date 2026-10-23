#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/gpio.h"

#include "ring_buffer.h"
#include "dac80508.h"
#include "output_engine.h"
#include "project_config.h"

#define OUTPUT_DEFAULT_SAMPLE_RATE_HZ 30000

#define OUTPUT_BATCH_SAMPLES DAC_BATCH_MAX_SAMPLES
#define BATCH_MODE_MIN_RATE_HZ 5000

#define FAILSAFE_TIMEOUT_US 20000
#define FAILSAFE_REPEAT_US 5000

static volatile bool s_running = false;
static volatile uint32_t s_sample_rate_hz = OUTPUT_DEFAULT_SAMPLE_RATE_HZ;
static volatile bool s_mirror_x = false;
static volatile bool s_mirror_y = true;

static volatile uint32_t s_consumed_samples = 0;
static volatile uint32_t s_underruns = 0;
static volatile uint32_t s_failsafe_blank_events = 0;
static volatile uint32_t s_mode_transitions = 0;
static volatile output_engine_mode_t s_output_mode = OUTPUT_MODE_HARD_ZERO;

static int16_t s_last_output[RP_SAMPLE_CHANNELS];

static critical_section_t s_output_cs;

static int16_t s_batch[OUTPUT_BATCH_SAMPLES][RP_SAMPLE_CHANNELS];

static absolute_time_t s_last_valid_sample_time;
static absolute_time_t s_last_forced_blank_time;

static volatile bool s_core1_launched = false;

static bool s_have_valid_xy = false;
static int16_t s_last_valid_x = 0;
static int16_t s_last_valid_y = 0;

static void trace_pins_init(void)
{
    /*
       Scope trace outputs on the RP_IO10/RP_IO11 breakout nets.
       OUTPUT_RUNNING is high while START is active.
       FORCED_BLANK is high whenever the DAC is not playing stream samples.
    */
    gpio_init(PIN_TRACE_OUTPUT_RUNNING);
    gpio_set_dir(PIN_TRACE_OUTPUT_RUNNING, GPIO_OUT);
    gpio_put(PIN_TRACE_OUTPUT_RUNNING, false);

    gpio_init(PIN_TRACE_FORCED_BLANK);
    gpio_set_dir(PIN_TRACE_FORCED_BLANK, GPIO_OUT);
    gpio_put(PIN_TRACE_FORCED_BLANK, true);
}

static void set_trace_running(bool running)
{
    gpio_put(PIN_TRACE_OUTPUT_RUNNING, running);
}

static void set_output_mode(output_engine_mode_t mode)
{
    bool changed = false;

    critical_section_enter_blocking(&s_output_cs);

    if (s_output_mode != mode) {
        s_output_mode = mode;
        s_mode_transitions++;
        changed = true;
    }

    critical_section_exit(&s_output_cs);

    if (changed) {
        gpio_put(PIN_TRACE_FORCED_BLANK, mode != OUTPUT_MODE_STREAMING);
    }
}

static void note_output_starved(void)
{
    /*
       START_BLANK and FAILSAFE_BLANK persist until real samples arrive.
       Only a running stream drops into UNDERRUN_BLANK.
    */
    if (s_output_mode == OUTPUT_MODE_STREAMING) {
        set_output_mode(OUTPUT_MODE_UNDERRUN_BLANK);
    }
}

static void sample_hard_zero(int16_t sample[RP_SAMPLE_CHANNELS])
{
    /*
       signed -32768 maps to raw DAC code 0x0000.
       Used only for stopped/boot/status hard-zero state.
    */
    for (int i = 0; i < RP_SAMPLE_CHANNELS; i++) {
        sample[i] = (int16_t)-32768;
    }
}

static int16_t mirror_i16(int16_t v)
{
    return (v == INT16_MIN) ? INT16_MAX : (int16_t)-v;
}

static void apply_output_mirror(int16_t sample[RP_SAMPLE_CHANNELS])
{
    if (s_mirror_x) {
        sample[0] = mirror_i16(sample[0]);
    }

    if (s_mirror_y) {
        sample[1] = mirror_i16(sample[1]);
    }
}

static void apply_xy_mirror(int16_t *x, int16_t *y)
{
    if (s_mirror_x) {
        *x = mirror_i16(*x);
    }

    if (s_mirror_y) {
        *y = mirror_i16(*y);
    }
}

static void get_hold_xy(int16_t *x, int16_t *y)
{
    if (s_have_valid_xy) {
        *x = s_last_valid_x;
        *y = s_last_valid_y;
    } else {
        /*
           No real point has arrived yet after START.
           Use scanner center while laser is blanked.
           signed 0 maps to raw DAC 0x8000.
        */
        *x = 0;
        *y = 0;
    }
}

static void sample_blanked_hold_xy(int16_t sample[RP_SAMPLE_CHANNELS])
{
    int16_t x;
    int16_t y;

    get_hold_xy(&x, &y);

    sample[0] = x;
    sample[1] = y;

    /*
       Laser/color/intensity and aux channels raw 0x0000.
    */
    sample[2] = (int16_t)-32768;
    sample[3] = (int16_t)-32768;
    sample[4] = (int16_t)-32768;
    sample[5] = (int16_t)-32768;
    sample[6] = (int16_t)-32768;
    sample[7] = (int16_t)-32768;
}

static void dac_write_blanked_xy_mirrored(int16_t x, int16_t y)
{
    apply_xy_mirror(&x, &y);
    dac_write_blanked_xy(x, y);
}

static void set_last_output_hard_zero(void)
{
    critical_section_enter_blocking(&s_output_cs);
    sample_hard_zero(s_last_output);
    critical_section_exit(&s_output_cs);
}

static void set_last_output_blanked_hold_xy(void)
{
    int16_t sample[RP_SAMPLE_CHANNELS];

    sample_blanked_hold_xy(sample);
    apply_output_mirror(sample);

    critical_section_enter_blocking(&s_output_cs);
    memcpy(s_last_output, sample, sizeof(sample));
    critical_section_exit(&s_output_cs);
}

static void note_valid_sample(const int16_t sample[RP_SAMPLE_CHANNELS])
{
    s_last_valid_x = sample[0];
    s_last_valid_y = sample[1];
    s_have_valid_xy = true;
    s_last_valid_sample_time = get_absolute_time();
}

static void force_dac_blanked_hold_xy_now(void)
{
    int16_t x;
    int16_t y;

    get_hold_xy(&x, &y);

    set_output_mode(OUTPUT_MODE_FAILSAFE_BLANK);

    critical_section_enter_blocking(&s_output_cs);
    s_failsafe_blank_events++;
    critical_section_exit(&s_output_cs);

    set_last_output_blanked_hold_xy();
    dac_write_blanked_xy_mirrored(x, y);

    s_last_forced_blank_time = get_absolute_time();
}

static void failsafe_check_no_samples(void)
{
    absolute_time_t now = get_absolute_time();

    int64_t since_valid_us = absolute_time_diff_us(s_last_valid_sample_time, now);

    if (since_valid_us < FAILSAFE_TIMEOUT_US) {
        return;
    }

    int64_t since_blank_us = absolute_time_diff_us(s_last_forced_blank_time, now);

    if (since_blank_us >= FAILSAFE_REPEAT_US) {
        force_dac_blanked_hold_xy_now();
    }
}

static void output_one_sample_timed(absolute_time_t *next_time)
{
    int16_t sample[RP_SAMPLE_CHANNELS];

    uint32_t rate = s_sample_rate_hz;

    if (rate < 1) {
        rate = 1;
    }

    if (rate > 200000) {
        rate = 200000;
    }

    int64_t period_us = 1000000LL / rate;

    if (period_us < 5) {
        period_us = 5;
    }

    *next_time = delayed_by_us(*next_time, period_us);
    sleep_until(*next_time);

    if (!s_running) {
        return;
    }

    if (ring_pop(sample)) {
        set_output_mode(OUTPUT_MODE_STREAMING);
        note_valid_sample(sample);
        apply_output_mirror(sample);

        critical_section_enter_blocking(&s_output_cs);
        memcpy(s_last_output, sample, RP_SAMPLE_CHANNELS * sizeof(int16_t));
        s_consumed_samples++;
        critical_section_exit(&s_output_cs);

        if (s_running) {
            dac_write_sample(sample);
        } else {
            dac_write_all_zero();
        }
    } else {
        int16_t blanked[RP_SAMPLE_CHANNELS];

        note_output_starved();
        sample_blanked_hold_xy(blanked);
        apply_output_mirror(blanked);

        critical_section_enter_blocking(&s_output_cs);
        s_underruns++;
        memcpy(s_last_output, blanked, sizeof(blanked));
        critical_section_exit(&s_output_cs);

        failsafe_check_no_samples();

        /*
           Active underrun:
           blank laser channels, but do not slam X/Y to raw 0x0000.
        */
        dac_write_sample(blanked);
    }
}

static void output_batch(void)
{
    uint32_t got = 0;

    if (s_running) {
        got = ring_pop_block(s_batch, OUTPUT_BATCH_SAMPLES);
    }

    for (uint32_t i = 0; i < got; i++) {
        note_valid_sample(s_batch[i]);
        apply_output_mirror(s_batch[i]);
    }

    for (uint32_t i = got; i < OUTPUT_BATCH_SAMPLES; i++) {
        sample_blanked_hold_xy(s_batch[i]);
        apply_output_mirror(s_batch[i]);
    }

    if (got > 0) {
        s_last_valid_sample_time = get_absolute_time();
    }

    uint32_t missing = OUTPUT_BATCH_SAMPLES - got;

    if (s_running) {
        if (got == 0) {
            note_output_starved();
        } else {
            set_output_mode(missing > 0 ? OUTPUT_MODE_UNDERRUN_BLANK : OUTPUT_MODE_STREAMING);
        }
    }

    if (s_running) {
        critical_section_enter_blocking(&s_output_cs);
        s_consumed_samples += got;
        s_underruns += missing;
        memcpy(s_last_output,
               s_batch[OUTPUT_BATCH_SAMPLES - 1],
               RP_SAMPLE_CHANNELS * sizeof(int16_t));
        critical_section_exit(&s_output_cs);
    }

    if (missing > 0) {
        failsafe_check_no_samples();
    }

    if (s_running) {
        /*
           Missing samples inside the batch are now blanked-hold-XY samples.
           No all-zero scanner jumps are injected during active streaming.
        */
        dac_write_sample_batch((const int16_t (*)[RP_SAMPLE_CHANNELS])s_batch, OUTPUT_BATCH_SAMPLES);
    } else {
        dac_write_all_zero();
    }
}

static void stop_stream_state(void)
{
    s_running = false;
    set_trace_running(false);
    set_output_mode(OUTPUT_MODE_HARD_ZERO);

    ring_clear();
    set_last_output_hard_zero();

    s_have_valid_xy = false;
    s_last_valid_x = 0;
    s_last_valid_y = 0;

    s_last_valid_sample_time = get_absolute_time();
    s_last_forced_blank_time = s_last_valid_sample_time;
}

static void output_core_main(void)
{
    absolute_time_t next_time = get_absolute_time();

    flash_safe_execute_core_init();

    while (true) {
        uint32_t rate = s_sample_rate_hz;

        if (!s_running) {
            sleep_ms(1);
            next_time = get_absolute_time();
            continue;
        }

        if (rate < BATCH_MODE_MIN_RATE_HZ) {
            output_one_sample_timed(&next_time);
        } else {
            output_batch();
        }
    }
}

static void launch_output_core(void)
{
    if (!s_core1_launched) {
        multicore_launch_core1(output_core_main);
        s_core1_launched = true;
    }
}

void output_engine_init(void)
{
    critical_section_init(&s_output_cs);
    trace_pins_init();

    set_last_output_hard_zero();

    s_have_valid_xy = false;
    s_last_valid_x = 0;
    s_last_valid_y = 0;

    s_last_valid_sample_time = get_absolute_time();
    s_last_forced_blank_time = s_last_valid_sample_time;

    dac_init();

    /*
       Critical boot-time sync:
       dac_init() initializes DAC timing internally, but the output engine
       owns the real output rate.
    */
    dac_set_sample_rate(s_sample_rate_hz);

    dac_write_all_zero();

    launch_output_core();
}

void output_engine_start(void)
{
    s_last_valid_sample_time = get_absolute_time();
    s_last_forced_blank_time = s_last_valid_sample_time;

    /*
       Until the first real point arrives, keep scanner centered
       and laser/color/intensity blanked.
    */
    s_have_valid_xy = false;
    s_last_valid_x = 0;
    s_last_valid_y = 0;

    dac_write_blanked_xy(0, 0);

    set_output_mode(OUTPUT_MODE_START_BLANK);
    set_last_output_blanked_hold_xy();

    s_running = true;
    set_trace_running(true);
}

void output_engine_stop_safe(void)
{
    stop_stream_state();

    /*
       Fully stopped state:
       hard raw-zero on every physical DAC channel.
    */
    dac_write_all_zero();
    dac_write_all_zero();
    dac_write_all_zero();
}

void output_engine_pause_for_flash(void)
{
    stop_stream_state();
    sleep_ms(5);
}

void output_engine_resume_after_flash(void)
{
    s_last_valid_sample_time = get_absolute_time();
    s_last_forced_blank_time = s_last_valid_sample_time;
    launch_output_core();
}

void output_engine_set_rate(uint32_t rate_hz)
{
    if (rate_hz < 1) {
        rate_hz = 1;
    }

    if (rate_hz > 200000) {
        rate_hz = 200000;
    }

    s_sample_rate_hz = rate_hz;
    dac_set_sample_rate(rate_hz);
}

void output_engine_set_mirror(bool mirror_x, bool mirror_y)
{
    s_mirror_x = mirror_x;
    s_mirror_y = mirror_y;
}

void output_engine_get_mirror(bool *mirror_x, bool *mirror_y)
{
    if (mirror_x) {
        *mirror_x = s_mirror_x;
    }

    if (mirror_y) {
        *mirror_y = s_mirror_y;
    }
}

void output_engine_get_status(output_engine_status_t *status)
{
    if (!status) {
        return;
    }

    critical_section_enter_blocking(&s_output_cs);

    status->running = s_running;
    status->sample_rate_hz = s_sample_rate_hz;
    status->consumed_samples = s_consumed_samples;
    status->underruns = s_underruns;
    status->failsafe_blank_events = s_failsafe_blank_events;
    status->mode_transitions = s_mode_transitions;
    status->mode = s_output_mode;
    status->mirror_x = s_mirror_x;
    status->mirror_y = s_mirror_y;

    memcpy(status->last_output, s_last_output, sizeof(status->last_output));

    critical_section_exit(&s_output_cs);
}
