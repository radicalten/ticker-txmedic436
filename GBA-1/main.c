/*
 * GBA DOOM-like FPS Engine
 * Uses Tonc library
 * Compile with devkitARM: make
 * 
 * Controls:
 *   D-Pad Up/Down: Move forward/backward
 *   D-Pad Left/Right: Strafe
 *   L/R Buttons: Rotate
 *   A Button: Shoot
 *   B Button: Run
 *   Start: Toggle map
 */

#include <tonc.h>
#include <string.h>

//============================================================================
// CONFIGURATION
//============================================================================

#define SCREEN_W        240
#define SCREEN_H        160
#define HALF_H          80

#define MAP_W           24
#define MAP_H           24

#define TEX_W           64
#define TEX_H           64

#define MAX_ENEMIES     8
#define MAX_PROJECTILES 4

#define FIX_SHIFT       8
#define FIX_HALF        (FIX_ONE >> 1)

#define TO_FIX(x)       ((x) << FIX_SHIFT)
#define FROM_FIX(x)     ((x) >> FIX_SHIFT)
#define FIX_MUL(a,b)    (((a) * (b)) >> FIX_SHIFT)
#define FIX_DIV(a,b)    (((a) << FIX_SHIFT) / (b))

#define RGB15_C(r,g,b)  ((r) | ((g) << 5) | ((b) << 10))

//============================================================================
// TYPES
//============================================================================

typedef struct {
    s32 x, y;           // Fixed point position (8.8)
    s32 dir_x, dir_y;   // Direction vector
    s32 plane_x, plane_y; // Camera plane
    s16 angle;          // 0-511 (using 512 entries for trig tables)
    s16 health;
    s16 ammo;
    u8 weapon;
    u8 shooting;
} Player;

typedef struct {
    s32 x, y;
    s16 health;
    u8 type;
    u8 active;
    u8 state;           // 0=idle, 1=chase, 2=attack, 3=hurt, 4=dead
    u8 anim_frame;
    s16 timer;
} Enemy;

typedef struct {
    s32 x, y;
    s32 dx, dy;
    u8 active;
    u8 owner;           // 0=player, 1=enemy
} Projectile;

//============================================================================
// GLOBAL DATA
//============================================================================

// Video page
static u16* vid_page_back;

// Player
static Player player;

// Enemies
static Enemy enemies[MAX_ENEMIES];
static int num_enemies = 0;

// Projectiles
static Projectile projectiles[MAX_PROJECTILES];

// Z-buffer for sprite rendering
static u16 z_buffer[SCREEN_W];

// Trig lookup tables (512 entries, fixed point 8.8)
static s16 cos_lut[512];

// Game state
static u8 show_map = 0;
static u8 game_frame = 0;
static s16 flash_timer = 0;
static s16 shoot_cooldown = 0;
static u16 score = 0;

