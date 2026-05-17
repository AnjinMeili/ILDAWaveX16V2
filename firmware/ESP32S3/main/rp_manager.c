#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "app_config.h"
#include "rp_manager.h"
#include "rp_uart.h"

static const char *TAG = "rp";

#define STREAM_MAGIC0 0xA5
#define STREAM_MAGIC1 0x53
#define RP_CONFIG_CMD_TIMEOUT_MS 1500
#define RP_PAYLOAD_QUEUE_DEPTH 16
#define RP_PAYLOAD_TX_STACK 4096
#define RP_PAYLOAD_TX_PRIORITY 8
#define RP_PAYLOAD_TX_CORE 1

static SemaphoreHandle_t s_rp_mutex = NULL;
static spi_device_handle_t s_payload_spi = NULL;
static QueueHandle_t s_payload_free_q = NULL;
static QueueHandle_t s_payload_ready_q = NULL;
static TaskHandle_t s_payload_tx_task = NULL;
static volatile bool s_payload_tx_busy = false;

static TaskHandle_t s_test_stream_task = NULL;
static volatile bool s_test_stream_requested = false;
static volatile bool s_realtime_streaming = false;
static volatile rp_test_shape_t s_test_shape = RP_TEST_SHAPE_BEAM;
static volatile uint8_t s_test_color_mask = 0x07;

static uint16_t s_stream_seq = 0;
static volatile uint32_t s_stream_generation = 0;
static uint32_t s_test_phase = 0;
static int      s_output_ref = 0;   /* ref-count: START sent at 0->1, STOP at 1->0 */

static rp_status_t s_status = {
    .initialized = false,
    .remote_sample_rate = 1000,
    .remote_mirror_y = true,
    .last_error = ESP_OK,
    .remote_fw_version = "unknown",
};

typedef struct {
    uint8_t *packet;
    uint32_t generation;
} rp_payload_slot_t;

static rp_payload_slot_t s_payload_slots[RP_PAYLOAD_QUEUE_DEPTH];


static uint32_t checksum32_payload(const uint8_t *data, size_t len)
{
    uint32_t c = 0;

    for (size_t i = 0; i < len; i++) {
        c += data[i];
    }

    return c;
}

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int parse_u32_key(const char *s, const char *key, uint32_t *out)
{
    const char *p = strstr(s, key);

    if (!p) {
        return 0;
    }

    p += strlen(key);

    unsigned long v = 0;
    if (sscanf(p, "%lu", &v) == 1) {
        *out = (uint32_t)v;
        return 1;
    }

    return 0;
}

static esp_err_t uart_cmd_locked_timeout(const char *cmd, char *resp, int resp_len, int timeout_ms)
{
    if (!resp || resp_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    resp[0] = '\0';

    esp_err_t ret = rp_uart_command(cmd, resp, resp_len, timeout_ms);

    s_status.last_error = ret;

    if (ret != ESP_OK) {
        s_status.error_count++;
        ESP_LOGW(TAG,
                 "UART command failed: '%s' err=%s partial='%s'",
                 cmd,
                 esp_err_to_name(ret),
                 resp);
    }

    return ret;
}

static esp_err_t uart_cmd_locked(const char *cmd, char *resp, int resp_len)
{
    return uart_cmd_locked_timeout(cmd, resp, resp_len, 200);
}

static void refresh_version_locked(void)
{
    char resp[128];

    esp_err_t ret = uart_cmd_locked("VERSION", resp, sizeof(resp));

    if (ret == ESP_OK && strncmp(resp, "VERSION ", 8) == 0) {
        strlcpy(s_status.remote_fw_version,
                resp + 8,
                sizeof(s_status.remote_fw_version));
    } else {
        strlcpy(s_status.remote_fw_version,
                "unknown",
                sizeof(s_status.remote_fw_version));

        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "Bad VERSION response: '%s'", resp);
        }
    }
}

