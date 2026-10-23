#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ILDA_LOOP_RETURN_BLANK_SAMPLES 64
#define ILDA_SCALE_MIN_PERCENT 10
#define ILDA_SCALE_MAX_PERCENT 100

#define ILDA_PATH_MAX 192
#define ILDA_FOLDER_MAX_FILES 512
#define ILDA_FOLDER_MAX_DEPTH 4
#define ILDA_FOLDER_HOLD_DEFAULT_S 5
#define ILDA_FOLDER_HOLD_MAX_S 3600
#define ILDA_FOLDER_PLAYS_DEFAULT 1
#define ILDA_FOLDER_PLAYS_MAX 1000

esp_err_t ilda_player_start(const char *path, uint8_t scale_percent);
esp_err_t ilda_player_start_folder(const char *dir_path, bool recursive,
                                   uint32_t hold_ms, uint16_t animation_plays,
                                   uint8_t scale_percent);
esp_err_t ilda_player_set_scale(uint8_t scale_percent);
esp_err_t ilda_player_stop(void);
esp_err_t ilda_player_stop_and_wait(uint32_t timeout_ms);
bool ilda_player_is_running(void);
size_t ilda_player_max_samples(void);
void ilda_player_get_current(char *path, size_t path_len, bool *folder);
