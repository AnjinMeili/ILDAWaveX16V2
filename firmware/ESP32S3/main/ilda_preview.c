#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "ilda_format.h"
#include "ilda_player.h"
#include "ilda_preview.h"

static const char *TAG = "ilda_preview";

#define ILDA_PREVIEW_CACHE_PATH_MAX 192
#define ILDA_PREVIEW_READ_BYTES 4096
#define ILDA_HEADER_BYTES 32

typedef struct {
    uint32_t payload_offset;
    uint16_t point_count;
    uint8_t format;
} preview_frame_index_t;

/*
   Frame index for the most recently scanned file. Animated previews and
   frame stepping reuse it instead of re-walking every header on the SD card.
   Only the web server's preview worker calls into this module.
*/
static preview_frame_index_t s_frame_index[ILDA_PREVIEW_MAX_ANIMATED_FRAMES];
static uint32_t s_frame_index_count = 0;
static uint64_t s_cached_file_size = 0;
static char s_cached_path[ILDA_PREVIEW_CACHE_PATH_MAX];
static ilda_preview_result_t s_cached_summary;

static void decode_point(uint8_t format, const uint8_t *record,
                         ilda_preview_point_t *point)
{
    point->x = (int16_t)ilda_be16(record);
    point->y = (int16_t)ilda_be16(record + 2);

    if (format == 0) {
        point->blanked = (record[6] & 0x40) != 0;
        ilda_default_index_color(record[7], &point->r, &point->g, &point->b);
    } else if (format == 1) {
        point->blanked = (record[4] & 0x40) != 0;
        ilda_default_index_color(record[5], &point->r, &point->g, &point->b);
    } else if (format == 4) {
        point->blanked = (record[6] & 0x40) != 0;
        point->b = record[7];
        point->g = record[8];
        point->r = record[9];
    } else {
        point->blanked = (record[4] & 0x40) != 0;
        point->b = record[5];
        point->g = record[6];
        point->r = record[7];
    }
}

static esp_err_t read_preview_frame(FILE *file, uint8_t format,
                                    uint16_t record_count,
                                    ilda_preview_result_t *result)
{
    uint8_t *records_buffer = malloc(ILDA_PREVIEW_READ_BYTES);
    if (!records_buffer) {
        return ESP_ERR_NO_MEM;
    }

    int record_size = ilda_record_size_for_format(format);
    size_t records_per_chunk = ILDA_PREVIEW_READ_BYTES / (size_t)record_size;
    uint16_t processed = 0;
    uint32_t preview_slot = 0;

    result->selected_frame_points = record_count;
    result->preview_point_count = 0;

    while (processed < record_count) {
        size_t batch_count = record_count - processed;
        if (batch_count > records_per_chunk) {
            batch_count = records_per_chunk;
        }

        if (fread(records_buffer, (size_t)record_size, batch_count, file) != batch_count) {
            esp_err_t ret = ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
            free(records_buffer);
            return ret;
        }

        /*
           Frames larger than ILDA_PREVIEW_MAX_POINTS are decimated evenly,
           always keeping the first and last point.
        */
        for (size_t i = 0; i < batch_count; i++) {
            uint32_t record_index = (uint32_t)processed + (uint32_t)i;
            bool capture = record_count <= ILDA_PREVIEW_MAX_POINTS ||
                record_index == (preview_slot * (record_count - 1U)) /
                                (ILDA_PREVIEW_MAX_POINTS - 1U);

            if (capture && preview_slot < ILDA_PREVIEW_MAX_POINTS) {
                decode_point(format, &records_buffer[i * (size_t)record_size],
                             &result->points[preview_slot]);
                preview_slot++;
            }
        }

        result->preview_point_count = preview_slot;
        processed += (uint16_t)batch_count;
        taskYIELD();
    }

    free(records_buffer);
    return ESP_OK;
}

static bool cache_matches(const char *path, uint64_t file_size)
{
    return s_frame_index_count > 0 &&
           file_size == s_cached_file_size &&
           strcmp(path, s_cached_path) == 0;
}

