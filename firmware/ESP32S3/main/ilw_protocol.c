#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "app_config.h"
#include "ilw_protocol.h"
#include "rp_manager.h"
#include "protocol_stream_stats.h"

#define ILW_MAGIC 0x49

#define ILW_TYPE_CONFIG  1
#define ILW_TYPE_POINTS  2
#define ILW_TYPE_CONTROL 3

#define ILW_VERSION_1 1

#define ILW_FMT_XY       1
#define ILW_FMT_XYRGB16  2
#define ILW_FMT_XYRGBI16 3
#define ILW_FMT_FULL16   4
#define ILW_FMT_XYRGB8   5
#define ILW_FMT_XYRGBI8  6
#define ILW_FMT_FULL8    7

#define ILW_CFG_OUTPUT_ENABLE  0x01
#define ILW_CFG_CLEAR_BUFFER   0x02
#define ILW_CFG_APPLY_DEFAULTS 0x04

#define ILW_CMD_STOP         0
#define ILW_CMD_OUTPUT_ON    1
#define ILW_CMD_OUTPUT_OFF   2
#define ILW_CMD_CLEAR_BUFFER 3
#define ILW_CMD_PARK         4

#define ILW_STACK_SERVER 8192u
#define ILW_STACK_FEED 6144u
#define ILW_RX_BUF_BYTES 4096u
#define ILW_SAMPLE_RING_CAPACITY 16384u
#define ILW_PREFILL_TARGET_US 30000u
#define ILW_PREFILL_MIN_PACKETS 4u
#define ILW_PREFILL_MAX_PACKETS 12u
#define ILW_MAX_PPS 200000u
#define ILW_BLANK_TIMEOUT_US 100000LL
#define ILW_DISABLE_TIMEOUT_US 500000LL
#define ILW_RP_BUFFER_SIZE_SAMPLES 16384u
#define ILW_RP_BUFFER_HIGH_WATER_SAMPLES 15360u
#define ILW_RP_BUFFER_LOW_WATER_SAMPLES 14848u

#if BOARD_VERSION == 1
#define ILW_FEED_TASK_CORE 0
#define ILW_FEED_TASK_PRIORITY 5
#else
#define ILW_FEED_TASK_CORE 1
#define ILW_FEED_TASK_PRIORITY 7
#endif

static const char *TAG = "ilw";

static int s_sock = -1;
static TaskHandle_t s_feed_task = NULL;

static volatile bool s_configured = false;
static volatile bool s_output_enabled = false;
static volatile bool s_output_started = false;
static volatile bool s_timeout_blanked = false;
static uint8_t s_format = ILW_FMT_FULL8;
static uint8_t s_point_size = 10;

static int16_t s_defaults[RP_SAMPLE_CHANNELS] = {
    0, 0, 32767, 32767, 32767, 32767, -32768, -32768
};

static int16_t (*s_sample_ring)[RP_SAMPLE_CHANNELS] = NULL;
static int16_t s_feed_packet[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS];
static uint32_t s_ring_head = 0;
static uint32_t s_ring_tail = 0;
static uint32_t s_ring_fill = 0;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_prefill_packets = 0;
static uint32_t s_prefill_target_packets = ILW_PREFILL_MIN_PACKETS;

static int64_t s_last_points_us = 0;

static inline uint16_t rd16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline int16_t rd_i16_le(const uint8_t *p)
{
    return (int16_t)rd16_le(p);
}

static inline int16_t u16_to_sample(uint16_t v)
{
    return (int16_t)((int32_t)v - 32768);
}

static inline int16_t u8_to_sample(uint8_t v)
{
    uint16_t expanded = (uint16_t)(((uint16_t)v << 8) | v);
    return u16_to_sample(expanded);
}

static uint8_t point_size_for_format(uint8_t fmt)
{
    switch (fmt) {
    case ILW_FMT_XY:       return 4;
    case ILW_FMT_XYRGB16:  return 10;
    case ILW_FMT_XYRGBI16: return 12;
    case ILW_FMT_FULL16:   return 16;
    case ILW_FMT_XYRGB8:   return 7;
    case ILW_FMT_XYRGBI8:  return 8;
    case ILW_FMT_FULL8:    return 10;
    default:               return 0;
    }
}

static void reset_defaults(void)
{
    s_defaults[0] = 0;
    s_defaults[1] = 0;
    s_defaults[2] = 32767;
    s_defaults[3] = 32767;
    s_defaults[4] = 32767;
    s_defaults[5] = 32767;
    s_defaults[6] = -32768;
    s_defaults[7] = -32768;
}

static void reset_local_packet(void)
{
    portENTER_CRITICAL(&s_ring_mux);
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_fill = 0;
    portEXIT_CRITICAL(&s_ring_mux);

    s_prefill_packets = 0;
}

