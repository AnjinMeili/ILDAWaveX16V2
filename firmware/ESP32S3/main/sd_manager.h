#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SD_FILE_NAME_MAX 128

typedef struct {
    char name[SD_FILE_NAME_MAX];
    unsigned long size;
    bool is_ild;
} sd_file_info_t;

typedef struct {
    bool inserted;
    bool mounted;
    int file_count;
    sd_file_info_t files[32];
} sd_status_t;

esp_err_t sd_manager_start(void);
void sd_manager_get_status(sd_status_t *status);
esp_err_t sd_manager_rescan(void);