static esp_err_t refresh_status_locked(void)
{
    char resp[512];

    esp_err_t ret = uart_cmd_locked("STATUS", resp, sizeof(resp));
    if (ret != ESP_OK) {
        return ret;
    }

    if (strncmp(resp, "STATUS ", 7) != 0) {
        ret = uart_cmd_locked("STATUS", resp, sizeof(resp));
        if (ret == ESP_OK && strncmp(resp, "STATUS ", 7) == 0) {
            goto parse_status;
        }

        s_status.error_count++;
        s_status.last_error = ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(TAG, "Bad STATUS response: '%s'", resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

parse_status:
    uint32_t v = 0;

    if (parse_u32_key(resp, "running=", &v)) {
        s_status.remote_running = v ? true : false;
    }

    if (parse_u32_key(resp, "rate=", &v)) {
        s_status.remote_sample_rate = v;
    }

    if (parse_u32_key(resp, "fill=", &v)) {
        s_status.remote_buffer_fill_samples = v;
    }

    if (parse_u32_key(resp, "buffer_size=", &v)) {
        s_status.remote_buffer_size_samples = v;
    }

    if (parse_u32_key(resp, "buffer_high_water=", &v)) {
        s_status.remote_buffer_high_water_samples = v;
    }

    if (parse_u32_key(resp, "buffer_low_water=", &v)) {
        s_status.remote_buffer_low_water_samples = v;
    }

    if (parse_u32_key(resp, "buffer_max=", &v)) {
        s_status.remote_buffer_max_samples = v;
    }

    if (parse_u32_key(resp, "mirror_x=", &v)) {
        s_status.remote_mirror_x = v ? true : false;
    }

    if (parse_u32_key(resp, "mirror_y=", &v)) {
        s_status.remote_mirror_y = v ? true : false;
    }

    if (parse_u32_key(resp, "underruns=", &v)) {
        s_status.remote_underruns = v;
    }

    if (parse_u32_key(resp, "payload_errors=", &v)) {
        s_status.remote_payload_errors = v;
    }

    if (parse_u32_key(resp, "stream_packets=", &v)) {
        s_status.remote_stream_packets = v;
    }

    if (parse_u32_key(resp, "bad_packets=", &v)) {
        s_status.remote_stream_bad_packets = v;
    }

    if (parse_u32_key(resp, "dropped_packets=", &v)) {
        s_status.remote_stream_dropped_packets = v;
    }

    if (parse_u32_key(resp, "rx_timeouts=", &v)) {
        s_status.remote_stream_rx_timeouts = v;
    }

    s_status.last_error = ESP_OK;
    return ESP_OK;
}

static int16_t clamp_i16(int32_t v)
{
    if (v < -32768) {
        return -32768;
    }

    if (v > 32767) {
        return 32767;
    }

    return (int16_t)v;
}

static uint32_t isqrt_u32(uint32_t n)
{
    uint32_t root = 0;
    uint32_t bit = 1UL << 30;

    while (bit > n) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }

        bit >>= 2;
    }

    return root;
}

static int32_t lerp_i32(int32_t a, int32_t b, uint32_t pos, uint32_t span)
{
    if (span == 0) {
        return b;
    }

    return a + (int32_t)(((int64_t)(b - a) * (int64_t)pos) / (int64_t)span);
}

static void line_point(int32_t ax, int32_t ay,
                       int32_t bx, int32_t by,
                       uint32_t pos, uint32_t span,
                       int32_t *x, int32_t *y)
{
    *x = lerp_i32(ax, bx, pos, span);
    *y = lerp_i32(ay, by, pos, span);
}

