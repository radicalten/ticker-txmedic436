/*
 * Kirby-like GBA Side Scroller
 * Uses Tonc library (http://www.coranac.com/tonc/)
 *
 * Build with:
 *   gcc -c -mthumb-interwork -mthumb -O2 -I/path/to/tonc/include main.c -o main.o
 *   gcc -specs=gba.specs -o game.elf main.o -L/path/to/tonc/lib -ltonc
 *   objcopy -O binary game.elf game.gba
 */

#include <tonc.h>

/* ========================================================================
 * CONSTANTS
 * ======================================================================== */
#define SCREEN_W        240
#define SCREEN_H        160
#define MAP_W           2048    // total level width in pixels
#define MAP_H           256     // total level height

#define GRAVITY         0x0300  // fixed-point gravity (Q8.8)
#define JUMP_VEL       -0x0600  // jump velocity (Q8.8)
#define MOVE_SPEED      0x0180  // horizontal speed (Q8.8)
#define MAX_FALL_SPEED  0x0800  // terminal velocity

#define KIRBY_W         24
#define KIRBY_H         24
#define ENEMY_W         16
#define ENEMY_H         16

#define MAX_ENEMIES     12
#define MAX_PARTICLES   20

/* ========================================================================
 * FIXED-POINT MATH (Q8.8)
 * ======================================================================== */
#define FIX8(x)         ((x) << 8)
#define FIX8_INT(x)     ((x) >> 8)
#define FIX8_FRAC(x)    ((x) & 0xFF)

typedef s32 fix8;

/* ========================================================================
 * GAME STATE
 * ======================================================================== */
typedef struct {
    fix8 x, y;
    fix8 vx, vy;
    int  w, h;
    int  on_ground;
    int  facing;       // 1 = right, -1 = left
    int  inhaling;
    int  inhale_timer;
    int  alive;
    int  score;
} Kirby;

typedef struct {
    fix8 x, y;
    fix8 vx, vy;
    int  w, h;
    int  alive;
    int  type;         // 0 = walker, 1 = floater
    int  anim_frame;
    int  inhale_timer; // countdown before being spat out
} Enemy;

typedef struct {
    fix8 x, y;
    fix8 vx, vy;
    int  life;
    int  color;
} Particle;

typedef struct {
    int x, y, w, h;   // platform rects in world space
} Platform;

/* ========================================================================
 * GLOBALS
 * ======================================================================== */
Kirby  kirby;
Enemy  enemies[MAX_ENEMIES];
Particle particles[MAX_PARTICLES];
Platform platforms[32];
int    num_platforms = 0;
int    camera_x = 0;
int    game_frame = 0;
int    score = 0;

/* ========================================================================
 * SPRITE TILE DATA (procedurally generated)
 * ======================================================================== */
// Kirby tiles: 4 tiles for a 24x24 sprite (using 32x32 OBJ with transparency)
// We'll use a 32x32 16-color OBJ and draw into tile RAM directly.

u16 *sprite_tile_mem = (u16*)OBJ_BASE_ADR;  // 0x06010000
u16 *bg_tile_mem     = (u16*)CHAR_BASE_ADR(0); // 0x06000000
u16 *bg_map_mem      = (u16*)SCREEN_BASE_ADR(0); // 0x06000000 (mirrored)
u16 *obj_attr_mem    = (u16*)OAM;

/* ========================================================================
 * PALETTE SETUP
 * ======================================================================== */
