// ============================================================
// Kirby-like GBA Side Scroller using Tonc
// Compile with devkitPro: make
// ============================================================

#include <tonc.h>
#include <string.h>

// ============================================================
// CONSTANTS
// ============================================================

#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   160

#define TILE_SIZE       8
#define MAP_WIDTH       64
#define MAP_HEIGHT      32

#define MAX_ENEMIES     8
#define MAX_STARS       6

#define GRAVITY         12
#define MAX_FALL_SPEED  384
#define JUMP_POWER      420
#define WALK_SPEED      192
#define FLOAT_GRAVITY   4
#define FLOAT_FALL_MAX  64

#define FIX_SHIFT       8

// Sprite OAM indices
#define SPR_PLAYER      0
#define SPR_ENEMIES     1
#define SPR_STARS       (SPR_ENEMIES + MAX_ENEMIES)
#define SPR_HUD         (SPR_STARS + MAX_STARS)

// Collision tile types
#define TILE_EMPTY      0
#define TILE_SOLID      1
#define TILE_SPIKE      2
#define TILE_PLATFORM   3

// ============================================================
// GRAPHICS DATA
// ============================================================

// 4bpp Palette (16 colors)
const u16 spritePal[16] = {
    0x0000,  // 0: Transparent
    0x7FFF,  // 1: White
    0x001F,  // 2: Red
    0x7C00,  // 3: Blue
    0x03E0,  // 4: Green
    0x5294,  // 5: Pink (Kirby body)
    0x6B5A,  // 6: Light pink
    0x0000,  // 7: Black
    0x03FF,  // 8: Yellow
    0x4210,  // 9: Gray
    0x56B5,  // 10: Light gray
    0x7C1F,  // 11: Magenta
    0x7E0,   // 12: Cyan
    0x294A,  // 13: Dark gray
    0x421F,  // 14: Orange
    0x6318,  // 15: Lavender
};

const u16 bgPal[16] = {
    0x7E94,  // 0: Sky blue
    0x6B5A,  // 1: Ground light
    0x4A31,  // 2: Ground dark
    0x7FFF,  // 3: White (clouds)
    0x03E0,  // 4: Green (grass top)
    0x02A0,  // 5: Dark green
    0x0000,  // 6: Black
    0x56B5,  // 7: Platform light
    0x35AD,  // 8: Platform dark
    0x001F,  // 9: Spike red
    0x4210,  // 10: Gray
    0x5EF7,  // 11: Cloud shadow
    0x0180,  // 12: Bush green
    0x7E73,  // 13: Sky light
    0x0015,  // 14: Dark red
    0x7FFF,  // 15: Unused
};

// Player sprite tiles (16x16, 4 tiles each frame) - 4bpp format
// Frame 0: Idle
const u32 playerIdleTiles[32] = {
    // Top-left
    0x00000000, 0x00055000, 0x00566500, 0x05666650,
    0x56666665, 0x56616165, 0x56171765, 0x56666665,
    // Top-right
    0x00000000, 0x00055000, 0x00566500, 0x05666650,
    0x56666665, 0x56161665, 0x56717165, 0x56666665,
    // Bottom-left
    0x05666650, 0x00555550, 0x00566650, 0x05666650,
    0x05555550, 0x00000000, 0x00000000, 0x00000000,
    // Bottom-right
    0x05666650, 0x05555500, 0x05666500, 0x05666650,
    0x05555550, 0x00000000, 0x00000000, 0x00000000,
};

// Frame 1: Walk 1
const u32 playerWalk1Tiles[32] = {
    0x00000000, 0x00055000, 0x00566500, 0x05666650,
    0x56666665, 0x56616165, 0x56171765, 0x56666665,
    0x00000000, 0x00055000, 0x00566500, 0x05666650,
    0x56666665, 0x56161665, 0x56717165, 0x56666665,
    0x05666650, 0x00555500, 0x00056650, 0x00566650,
    0x05555000, 0x05550000, 0x00000000, 0x00000000,
    0x05666650, 0x00555500, 0x05665000, 0x05666500,
    0x00055550, 0x00005550, 0x00000000, 0x00000000,
};

// Frame 2: Walk 2
const u32 playerWalk2Tiles[32] = {
    0x00000000, 0x00055000, 0x00566500, 0x05666650,
    0x56666665, 0x56616165, 0x56171765, 0x56666665,
    0x00000000, 0x00055000, 0x00566500, 0x05666650,
    0x56666665, 0x56161665, 0x56717165, 0x56666665,
    0x05666650, 0x05555500, 0x05666000, 0x05666500,
    0x00555550, 0x00005550, 0x00000000, 0x00000000,
    0x05666650, 0x00555550, 0x00066650, 0x00566650,
    0x05555000, 0x05550000, 0x00000000, 0x00000000,
};

