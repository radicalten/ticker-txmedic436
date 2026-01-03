#include <tonc.h>

// --- Constants & Fixed Point Math ---
#define SW 240  // Screen Width
#define SH 160  // Screen Height
#define MH 24   // Map Height
#define MW 24   // Map Width

// Using .12 Fixed point math (Tonc standard) for speed
// 1.0 = 4096 (1 << 12)
typedef int fixed;
#define F_SHIFT 12
#define F_ONE   (1 << F_SHIFT)
#define F_MUL(a, b) ((fixed)(((long long)(a) * (b)) >> F_SHIFT))
#define F_DIV(a, b) ((fixed)(((long long)(a) << F_SHIFT) / (b)))
#define INT2F(n)    ((n) << F_SHIFT)
#define F2INT(n)    ((n) >> F_SHIFT)

// Colors
#define CLR_CEILING 0x1084 // Dark Grey
#define CLR_FLOOR   0x2949 // Darker Grey
#define CLR_GUN     0x3193 // Gun metal

// --- World Map (1 = Wall, 0 = Empty) ---
const u8 worldMap[MH][MW]=
{
  {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,2,2,2,2,2,0,0,0,0,3,0,3,0,3,0,0,0,1},
  {1,0,0,0,0,0,2,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,2,0,0,0,2,0,0,0,0,3,0,0,0,3,0,0,0,1},
  {1,0,0,0,0,0,2,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,2,2,0,2,2,0,0,0,0,3,0,3,0,3,0,0,0,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,4,4,4,4,4,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,4,0,4,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,4,0,0,0,0,5,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,4,0,4,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,4,0,4,4,4,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,4,4,4,4,4,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
  {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

// Player State
fixed posX, posY;      // Position vector
fixed dirX, dirY;      // Direction vector
fixed planeX, planeY;  // Camera plane (determines FOV)

void init_player() {
    // Start position
    posX = INT2F(22);
    posY = INT2F(12);
    // Initial direction vector (North)
    dirX = -F_ONE;
    dirY = 0;
    // Camera plane (FOV) - Perpendicular to direction
    planeX = 0;
    planeY = INT2F(66) / 100; // 0.66 for roughly 66 degree FOV
}

// Draw a vertical line efficiently in Mode 3
void draw_ver_line(int x, int drawStart, int drawEnd, u16 color) {
    if(drawStart < 0) drawStart = 0;
    if(drawEnd >= SH) drawEnd = SH - 1;
    
    // Pointer to VRAM at the start pixel
    u16 *dst = &vid_mem[drawStart * SW + x];
    int height = drawEnd - drawStart;
    
    // Simple loop is fast enough for Mode 3 columns
    while(height-- >= 0) {
        *dst = color;
        dst += SW; // Move down one line
    }
}

// Draw a simple "gun" at bottom center
void draw_gun() {
    int gunW = 40;
    int gunH = 40;
    int startX = (SW - gunW) / 2;
    int startY = SH - gunH;
    
    for(int y=0; y<gunH; y++) {
        u16 *dst = &vid_mem[(startY + y) * SW + startX];
        for(int x=0; x<gunW; x++) {
            *dst++ = CLR_GUN;
        }
    }
}

int main() {
    // 1. Init Video (Mode 3, BG2 enabled)
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    init_player();
    
    // Movement speeds
    fixed moveSpeed = INT2F(10) / 100; // 0.10
    fixed rotSpeed = INT2F(8) / 100;  // 0.08

    while(1) {
        key_poll();

        // --- 1. Movement Logic ---
        
        // Move Forward
        if(key_is_down(KEY_UP)) {
            fixed nextX = posX + F_MUL(dirX, moveSpeed);
            fixed nextY = posY + F_MUL(dirY, moveSpeed);
            if(worldMap[F2INT(nextX)][F2INT(posY)] == 0) posX = nextX;
            if(worldMap[F2INT(posX)][F2INT(nextY)] == 0) posY = nextY;
        }
        // Move Backward
        if(key_is_down(KEY_DOWN)) {
            fixed nextX = posX - F_MUL(dirX, moveSpeed);
            fixed nextY = posY - F_MUL(dirY, moveSpeed);
            if(worldMap[F2INT(nextX)][F2INT(posY)] == 0) posX = nextX;
            if(worldMap[F2INT(posX)][F2INT(nextY)] == 0) posY = nextY;
        }
        // Rotate Right (Rotate vectors)
        if(key_is_down(KEY_RIGHT)) {
            // Precalculate sin/cos
            fixed c = lu_cos(F2INT(rotSpeed * 10)); // Approximate LUT usage or standard rotation
            // Actually, for arbitrary rotation without LUT lookups for specific angles, 
            // we use the standard rotation matrix. 
            // However, Tonc's lu_sin takes an integer 0-0xFFFF.
            // Let's use simple float emulation with Fixed Point for rotation matrix:
            // x' = x*cos - y*sin
            // y' = x*sin + y*cos
            // Note: For simplicity in this demo, we use a small hardcoded approx for cos/sin of rotSpeed
            // cos(0.08) ~= 0.996, sin(0.08) ~= 0.079. 
            // In Fixed 12: cos=4083, sin=327
            fixed cosR = 4083;
            fixed sinR = 327; // Negative for right? No, standard rotation.
            
            // Rotate Direction
            fixed oldDirX = dirX;
            dirX = F_MUL(dirX, cosR) - F_MUL(dirY, sinR);
            dirY = F_MUL(oldDirX, sinR) + F_MUL(dirY, cosR);
            // Rotate Plane
            fixed oldPlaneX = planeX;
            planeX = F_MUL(planeX, cosR) - F_MUL(planeY, sinR);
            planeY = F_MUL(oldPlaneX, sinR) + F_MUL(planeY, cosR);
        }
        // Rotate Left
        if(key_is_down(KEY_LEFT)) {
            fixed cosR = 4083;
            fixed sinR = -327; // Negative sin for left rotation
            
            fixed oldDirX = dirX;
            dirX = F_MUL(dirX, cosR) - F_MUL(dirY, sinR);
            dirY = F_MUL(oldDirX, sinR) + F_MUL(dirY, cosR);
            fixed oldPlaneX = planeX;
            planeX = F_MUL(planeX, cosR) - F_MUL(planeY, sinR);
            planeY = F_MUL(oldPlaneX, sinR) + F_MUL(planeY, cosR);
        }

        // --- 2. Rendering ---
        
        // A. Clear Background (Floor/Ceiling)
        // Optimization: DMA Fill is much faster than pixel plotting
        DMA_TRANSFER(&vid_mem[0], &((u16){CLR_CEILING}), (SW*SH/2) , DMA_32 , DMA_SRC_FIXED);
        DMA_TRANSFER(&vid_mem[SW*SH/2], &((u16){CLR_FLOOR}), (SW*SH/2) , DMA_32 , DMA_SRC_FIXED);

        // B. Raycasting Loop
        for(int x = 0; x < SW; x++) {
            // Calculate ray position and direction
            fixed cameraX = F_DIV(INT2F(2 * x), INT2F(SW)) - F_ONE; // x-coordinate in camera space
            fixed rayDirX = dirX + F_MUL(planeX, cameraX);
            fixed rayDirY = dirY + F_MUL(planeY, cameraX);

            // Which box of the map we're in
            int mapX = F2INT(posX);
            int mapY = F2INT(posY);

            // Length of ray from current position to next x or y-side
            fixed sideDistX;
            fixed sideDistY;

            // Length of ray from one x or y-side to next x or y-side
            // deltaDistX = abs(1 / rayDirX)
            // To avoid div by zero and handle fixed point:
            fixed deltaDistX = (rayDirX == 0) ? 0x7FFFFFFF : abs(F_DIV(F_ONE, rayDirX));
            fixed deltaDistY = (rayDirY == 0) ? 0x7FFFFFFF : abs(F_DIV(F_ONE, rayDirY));

            fixed perpWallDist;

            // Step direction
            int stepX;
            int stepY;

            int hit = 0;
            int side; // 0 for NS, 1 for EW

            // Calculate step and initial sideDist
            if (rayDirX < 0) {
                stepX = -1;
                sideDistX = F_MUL((posX - INT2F(mapX)), deltaDistX);
            } else {
                stepX = 1;
                sideDistX = F_MUL((INT2F(mapX + 1) - posX), deltaDistX);
            }
            if (rayDirY < 0) {
                stepY = -1;
                sideDistY = F_MUL((posY - INT2F(mapY)), deltaDistY);
            } else {
                stepY = 1;
                sideDistY = F_MUL((INT2F(mapY + 1) - posY), deltaDistY);
            }

            // DDA Algorithm (Digital Differential Analysis)
            // March the ray through the grid
            while (hit == 0) {
                // Jump to next map square
                if (sideDistX < sideDistY) {
                    sideDistX += deltaDistX;
                    mapX += stepX;
                    side = 0;
                } else {
                    sideDistY += deltaDistY;
                    mapY += stepY;
                    side = 1;
                }
                // Check if ray has hit a wall
                if (worldMap[mapX][mapY] > 0) hit = 1;
            }

            // Calculate distance projected on camera direction (No fisheye)
            if (side == 0) perpWallDist = (sideDistX - deltaDistX);
            else           perpWallDist = (sideDistY - deltaDistY);

            // Calculate height of line to draw on screen
            // h = SH, dist is fixed point.
            // lineHeight = (h / perpWallDist)
            int lineHeight;
            if(perpWallDist <= 0) lineHeight = SH; // Prevent div by zero/negative
            else lineHeight = F2INT(F_DIV(INT2F(SH), perpWallDist));

            // Calculate lowest and highest pixel to fill in current stripe
            int drawStart = -lineHeight / 2 + SH / 2;
            int drawEnd = lineHeight / 2 + SH / 2;

            // Pick wall color
            u16 color;
            switch(worldMap[mapX][mapY]) {
                case 1: color = CLR_RED; break;
                case 2: color = CLR_GREEN; break;
                case 3: color = CLR_BLUE; break;
                case 4: color = CLR_WHITE; break;
                default: color = CLR_YELLOW; break;
            }

            // Shade side walls darker to give 3D effect
            if (side == 1) {
                color = (color >> 1) & 0x7BEF; // Simple bit-shift dimming
            }

            // Draw the column
            draw_ver_line(x, drawStart, drawEnd, color);
        }

        // Draw HUD/Gun
        draw_gun();

        // Sync to 60FPS
        vid_vsync();
    }

    return 0;
}