void init_palettes(void)
{
    // Sprite palette (OBJ palette starts at 0x05000200)
    u16 *obj_pal = (u16*)MEM_PAL_OBJ;

    // Color 0 = transparent
    obj_pal[0] = CLR_WHITE;

    // Kirby colors
    obj_pal[1] = RGB15(31, 15, 20);  // pink body
    obj_pal[2] = RGB15(31, 20, 25);  // light pink
    obj_pal[3] = RGB15(20, 10, 15);  // dark pink
    obj_pal[4] = CLR_BLACK;          // eyes
    obj_pal[5] = CLR_RED;            // cheeks
    obj_pal[6] = CLR_BLUE;           // feet
    obj_pal[7] = RGB15(0, 0, 20);    // dark blue

    // Enemy colors
    obj_pal[8]  = RGB15(20, 20, 20); // dark enemy body
    obj_pal[9]  = RGB15(31, 0, 0);   // red enemy
    obj_pal[10] = RGB15(31, 31, 0);  // yellow enemy
    obj_pal[11] = CLR_BLACK;         // enemy eyes
    obj_pal[12] = RGB15(0, 31, 0);   // green enemy
    obj_pal[13] = RGB15(31, 0, 31);  // purple enemy
    obj_pal[14] = CLR_WHITE;         // highlight
    obj_pal[15] = RGB15(15, 15, 15); // grey

    // Background palette
    u16 *bg_pal = (u16*)MEM_PAL_BG;
    bg_pal[0]  = RGB15(10, 15, 31);  // sky blue
    bg_pal[1]  = RGB15(5, 10, 25);   // darker sky
    bg_pal[2]  = RGB15(0, 20, 0);    // grass green
    bg_pal[3]  = RGB15(0, 15, 0);    // dark grass
    bg_pal[4]  = RGB15(20, 15, 10);  // dirt brown
    bg_pal[5]  = RGB15(15, 10, 5);   // dark dirt
    bg_pal[6]  = RGB15(25, 25, 25);  // white cloud
    bg_pal[7]  = RGB15(20, 20, 20);  // grey cloud
    bg_pal[8]  = RGB15(15, 10, 5);   // platform top
    bg_pal[9]  = RGB15(25, 20, 15);  // platform highlight
    bg_pal[10] = RGB15(31, 31, 31);  // star white
    bg_pal[11] = RGB15(31, 31, 0);   // star yellow
    bg_pal[12] = RGB15(20, 20, 0);   // star dark yellow
    bg_pal[13] = RGB15(31, 20, 0);   // orange
    bg_pal[14] = RGB15(10, 10, 10);  // dark
    bg_pal[15] = RGB15(0, 0, 0);     // black
}

/* ========================================================================
 * DRAW KIRBY SPRITE TILES
 * ======================================================================== */
void draw_kirby_tiles(void)
{
    // Kirby is 24x24, we use a 32x32 (16x16 tile) OBJ
    // Tiles 0-3 in sprite tile memory
    u16 *tiles = sprite_tile_mem;
    int t;

    // Clear tiles
    for (t = 0; t < 64; t++) {
        for (int i = 0; i < 8; i++) {
            tiles[t * 8 + i] = 0;
        }
    }

    // Draw Kirby pixel by pixel into 4 tiles (2x2 grid of 8x8 tiles)
    // Tile layout: tile0(0,0) tile1(1,0)
    //              tile2(0,1) tile3(1,1)
    for (int py = 0; py < 32; py++) {
        for (int px = 0; px < 32; px++) {
            // Center Kirby in 32x32 (offset by 4)
            int kx = px - 4;
            int ky = py - 4;
            if (kx < 0 || kx >= 24 || ky < 0 || ky >= 24)
                continue;

            u16 color = 0; // transparent

            // Body: circle-ish shape
            int cx = 12, cy = 12;
            int dx = kx - cx, dy = ky - cy;
            int dist = (dx * dx + dy * dy) >> 2;

            if (dist < 100) {
                // Body
                if (dist < 90)
                    color = 1; // pink
                else
                    color = 2; // light pink edge

                // Eyes
                if (ky >= 8 && ky <= 12) {
                    if ((kx >= 7 && kx <= 9) || (kx >= 15 && kx <= 17)) {
                        color = 4; // black eyes
                        if (ky >= 9 && ky <= 10 &&
                            ((kx >= 8 && kx <= 9) || (kx >= 16 && kx <= 17)))
                            color = 14; // eye highlight
                    }
                }

                // Cheeks (blush)
                if (ky >= 13 && ky <= 15) {
                    if ((kx >= 4 && kx <= 6) || (kx >= 18 && kx <= 20))
                        color = 5; // red cheeks
                }

                // Mouth
                if (ky == 15 && kx >= 10 && kx <= 14)
                    color = 4;
            }

            // Feet
            if (ky >= 20 && ky <= 23) {
                if ((kx >= 4 && kx <= 9) || (kx >= 15 && kx <= 20)) {
                    if (ky >= 21 || (kx >= 5 && kx <= 8) || (kx >= 16 && kx <= 19))
                        color = 6; // blue feet
                }
            }

            // Determine which tile
            int tile_x = px / 8;
            int tile_y = py / 8;
            int tile_idx = tile_y * 2 + tile_x;
            int tile_px = px % 8;
            int tile_py = py % 8;

            // Each tile is 8 words (8 pixels per row, 4 bits per pixel)
            u16 *tile = &tiles[tile_idx * 8 + tile_py];
            if (tile_px < 4)
                *tile = (*tile & 0xFFF0) | color;
            else
                *tile = (*tile & 0xFF0F) | (color << 4);
        }
    }
}

