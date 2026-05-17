#pragma once

#include "esp_err.h"

#define ILW_PROTOCOL_UDP_PORT 45457

esp_err_t ilw_protocol_start(void);
void ilw_protocol_stop(void);