// Frame 3: Jump
const u32 playerJumpTiles[32] = {
    0x00000000, 0x00055000, 0x00566500, 0x05666650,
    0x56666665, 0x56666665, 0x56666665, 0x56666665,
    0x00000000, 0x00055000, 0x00566500, 0x05666650,
    0x56666665, 0x56666665, 0x56666665, 0x56666665,
    0x05666650, 0x55555555, 0x05666650, 0x05666650,
    0x05555550, 0x00000000, 0x00000000, 0x00000000,
    0x05666650, 0x55555555, 0x05666650, 0x05666650,
    0x05555550, 0x00000000, 0x00000000, 0x00000000,
};

// Frame 4: Float/Puff
const u32 playerFloatTiles[32] = {
    0x00000000, 0x00555500, 0x05666650, 0x56666665,
    0x66666666, 0x66616166, 0x66171766, 0x66666666,
    0x00000000, 0x00555500, 0x05666650, 0x56666665,
    0x66666666, 0x66161666, 0x66717166, 0x66666666,
    0x66666666, 0x56666665, 0x05666650, 0x05666650,
    0x00555500, 0x00000000, 0x00000000, 0x00000000,
    0x66666666, 0x56666665, 0x05666650, 0x05666650,
    0x00555500, 0x00000000, 0x00000000, 0x00000000,
};

// Enemy sprite (simple waddle dee style) - 16x16
const u32 enemyTiles[32] = {
    0x00000000, 0x00022000, 0x00288200, 0x02888820,
    0x28888882, 0x28818182, 0x28828282, 0x28888882,
    0x00000000, 0x00022000, 0x00288200, 0x02888820,
    0x28888882, 0x28181882, 0x28282882, 0x28888882,
    0x02888820, 0x00222200, 0x00288820, 0x02888820,
    0x02222200, 0x00000000, 0x00000000, 0x00000000,
    0x02888820, 0x00222200, 0x02888200, 0x02888820,
    0x00222220, 0x00000000, 0x00000000, 0x00000000,
};

// Star projectile (8x8)
const u32 starTiles[8] = {
    0x00080000, 0x00888000, 0x08888800, 0x88888888,
    0x88888888, 0x08888800, 0x00888000, 0x00080000,
};

// Background tiles (8x8 each)
const u32 bgTiles[10 * 8] = {
    // Tile 0: Empty sky
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    // Tile 1: Ground top with grass
    0x44444444, 0x44444444, 0x11111111, 0x12121212,
    0x21212121, 0x12121212, 0x21212121, 0x12121212,
    // Tile 2: Ground middle
    0x12121212, 0x21212121, 0x12121212, 0x21212121,
    0x12121212, 0x21212121, 0x12121212, 0x21212121,
    // Tile 3: Platform top
    0x77777777, 0x78888887, 0x78888887, 0x78888887,
    0x78888887, 0x78888887, 0x78888887, 0x77777777,
    // Tile 4: Cloud left
    0x00000000, 0x00000333, 0x00003333, 0x00033333,
    0x00333333, 0x03333333, 0x00333333, 0x00000000,
    // Tile 5: Cloud middle
    0x00000000, 0x33333333, 0x33333333, 0x33333333,
    0x33333333, 0x33333333, 0x33333333, 0x00000000,
    // Tile 6: Cloud right
    0x00000000, 0x33300000, 0x33330000, 0x33333000,
    0x33333300, 0x33333330, 0x33333300, 0x00000000,
    // Tile 7: Spike
    0x00090000, 0x00990000, 0x00999000, 0x09999000,
    0x09999900, 0x99999900, 0x99999990, 0x99999999,
    // Tile 8: Bush
    0x00000000, 0x00CCC000, 0x0CCCCC00, 0xCCCCCCCC,
    0xCCCCCCCC, 0x0CCCCC00, 0x00000000, 0x00000000,
    // Tile 9: Decorative block
    0xAAAAAAAA, 0xA1111111, 0xA1222221, 0xA1222221,
    0xA1222221, 0xA1222221, 0xA1111111, 0xAAAAAAAA,
};