/* ========================================================================
 * DRAW ENEMY SPRITE TILES
 * ======================================================================== */
void draw_enemy_tiles(void)
{
    // Enemy tiles start at tile 4 (after Kirby's 4 tiles)
    u16 *tiles = &sprite_tile_mem[4 * 8];
    int types = 3; // 3 enemy types

    for (int type = 0; type < types; type++) {
        u16 base_color = 8 + type; // palette indices 8, 9, 12
        u16 eye_color = 11;

        for (int t = 0; t < 4; t++) { // 4 tiles per enemy (32x32 but we use 16x16 area)
            for (int i = 0; i < 8; i++)
                tiles[type * 32 + t * 8 + i] = 0;
        }

        // Draw 16x16 enemy in top-left of 32x32 tile area
        for (int py = 0; py < 16; py++) {
            for (int px = 0; px < 16; px++) {
                u16 color = 0;
                int cx = 8, cy = 8;
                int dx = px - cx, dy = py - cy;
                int dist = (dx * dx + dy * dy) >> 2;

                if (dist < 40) {
                    color = base_color;
                    // Eyes
                    if (py >= 5 && py <= 7) {
                        if ((px >= 4 && px <= 5) || (px >= 10 && px <= 11))
                            color = eye_color;
                    }
                    // Spikes on top
                    if (py <= 3) {
                        if ((px >= 3 && px <= 4) || (px >= 7 && px <= 8) ||
                            (px >= 11 && px <= 12))
                            color = base_color + 1 < 15 ? base_color + 1 : base_color;
                    }
                }

                int tile_x = px / 8;
                int tile_y = py / 8;
                int tile_idx = type * 4 + tile_y * 2 + tile_x;
                int tile_px = px % 8;
                int tile_py = py % 8;

                u16 *tile = &tiles[tile_idx * 8 + tile_py];
                if (tile_px < 4)
                    *tile = (*tile & 0xFFF0) | color;
                else
                    *tile = (*tile & 0xFF0F) | (color << 4);
            }
        }
    }
}

/* ========================================================================
 * DRAW INHALED ENEMY TILES (smaller, spinning)
 * ======================================================================== */
void draw_inhaled_tile(void)
{
    u16 *tiles = &sprite_tile_mem[16 * 8]; // tile 16
    for (int i = 0; i < 8; i++)
        tiles[i] = 0;

    // Small 8x8 ball
    for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {
            int dx = px - 4, dy = py - 4;
            u16 color = 0;
            if (dx * dx + dy * dy < 12)
                color = 1; // pink ball
            else if (dx * dx + dy * dy < 16)
                color = 2;

            if (px < 4)
                tiles[py] = (tiles[py] & 0xFFF0) | color;
            else
                tiles[py] = (tiles[py] & 0xFF0F) | (color << 4);
        }
    }
}

/* ========================================================================
 * BUILD BACKGROUND
 * ======================================================================== */