static uint32_t sample_ring_fill(void)
{
    uint32_t fill;

    portENTER_CRITICAL(&s_ring_mux);
    fill = s_ring_fill;
    portEXIT_CRITICAL(&s_ring_mux);

    return fill;
}

static uint32_t sample_ring_free(void)
{
    uint32_t free_samples;

    portENTER_CRITICAL(&s_ring_mux);
    free_samples = ILW_SAMPLE_RING_CAPACITY - s_ring_fill;
    portEXIT_CRITICAL(&s_ring_mux);

    return free_samples;
}

static bool sample_ring_push(const int16_t sample[RP_SAMPLE_CHANNELS])
{
    if (!s_sample_ring) {
        return false;
    }

    bool ok = false;

    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_fill < ILW_SAMPLE_RING_CAPACITY) {
        memcpy(s_sample_ring[s_ring_head], sample, RP_SAMPLE_CHANNELS * sizeof(int16_t));
        s_ring_head = (s_ring_head + 1u) & (ILW_SAMPLE_RING_CAPACITY - 1u);
        s_ring_fill++;
        ok = true;
    }
    portEXIT_CRITICAL(&s_ring_mux);

    return ok;
}

static uint32_t sample_ring_pop_block(int16_t out[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS])
{
    if (!s_sample_ring) {
        return 0;
    }

    uint32_t got = 0;

    portENTER_CRITICAL(&s_ring_mux);
    while (got < RP_STREAM_SAMPLES_PER_PACKET && s_ring_fill > 0) {
        memcpy(out[got], s_sample_ring[s_ring_tail], RP_SAMPLE_CHANNELS * sizeof(int16_t));
        s_ring_tail = (s_ring_tail + 1u) & (ILW_SAMPLE_RING_CAPACITY - 1u);
        s_ring_fill--;
        got++;
    }
    portEXIT_CRITICAL(&s_ring_mux);

    return got;
}

static uint32_t prefill_packets_for_pps(uint32_t pps)
{
    uint64_t samples = ((uint64_t)pps * ILW_PREFILL_TARGET_US + 999999ULL) / 1000000ULL;
    uint32_t packets = (uint32_t)((samples + RP_STREAM_SAMPLES_PER_PACKET - 1u) /
                                  RP_STREAM_SAMPLES_PER_PACKET);

    if (packets < ILW_PREFILL_MIN_PACKETS) {
        packets = ILW_PREFILL_MIN_PACKETS;
    }

    if (packets > ILW_PREFILL_MAX_PACKETS) {
        packets = ILW_PREFILL_MAX_PACKETS;
    }

    return packets;
}

static void mark_stream_inactive(void)
{
    s_output_started = false;
    reset_local_packet();
    rp_manager_set_realtime_streaming(false);
}

static void ensure_realtime_rp_buffer(void)
{
    rp_status_t rp = {0};
    rp_manager_get_status(&rp);

    if (rp.remote_buffer_size_samples == ILW_RP_BUFFER_SIZE_SAMPLES &&
        rp.remote_buffer_high_water_samples == ILW_RP_BUFFER_HIGH_WATER_SAMPLES &&
        rp.remote_buffer_low_water_samples == ILW_RP_BUFFER_LOW_WATER_SAMPLES) {
        return;
    }

    ESP_LOGI(TAG, "Configuring RP realtime buffer: size=%u high=%u low=%u",
             ILW_RP_BUFFER_SIZE_SAMPLES,
             ILW_RP_BUFFER_HIGH_WATER_SAMPLES,
             ILW_RP_BUFFER_LOW_WATER_SAMPLES);

    esp_err_t ret = rp_manager_set_buffer_config(ILW_RP_BUFFER_SIZE_SAMPLES,
                                                ILW_RP_BUFFER_HIGH_WATER_SAMPLES,
                                                ILW_RP_BUFFER_LOW_WATER_SAMPLES);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RP realtime buffer config failed: %s", esp_err_to_name(ret));
    }
}