// Level map (collision layer) - 0=empty, 1=solid, 2=spike, 3=platform
const u8 levelCollision[MAP_HEIGHT * MAP_WIDTH] = {
    // Row 0-7 (top - mostly empty)
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Row 8-11
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Row 12-15
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,3,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Row 16-19
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,2,2,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    // Row 20-23 (ground)
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    // Row 24-31 (below ground)
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

// ============================================================
// GAME STRUCTURES
// ============================================================

typedef enum {
    STATE_IDLE,
    STATE_WALK,
    STATE_JUMP,
    STATE_FALL,
    STATE_FLOAT,
    STATE_HURT,
    STATE_INHALE,
    STATE_DEAD
} PlayerState;

typedef struct {
    int x, y;           // Fixed point position (8.8)
    int vx, vy;         // Fixed point velocity
    int width, height;  // Hitbox size
    PlayerState state;
    int facing;         // 0 = right, 1 = left
    int onGround;
    int health;
    int maxHealth;
    int floatTimer;
    int invincible;
    int animFrame;
    int animTimer;
    int inhaleTimer;
    int hasAbility;
} Player;

typedef struct {
    int x, y;
    int vx, vy;
    int width, height;
    int active;
    int type;       // 0 = waddle, 1 = flying
    int health;
    int animFrame;
    int animTimer;
    int facing;
} Enemy;

typedef struct {
    int x, y;
    int vx, vy;
    int active;
    int lifetime;
} Star;

typedef struct {
    int scrollX, scrollY;
    int targetScrollX;
} Camera;

// ============================================================
// GLOBAL VARIABLES
// ============================================================

Player player;
Enemy enemies[MAX_ENEMIES];
Star stars[MAX_STARS];
Camera camera;
int gameFrame = 0;

OBJ_ATTR obj_buffer[128];
OBJ_AFFINE *obj_aff_buffer = (OBJ_AFFINE*)obj_buffer;

// ============================================================
// UTILITY FUNCTIONS
// ============================================================

static inline int fixMul(int a, int b) {
    return (a * b) >> FIX_SHIFT;
}

static inline int fixToInt(int f) {
    return f >> FIX_SHIFT;
}

static inline int intToFix(int i) {
    return i << FIX_SHIFT;
}

static inline int abs_val(int x) {
    return x < 0 ? -x : x;
}

// ============================================================
// COLLISION FUNCTIONS
// ============================================================

int getTileAt(int pixelX, int pixelY) {
    int tileX = pixelX / TILE_SIZE;
    int tileY = pixelY / TILE_SIZE;
    
    if (tileX < 0 || tileX >= MAP_WIDTH || tileY < 0 || tileY >= MAP_HEIGHT) {
        return TILE_SOLID;
    }
    
    return levelCollision[tileY * MAP_WIDTH + tileX];
}

int isSolidTile(int type) {
    return type == TILE_SOLID || type == TILE_SPIKE;
}

int checkCollisionX(int x, int y, int width, int height, int dir) {
    int checkX = (dir > 0) ? (x + width - 1) : x;
    
    for (int py = y; py < y + height; py += TILE_SIZE - 1) {
        if (isSolidTile(getTileAt(checkX, py))) {
            return 1;
        }
    }
    if (isSolidTile(getTileAt(checkX, y + height - 1))) {
        return 1;
    }
    
    return 0;
}

int checkCollisionY(int x, int y, int width, int height, int dir) {
    int checkY = (dir > 0) ? (y + height - 1) : y;
    
    for (int px = x; px < x + width; px += TILE_SIZE - 1) {
        int tile = getTileAt(px, checkY);
        if (dir > 0 && tile == TILE_PLATFORM && getTileAt(px, checkY - 1) != TILE_PLATFORM) {
            return 1;
        }
        if (isSolidTile(tile)) {
            return 1;
        }
    }
    int tile = getTileAt(x + width - 1, checkY);
    if (dir > 0 && tile == TILE_PLATFORM && getTileAt(x + width - 1, checkY - 1) != TILE_PLATFORM) {
        return 1;
    }
    if (isSolidTile(tile)) {
        return 1;
    }
    
    return 0;
}

int checkSpike(int x, int y, int width, int height) {
    for (int py = y; py < y + height; py += TILE_SIZE / 2) {
        for (int px = x; px < x + width; px += TILE_SIZE / 2) {
            if (getTileAt(px, py) == TILE_SPIKE) {
                return 1;
            }
        }
    }
    return 0;
}

// ============================================================
// PLAYER FUNCTIONS
// ============================================================

void initPlayer(void) {
    player.x = intToFix(24);
    player.y = intToFix(128);
    player.vx = 0;
    player.vy = 0;
    player.width = 12;
    player.height = 14;
    player.state = STATE_IDLE;
    player.facing = 0;
    player.onGround = 0;
    player.health = 6;
    player.maxHealth = 6;
    player.floatTimer = 0;
    player.invincible = 0;
    player.animFrame = 0;
    player.animTimer = 0;
    player.inhaleTimer = 0;
    player.hasAbility = 0;
}

void hurtPlayer(void) {
    if (player.invincible > 0 || player.state == STATE_DEAD) return;
    
    player.health--;
    player.invincible = 90;
    player.vy = -JUMP_POWER / 2;
    player.state = STATE_HURT;
    
    if (player.health <= 0) {
        player.state = STATE_DEAD;
    }
}

void updatePlayer(void) {
    if (player.state == STATE_DEAD) {
        player.vy += GRAVITY;
        player.y += player.vy;
        return;
    }
    
    // Decrease invincibility
    if (player.invincible > 0) {
        player.invincible--;
        if (player.invincible == 0 && player.state == STATE_HURT) {
            player.state = player.onGround ? STATE_IDLE : STATE_FALL;
        }
    }
    
    // Input handling
    int moveDir = 0;
    
    if (key_is_down(KEY_LEFT)) {
        moveDir = -1;
        player.facing = 1;
    } else if (key_is_down(KEY_RIGHT)) {
        moveDir = 1;
        player.facing = 0;
    }
    
    // Horizontal movement
    if (moveDir != 0 && player.state != STATE_HURT) {
        player.vx = moveDir * WALK_SPEED;
    } else {
        // Friction
        if (player.vx > 0) {
            player.vx -= 16;
            if (player.vx < 0) player.vx = 0;
        } else if (player.vx < 0) {
            player.vx += 16;
            if (player.vx > 0) player.vx = 0;
        }
    }
    
    // Jump / Float
    if (key_hit(KEY_A) && player.state != STATE_HURT) {
        if (player.onGround) {
            player.vy = -JUMP_POWER;
            player.onGround = 0;
            player.state = STATE_JUMP;
        } else if (player.state != STATE_FLOAT) {
            // Start floating
            player.state = STATE_FLOAT;
            player.vy = -JUMP_POWER / 3;
            player.floatTimer = 60;
        } else if (player.floatTimer > 0) {
            // Continue float boost
            player.vy = -JUMP_POWER / 3;
            player.floatTimer--;
        }
    }
    
    // Cancel float
    if (player.state == STATE_FLOAT && !key_is_down(KEY_A)) {
        player.state = STATE_FALL;
    }
    
    // Inhale (B button)
    if (key_is_down(KEY_B) && player.onGround && player.state != STATE_HURT) {
        player.state = STATE_INHALE;
        player.inhaleTimer = 20;
        player.vx = 0;
    }
    
    // Apply gravity
    if (player.state == STATE_FLOAT) {
        player.vy += FLOAT_GRAVITY;
        if (player.vy > FLOAT_FALL_MAX) player.vy = FLOAT_FALL_MAX;
    } else {
        player.vy += GRAVITY;
        if (player.vy > MAX_FALL_SPEED) player.vy = MAX_FALL_SPEED;
    }
    
    // Move and collide X
    int newX = player.x + player.vx;
    int pixelX = fixToInt(newX);
    int pixelY = fixToInt(player.y);
    
    if (player.vx > 0) {
        if (!checkCollisionX(pixelX + 2, pixelY + 1, player.width, player.height - 2, 1)) {
            player.x = newX;
        } else {
            player.x = intToFix((((pixelX + 2 + player.width) / TILE_SIZE) * TILE_SIZE) - player.width - 3);
            player.vx = 0;
        }
    } else if (player.vx < 0) {
        if (!checkCollisionX(pixelX + 2, pixelY + 1, player.width, player.height - 2, -1)) {
            player.x = newX;
        } else {
            player.x = intToFix(((pixelX + 2) / TILE_SIZE + 1) * TILE_SIZE - 2);
            player.vx = 0;
        }
    }
    
    // Move and collide Y
    int newY = player.y + player.vy;
    pixelX = fixToInt(player.x);
    pixelY = fixToInt(newY);
    
    if (player.vy > 0) {
        if (!checkCollisionY(pixelX + 2, pixelY + 1, player.width, player.height - 2, 1)) {
            player.y = newY;
            player.onGround = 0;
            if (player.state != STATE_FLOAT && player.state != STATE_HURT) {
                player.state = STATE_FALL;
            }
        } else {
            player.y = intToFix((((pixelY + 1 + player.height) / TILE_SIZE) * TILE_SIZE) - player.height - 1);
            player.vy = 0;
            player.onGround = 1;
            if (player.state == STATE_FLOAT || player.state == STATE_FALL || player.state == STATE_JUMP) {
                player.state = (moveDir != 0) ? STATE_WALK : STATE_IDLE;
            }
        }
    } else if (player.vy < 0) {
        if (!checkCollisionY(pixelX + 2, pixelY + 1, player.width, player.height - 2, -1)) {
            player.y = newY;
        } else {
            player.y = intToFix(((pixelY + 1) / TILE_SIZE + 1) * TILE_SIZE - 1);
            player.vy = 0;
        }
    }
    
    // Check spikes
    pixelX = fixToInt(player.x);
    pixelY = fixToInt(player.y);
    if (checkSpike(pixelX + 2, pixelY + 1, player.width, player.height)) {
        hurtPlayer();
    }
    
    // Fall death
    if (fixToInt(player.y) > MAP_HEIGHT * TILE_SIZE) {
        player.health = 0;
        player.state = STATE_DEAD;
    }
    
    // Update state based on movement
    if (player.state != STATE_HURT && player.state != STATE_FLOAT && 
        player.state != STATE_INHALE && player.state != STATE_DEAD) {
        if (!player.onGround) {
            player.state = (player.vy < 0) ? STATE_JUMP : STATE_FALL;
        } else {
            player.state = (player.vx != 0) ? STATE_WALK : STATE_IDLE;
        }
    }
    
    // Animation
    player.animTimer++;
    if (player.animTimer >= 8) {
        player.animTimer = 0;
        player.animFrame = (player.animFrame + 1) % 4;
    }
    
    // Clamp to level bounds
    if (player.x < 0) player.x = 0;
    if (player.x > intToFix(MAP_WIDTH * TILE_SIZE - player.width - 4)) {
        player.x = intToFix(MAP_WIDTH * TILE_SIZE - player.width - 4);
    }
}

// ============================================================
// ENEMY FUNCTIONS
// ============================================================

void spawnEnemy(int x, int y, int type) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) {
            enemies[i].x = intToFix(x);
            enemies[i].y = intToFix(y);
            enemies[i].vx = -64;
            enemies[i].vy = 0;
            enemies[i].width = 12;
            enemies[i].height = 12;
            enemies[i].active = 1;
            enemies[i].type = type;
            enemies[i].health = 1;
            enemies[i].animFrame = 0;
            enemies[i].animTimer = 0;
            enemies[i].facing = 1;
            break;
        }
    }
}

