#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "app_config.h"
#include "ilda_player.h"
#include "rp_manager.h"
#include "protocol_stream_stats.h"

static const char *TAG = "ilda";

#define PSRAM_LIMIT_BYTES (1400 * 1024)
#define ILDA_LOOP_RETURN_BLANK_SAMPLES 64

typedef struct {
    int16_t ch[RP_SAMPLE_CHANNELS];
} sample_t;

static TaskHandle_t s_sender_task = NULL;
static volatile bool s_stop = false;

static sample_t *s_samples = NULL;
static size_t s_sample_count = 0;

#if BOARD_VERSION == 1
#define ILDA_SENDER_CORE 0
#define ILDA_SENDER_PRIORITY 4   /* above DNS(3), below IDN/ED feed(5) and WiFi(18+) */
#define ILDA_PACKET_PACE_TICKS 0  /* output_task is on core 1; no sleep needed between packets */
#else
#define ILDA_SENDER_CORE 1
#define ILDA_SENDER_PRIORITY 5
#define ILDA_PACKET_PACE_TICKS 0
#endif

static inline TickType_t delay_ticks_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

static inline void suspend_idle0_wdt_for_v1_sender(void) {}
static inline void resume_idle0_wdt_after_v1_sender(void) {}

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static int16_t be_i16(const uint8_t *p)
{
    return (int16_t)be16(p);
}

static int16_t color_to_sample(uint8_t c)
{
    return (int16_t)(((int32_t)c * 65535 / 255) - 32768);
}

static void default_index_color(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /*
       ILDA format 0/1 uses an indexed palette. Many simple ILDA files do not
       include a format-2 palette section and instead rely on the common ILDA
       default palette. TEST_12K.ILD is one of those files: it uses indices
       0, 8, 16, 24, 40, 48, 56.

       The previous code used idx & 7, so all multiples of 8 became index 0
       -> black. That made the mirrors move while the laser stayed off.

       Keep compatibility with tiny 0..7 palettes, but also support common
       64-entry ILDA-style indices by mapping 0,8,16,...,56 to 0..7.
    */
    static const uint8_t pal8[8][3] = {
        {0,   0,   0  }, /* 0 black */
        {255, 0,   0  }, /* 1 red */
        {0,   255, 0  }, /* 2 green */
        {255, 255, 0  }, /* 3 yellow */
        {0,   0,   255}, /* 4 blue */
        {255, 0,   255}, /* 5 magenta */
        {0,   255, 255}, /* 6 cyan */
        {255, 255, 255}, /* 7 white */
    };

    uint8_t pal_index;

    if (idx < 8) {
        pal_index = idx;
    } else {
        pal_index = idx >> 3;
        if (pal_index > 7) {
            pal_index = 7;
        }
    }

    const uint8_t *c = pal8[pal_index];
    *r = c[0];
    *g = c[1];
    *b = c[2];
}

static int record_size_for_format(uint8_t format)
{
    if (format == 0) return 8;
    if (format == 1) return 6;
    if (format == 4) return 10;
    if (format == 5) return 8;
    return -1;
}

static void make_sample(
    int16_t x,
    int16_t y,
    bool blanked,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    sample_t *out)
{
    out->ch[0] = x;
    out->ch[1] = y;

    if (blanked) {
        out->ch[2] = -32768;
        out->ch[3] = -32768;
        out->ch[4] = -32768;
        out->ch[5] = -32768;
    } else {
        out->ch[2] = color_to_sample(r);
        out->ch[3] = color_to_sample(g);
        out->ch[4] = color_to_sample(b);

        uint8_t i = r;
        if (g > i) i = g;
        if (b > i) i = b;

        out->ch[5] = color_to_sample(i);
    }

    out->ch[6] = 0;
    out->ch[7] = 0;
}

static void make_blanked_hold_sample(const int16_t source[RP_SAMPLE_CHANNELS],
                                     int16_t out[RP_SAMPLE_CHANNELS])
{
    out[0] = source[0];
    out[1] = source[1];

    for (int ch = 2; ch < RP_SAMPLE_CHANNELS; ch++) {
        out[ch] = -32768;
    }
}

static int16_t lerp_sample_channel(int16_t from, int16_t to, uint32_t step, uint32_t steps)
{
    int32_t delta = (int32_t)to - (int32_t)from;
    return (int16_t)((int32_t)from + ((delta * (int32_t)step) / (int32_t)steps));
}

