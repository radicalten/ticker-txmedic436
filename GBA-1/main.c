// outrun_like.c
// Simple pseudo-3D “into the screen” racer demo for GBA using tonc.
// Mode 3 bitmap; draws a curved road and a simple car sprite-like rectangle.

#include <tonc.h>
#include <math.h>

#define SCREEN_W 240
#define SCREEN_H 160

#define HORIZON_Y 40
#define MAX_ROAD_HALF (SCREEN_W/2 - 8)

// Convenience pointer to Mode 3 framebuffer
#define VRAM16 ((u16*)MEM_VRAM)

// Colors
#define CLR_SKY        RGB15(8, 12, 31)
#define CLR_GRASS      RGB15(0, 20, 0)
#define CLR_ROAD       RGB15(10, 10, 10)
#define CLR_RUMBLE1    RGB15(31, 31, 31)
#define CLR_RUMBLE2    RGB15(31, 0, 0)
#define CLR_CENTERLINE RGB15(31, 31, 0)
#define CLR_CAR_BODY   RGB15(31, 0, 0)
#define CLR_CAR_HIGHL  RGB15(31, 16, 16)

// Draw a horizontal span of pixels y, from x0..x1 inclusive
static inline void draw_span(int y, int x0, int x1, u16 clr)
{
    if(y < 0 || y >= SCREEN_H)
        return;

    if(x0 < 0) x0 = 0;
    if(x1 >= SCREEN_W) x1 = SCREEN_W - 1;
    if(x0 > x1) return;

    u16 *dst = &VRAM16[y*SCREEN_W + x0];
    for(int x=x0; x<=x1; x++)
        *dst++ = clr;
}

// Clear screen to sky/grass background
static void draw_background(void)
{
    // Sky
    for(int y=0; y<HORIZON_Y; y++)
    {
        u16 *dst = &VRAM16[y*SCREEN_W];
        for(int x=0; x<SCREEN_W; x++)
            *dst++ = CLR_SKY;
    }

    // Ground (grass)
    for(int y=HORIZON_Y; y<SCREEN_H; y++)
    {
        u16 *dst = &VRAM16[y*SCREEN_W];
        for(int x=0; x<SCREEN_W; x++)
            *dst++ = CLR_GRASS;
    }
}

// Very simple pseudo-3D road renderer
static void draw_road(float scrollZ)
{
    for(int y=HORIZON_Y; y<SCREEN_H; y++)
    {
        // Perspective factor: 0 at horizon, 1 at bottom
        float p = (float)(y - HORIZON_Y) / (float)(SCREEN_H - HORIZON_Y);
        if(p < 0.01f) p = 0.01f;

        // Approximate distance along the road for this scanline
        float z = scrollZ + (SCREEN_H - y);

        // Curvature as a function of distance
        float curve =
            sinf(z * 0.035f) * 0.8f +
            sinf(z * 0.012f) * 0.4f;

        // Scale curvature; stronger near the bottom of the screen
        float curveScale = p * p;
        int   centerX    = SCREEN_W/2 + (int)(curve * 80.0f * curveScale);

        // Perspective road width
        int roadHalf = (int)(p * MAX_ROAD_HALF);
        if(roadHalf < 10) roadHalf = 10;

        int roadLeft  = centerX - roadHalf;
        int roadRight = centerX + roadHalf;

        if(roadLeft  < 0)           roadLeft  = 0;
        if(roadRight >= SCREEN_W)   roadRight = SCREEN_W-1;

        int rumbleWidth = roadHalf / 4;
        if(rumbleWidth < 2) rumbleWidth = 2;

        int leftRumbleStart   = roadLeft;
        int leftRumbleEnd     = roadLeft + rumbleWidth;
        int rightRumbleStart  = roadRight - rumbleWidth;
        int rightRumbleEnd    = roadRight;

        if(leftRumbleEnd   > rightRumbleStart)  leftRumbleEnd   = rightRumbleStart;
        if(rightRumbleEnd  < leftRumbleStart)   rightRumbleEnd  = leftRumbleStart;

        // Alternate rumble strip colors based on distance
        int stripeIndex = ((int)z / 4) & 1;
        u16 rumbleClr   = stripeIndex ? CLR_RUMBLE1 : CLR_RUMBLE2;

        // Road body color, slightly shaded by distance
        int shade = 10 + (int)(5.0f * (1.0f - p));
        if(shade > 15) shade = 15;
        u16 roadClr = RGB15(shade, shade, shade);

        // Left rumble strip
        draw_span(y, leftRumbleStart, leftRumbleEnd, rumbleClr);
        // Right rumble strip
        draw_span(y, rightRumbleStart, rightRumbleEnd, rumbleClr);

        // Road interior
        draw_span(y, leftRumbleEnd+1, rightRumbleStart-1, roadClr);

        // Center dashed line
        int centerStripeHalf = roadHalf / 16;
        if(centerStripeHalf < 1) centerStripeHalf = 1;

        // Dashed pattern along Z
        if( ((int)(z / 8)) & 1 )
        {
            int stripeLeft  = centerX - centerStripeHalf;
            int stripeRight = centerX + centerStripeHalf;

            if(stripeLeft  < leftRumbleEnd+1)    stripeLeft  = leftRumbleEnd+1;
            if(stripeRight > rightRumbleStart-1) stripeRight = rightRumbleStart-1;

            draw_span(y, stripeLeft, stripeRight, CLR_CENTERLINE);
        }
    }
}