void updateEnemies(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;
        
        Enemy *e = &enemies[i];
        
        // Simple AI: walk back and forth
        e->x += e->vx;
        e->vy += GRAVITY;
        if (e->vy > MAX_FALL_SPEED) e->vy = MAX_FALL_SPEED;
        e->y += e->vy;
        
        int px = fixToInt(e->x);
        int py = fixToInt(e->y);
        
        // Ground collision
        if (checkCollisionY(px + 2, py, e->width, e->height, 1)) {
            e->y = intToFix(((py + e->height) / TILE_SIZE) * TILE_SIZE - e->height);
            e->vy = 0;
        }
        
        // Wall collision - turn around
        if (e->vx > 0 && checkCollisionX(px + 2, py, e->width, e->height, 1)) {
            e->vx = -e->vx;
            e->facing = 1;
        } else if (e->vx < 0 && checkCollisionX(px + 2, py, e->width, e->height, -1)) {
            e->vx = -e->vx;
            e->facing = 0;
        }
        
        // Check edge and turn
        int checkX = (e->vx > 0) ? px + e->width + 4 : px - 4;
        if (!isSolidTile(getTileAt(checkX, py + e->height + 4))) {
            e->vx = -e->vx;
            e->facing = (e->vx > 0) ? 0 : 1;
        }
        
        // Player collision
        int playerPx = fixToInt(player.x);
        int playerPy = fixToInt(player.y);
        
        if (player.state != STATE_DEAD && player.invincible == 0) {
            if (px + e->width > playerPx + 2 && px < playerPx + player.width + 2 &&
                py + e->height > playerPy && py < playerPy + player.height) {
                
                // Check if player is jumping on enemy
                if (player.vy > 0 && playerPy + player.height < py + e->height / 2) {
                    e->health--;
                    player.vy = -JUMP_POWER / 2;
                    if (e->health <= 0) {
                        e->active = 0;
                    }
                } else {
                    hurtPlayer();
                }
            }
        }
        
        // Deactivate if too far
        if (abs_val(px - fixToInt(player.x)) > 300) {
            e->active = 0;
        }
        
        // Animation
        e->animTimer++;
        if (e->animTimer >= 10) {
            e->animTimer = 0;
            e->animFrame = (e->animFrame + 1) % 2;
        }
    }
}