void build_background(void)
{
    // Clear background
    for (int i = 0; i < 32 * 32; i++)
        bg_map_mem[i] = 0;

    // Draw sky gradient (tiles 0-3 are just colored)
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < 64; tx++) {
            u16 tile = (ty < 8) ? 0 : 1;
            bg_map_mem[ty * 64 + tx] = tile;
        }
    }

    // Draw clouds
    int cloud_positions[][2] = {
        {5, 2}, {20, 3}, {35, 1}, {50, 4}, {60, 2},
        {10, 5}, {45, 3}, {55, 5}
    };
    for (int c = 0; c < 8; c++) {
        int cx = cloud_positions[c][0];
        int cy = cloud_positions[c][1];
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 4; dx++) {
                if (dx == 0 && dy == 0) continue;
                if (dx == 3 && dy == 0) continue;
                int tx = cx + dx;
                int ty = cy + dy;
                if (tx < 64 && ty < 32)
                    bg_map_mem[ty * 64 + tx] = 6 + (dy == 0 ? 0 : 1);
            }
        }
    }

    // Draw stars
    for (int i = 0; i < 30; i++) {
        int sx = (i * 37 + 5) % 64;
        int sy = (i * 23 + 1) % 6;
        bg_map_mem[sy * 64 + sx] = 10 + (i % 3);
    }
}

/* ========================================================================
 * DRAW PLATFORMS TO BACKGROUND
 * ======================================================================== */
void draw_platforms(void)
{
    for (int p = 0; p < num_platforms; p++) {
        int px = platforms[p].x;
        int py = platforms[p].y;
        int pw = platforms[p].w;
        int ph = platforms[p].h;

        int start_tx = px / 8;
        int start_ty = py / 8;
        int end_tx = (px + pw - 1) / 8;
        int end_ty = (py + ph - 1) / 8;

        for (int ty = start_ty; ty <= end_ty; ty++) {
            for (int tx = start_tx; tx <= end_tx; tx++) {
                if (tx < 0 || tx >= 64 || ty < 0 || ty >= 32)
                    continue;

                u16 tile = 4; // dirt
                // Top row = grass
                if (ty == start_ty)
                    tile = 2;
                // Left/right edges
                if (tx == start_tx || tx == end_ty)
                    tile = 8; // platform edge

                bg_map_mem[ty * 64 + tx] = tile;
            }
        }
    }
}

/* ========================================================================
 * LEVEL SETUP
 * ======================================================================== */
void setup_level(void)
{
    num_platforms = 0;

    // Ground
    platforms[num_platforms++] = (Platform){0, 128, 2048, 32};

    // Floating platforms
    platforms[num_platforms++] = (Platform){100, 100, 64, 8};
    platforms[num_platforms++] = (Platform){250, 80, 48, 8};
    platforms[num_platforms++] = (Platform){400, 96, 80, 8};
    platforms[num_platforms++] = (Platform){550, 72, 48, 8};
    platforms[num_platforms++] = (Platform){700, 104, 64, 8};
    platforms[num_platforms++] = (Platform){850, 88, 48, 8};
    platforms[num_platforms++] = (Platform){1000, 64, 96, 8};
    platforms[num_platforms++] = (Platform){1200, 96, 48, 8};
    platforms[num_platforms++] = (Platform){1350, 80, 64, 8};
    platforms[num_platforms++] = (Platform){1500, 104, 48, 8};
    platforms[num_platforms++] = (Platform){1650, 72, 80, 8};
    platforms[num_platforms++] = (Platform){1800, 96, 64, 8};

    // Walls
    platforms[num_platforms++] = (Platform){300, 64, 8, 64};
    platforms[num_platforms++] = (Platform){900, 48, 8, 80};
    platforms[num_platforms++] = (Platform){1400, 56, 8, 72};
}

/* ========================================================================
 * ENEMY SETUP
 * ======================================================================== */
