#include "update.h"

#include <math.h>
#include <string.h>

void update_frames(uint8_t lum[UPDATE_FRAMES][9][16]) {
    /* bands eight pixels apart, so the eight frames move them exactly one band on and the loop has no seam */
    for (int f = 0; f < UPDATE_FRAMES; f++)
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 16; x++) {
                float c = 0.5f + 0.5f * cosf(6.2831853f * ((float)x + 0.6f * (float)y - (float)f) / 8);
                lum[f][y][x] = (uint8_t)(0.8f * c * c * c * 255.0f + 0.5f);
            }
}

void update_at(uint8_t lum[UPDATE_FRAMES][9][16], uint32_t ms, uint8_t out[9][16]) {
    memcpy(out, lum[ms / (UPDATE_DELAY * 11u) % UPDATE_FRAMES], 9 * 16);
}