// ============================================================
// STAR PROJECTILE FUNCTIONS
// ============================================================

void shootStar(int x, int y, int dir) {
    for (int i = 0; i < MAX_STARS; i++) {
        if (!stars[i].active) {
            stars[i].x = intToFix(x);
            stars[i].y = intToFix(y);
            stars[i].vx = dir * 512;
            stars[i].vy = 0;
            stars[i].active = 1;
            stars[i].lifetime = 60;
            break;
        }
    }
}

void updateStars(void) {
    for (int i = 0; i < MAX_STARS; i++) {
        if (!stars[i].active) continue;
        
        stars[i].x += stars[i].vx;
        stars[i].y += stars[i].vy;
        stars[i].lifetime--;
        
        if (stars[i].lifetime <= 0) {
            stars[i].active = 0;
            continue;
        }
        
        int px = fixToInt(stars[i].x);
        int py = fixToInt(stars[i].y);
        
        // Wall collision
        if (isSolidTile(getTileAt(px, py)) || isSolidTile(getTileAt(px + 8, py))) {
            stars[i].active = 0;
            continue;
        }
        
        // Enemy collision
        for (int j = 0; j < MAX_ENEMIES; j++) {
            if (!enemies[j].active) continue;
            
            int ex = fixToInt(enemies[j].x);
            int ey = fixToInt(enemies[j].y);
            
            if (px + 8 > ex && px < ex + enemies[j].width &&
                py + 8 > ey && py < ey + enemies[j].height) {
                enemies[j].health--;
                if (enemies[j].health <= 0) {
                    enemies[j].active = 0;
                }
                stars[i].active = 0;
                break;
            }
        }
    }
}