static void decode_point(const uint8_t *p, int16_t out[RP_SAMPLE_CHANNELS])
{
    memcpy(out, s_defaults, sizeof(s_defaults));

    out[0] = rd_i16_le(p);
    out[1] = rd_i16_le(p + 2);

    switch (s_format) {
    case ILW_FMT_XY:
        break;

    case ILW_FMT_XYRGB16:
        out[2] = u16_to_sample(rd16_le(p + 4));
        out[3] = u16_to_sample(rd16_le(p + 6));
        out[4] = u16_to_sample(rd16_le(p + 8));
        break;

    case ILW_FMT_XYRGBI16:
        out[2] = u16_to_sample(rd16_le(p + 4));
        out[3] = u16_to_sample(rd16_le(p + 6));
        out[4] = u16_to_sample(rd16_le(p + 8));
        out[5] = u16_to_sample(rd16_le(p + 10));
        break;

    case ILW_FMT_FULL16:
        out[2] = u16_to_sample(rd16_le(p + 4));
        out[3] = u16_to_sample(rd16_le(p + 6));
        out[4] = u16_to_sample(rd16_le(p + 8));
        out[5] = u16_to_sample(rd16_le(p + 10));
        out[6] = u16_to_sample(rd16_le(p + 12));
        out[7] = u16_to_sample(rd16_le(p + 14));
        break;

    case ILW_FMT_XYRGB8:
        out[2] = u8_to_sample(p[4]);
        out[3] = u8_to_sample(p[5]);
        out[4] = u8_to_sample(p[6]);
        break;

    case ILW_FMT_XYRGBI8:
        out[2] = u8_to_sample(p[4]);
        out[3] = u8_to_sample(p[5]);
        out[4] = u8_to_sample(p[6]);
        out[5] = u8_to_sample(p[7]);
        break;

    case ILW_FMT_FULL8:
        out[2] = u8_to_sample(p[4]);
        out[3] = u8_to_sample(p[5]);
        out[4] = u8_to_sample(p[6]);
        out[5] = u8_to_sample(p[7]);
        out[6] = u8_to_sample(p[8]);
        out[7] = u8_to_sample(p[9]);
        break;

    default:
        break;
    }
}

static esp_err_t send_full_packet(const int16_t packet[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS])
{
    esp_err_t ret = rp_manager_send_samples_packet(packet);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send_samples_packet: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_output_enabled && !s_output_started) {
        s_prefill_packets++;
        if (s_prefill_packets >= s_prefill_target_packets) {
            ret = rp_manager_output_start();
            if (ret == ESP_OK) {
                s_output_started = true;
                rp_manager_set_realtime_streaming(true);
                ESP_LOGI(TAG, "ILW output started after %lu/%lu prefill packets",
                         (unsigned long)s_prefill_packets,
                         (unsigned long)s_prefill_target_packets);
            } else {
                ESP_LOGW(TAG, "output_start: %s", esp_err_to_name(ret));
                reset_local_packet();
                return ret;
            }
        }
    }

    return ESP_OK;
}

