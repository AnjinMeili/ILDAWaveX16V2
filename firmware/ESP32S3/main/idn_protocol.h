#pragma once

#include "esp_err.h"

#define IDN_PROTOCOL_UDP_PORT 7255

/**
 * @brief Start the IDN (ILDA Digital Network) protocol server.
 *
 * Implements:
 *   - IDN-Hello over UDP port 7255 (discovery, ping, service map)
 *   - IDN-RT simplex (0x40) and acknowledged (0x41) realtime streaming
 *   - IDN-Stream Laser Projector Continuous Graphic mode (chunk type 0x01)
 *   - Tag dictionary decoder for the ISP-DB25 compatible default layout
 *     and any producer-supplied layout with X/Y (with optional 16-bit
 *     precision) and R/G/B/I channels
 *   - 1-second link timeout per IDN-Hello spec
 *   - Galvo-hold padding on ring underrun
 *
 * Must be called after net_manager_start() and rp_manager_start().
 */
esp_err_t idn_protocol_start(void);
