// ===========================================================================
// Single-file Kirby-like 2D side-scroller for GBA using Tonc + Mode 3 bitmap
// - Left/Right: move
// - A: jump (when on ground)
// - Hold A in air: float (slow descent + Up/Down to rise/fall)
// - Smooth camera, parallax clouds, platforms with basic collision
// - Everything in one file, no external assets
// Compile with devkitARM + libtonc (standard GBA homebrew setup)
// ===========================================================================

#include <tonc.h>

typedef int fixed;  // 8-bit fixed point (1 pixel = 256)

// ---------------------------------------------------------------------------
// Colors (RGB15)
// ---------------------------------------------------------------------------
const u16 CLR_SKY     = RGB15( 8, 18, 31);
const u16 CLR_GRASS   = RGB15( 4, 22,  8);
const u16 CLR_DIRT    = RGB15(16, 10,  4);
const u16 CLR_PINK    = RGB15(31, 16, 24);
const u16 CLR_DARK    = RGB15(28,  8, 16);
const u16 CLR_RED     = RGB15(31,  8,  8);
const u16 CLR_BLACK   = RGB15( 0,  0,  0);
const u16 CLR_WHITE   = RGB15(31, 31, 31);
const u16 CLR_PLATFORM= RGB15(20, 14,  6);

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------
inline int max(int a, int b) { return a > b ? a : b; }
inline int min(int a, int b) { return a < b ? a : b; }

int isqrt(int num)
{
    if (num <= 1) return num;
    int res = 0;
    for (int i = 1 << 15; i != 0; i >>= 1)
    {
        int b = res | i;
        if (num >= b << 1) { res = b; num -= b << 1; }
    }
    return res;
}

void filled_circle(int cx, int cy, int r, u16 clr)
{
    for (int dy = -r; dy <= r; dy++)
    {
        int y = cy + dy;
        if (y < 0 || y >= 160) continue;
        int dx = isqrt(r * r - dy * dy);
        int x0 = max(cx - dx, 0);
        int x1 = min(cx + dx, 239);
        if (x0 > x1) continue;
        u16 *dst = vid_mem + y * 240 + x0;
        for (int x = x0; x <= x1; x++) *dst++ = clr;
    }
}

void filled_ellipse(int cx, int cy, int rx, int ry, u16 clr)
{
    for (int dy = -ry; dy <= ry; dy++)
    {
        int y = cy + dy;
        if (y < 0 || y >= 160) continue;
        int temp = ry * ry - dy * dy;
        if (temp < 0) continue;
        int dx = rx * isqrt(temp) / ry;
        int x0 = max(cx - dx, 0);
        int x1 = min(cx + dx, 239);
        if (x0 > x1) continue;
        u16 *dst = vid_mem + y * 240 + x0;
        for (int x = x0; x <= x1; x++) *dst++ = clr;
    }
}

void draw_kirby(int cx, int cy)
{
    // Body
    filled_ellipse(cx, cy + 2, 15, 17, CLR_PINK);
    filled_ellipse(cx, cy + 9, 13, 7, CLR_DARK);      // bottom shadow

    // Arms
    filled_circle(cx - 15, cy + 3, 6, CLR_PINK);
    filled_circle(cx + 15, cy + 3, 6, CLR_PINK);

    // Feet
    filled_ellipse(cx - 7, cy + 13, 8, 7, CLR_RED);
    filled_ellipse(cx + 7, cy + 13, 8, 7, CLR_RED);

    // Eyes
    filled_ellipse(cx - 6, cy - 6, 4, 9, CLR_BLACK);
    filled_ellipse(cx + 6, cy - 6, 4, 9, CLR_BLACK);
    filled_circle(cx - 6, cy - 9, 3, CLR_WHITE);
    filled_circle(cx + 6, cy - 9, 3, CLR_WHITE);

    // Cheeks
    filled_ellipse(cx - 11, cy + 1, 6, 4, CLR_RED);
    filled_ellipse(cx + 11, cy + 1, 6, 4, CLR_RED);
}

// ---------------------------------------------------------------------------
// Clouds (parallax)
// ---------------------------------------------------------------------------
typedef struct { int wx, y, size; } Cloud;
Cloud clouds[] = {
    {  150, 30, 22}, {  450, 45, 28}, {  800, 35, 20}, { 1100, 55, 25},
    { 1400, 40, 24}, { 1800, 30, 26}, { 2200, 50, 22}, { 2600, 38, 27}
};