// World map (0=empty, 1-5=wall types, 9=door)
static const u8 world_map[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,2,2,2,2,2,0,0,0,0,0,3,3,3,3,3,0,0,0,0,1},
    {1,0,0,0,2,0,0,0,2,0,0,0,0,0,3,0,0,0,3,0,0,0,0,1},
    {1,0,0,0,2,0,0,0,2,0,0,0,0,0,3,0,0,0,3,0,0,0,0,1},
    {1,0,0,0,2,0,0,0,2,0,0,0,0,0,3,0,0,0,3,0,0,0,0,1},
    {1,0,0,0,2,2,0,2,2,0,0,0,0,0,3,3,0,3,3,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,4,4,4,4,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,4,0,0,4,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,4,0,0,4,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,4,4,4,4,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,5,5,5,5,5,0,0,0,0,0,0,0,0,5,5,5,5,5,0,1},
    {1,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,1},
    {1,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,1},
    {1,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,1},
    {1,0,0,0,5,5,5,5,5,0,0,0,0,0,0,0,0,5,5,5,5,5,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

//============================================================================
// TEXTURES (8x8 scaled up to 64x64 procedurally)
//============================================================================

// Wall texture patterns (8x8 tiles that get scaled)
static const u8 tex_patterns[5][8] = {
    // Pattern 0: Brick
    {0b11111111, 0b10001000, 0b10001000, 0b11111111,
     0b00100010, 0b00100010, 0b11111111, 0b10001000},
    // Pattern 1: Stone
    {0b11011011, 0b10010010, 0b11011011, 0b01101101,
     0b11011011, 0b10110110, 0b11011011, 0b01001001},
    // Pattern 2: Metal
    {0b11111111, 0b10000001, 0b10111101, 0b10100101,
     0b10100101, 0b10111101, 0b10000001, 0b11111111},
    // Pattern 3: Tech
    {0b11111111, 0b10000001, 0b10111101, 0b10110101,
     0b10101101, 0b10111101, 0b10000001, 0b11111111},
    // Pattern 4: Wood
    {0b10101010, 0b10101010, 0b11111111, 0b10101010,
     0b10101010, 0b10101010, 0b11111111, 0b10101010}
};

// Base colors for each wall type (15-bit RGB)
static const u16 wall_colors[5][2] = {
    {RGB15_C(20, 8, 4), RGB15_C(14, 5, 2)},    // Brick: red-brown
    {RGB15_C(18, 18, 18), RGB15_C(10, 10, 10)}, // Stone: gray
    {RGB15_C(12, 12, 20), RGB15_C(6, 6, 14)},   // Metal: blue-gray
    {RGB15_C(8, 20, 8), RGB15_C(4, 12, 4)},     // Tech: green
    {RGB15_C(20, 14, 6), RGB15_C(14, 10, 4)}    // Wood: brown
};

//============================================================================
// PALETTE SETUP
//============================================================================

static void init_palette(void) {
    // Color 0: Transparent/black
    pal_bg_mem[0] = RGB15(0, 0, 0);
    
    // Colors 1-32: Wall shading (8 shades per wall type, 4 wall types = 32)
    int idx = 1;
    for (int wall = 0; wall < 5; wall++) {
        // Light side (4 shades)
        for (int shade = 0; shade < 4; shade++) {
            int r = (wall_colors[wall][0] & 0x1F) * (4 - shade) / 4;
            int g = ((wall_colors[wall][0] >> 5) & 0x1F) * (4 - shade) / 4;
            int b = ((wall_colors[wall][0] >> 10) & 0x1F) * (4 - shade) / 4;
            pal_bg_mem[idx++] = RGB15(r, g, b);
        }
        // Dark side (4 shades)
        for (int shade = 0; shade < 4; shade++) {
            int r = (wall_colors[wall][1] & 0x1F) * (4 - shade) / 4;
            int g = ((wall_colors[wall][1] >> 5) & 0x1F) * (4 - shade) / 4;
            int b = ((wall_colors[wall][1] >> 10) & 0x1F) * (4 - shade) / 4;
            pal_bg_mem[idx++] = RGB15(r, g, b);
        }
    }
    
    // Colors for ceiling and floor gradients (41-56)
    for (int i = 0; i < 8; i++) {
        // Ceiling (dark blue gradient)
        pal_bg_mem[41 + i] = RGB15(2 + i/2, 2 + i/2, 8 + i);
        // Floor (gray gradient)
        pal_bg_mem[49 + i] = RGB15(4 + i, 4 + i, 4 + i);
    }
    
    // UI and sprite colors
    pal_bg_mem[60] = RGB15(31, 0, 0);      // Red (blood, health)
    pal_bg_mem[61] = RGB15(31, 31, 0);     // Yellow (muzzle flash)
    pal_bg_mem[62] = RGB15(0, 31, 0);      // Green (enemy)
    pal_bg_mem[63] = RGB15(31, 31, 31);    // White
    pal_bg_mem[64] = RGB15(20, 20, 20);    // Light gray (gun)
    pal_bg_mem[65] = RGB15(10, 10, 10);    // Dark gray
    pal_bg_mem[66] = RGB15(31, 16, 0);     // Orange
    pal_bg_mem[67] = RGB15(16, 0, 0);      // Dark red
}

//============================================================================
// TRIG TABLE INITIALIZATION
//============================================================================

// Fixed point sine approximation using parabolic curves
static void init_trig_tables(void) {
    for (int i = 0; i < 512; i++) {
        // Map 0-511 to 0-2π, result in 8.8 fixed point
        // Using improved sine approximation
        int x = i;
        int sign = 1;
        
        // Reduce to first quadrant
        if (x >= 256) {
            x -= 256;
            sign = -sign;
        }
        if (x >= 128) {
            x = 256 - x;
        }
        
        // Quadratic approximation: sin(x) ≈ (x/128) * (256 - x) / 128
        // Scaled to 8.8 fixed point
        s32 val = (x * (128 - x) * 512) / (128 * 128);
        
        sin_lut[i] = (s16)(val * sign);
        cos_lut[i] = sin_lut[(i + 128) & 511];
    }
}

INLINE s32 fix_sin(int angle) {
    return sin_lut[angle & 511];
}

INLINE s32 fix_cos(int angle) {
    return cos_lut[angle & 511];
}

//============================================================================
// PIXEL DRAWING HELPERS (Mode 4)
//============================================================================

INLINE void plot_pixel(int x, int y, u8 color) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    
    u32 addr = y * SCREEN_W + x;
    u16* dest = &vid_page_back[addr >> 1];
    
    if (addr & 1) {
        *dest = (*dest & 0x00FF) | (color << 8);
    } else {
        *dest = (*dest & 0xFF00) | color;
    }
}