static void build_test_samples(int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS])
{
    const rp_test_shape_t shape = s_test_shape;
    const uint8_t color_mask = s_test_color_mask & 0x07;

    for (int s = 0; s < RP_STREAM_SAMPLES_PER_PACKET; s++) {
        uint32_t p = s_test_phase + (uint32_t)s;
        int32_t x = 0;
        int32_t y = 0;
        bool blanked = false;

        if (shape == RP_TEST_SHAPE_BEAM) {
            x = 0;
            y = 0;
        } else if (shape == RP_TEST_SHAPE_CIRCLE) {
            const int32_t r = 32767;
            const uint32_t rr = (uint32_t)(r * r);
            uint32_t t = p % 1024;
            uint32_t q = t & 255u;
            int32_t v = r - (int32_t)((q * (uint32_t)r) / 255u);
            int32_t h = (int32_t)isqrt_u32(rr - (uint32_t)(v * v));

            if (t < 256u) {
                x = v;
                y = h;
            } else if (t < 512u) {
                x = -h;
                y = v;
            } else if (t < 768u) {
                x = -v;
                y = -h;
            } else {
                x = h;
                y = -v;
            }
        } else if (shape == RP_TEST_SHAPE_CROSS) {
            uint32_t t = p % 1024;

            if (t < 448u) {
                line_point(-32768, 0, 32767, 0, t, 447u, &x, &y);
            } else if (t < 512u) {
                blanked = true;
                line_point(32767, 0, 0, -32768, t - 448u, 63u, &x, &y);
            } else if (t < 960u) {
                line_point(0, -32768, 0, 32767, t - 512u, 447u, &x, &y);
            } else {
                blanked = true;
                line_point(0, 32767, -32768, 0, t - 960u, 63u, &x, &y);
            }
        } else if (shape == RP_TEST_SHAPE_HEART) {
            uint32_t t = p % 1536;
            uint32_t seg = t / 256u;
            uint32_t q = t & 255u;

            switch (seg) {
            case 0:
                line_point(0, 12000, -21000, 30000, q, 255u, &x, &y);
                break;
            case 1:
                line_point(-21000, 30000, -32768, 5000, q, 255u, &x, &y);
                break;
            case 2:
                line_point(-32768, 5000, 0, -32768, q, 255u, &x, &y);
                break;
            case 3:
                line_point(0, -32768, 32767, 5000, q, 255u, &x, &y);
                break;
            case 4:
                line_point(32767, 5000, 21000, 30000, q, 255u, &x, &y);
                break;
            default:
                line_point(21000, 30000, 0, 12000, q, 255u, &x, &y);
                break;
            }
        } else {
            uint32_t t = p % 1280;

            if (t < 256u) {
                line_point(-32768, -32768, 32767, -32768, t, 255u, &x, &y);
            } else if (t < 512u) {
                line_point(32767, -32768, 32767, 32767, t - 256u, 255u, &x, &y);
            } else if (t < 768u) {
                line_point(32767, 32767, -32768, 32767, t - 512u, 255u, &x, &y);
            } else if (t < 1024u) {
                line_point(-32768, 32767, -32768, -32768, t - 768u, 255u, &x, &y);
            } else {
                blanked = true;
                line_point(-32768, -32768, -32768, -32768, t - 1024u, 255u, &x, &y);
            }
        }

        samples[s][0] = clamp_i16(x);
        samples[s][1] = clamp_i16(y);
        samples[s][2] = (!blanked && (color_mask & 0x01)) ? 32767 : -32768;
        samples[s][3] = (!blanked && (color_mask & 0x02)) ? 32767 : -32768;
        samples[s][4] = (!blanked && (color_mask & 0x04)) ? 32767 : -32768;
        samples[s][5] = (!blanked && color_mask) ? 32767 : -32768;
        for (int ch = 6; ch < RP_SAMPLE_CHANNELS; ch++) {
            samples[s][ch] = 0;
        }
    }

    s_test_phase += RP_STREAM_SAMPLES_PER_PACKET;
}

static esp_err_t payload_spi_start(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RP_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = RP_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RP_STREAM_PACKET_BYTES,
    };

    esp_err_t ret = spi_bus_initialize(RP_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    gpio_config_t ready_io = {
        .pin_bit_mask = 1ULL << RP_MISO_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&ready_io);
    if (ret != ESP_OK) {
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = RP_SPI_PAYLOAD_SPEED_HZ,
        .mode = 3,
        .spics_io_num = RP_CS_GPIO,
        .queue_size = 4,
        .cs_ena_pretrans = 16,
        .cs_ena_posttrans = 16,
    };

    ret = spi_bus_add_device(RP_SPI_HOST, &dev_cfg, &s_payload_spi);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "Payload SPI ready speed=%d packet=%d bytes ready_gpio=%d",
             RP_SPI_PAYLOAD_SPEED_HZ,
             RP_STREAM_PACKET_BYTES,
             RP_MISO_GPIO);

    return ESP_OK;
}

