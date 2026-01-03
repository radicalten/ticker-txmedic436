// ---------------------------------------------------------------------------
// Simple top-down single-screen racing demo for GBA using tonc (Mode 3).
//
// Controls:
//   D-Pad Up    = accelerate
//   D-Pad Down  = reverse / brake
//   D-Pad Left  = steer left
//   D-Pad Right = steer right
//
// Build: use a typical tonc/devkitARM setup, e.g. link with -ltonc -lm
// ---------------------------------------------------------------------------

#include <tonc.h>
#include <math.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Constants and configuration
// ---------------------------------------------------------------------------

#define SCREEN_W 240
#define SCREEN_H 160

// Track layout (all in screen coordinates)
#define TRACK_OUTER_LEFT    20
#define TRACK_OUTER_RIGHT   220
#define TRACK_OUTER_TOP     10
#define TRACK_OUTER_BOTTOM  150

#define TRACK_INNER_LEFT    60
#define TRACK_INNER_RIGHT   180
#define TRACK_INNER_TOP     40
#define TRACK_INNER_BOTTOM  120

// Car physics
#define CAR_ACCEL       0.08f      // acceleration per frame when holding UP
#define CAR_REVERSE     0.06f      // reverse / braking per frame when holding DOWN
#define CAR_FRICTION    0.02f      // passive slowdown per frame
#define CAR_MAX_SPEED   2.5f       // max forward speed (pixels/frame)
#define CAR_MAX_REVERSE 1.2f       // max reverse speed (pixels/frame)
#define CAR_TURN_SPEED  0.06f      // radians per frame when steering

#define PI_F 3.1415926f

// Colors (Mode 3, 15-bit RGB)
#define CLR_GRASS   RGB15(0,12,0)
#define CLR_ROAD    RGB15(10,10,10)
#define CLR_BORDER  RGB15(20,20,20)
#define CLR_LINE    RGB15(31,31,31)
#define CLR_CAR     RGB15(31,0,0)
#define CLR_CAR_NOSE RGB15(31,31,0)

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct
{
    float x, y;    // position (center in pixels)
    float speed;   // scalar speed (pixels per frame, signed: forward=+)
    float angle;   // radians, 0=right, pi/2=down
} Car;

// ---------------------------------------------------------------------------
// Drawing helpers (Mode 3)
// ---------------------------------------------------------------------------

static inline void m3_plot_safe(int x, int y, COLOR clr)
{
    if(x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)
        return;
    vid_mem[y * SCREEN_W + x] = clr;
}

