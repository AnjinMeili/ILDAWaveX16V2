#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "app_config.h"
#include "status_led.h"

#if ENABLE_STATUS_LED

#include "driver/rmt_tx.h"

#include "net_manager.h"
#include "rp_manager.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_rgb_t;

static const char *TAG = "status_led";

static rmt_channel_handle_t s_led_channel = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;
static TaskHandle_t s_led_task = NULL;
static volatile bool s_booting = true;

static uint32_t s_prev_underruns;
static uint32_t s_prev_payload_errors;
static uint32_t s_prev_bad_packets;
static uint32_t s_prev_dropped_packets;
static bool s_prev_counters_valid = false;
static TickType_t s_warning_until = 0;

static uint8_t clamp_brightness(uint8_t value)
{
    return value > STATUS_LED_MAX_BRIGHTNESS ? STATUS_LED_MAX_BRIGHTNESS : value;
}

static led_rgb_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_rgb_t color = {
        .r = clamp_brightness(r),
        .g = clamp_brightness(g),
        .b = clamp_brightness(b),
    };

    return color;
}

static led_rgb_t scale_color(led_rgb_t color, uint8_t scale)
{
    led_rgb_t out = {
        .r = (uint8_t)(((uint16_t)color.r * scale) / 255),
        .g = (uint8_t)(((uint16_t)color.g * scale) / 255),
        .b = (uint8_t)(((uint16_t)color.b * scale) / 255),
    };

    return out;
}

static uint8_t triangle_level(uint32_t now_ms, uint32_t period_ms, uint8_t min_level, uint8_t max_level)
{
    if (period_ms == 0 || max_level <= min_level) {
        return max_level;
    }

    uint32_t half = period_ms / 2;
    uint32_t phase = now_ms % period_ms;
    uint32_t ramp = phase < half ? phase : (period_ms - phase);

    return (uint8_t)(min_level + ((uint32_t)(max_level - min_level) * ramp) / half);
}

static bool single_blink(uint32_t now_ms, uint32_t period_ms, uint32_t on_ms)
{
    return (now_ms % period_ms) < on_ms;
}

static led_rgb_t breathe(led_rgb_t color, uint32_t now_ms, uint32_t period_ms,
                         uint8_t min_level, uint8_t max_level)
{
    return scale_color(color, triangle_level(now_ms, period_ms, min_level, max_level));
}

static led_rgb_t heartbeat(led_rgb_t base, led_rgb_t pulse, uint32_t now_ms)
{
    uint32_t phase = now_ms % 1400;

    if (phase < 90 || (phase >= 180 && phase < 270)) {
        return pulse;
    }

    return base;
}

static esp_err_t ws2812_show(led_rgb_t color)
{
    if (!s_led_channel || !s_copy_encoder) {
        return ESP_ERR_INVALID_STATE;
    }

    enum {
        WS2812_SYMBOLS = 25,
        T0H = 4,
        T0L = 9,
        T1H = 9,
        T1L = 4,
        RESET_US_TICKS = 800,
    };

    rmt_symbol_word_t symbols[WS2812_SYMBOLS];
    uint8_t grb[3] = { color.g, color.r, color.b };
    size_t symbol_index = 0;

    for (size_t byte_index = 0; byte_index < sizeof(grb); byte_index++) {
        for (int bit = 7; bit >= 0; bit--) {
            bool one = (grb[byte_index] & (1U << bit)) != 0;

            symbols[symbol_index++] = (rmt_symbol_word_t) {
                .level0 = 1,
                .duration0 = one ? T1H : T0H,
                .level1 = 0,
                .duration1 = one ? T1L : T0L,
            };
        }
    }

    symbols[symbol_index++] = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = RESET_US_TICKS,
        .level1 = 0,
        .duration1 = RESET_US_TICKS,
    };

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    esp_err_t ret = rmt_transmit(s_led_channel,
                                 s_copy_encoder,
                                 symbols,
                                 symbol_index * sizeof(symbols[0]),
                                 &tx_config);
    if (ret != ESP_OK) {
        return ret;
    }

    return rmt_tx_wait_all_done(s_led_channel, STATUS_LED_RMT_TIMEOUT_MS);
}

static bool network_ready(const net_status_t *net)
{
    return net->eth_has_ip || net->wifi_sta_connected || net->wifi_ap_enabled;
}

