#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool sd_ilda_enabled;
    bool idn_enabled;
    bool etherdream_enabled;
    bool ilw_enabled;
} protocol_settings_t;

esp_err_t protocol_settings_start(void);
void protocol_settings_get(protocol_settings_t *settings);
void protocol_settings_get_active(protocol_settings_t *settings);
bool protocol_settings_pending_reboot(void);
esp_err_t protocol_settings_save(const protocol_settings_t *settings);
esp_err_t protocol_settings_restore_defaults(void);