// ============================================================
// CAMERA FUNCTIONS
// ============================================================

void updateCamera(void) {
    int playerPixelX = fixToInt(player.x);
    
    camera.targetScrollX = playerPixelX - SCREEN_WIDTH / 2 + 8;
    
    // Smooth camera
    int diff = camera.targetScrollX - camera.scrollX;
    camera.scrollX += diff / 8;
    
    // Clamp
    if (camera.scrollX < 0) camera.scrollX = 0;
    if (camera.scrollX > MAP_WIDTH * TILE_SIZE - SCREEN_WIDTH) {
        camera.scrollX = MAP_WIDTH * TILE_SIZE - SCREEN_WIDTH;
    }
    
    camera.scrollY = 0;
}

// ============================================================
// RENDERING FUNCTIONS
// ============================================================

void loadGraphics(void) {
    // Load sprite palette
    memcpy(pal_obj_mem, spritePal, sizeof(spritePal));
    
    // Load BG palette
    memcpy(pal_bg_mem, bgPal, sizeof(bgPal));
    
    // Load player sprites to VRAM (tiles 0-19 for all frames)
    memcpy(&tile_mem[4][0], playerIdleTiles, sizeof(playerIdleTiles));
    memcpy(&tile_mem[4][4], playerWalk1Tiles, sizeof(playerWalk1Tiles));
    memcpy(&tile_mem[4][8], playerWalk2Tiles, sizeof(playerWalk2Tiles));
    memcpy(&tile_mem[4][12], playerJumpTiles, sizeof(playerJumpTiles));
    memcpy(&tile_mem[4][16], playerFloatTiles, sizeof(playerFloatTiles));
    
    // Load enemy sprite
    memcpy(&tile_mem[4][20], enemyTiles, sizeof(enemyTiles));
    
    // Load star sprite
    memcpy(&tile_mem[4][24], starTiles, sizeof(starTiles));
    
    // Load BG tiles
    memcpy(&tile_mem[0][0], bgTiles, sizeof(bgTiles));
}