void setup_enemies(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++)
        enemies[i].alive = 0;

    // Place enemies
    struct { int x, y, type; } einit[] = {
        {200, 112, 0}, {350, 112, 1}, {500, 112, 0},
        {650, 112, 2}, {800, 112, 0}, {950, 112, 1},
        {1100, 112, 0}, {1300, 112, 2}, {1450, 112, 0},
        {1600, 112, 1}, {1750, 112, 0}, {1900, 112, 2},
    };

    for (int i = 0; i < 12 && i < MAX_ENEMIES; i++) {
        enemies[i].x = FIX8(einit[i].x);
        enemies[i].y = FIX8(einit[i].y);
        enemies[i].vx = FIX8(1) * (i % 2 == 0 ? 1 : -1);
        enemies[i].vy = 0;
        enemies[i].w = ENEMY_W;
        enemies[i].h = ENEMY_H;
        enemies[i].alive = 1;
        enemies[i].type = einit[i].type;
        enemies[i].anim_frame = 0;
        enemies[i].inhale_timer = 0;
    }
}

/* ========================================================================
 * KIRBY INIT
 * ======================================================================== */
void init_kirby(void)
{
    kirby.x = FIX8(50);
    kirby.y = FIX8(80);
    kirby.vx = 0;
    kirby.vy = 0;
    kirby.w = KIRBY_W;
    kirby.h = KIRBY_H;
    kirby.on_ground = 0;
    kirby.facing = 1;
    kirby.inhaling = 0;
    kirby.inhale_timer = 0;
    kirby.alive = 1;
    kirby.score = 0;
}

/* ========================================================================
 * COLLISION DETECTION
 * ======================================================================== */
int rects_overlap(fix8 ax, fix8 ay, int aw, int ah,
                  fix8 bx, fix8 by, int bw, int bh)
{
    return (ax < bx + FIX8(bw)) && (ax + FIX8(aw) > bx) &&
           (ay < by + FIX8(bh)) && (ay + FIX8(ah) > by);
}

void resolve_platform_collision(void)
{
    kirby.on_ground = 0;

    for (int p = 0; p < num_platforms; p++) {
        fix8 px = FIX8(platforms[p].x);
        fix8 py = FIX8(platforms[p].y);
        int pw = platforms[p].w;
        int ph = platforms[p].h;

        if (!rects_overlap(kirby.x, kirby.y, kirby.w, kirby.h,
                          px, py, pw, ph))
            continue;

        // Determine collision side
        fix8 kirby_cx = kirby.x + FIX8(kirby.w / 2);
        fix8 kirby_cy = kirby.y + FIX8(kirby.h / 2);
        fix8 plat_cx = px + FIX8(pw / 2);
        fix8 plat_cy = py + FIX8(ph / 2);

        fix8 overlap_x = (FIX8(kirby.w / 2) + FIX8(pw / 2)) -
                         (kirby_cx > plat_cx ? kirby_cx - plat_cx : plat_cx - kirby_cx);
        fix8 overlap_y = (FIX8(kirby.h / 2) + FIX8(ph / 2)) -
                         (kirby_cy > plat_cy ? kirby_cy - plat_cy : plat_cy - kirby_cy);

        if (overlap_x < overlap_y) {
            // Horizontal collision
            if (kirby_cx < plat_cx)
                kirby.x = px - FIX8(kirby.w);
            else
                kirby.x = px + FIX8(pw);
            kirby.vx = 0;
        } else {
            // Vertical collision
            if (kirby_cy < plat_cy) {
                kirby.y = py - FIX8(kirby.h);
                kirby.vy = 0;
                kirby.on_ground = 1;
            } else {
                kirby.y = py + FIX8(ph);
                kirby.vy = 0;
            }
        }
    }
}

/* ========================================================================
 * PARTICLE SYSTEM
 * ======================================================================== */
void spawn_particles(fix8 x, fix8 y, int count, int color)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life <= 0) {
            particles[i].x = x;
            particles[i].y = y;
            particles[i].vx = FIX8((rand() % 5) - 2);
            particles[i].vy = FIX8((rand() % 5) - 3);
            particles[i].life = 20 + (rand() % 15);
            particles[i].color = color;
            return;
        }
    }
}

