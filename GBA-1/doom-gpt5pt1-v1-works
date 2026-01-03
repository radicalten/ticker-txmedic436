// main.c - simple DOOM-like (Wolf3D-style) raycaster for GBA using tonc
// Single source file, uses Mode 3 bitmap rendering.
//
// Controls:
//   D-Pad UP / DOWN : move forward / backward
//   D-Pad LEFT / RIGHT : rotate view
//   L / R : strafe left / right

#include <tonc.h>

#define SCREEN_W 240
#define SCREEN_H 160

#define MAP_W 16
#define MAP_H 16

// Simple 16x16 map: 0 = empty, 1..4 = wall types
static const int worldMap[MAP_H][MAP_W] =
{
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,2,0,0,0,0,0,0,0,3,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,4,4,4,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,4,0,4,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,4,4,4,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,2,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,3,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

// Small helper to avoid <math.h>
static inline float f_abs(float x)
{
    return (x < 0.0f) ? -x : x;
}

int main(void)
{
    // Basic GBA/tonc init
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    // Player state
    float posX = 8.0f;   // player position in map space
    float posY = 8.0f;
    float dirX = -1.0f;  // initial direction vector
    float dirY =  0.0f;
    float planeX = 0.0f;     // camera plane (controls FOV)
    float planeY = 0.66f;    // FOV ~ 66°

    // Movement / rotation constants
    const float moveSpeed = 0.08f;     // per frame
    // Rotation of about 0.1 radians (~5.7 degrees)
    const float rotCos   = 0.99500417f;  // cos(0.1)
    const float rotSin   = 0.09983342f;  // sin(0.1)

    // Colors
    const u16 skyColor   = RGB15(8, 8, 16);
    const u16 floorColor = RGB15(4, 12, 4);

    while(1)
    {
        // Wait for VBlank
        vid_vsync();
        key_poll();

        // --- Movement ---

        // Forward
        if(key_is_down(KEY_UP))
        {
            float newX = posX + dirX * moveSpeed;
            float newY = posY + dirY * moveSpeed;

            // Basic collision: only move if target tile is empty (0)
            if(worldMap[(int)posY][(int)newX] == 0) posX = newX;
            if(worldMap[(int)newY][(int)posX] == 0) posY = newY;
        }
        // Backward
        if(key_is_down(KEY_DOWN))
        {
            float newX = posX - dirX * moveSpeed;
            float newY = posY - dirY * moveSpeed;

            if(worldMap[(int)posY][(int)newX] == 0) posX = newX;
            if(worldMap[(int)newY][(int)posX] == 0) posY = newY;
        }

        // Strafe left (L)
        if(key_is_down(KEY_L))
        {
            float newX = posX - planeX * moveSpeed;
            float newY = posY - planeY * moveSpeed;

            if(worldMap[(int)posY][(int)newX] == 0) posX = newX;
            if(worldMap[(int)newY][(int)posX] == 0) posY = newY;
        }

        // Strafe right (R)
        if(key_is_down(KEY_R))
        {
            float newX = posX + planeX * moveSpeed;
            float newY = posY + planeY * moveSpeed;

            if(worldMap[(int)posY][(int)newX] == 0) posX = newX;
            if(worldMap[(int)newY][(int)posX] == 0) posY = newY;
        }

        // Rotate left
        if(key_is_down(KEY_LEFT))
        {
            float oldDirX   = dirX;
            float oldPlaneX = planeX;

            dirX   = dirX   * rotCos - dirY   * rotSin;
            dirY   = oldDirX * rotSin + dirY  * rotCos;

            planeX = planeX * rotCos - planeY * rotSin;
            planeY = oldPlaneX * rotSin + planeY * rotCos;
        }

        // Rotate right
        if(key_is_down(KEY_RIGHT))
        {
            float oldDirX   = dirX;
            float oldPlaneX = planeX;

            // Rotation by -angle
            dirX   = dirX   * rotCos + dirY   * rotSin;
            dirY   = -oldDirX * rotSin + dirY * rotCos;

            planeX = planeX * rotCos + planeY * rotSin;
            planeY = -oldPlaneX * rotSin + planeY * rotCos;
        }

        // --- Clear screen with sky/floor ---

        int y, x;
        for(y=0; y<SCREEN_H/2; y++)
            for(x=0; x<SCREEN_W; x++)
                m3_plot(x, y, skyColor);

        for(; y<SCREEN_H; y++)
            for(x=0; x<SCREEN_W; x++)
                m3_plot(x, y, floorColor);

        // --- Raycasting per vertical stripe ---

        // Use 2-pixel wide columns for speed
        for(x=0; x<SCREEN_W; x += 2)
        {
            // Camera space x coordinate: -1 to +1
            float cameraX = 2.0f * x / (float)SCREEN_W - 1.0f;

            float rayDirX = dirX + planeX * cameraX;
            float rayDirY = dirY + planeY * cameraX;

            int mapX = (int)posX;
            int mapY = (int)posY;

            float sideDistX;
            float sideDistY;

            float deltaDistX = (rayDirX == 0.0f) ? 1e30f : f_abs(1.0f / rayDirX);
            float deltaDistY = (rayDirY == 0.0f) ? 1e30f : f_abs(1.0f / rayDirY);

            int stepX, stepY;
            int hit = 0;
            int side = 0;

            // Initial step and side distances
            if(rayDirX < 0.0f)
            {
                stepX = -1;
                sideDistX = (posX - mapX) * deltaDistX;
            }
            else
            {
                stepX = 1;
                sideDistX = (mapX + 1.0f - posX) * deltaDistX;
            }

            if(rayDirY < 0.0f)
            {
                stepY = -1;
                sideDistY = (posY - mapY) * deltaDistY;
            }
            else
            {
                stepY = 1;
                sideDistY = (mapY + 1.0f - posY) * deltaDistY;
            }

            // DDA: step through map grid until we hit a wall
            while(!hit)
            {
                if(sideDistX < sideDistY)
                {
                    sideDistX += deltaDistX;
                    mapX += stepX;
                    side = 0;
                }
                else
                {
                    sideDistY += deltaDistY;
                    mapY += stepY;
                    side = 1;
                }

                // Map is fully enclosed by walls, so no need for bounds check:
                if(worldMap[mapY][mapX] > 0)
                    hit = 1;
            }

            // Perpendicular distance to the wall
            float perpWallDist;
            if(side == 0)
                perpWallDist = sideDistX - deltaDistX;
            else
                perpWallDist = sideDistY - deltaDistY;

            if(perpWallDist <= 0.0f)
                perpWallDist = 0.1f;

            int lineHeight = (int)(SCREEN_H / perpWallDist);

            int drawStart = -lineHeight / 2 + SCREEN_H / 2;
            if(drawStart < 0) drawStart = 0;

            int drawEnd = lineHeight / 2 + SCREEN_H / 2;
            if(drawEnd >= SCREEN_H) drawEnd = SCREEN_H - 1;

            // Choose wall color based on tile
            int tile = worldMap[mapY][mapX];
            u16 color;
            switch(tile)
            {
                case 1: color = RGB15(31, 0, 0);  break; // red
                case 2: color = RGB15(0, 31, 0);  break; // green
                case 3: color = RGB15(0, 0, 31);  break; // blue
                case 4: color = RGB15(31, 31, 0); break; // yellow
                default: color = RGB15(31, 31, 31); break;
            }

            // Simple shading for y-sides
            if(side == 1)
                color = (color >> 1) & 0x7BDE;

            // Draw the vertical wall slice, 2 pixels wide for speed
            for(y = drawStart; y <= drawEnd; y++)
            {
                m3_plot(x,   y, color);
                if(x+1 < SCREEN_W)
                    m3_plot(x+1, y, color);
            }
        }

        // Optional crosshair at center of screen
        {
            int cx = SCREEN_W/2;
            int cy = SCREEN_H/2;
            u16 crossColor = RGB15(31, 31, 31);

            int i;
            for(i=-4; i<=4; i++)
            {
                m3_plot(cx + i, cy, crossColor);
                m3_plot(cx, cy + i, crossColor);
            }
        }
    }

    // Never reached
    return 0;
}
