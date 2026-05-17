#pragma once

#include "esp_err.h"
#include "driver/uart.h"

#define RP_UART_PORT_NUM   UART_NUM_1
#define RP_UART_TX_GPIO    6
#define RP_UART_RX_GPIO    5
#define RP_UART_BAUD_RATE  921600

esp_err_t rp_uart_start(void);
esp_err_t rp_uart_command(const char *cmd, char *response, int response_len, int timeout_ms);
esp_err_t rp_uart_read_line(char *response, int response_len, int timeout_ms);