#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include <strings.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "app_config.h"
#include "ilda_format.h"
#include "ilda_player.h"
#include "rp_manager.h"
#include "protocol_stream_stats.h"

static const char *TAG = "ilda";

#define PSRAM_LIMIT_BYTES (1400 * 1024)

typedef struct {
    int16_t ch[RP_SAMPLE_CHANNELS];
} sample_t;

static TaskHandle_t s_sender_task = NULL;
static volatile bool s_stop = false;
static volatile uint8_t s_scale_percent = 100;

static sample_t *s_samples = NULL;
static size_t s_sample_count = 0;
static uint32_t s_frame_count = 0;

typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
    size_t index;
    uint32_t hold_ms;
    uint16_t animation_plays;
} playlist_t;

/* Folder playback list; empty for single-file playback. Owned by the sender task while it runs. */
static playlist_t s_playlist;

static portMUX_TYPE s_current_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_current_path[ILDA_PATH_MAX];
static bool s_current_folder = false;

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

static int16_t be_i16(const uint8_t *p)
{
    return (int16_t)ilda_be16(p);
}

static int16_t color_to_sample(uint8_t c)
{
    return (int16_t)(((int32_t)c * 65535 / 255) - 32768);
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
                                       uint8_t scale_percent,
                                       int16_t out[RP_SAMPLE_CHANNELS])
{
    int16_t x = lerp_sample_channel(from[0], to[0], step, ILDA_LOOP_RETURN_BLANK_SAMPLES);
    int16_t y = lerp_sample_channel(from[1], to[1], step, ILDA_LOOP_RETURN_BLANK_SAMPLES);
    out[0] = (int16_t)((int32_t)x * scale_percent / 100);
    out[1] = (int16_t)((int32_t)y * scale_percent / 100);

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
        ilda_default_index_color(rec[7], &r, &g, &b);
    } else if (format == 1) {
        x = be_i16(&rec[0]);
        y = be_i16(&rec[2]);
        blanked = (rec[4] & 0x40) != 0;
        ilda_default_index_color(rec[5], &r, &g, &b);
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

size_t ilda_player_max_samples(void)
{
    return PSRAM_LIMIT_BYTES / sizeof(sample_t);
}

static esp_err_t check_ilda_file(const char *path, size_t *sample_count, uint32_t *frame_count)
{
    /*
       Validate the whole file before playback state is touched, so a bad
       file is rejected while the current output keeps running.
    */
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_FAIL;
    }

    uint8_t discard[256];
    size_t total_samples = 0;
    uint32_t total_frames = 0;
    size_t max_samples = ilda_player_max_samples();
    long offset = 0;
    esp_err_t ret = ESP_OK;

    while (true) {
        uint8_t header[32];
        size_t header_bytes = fread(header, 1, sizeof(header), f);

        if (header_bytes != sizeof(header)) {
            if (ferror(f)) {
                ESP_LOGE(TAG, "Read error at offset %ld", offset);
                ret = ESP_FAIL;
            } else if (header_bytes != 0) {
                /* The loader has always treated a short tail as end of file. */
                ESP_LOGW(TAG, "Ignoring %u trailing bytes at offset %ld",
                         (unsigned int)header_bytes, offset);
            }
            break;
        }

        if (memcmp(header, "ILDA", 4) != 0) {
            ESP_LOGE(TAG, "Bad ILDA header at offset %ld", offset);
            ret = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        uint8_t format = header[7];
        uint16_t records = ilda_be16(&header[24]);
        if (records == 0) {
            break;
        }

        int rec_size = ilda_record_size_for_format(format);
        if (rec_size <= 0) {
            ESP_LOGE(TAG, "Unsupported ILDA format %u at offset %ld", format, offset);
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        offset += (long)sizeof(header);

        size_t payload = (size_t)records * (size_t)rec_size;
        size_t remaining = payload;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
            size_t got = fread(discard, 1, chunk, f);
            remaining -= got;
            if (got != chunk) {
                break;
            }
        }

        if (remaining == payload && feof(f)) {
            /*
               Some exporters write the closing header with the last frame's
               point count instead of zero. A header with nothing after it
               can only be the end-of-file marker.
            */
            ESP_LOGW(TAG, "Header at offset %ld has no point data, treating it as end of file",
                     offset - (long)sizeof(header));
            break;
        }

        if (remaining > 0) {
            ESP_LOGE(TAG, "Frame at offset %ld is truncated: %u of %u point bytes missing",
                     offset - (long)sizeof(header), (unsigned int)remaining,
                     (unsigned int)payload);
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        if ((size_t)records > max_samples - total_samples) {
            ESP_LOGE(TAG, "ILDA exceeds memory limit of %u samples", (unsigned int)max_samples);
            ret = ESP_ERR_NO_MEM;
            break;
        }

        total_samples += records;
        total_frames++;
        offset += (long)payload;
        taskYIELD();
    }

    fclose(f);

    if (ret == ESP_OK && total_samples == 0) {
        ESP_LOGE(TAG, "ILDA file contains no points");
        ret = ESP_ERR_INVALID_SIZE;
    }

    if (ret == ESP_OK) {
        *sample_count = total_samples;
        *frame_count = total_frames;
    }

    return ret;
}

static esp_err_t load_ilda_to_psram(const char *path, size_t expected_samples)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_FAIL;
    }

    free_samples();

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

    while (!s_stop && count < expected_samples) {
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
        uint16_t records = ilda_be16(&header[24]);

        if (records == 0) {
            break;
        }

        int rec_size = ilda_record_size_for_format(format);
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

            if (count >= expected_samples) {
                fclose(f);
                free_samples();
                ESP_LOGE(TAG, "ILDA grew while loading: expected %u samples",
                         (unsigned int)expected_samples);
                return ESP_ERR_INVALID_SIZE;
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

typedef struct {
    int16_t (*packet)[RP_SAMPLE_CHANNELS];
    uint32_t pc;
    bool output_started;
    uint32_t consecutive_failures;
} sender_state_t;

static void sender_send_packet(sender_state_t *st, uint32_t real_samples, const char *what)
{
    esp_err_t ret = send_packet_and_maybe_start(st->packet, real_samples, &st->output_started);

    if (ret != ESP_OK) {
        st->consecutive_failures++;
        if (st->consecutive_failures == 1) {
            ESP_LOGW(TAG, "ILDA %s send failed: %s (flow-control pause)", what, esp_err_to_name(ret));
        }
        vTaskDelay(delay_ticks_ms(250));
    } else {
        if (st->consecutive_failures > 1) {
            ESP_LOGI(TAG, "ILDA packet send resumed after %lu failures", (unsigned long)st->consecutive_failures);
        }
        st->consecutive_failures = 0;
    }

    st->pc = 0;
}

static void sender_send_if_full(sender_state_t *st, const char *what)
{
    if (st->pc < RP_STREAM_SAMPLES_PER_PACKET) {
        return;
    }

    sender_send_packet(st, st->pc, what);

    if (ILDA_PACKET_PACE_TICKS > 0) {
        vTaskDelay(ILDA_PACKET_PACE_TICKS);
    } else {
        taskYIELD();
    }
}

static void sender_flush_partial(sender_state_t *st)
{
    if (st->pc == 0) {
        return;
    }

    uint32_t real_pc = st->pc;
    while (st->pc < RP_STREAM_SAMPLES_PER_PACKET) {
        make_blanked_hold_sample(st->packet[st->pc - 1], st->packet[st->pc]);
        st->pc++;
    }

    sender_send_packet(st, real_pc, "final packet");

    if (ILDA_PACKET_PACE_TICKS > 0) {
        vTaskDelay(ILDA_PACKET_PACE_TICKS);
    }
}

static void sender_blanked_move(sender_state_t *st,
                                const int16_t from[RP_SAMPLE_CHANNELS],
                                const int16_t to[RP_SAMPLE_CHANNELS],
                                uint8_t *scale_percent)
{
    for (uint32_t step = 1; step <= ILDA_LOOP_RETURN_BLANK_SAMPLES && !s_stop; step++) {
        if (st->pc == 0) {
            *scale_percent = s_scale_percent;
        }

        make_blanked_return_sample(from, to, step, *scale_percent, st->packet[st->pc]);
        st->pc++;
        sender_send_if_full(st, "loop blanking packet");
    }
}

/* ===================== FOLDER PLAYLIST ===================== */

static void set_current_file(const char *path, uint32_t frames, bool folder)
{
    s_frame_count = frames;

    taskENTER_CRITICAL(&s_current_lock);
    strlcpy(s_current_path, path ? path : "", sizeof(s_current_path));
    s_current_folder = folder;
    taskEXIT_CRITICAL(&s_current_lock);
}

static void playlist_free(playlist_t *list)
{
    for (size_t i = 0; i < list->count; i++) {
        free(list->paths[i]);
    }

    free(list->paths);
    memset(list, 0, sizeof(*list));
}

static esp_err_t playlist_add(playlist_t *list, const char *path)
{
    if (list->count >= ILDA_FOLDER_MAX_FILES) {
        return ESP_ERR_NO_MEM;
    }

    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 32;
        char **paths = realloc(list->paths, capacity * sizeof(*paths));
        if (!paths) {
            return ESP_ERR_NO_MEM;
        }

        list->paths = paths;
        list->capacity = capacity;
    }

    list->paths[list->count] = strdup(path);
    if (!list->paths[list->count]) {
        return ESP_ERR_NO_MEM;
    }

    list->count++;
    return ESP_OK;
}

static void playlist_remove(playlist_t *list, size_t index)
{
    free(list->paths[index]);
    memmove(&list->paths[index], &list->paths[index + 1],
            (list->count - index - 1) * sizeof(*list->paths));
    list->count--;
}

static void playlist_scan(playlist_t *list, char *path, size_t path_len,
                          bool recursive, int depth)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return;
    }

    size_t base_len = strlen(path);
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && list->count < ILDA_FOLDER_MAX_FILES) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        int len = snprintf(path + base_len, path_len - base_len, "/%s", ent->d_name);
        if (len < 0 || (size_t)len >= path_len - base_len) {
            continue;
        }

        if (ent->d_type == DT_DIR) {
            if (recursive && depth < ILDA_FOLDER_MAX_DEPTH) {
                playlist_scan(list, path, path_len, recursive, depth + 1);
            }
            continue;
        }

        const char *dot = strrchr(ent->d_name, '.');
        if (dot && strcasecmp(dot, ".ild") == 0 && playlist_add(list, path) != ESP_OK) {
            break;
        }
    }

    path[base_len] = '\0';
    closedir(dir);
}

