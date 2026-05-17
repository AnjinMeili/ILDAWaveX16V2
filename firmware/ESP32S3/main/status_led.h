#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t status_led_start(void);
void status_led_set_booting(bool booting);