// ---------------------------------------------------------------------------
// Platforms
// ---------------------------------------------------------------------------
typedef struct { int wx, y, w; } Platform;
Platform platforms[] = {
    { 400, 100, 120},
    { 800,  80, 100},
    {1200, 110, 140},
    {1700,  90, 110},
    {2200, 105, 130}
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main()
{
    irq_init(NULL);
    irq_enable(II_VBLANK);

    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    fixed px = 120 << 8;      // player world position (fixed point)
    fixed py = 80 << 8;
    fixed vx = 0, vy = 0;
    int cam_x = 0;

    const fixed GRAVITY     = 0x60;
    const fixed FLOAT_GRAV  = 0x18;
    const fixed WALK_SPEED  = 0x180;
    const fixed JUMP_POWER  = -0x580;
    const int   GROUND_Y    = 120;

    while (1)
    {
        VBlankIntrWait();
        key_poll();

        // -------------------------------------------------------------------
        // Input & physics
        // -------------------------------------------------------------------
        vx = 0;
        if (key_is_down(KEY_LEFT))  vx -= WALK_SPEED;
        if (key_is_down(KEY_RIGHT)) vx += WALK_SPEED;

        bool floating = key_is_down(KEY_A) && (py >> 8) < GROUND_Y - 20;

        if (floating)
            vy += FLOAT_GRAV;
        else
            vy += GRAVITY;

        if (key_is_down(KEY_UP)   && floating) vy -= 0x100;
        if (key_is_down(KEY_DOWN) && floating) vy += 0x200;

        if (key_hit(KEY_A) && (py >> 8) >= GROUND_Y - 22) // simple ground check for initial jump
            vy = JUMP_POWER;

        px += vx;
        py += vy;

        // -------------------------------------------------------------------
        // Simple collision (ground + platforms)
        // -------------------------------------------------------------------
        bool on_ground = false;
        int feet_y = (py >> 8) + 18;
        int world_px = (px >> 8);

        // Ground
        if (feet_y >= GROUND_Y && vy >= 0)
        {
            py = (GROUND_Y - 18) << 8;
            vy = 0;
            on_ground = true;
        }

        // Platforms
        for (int i = 0; i < sizeof(platforms)/sizeof(Platform); i++)
        {
            Platform *p = &platforms[i];
            if (feet_y >= p->y && feet_y <= p->y + 22 &&
                world_px >= p->wx - 18 && world_px <= p->wx + p->w + 18 &&
                vy >= 0)
            {
                py = (p->y - 18) << 8;
                vy = 0;
                on_ground = true;
            }
        }

        // Death by falling
        if (py >> 8 > 200)
        {
            px = 120 << 8;
            py = 80 << 8;
            vy = 0;
        }

        // -------------------------------------------------------------------
        // Camera (player centered)
        // -------------------------------------------------------------------
        cam_x = (px >> 8) - 120;
        if (cam_x < 0) cam_x = 0;

        // -------------------------------------------------------------------
        // Rendering
        // -------------------------------------------------------------------
        // Sky
        for (int i = 0; i < 240*160; i++) vid_mem[i] = CLR_SKY;

        // Clouds (half-speed parallax)
        for (int i = 0; i < sizeof(clouds)/sizeof(Cloud); i++)
        {
            int sx = clouds[i].wx - (cam_x >> 1);
            if (sx > -60 && sx < 300)
            {
                filled_circle(sx,           clouds[i].y, clouds[i].size,     CLR_WHITE);
                filled_circle(sx + 18,      clouds[i].y - 8, clouds[i].size-6, CLR_WHITE);
                filled_circle(sx - 15,      clouds[i].y - 6, clouds[i].size-8, CLR_WHITE);
            }
        }

        // Ground
        for (int y = GROUND_Y; y < 160; y++)
        {
            u16 *dst = vid_mem + y * 240;
            for (int x = 0; x < 240; x++) dst[x] = CLR_GRASS;
        }
        // Dirt layer
        for (int y = 140; y < 160; y++)
        {
            u16 *dst = vid_mem + y * 240;
            for (int x = 0; x < 240; x++) dst[x] = CLR_DIRT;
        }

        // Platforms
        for (int i = 0; i < sizeof(platforms)/sizeof(Platform); i++)
        {
            Platform *p = &platforms[i];
            int sx = p->wx - cam_x;
            if (sx > -200 && sx < 440)
                for (int y = p->y; y < p->y + 20; y++)
                    if (y >= 0 && y < 160)
                    {
                        int x0 = max(sx, 0);
                        int x1 = min(sx + p->w - 1, 239);
                        if (x0 <= x1)
                        {
                            u16 *dst = vid_mem + y * 240 + x0;
                            for (int x = x0; x <= x1; x++) *dst++ = CLR_PLATFORM;
                        }
                    }
        }

        // Kirby
        draw_kirby(120, py >> 8);  // always centered horizontally

    }
    return 0;
}