void update_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life > 0) {
            particles[i].x += particles[i].vx;
            particles[i].y += particles[i].vy;
            particles[i].vy += FIX8(1) / 4;
            particles[i].life--;
        }
    }
}

/* ========================================================================
 * UPDATE KIRBY
 * ======================================================================== */
void update_kirby(void)
{
    if (!kirby.alive) return;

    u16 keys = key_poll();

    // Horizontal movement
    kirby.vx = 0;
    if (keys & KEY_LEFT) {
        kirby.vx = -MOVE_SPEED;
        kirby.facing = -1;
    }
    if (keys & KEY_RIGHT) {
        kirby.vx = MOVE_SPEED;
        kirby.facing = 1;
    }

    // Jump
    if ((keys & KEY_A) && kirby.on_ground) {
        kirby.vy = JUMP_VEL;
        kirby.on_ground = 0;
    }

    // Inhale
    if (keys & KEY_B) {
        kirby.inhaling = 1;
        kirby.inhale_timer++;
    } else {
        if (kirby.inhaling && kirby.inhale_timer > 10) {
            // Spit out!
            for (int i = 0; i < MAX_ENEMIES; i++) {
                if (enemies[i].inhale_timer > 0) {
                    enemies[i].alive = 1;
                    enemies[i].inhale_timer = 0;
                    enemies[i].x = kirby.x + FIX8(kirby.facing * 32);
                    enemies[i].y = kirby.y;
                    enemies[i].vx = FIX8(kirby.facing * 6);
                    enemies[i].vy = FIX8(-2);
                    kirby.score += 100;
                    spawn_particles(enemies[i].x, enemies[i].y, 8, 14);
                }
            }
        }
        kirby.inhaling = 0;
        kirby.inhale_timer = 0;
    }

    // Apply gravity
    kirby.vy += GRAVITY;
    if (kirby.vy > MAX_FALL_SPEED)
        kirby.vy = MAX_FALL_SPEED;

    // Move
    kirby.x += kirby.vx;
    kirby.y += kirby.vy;

    // Clamp to level bounds
    if (kirby.x < 0) kirby.x = 0;
    if (kirby.x > FIX8(MAP_W - kirby.w))
        kirby.x = FIX8(MAP_W - kirby.w);

    // Fall death
    if (kirby.y > FIX8(MAP_H)) {
        kirby.x = FIX8(50);
        kirby.y = FIX8(80);
        kirby.vx = 0;
        kirby.vy = 0;
    }

    // Platform collision
    resolve_platform_collision();

    // Check enemy inhale
    if (kirby.inhaling) {
        for (int i = 0; i < MAX_ENEMIES; i++) {
            if (!enemies[i].alive || enemies[i].inhale_timer > 0)
                continue;

            fix8 inhale_range = FIX8(48);
            fix8 dx = enemies[i].x - kirby.x;
            fix8 dy = enemies[i].y - kirby.y;

            // Check if enemy is in front of Kirby and within range
            int in_front = (kirby.facing > 0 && dx > 0 && dx < inhale_range) ||
                          (kirby.facing < 0 && dx < 0 && dx > -inhale_range);

            if (in_front && dy > -FIX8(32) && dy < FIX8(32)) {
                enemies[i].inhale_timer = 60; // frames until spit
                enemies[i].alive = 0;
                spawn_particles(enemies[i].x, enemies[i].y, 5, 1);
            }
        }
    }

    // Update inhaled enemies
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (enemies[i].inhale_timer > 0) {
            enemies[i].inhale_timer--;
            enemies[i].x = kirby.x + FIX8(kirby.w / 2) - FIX8(4);
            enemies[i].y = kirby.y + FIX8(kirby.h / 2) - FIX8(4);
        }
    }

    // Check enemy collision (damage)
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].alive || enemies[i].inhale_timer > 0)
            continue;

        if (rects_overlap(kirby.x, kirby.y, kirby.w, kirby.h,
                         enemies[i].x, enemies[i].y, enemies[i].w, enemies[i].h)) {
            // Bounce back
            kirby.vy = JUMP_VEL / 2;
            kirby.vx = FIX8(kirby.facing * -3);
            spawn_particles(kirby.x + FIX8(kirby.w/2),
                          kirby.y + FIX8(kirby.h/2), 6, 5);
        }
    }
}