static int playlist_compare(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static bool playlist_should_advance(uint32_t passes, TickType_t file_started)
{
    /* A one-file folder just loops that file instead of reloading it. */
    if (s_playlist.count <= 1) {
        return false;
    }

    if (s_frame_count > 1) {
        return passes >= s_playlist.animation_plays;
    }

    return xTaskGetTickCount() - file_started >= pdMS_TO_TICKS(s_playlist.hold_ms);
}

static esp_err_t playlist_load_next(void)
{
    /*
       Runs in the sender task between passes. Files that fail the check or
       load are dropped from the list so they are not retried every cycle.
    */
    while (s_playlist.count > 0 && !s_stop) {
        s_playlist.index = (s_playlist.index + 1) % s_playlist.count;

        const char *path = s_playlist.paths[s_playlist.index];
        size_t expected_samples = 0;
        uint32_t frames = 0;

        esp_err_t ret = check_ilda_file(path, &expected_samples, &frames);
        if (ret == ESP_OK) {
            ret = load_ilda_to_psram(path, expected_samples);
        }

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Folder playback: %s (%lu frames)", path, (unsigned long)frames);
            set_current_file(path, frames, true);
            return ESP_OK;
        }

        if (s_stop) {
            break;
        }

        ESP_LOGW(TAG, "Folder playback: dropping %s (%s)", path, esp_err_to_name(ret));

        size_t removed = s_playlist.index;
        playlist_remove(&s_playlist, removed);
        if (s_playlist.count > 0) {
            s_playlist.index = (removed + s_playlist.count - 1) % s_playlist.count;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

/* ===================== SENDER ===================== */

static void sender_task(void *arg)
{
    (void)arg;

    suspend_idle0_wdt_for_v1_sender();

    sender_state_t st = {
        .packet = heap_caps_malloc(
            RP_STREAM_SAMPLES_PER_PACKET * RP_SAMPLE_CHANNELS * sizeof(int16_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA
        ),
    };

    if (!st.packet) {
        ESP_LOGE(TAG, "Failed to allocate ILDA packet buffer");
        free_samples();
        playlist_free(&s_playlist);
        set_current_file(NULL, 0, false);
        s_sender_task = NULL;
        resume_idle0_wdt_after_v1_sender();
        vTaskDelete(NULL);
        return;
    }

    uint8_t scale_percent = s_scale_percent;
    TickType_t file_started = xTaskGetTickCount();
    uint32_t passes = 0;

    while (!s_stop) {
        for (size_t i = 0; i < s_sample_count && !s_stop; i++) {
            /*
               Pick up XY scale changes at packet boundaries so the web slider
               takes effect within one packet, not one full file loop.
            */
            if (st.pc == 0) {
                scale_percent = s_scale_percent;
            }

            int16_t *sample = st.packet[st.pc];
            memcpy(sample, s_samples[i].ch, sizeof(st.packet[st.pc]));
            sample[0] = (int16_t)((int32_t)sample[0] * scale_percent / 100);
            sample[1] = (int16_t)((int32_t)sample[1] * scale_percent / 100);
            st.pc++;

            sender_send_if_full(&st, "packet");
        }

        passes++;
        bool advance = !s_stop && playlist_should_advance(passes, file_started);

        if (!advance && !s_stop && s_sample_count > 1) {
            sender_blanked_move(&st, s_samples[s_sample_count - 1].ch, s_samples[0].ch,
                                &scale_percent);
        }

        if (!s_stop) {
            sender_flush_partial(&st);
        }

        if (advance) {
            /*
               Folder playback: load the next file, then make a blanked move
               from where the old file ended to where the new one starts.
            */
            int16_t last[RP_SAMPLE_CHANNELS];
            memcpy(last, s_samples[s_sample_count - 1].ch, sizeof(last));

            if (playlist_load_next() != ESP_OK) {
                ESP_LOGW(TAG, "Folder playback: no playable files left");
                break;
            }

            passes = 0;
            file_started = xTaskGetTickCount();
            sender_blanked_move(&st, last, s_samples[0].ch, &scale_percent);
        }
    }

    free(st.packet);
    if (st.output_started) {
        rp_manager_output_stop();
    }
    free_samples();
    playlist_free(&s_playlist);
    set_current_file(NULL, 0, false);
    s_sender_task = NULL;
    resume_idle0_wdt_after_v1_sender();

    vTaskDelete(NULL);
}

/* ===================== START ===================== */

static esp_err_t begin_playback(const char *path, size_t expected_samples, uint32_t frames,
                                uint8_t scale_percent, playlist_t *playlist)
{
    /*
       The file has already passed check_ilda_file(). Only now is the current
       output stopped. A playlist, if given, is handed over to the sender.
    */
    esp_err_t ret = ilda_player_stop_and_wait(5000);
    if (ret != ESP_OK) {
        return ret;
    }

    playlist_free(&s_playlist);

    ret = rp_manager_output_force_stop();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop previous RP output before ILDA start: %s", esp_err_to_name(ret));
    }

    ret = rp_manager_clear_buffer();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear RP buffer before ILDA start: %s", esp_err_to_name(ret));
    }

    s_stop = false;
    s_scale_percent = scale_percent;

    if (playlist) {
        s_playlist = *playlist;
        memset(playlist, 0, sizeof(*playlist));
    }

    ESP_LOGI(TAG, "Starting ILDA memory playback: %s at %u%% XY scale",
             path, (unsigned int)scale_percent);

    ret = load_ilda_to_psram(path, expected_samples);
    if (ret != ESP_OK) {
        s_stop = true;
        free_samples();
        playlist_free(&s_playlist);
        ESP_LOGE(TAG, "ILDA load failed: %s", esp_err_to_name(ret));
        return ret;
    }

    set_current_file(path, frames, s_playlist.count > 0);

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
        playlist_free(&s_playlist);
        set_current_file(NULL, 0, false);
        s_sender_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ilda_player_start(const char *path, uint8_t scale_percent)
{
    if (!path || path[0] == '\0' ||
        scale_percent < ILDA_SCALE_MIN_PERCENT || scale_percent > ILDA_SCALE_MAX_PERCENT) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t expected_samples = 0;
    uint32_t frames = 0;
    esp_err_t ret = check_ilda_file(path, &expected_samples, &frames);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ILDA check failed for %s: %s", path, esp_err_to_name(ret));
        return ret;
    }

    return begin_playback(path, expected_samples, frames, scale_percent, NULL);
}

esp_err_t ilda_player_start_folder(const char *dir_path, bool recursive,
                                   uint32_t hold_ms, uint16_t animation_plays,
                                   uint8_t scale_percent)
{
    if (!dir_path || dir_path[0] == '\0' || hold_ms == 0 || animation_plays == 0 ||
        scale_percent < ILDA_SCALE_MIN_PERCENT || scale_percent > ILDA_SCALE_MAX_PERCENT) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[ILDA_PATH_MAX];
    strlcpy(path, dir_path, sizeof(path));

    playlist_t list = {
        .hold_ms = hold_ms,
        .animation_plays = animation_plays,
    };

    playlist_scan(&list, path, sizeof(path), recursive, 0);

    if (list.count == 0) {
        playlist_free(&list);
        ESP_LOGW(TAG, "No ILDA files in %s", dir_path);
        return ESP_ERR_NOT_FOUND;
    }

    qsort(list.paths, list.count, sizeof(*list.paths), playlist_compare);

    /* Find the first playable file before the current output is touched. */
    size_t expected_samples = 0;
    uint32_t frames = 0;

    while (list.count > 0) {
        esp_err_t ret = check_ilda_file(list.paths[0], &expected_samples, &frames);
        if (ret == ESP_OK) {
            break;
        }

        ESP_LOGW(TAG, "Folder playback: dropping %s (%s)", list.paths[0], esp_err_to_name(ret));
        playlist_remove(&list, 0);
    }

    if (list.count == 0) {
        playlist_free(&list);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Folder playback: %u files from %s, hold %lu ms, %u plays per animation",
             (unsigned int)list.count, dir_path, (unsigned long)hold_ms,
             (unsigned int)animation_plays);

    char first[ILDA_PATH_MAX];
    strlcpy(first, list.paths[0], sizeof(first));
    list.index = 0;

    esp_err_t ret = begin_playback(first, expected_samples, frames, scale_percent, &list);
    playlist_free(&list);
    return ret;
}

void ilda_player_get_current(char *path, size_t path_len, bool *folder)
{
    taskENTER_CRITICAL(&s_current_lock);
    if (path && path_len > 0) {
        strlcpy(path, s_current_path, path_len);
    }
    if (folder) {
        *folder = s_current_folder;
    }
    taskEXIT_CRITICAL(&s_current_lock);
}

esp_err_t ilda_player_set_scale(uint8_t scale_percent)
{
    if (scale_percent < ILDA_SCALE_MIN_PERCENT || scale_percent > ILDA_SCALE_MAX_PERCENT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ilda_player_is_running()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_scale_percent = scale_percent;
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