// Fast vertical line with single color
static void vline(int x, int y1, int y2, u8 color) {
    if (x < 0 || x >= SCREEN_W) return;
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (y1 < 0) y1 = 0;
    if (y2 >= SCREEN_H) y2 = SCREEN_H - 1;
    
    for (int y = y1; y <= y2; y++) {
        plot_pixel(x, y, color);
    }
}

// Fast horizontal line
static void hline(int x1, int x2, int y, u8 color) {
    if (y < 0 || y >= SCREEN_H) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (x1 < 0) x1 = 0;
    if (x2 >= SCREEN_W) x2 = SCREEN_W - 1;
    
    u32 addr = y * SCREEN_W + x1;
    
    // Align to 16-bit boundary
    if ((x1 & 1) && x1 <= x2) {
        plot_pixel(x1++, y, color);
        addr++;
    }
    
    // Write 16-bit pairs
    u16 color16 = color | (color << 8);
    u16* dest = &vid_page_back[addr >> 1];
    int pairs = (x2 - x1 + 1) >> 1;
    
    while (pairs--) {
        *dest++ = color16;
    }
    
    // Handle remaining pixel
    if (((x2 - x1 + 1) & 1) && x1 <= x2) {
        plot_pixel(x2, y, color);
    }
}

// Fill rectangle
static void fill_rect(int x, int y, int w, int h, u8 color) {
    for (int i = 0; i < h; i++) {
        hline(x, x + w - 1, y + i, color);
    }
}

//============================================================================
// DRAW FLOOR AND CEILING
//============================================================================

static void draw_floor_ceiling(void) {
    // Ceiling - top half, dark gradient
    for (int y = 0; y < HALF_H; y++) {
        int shade = y * 8 / HALF_H;
        u8 color = 41 + (7 - shade);
        hline(0, SCREEN_W - 1, y, color);
    }
    
    // Floor - bottom half, gradient from distance
    for (int y = HALF_H; y < SCREEN_H; y++) {
        int dist = y - HALF_H;
        int shade = dist * 8 / HALF_H;
        if (shade > 7) shade = 7;
        u8 color = 49 + shade;
        hline(0, SCREEN_W - 1, y, color);
    }
}

//============================================================================
// RAYCASTING ENGINE
//============================================================================

