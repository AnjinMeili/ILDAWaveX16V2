#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ILDA_PREVIEW_MAX_POINTS 256
#define ILDA_PREVIEW_MAX_ANIMATED_FRAMES 1024

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    bool blanked;
} ilda_preview_point_t;

typedef struct {
    uint32_t frame_count;
    uint32_t total_points;
    uint32_t max_frame_points;
    uint32_t selected_frame;
    uint32_t selected_frame_points;
    uint32_t preview_point_count;
    uint32_t format_mask;
    size_t max_samples;
    bool within_sample_limit;
    ilda_preview_point_t points[ILDA_PREVIEW_MAX_POINTS];
} ilda_preview_result_t;

typedef esp_err_t (*ilda_preview_frame_callback_t)(
    void *context, uint32_t frame_index, uint16_t frame_points,
    const ilda_preview_point_t *points, uint32_t point_count);

esp_err_t ilda_preview_analyze(const char *path, uint32_t selected_frame,
                               uint64_t file_size, ilda_preview_result_t *result);
esp_err_t ilda_preview_stream_frames(const char *path, uint64_t file_size,
                                     ilda_preview_frame_callback_t callback,
                                     void *context,
                                     ilda_preview_result_t *summary);