static bool setup_portal_only(const net_status_t *net)
{
    return net->wifi_ap_enabled && !net->eth_has_ip && !net->wifi_sta_connected;
}

static void update_warning_state(const rp_status_t *rp)
{
    if (!rp->initialized) {
        s_prev_counters_valid = false;
        return;
    }

    if (s_prev_counters_valid) {
        bool counters_increased =
            rp->remote_underruns > s_prev_underruns ||
            rp->remote_payload_errors > s_prev_payload_errors ||
            rp->remote_stream_bad_packets > s_prev_bad_packets ||
            rp->remote_stream_dropped_packets > s_prev_dropped_packets;

        if (counters_increased) {
            s_warning_until = xTaskGetTickCount() + pdMS_TO_TICKS(3500);
        }
    }

    s_prev_underruns = rp->remote_underruns;
    s_prev_payload_errors = rp->remote_payload_errors;
    s_prev_bad_packets = rp->remote_stream_bad_packets;
    s_prev_dropped_packets = rp->remote_stream_dropped_packets;
    s_prev_counters_valid = true;
}

static led_rgb_t choose_status_color(uint32_t now_ms)
{
    /*
     * Front-panel LED logic:
     *
     *   Blue breathing:
     *     The ESP32 is still booting, or the unit is only available through
     *     its Wi-Fi setup access point.
     *
     *   Cyan breathing:
     *     The unit is ready, the DAC Engine is responding, network is usable,
     *     and DAC output is stopped.
     *
     *   Green heartbeat:
     *     DAC output is running. This is the normal "laser output active" state.
     *
     *   Amber blink:
     *     The unit needs attention but is not in a hard fault state. This is
     *     used for no usable network, recent underruns, payload errors, bad
     *     stream packets, or dropped stream packets.
     *
     *   Red blink:
     *     Scan engine/RP communication is not healthy. The unit should not be
     *     considered ready until this clears.
     */
    if (s_booting) {
        return breathe(rgb(0, 14, 56), now_ms, 1800, 25, 255);
    }

    rp_status_t rp;
    net_status_t net;

    memset(&rp, 0, sizeof(rp));
    memset(&net, 0, sizeof(net));

    rp_manager_get_status(&rp);
    net_manager_get_status(&net);
    update_warning_state(&rp);

    if (!rp.initialized || rp.last_error != ESP_OK) {
        return single_blink(now_ms, 900, 180) ? rgb(56, 0, 0) : rgb(2, 0, 0);
    }

    TickType_t now_ticks = xTaskGetTickCount();
    bool warning_active = (int32_t)(s_warning_until - now_ticks) > 0;
    if (warning_active || !network_ready(&net)) {
        return single_blink(now_ms, 1100, 260) ? rgb(56, 30, 0) : rgb(2, 1, 0);
    }

    if (setup_portal_only(&net)) {
        return breathe(rgb(0, 14, 56), now_ms, 1800, 25, 255);
    }

    if (rp.remote_running) {
        return heartbeat(rgb(0, 14, 0), rgb(0, 56, 4), now_ms);
    }

    return breathe(rgb(0, 32, 40), now_ms, 2200, 35, 255);
}

static void status_led_task(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        esp_err_t ret = ws2812_show(choose_status_color(now_ms));

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "LED update failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(STATUS_LED_UPDATE_MS));
        }
    }
}

esp_err_t status_led_start(void)
{
    if (s_led_task) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_config = {
        .gpio_num = STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = STATUS_LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_config, &s_led_channel);
    if (ret != ESP_OK) {
        return ret;
    }

    rmt_copy_encoder_config_t encoder_config = {};
    ret = rmt_new_copy_encoder(&encoder_config, &s_copy_encoder);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = rmt_enable(s_led_channel);
    if (ret != ESP_OK) {
        return ret;
    }

    BaseType_t ok = xTaskCreate(
        status_led_task,
        "status_led",
        STATUS_LED_TASK_STACK,
        NULL,
        STATUS_LED_TASK_PRIORITY,
        &s_led_task
    );

    if (ok != pdPASS) {
        s_led_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WS2812B status LED started on GPIO%d", STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led_set_booting(bool booting)
{
    s_booting = booting;
}

#else

esp_err_t status_led_start(void)
{
    return ESP_OK;
}

void status_led_set_booting(bool booting)
{
    (void)booting;
}

#endif