static void cast_rays(void) {
    s32 pos_x = player.x;
    s32 pos_y = player.y;
    
    for (int x = 0; x < SCREEN_W; x++) {
        // Calculate ray direction
        // Camera x from -1 to 1 (fixed point 8.8)
        s32 camera_x = ((x << 9) / SCREEN_W) - 256; // -256 to 256
        
        s32 ray_dir_x = player.dir_x + FIX_MUL(player.plane_x, camera_x);
        s32 ray_dir_y = player.dir_y + FIX_MUL(player.plane_y, camera_x);
        
        // Map position
        int map_x = FROM_FIX(pos_x);
        int map_y = FROM_FIX(pos_y);
        
        // Length from current position to next x or y side
        s32 delta_dist_x, delta_dist_y;
        
        if (ray_dir_x == 0) {
            delta_dist_x = 0x7FFF;
        } else {
            delta_dist_x = ABS(FIX_DIV(FIX_ONE, ray_dir_x));
            if (delta_dist_x > 0x7FFF) delta_dist_x = 0x7FFF;
        }
        
        if (ray_dir_y == 0) {
            delta_dist_y = 0x7FFF;
        } else {
            delta_dist_y = ABS(FIX_DIV(FIX_ONE, ray_dir_y));
            if (delta_dist_y > 0x7FFF) delta_dist_y = 0x7FFF;
        }
        
        s32 side_dist_x, side_dist_y;
        int step_x, step_y;
        
        // Calculate step direction and initial side distance
        if (ray_dir_x < 0) {
            step_x = -1;
            side_dist_x = FIX_MUL(pos_x - TO_FIX(map_x), delta_dist_x);
        } else {
            step_x = 1;
            side_dist_x = FIX_MUL(TO_FIX(map_x + 1) - pos_x, delta_dist_x);
        }
        
        if (ray_dir_y < 0) {
            step_y = -1;
            side_dist_y = FIX_MUL(pos_y - TO_FIX(map_y), delta_dist_y);
        } else {
            step_y = 1;
            side_dist_y = FIX_MUL(TO_FIX(map_y + 1) - pos_y, delta_dist_y);
        }
        
        // DDA algorithm
        int hit = 0;
        int side = 0;
        int wall_type = 0;
        
        while (!hit) {
            if (side_dist_x < side_dist_y) {
                side_dist_x += delta_dist_x;
                map_x += step_x;
                side = 0;
            } else {
                side_dist_y += delta_dist_y;
                map_y += step_y;
                side = 1;
            }
            
            // Bounds check
            if (map_x < 0 || map_x >= MAP_W || map_y < 0 || map_y >= MAP_H) {
                hit = 1;
                wall_type = 1;
            } else if (world_map[map_y][map_x] > 0) {
                hit = 1;
                wall_type = world_map[map_y][map_x];
            }
        }
        
        // Calculate perpendicular wall distance (avoid fisheye)
        s32 perp_wall_dist;
        
        if (side == 0) {
            perp_wall_dist = side_dist_x - delta_dist_x;
        } else {
            perp_wall_dist = side_dist_y - delta_dist_y;
        }
        
        if (perp_wall_dist < 1) perp_wall_dist = 1;
        
        // Store in z-buffer for sprite rendering
        z_buffer[x] = (u16)(perp_wall_dist > 0xFFFF ? 0xFFFF : perp_wall_dist);
        
        // Calculate wall height
        int line_height = (SCREEN_H << FIX_SHIFT) / perp_wall_dist;
        if (line_height > SCREEN_H * 4) line_height = SCREEN_H * 4;
        
        int draw_start = HALF_H - line_height / 2;
        int draw_end = HALF_H + line_height / 2;
        
        // Calculate where on the wall the ray hit (for texturing)
        s32 wall_x;
        if (side == 0) {
            wall_x = pos_y + FIX_MUL(perp_wall_dist, ray_dir_y);
        } else {
            wall_x = pos_x + FIX_MUL(perp_wall_dist, ray_dir_x);
        }
        wall_x = wall_x & (FIX_ONE - 1); // Get fractional part
        
        // Texture X coordinate (0-7 for our 8x8 pattern)
        int tex_x = (wall_x * 8) >> FIX_SHIFT;
        if (tex_x > 7) tex_x = 7;
        
        // Mirror texture on certain sides for continuity
        if ((side == 0 && ray_dir_x > 0) || (side == 1 && ray_dir_y < 0)) {
            tex_x = 7 - tex_x;
        }
        
        // Calculate shade based on distance
        int shade = perp_wall_dist >> 7; // Distance-based shading
        if (shade > 3) shade = 3;
        
        // Color index calculation
        // Each wall type has 8 colors: 4 light + 4 dark
        int wall_idx = (wall_type - 1) % 5;
        int base_color = 1 + wall_idx * 8;
        
        if (side == 1) {
            base_color += 4; // Use darker colors for Y-sides
        }
        base_color += shade;
        
        // Clamp drawing coordinates
        int y_start = draw_start;
        int y_end = draw_end;
        if (y_start < 0) y_start = 0;
        if (y_end >= SCREEN_H) y_end = SCREEN_H - 1;
        
        // Draw the textured wall column
        for (int y = y_start; y <= y_end; y++) {
            // Calculate texture Y coordinate
            int tex_y = ((y - draw_start) * 8) / line_height;
            if (tex_y < 0) tex_y = 0;
            if (tex_y > 7) tex_y = 7;
            
            // Get pattern bit
            u8 pattern_row = tex_patterns[wall_idx][tex_y];
            int pattern_bit = (pattern_row >> (7 - tex_x)) & 1;
            
            // Vary color slightly based on pattern
            u8 final_color = base_color;
            if (pattern_bit == 0) {
                final_color += (shade < 3) ? 1 : 0;
            }
            
            plot_pixel(x, y, final_color);
        }
    }
}

//============================================================================
// ENEMY SPRITES
//============================================================================

static void spawn_enemies(void) {
    // Spawn some enemies at fixed positions
    int spawn_points[][2] = {
        {6, 6}, {17, 6}, {6, 17}, {17, 17}, {12, 12}
    };
    
    num_enemies = 5;
    for (int i = 0; i < num_enemies; i++) {
        enemies[i].x = TO_FIX(spawn_points[i][0]) + FIX_HALF;
        enemies[i].y = TO_FIX(spawn_points[i][1]) + FIX_HALF;
        enemies[i].health = 3;
        enemies[i].type = 0;
        enemies[i].active = 1;
        enemies[i].state = 0;
        enemies[i].anim_frame = 0;
        enemies[i].timer = 0;
    }
}