static void make_blanked_return_sample(const int16_t from[RP_SAMPLE_CHANNELS],
                                       const int16_t to[RP_SAMPLE_CHANNELS],
                                       uint32_t step,
                                       int16_t out[RP_SAMPLE_CHANNELS])
{
    out[0] = lerp_sample_channel(from[0], to[0], step, ILDA_LOOP_RETURN_BLANK_SAMPLES);
    out[1] = lerp_sample_channel(from[1], to[1], step, ILDA_LOOP_RETURN_BLANK_SAMPLES);

    for (int ch = 2; ch < RP_SAMPLE_CHANNELS; ch++) {
        out[ch] = -32768;
    }
}

static void parse_record(uint8_t format, const uint8_t *rec, sample_t *out)
{
    int16_t x = 0;
    int16_t y = 0;
    bool blanked = false;
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;

    if (format == 0) {
        x = be_i16(&rec[0]);
        y = be_i16(&rec[2]);
        blanked = (rec[6] & 0x40) != 0;
        default_index_color(rec[7], &r, &g, &b);
    } else if (format == 1) {
        x = be_i16(&rec[0]);
        y = be_i16(&rec[2]);
        blanked = (rec[4] & 0x40) != 0;
        default_index_color(rec[5], &r, &g, &b);
    } else if (format == 4) {
        x = be_i16(&rec[0]);
        y = be_i16(&rec[2]);
        blanked = (rec[6] & 0x40) != 0;
        b = rec[7];
        g = rec[8];
        r = rec[9];
    } else if (format == 5) {
        x = be_i16(&rec[0]);
        y = be_i16(&rec[2]);
        blanked = (rec[4] & 0x40) != 0;
        b = rec[5];
        g = rec[6];
        r = rec[7];
    }

    make_sample(x, y, blanked, r, g, b, out);
}

static void free_samples(void)
{
    if (s_samples) {
        free(s_samples);
        s_samples = NULL;
    }

    s_sample_count = 0;
}

