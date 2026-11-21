#include "cube.h"

#include <math.h>
#include <string.h>

#define DIST 4.6f     /* camera to the cube's centre, in half-sides */
#define FOCAL 9.6f    /* the nearest corner stays within the 9 rows */
#define FAR_DIM 0.3f  /* the far edges at 30 %: depth reads without shading */

static const float corner[8][3] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
    {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},
};
static const uint8_t edge[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static void splat(float acc[9][16], float x, float y, float w) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            int px = x0 + dx, py = y0 + dy;
            if (px < 0 || px >= 16 || py < 0 || py >= 9) continue;
            acc[py][px] += w * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        }
}

void cube_render(float t, uint8_t lum[9][16]) {
    /* multiples of 0.01 rad/s, see CUBE_PERIOD_MS */
    float ax = t * 0.83f, ay = t * 1.17f, az = t * 0.29f;
    float sx = sinf(ax), cx = cosf(ax), sy = sinf(ay), cy = cosf(ay), sz = sinf(az), cz = cosf(az);
    float px[8], py[8], pz[8];
    for (int i = 0; i < 8; i++) {
        float x = corner[i][0], y = corner[i][1], z = corner[i][2], u;
        u = y * cx - z * sx; z = y * sx + z * cx; y = u;
        u = x * cy + z * sy; z = -x * sy + z * cy; x = u;
        u = x * cz - y * sz; y = x * sz + y * cz; x = u;
        z += DIST;
        px[i] = 7.5f + FOCAL * x / z;
        py[i] = 4.0f + FOCAL * y / z;
        pz[i] = z;
    }

    static float img[9][16], line[9][16]; /* static: the main stack is 4 KB */
    memset(img, 0, sizeof img);
    const float znear = DIST - 1.7320508f, zspan = 2 * 1.7320508f;
    for (int e = 0; e < 12; e++) {
        int a = edge[e][0], b = edge[e][1];
        float dx = px[b] - px[a], dy = py[b] - py[a], len = sqrtf(dx * dx + dy * dy);
        int n = (int)ceilf(len * 4) + 1;
        float step = len / (float)n;
        memset(line, 0, sizeof line);
        for (int i = 0; i <= n; i++) {
            float k = (float)i / (float)n;
            float depth = (pz[a] + (pz[b] - pz[a]) * k - znear) / zspan;
            float w = (1 - (1 - FAR_DIM) * depth) * (i == 0 || i == n ? step * 0.5f : step);
            splat(line, px[a] + dx * k, py[a] + dy * k, w);
        }
        /* max, not sum: corners where three edges meet would flare */
        for (int r = 0; r < 9; r++)
            for (int c = 0; c < 16; c++)
                if (line[r][c] > img[r][c]) img[r][c] = line[r][c];
    }
    /* coverage is what the eye should see, and the panel's gamma turns perceived levels into light */
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 16; c++) lum[r][c] = (uint8_t)(img[r][c] >= 1 ? 255 : img[r][c] * 255.0f + 0.5f);
}

void cube_draw(uint32_t ms, float img[9][16]) {
    uint8_t lum[9][16];
    cube_render((float)(ms % CUBE_PERIOD_MS) / 1000.0f, lum);
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++) img[y][x] = lum[y][x] / 255.0f;
}
