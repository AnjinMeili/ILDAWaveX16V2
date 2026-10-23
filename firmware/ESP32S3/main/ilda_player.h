#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ILDA_LOOP_RETURN_BLANK_SAMPLES 64
#define ILDA_SCALE_MIN_PERCENT 10
#define ILDA_SCALE_MAX_PERCENT 100

esp_err_t ilda_player_start(const char *path, uint8_t scale_percent);
esp_err_t ilda_player_set_scale(uint8_t scale_percent);
esp_err_t ilda_player_stop(void);
esp_err_t ilda_player_stop_and_wait(uint32_t timeout_ms);
bool ilda_player_is_running(void);
size_t ilda_player_max_samples(void);