static bool cached_frame_header_matches(FILE *file, const preview_frame_index_t *frame)
{
    uint8_t header[ILDA_HEADER_BYTES];

    if (frame->payload_offset < sizeof(header) ||
        fseek(file, (long)(frame->payload_offset - sizeof(header)), SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return false;
    }

    return memcmp(header, "ILDA", 4) == 0 &&
           header[7] == frame->format &&
           ilda_be16(&header[24]) == frame->point_count;
}

esp_err_t ilda_preview_analyze(const char *path, uint32_t selected_frame,
                               uint64_t file_size, ilda_preview_result_t *result)
{
    if (!path || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->selected_frame = selected_frame;
    result->max_samples = ilda_player_max_samples();

    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_FAIL;
    }

    if (cache_matches(path, file_size)) {
        if (selected_frame >= s_frame_index_count) {
            fclose(file);
            return ESP_ERR_NOT_FOUND;
        }

        const preview_frame_index_t *frame = &s_frame_index[selected_frame];

        if (cached_frame_header_matches(file, frame)) {
            *result = s_cached_summary;
            result->selected_frame = selected_frame;
            result->selected_frame_points = 0;
            result->preview_point_count = 0;

            esp_err_t read_ret = read_preview_frame(file, frame->format,
                                                    frame->point_count, result);
            fclose(file);
            return read_ret;
        }

        /* Same path and size but different content: rebuild the index. */
        ESP_LOGW(TAG, "Stale frame index for %s, rescanning", path);
        if (fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            return ESP_FAIL;
        }
    }

    s_frame_index_count = 0;
    s_cached_path[0] = '\0';

    esp_err_t ret = ESP_OK;
    uint64_t file_offset = 0;
    bool index_cacheable = true;

    while (true) {
        uint8_t header[ILDA_HEADER_BYTES];
        size_t header_bytes = fread(header, 1, sizeof(header), file);

        if (header_bytes != sizeof(header)) {
            if (ferror(file)) {
                ESP_LOGW(TAG, "%s: read error at offset %llu", path,
                         (unsigned long long)file_offset);
                ret = ESP_FAIL;
            } else if (header_bytes != 0) {
                /* Matches the player: a short tail is treated as end of file. */
                ESP_LOGW(TAG, "%s: ignoring %u trailing bytes at offset %llu", path,
                         (unsigned)header_bytes, (unsigned long long)file_offset);
            }
            break;
        }

        file_offset += sizeof(header);

        if (memcmp(header, "ILDA", 4) != 0) {
            ESP_LOGW(TAG, "%s: bad ILDA header at offset %llu", path,
                     (unsigned long long)(file_offset - sizeof(header)));
            ret = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        uint8_t format = header[7];
        uint16_t record_count = ilda_be16(&header[24]);
        if (record_count == 0) {
            break;
        }

        if (file_offset == file_size) {
            /*
               Some exporters write the closing header with the last frame's
               point count instead of zero. A header with nothing after it
               can only be the end-of-file marker.
            */
            break;
        }

        int record_size = ilda_record_size_for_format(format);
        if (record_size < 0) {
            ESP_LOGW(TAG, "%s: unsupported format %u at offset %llu", path, format,
                     (unsigned long long)(file_offset - sizeof(header)));
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        if (UINT32_MAX - result->total_points < record_count ||
            result->frame_count == UINT32_MAX) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        uint32_t frame_index = result->frame_count++;
        result->total_points += record_count;

        if (record_count > result->max_frame_points) {
            result->max_frame_points = record_count;
        }

        result->format_mask |= 1U << format;

        uint64_t payload_bytes = (uint64_t)record_count * (uint64_t)record_size;
        if (file_offset > file_size || payload_bytes > file_size - file_offset) {
            ESP_LOGW(TAG, "%s: frame at offset %llu needs %llu point bytes, file has %llu",
                     path, (unsigned long long)(file_offset - sizeof(header)),
                     (unsigned long long)payload_bytes,
                     (unsigned long long)(file_size - file_offset));
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        if (index_cacheable && frame_index < ILDA_PREVIEW_MAX_ANIMATED_FRAMES &&
            file_offset <= UINT32_MAX) {
            s_frame_index[frame_index] = (preview_frame_index_t){
                .payload_offset = (uint32_t)file_offset,
                .point_count = record_count,
                .format = format,
            };
        } else {
            index_cacheable = false;
        }

        if (frame_index != selected_frame) {
            if (fseek(file, (long)payload_bytes, SEEK_CUR) != 0) {
                ret = ESP_FAIL;
                break;
            }

            file_offset += payload_bytes;
            taskYIELD();
            continue;
        }

        ret = read_preview_frame(file, format, record_count, result);
        file_offset += payload_bytes;

        if (ret != ESP_OK) {
            break;
        }
    }

    fclose(file);

    if (ret != ESP_OK) {
        return ret;
    }

    if (result->frame_count == 0 || result->total_points == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (selected_frame >= result->frame_count) {
        return ESP_ERR_NOT_FOUND;
    }

    result->within_sample_limit = result->total_points <= result->max_samples;

    if (index_cacheable && result->frame_count <= ILDA_PREVIEW_MAX_ANIMATED_FRAMES) {
        s_frame_index_count = result->frame_count;
        s_cached_file_size = file_size;
        strlcpy(s_cached_path, path, sizeof(s_cached_path));
        s_cached_summary = *result;
        memset(s_cached_summary.points, 0, sizeof(s_cached_summary.points));
    }

    return ESP_OK;
}

esp_err_t ilda_preview_stream_frames(const char *path, uint64_t file_size,
                                     ilda_preview_frame_callback_t callback,
                                     void *context,
                                     ilda_preview_result_t *summary)
{
    if (!path || !callback || !summary) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ilda_preview_analyze(path, 0, file_size, summary);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!cache_matches(path, file_size) || s_frame_index_count != summary->frame_count) {
        ESP_LOGW(TAG, "Frame index unavailable for %s (%lu frames)",
                 path, (unsigned long)summary->frame_count);
        return ESP_ERR_NOT_SUPPORTED;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_FAIL;
    }

    ilda_preview_result_t *frame_result = calloc(1, sizeof(*frame_result));
    if (!frame_result) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t i = 0; i < s_frame_index_count; i++) {
        const preview_frame_index_t *frame = &s_frame_index[i];
        uint8_t header[ILDA_HEADER_BYTES];

        if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
            memcmp(header, "ILDA", 4) != 0 ||
            header[7] != frame->format ||
            ilda_be16(&header[24]) != frame->point_count) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        memset(frame_result, 0, sizeof(*frame_result));
        ret = read_preview_frame(file, frame->format, frame->point_count, frame_result);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Frame %lu read failed: %s",
                     (unsigned long)i, esp_err_to_name(ret));
            break;
        }

        /* A callback failure is normally the browser closing the request. */
        ret = callback(context, i, frame->point_count,
                       frame_result->points, frame_result->preview_point_count);
        if (ret != ESP_OK) {
            break;
        }

        taskYIELD();
    }

    fclose(file);
    free(frame_result);
    return ret;
}
