#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "sdmmc_cmd.h"

#include "app_config.h"
#include "sd_manager.h"

static const char *TAG = "sd";

static sdmmc_card_t *s_card = NULL;
static SemaphoreHandle_t s_sd_mutex = NULL;

static sd_status_t s_status = {
    .inserted = false,
    .mounted = false,
    .file_count = 0,
};

static bool ends_with_ild(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }

    return strcasecmp(name + len - 4, ".ild") == 0;
}

static bool sd_card_inserted_raw(void)
{
    return gpio_get_level(SD_DET_GPIO) == 0;
}

static bool sd_card_inserted_debounced(void)
{
    bool a = sd_card_inserted_raw();
    vTaskDelay(pdMS_TO_TICKS(30));
    bool b = sd_card_inserted_raw();
    vTaskDelay(pdMS_TO_TICKS(30));
    bool c = sd_card_inserted_raw();

    return a && b && c;
}

static bool build_sd_path(char *out, size_t out_size, const char *file_name)
{
    size_t mount_len = strlen(SD_MOUNT_POINT);
    size_t name_len = strlen(file_name);

    if (mount_len + 1 + name_len + 1 > out_size) {
        ESP_LOGW(TAG, "Skipping too-long filename: %s", file_name);
        return false;
    }

    memcpy(out, SD_MOUNT_POINT, mount_len);
    out[mount_len] = '/';
    memcpy(out + mount_len + 1, file_name, name_len);
    out[mount_len + 1 + name_len] = '\0';

    return true;
}

static esp_err_t sd_mount_locked(void)
{
    if (s_status.mounted) {
        return ESP_OK;
    }

    if (!s_status.inserted) {
        ESP_LOGW(TAG, "Cannot mount: no SD card inserted");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Mounting SD card");

    esp_err_t ret = ESP_OK;

#if BOARD_VERSION == 1
    /* V1: SD is on its own SPI2/FSPI bus, matching the original firmware */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SD SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ESP_OK;
#else
    /* V2: SPI2 already initialized by W5500 in net_manager */
    ESP_LOGI(TAG, "Using shared SPI bus already initialized by W5500");
#endif

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 10000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS_GPIO;
    slot.host_id = SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT,
        &host,
        &slot,
        &mount,
        &s_card
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        s_card = NULL;
        s_status.mounted = false;
        s_status.file_count = 0;
        return ret;
    }

    s_status.mounted = true;
    s_status.file_count = 0;

    sdmmc_card_print_info(stdout, s_card);

    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);

    return ESP_OK;
}

static void sd_unmount_locked(void)
{
    if (!s_status.mounted) {
        s_status.file_count = 0;
        return;
    }

    ESP_LOGW(TAG, "Unmounting SD card");

    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);

    s_card = NULL;
    s_status.mounted = false;
    s_status.file_count = 0;

    ESP_LOGI(TAG, "SD unmounted");
}

esp_err_t sd_manager_rescan(void)
{
    if (!s_sd_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    s_status.file_count = 0;

    if (!s_status.mounted) {
        xSemaphoreGive(s_sd_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open SD root directory");
        xSemaphoreGive(s_sd_mutex);
        return ESP_FAIL;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (s_status.file_count >= SD_MAX_FILES) {
            break;
        }

        if (entry->d_name[0] == '.') {
            continue;
        }

        char path[320];

        if (!build_sd_path(path, sizeof(path), entry->d_name)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            continue;
        }

        sd_file_info_t *file = &s_status.files[s_status.file_count];

        strncpy(file->name, entry->d_name, sizeof(file->name) - 1);
        file->name[sizeof(file->name) - 1] = '\0';

        file->size = (unsigned long)st.st_size;
        file->is_ild = ends_with_ild(entry->d_name);

        s_status.file_count++;
    }

    closedir(dir);

    ESP_LOGI(TAG, "SD files: %d", s_status.file_count);

    xSemaphoreGive(s_sd_mutex);

    return ESP_OK;
}

void sd_manager_get_status(sd_status_t *status)
{
    if (!status || !s_sd_mutex) {
        return;
    }

    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_sd_mutex);
}

static void sd_hotplug_task(void *arg)
{
    bool last_inserted = false;

    while (1) {
        bool now_inserted = sd_card_inserted_debounced();

        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

        if (now_inserted != last_inserted) {
            s_status.inserted = now_inserted;

            if (now_inserted) {
                ESP_LOGI(TAG, "SD card inserted");
                esp_err_t ret = sd_mount_locked();
                if (ret == ESP_OK) {
                    xSemaphoreGive(s_sd_mutex);
                    sd_manager_rescan();
                    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
                }
            } else {
                ESP_LOGW(TAG, "SD card removed");
                sd_unmount_locked();
            }

            last_inserted = now_inserted;
        }

        xSemaphoreGive(s_sd_mutex);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t sd_manager_start(void)
{
    ESP_LOGI(TAG, "Starting SD manager");

    s_sd_mutex = xSemaphoreCreateMutex();
    if (!s_sd_mutex) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SD_DET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&cfg));

    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    s_status.inserted = sd_card_inserted_debounced();

    if (s_status.inserted) {
        ESP_LOGI(TAG, "SD card present at boot");
        esp_err_t ret = sd_mount_locked();

        xSemaphoreGive(s_sd_mutex);

        if (ret == ESP_OK) {
            sd_manager_rescan();
        }
    } else {
        ESP_LOGW(TAG, "No SD card detected at boot");
        s_status.mounted = false;
        s_status.file_count = 0;
        xSemaphoreGive(s_sd_mutex);
    }

    xTaskCreate(
        sd_hotplug_task,
        "sd_hotplug",
        4096,
        NULL,
        4,
        NULL
    );

    return ESP_OK;
}