static void update_enemies(void) {
    for (int i = 0; i < num_enemies; i++) {
        if (!enemies[i].active) continue;
        
        Enemy* e = &enemies[i];
        
        // Calculate distance to player
        s32 dx = player.x - e->x;
        s32 dy = player.y - e->y;
        s32 dist_sq = FIX_MUL(dx, dx) + FIX_MUL(dy, dy);
        
        e->timer++;
        
        if (e->state == 4) {
            // Dead - just animate death
            if (e->timer > 60) {
                e->active = 0;
            }
            continue;
        }
        
        if (e->state == 3) {
            // Hurt - brief stun
            if (e->timer > 15) {
                e->state = 1;
                e->timer = 0;
            }
            continue;
        }
        
        // Check if player is close enough to trigger chase
        if (dist_sq < TO_FIX(64)) { // Within 8 units
            e->state = 1; // Chase
        }
        
        if (e->state == 1) {
            // Move towards player
            s32 move_speed = FIX_ONE / 32;
            
            // Normalize direction
            s32 len = 1;
            if (dist_sq > 0) {
                // Approximate distance (not perfect but fast)
                s32 adx = ABS(dx);
                s32 ady = ABS(dy);
                len = (adx > ady) ? adx + (ady >> 1) : ady + (adx >> 1);
                if (len == 0) len = 1;
            }
            
            s32 move_x = (dx * move_speed) / len;
            s32 move_y = (dy * move_speed) / len;
            
            // Check collision
            s32 new_x = e->x + move_x;
            s32 new_y = e->y + move_y;
            
            int map_x = FROM_FIX(new_x);
            int map_y = FROM_FIX(new_y);
            
            if (map_x >= 0 && map_x < MAP_W && map_y >= 0 && map_y < MAP_H) {
                if (world_map[FROM_FIX(e->y)][map_x] == 0) {
                    e->x = new_x;
                }
                if (world_map[map_y][FROM_FIX(e->x)] == 0) {
                    e->y = new_y;
                }
            }
            
            // Attack if very close
            if (dist_sq < TO_FIX(2)) {
                if (e->timer > 30) {
                    player.health -= 5;
                    e->timer = 0;
                    flash_timer = 10;
                }
            }
        }
        
        // Animation
        if ((game_frame & 15) == 0) {
            e->anim_frame = (e->anim_frame + 1) & 1;
        }
    }
}