static esp_err_t wait_rp_ready_high(uint32_t timeout_us)
{
    int64_t deadline = esp_timer_get_time() + timeout_us;

    while (esp_timer_get_time() < deadline) {
        if (gpio_get_level(RP_MISO_GPIO)) {
            esp_rom_delay_us(3);
            return ESP_OK;
        }

        for (int i = 0; i < 100; i++) {
            esp_rom_delay_us(2);
            if (gpio_get_level(RP_MISO_GPIO)) {
                esp_rom_delay_us(3);
                return ESP_OK;
            }
        }

        taskYIELD();
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_rp_ready_low(uint32_t timeout_us)
{
    int64_t deadline = esp_timer_get_time() + timeout_us;

    while (esp_timer_get_time() < deadline) {
        if (!gpio_get_level(RP_MISO_GPIO)) {
            return ESP_OK;
        }

        for (int i = 0; i < 100; i++) {
            esp_rom_delay_us(2);
            if (!gpio_get_level(RP_MISO_GPIO)) {
                return ESP_OK;
            }
        }

        taskYIELD();
    }

    return ESP_ERR_TIMEOUT;
}

static void build_packet_payload(uint8_t *packet, const uint8_t *payload)
{
    packet[0] = STREAM_MAGIC0;
    packet[1] = STREAM_MAGIC1;

    uint16_t seq = __atomic_fetch_add(&s_stream_seq, 1, __ATOMIC_RELAXED);
    put_u16_le(&packet[2], seq);
    put_u16_le(&packet[4], RP_STREAM_SAMPLES_PER_PACKET);
    put_u16_le(&packet[6], 0);

    memcpy(&packet[RP_STREAM_HEADER_BYTES], payload, RP_STREAM_PAYLOAD_BYTES);

    uint32_t checksum =
        checksum32_payload(&packet[RP_STREAM_HEADER_BYTES], RP_STREAM_PAYLOAD_BYTES);

    put_u32_le(&packet[8], checksum);
}

static esp_err_t transmit_built_packet(const uint8_t *packet)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    t.length = RP_STREAM_PACKET_BYTES * 8;
    t.tx_buffer = packet;

    esp_err_t ret = spi_device_transmit(s_payload_spi, &t);
    if (ret != ESP_OK) {
        return ret;
    }

    return wait_rp_ready_low(100000);
}

static void payload_record_tx_result(esp_err_t ret)
{
    if (ret != ESP_OK) {
        s_status.last_error = ret;
        s_status.error_count++;
        return;
    }

    s_status.test_stream_blocks_sent++;
    s_status.last_error = ESP_OK;
}

static void payload_tx_task(void *arg)
{
    (void)arg;

    while (true) {
        rp_payload_slot_t *slot = NULL;

        if (xQueueReceive(s_payload_ready_q, &slot, portMAX_DELAY) != pdTRUE || !slot) {
            continue;
        }

        uint32_t generation = __atomic_load_n(&s_stream_generation, __ATOMIC_ACQUIRE);
        if (slot->generation == generation) {
            s_payload_tx_busy = true;
            esp_err_t ret = wait_rp_ready_high(500000);

            generation = __atomic_load_n(&s_stream_generation, __ATOMIC_ACQUIRE);
            if (ret == ESP_OK && slot->generation == generation) {
                ret = transmit_built_packet(slot->packet);
            }

            payload_record_tx_result(ret);
            s_payload_tx_busy = false;
        }

        xQueueSend(s_payload_free_q, &slot, portMAX_DELAY);
    }
}

static void payload_queue_reset(void)
{
    __atomic_add_fetch(&s_stream_generation, 1, __ATOMIC_ACQ_REL);

    if (!s_payload_ready_q || !s_payload_free_q) {
        return;
    }

    rp_payload_slot_t *slot = NULL;
    while (xQueueReceive(s_payload_ready_q, &slot, 0) == pdTRUE) {
        if (slot) {
            xQueueSend(s_payload_free_q, &slot, 0);
        }
    }
}

static esp_err_t payload_queue_start(void)
{
    if (s_payload_tx_task) {
        return ESP_OK;
    }

    if (!s_payload_free_q) {
        s_payload_free_q = xQueueCreate(RP_PAYLOAD_QUEUE_DEPTH, sizeof(rp_payload_slot_t *));
        if (!s_payload_free_q) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_payload_ready_q) {
        s_payload_ready_q = xQueueCreate(RP_PAYLOAD_QUEUE_DEPTH, sizeof(rp_payload_slot_t *));
        if (!s_payload_ready_q) {
            return ESP_ERR_NO_MEM;
        }
    }

    for (int i = 0; i < RP_PAYLOAD_QUEUE_DEPTH; i++) {
        if (!s_payload_slots[i].packet) {
            s_payload_slots[i].packet = heap_caps_malloc(
                RP_STREAM_PACKET_BYTES,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

            if (!s_payload_slots[i].packet) {
                return ESP_ERR_NO_MEM;
            }
        }

        rp_payload_slot_t *slot = &s_payload_slots[i];
        xQueueSend(s_payload_free_q, &slot, 0);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        payload_tx_task,
        "rp_tx",
        RP_PAYLOAD_TX_STACK,
        NULL,
        RP_PAYLOAD_TX_PRIORITY,
        &s_payload_tx_task,
        RP_PAYLOAD_TX_CORE);

    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t rp_manager_wait_payload_idle(uint32_t timeout_ms)
{
    if (!s_payload_free_q || !s_payload_ready_q) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline_us) {
        UBaseType_t ready = uxQueueMessagesWaiting(s_payload_ready_q);
        UBaseType_t free_count = uxQueueMessagesWaiting(s_payload_free_q);

        if (ready == 0 && !s_payload_tx_busy && free_count >= RP_PAYLOAD_QUEUE_DEPTH) {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_packet_payload(const uint8_t *payload)
{
    if (!s_payload_spi || !s_payload_free_q || !s_payload_ready_q) {
        return ESP_ERR_INVALID_STATE;
    }

    rp_payload_slot_t *slot = NULL;
    if (xQueueReceive(s_payload_free_q, &slot, portMAX_DELAY) != pdTRUE || !slot) {
        return ESP_FAIL;
    }

    build_packet_payload(slot->packet, payload);
    slot->generation = __atomic_load_n(&s_stream_generation, __ATOMIC_ACQUIRE);

    if (xQueueSend(s_payload_ready_q, &slot, portMAX_DELAY) != pdTRUE) {
        xQueueSend(s_payload_free_q, &slot, portMAX_DELAY);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t send_stream_packet_spi(void)
{
    static int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS];

    build_test_samples(samples);

    return send_packet_payload((const uint8_t *)samples);
}

esp_err_t rp_manager_send_samples_packet(
    const int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS])
{
    return send_packet_payload((const uint8_t *)samples);
}

static void test_stream_task(void *arg)
{
    (void)arg;

    int64_t next_status_us = 0;

    while (s_test_stream_requested) {
        esp_err_t ret = send_stream_packet_spi();

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "stream packet send failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        int64_t now = esp_timer_get_time();

        if (now >= next_status_us) {
            if (s_rp_mutex &&
                xSemaphoreTake(s_rp_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                refresh_status_locked();
                xSemaphoreGive(s_rp_mutex);
            }

            next_status_us = now + 250000;
        }

        taskYIELD();
    }

    s_status.test_stream_running = false;
    s_test_stream_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t rp_manager_start(void)
{
    if (!s_rp_mutex) {
        s_rp_mutex = xSemaphoreCreateMutex();

        if (!s_rp_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = rp_uart_start();
    if (ret != ESP_OK) {
        s_status.last_error = ret;
        return ret;
    }

    ret = payload_spi_start();
    if (ret != ESP_OK) {
        s_status.last_error = ret;
        return ret;
    }

    ret = payload_queue_start();
    if (ret != ESP_OK) {
        s_status.last_error = ret;
        return ret;
    }

    char resp[128];

    s_output_ref = 0;

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    ret = uart_cmd_locked("PING", resp, sizeof(resp));
    if (ret == ESP_OK && strcmp(resp, "PONG") == 0) {
        s_status.initialized = true;
        s_status.ping_count++;

        refresh_version_locked();

        ret = refresh_status_locked();
    } else {
        ESP_LOGW(TAG, "RP not ready or bad ping response: '%s'", resp);
        s_status.initialized = false;
        s_status.last_error = ESP_ERR_INVALID_RESPONSE;
        s_status.error_count++;
        strlcpy(s_status.remote_fw_version,
                "unknown",
                sizeof(s_status.remote_fw_version));

        ret = ESP_OK;
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_ping(void)
{
    if (s_realtime_streaming) {
        return ESP_OK;
    }

    char resp[128];

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked("PING", resp, sizeof(resp));
    if (ret == ESP_OK && strcmp(resp, "PONG") == 0) {
        s_status.initialized = true;
        s_status.ping_count++;

        refresh_version_locked();
    } else if (ret == ESP_OK) {
        ret = uart_cmd_locked("PING", resp, sizeof(resp));
        if (ret == ESP_OK && strcmp(resp, "PONG") == 0) {
            s_status.initialized = true;
            s_status.ping_count++;
            refresh_version_locked();
        } else if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_RESPONSE;
            s_status.last_error = ret;
            s_status.error_count++;
            ESP_LOGW(TAG, "Bad PING response: '%s'", resp);
        }
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_get_remote_status(void)
{
    if (s_realtime_streaming) {
        return ESP_OK;
    }

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);
    esp_err_t ret = refresh_status_locked();
    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_output_start(void)
{
    char resp[128];
    esp_err_t ret = ESP_OK;

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    if (s_output_ref++ == 0) {
        ret = uart_cmd_locked("START", resp, sizeof(resp));
        if (ret == ESP_OK)
            s_status.remote_running = true;
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_output_stop(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;

    char resp[128];
    esp_err_t ret = ESP_OK;

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    if (s_output_ref > 0 && --s_output_ref == 0) {
        payload_queue_reset();
        ret = uart_cmd_locked("STOP", resp, sizeof(resp));
        if (ret == ESP_OK)
            refresh_status_locked();
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_output_force_stop(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;
    payload_queue_reset();

    char resp[128];

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    s_output_ref = 0;
    esp_err_t ret = uart_cmd_locked("STOP", resp, sizeof(resp));
    if (ret == ESP_OK) {
        s_status.remote_running = false;
        refresh_status_locked();
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_clear_buffer(void)
{
    char resp[128];

    s_test_stream_requested = false;
    s_status.test_stream_running = false;
    payload_queue_reset();

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked("CLEAR", resp, sizeof(resp));
    if (ret == ESP_OK) {
        ret = refresh_status_locked();
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_set_rate(uint32_t rate_hz)
{
    char cmd[64];
    char resp[128];

    if (rate_hz < 1) {
        rate_hz = 1;
    }

    if (rate_hz > 200000) {
        rate_hz = 200000;
    }

    snprintf(cmd, sizeof(cmd), "RATE %lu", (unsigned long)rate_hz);

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked(cmd, resp, sizeof(resp));
    if (ret == ESP_OK) {
        s_status.remote_sample_rate = rate_hz;
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_set_buffer_config(uint32_t size_samples,
                                       uint32_t high_water_samples,
                                       uint32_t low_water_samples)
{
    char cmd[96];
    char resp[128];

    if (size_samples < 512) {
        size_samples = 512;
    }

    if (high_water_samples < 256) {
        high_water_samples = 256;
    }

    if (high_water_samples >= size_samples) {
        high_water_samples = size_samples > 256 ? size_samples - 256 : 256;
    }

    if (low_water_samples > high_water_samples - 256) {
        low_water_samples = high_water_samples > 256 ? high_water_samples - 256 : 0;
    }

    snprintf(cmd, sizeof(cmd),
             "BUFFER size=%lu high_water=%lu low_water=%lu",
             (unsigned long)size_samples,
             (unsigned long)high_water_samples,
             (unsigned long)low_water_samples);

    s_test_stream_requested = false;
    s_status.test_stream_running = false;
    payload_queue_reset();

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked_timeout(cmd, resp, sizeof(resp), RP_CONFIG_CMD_TIMEOUT_MS);
    if (ret == ESP_OK && strncmp(resp, "OK BUFFER", 9) == 0) {
        s_status.remote_buffer_size_samples = size_samples;
        s_status.remote_buffer_high_water_samples = high_water_samples;
        s_status.remote_buffer_low_water_samples = low_water_samples;
        s_status.remote_running = false;
        s_output_ref = 0;
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_RESPONSE;
        s_status.last_error = ret;
        s_status.error_count++;
        ESP_LOGW(TAG, "Bad BUFFER response: '%s'", resp);
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_set_mirror(bool mirror_x, bool mirror_y)
{
    char cmd[64];
    char resp[128];

    snprintf(cmd, sizeof(cmd),
             "MIRROR x=%lu y=%lu",
             mirror_x ? 1ul : 0ul,
             mirror_y ? 1ul : 0ul);

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked_timeout(cmd, resp, sizeof(resp), RP_CONFIG_CMD_TIMEOUT_MS);
    if (ret == ESP_OK && strncmp(resp, "OK MIRROR", 9) == 0) {
        s_status.remote_mirror_x = mirror_x;
        s_status.remote_mirror_y = mirror_y;
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_RESPONSE;
        s_status.last_error = ret;
        s_status.error_count++;
        ESP_LOGW(TAG, "Bad MIRROR response: '%s'", resp);
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

void rp_manager_set_realtime_streaming(bool enabled)
{
    s_realtime_streaming = enabled;
}

esp_err_t rp_manager_send_test_block(void)
{
    return send_stream_packet_spi();
}

esp_err_t rp_manager_start_test_stream(void)
{
    return rp_manager_start_test_shape(RP_TEST_SHAPE_SQUARE);
}

esp_err_t rp_manager_start_test_shape(rp_test_shape_t shape)
{
    if (s_test_stream_task) {
        s_test_shape = shape;
        s_test_phase = 0;
        s_test_stream_requested = true;
        s_status.test_stream_running = true;
        return ESP_OK;
    }

    s_test_shape = shape;
    s_test_phase = 0;
    s_test_stream_requested = true;
    s_status.test_stream_running = true;

    BaseType_t ok = xTaskCreate(
        test_stream_task,
        "rp_stream",
        6144,
        NULL,
        6,
        &s_test_stream_task
    );

    if (ok != pdPASS) {
        s_test_stream_requested = false;
        s_status.test_stream_running = false;
        s_test_stream_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return rp_manager_output_start();
}

void rp_manager_set_test_color_mask(uint8_t mask)
{
    s_test_color_mask = mask & 0x07;
}

esp_err_t rp_manager_stop_test_stream(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;

    uint32_t waited_ms = 0;
    while (s_test_stream_task && waited_ms < 1000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }

    return s_test_stream_task ? ESP_ERR_TIMEOUT : ESP_OK;
}

void rp_manager_get_status(rp_status_t *status)
{
    if (!status) {
        return;
    }

    if (!s_rp_mutex) {
        *status = s_status;
        return;
    }

    if (s_realtime_streaming) {
        if (xSemaphoreTake(s_rp_mutex, 0) == pdTRUE) {
            *status = s_status;
            xSemaphoreGive(s_rp_mutex);
        } else {
            *status = s_status;
        }

        return;
    }

    if (xSemaphoreTake(s_rp_mutex, 0) == pdTRUE) {
        *status = s_status;
        xSemaphoreGive(s_rp_mutex);
    } else {
        *status = s_status;
    }
}