// Inclusive rectangle (x1,y1) .. (x2,y2)
static void m3_rect_fill(int x1, int y1, int x2, int y2, COLOR clr)
{
    int x, y;

    if(x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if(y1 > y2) { int t = y1; y1 = y2; y2 = t; }

    if(x1 < 0) x1 = 0;
    if(y1 < 0) y1 = 0;
    if(x2 >= SCREEN_W) x2 = SCREEN_W - 1;
    if(y2 >= SCREEN_H) y2 = SCREEN_H - 1;

    for(y = y1; y <= y2; y++)
    {
        u16 *row = &vid_mem[y * SCREEN_W];
        for(x = x1; x <= x2; x++)
            row[x] = clr;
    }
}

// Simple 1-pixel-wide vertical line
static void m3_vline(int x, int y1, int y2, COLOR clr)
{
    int y;
    if(x < 0 || x >= SCREEN_W)
        return;
    if(y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if(y1 < 0) y1 = 0;
    if(y2 >= SCREEN_H) y2 = SCREEN_H - 1;

    for(y = y1; y <= y2; y++)
        vid_mem[y * SCREEN_W + x] = clr;
}

// Simple 1-pixel-wide horizontal line
static void m3_hline(int x1, int x2, int y, COLOR clr)
{
    int x;
    if(y < 0 || y >= SCREEN_H)
        return;
    if(x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if(x1 < 0) x1 = 0;
    if(x2 >= SCREEN_W) x2 = SCREEN_W - 1;

    u16 *row = &vid_mem[y * SCREEN_W];
    for(x = x1; x <= x2; x++)
        row[x] = clr;
}

// ---------------------------------------------------------------------------
// Track logic
// ---------------------------------------------------------------------------

// Returns true if the given position is on the drivable road.
static bool is_on_road(float fx, float fy)
{
    int x = (int)fx;
    int y = (int)fy;

    bool inside_outer =
        (x >= TRACK_OUTER_LEFT  && x <= TRACK_OUTER_RIGHT &&
         y >= TRACK_OUTER_TOP   && y <= TRACK_OUTER_BOTTOM);

    bool inside_inner =
        (x > TRACK_INNER_LEFT && x < TRACK_INNER_RIGHT &&
         y > TRACK_INNER_TOP  && y < TRACK_INNER_BOTTOM);

    return inside_outer && !inside_inner;
}

// Draw the static track.
static void draw_track(void)
{
    // Background grass
    m3_rect_fill(0, 0, SCREEN_W-1, SCREEN_H-1, CLR_GRASS);

    // Road ring
    m3_rect_fill(
        TRACK_OUTER_LEFT,
        TRACK_OUTER_TOP,
        TRACK_OUTER_RIGHT,
        TRACK_OUTER_BOTTOM,
        CLR_ROAD
    );

    // Infield (grass again)
    m3_rect_fill(
        TRACK_INNER_LEFT+1,
        TRACK_INNER_TOP+1,
        TRACK_INNER_RIGHT-1,
        TRACK_INNER_BOTTOM-1,
        CLR_GRASS
    );

    // Borders (simple darker lines)
    // Outer border
    m3_rect_fill(TRACK_OUTER_LEFT, TRACK_OUTER_TOP,
                 TRACK_OUTER_RIGHT, TRACK_OUTER_TOP, CLR_BORDER);           // top
    m3_rect_fill(TRACK_OUTER_LEFT, TRACK_OUTER_BOTTOM,
                 TRACK_OUTER_RIGHT, TRACK_OUTER_BOTTOM, CLR_BORDER);       // bottom
    m3_rect_fill(TRACK_OUTER_LEFT, TRACK_OUTER_TOP,
                 TRACK_OUTER_LEFT, TRACK_OUTER_BOTTOM, CLR_BORDER);        // left
    m3_rect_fill(TRACK_OUTER_RIGHT, TRACK_OUTER_TOP,
                 TRACK_OUTER_RIGHT, TRACK_OUTER_BOTTOM, CLR_BORDER);       // right

    // Inner border
    m3_rect_fill(TRACK_INNER_LEFT, TRACK_INNER_TOP,
                 TRACK_INNER_RIGHT, TRACK_INNER_TOP, CLR_BORDER);
    m3_rect_fill(TRACK_INNER_LEFT, TRACK_INNER_BOTTOM,
                 TRACK_INNER_RIGHT, TRACK_INNER_BOTTOM, CLR_BORDER);
    m3_rect_fill(TRACK_INNER_LEFT, TRACK_INNER_TOP,
                 TRACK_INNER_LEFT, TRACK_INNER_BOTTOM, CLR_BORDER);
    m3_rect_fill(TRACK_INNER_RIGHT, TRACK_INNER_TOP,
                 TRACK_INNER_RIGHT, TRACK_INNER_BOTTOM, CLR_BORDER);

    // Center lines (just for looks)
    m3_hline(TRACK_OUTER_LEFT+5, TRACK_OUTER_RIGHT-5,
             (TRACK_OUTER_TOP+TRACK_INNER_TOP)/2, CLR_LINE);
    m3_hline(TRACK_OUTER_LEFT+5, TRACK_OUTER_RIGHT-5,
             (TRACK_OUTER_BOTTOM+TRACK_INNER_BOTTOM)/2, CLR_LINE);

    // Start/finish line near bottom center
    int sx = (TRACK_OUTER_LEFT + TRACK_OUTER_RIGHT) / 2;
    int sy1 = TRACK_OUTER_BOTTOM - 16;
    int sy2 = TRACK_OUTER_BOTTOM;
    m3_rect_fill(sx-2, sy1, sx+2, sy2, CLR_LINE);
}

// ---------------------------------------------------------------------------
// Car logic
// ---------------------------------------------------------------------------

static void update_car(Car *c)
{
    float accel = 0.0f;
    u16 keys = key_curr_state();    // key_poll() must be called before this

    if(keys & KEY_UP)
        accel += CAR_ACCEL;
    if(keys & KEY_DOWN)
        accel -= CAR_REVERSE;

    // Update speed with acceleration
    c->speed += accel;

    // Clamp speed
    if(c->speed > CAR_MAX_SPEED)
        c->speed = CAR_MAX_SPEED;
    if(c->speed < -CAR_MAX_REVERSE)
        c->speed = -CAR_MAX_REVERSE;

    // Apply friction
    if(c->speed > 0.0f)
    {
        c->speed -= CAR_FRICTION;
        if(c->speed < 0.0f)
            c->speed = 0.0f;
    }
    else if(c->speed < 0.0f)
    {
        c->speed += CAR_FRICTION;
        if(c->speed > 0.0f)
            c->speed = 0.0f;
    }

    // Steering
    if(keys & KEY_LEFT)
        c->angle -= CAR_TURN_SPEED;
    if(keys & KEY_RIGHT)
        c->angle += CAR_TURN_SPEED;

    // Keep angle in -pi..pi (not strictly necessary, but keeps numbers small)
    if(c->angle > PI_F)
        c->angle -= 2.0f * PI_F;
    else if(c->angle < -PI_F)
        c->angle += 2.0f * PI_F;

    // Move
    float old_x = c->x;
    float old_y = c->y;

    float cs = cosf(c->angle);
    float sn = sinf(c->angle);

    c->x += cs * c->speed;
    c->y += sn * c->speed;

    // Collision with track: bounce back when leaving road
    if(!is_on_road(c->x, c->y))
    {
        c->x = old_x;
        c->y = old_y;
        c->speed *= -0.4f;       // simple bounce/lose energy
    }

    // Clamp to screen just in case
    if(c->x < 4)   c->x = 4;
    if(c->x > SCREEN_W-5) c->x = SCREEN_W-5;
    if(c->y < 4)   c->y = 4;
    if(c->y > SCREEN_H-5) c->y = SCREEN_H-5;
}

// Draw the car as a small square with a nose pixel showing heading.
static void draw_car(const Car *c)
{
    int cx = (int)(c->x + 0.5f);
    int cy = (int)(c->y + 0.5f);

    // Car body
    m3_rect_fill(cx-3, cy-3, cx+3, cy+3, CLR_CAR);

    // Nose pixel to show direction
    float cs = cosf(c->angle);
    float sn = sinf(c->angle);
    int nx = cx + (int)(cs * 6.0f);
    int ny = cy + (int)(sn * 6.0f);
    m3_plot_safe(nx, ny, CLR_CAR_NOSE);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(void)
{
    // Set video mode: Mode 3, BG2 on
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    // Initialize car roughly at bottom center of outer track, facing up
    Car car;
    car.x = (TRACK_OUTER_LEFT + TRACK_OUTER_RIGHT) * 0.5f;
    car.y = TRACK_OUTER_BOTTOM - 12.0f;
    car.speed = 0.0f;
    car.angle = -PI_F / 2.0f;    // up

    while(1)
    {
        vid_vsync();     // tonc helper: wait for VBlank

        key_poll();      // update key state in tonc

        update_car(&car);

        draw_track();
        draw_car(&car);
    }

    // Unreachable on GBA, but keeps compiler happy
    return 0;
}