static esp_err_t preflight_ilda(FILE *f, size_t *sample_count)
{
    uint8_t discard[256];
    size_t total_samples = 0;
    size_t max_samples = PSRAM_LIMIT_BYTES / sizeof(sample_t);

    if (fseek(f, 0, SEEK_SET) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    while (!s_stop) {
        uint8_t header[32];
        size_t header_bytes = fread(header, 1, sizeof(header), f);
        if (header_bytes != sizeof(header)) {
            if (ferror(f)) {
                return ESP_FAIL;
            }
            if (header_bytes != 0) {
                return ESP_ERR_INVALID_SIZE;
            }
            break;
        }

        if (memcmp(header, "ILDA", 4) != 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        uint8_t format = header[7];
        uint16_t records = be16(&header[24]);
        if (records == 0) {
            break;
        }

        int rec_size = record_size_for_format(format);
        if (rec_size <= 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        if ((size_t)records > max_samples - total_samples) {
            ESP_LOGE(TAG, "ILDA exceeds memory limit of %u samples", (unsigned int)max_samples);
            return ESP_ERR_NO_MEM;
        }

        total_samples += records;
        size_t remaining = (size_t)records * (size_t)rec_size;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
            if (fread(discard, 1, chunk, f) != chunk) {
                return ESP_ERR_INVALID_SIZE;
            }
            remaining -= chunk;
        }

        taskYIELD();
    }

    if (s_stop) {
        return ESP_ERR_INVALID_STATE;
    }
    if (total_samples == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    *sample_count = total_samples;
    return ESP_OK;
}

static esp_err_t load_ilda_to_psram(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_FAIL;
    }

    free_samples();

    size_t max_samples = PSRAM_LIMIT_BYTES / sizeof(sample_t);
    size_t expected_samples = 0;
    esp_err_t ret = preflight_ilda(f, &expected_samples);
    if (ret != ESP_OK) {
        fclose(f);
        ESP_LOGE(TAG, "ILDA preflight failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_samples = heap_caps_malloc(expected_samples * sizeof(sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_samples) {
        s_samples = heap_caps_malloc(expected_samples * sizeof(sample_t), MALLOC_CAP_8BIT);
    }

    if (!s_samples) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate sample memory");
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;

    while (!s_stop) {
        uint8_t header[32];

        if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
            break;
        }

        if (memcmp(header, "ILDA", 4) != 0) {
            ESP_LOGE(TAG, "Bad ILDA header");
            fclose(f);
            free_samples();
            return ESP_ERR_INVALID_RESPONSE;
        }

        uint8_t format = header[7];
        uint16_t records = be16(&header[24]);

        if (records == 0) {
            break;
        }

        int rec_size = record_size_for_format(format);
        if (rec_size <= 0) {
            ESP_LOGE(TAG, "Unsupported ILDA format %u", format);
            fclose(f);
            free_samples();
            return ESP_ERR_NOT_SUPPORTED;
        }

        uint8_t rec[10];

        for (uint16_t i = 0; i < records && !s_stop; i++) {
            if (fread(rec, 1, rec_size, f) != (size_t)rec_size) {
                fclose(f);
                free_samples();
                return ESP_ERR_INVALID_SIZE;
            }

            if (count >= expected_samples || count >= max_samples) {
                fclose(f);
                free_samples();
                ESP_LOGE(TAG, "ILDA too large for memory. Max converted samples=%u", (unsigned int)max_samples);
                return ESP_ERR_NO_MEM;
            }

            parse_record(format, rec, &s_samples[count]);
            count++;
        }

        taskYIELD();
    }

    fclose(f);

    if (s_stop) {
        free_samples();
        return ESP_ERR_INVALID_STATE;
    }

    if (count != expected_samples) {
        free_samples();
        ESP_LOGE(TAG, "ILDA changed while loading: expected %u samples, read %u",
                 (unsigned int)expected_samples, (unsigned int)count);
        return ESP_ERR_INVALID_SIZE;
    }

    if (count == 0) {
        free_samples();
        ESP_LOGE(TAG, "ILDA file contains no usable samples");
        return ESP_ERR_INVALID_SIZE;
    }

    s_sample_count = count;

    ESP_LOGI(TAG, "Loaded %u samples into memory (%u bytes)",
             (unsigned int)s_sample_count,
             (unsigned int)(s_sample_count * sizeof(sample_t)));

    return ESP_OK;
}

static esp_err_t send_packet_and_maybe_start(
    int16_t packet[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS],
    uint32_t real_samples,
    bool *output_started)
{
    esp_err_t ret = rp_manager_send_samples_packet(packet);

    if (ret == ESP_OK && !s_stop && !*output_started) {
        rp_manager_output_start();
        *output_started = true;
    }

    if (ret == ESP_OK) {
        protocol_stream_stats_add_samples(PROTOCOL_STREAM_SD_ILDA, real_samples);
    }

    return ret;
}

static void sender_task(void *arg)
{
    (void)arg;

    bool output_started = false;
    uint32_t consecutive_failures = 0;

    suspend_idle0_wdt_for_v1_sender();

    int16_t (*packet)[RP_SAMPLE_CHANNELS] = heap_caps_malloc(
        RP_STREAM_SAMPLES_PER_PACKET * RP_SAMPLE_CHANNELS * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA
    );

    if (!packet) {
        ESP_LOGE(TAG, "Failed to allocate ILDA packet buffer");
        s_sender_task = NULL;
        free_samples();
        resume_idle0_wdt_after_v1_sender();
        vTaskDelete(NULL);
        return;
    }

    while (!s_stop) {
        uint32_t pc = 0;

        for (size_t i = 0; i < s_sample_count && !s_stop; i++) {
            memcpy(packet[pc], s_samples[i].ch, sizeof(packet[pc]));
            pc++;

            if (pc >= RP_STREAM_SAMPLES_PER_PACKET) {
                esp_err_t ret = send_packet_and_maybe_start(packet, pc, &output_started);

                if (ret != ESP_OK) {
                    consecutive_failures++;
                    if (consecutive_failures == 1) {
                        ESP_LOGW(TAG, "ILDA packet send failed: %s (flow-control pause)", esp_err_to_name(ret));
                    }
                    vTaskDelay(delay_ticks_ms(250));
                } else {
                    if (consecutive_failures > 1) {
                        ESP_LOGI(TAG, "ILDA packet send resumed after %lu failures", (unsigned long)consecutive_failures);
                    }
                    consecutive_failures = 0;
                }

                pc = 0;
                if (ILDA_PACKET_PACE_TICKS > 0) {
                    vTaskDelay(ILDA_PACKET_PACE_TICKS);
                } else {
                    taskYIELD();
                }
            }
        }

        if (!s_stop && s_sample_count > 1) {
            const int16_t *last = s_samples[s_sample_count - 1].ch;
            const int16_t *first = s_samples[0].ch;

            for (uint32_t step = 1; step <= ILDA_LOOP_RETURN_BLANK_SAMPLES && !s_stop; step++) {
                make_blanked_return_sample(last, first, step, packet[pc]);
                pc++;

                if (pc >= RP_STREAM_SAMPLES_PER_PACKET) {
                    esp_err_t ret = send_packet_and_maybe_start(packet, pc, &output_started);

                    if (ret != ESP_OK) {
                        consecutive_failures++;
                        if (consecutive_failures == 1) {
                            ESP_LOGW(TAG, "ILDA loop blanking packet send failed: %s (flow-control pause)", esp_err_to_name(ret));
                        }
                        vTaskDelay(delay_ticks_ms(250));
                    } else {
                        if (consecutive_failures > 1) {
                            ESP_LOGI(TAG, "ILDA packet send resumed after %lu failures", (unsigned long)consecutive_failures);
                        }
                        consecutive_failures = 0;
                    }

                    pc = 0;
                    if (ILDA_PACKET_PACE_TICKS > 0) {
                        vTaskDelay(ILDA_PACKET_PACE_TICKS);
                    } else {
                        taskYIELD();
                    }
                }
            }
        }

        if (!s_stop && pc > 0) {
            uint32_t real_pc = pc;
            while (pc < RP_STREAM_SAMPLES_PER_PACKET) {
                make_blanked_hold_sample(packet[pc - 1], packet[pc]);
                pc++;
            }

            esp_err_t ret = send_packet_and_maybe_start(packet, real_pc, &output_started);

            if (ret != ESP_OK) {
                consecutive_failures++;
                if (consecutive_failures == 1) {
                    ESP_LOGW(TAG, "ILDA final packet send failed: %s (flow-control pause)", esp_err_to_name(ret));
                }
                vTaskDelay(delay_ticks_ms(250));
            } else {
                if (consecutive_failures > 1) {
                    ESP_LOGI(TAG, "ILDA packet send resumed after %lu failures", (unsigned long)consecutive_failures);
                }
                consecutive_failures = 0;
            }

            if (ILDA_PACKET_PACE_TICKS > 0) {
                vTaskDelay(ILDA_PACKET_PACE_TICKS);
            }
        }

    }

    free(packet);
    if (output_started) {
        rp_manager_output_stop();
    }
    s_sender_task = NULL;
    free_samples();
    resume_idle0_wdt_after_v1_sender();

    vTaskDelete(NULL);
}

esp_err_t ilda_player_start(const char *path)
{
    if (!path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ilda_player_stop_and_wait(5000);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = rp_manager_output_force_stop();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop previous RP output before ILDA start: %s", esp_err_to_name(ret));
    }

    ret = rp_manager_clear_buffer();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear RP buffer before ILDA start: %s", esp_err_to_name(ret));
    }

    s_stop = false;

    ESP_LOGI(TAG, "Starting ILDA memory playback: %s", path);

    ret = load_ilda_to_psram(path);
    if (ret != ESP_OK) {
        s_stop = true;
        free_samples();
        ESP_LOGE(TAG, "ILDA load failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        sender_task,
        "ilda_sender",
        12288,
        NULL,
        ILDA_SENDER_PRIORITY,
        &s_sender_task,
        ILDA_SENDER_CORE
    );

    if (ok != pdPASS) {
        s_stop = true;
        free_samples();
        s_sender_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ilda_player_stop(void)
{
    s_stop = true;
    return ESP_OK;
}

esp_err_t ilda_player_stop_and_wait(uint32_t timeout_ms)
{
    s_stop = true;

    uint32_t waited_ms = 0;
    while (s_sender_task && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }

    return s_sender_task ? ESP_ERR_TIMEOUT : ESP_OK;
}

bool ilda_player_is_running(void)
{
    return s_sender_task != NULL;
}
