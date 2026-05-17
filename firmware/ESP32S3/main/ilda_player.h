#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t ilda_player_start(const char *path);
esp_err_t ilda_player_stop(void);
esp_err_t ilda_player_stop_and_wait(uint32_t timeout_ms);
bool ilda_player_is_running(void);