static void draw_enemies(void) {
    // Sort enemies by distance (simple bubble sort for few enemies)
    int order[MAX_ENEMIES];
    s32 distances[MAX_ENEMIES];
    
    for (int i = 0; i < num_enemies; i++) {
        order[i] = i;
        s32 dx = enemies[i].x - player.x;
        s32 dy = enemies[i].y - player.y;
        distances[i] = FIX_MUL(dx, dx) + FIX_MUL(dy, dy);
    }
    
    // Sort far to near
    for (int i = 0; i < num_enemies - 1; i++) {
        for (int j = i + 1; j < num_enemies; j++) {
            if (distances[order[i]] < distances[order[j]]) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    
    // Draw each enemy
    for (int n = 0; n < num_enemies; n++) {
        int i = order[n];
        Enemy* e = &enemies[i];
        if (!e->active) continue;
        
        // Transform sprite position relative to camera
        s32 sprite_x = e->x - player.x;
        s32 sprite_y = e->y - player.y;
        
        // Inverse camera matrix multiplication
        s32 inv_det = FIX_DIV(FIX_ONE, 
            FIX_MUL(player.plane_x, player.dir_y) - 
            FIX_MUL(player.dir_x, player.plane_y));
        
        s32 transform_x = FIX_MUL(inv_det, 
            FIX_MUL(player.dir_y, sprite_x) - 
            FIX_MUL(player.dir_x, sprite_y));
        s32 transform_y = FIX_MUL(inv_det,
            -FIX_MUL(player.plane_y, sprite_x) + 
            FIX_MUL(player.plane_x, sprite_y));
        
        // Check if in front of camera
        if (transform_y <= 0) continue;
        
        // Screen X position
        int sprite_screen_x = (SCREEN_W / 2) * (FIX_ONE + FIX_DIV(transform_x, transform_y)) >> FIX_SHIFT;
        
        // Calculate sprite height and width
        int sprite_height = ABS((SCREEN_H << FIX_SHIFT) / transform_y);
        if (sprite_height > SCREEN_H * 2) sprite_height = SCREEN_H * 2;
        
        int sprite_width = sprite_height; // Square sprites
        
        int draw_start_y = HALF_H - sprite_height / 2;
        int draw_end_y = HALF_H + sprite_height / 2;
        int draw_start_x = sprite_screen_x - sprite_width / 2;
        int draw_end_x = sprite_screen_x + sprite_width / 2;
        
        // Clamp
        if (draw_start_y < 0) draw_start_y = 0;
        if (draw_end_y >= SCREEN_H) draw_end_y = SCREEN_H - 1;
        if (draw_start_x < 0) draw_start_x = 0;
        if (draw_end_x >= SCREEN_W) draw_end_x = SCREEN_W - 1;
        
        // Choose color based on state
        u8 enemy_color = 62; // Green
        if (e->state == 3) enemy_color = 60; // Red when hurt
        if (e->state == 4) enemy_color = 67; // Dark red when dead
        
        // Draw the sprite (simple column-based with z-buffer check)
        for (int stripe = draw_start_x; stripe < draw_end_x; stripe++) {
            if (stripe < 0 || stripe >= SCREEN_W) continue;
            
            // Z-buffer check
            u16 sprite_z = (u16)(transform_y > 0xFFFF ? 0xFFFF : transform_y);
            if (sprite_z >= z_buffer[stripe]) continue;
            
            // Calculate texture X
            int tex_x = ((stripe - (sprite_screen_x - sprite_width / 2)) * 8) / sprite_width;
            if (tex_x < 0 || tex_x > 7) continue;
            
            for (int y = draw_start_y; y < draw_end_y; y++) {
                int tex_y = ((y - (HALF_H - sprite_height / 2)) * 8) / sprite_height;
                if (tex_y < 0 || tex_y > 7) continue;
                
                // Simple sprite pattern - demon/imp shape
                int draw = 0;
                
                if (e->state == 4) {
                    // Dead - flat
                    if (tex_y >= 6) draw = 1;
                } else {
                    // Body
                    if (tex_y >= 2 && tex_y <= 6 && tex_x >= 2 && tex_x <= 5) draw = 1;
                    // Head
                    if (tex_y >= 0 && tex_y <= 2 && tex_x >= 3 && tex_x <= 4) draw = 1;
                    // Arms (animated)
                    if (tex_y >= 3 && tex_y <= 4) {
                        if (e->anim_frame == 0) {
                            if (tex_x == 1 || tex_x == 6) draw = 1;
                        } else {
                            if (tex_x == 0 || tex_x == 7) draw = 1;
                        }
                    }
                    // Legs
                    if (tex_y >= 6 && tex_y <= 7) {
                        if (e->anim_frame == 0) {
                            if (tex_x == 2 || tex_x == 5) draw = 1;
                        } else {
                            if (tex_x == 3 || tex_x == 4) draw = 1;
                        }
                    }
                }
                
                if (draw) {
                    plot_pixel(stripe, y, enemy_color);
                }
            }
        }
    }
}

//============================================================================
// WEAPON AND UI
//============================================================================

static void draw_weapon(void) {
    int gun_x = SCREEN_W / 2 - 20;
    int gun_y = SCREEN_H - 45;
    
    // Bobbing effect when moving
    if (key_is_down(KEY_UP) || key_is_down(KEY_DOWN) || 
        key_is_down(KEY_LEFT) || key_is_down(KEY_RIGHT)) {
        gun_y += ((game_frame >> 2) & 1) ? 2 : -2;
        gun_x += ((game_frame >> 2) & 2) ? 1 : -1;
    }
    
    // Recoil when shooting
    if (shoot_cooldown > 10) {
        gun_y += 5;
    }
    
    u8 gun_color = 64;
    
    // Gun barrel
    fill_rect(gun_x + 17, gun_y, 6, 25, gun_color);
    fill_rect(gun_x + 15, gun_y + 5, 10, 5, gun_color);
    
    // Gun body
    fill_rect(gun_x + 10, gun_y + 20, 20, 15, gun_color);
    fill_rect(gun_x + 12, gun_y + 35, 16, 12, 65);
    
    // Grip
    fill_rect(gun_x + 15, gun_y + 40, 10, 15, 65);
    
    // Muzzle flash when shooting
    if (shoot_cooldown > 10) {
        fill_rect(gun_x + 15, gun_y - 15, 10, 15, 61); // Yellow flash
        fill_rect(gun_x + 12, gun_y - 10, 16, 10, 66); // Orange core
    }
}

static void draw_hud(void) {
    // Health bar background
    fill_rect(5, SCREEN_H - 15, 52, 10, 65);
    
    // Health bar
    int health_width = (player.health * 50) / 100;
    if (health_width > 0) {
        fill_rect(6, SCREEN_H - 14, health_width, 8, 60);
    }
    
    // Ammo counter (simple representation)
    for (int i = 0; i < player.ammo && i < 10; i++) {
        fill_rect(SCREEN_W - 10 - i * 6, SCREEN_H - 14, 4, 8, 61);
    }
    
    // Damage flash
    if (flash_timer > 0) {
        // Red tint on edges
        for (int i = 0; i < 5; i++) {
            vline(i, 0, SCREEN_H - 1, 60);
            vline(SCREEN_W - 1 - i, 0, SCREEN_H - 1, 60);
        }
    }
}

static void draw_minimap(void) {
    if (!show_map) return;
    
    int map_x = SCREEN_W - 55;
    int map_y = 5;
    int cell = 2;
    
    // Background
    fill_rect(map_x - 2, map_y - 2, MAP_W * cell + 4, MAP_H * cell + 4, 65);
    
    // Draw walls
    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            if (world_map[y][x] > 0) {
                fill_rect(map_x + x * cell, map_y + y * cell, cell, cell, 63);
            }
        }
    }
    
    // Draw enemies
    for (int i = 0; i < num_enemies; i++) {
        if (!enemies[i].active) continue;
        int ex = map_x + (FROM_FIX(enemies[i].x) * cell);
        int ey = map_y + (FROM_FIX(enemies[i].y) * cell);
        plot_pixel(ex, ey, 62);
    }
    
    // Draw player
    int px = map_x + (FROM_FIX(player.x) * cell);
    int py = map_y + (FROM_FIX(player.y) * cell);
    plot_pixel(px, py, 60);
    
    // Direction indicator
    int dx = fix_cos(player.angle) >> 6;
    int dy = fix_sin(player.angle) >> 6;
    plot_pixel(px + dx, py + dy, 61);
}