/* ========================================================================
 * UPDATE ENEMIES
 * ======================================================================== */
void update_enemies(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].alive || enemies[i].inhale_timer > 0)
            continue;

        // Simple AI
        if (enemies[i].type == 0) {
            // Walker: patrol back and forth
            enemies[i].x += enemies[i].vx;

            // Reverse at edges or walls
            int on_platform = 0;
            for (int p = 0; p < num_platforms; p++) {
                fix8 px = FIX8(platforms[p].x);
                fix8 py = FIX8(platforms[p].y);
                int pw = platforms[p].w;
                int ph = platforms[p].h;

                fix8 next_x = enemies[i].x + enemies[i].vx;
                if (next_x + FIX8(enemies[i].w) > px &&
                    next_x < px + FIX8(pw) &&
                    enemies[i].y + FIX8(enemies[i].h) >= py &&
                    enemies[i].y + FIX8(enemies[i].h) <= py + FIX8(ph + 4)) {
                    on_platform = 1;
                    break;
                }
            }

            // Simple boundary check
            if (enemies[i].x < FIX8(50) || enemies[i].x > FIX8(MAP_W - 50))
                enemies[i].vx = -enemies[i].vx;
        } else {
            // Floater: bob up and down
            enemies[i].y += FIX8_INT(enemies[i].vy);
            enemies[i].vy += (game_frame % 60 < 30) ? FIX8(1)/8 : -FIX8(1)/8;
            if (enemies[i].vy > FIX8(2)) enemies[i].vy = FIX8(2);
            if (enemies[i].vy < -FIX8(2)) enemies[i].vy = -FIX8(2);
        }

        enemies[i].anim_frame++;
    }
}

/* ========================================================================
 * RENDER SPRITES
 * ======================================================================== */
void render_sprites(void)
{
    int obj_idx = 0;

    // Kirby sprite
    if (kirby.alive) {
        int sx = FIX8_INT(kirby.x) - camera_x;
        int sy = FIX8_INT(kirby.y);

        if (sx > -32 && sx < SCREEN_W + 32 && sy > -32 && sy < SCREEN_H + 32) {
            obj_set_attr(obj_idx,
                ATTR0_SQUARE | ATTR0_16COLOR | (sy << ATTR0_Y_SHIFT),
                ATTR1_SIZE_32 | ((sx & 0x1FF) << ATTR1_X_SHIFT),
                ATTR2_TILE(0) | ATTR2_PALBANK(0));
            obj_idx++;
        }
    }

    // Enemy sprites
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].alive && enemies[i].inhale_timer <= 0)
            continue;

        int sx, sy;
        if (enemies[i].inhale_timer > 0) {
            sx = FIX8_INT(enemies[i].x) - camera_x;
            sy = FIX8_INT(enemies[i].y);
        } else {
            sx = FIX8_INT(enemies[i].x) - camera_x;
            sy = FIX8_INT(enemies[i].y);
        }

        if (sx > -16 && sx < SCREEN_W + 16 && sy > -16 && sy < SCREEN_H + 16) {
            int tile = 4 + enemies[i].type * 4; // enemy tile offset
            if (enemies[i].inhale_timer > 0)
                tile = 16; // inhaled ball tile

            // Flip based on velocity direction
            u16 attr1 = ATTR1_SIZE_16 | ((sx & 0x1FF) << ATTR1_X_SHIFT);
            if (enemies[i].vx < 0 && enemies[i].inhale_timer <= 0)
                attr1 |= ATTR1_HFLIP;

            obj_set_attr(obj_idx,
                ATTR0_SQUARE | ATTR0_16COLOR | (sy << ATTR0_Y_SHIFT),
                attr1,
                ATTR2_TILE(tile) | ATTR2_PALBANK(0));
            obj_idx++;
        }
    }

    // Particle sprites (reuse enemy tiles with different palette)
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life <= 0) continue;

        int sx = FIX8_INT(particles[i].x) - camera_x;
        int sy = FIX8_INT(particles[i].y);

        if (sx > -8 && sx < SCREEN_W + 8 && sy > -8 && sy < SCREEN_H + 8) {
            obj_set_attr(obj_idx,
                ATTR0_SQUARE | ATTR0_16COLOR | (sy << ATTR0_Y_SHIFT),
                ATTR1_SIZE_8 | ((sx & 0x1FF) << ATTR1_X_SHIFT),
                ATTR2_TILE(16) | ATTR2_PALBANK(0) |
                ((particles[i].life & 0x07) << 10)); // use palette for color variation
            obj_idx++;
        }
    }

    // Hide unused sprites
    for (int i = obj_idx; i < 128; i++) {
        obj_set_attr(i, ATTR0_HIDE, 0, 0);
    }
}