void buildBackground(void) {
    // Build visible map
    u16 *mapMem = (u16*)se_mem[31];
    
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int mapX = x;
            int mapY = y;
            
            if (mapX >= MAP_WIDTH || mapY >= MAP_HEIGHT) {
                mapMem[y * 32 + x] = 0;
                continue;
            }
            
            int tile = levelCollision[mapY * MAP_WIDTH + mapX];
            int bgTile = 0;
            
            switch (tile) {
                case TILE_EMPTY:
                    bgTile = 0;
                    break;
                case TILE_SOLID:
                    // Check if it's a surface tile
                    if (mapY > 0 && levelCollision[(mapY - 1) * MAP_WIDTH + mapX] == TILE_EMPTY) {
                        bgTile = 1; // Ground with grass
                    } else {
                        bgTile = 2; // Underground
                    }
                    break;
                case TILE_SPIKE:
                    bgTile = 7;
                    break;
                case TILE_PLATFORM:
                    bgTile = 3;
                    break;
            }
            
            mapMem[y * 32 + x] = bgTile;
        }
    }
}

void updateBackground(void) {
    REG_BG0HOFS = camera.scrollX;
    REG_BG0VOFS = camera.scrollY;
    
    // Update map based on scroll
    u16 *mapMem = (u16*)se_mem[31];
    int startTileX = camera.scrollX / TILE_SIZE;
    
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int mapX = (startTileX + x) % MAP_WIDTH;
            int mapY = y;
            
            if (mapY >= MAP_HEIGHT) {
                mapMem[y * 32 + x] = 2;
                continue;
            }
            
            int tile = levelCollision[mapY * MAP_WIDTH + mapX];
            int bgTile = 0;
            
            switch (tile) {
                case TILE_EMPTY:
                    // Add some clouds
                    if (y == 3 && (mapX % 20) >= 8 && (mapX % 20) <= 10) {
                        if ((mapX % 20) == 8) bgTile = 4;
                        else if ((mapX % 20) == 9) bgTile = 5;
                        else bgTile = 6;
                    } else if (y == 5 && (mapX % 15) >= 3 && (mapX % 15) <= 5) {
                        if ((mapX % 15) == 3) bgTile = 4;
                        else if ((mapX % 15) == 4) bgTile = 5;
                        else bgTile = 6;
                    } else {
                        bgTile = 0;
                    }
                    break;
                case TILE_SOLID:
                    if (mapY > 0 && levelCollision[(mapY - 1) * MAP_WIDTH + mapX] != TILE_SOLID) {
                        bgTile = 1;
                    } else {
                        bgTile = 2;
                    }
                    break;
                case TILE_SPIKE:
                    bgTile = 7;
                    break;
                case TILE_PLATFORM:
                    bgTile = 3;
                    break;
            }
            
            mapMem[y * 32 + x] = bgTile;
        }
    }
}

