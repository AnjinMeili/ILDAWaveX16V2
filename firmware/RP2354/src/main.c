#include "pico/stdlib.h"

#include "ring_buffer.h"
#include "rp_uart.h"
#include "stream_rx.h"
#include "output_engine.h"
#include "rp_settings.h"

#ifdef ENABLE_HELIOS_USB
#include "tusb.h"
#include "helios_protocol.h"
#endif

int main(void)
{
    stdio_init_all();
    sleep_ms(100);

    rp_settings_load();

    rp_settings_t settings;
    rp_settings_get(&settings);

    ring_init();
    ring_configure(settings.buffer_size_samples,
                   settings.buffer_high_water_samples,
                   settings.buffer_low_water_samples);
    rp_uart_init();
    stream_rx_init();
    output_engine_init();
    output_engine_set_mirror(settings.mirror_x, settings.mirror_y);

#ifdef ENABLE_HELIOS_USB
    helios_protocol_init();
    tusb_init();
#endif

    while (true) {
#ifdef ENABLE_HELIOS_USB
        tud_task();
        helios_protocol_task();
        rp_uart_poll();

        /*
         * SPI reception is skipped while USB is active: the main loop must
         * remain free to call tud_task() and fire the Helios tick at rate.
         * When USB is not mounted both modes are inactive and SPI runs as before.
         */
        if (!tud_mounted()) {
            if (stream_rx_receive_packet(10)) {
                stream_rx_process_packet();
            }
        }
#else
        rp_uart_poll();
        if (stream_rx_receive_packet(10)) {
            stream_rx_process_packet();
        }
#endif
    }
}
