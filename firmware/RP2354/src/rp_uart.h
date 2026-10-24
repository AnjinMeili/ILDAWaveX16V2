#pragma once

#include <stdbool.h>

bool rp_log_console_connected(void);
void rp_log(const char *format, ...);

void rp_uart_init(void);
void rp_uart_poll(void);
void uart_send_line(const char *s);
