// main.c
// Minimal “DOOM-ish” FPS for GBA using tonc: a Wolfenstein-style raycaster.
// Single file. No textures, just shaded walls + floor/ceiling.
// Renders in MODE 4 at half horizontal resolution (120 “cells” = 240 pixels).

#include <tonc.h>

// ---------- Raycaster config ----------
#define SCR_H           160
#define CELLS_W         120      // each cell is 2 horizontal pixels in MODE 4
#define FOV_ANG         112      // in 0..511 angle units (512 = 360deg). ~78.75deg
#define ANG_MAX         512

// Fixed-point: tonc sin/cos LUT is Q12 (1.0 == 4096)
#define Q               12
#define ONE             (1<<Q)
#define TILE            (1<<Q)

// Ray marching
#define STEP_Q          128       // 1/32 tile per step (since TILE=4096)
#define MAX_STEPS       256       // max distance ~ 256*(1/32)=8 tiles

// Movement
#define MOVE_Q          120       // ~0.029 tiles per frame
#define ROT_SPD         4         // angle units per frame (512 units = 360deg)

// Colors (palette indices)
enum {
    C_BLACK   = 0,
    C_CEIL    = 1,
    C_FLOOR   = 2,
    C_WALL0   = 10,  // near
    C_WALL1   = 11,
    C_WALL2   = 12,
    C_WALL3   = 13   // far
};

// ---------- MODE 4 page flipping ----------
#define M4_PAGE0 ((u16*)0x6000000)
#define M4_PAGE1 ((u16*)0x600A000)

static u16 *g_draw = M4_PAGE1;   // we start displaying page0, so draw on page1

static inline void page_flip(void)
{
    REG_DISPCNT ^= DCNT_PAGE;
    // If PAGE bit is 1, display page1 -> draw page0; else display page0 -> draw page1.
    g_draw = (REG_DISPCNT & DCNT_PAGE) ? M4_PAGE0 : M4_PAGE1;
}

// ---------- Small helpers ----------
static inline int wrap_ang(int a)
{
    a %= ANG_MAX;
    if(a < 0) a += ANG_MAX;
    return a;
}

// Convert difference to [-256, 255] range
static inline int ang_diff(int a, int b)
{
    int d = a - b;
    d %= ANG_MAX;
    if(d < -ANG_MAX/2) d += ANG_MAX;
    if(d >  ANG_MAX/2) d -= ANG_MAX;
    return d;
}

// MODE 4 “cell” write: writes both pixels in the pair to the same palette index
static inline void m4_cell_put(int cx, int y, u8 c)
{
    // cx: 0..119, y: 0..159
    g_draw[y*CELLS_W + cx] = (u16)c | ((u16)c<<8);
}

static inline void m4_fill_half(int y0, int y1, u8 c)
{
    u16 v = (u16)c | ((u16)c<<8);
    for(int y=y0; y<y1; y++)
    {
        u16 *row = &g_draw[y*CELLS_W];
        for(int x=0; x<CELLS_W; x++)
            row[x] = v;
    }
}

static inline void m4_vline_cell(int cx, int y0, int y1, u8 c)
{
    if(y0 < 0) y0 = 0;
    if(y1 > SCR_H) y1 = SCR_H;
    if(y0 >= y1) return;

    u16 v = (u16)c | ((u16)c<<8);
    u16 *p = &g_draw[y0*CELLS_W + cx];
    for(int y=y0; y<y1; y++, p += CELLS_W)
        *p = v;
}

// ---------- Map ----------
#define MAP_W 16
#define MAP_H 16