static void feed_task(void *arg)
{
    (void)arg;

    while (true) {
        if (!s_configured || !s_output_enabled) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        if (sample_ring_fill() < RP_STREAM_SAMPLES_PER_PACKET) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
            continue;
        }

        uint32_t got = sample_ring_pop_block(s_feed_packet);
        if (got != RP_STREAM_SAMPLES_PER_PACKET) {
            continue;
        }

        esp_err_t ret = send_full_packet((const int16_t (*)[RP_SAMPLE_CHANNELS])s_feed_packet);
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

static void handle_config(const uint8_t *data, int len)
{
    if (len != 7 || data[2] != ILW_VERSION_1) {
        return;
    }

    uint8_t fmt = data[3];
    uint8_t point_size = point_size_for_format(fmt);
    uint16_t pps100 = rd16_le(data + 5);

    if (point_size == 0 || pps100 == 0) {
        return;
    }

    uint32_t pps = (uint32_t)pps100 * 100u;
    if (pps > ILW_MAX_PPS) {
        pps = ILW_MAX_PPS;
    }

    uint8_t flags = data[4];

    if (flags & ILW_CFG_APPLY_DEFAULTS) {
        reset_defaults();
    }

    s_format = fmt;
    s_point_size = point_size;
    s_configured = true;
    s_timeout_blanked = false;
    s_last_points_us = esp_timer_get_time();
    s_prefill_target_packets = prefill_packets_for_pps(pps);
    reset_local_packet();

    ensure_realtime_rp_buffer();
    rp_manager_set_rate(pps);

    if (flags & ILW_CFG_CLEAR_BUFFER) {
        rp_manager_output_force_stop();
        mark_stream_inactive();
    }

    s_output_enabled = (flags & ILW_CFG_OUTPUT_ENABLE) != 0;
    if (!s_output_enabled) {
        rp_manager_output_force_stop();
        mark_stream_inactive();
    }

    ESP_LOGI(TAG, "CONFIG fmt=%u pps=%lu output=%u clear=%u prefill=%lu packets",
             fmt,
             (unsigned long)pps,
             s_output_enabled ? 1u : 0u,
             (flags & ILW_CFG_CLEAR_BUFFER) ? 1u : 0u,
             (unsigned long)s_prefill_target_packets);
}

static void handle_points(const uint8_t *data, int len)
{
    if (!s_configured || len < 3) {
        return;
    }

    uint8_t count = data[2];
    if (count == 0) {
        return;
    }

    int expected = 3 + (int)count * (int)s_point_size;
    if (len != expected) {
        return;
    }

    if (!s_output_enabled) {
        return;
    }

    if (sample_ring_free() < count) {
        return;
    }

    int16_t sample[RP_SAMPLE_CHANNELS];
    const uint8_t *p = data + 3;
    uint32_t accepted = 0;

    for (uint8_t i = 0; i < count; i++) {
        decode_point(p, sample);
        if (sample_ring_push(sample)) {
            accepted++;
        }
        p += s_point_size;
    }

    if (accepted > 0 && s_feed_task) {
        xTaskNotifyGive(s_feed_task);
    }

    protocol_stream_stats_add_samples(PROTOCOL_STREAM_ILW, accepted);
    s_last_points_us = esp_timer_get_time();
    s_timeout_blanked = false;
}

static void handle_control(const uint8_t *data, int len)
{
    if (len != 3) {
        return;
    }

    switch (data[2]) {
    case ILW_CMD_STOP:
        rp_manager_output_force_stop();
        s_output_enabled = false;
        mark_stream_inactive();
        break;

    case ILW_CMD_OUTPUT_ON:
        if (s_configured) {
            s_output_enabled = true;
            s_timeout_blanked = false;
        }
        break;

    case ILW_CMD_OUTPUT_OFF:
        rp_manager_output_force_stop();
        s_output_enabled = false;
        mark_stream_inactive();
        break;

    case ILW_CMD_CLEAR_BUFFER:
        rp_manager_output_force_stop();
        mark_stream_inactive();
        break;

    case ILW_CMD_PARK:
        rp_manager_output_force_stop();
        s_output_enabled = false;
        mark_stream_inactive();
        break;

    default:
        break;
    }
}

static void safety_check(void)
{
    if (!s_configured || !s_output_enabled || s_last_points_us == 0) {
        return;
    }

    int64_t age = esp_timer_get_time() - s_last_points_us;

    if (age > ILW_DISABLE_TIMEOUT_US) {
        rp_manager_output_force_stop();
        s_output_enabled = false;
        mark_stream_inactive();
        ESP_LOGW(TAG, "POINTS timeout, output disabled");
        return;
    }

    if (age > ILW_BLANK_TIMEOUT_US && !s_timeout_blanked) {
        rp_manager_output_force_stop();
        mark_stream_inactive();
        s_timeout_blanked = true;
        ESP_LOGW(TAG, "POINTS timeout, output blanked");
    }
}

static void server_task(void *arg)
{
    (void)arg;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "UDP socket failed");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int rcvbuf = 131072;
    setsockopt(s_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ILW_PROTOCOL_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "UDP bind failed on port %d", ILW_PROTOCOL_UDP_PORT);
        close(s_sock);
        s_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    static uint8_t rxbuf[ILW_RX_BUF_BYTES];

    ESP_LOGI(TAG, "ILW-UDP server on port %d", ILW_PROTOCOL_UDP_PORT);

    while (true) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        int n = recvfrom(s_sock, rxbuf, sizeof(rxbuf), 0,
                         (struct sockaddr *)&from, &from_len);

        safety_check();

        if (n < 2 || rxbuf[0] != ILW_MAGIC) {
            continue;
        }

        switch (rxbuf[1]) {
        case ILW_TYPE_CONFIG:
            handle_config(rxbuf, n);
            break;

        case ILW_TYPE_POINTS:
            handle_points(rxbuf, n);
            break;

        case ILW_TYPE_CONTROL:
            handle_control(rxbuf, n);
            break;

        default:
            break;
        }
    }
}

esp_err_t ilw_protocol_start(void)
{
    reset_defaults();

    if (!s_sample_ring) {
        s_sample_ring = heap_caps_malloc(
            ILW_SAMPLE_RING_CAPACITY * RP_SAMPLE_CHANNELS * sizeof(int16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sample_ring) {
            s_sample_ring = heap_caps_malloc(
                ILW_SAMPLE_RING_CAPACITY * RP_SAMPLE_CHANNELS * sizeof(int16_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!s_sample_ring) {
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "ILW sample ring: %u samples (%u bytes)",
                 ILW_SAMPLE_RING_CAPACITY,
                 (unsigned)(ILW_SAMPLE_RING_CAPACITY * RP_SAMPLE_CHANNELS * sizeof(int16_t)));
    }

    reset_local_packet();

    if (!s_feed_task) {
        BaseType_t feed_ok = xTaskCreatePinnedToCore(
            feed_task,
            "ilw_feed",
            ILW_STACK_FEED,
            NULL,
            ILW_FEED_TASK_PRIORITY,
            &s_feed_task,
            ILW_FEED_TASK_CORE);

        if (feed_ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        server_task,
        "ilw_udp",
        ILW_STACK_SERVER,
        NULL,
        8,
        NULL,
        0);

    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void ilw_protocol_stop(void)
{
    s_output_enabled = false;
    s_timeout_blanked = false;
    mark_stream_inactive();
}