/* ========================================================================
 * RENDER HUD
 * ======================================================================== */
void render_hud(void)
{
    // Use text mode overlay - we'll use simple OBJ text or bg text
    // For simplicity, draw score using background tiles as digits

    // Score display area: top-left corner
    // We'll use a simple approach: write to a separate text BG
    // But since we're using BG0 for the level, let's use sprites for HUD

    // Actually, let's use BG1 as a text overlay for the HUD
    // For now, skip complex HUD and just use the background approach
    // We can draw score digits as sprites

    int s = kirby.score;
    int digit_x = 8;
    int digit_y = 8;

    // Draw "SCORE:" text using simple colored tiles
    // This is a simplified approach - in a real game you'd use a font
}

/* ========================================================================
 * UPDATE CAMERA
 * ======================================================================== */
void update_camera(void)
{
    int target_x = FIX8_INT(kirby.x) - SCREEN_W / 2 + kirby.w / 2;

    // Smooth follow
    camera_x += (target_x - camera_x) / 8;

    // Clamp
    if (camera_x < 0) camera_x = 0;
    if (camera_x > MAP_W - SCREEN_W)
        camera_x = MAP_W - SCREEN_W;

    // Set background scroll
    REG_BG0HOFS = camera_x & 0x1FF;
    REG_BG0VOFS = 0;
}

/* ========================================================================
 * VBLANK INTERRUPT HANDLER
 * ======================================================================== */
void vblank_handler(void)
{
    // Copy OAM
    oam_copy(OAM, obj_attr_mem, 128);
}

/* ========================================================================
 * MAIN
 * ======================================================================== */
int main(void)
{
    // Initialize Tonc
    irq_init(NULL);
    irq_add(II_VBLANK, vblank_handler);

    // Set display mode: Mode 0, BG0 + OBJ, 1D mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // Set OBJ character base and size
    REG_OBJCNT = 0; // 1D mapping, 32KB

    // Initialize palettes
    init_palettes();

    // Draw sprite tiles
    draw_kirby_tiles();
    draw_enemy_tiles();
    draw_inhaled_tile();

    // Build background
    build_background();
    setup_level();
    draw_platforms();

    // Setup game objects
    init_kirby();
    setup_enemies();

    // Clear particles
    for (int i = 0; i < MAX_PARTICLES; i++)
        particles[i].life = 0;

    // Hide all sprites initially
    for (int i = 0; i < 128; i++)
        obj_set_attr(i, ATTR0_HIDE, 0, 0);

    // Main game loop
    while (1) {
        VBlankIntrWait();

        // Wait for next frame
        game_frame++;

        // Update
        update_kirby();
        update_enemies();
        update_particles();
        update_camera();

        // Render
        render_sprites();

        // Simple score display using background
        // Draw score as text in top-left using BG tiles
        // (Simplified - just show a colored bar)
    }

    return 0;
}
