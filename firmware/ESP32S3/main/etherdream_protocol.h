#pragma once

#include "esp_err.h"

#define ETHERDREAM_PROTOCOL_UDP_PORT 7654
#define ETHERDREAM_PROTOCOL_TCP_PORT 7765

/**
 * @brief Start the Ether Dream DAC protocol server.
 *
 * Starts:
 *  - UDP broadcast task (1 Hz, port 7654) for DAC discovery
 *  - TCP listener task (port 7765) for control/streaming connections
 *
 * Must be called after net_manager_start() and rp_manager_start().
 */
esp_err_t etherdream_protocol_start(void);
