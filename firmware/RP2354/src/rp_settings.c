#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include "ring_buffer.h"
#include "rp_settings.h"

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#define SETTINGS_MAGIC   0x49574442u
#define SETTINGS_VERSION 1u
#define SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - (2 * FLASH_SECTOR_SIZE))

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t buffer_size_samples;
    uint32_t buffer_high_water_samples;
    uint32_t buffer_low_water_samples;
    uint8_t  mirror_x;
    uint8_t  mirror_y;
    uint8_t  _pad[2];
    uint32_t checksum;
} settings_record_t;

static rp_settings_t s_settings = {
    .buffer_size_samples      = RP_RING_SAMPLES,
    .buffer_high_water_samples = RP_RING_HIGH_WATER,
    .buffer_low_water_samples  = RP_RING_LOW_WATER,
    .mirror_x = false,
    .mirror_y = true,
};

static const settings_record_t *s_flash_record =
    (const settings_record_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);

typedef struct {
    uint8_t page[FLASH_PAGE_SIZE];
} flash_save_ctx_t;

static uint32_t clamp_capacity(uint32_t value)
{
    if (value < RP_RING_MIN_SAMPLES) {
        return RP_RING_MIN_SAMPLES;
    }

    if (value > RP_RING_SAMPLES) {
        return RP_RING_SAMPLES;
    }

    return value;
}

static uint32_t clamp_high_water(uint32_t capacity, uint32_t value)
{
    uint32_t max_high_water = capacity - RP_RING_PACKET_MARGIN;

    if (value < RP_RING_PACKET_MARGIN) {
        return RP_RING_PACKET_MARGIN;
    }

    if (value > max_high_water) {
        return max_high_water;
    }

    return value;
}

static uint32_t clamp_low_water(uint32_t high_water, uint32_t value)
{
    uint32_t max_low_water = high_water > RP_RING_PACKET_MARGIN ?
                             high_water - RP_RING_PACKET_MARGIN : 0;

    if (value > max_low_water) {
        return max_low_water;
    }

    return value;
}

static rp_settings_t normalized_settings(uint32_t capacity,
                                         uint32_t high_water,
                                         uint32_t low_water,
                                         bool mirror_x,
                                         bool mirror_y)
{
    rp_settings_t settings;

    settings.buffer_size_samples = clamp_capacity(capacity);
    settings.buffer_high_water_samples =
        clamp_high_water(settings.buffer_size_samples, high_water);
    settings.buffer_low_water_samples =
        clamp_low_water(settings.buffer_high_water_samples, low_water);
    settings.mirror_x = mirror_x;
    settings.mirror_y = mirror_y;

    return settings;
}

static uint32_t record_checksum(const settings_record_t *record)
{
    return record->magic ^
           (record->version * 0x45d9f3bu) ^
           (record->buffer_size_samples * 0x119de1f3u) ^
           (record->buffer_high_water_samples * 0x3449c5abu) ^
           (record->buffer_low_water_samples * 0x27d4eb2du) ^
           ((uint32_t)record->mirror_x * 0x1b873593u) ^
           ((uint32_t)record->mirror_y * 0xe654c4ebu);
}

void rp_settings_load(void)
{
    if (s_flash_record->magic == SETTINGS_MAGIC &&
        s_flash_record->version == SETTINGS_VERSION &&
        s_flash_record->checksum == record_checksum(s_flash_record)) {
        s_settings = normalized_settings(
            s_flash_record->buffer_size_samples,
            s_flash_record->buffer_high_water_samples,
            s_flash_record->buffer_low_water_samples,
            s_flash_record->mirror_x != 0,
            s_flash_record->mirror_y != 0);
    } else {
        s_settings = normalized_settings(
            RP_RING_SAMPLES,
            RP_RING_HIGH_WATER,
            RP_RING_LOW_WATER,
            false, true);
    }
}

void rp_settings_get(rp_settings_t *settings)
{
    if (!settings) {
        return;
    }

    *settings = s_settings;
}

static void flash_save_execute(void *param)
{
    flash_save_ctx_t *ctx = (flash_save_ctx_t *)param;

    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, ctx->page, sizeof(ctx->page));
}

bool rp_settings_save(uint32_t buffer_size_samples,
                      uint32_t buffer_high_water_samples,
                      uint32_t buffer_low_water_samples,
                      bool mirror_x,
                      bool mirror_y)
{
    flash_save_ctx_t ctx;
    settings_record_t record;

    s_settings = normalized_settings(buffer_size_samples,
                                     buffer_high_water_samples,
                                     buffer_low_water_samples,
                                     mirror_x,
                                     mirror_y);

    memset(&ctx, 0xff, sizeof(ctx));
    memset(&record, 0, sizeof(record));

    record.magic                    = SETTINGS_MAGIC;
    record.version                  = SETTINGS_VERSION;
    record.buffer_size_samples      = s_settings.buffer_size_samples;
    record.buffer_high_water_samples = s_settings.buffer_high_water_samples;
    record.buffer_low_water_samples  = s_settings.buffer_low_water_samples;
    record.mirror_x                 = s_settings.mirror_x ? 1u : 0u;
    record.mirror_y                 = s_settings.mirror_y ? 1u : 0u;
    record.checksum                 = record_checksum(&record);

    memcpy(ctx.page, &record, sizeof(record));

    return flash_safe_execute(flash_save_execute, &ctx, 1000) == PICO_OK;
}

void rp_settings_save_and_reboot(uint32_t buffer_size_samples,
                                 uint32_t buffer_high_water_samples,
                                 uint32_t buffer_low_water_samples,
                                 bool mirror_x,
                                 bool mirror_y)
{
    if (!rp_settings_save(buffer_size_samples,
                          buffer_high_water_samples,
                          buffer_low_water_samples,
                          mirror_x,
                          mirror_y)) {
        return;
    }

    watchdog_reboot(0, 0, 0);

    for (;;) {
        tight_loop_contents();
    }
}