void renderSprites(void) {
    // Hide all sprites first
    oam_init(obj_buffer, 128);
    
    // Player sprite
    int playerScreenX = fixToInt(player.x) - camera.scrollX;
    int playerScreenY = fixToInt(player.y) - camera.scrollY;
    
    if (player.state != STATE_DEAD || (gameFrame % 4 < 2)) {
        // Select animation frame based on state
        int tileIdx = 0;
        switch (player.state) {
            case STATE_IDLE:
            case STATE_HURT:
                tileIdx = 0;
                break;
            case STATE_WALK:
                tileIdx = (player.animFrame % 2 == 0) ? 4 : 8;
                break;
            case STATE_JUMP:
                tileIdx = 12;
                break;
            case STATE_FALL:
                tileIdx = 12;
                break;
            case STATE_FLOAT:
                tileIdx = 16;
                break;
            case STATE_INHALE:
                tileIdx = 0;
                break;
            default:
                tileIdx = 0;
        }
        
        // Invincibility flash
        if (player.invincible > 0 && (player.invincible / 4) % 2 == 0) {
            // Skip rendering for flash effect
        } else {
            u16 attr2 = ATTR2_ID(tileIdx) | ATTR2_PALBANK(0);
            
            obj_set_attr(&obj_buffer[SPR_PLAYER],
                ATTR0_Y(playerScreenY) | ATTR0_SQUARE | ATTR0_4BPP,
                ATTR1_X(playerScreenX) | ATTR1_SIZE_16 | (player.facing ? ATTR1_HFLIP : 0),
                attr2);
        }
    }
    
    // Enemy sprites
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) {
            obj_hide(&obj_buffer[SPR_ENEMIES + i]);
            continue;
        }
        
        int screenX = fixToInt(enemies[i].x) - camera.scrollX;
        int screenY = fixToInt(enemies[i].y) - camera.scrollY;
        
        if (screenX > -16 && screenX < SCREEN_WIDTH && screenY > -16 && screenY < SCREEN_HEIGHT) {
            obj_set_attr(&obj_buffer[SPR_ENEMIES + i],
                ATTR0_Y(screenY) | ATTR0_SQUARE | ATTR0_4BPP,
                ATTR1_X(screenX) | ATTR1_SIZE_16 | (enemies[i].facing ? ATTR1_HFLIP : 0),
                ATTR2_ID(20) | ATTR2_PALBANK(0));
        } else {
            obj_hide(&obj_buffer[SPR_ENEMIES + i]);
        }
    }
    
    // Star sprites
    for (int i = 0; i < MAX_STARS; i++) {
        if (!stars[i].active) {
            obj_hide(&obj_buffer[SPR_STARS + i]);
            continue;
        }
        
        int screenX = fixToInt(stars[i].x) - camera.scrollX;
        int screenY = fixToInt(stars[i].y) - camera.scrollY;
        
        if (screenX > -8 && screenX < SCREEN_WIDTH && screenY > -8 && screenY < SCREEN_HEIGHT) {
            obj_set_attr(&obj_buffer[SPR_STARS + i],
                ATTR0_Y(screenY) | ATTR0_SQUARE | ATTR0_4BPP,
                ATTR1_X(screenX) | ATTR1_SIZE_8,
                ATTR2_ID(24) | ATTR2_PALBANK(0));
        } else {
            obj_hide(&obj_buffer[SPR_STARS + i]);
        }
    }
    
    // Health display (simple hearts using small squares)
    for (int i = 0; i < player.maxHealth; i++) {
        int hx = 8 + i * 10;
        int hy = 8;
        
        u16 palbank = (i < player.health) ? 0 : 0; // Could use different palette for empty
        int tileId = (i < player.health) ? 24 : 24;
        
        if (i < player.health) {
            obj_set_attr(&obj_buffer[SPR_HUD + i],
                ATTR0_Y(hy) | ATTR0_SQUARE | ATTR0_4BPP,
                ATTR1_X(hx) | ATTR1_SIZE_8,
                ATTR2_ID(tileId) | ATTR2_PALBANK(palbank));
        } else {
            obj_hide(&obj_buffer[SPR_HUD + i]);
        }
    }
    
    // Copy to OAM
    oam_copy(oam_mem, obj_buffer, 128);
}

// ============================================================
// GAME INITIALIZATION
// ============================================================

void initGame(void) {
    // Initialize player
    initPlayer();
    
    // Initialize camera
    camera.scrollX = 0;
    camera.scrollY = 0;
    camera.targetScrollX = 0;
    
    // Clear enemies and stars
    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].active = 0;
    }
    for (int i = 0; i < MAX_STARS; i++) {
        stars[i].active = 0;
    }
    
    // Spawn some initial enemies
    spawnEnemy(100, 140, 0);
    spawnEnemy(180, 140, 0);
    spawnEnemy(280, 140, 0);
    spawnEnemy(350, 50, 0);
    spawnEnemy(420, 140, 0);
}

void gameOver(void) {
    // Simple game over - reset after delay
    static int deathTimer = 0;
    deathTimer++;
    
    if (deathTimer > 120 || key_hit(KEY_START)) {
        initGame();
        deathTimer = 0;
    }
}

// ============================================================
// MAIN FUNCTION
// ============================================================

int main(void) {
    // Set up display
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // Set up BG0
    REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32 | BG_PRIO(1);
    
    // Load all graphics
    loadGraphics();
    
    // Build initial background
    buildBackground();
    
    // Initialize game
    initGame();
    
    // Main game loop
    while (1) {
        // Wait for VBlank
        vid_vsync();
        
        // Poll input
        key_poll();
        
        // Game logic
        if (player.state == STATE_DEAD) {
            gameOver();
        } else {
            updatePlayer();
            updateEnemies();
            updateStars();
            updateCamera();
            
            // Shoot star with R button
            if (key_hit(KEY_R)) {
                int dir = player.facing ? -1 : 1;
                shootStar(fixToInt(player.x) + 4, fixToInt(player.y) + 4, dir);
            }
            
            // Respawn enemies periodically
            if (gameFrame % 180 == 0) {
                int spawnX = fixToInt(player.x) + (player.facing ? -200 : 200);
                if (spawnX > 0 && spawnX < MAP_WIDTH * TILE_SIZE - 20) {
                    spawnEnemy(spawnX, 100, 0);
                }
            }
        }
        
        // Rendering
        updateBackground();
        renderSprites();
        
        gameFrame++;
    }
    
    return 0;
}