//============================================================================
// INPUT AND GAME LOGIC
//============================================================================

static void handle_input(void) {
    key_poll();
    
    // Movement speed
    s32 move_speed = FIX_ONE / 12;
    s32 rot_speed = 8;
    
    // Run modifier
    if (key_is_down(KEY_B)) {
        move_speed = FIX_ONE / 8;
    }
    
    // Rotation
    if (key_is_down(KEY_L)) {
        player.angle -= rot_speed;
        if (player.angle < 0) player.angle += 512;
        
        // Update direction vectors
        s32 old_dir_x = player.dir_x;
        player.dir_x = FIX_MUL(player.dir_x, fix_cos(-rot_speed)) - 
                       FIX_MUL(player.dir_y, fix_sin(-rot_speed));
        player.dir_y = FIX_MUL(old_dir_x, fix_sin(-rot_speed)) + 
                       FIX_MUL(player.dir_y, fix_cos(-rot_speed));
        
        s32 old_plane_x = player.plane_x;
        player.plane_x = FIX_MUL(player.plane_x, fix_cos(-rot_speed)) - 
                         FIX_MUL(player.plane_y, fix_sin(-rot_speed));
        player.plane_y = FIX_MUL(old_plane_x, fix_sin(-rot_speed)) + 
                         FIX_MUL(player.plane_y, fix_cos(-rot_speed));
    }
    
    if (key_is_down(KEY_R)) {
        player.angle += rot_speed;
        if (player.angle >= 512) player.angle -= 512;
        
        s32 old_dir_x = player.dir_x;
        player.dir_x = FIX_MUL(player.dir_x, fix_cos(rot_speed)) - 
                       FIX_MUL(player.dir_y, fix_sin(rot_speed));
        player.dir_y = FIX_MUL(old_dir_x, fix_sin(rot_speed)) + 
                       FIX_MUL(player.dir_y, fix_cos(rot_speed));
        
        s32 old_plane_x = player.plane_x;
        player.plane_x = FIX_MUL(player.plane_x, fix_cos(rot_speed)) - 
                         FIX_MUL(player.plane_y, fix_sin(rot_speed));
        player.plane_y = FIX_MUL(old_plane_x, fix_sin(rot_speed)) + 
                         FIX_MUL(player.plane_y, fix_cos(rot_speed));
    }
    
    // Forward/backward movement
    if (key_is_down(KEY_UP)) {
        s32 new_x = player.x + FIX_MUL(player.dir_x, move_speed);
        s32 new_y = player.y + FIX_MUL(player.dir_y, move_speed);
        
        // Collision with margin
        int margin = FIX_ONE / 4;
        int map_x = FROM_FIX(new_x + (player.dir_x > 0 ? margin : -margin));
        int map_y = FROM_FIX(new_y + (player.dir_y > 0 ? margin : -margin));
        
        if (map_x >= 0 && map_x < MAP_W && world_map[FROM_FIX(player.y)][map_x] == 0) {
            player.x = new_x;
        }
        if (map_y >= 0 && map_y < MAP_H && world_map[map_y][FROM_FIX(player.x)] == 0) {
            player.y = new_y;
        }
    }
    
    if (key_is_down(KEY_DOWN)) {
        s32 new_x = player.x - FIX_MUL(player.dir_x, move_speed);
        s32 new_y = player.y - FIX_MUL(player.dir_y, move_speed);
        
        int margin = FIX_ONE / 4;
        int map_x = FROM_FIX(new_x + (player.dir_x < 0 ? margin : -margin));
        int map_y = FROM_FIX(new_y + (player.dir_y < 0 ? margin : -margin));
        
        if (map_x >= 0 && map_x < MAP_W && world_map[FROM_FIX(player.y)][map_x] == 0) {
            player.x = new_x;
        }
        if (map_y >= 0 && map_y < MAP_H && world_map[map_y][FROM_FIX(player.x)] == 0) {
            player.y = new_y;
        }
    }
    
    // Strafing
    if (key_is_down(KEY_LEFT)) {
        s32 strafe_x = player.dir_y;
        s32 strafe_y = -player.dir_x;
        
        s32 new_x = player.x - FIX_MUL(strafe_x, move_speed);
        s32 new_y = player.y - FIX_MUL(strafe_y, move_speed);
        
        if (world_map[FROM_FIX(player.y)][FROM_FIX(new_x)] == 0) {
            player.x = new_x;
        }
        if (world_map[FROM_FIX(new_y)][FROM_FIX(player.x)] == 0) {
            player.y = new_y;
        }
    }
    
    if (key_is_down(KEY_RIGHT)) {
        s32 strafe_x = player.dir_y;
        s32 strafe_y = -player.dir_x;
        
        s32 new_x = player.x + FIX_MUL(strafe_x, move_speed);
        s32 new_y = player.y + FIX_MUL(strafe_y, move_speed);
        
        if (world_map[FROM_FIX(player.y)][FROM_FIX(new_x)] == 0) {
            player.x = new_x;
        }
        if (world_map[FROM_FIX(new_y)][FROM_FIX(player.x)] == 0) {
            player.y = new_y;
        }
    }
    
    // Shooting
    if (key_is_down(KEY_A) && shoot_cooldown == 0 && player.ammo > 0) {
        player.shooting = 1;
        shoot_cooldown = 15;
        player.ammo--;
        
        // Hit detection - check if any enemy is in crosshair
        for (int i = 0; i < num_enemies; i++) {
            if (!enemies[i].active || enemies[i].state == 4) continue;
            
            Enemy* e = &enemies[i];
            
            // Check if enemy is roughly in front of player
            s32 dx = e->x - player.x;
            s32 dy = e->y - player.y;
            
            // Dot product with direction
            s32 dot = FIX_MUL(dx, player.dir_x) + FIX_MUL(dy, player.dir_y);
            if (dot <= 0) continue; // Behind player
            
            // Cross product for left/right
            s32 cross = FIX_MUL(dx, player.dir_y) - FIX_MUL(dy, player.dir_x);
            
            // Check if enemy is within aiming cone
            if (ABS(cross) < dot / 4) {
                // Hit!
                e->health--;
                e->state = 3; // Hurt
                e->timer = 0;
                
                if (e->health <= 0) {
                    e->state = 4; // Dead
                    e->timer = 0;
                    score += 100;
                }
                break; // Only hit one enemy per shot
            }
        }
    }
    
    // Toggle map
    if (key_hit(KEY_START)) {
        show_map = !show_map;
    }
}