// Simple rectangle "car" at bottom of the screen
static void draw_car(float playerX)
{
    int carWidth  = 32;
    int carHeight = 20;
    int carY      = SCREEN_H - carHeight - 6;

    // Map playerX in [-1,1] to on-screen offset
    int maxOffset = SCREEN_W/2 - carWidth/2 - 6;
    if(maxOffset < 8) maxOffset = 8;

    int carCenterX = SCREEN_W/2 + (int)(playerX * (float)maxOffset);
    int carX       = carCenterX - carWidth/2;

    if(carX < 0) carX = 0;
    if(carX + carWidth >= SCREEN_W)
        carX = SCREEN_W - carWidth - 1;

    // Simple rectangular car with a lighter mid stripe
    for(int y=0; y<carHeight; y++)
    {
        int yy = carY + y;
        u16 bodyClr = CLR_CAR_BODY;

        // Slight highlight around the middle
        if(y > 4 && y < 10)
            bodyClr = CLR_CAR_HIGHL;

        u16 *dst = &VRAM16[yy*SCREEN_W + carX];
        for(int x=0; x<carWidth; x++)
            *dst++ = bodyClr;
    }
}

int main(void)
{
    // Mode 3, BG2 on
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    float scrollZ = 0.0f;
    float speed   = 1.5f;   // world units per frame
    float playerX = 0.0f;   // -1.0 (left) .. +1.0 (right)

    while(1)
    {
        vid_vsync();

        // Input (use tonc's key constants, read directly from REG_KEYINPUT)
        u16 keys = ~REG_KEYINPUT & KEY_MASK;

        if(keys & KEY_LEFT)
            playerX -= 0.04f;
        if(keys & KEY_RIGHT)
            playerX += 0.04f;

        if(keys & KEY_UP)
            speed += 0.03f;
        if(keys & KEY_DOWN)
            speed -= 0.03f;

        // Clamp values
        if(playerX < -1.0f) playerX = -1.0f;
        if(playerX >  1.0f) playerX =  1.0f;

        if(speed < 0.5f) speed = 0.5f;
        if(speed > 4.0f) speed = 4.0f;

        scrollZ += speed;

        // Render frame
        draw_background();
        draw_road(scrollZ);
        draw_car(playerX);
    }

    return 0;
}
