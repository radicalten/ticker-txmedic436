// iso_racer.c
//
// Minimal single-file GBA isometric-style racing demo using Tonc.
// Controls:
//   D-Pad LEFT/RIGHT : steer
//   D-Pad UP         : accelerate
//   D-Pad DOWN       : brake/reverse

#include <tonc.h>
#include <math.h>

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  160

// Colors (15-bit BGR)
#define COL_GRASS   RGB15( 4, 12,  4)
#define COL_ROAD    RGB15(16, 16, 16)
#define COL_PLAYER  RGB15(31,  0,  0)
#define COL_AI      RGB15( 0,  0, 31)

// Simple car struct in world space (top-down)
typedef struct
{
    float wx, wy;    // world position (0..~256)
    float angle;     // facing angle in radians
    float speed;     // scalar speed
} Car;

static Car player;
static Car ai;
static float aiParam = 0.25f;   // 0..1 around the track

//------------------------------------------------------------------------------
// World / track helpers
//------------------------------------------------------------------------------

// Returns non-zero if (wx,wy) is on the road.
// Track is a square ring: outer square minus inner square.
static int isRoad(int wx, int wy)
{
    const int outerMin = 32, outerMax = 224;
    const int innerMin = 96, innerMax = 160;

    // Outside outer bounds -> not road
    if (wx < outerMin || wx > outerMax || wy < outerMin || wy > outerMax)
        return 0;

    // Inside inner "infield" -> not road
    if (wx > innerMin && wx < innerMax && wy > innerMin && wy < innerMax)
        return 0;

    // Otherwise it's road
    return 1;
}

// Convert world (wx, wy) into isometric screen coords (sx, sy).
// World is a flat square; we project it into a diamond.
static void world_to_screen(int wx, int wy, int *sx, int *sy)
{
    // Simple isometric-ish projection
    int ix = (wx - wy) / 2;
    int iy = (wx + wy) / 4;

    *sx = ix + SCREEN_WIDTH / 2;   // center horizontally
    *sy = iy + 24;                 // shift down a bit
}

//------------------------------------------------------------------------------
// Drawing helpers (Mode 3)
//------------------------------------------------------------------------------

// Clear entire Mode 3 screen to a single color.
static void clear_screen(u16 clr)
{
    int count = SCREEN_WIDTH * SCREEN_HEIGHT;
    int i;
    for (i = 0; i < count; i++)
        vid_mem[i] = clr;
}

// Draw a filled rectangle with clipping.
static void draw_rect(int x, int y, int w, int h, u16 clr)
{
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT)
        return;

    int x2 = x + w;
    int y2 = y + h;

    if (x2 <= 0 || y2 <= 0)
        return;

    if (x < 0)  x = 0;
    if (y < 0)  y = 0;
    if (x2 > SCREEN_WIDTH)  x2 = SCREEN_WIDTH;
    if (y2 > SCREEN_HEIGHT) y2 = SCREEN_HEIGHT;

    for (int iy = y; iy < y2; iy++)
    {
        u16 *row = &vid_mem[iy * SCREEN_WIDTH];
        for (int ix = x; ix < x2; ix++)
            row[ix] = clr;
    }
}

// Stamp the track onto the screen by sampling world positions
// and drawing small road patches at their projected positions.
static void draw_track(void)
{
    // Coarse grid sampling in world space
    for (int wy = 32; wy <= 224; wy += 3)
    {
        for (int wx = 32; wx <= 224; wx += 3)
        {
            if (isRoad(wx, wy))
            {
                int sx, sy;
                world_to_screen(wx, wy, &sx, &sy);
                // Small 3x3 road tile
                draw_rect(sx - 1, sy - 1, 3, 3, COL_ROAD);
            }
        }
    }
}

//------------------------------------------------------------------------------
// AI path around centerline of the ring
//------------------------------------------------------------------------------

