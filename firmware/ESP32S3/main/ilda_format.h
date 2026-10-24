#pragma once

#include <stdint.h>

static inline uint16_t ilda_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline int ilda_record_size_for_format(uint8_t format)
{
    if (format == 0) return 8;
    if (format == 1) return 6;
    if (format == 4) return 10;
    if (format == 5) return 8;
    return -1;
}

static inline void ilda_default_index_color(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /*
       ILDA format 0/1 uses an indexed palette. Many simple ILDA files do not
       include a format-2 palette section and instead rely on the common ILDA
       default palette. TEST_12K.ILD is one of those files: it uses indices
       0, 8, 16, 24, 40, 48, 56.

       The previous code used idx & 7, so all multiples of 8 became index 0
       -> black. That made the mirrors move while the laser stayed off.

       Keep compatibility with tiny 0..7 palettes, but also support common
       64-entry ILDA-style indices by mapping 0,8,16,...,56 to 0..7.
    */
    static const uint8_t pal8[8][3] = {
        {0,   0,   0  }, /* 0 black */
        {255, 0,   0  }, /* 1 red */
        {0,   255, 0  }, /* 2 green */
        {255, 255, 0  }, /* 3 yellow */
        {0,   0,   255}, /* 4 blue */
        {255, 0,   255}, /* 5 magenta */
        {0,   255, 255}, /* 6 cyan */
        {255, 255, 255}, /* 7 white */
    };

    uint8_t pal_index;

    if (idx < 8) {
        pal_index = idx;
    } else {
        pal_index = idx >> 3;
        if (pal_index > 7) {
            pal_index = 7;
        }
    }

    const uint8_t *c = pal8[pal_index];
    *r = c[0];
    *g = c[1];
    *b = c[2];
}