static const u8 g_map[MAP_H][MAP_W] =
{
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,0,0,0,0,1,1,1,0,0,0,1,0,1},
    {1,0,1,0,0,0,0,0,0,1,0,0,0,1,0,1},
    {1,0,1,0,0,1,1,1,0,1,0,1,0,0,0,1},
    {1,0,0,0,0,1,0,1,0,0,0,1,0,0,0,1},
    {1,0,0,1,0,1,0,1,1,1,0,1,0,1,0,1},
    {1,0,0,1,0,0,0,0,0,0,0,0,0,1,0,1},
    {1,0,0,1,0,0,0,1,1,1,0,0,0,1,0,1},
    {1,0,0,0,0,0,0,1,0,1,0,0,0,0,0,1},
    {1,0,1,1,1,0,0,1,0,1,0,1,1,1,0,1},
    {1,0,0,0,1,0,0,0,0,0,0,1,0,0,0,1},
    {1,0,0,0,1,0,1,1,1,1,0,1,0,0,0,1},
    {1,0,1,0,0,0,0,0,0,0,0,0,0,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

static inline int is_wall_q12(int xq, int yq)
{
    int mx = xq >> Q;
    int my = yq >> Q;
    if(mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H)
        return 1;
    return g_map[my][mx] != 0;
}

// ---------- Player ----------
static int g_px = 2*TILE + TILE/2;  // Q12
static int g_py = 2*TILE + TILE/2;  // Q12
static int g_ang = 0;              // 0..511

// ---------- Rendering ----------
static inline u8 wall_shade_from_dist(int dist_q)
{
    // dist_q is Q12. Convert to “tiles” by >>12.
    int t = dist_q >> Q;
    if(t < 2) return C_WALL0;
    if(t < 4) return C_WALL1;
    if(t < 6) return C_WALL2;
    return C_WALL3;
}

static void render_frame(void)
{
    // background: ceiling + floor
    m4_fill_half(0, SCR_H/2, C_CEIL);
    m4_fill_half(SCR_H/2, SCR_H, C_FLOOR);

    // raycast per column (cell)
    for(int cx=0; cx<CELLS_W; cx++)
    {
        // angle offset across FOV
        int rel = cx - (CELLS_W/2);
        int ang_off = (rel * FOV_ANG) / CELLS_W;
        int ray_ang = wrap_ang(g_ang + ang_off);

        // tonc LUT trig: lu_cos/lu_sin are Q12
        int dx = lu_cos(ray_ang);
        int dy = lu_sin(ray_ang);

        int rx = g_px;
        int ry = g_py;

        int hit_dist = MAX_STEPS * STEP_Q; // fallback
        int hit = 0;

        for(int s=0; s<MAX_STEPS; s++)
        {
            rx += (dx * STEP_Q) >> Q;
            ry += (dy * STEP_Q) >> Q;

            if(is_wall_q12(rx, ry))
            {
                hit = 1;
                hit_dist = (s+1) * STEP_Q;
                break;
            }
        }

        // Fish-eye correction: multiply by cos(angle difference)
        int d = ang_diff(ray_ang, g_ang);
        if(d < 0) d = -d;                 // cos is even
        int cos_fix = lu_cos(d);          // Q12
        int dist_corr = (hit_dist * cos_fix) >> Q;
        if(dist_corr < STEP_Q) dist_corr = STEP_Q;

        // Project wall height: wall_h_px = (SCR_H / dist)
        // dist_corr is Q12; numerator in Q12 to keep integer division stable.
        int wall_h = (SCR_H << Q) / dist_corr;     // pixels
        if(wall_h > SCR_H) wall_h = SCR_H;

        int y0 = (SCR_H - wall_h)/2;
        int y1 = y0 + wall_h;

        u8 wc = hit ? wall_shade_from_dist(dist_corr) : C_BLACK;
        m4_vline_cell(cx, y0, y1, wc);
    }

    // simple center “crosshair” (blocky because of cell rendering)
    m4_cell_put(CELLS_W/2, SCR_H/2, C_BLACK);
    m4_cell_put(CELLS_W/2, SCR_H/2 - 1, C_BLACK);
    m4_cell_put(CELLS_W/2, SCR_H/2 + 1, C_BLACK);
}

// ---------- Main ----------
int main(void)
{
    // Video setup
    REG_DISPCNT = DCNT_MODE4 | DCNT_BG2;  // MODE 4, BG2 on, page0 displayed (PAGE bit = 0)
    g_draw = M4_PAGE1;                    // draw on the non-displayed page

    // Palette
    pal_bg_mem[C_BLACK] = RGB15(0,0,0);
    pal_bg_mem[C_CEIL]  = RGB15(10, 14, 20);
    pal_bg_mem[C_FLOOR] = RGB15(6, 6, 7);

    pal_bg_mem[C_WALL0] = RGB15(28, 28, 28);
    pal_bg_mem[C_WALL1] = RGB15(20, 20, 20);
    pal_bg_mem[C_WALL2] = RGB15(14, 14, 14);
    pal_bg_mem[C_WALL3] = RGB15(9,  9,  9);

    while(1)
    {
        key_poll();

        // Rotation
        if(key_is_down(KEY_LEFT))  g_ang = wrap_ang(g_ang - ROT_SPD);
        if(key_is_down(KEY_RIGHT)) g_ang = wrap_ang(g_ang + ROT_SPD);

        // Movement intent
        int fwd = 0;
        int str = 0;
        if(key_is_down(KEY_UP))    fwd += MOVE_Q;
        if(key_is_down(KEY_DOWN))  fwd -= MOVE_Q;
        if(key_is_down(KEY_B))     str += MOVE_Q;
        if(key_is_down(KEY_A))     str -= MOVE_Q;

        // Convert movement to world delta (Q12)
        int fx = (lu_cos(g_ang) * fwd) >> Q;
        int fy = (lu_sin(g_ang) * fwd) >> Q;

        int str_ang = wrap_ang(g_ang + 128); // +90 degrees
        int sx = (lu_cos(str_ang) * str) >> Q;
        int sy = (lu_sin(str_ang) * str) >> Q;

        int nx = g_px + fx + sx;
        int ny = g_py + fy + sy;

        // Collision (simple sliding)
        if(!is_wall_q12(nx, g_py)) g_px = nx;
        if(!is_wall_q12(g_px, ny)) g_py = ny;

        render_frame();

        vid_vsync();
        page_flip();
    }
}
