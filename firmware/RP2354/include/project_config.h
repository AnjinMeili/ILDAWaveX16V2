#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define POINT_CHANNELS               8u
#define POINT_BYTES                  (POINT_CHANNELS * 2u)

#define INGRESS_POINTS_PER_BLOCK     64u
#define INGRESS_BLOCK_COUNT          8u
#define INGRESS_BLOCK_BYTES          (INGRESS_POINTS_PER_BLOCK * POINT_BYTES)

#define TX_POINT_BATCH_POINTS        16u
#define TX_WORDS_PER_POINT           9u
#define TX_WORD_COUNT                (TX_POINT_BATCH_POINTS * TX_WORDS_PER_POINT)

#define TARGET_POINT_RATE_HZ         30000u
#define DAC_SPI_BITRATE_HZ           ((uint32_t)(TARGET_POINT_RATE_HZ * TX_WORDS_PER_POINT * 24u))

#define PIN_DAC_MISO                 0u
#define PIN_DAC_CS                   1u
#define PIN_DAC_SCK                  2u
#define PIN_DAC_MOSI                 3u
#define PIN_RP_UART_TX               4u
#define PIN_RP_UART_RX               5u
#define PIN_TRACE_OUTPUT_RUNNING     10u
#define PIN_TRACE_FORCED_BLANK       11u

#define PIN_SPI0_RX                  16u
#define PIN_SPI0_CSN                 17u
#define PIN_SPI0_SCK                 18u
#define PIN_SPI0_TX                  19u

#define SPI_INGRESS_PORT             spi0
#define SPI_INGRESS_BAUD             1000000u
#define SPI_INGRESS_POLARITY         SPI_CPOL_0
#define SPI_INGRESS_PHASE            SPI_CPHA_0
#define SPI_INGRESS_BIT_ORDER        SPI_MSB_FIRST

#define DAC_SPI_PIO                  pio0
#define DAC_SPI_SM                   0u
#define DAC_PIO_CLKDIV               1.0f

#define DAC_REG_NOOP                 0x00u
#define DAC_REG_DEVICE_ID            0x01u
#define DAC_REG_SYNC                 0x02u
#define DAC_REG_CONFIG               0x03u
#define DAC_REG_GAIN                 0x04u
#define DAC_REG_TRIGGER              0x05u
#define DAC_REG_DAC0                 0x08u

#define DAC_GAIN_2X_ALL_CHANNELS     0x00FFu
#define DAC_SYNC_ALL_CHANNELS        0x00FFu
#define DAC_TRIGGER_LDAC_MASK        0x0010u

/*
 * Upstream SPI transport frame.
 * One validated transport frame becomes exactly one 8-channel DAC point.
 *
 * Byte 0  : SYNC0
 * Byte 1  : SYNC1
 * Byte 2  : sequence
 * Byte 3  : flags (reserved for future use)
 * Byte 4..19  : payload = 8x uint16 little-endian samples
 * Byte 20 : CRC-8 over bytes 2..19 (sequence, flags, payload)
 */
#define SPI_FRAME_SYNC0              0xA5u
#define SPI_FRAME_SYNC1              0x5Au
#define SPI_FRAME_FLAGS_OFFSET       3u
#define SPI_FRAME_PAYLOAD_OFFSET     4u
#define SPI_FRAME_CRC_OFFSET         (SPI_FRAME_PAYLOAD_OFFSET + POINT_BYTES)
#define SPI_FRAME_BYTES              (SPI_FRAME_CRC_OFFSET + 1u)
#define SPI_FRAME_CRC_INIT           0x00u
#define SPI_FRAME_CRC_POLY           0x07u

/*
 * Max bytes captured for one CS-low packet from the upstream SPI master.
 * Keep every transaction <= this size.
 */
#define INGRESS_PACKET_MAX_BYTES     1024u
#define INGRESS_PACKET_BUFFER_COUNT  2u

#ifdef PICO_DEFAULT_LED_PIN
#define STATUS_LED_PIN               PICO_DEFAULT_LED_PIN
#endif

#endif
