#include "panel.h"

#include <math.h>

void panel_pwm(const uint8_t lum[9][16], float brightness, uint8_t pwm[9][16]) {
    static float gamma[256];
    if (gamma[255] == 0)
        for (int i = 0; i < 256; i++) gamma[i] = powf((float)i / 255.0f, 2.2f);
    float b = brightness < 0 ? 0 : brightness > 1 ? 1 : brightness;
    static float v[9][16];
    float k = 255.0f * powf(b, 2.2f), sum = 0;
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 16; c++) sum += v[r][c] = k * gamma[lum[r][c]];
    float cap = sum > (float)PANEL_CAP ? (float)PANEL_CAP / sum : 1.0f;
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 16; c++) {
            /* in the dark a quarter-lit pixel falls under one PWM step: keep it at one step rather than
               let half the picture vanish, but let faint antialiasing fringes go dark */
            float p = v[r][c] * cap;
            pwm[r][c] = (uint8_t)(p >= 0.5f ? p + 0.5f : lum[r][c] >= 64 ? 1 : 0);
        }
}