static void path_pos(float t, float *wx, float *wy)
{
    const float x1 = 64.0f, y1 = 64.0f;
    const float x2 = 192.0f, y2 = 192.0f;

    // Wrap t into [0,1)
    t = fmodf(t, 1.0f);
    if (t < 0.0f)
        t += 1.0f;

    float seg = t * 4.0f;      // 4 segments
    int   si  = (int)seg;      // which side
    float ft  = seg - si;      // fraction along that side

    switch (si)
    {
    case 0: // Top edge: (x1,y1) -> (x2,y1)
        *wx = x1 + (x2 - x1) * ft;
        *wy = y1;
        break;
    case 1: // Right edge: (x2,y1) -> (x2,y2)
        *wx = x2;
        *wy = y1 + (y2 - y1) * ft;
        break;
    case 2: // Bottom edge: (x2,y2) -> (x1,y2)
        *wx = x2 - (x2 - x1) * ft;
        *wy = y2;
        break;
    case 3: // Left edge: (x1,y2) -> (x1,y1)
    default:
        *wx = x1;
        *wy = y2 - (y2 - y1) * ft;
        break;
    }
}

//------------------------------------------------------------------------------
// Game update & draw
//------------------------------------------------------------------------------

static void update_player(void)
{
    const float TURN_RATE = 0.03f;
    const float ACCEL     = 0.05f;
    const float FRICTION  = 0.985f;
    const float MAX_FWD   = 2.0f;
    const float MAX_REV   = -1.0f;

    key_poll();

    if (key_is_down(KEY_LEFT))
        player.angle -= TURN_RATE;
    if (key_is_down(KEY_RIGHT))
        player.angle += TURN_RATE;

    if (key_is_down(KEY_UP))
        player.speed += ACCEL;
    if (key_is_down(KEY_DOWN))
        player.speed -= ACCEL * 0.7f;

    // Friction
    player.speed *= FRICTION;

    // Clamp speed
    if (player.speed >  MAX_FWD) player.speed =  MAX_FWD;
    if (player.speed <  MAX_REV) player.speed =  MAX_REV;

    // Move in facing direction
    float dx = cosf(player.angle) * player.speed;
    float dy = sinf(player.angle) * player.speed;

    float newx = player.wx + dx;
    float newy = player.wy + dy;

    if (isRoad((int)newx, (int)newy))
    {
        player.wx = newx;
        player.wy = newy;
    }
    else
    {
        // Simple bounce against walls
        player.speed *= -0.3f;
    }
}

static void update_ai(void)
{
    // Constant-speed parametric motion around centerline
    aiParam += 0.0015f;
    path_pos(aiParam, &ai.wx, &ai.wy);
}

// Draw the two cars on top of the track.
static void draw_cars(void)
{
    int sx, sy;

    // AI car
    world_to_screen((int)ai.wx, (int)ai.wy, &sx, &sy);
    draw_rect(sx - 2, sy - 2, 5, 5, COL_AI);

    // Player car
    world_to_screen((int)player.wx, (int)player.wy, &sx, &sy);
    draw_rect(sx - 2, sy - 2, 5, 5, COL_PLAYER);
}

//------------------------------------------------------------------------------
// Init and main
//------------------------------------------------------------------------------

static void init_game(void)
{
    // Mode 3, BG2 on
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    // Player starting position: somewhere on the track
    player.wx    = 64.0f;
    player.wy    = 160.0f;
    player.angle = 0.0f;
    player.speed = 0.0f;

    // AI car initial position
    ai.angle = 0.0f;
    ai.speed = 0.0f;
    path_pos(aiParam, &ai.wx, &ai.wy);
}

int main(void)
{
    init_game();

    while (1)
    {
        vid_vsync();                // Wait for VBlank

        // Clear to grass; road will be drawn on top
        clear_screen(COL_GRASS);

        // Update
        update_player();
        update_ai();

        // Draw
        draw_track();
        draw_cars();
    }

    return 0;
}
