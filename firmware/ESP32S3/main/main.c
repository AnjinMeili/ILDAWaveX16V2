#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_err.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "net_manager.h"
#include "web_server.h"
#include "sd_manager.h"
#include "rp_manager.h"
#include "status_led.h"
#include "protocol_settings.h"
#include "etherdream_protocol.h"
#include "idn_protocol.h"
#include "ilw_protocol.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting network stack");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    ESP_LOGI(TAG, "Starting status LED");
    ret = status_led_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Status LED unavailable: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Starting network manager");
    ESP_ERROR_CHECK(net_manager_start());

    ESP_LOGI(TAG, "Starting RP manager");
    ESP_ERROR_CHECK(rp_manager_start());

    ESP_LOGI(TAG, "Loading protocol settings");
    ESP_ERROR_CHECK(protocol_settings_start());

    ESP_LOGI(TAG, "Starting web server");
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "Starting SD manager");
    ESP_ERROR_CHECK(sd_manager_start());

    protocol_settings_t protocols;
    protocol_settings_get_active(&protocols);

    if (protocols.etherdream_enabled) {
        ESP_LOGI(TAG, "Starting Ether Dream protocol server");
        ESP_ERROR_CHECK(etherdream_protocol_start());
    } else {
        ESP_LOGI(TAG, "Ether Dream protocol server disabled");
    }

    if (protocols.idn_enabled) {
        ESP_LOGI(TAG, "Starting IDN protocol server");
        ESP_ERROR_CHECK(idn_protocol_start());
    } else {
        ESP_LOGI(TAG, "IDN protocol server disabled");
    }

    if (protocols.ilw_enabled) {
        ESP_LOGI(TAG, "Starting ILW-UDP protocol server");
        ESP_ERROR_CHECK(ilw_protocol_start());
    } else {
        ESP_LOGI(TAG, "ILW-UDP protocol server disabled");
    }

    status_led_set_booting(false);

    ESP_LOGI(TAG, "Firmware running");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
