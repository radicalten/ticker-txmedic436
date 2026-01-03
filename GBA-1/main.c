// simple_racer.c
// A minimal top-down "racer" for GBA using tonc in Mode 3.
// Controls: D-Pad UP/DOWN to accelerate/brake, LEFT/RIGHT to steer.

#include <tonc.h>
#include <math.h>
#include <stdbool.h>

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  160

// Colors
#define CLR_GRASS  RGB15(0,16,0)
#define CLR_ROAD   RGB15(10,10,10)
#define CLR_CAR    RGB15(31,0,0)
#define CLR_DIR    RGB15(31,31,31)
#define CLR_LINE   RGB15(31,31,31)

// Track parameters: rectangular loop
#define OUTER_MARGIN  10     // distance from screen edge to outer road boundary
#define INNER_MARGIN  40     // distance from screen edge to inner hole boundary

typedef struct {
    float x, y;    // position in pixels (center of car)
    float angle;   // direction in radians
    float speed;   // pixels per frame
} Car;

// Draw the track: grass + rectangular loop road + a start line
static void draw_track(void)
{
    u16 *dst = (u16*)MEM_VRAM;

    for (int y = 0; y < SCREEN_HEIGHT; y++)
    {
        for (int x = 0; x < SCREEN_WIDTH; x++)
        {
            bool in_outer = (x > OUTER_MARGIN &&
                             x < SCREEN_WIDTH  - OUTER_MARGIN &&
                             y > OUTER_MARGIN &&
                             y < SCREEN_HEIGHT - OUTER_MARGIN);

            bool in_inner = (x > INNER_MARGIN &&
                             x < SCREEN_WIDTH  - INNER_MARGIN &&
                             y > INNER_MARGIN &&
                             y < SCREEN_HEIGHT - INNER_MARGIN);

            // Road is area inside outer rect but outside inner rect.
            u16 clr = (!in_outer || in_inner) ? CLR_GRASS : CLR_ROAD;

            dst[y * SCREEN_WIDTH + x] = clr;
        }
    }

    // Simple vertical start/finish line near the top center of the road
    int sx = SCREEN_WIDTH / 2;
    for (int y = OUTER_MARGIN; y < OUTER_MARGIN + 12; y++)
    {
        for (int x = sx - 2; x <= sx + 2; x++)
        {
            if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT)
                dst[y * SCREEN_WIDTH + x] = CLR_LINE;
        }
    }
}

// Logical check if a point is on the road, using same geometry as draw_track
static bool is_on_road(float fx, float fy)
{
    int x = (int)fx;
    int y = (int)fy;

    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
        return false;

    bool in_outer = (x > OUTER_MARGIN &&
                     x < SCREEN_WIDTH  - OUTER_MARGIN &&
                     y > OUTER_MARGIN &&
                     y < SCREEN_HEIGHT - OUTER_MARGIN);

    bool in_inner = (x > INNER_MARGIN &&
                     x < SCREEN_WIDTH  - INNER_MARGIN &&
                     y > INNER_MARGIN &&
                     y < SCREEN_HEIGHT - INNER_MARGIN);

    return in_outer && !in_inner;
}

// Draw the car as a small rectangle plus a direction line
static void draw_car(const Car *car)
{
    u16 *dst = (u16*)MEM_VRAM;
    int cx = (int)car->x;
    int cy = (int)car->y;

    const int half_w = 4;   // car half width
    const int half_h = 6;   // car half height

    // Body
    for (int y = cy - half_h; y <= cy + half_h; y++)
    {
        if (y < 0 || y >= SCREEN_HEIGHT) continue;

        for (int x = cx - half_w; x <= cx + half_w; x++)
        {
            if (x < 0 || x >= SCREEN_WIDTH) continue;
            dst[y * SCREEN_WIDTH + x] = CLR_CAR;
        }
    }

    // Direction line from center in direction of angle
    int lx = cx + (int)(cosf(car->angle) * 8.0f);
    int ly = cy + (int)(sinf(car->angle) * 8.0f);

    if (lx < 0) lx = 0;
    if (lx >= SCREEN_WIDTH) lx = SCREEN_WIDTH - 1;
    if (ly < 0) ly = 0;
    if (ly >= SCREEN_HEIGHT) ly = SCREEN_HEIGHT - 1;

    m3_line(cx, cy, lx, ly, CLR_DIR);
}

int main(void)
{
    // Set Mode 3, BG2 on
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    // Initialize car starting near bottom center, facing up
    Car car;
    car.x     = SCREEN_WIDTH / 2.0f;
    car.y     = SCREEN_HEIGHT - OUTER_MARGIN - 15.0f;
    car.angle = -3.14159265f / 2.0f;  // -PI/2: up
    car.speed = 0.0f;

    // Physics constants
    const float ACCEL      = 0.06f;
    const float BRAKE      = 0.08f;
    const float MAX_SPEED  = 2.0f;
    const float TURN_RATE  = 0.06f;   // radians per frame
    const float FRICTION   = 0.02f;

    while (1)
    {
        vid_vsync();   // Wait for VBlank
        key_poll();    // Update key state

        // --- Update car ---

        // Throttle / brake
        if (key_is_down(KEY_UP))
            car.speed += ACCEL;
        if (key_is_down(KEY_DOWN))
            car.speed -= BRAKE;

        // Steering: turn more easily when moving
        if (key_is_down(KEY_LEFT))
            car.angle -= TURN_RATE * (car.speed >= 0 ? 1.0f : -1.0f);
        if (key_is_down(KEY_RIGHT))
            car.angle += TURN_RATE * (car.speed >= 0 ? 1.0f : -1.0f);

        // Friction
        if (car.speed > 0.0f)
        {
            car.speed -= FRICTION;
            if (car.speed < 0.0f)
                car.speed = 0.0f;
        }
        else if (car.speed < 0.0f)
        {
            car.speed += FRICTION;
            if (car.speed > 0.0f)
                car.speed = 0.0f;
        }

        // Clamp speed (reverse slower)
        if (car.speed >  MAX_SPEED)    car.speed =  MAX_SPEED;
        if (car.speed < -MAX_SPEED/2)  car.speed = -MAX_SPEED/2;

        float old_x = car.x;
        float old_y = car.y;

        // Move
        car.x += cosf(car.angle) * car.speed;
        car.y += sinf(car.angle) * car.speed;

        // Keep within screen bounds (basic clamp)
        if (car.x < 4.0f)                 car.x = 4.0f;
        if (car.x > SCREEN_WIDTH - 5.0f)  car.x = SCREEN_WIDTH - 5.0f;
        if (car.y < 6.0f)                 car.y = 6.0f;
        if (car.y > SCREEN_HEIGHT - 7.0f) car.y = SCREEN_HEIGHT - 7.0f;

        // Off-road penalty: slow down and pull slightly back toward old position
        if (!is_on_road(car.x, car.y))
        {
            car.x = old_x * 0.7f + car.x * 0.3f;
            car.y = old_y * 0.7f + car.y * 0.3f;
            car.speed *= 0.8f;
        }

        // --- Render ---
        draw_track();
        draw_car(&car);
    }

    return 0;
}