static void update_timers(void) {
    if (shoot_cooldown > 0) shoot_cooldown--;
    if (flash_timer > 0) flash_timer--;
    
    // Ammo regeneration (for gameplay)
    if ((game_frame & 63) == 0 && player.ammo < 30) {
        player.ammo++;
    }
    
    // Health regeneration (slow)
    if ((game_frame & 127) == 0 && player.health < 100 && player.health > 0) {
        player.health++;
    }
}

//============================================================================
// INITIALIZATION
//============================================================================

static void init_player(void) {
    player.x = TO_FIX(2) + FIX_HALF;
    player.y = TO_FIX(2) + FIX_HALF;
    player.angle = 0;
    
    // Direction vector (looking right initially)
    player.dir_x = FIX_ONE;
    player.dir_y = 0;
    
    // Camera plane (perpendicular to direction, FOV ~66 degrees)
    player.plane_x = 0;
    player.plane_y = FIX_ONE * 2 / 3; // 0.66 in fixed point
    
    player.health = 100;
    player.ammo = 30;
    player.weapon = 0;
    player.shooting = 0;
}

static void init_game(void) {
    init_palette();
    init_trig_tables();
    init_player();
    spawn_enemies();
    
    show_map = 0;
    game_frame = 0;
    flash_timer = 0;
    shoot_cooldown = 0;
    score = 0;
}

//============================================================================
// MAIN
//============================================================================

int main(void) {
    // Initialize interrupt handler
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    // Set up Mode 4 (8-bit bitmap, page flipping)
    REG_DISPCNT = DCNT_MODE4 | DCNT_BG2;
    
    // Initialize game
    init_game();
    
    // Main loop
    while (1) {
        // Get the back buffer for drawing
        vid_page_back = (u16*)vid_mem_back;
        
        // Clear/draw floor and ceiling
        draw_floor_ceiling();
        
        // Raycasting (walls)
        cast_rays();
        
        // Draw sprites (enemies)
        draw_enemies();
        
        // Draw weapon overlay
        draw_weapon();
        
        // Draw HUD
        draw_hud();
        
        // Draw minimap if enabled
        draw_minimap();
        
        // Wait for VBlank and flip pages
        VBlankIntrWait();
        vid_flip();
        
        // Handle input and update game state
        handle_input();
        update_enemies();
        update_timers();
        
        game_frame++;
        
        // Check game over condition
        if (player.health <= 0) {
            // Simple game over - reset after delay
            for (int i = 0; i < 120; i++) {
                VBlankIntrWait();
            }
            init_game();
        }
    }
    
    return 0;
}
