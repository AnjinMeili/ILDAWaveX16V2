#pragma once

void rp_uart_init(void);
void rp_uart_poll(void);
void uart_send_line(const char *s);
