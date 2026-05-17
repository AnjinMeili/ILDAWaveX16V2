#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "app_config.h"
#include "protocol_settings.h"

#ifndef ENABLE_IDN_PROTOCOL
#define ENABLE_IDN_PROTOCOL 1
#endif

#ifndef ENABLE_ETHERDREAM_PROTOCOL
#define ENABLE_ETHERDREAM_PROTOCOL 1
#endif

#ifndef ENABLE_ILW_PROTOCOL
#define ENABLE_ILW_PROTOCOL 1
#endif

static const char *TAG = "protocols";
static const char *PROTOCOL_NVS_NAMESPACE = "protocfg";

static protocol_settings_t s_settings;
static protocol_settings_t s_active_settings;
static bool s_settings_loaded = false;

static void protocol_settings_set_defaults(protocol_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->sd_ilda_enabled = true;
    settings->idn_enabled = ENABLE_IDN_PROTOCOL != 0;
    settings->etherdream_enabled = ENABLE_ETHERDREAM_PROTOCOL != 0;
    settings->ilw_enabled = ENABLE_ILW_PROTOCOL != 0;
}

static bool settings_equal(const protocol_settings_t *a, const protocol_settings_t *b)
{
    return a->sd_ilda_enabled == b->sd_ilda_enabled &&
           a->idn_enabled == b->idn_enabled &&
           a->etherdream_enabled == b->etherdream_enabled &&
           a->ilw_enabled == b->ilw_enabled;
}

static void enforce_fixed_settings(protocol_settings_t *settings)
{
    settings->sd_ilda_enabled = true;
}

static esp_err_t protocol_settings_load(protocol_settings_t *settings)
{
    protocol_settings_set_defaults(settings);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PROTOCOL_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t value = 0;

    if (nvs_get_u8(nvs, "idn_en", &value) == ESP_OK) {
        settings->idn_enabled = value != 0;
    }

    if (nvs_get_u8(nvs, "ed_en", &value) == ESP_OK) {
        settings->etherdream_enabled = value != 0;
    }

    if (nvs_get_u8(nvs, "ilw_en", &value) == ESP_OK) {
        settings->ilw_enabled = value != 0;
    }

    nvs_close(nvs);
    enforce_fixed_settings(settings);

    return ESP_OK;
}

esp_err_t protocol_settings_start(void)
{
    esp_err_t ret = protocol_settings_load(&s_settings);
    if (ret != ESP_OK) {
        return ret;
    }

    s_active_settings = s_settings;
    s_settings_loaded = true;

    ESP_LOGI(TAG, "SD ILDA playback: enabled");
    ESP_LOGI(TAG, "IDN protocol: %s", s_active_settings.idn_enabled ? "enabled" : "disabled");
    ESP_LOGI(TAG, "Ether Dream protocol: %s", s_active_settings.etherdream_enabled ? "enabled" : "disabled");
    ESP_LOGI(TAG, "ILW-UDP protocol: %s", s_active_settings.ilw_enabled ? "enabled" : "disabled");

    return ESP_OK;
}

void protocol_settings_get(protocol_settings_t *settings)
{
    if (!settings) {
        return;
    }

    if (!s_settings_loaded) {
        protocol_settings_set_defaults(&s_settings);
        s_active_settings = s_settings;
        s_settings_loaded = true;
    }

    *settings = s_settings;
}

void protocol_settings_get_active(protocol_settings_t *settings)
{
    if (!settings) {
        return;
    }

    if (!s_settings_loaded) {
        protocol_settings_get(settings);
        return;
    }

    *settings = s_active_settings;
}

bool protocol_settings_pending_reboot(void)
{
    if (!s_settings_loaded) {
        return false;
    }

    return !settings_equal(&s_settings, &s_active_settings);
}

esp_err_t protocol_settings_save(const protocol_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }

    protocol_settings_t next = *settings;
    enforce_fixed_settings(&next);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PROTOCOL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, "idn_en", next.idn_enabled ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_set_u8(nvs, "ed_en", next.etherdream_enabled ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_set_u8(nvs, "ilw_en", next.ilw_enabled ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_commit(nvs);

    nvs_close(nvs);

    if (ret == ESP_OK) {
        s_settings = next;
        s_settings_loaded = true;
        ESP_LOGI(TAG, "Saved protocol settings; reboot required to apply");
    }

    return ret;
}

esp_err_t protocol_settings_restore_defaults(void)
{
    protocol_settings_t defaults;

    protocol_settings_set_defaults(&defaults);
    return protocol_settings_save(&defaults);
}
