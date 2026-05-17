#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RP_SAMPLE_CHANNELS 8

#define RP_STREAM_SAMPLES_PER_PACKET 256
#define RP_STREAM_HEADER_BYTES 12
#define RP_STREAM_PAYLOAD_BYTES (RP_STREAM_SAMPLES_PER_PACKET * RP_SAMPLE_CHANNELS * 2)
#define RP_STREAM_PACKET_BYTES (RP_STREAM_HEADER_BYTES + RP_STREAM_PAYLOAD_BYTES)

typedef enum {
    RP_TEST_SHAPE_BEAM = 0,
    RP_TEST_SHAPE_SQUARE,
    RP_TEST_SHAPE_CIRCLE,
    RP_TEST_SHAPE_CROSS,
    RP_TEST_SHAPE_HEART,
} rp_test_shape_t;

typedef struct {
    bool initialized;
    uint32_t ping_count;
    uint32_t error_count;

    esp_err_t last_error;

    bool remote_running;
    uint32_t remote_sample_rate;
    uint32_t remote_payload_errors;

    uint32_t remote_buffer_fill_samples;
    uint32_t remote_buffer_size_samples;
    uint32_t remote_buffer_high_water_samples;
    uint32_t remote_buffer_low_water_samples;
    uint32_t remote_buffer_max_samples;
    bool remote_mirror_x;
    bool remote_mirror_y;
    uint32_t remote_underruns;

    uint32_t remote_stream_packets;
    uint32_t remote_stream_bad_packets;
    uint32_t remote_stream_dropped_packets;
    uint32_t remote_stream_rx_timeouts;

    bool test_stream_running;
    uint32_t test_stream_blocks_sent;
    char remote_fw_version[64];
} rp_status_t;

esp_err_t rp_manager_start(void);
esp_err_t rp_manager_ping(void);
esp_err_t rp_manager_get_remote_status(void);
esp_err_t rp_manager_output_start(void);
esp_err_t rp_manager_output_stop(void);
esp_err_t rp_manager_output_force_stop(void);
esp_err_t rp_manager_clear_buffer(void);
esp_err_t rp_manager_set_rate(uint32_t rate_hz);
esp_err_t rp_manager_set_buffer_config(uint32_t size_samples,
                                       uint32_t high_water_samples,
                                       uint32_t low_water_samples);
esp_err_t rp_manager_set_mirror(bool mirror_x, bool mirror_y);
void rp_manager_set_realtime_streaming(bool enabled);
esp_err_t rp_manager_send_test_block(void);
esp_err_t rp_manager_start_test_stream(void);
esp_err_t rp_manager_start_test_shape(rp_test_shape_t shape);
void rp_manager_set_test_color_mask(uint8_t mask);
esp_err_t rp_manager_stop_test_stream(void);
esp_err_t rp_manager_send_samples_packet(const int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS]);
esp_err_t rp_manager_wait_payload_idle(uint32_t timeout_ms);
void rp_manager_get_status(rp_status_t *status);
