// kirby_gba.c - A Kirby-like 2D side-scroller for GBA using tonc
// Compile with: gcc -mthumb -mthumb-interwork -O2 -o kirby.gba kirby_gba.c -ltonc
// Or use a Makefile with devkitARM

#include <tonc.h>
#include <string.h>

// ============================================================
// CONSTANTS
// ============================================================
#define FIXED_SHIFT     8
#define FIXED_ONE       (1 << FIXED_SHIFT)
#define INT2FX(x)       ((x) << FIXED_SHIFT)
#define FX2INT(x)       ((x) >> FIXED_SHIFT)
#define FLOAT2FX(x)     ((int)((x) * FIXED_SHIFT))

#define SCREEN_W        240
#define SCREEN_H        160

#define TILE_SIZE       8
#define META_TILE_SIZE  16
#define MAP_W           64
#define MAP_H           20

#define GRAVITY         12
#define JUMP_VEL        (-350)
#define FLOAT_VEL       (-80)
#define FLOAT_GRAVITY   4
#define MOVE_SPEED      180
#define MAX_FALL_SPEED  400
#define FLOAT_MAX_FALL  60

#define MAX_ENEMIES     8
#define ENEMY_SPEED     60

#define PLAYER_W        14
#define PLAYER_H        14
#define ENEMY_W         12
#define ENEMY_H         12

#define KIRBY_HEALTH    6
#define INHALE_RANGE    40
#define INHALE_W        40
#define INHALE_H        16

// Player states
#define STATE_NORMAL    0
#define STATE_FLOATING  1
#define STATE_INHALE    2
#define STATE_FULL      3  // has enemy in mouth

// Animation frames
#define ANIM_IDLE       0
#define ANIM_WALK1      1
#define ANIM_WALK2      2
#define ANIM_JUMP       3
#define ANIM_FLOAT1     4
#define ANIM_FLOAT2     5
#define ANIM_INHALE     6
#define ANIM_FULL       7

// Sprite tile indices (each 16x16 = 4 tiles in 1D mapping)
#define SPR_KIRBY_BASE  0
#define SPR_ENEMY_BASE  32
#define SPR_STAR_BASE   40
#define SPR_HUD_BASE    44

// BG tile indices
#define BG_EMPTY        0
#define BG_GROUND       1
#define BG_GROUND_TOP   2
#define BG_BRICK        3
#define BG_PLATFORM     4
#define BG_SKY          0
#define BG_CLOUD1       5
#define BG_CLOUD2       6
#define BG_HILL         7

// OAM
#define OAM_KIRBY       0
#define OAM_ENEMY_START 1
#define OAM_STAR        9
#define OAM_HUD_START   10

// ============================================================
// TILE DATA - hand-crafted pixel art (4bpp)
// Each 8x8 tile = 32 bytes (8 rows * 4 bytes/row in 4bpp)
// ============================================================

// Color palette for sprites
static const u16 sprite_pal[16] = {
    0x0000,  // 0: transparent
    0x7FFF,  // 1: white
    0x001F,  // 2: red (BGR: red)
    0x7C00,  // 3: blue
    0x03E0,  // 4: green
    0x0010,  // 5: dark red / pink core
    0x5AD6,  // 6: light pink (kirby body)  - RGB(22,22,22) approx
    0x7E1F,  // 7: bright pink (kirby main)
    0x0000,  // 8: black (outline)
    0x2D7F,  // 9: kirby pink  (actual pink in BGR555)
    0x56BF,  // 10: lighter pink
    0x7FFF,  // 11: white (eyes)
    0x4210,  // 12: gray
    0x03FF,  // 13: yellow (star)
    0x7C1F,  // 14: magenta
    0x0200,  // 15: dark green
};

// Better Kirby pink palette
static const u16 kirby_pal[16] = {
    0x0000,  // 0: transparent
    0x7FFF,  // 1: white
    0x529F,  // 2: kirby pink (main body)
    0x7FBF,  // 3: kirby light pink (highlights)
    0x2D3F,  // 4: kirby dark pink (shadows/feet)
    0x0000,  // 5: black (outline/eyes)
    0x001F,  // 6: red (feet/blush)
    0x03E0,  // 7: green (enemy)
    0x7C00,  // 8: blue (sky stuff)
    0x03FF,  // 9: yellow (star)
    0x4210,  // 10: gray
    0x294A,  // 11: dark gray
    0x7FFF,  // 12: white (eye shine)
    0x0014,  // 13: dark red
    0x56B5,  // 14: light gray
    0x7C1F,  // 15: magenta
};

// Kirby idle - 16x16 (4 tiles: TL, TR, BL, BR) - 4bpp
// Each u32 = one row of an 8x8 tile (8 pixels, 4 bits each)
// Pixel order: least significant nibble = leftmost pixel

static const u32 kirby_idle_tiles[4 * 8] = {
    // Tile 0 - Top Left
    0x00000000,
    0x00005500,
    0x00052250,
    0x00522220,
    0x05222220,
    0x52222120,
    0x52221120,
    0x52225520,
    // Tile 1 - Top Right
    0x00000000,
    0x00550000,
    0x05222000,
    0x02222500,
    0x02222250,
    0x02122250,
    0x02115250,
    0x02552250,
    // Tile 2 - Bottom Left
    0x52222220,
    0x52222220,
    0x05222250,
    0x00522200,
    0x00044500,
    0x00044400,
    0x00044400,
    0x00000000,
    // Tile 3 - Bottom Right
    0x02222250,
    0x02222250,
    0x05222500,
    0x00222500,
    0x00544000,
    0x00444000,
    0x00444000,
    0x00000000,
};

// Kirby walk frame
static const u32 kirby_walk_tiles[4 * 8] = {
    // Tile 0 - Top Left
    0x00000000,
    0x00005500,
    0x00052250,
    0x00522220,
    0x05222220,
    0x52222120,
    0x52221120,
    0x52225520,
    // Tile 1 - Top Right
    0x00000000,
    0x00550000,
    0x05222000,
    0x02222500,
    0x02222250,
    0x02122250,
    0x02115250,
    0x02552250,
    // Tile 2 - Bottom Left
    0x52222220,
    0x52222220,
    0x05222250,
    0x00522200,
    0x00054500,
    0x00004400,
    0x00004400,
    0x00000000,
    // Tile 3 - Bottom Right
    0x02222250,
    0x02222250,
    0x05222500,
    0x00222500,
    0x00544000,
    0x00440000,
    0x00440000,
    0x00000000,
};

// Kirby jump
static const u32 kirby_jump_tiles[4 * 8] = {
    // Tile 0 - Top Left
    0x00000000,
    0x00005500,
    0x00052250,
    0x00522220,
    0x05222220,
    0x52222120,
    0x52221120,
    0x52225520,
    // Tile 1 - Top Right
    0x00000000,
    0x00550000,
    0x05222000,
    0x02222500,
    0x02222250,
    0x02122250,
    0x02115250,
    0x02552250,
    // Tile 2 - Bottom Left
    0x52222220,
    0x05222220,
    0x00522250,
    0x40052200,
    0x44005500,
    0x44000000,
    0x00000000,
    0x00000000,
    // Tile 3 - Bottom Right
    0x02222250,
    0x02222500,
    0x05222500,
    0x00225004,
    0x00550044,
    0x00000044,
    0x00000000,
    0x00000000,
};

// Kirby float (puffed up)
static const u32 kirby_float_tiles[4 * 8] = {
    // Tile 0 - Top Left
    0x00000000,
    0x00055500,
    0x00522250,
    0x05222220,
    0x52222220,
    0x52222120,
    0x52221120,
    0x52222220,
    // Tile 1 - Top Right
    0x00000000,
    0x00555000,
    0x05222500,
    0x02222250,
    0x02222250,
    0x02122250,
    0x02115250,
    0x02222250,
    // Tile 2 - Bottom Left
    0x52222220,
    0x52222220,
    0x52222220,
    0x05222250,
    0x00522200,
    0x00055500,
    0x00000000,
    0x00000000,
    // Tile 3 - Bottom Right
    0x02222250,
    0x02222250,
    0x02222250,
    0x05222500,
    0x00222500,
    0x00555000,
    0x00000000,
    0x00000000,
};

// Kirby inhale (mouth open)
static const u32 kirby_inhale_tiles[4 * 8] = {
    // Tile 0 - Top Left
    0x00000000,
    0x00005500,
    0x00052250,
    0x00522220,
    0x05222220,
    0x52222120,
    0x52221120,
    0x52225520,
    // Tile 1 - Top Right
    0x00000000,
    0x00550000,
    0x05222000,
    0x02222500,
    0x02222250,
    0x02122250,
    0x02115250,
    0x02552250,
    // Tile 2 - Bottom Left
    0x52222220,
    0x52255550,
    0x05250050,
    0x00525050,
    0x00055500,
    0x00044400,
    0x00044400,
    0x00000000,
    // Tile 3 - Bottom Right
    0x02222250,
    0x05555250,
    0x05005250,
    0x05052500,
    0x00555000,
    0x00444000,
    0x00444000,
    0x00000000,
};

// Kirby full (with enemy - puffy cheeks)
static const u32 kirby_full_tiles[4 * 8] = {
    // Tile 0 - Top Left
    0x00000000,
    0x00055500,
    0x00522250,
    0x05222220,
    0x52222220,
    0x52222120,
    0x52221120,
    0x52222220,
    // Tile 1 - Top Right
    0x00000000,
    0x00555000,
    0x05222500,
    0x02222250,
    0x02222250,
    0x02122250,
    0x02115250,
    0x02222250,
    // Tile 2 - Bottom Left
    0x52233220,
    0x52233220,
    0x05222250,
    0x00522200,
    0x00044500,
    0x00044400,
    0x00044400,
    0x00000000,
    // Tile 3 - Bottom Right
    0x02332250,
    0x02332250,
    0x05222500,
    0x00222500,
    0x00544000,
    0x00444000,
    0x00444000,
    0x00000000,
};

// Enemy sprite (simple waddle-dee like blob)
static const u32 enemy_tiles[4 * 8] = {
    // Tile 0 - Top Left
    0x00000000,
    0x00000000,
    0x00007700,
    0x00077770,
    0x00777770,
    0x07757570,
    0x07755570,
    0x07777770,
    // Tile 1 - Top Right
    0x00000000,
    0x00000000,
    0x00770000,
    0x07777000,
    0x07777700,
    0x07575700,
    0x07555700,
    0x07777700,
    // Tile 2 - Bottom Left
    0x07777770,
    0x07777770,
    0x00777700,
    0x00077000,
    0x00077000,
    0x00000000,
    0x00000000,
    0x00000000,
    // Tile 3 - Bottom Right
    0x07777700,
    0x07777700,
    0x00777700,
    0x00077000,
    0x00077000,
    0x00000000,
    0x00000000,
    0x00000000,
};

// Star projectile (8x8 single tile, we'll use 16x16 with one tile repeated or just small)
static const u32 star_tiles[4 * 8] = {
    // Tile 0 - Top Left
    0x00000000,
    0x00090000,
    0x00099000,
    0x09999900,
    0x09999900,
    0x00099000,
    0x00090000,
    0x00000000,
    // Tile 1 - Top Right
    0x00000000,
    0x00009000,
    0x00099000,
    0x00999900,
    0x00999900,
    0x00099000,
    0x00009000,
    0x00000000,
    // Tile 2 - Bottom Left
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    // Tile 3 - Bottom Right
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
};

// Heart (HUD health icon) - 8x8
static const u32 heart_tile[8] = {
    0x00000000,
    0x06600660,
    0x06660660,
    0x06666660,
    0x06666660,
    0x00666600,
    0x00066000,
    0x00000000,
};

// ============================================================
// BG TILE DATA
// ============================================================

static const u16 bg_pal[16] = {
    0x7E18,  // 0: sky blue
    0x7FFF,  // 1: white
    0x0200,  // 2: dark green (ground)
    0x02A0,  // 3: green (grass top)
    0x0150,  // 4: brown  (dirt)
    0x5294,  // 5: light gray (brick)
    0x294A,  // 6: dark gray (brick shadow)
    0x7F00,  // 7: light blue (cloud)
    0x7FFF,  // 8: white (cloud)
    0x03C0,  // 9: bright green (hill)
    0x7E98,  // 10: lighter sky
    0x01A0,  // 11: medium green
    0x4210,  // 12: gray
    0x7E52,  // 13: pale blue
    0x02E0,  // 14: grass green
    0x0000,  // 15: black
};

// Empty tile (sky)
static const u32 bg_sky_tile[8] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Ground tile (dirt)
static const u32 bg_ground_tile[8] = {
    0x44444444, 0x44444444, 0x44244444, 0x44444424,
    0x44444444, 0x42444444, 0x44444244, 0x44444444,
};

// Ground top tile (grass on top)
static const u32 bg_ground_top_tile[8] = {
    0x33333333, 0x33333333, 0x34343434, 0x44444444,
    0x44444444, 0x44244444, 0x44444424, 0x44444444,
};

// Brick tile
static const u32 bg_brick_tile[8] = {
    0x55555556, 0x55555556, 0x55555556, 0x66666666,
    0x55655555, 0x55655555, 0x55655555, 0x66666666,
};

// Platform tile (thin)
static const u32 bg_platform_tile[8] = {
    0x55555555, 0x56565656, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Cloud tile 1
static const u32 bg_cloud1_tile[8] = {
    0x00000000, 0x00088000, 0x00888800, 0x08888880,
    0x88888888, 0x88888888, 0x08888880, 0x00000000,
};

// Cloud tile 2
static const u32 bg_cloud2_tile[8] = {
    0x00000000, 0x00000000, 0x00880000, 0x08888800,
    0x88888888, 0x88888888, 0x08888880, 0x00000000,
};

// Hill tile (background decoration)
static const u32 bg_hill_tile[8] = {
    0x00000000, 0x00099000, 0x00999900, 0x09999990,
    0x09999990, 0x99999999, 0x99999999, 0x99999999,
};

// ============================================================
// LEVEL MAP DATA
// ============================================================

// Map uses meta-tile IDs that reference the bg tiles above
// 0=sky, 1=ground, 2=ground_top, 3=brick, 4=platform, 5=cloud1, 6=cloud2, 7=hill
static const u8 level_map[MAP_H][MAP_W] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,5,6,0,0,0,0,0,0,0,0,0,0,0,5,6,0,0,0,0,0,0,0,0,0,0,0,5,6,0,0,0,0,0,0,0,0,0,0,5,6,0,0,0,0,0,0,0,5,6,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,2,2,2,2,2,2,2,2,0,0,0,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,0,0,0,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,0,0,0,2,2,2,2,2,2,2,2,2,2},
    {1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1},
};

// ============================================================
// GAME STRUCTURES
// ============================================================

typedef struct {
    int x, y;          // fixed point position
    int vx, vy;        // fixed point velocity
    int state;
    int health;
    int facing;        // 0=right, 1=left
    int on_ground;
    int anim_frame;
    int anim_timer;
    int invincible;    // invincibility frames
    int inhale_timer;
    BOOL has_enemy;    // has swallowed enemy
    BOOL alive;
} Player;

typedef struct {
    int x, y;
    int vx;
    int dir;       // 0=right, 1=left
    int anim_timer;
    int type;      // enemy type
    BOOL alive;
    BOOL active;   // on screen
    int patrol_left, patrol_right;
} Enemy;

typedef struct {
    int x, y;
    int vx, vy;
    BOOL active;
    int life;
} Star;

typedef struct {
    int cam_x, cam_y;
    int scroll_x;
    int score;
    BOOL paused;
    int frame_count;
} GameState;

// ============================================================
// GLOBALS
// ============================================================

static Player player;
static Enemy enemies[MAX_ENEMIES];
static Star star;
static GameState game;

// ============================================================
// HELPER FUNCTIONS
// ============================================================

// Check if a tile at map position (tx, ty) is solid
static int is_solid(int tx, int ty) {
    if (tx < 0 || tx >= MAP_W || ty < 0) return 0;
    if (ty >= MAP_H) return 1; // below map is solid
    u8 t = level_map[ty][tx];
    return (t == BG_GROUND || t == BG_GROUND_TOP || t == BG_BRICK);
}

// Check if tile is a platform (can stand on top but pass through from below)
static int is_platform(int tx, int ty) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H) return 0;
    return level_map[ty][tx] == BG_PLATFORM;
}

// Get tile at pixel position
static int tile_at_px(int px, int py) {
    int tx = px / TILE_SIZE;
    int ty = py / TILE_SIZE;
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H) return 0;
    return level_map[ty][tx];
}

// AABB collision
static int aabb_check(int x1, int y1, int w1, int h1,
                       int x2, int y2, int w2, int h2) {
    return (x1 < x2 + w2 && x1 + w1 > x2 &&
            y1 < y2 + h2 && y1 + h1 > y2);
}

// ============================================================
// INITIALIZATION
// ============================================================

static void load_sprite_tiles(void) {
    // Copy Kirby frames into OBJ VRAM
    // Using 1D tile mapping, each 16x16 sprite uses 4 consecutive 8x8 tiles
    // In 4bpp: each tile = 32 bytes = 8 u32s

    // Kirby idle (tiles 0-3)
    memcpy(&tile_mem[4][SPR_KIRBY_BASE + 0], kirby_idle_tiles, 4 * 32);

    // Kirby walk (tiles 4-7)
    memcpy(&tile_mem[4][SPR_KIRBY_BASE + 4], kirby_walk_tiles, 4 * 32);

    // Kirby walk2 = idle (reuse) (tiles 8-11)
    memcpy(&tile_mem[4][SPR_KIRBY_BASE + 8], kirby_idle_tiles, 4 * 32);

    // Kirby jump (tiles 12-15)
    memcpy(&tile_mem[4][SPR_KIRBY_BASE + 12], kirby_jump_tiles, 4 * 32);

    // Kirby float1 (tiles 16-19)
    memcpy(&tile_mem[4][SPR_KIRBY_BASE + 16], kirby_float_tiles, 4 * 32);

    // Kirby float2 (tiles 20-23) - reuse float
    memcpy(&tile_mem[4][SPR_KIRBY_BASE + 20], kirby_float_tiles, 4 * 32);

    // Kirby inhale (tiles 24-27)
    memcpy(&tile_mem[4][SPR_KIRBY_BASE + 24], kirby_inhale_tiles, 4 * 32);

    // Kirby full (tiles 28-31)
    memcpy(&tile_mem[4][SPR_KIRBY_BASE + 28], kirby_full_tiles, 4 * 32);

    // Enemy (tiles 32-35)
    memcpy(&tile_mem[4][SPR_ENEMY_BASE], enemy_tiles, 4 * 32);

    // Star (tiles 40-43)
    memcpy(&tile_mem[4][SPR_STAR_BASE], star_tiles, 4 * 32);

    // Heart for HUD (single 8x8 tile at tile 44)
    memcpy(&tile_mem[4][SPR_HUD_BASE], heart_tile, 32);
}

static void load_bg_tiles(void) {
    // Load BG tiles into charblock 0
    memcpy(&tile_mem[0][BG_SKY],         bg_sky_tile,         32);
    memcpy(&tile_mem[0][BG_GROUND],      bg_ground_tile,      32);
    memcpy(&tile_mem[0][BG_GROUND_TOP],  bg_ground_top_tile,  32);
    memcpy(&tile_mem[0][BG_BRICK],       bg_brick_tile,       32);
    memcpy(&tile_mem[0][BG_PLATFORM],    bg_platform_tile,    32);
    memcpy(&tile_mem[0][BG_CLOUD1],      bg_cloud1_tile,      32);
    memcpy(&tile_mem[0][BG_CLOUD2],      bg_cloud2_tile,      32);
    memcpy(&tile_mem[0][BG_HILL],        bg_hill_tile,        32);
}

static void build_bg_map(void) {
    // Fill screenblock 31 with map data
    // BG0 uses 8x8 tiles, our map is in 8x8 tile units
    SCR_ENTRY *map = se_mem[31];

    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {  // 64 tiles wide (512px, we use 2 screenblocks)
            int tile_id = BG_SKY;
            if (y < MAP_H && x < MAP_W) {
                tile_id = level_map[y][x];
            }
            // For 64-wide map, we need to handle screenblock layout
            // 64x32 map uses screenblocks 31 and next
            if (x < 32) {
                se_mem[30][y * 32 + x] = tile_id;
            } else {
                se_mem[31][y * 32 + (x - 32)] = tile_id;
            }
        }
    }
}

static void init_gfx(void) {
    // Set display mode
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG0: main level tilemap (64x32 tiles)
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_SIZE(1) | BG_PRIO(1) | BG_4BPP;

    // BG1: parallax background (not used for now, but set up)
    REG_BG1CNT = BG_CBB(0) | BG_SBB(28) | BG_SIZE(0) | BG_PRIO(2) | BG_4BPP;

    // Load palettes
    memcpy(pal_obj_mem, kirby_pal, 16 * sizeof(u16));
    memcpy(pal_bg_mem, bg_pal, 16 * sizeof(u16));

    // Load tile graphics
    load_bg_tiles();
    load_sprite_tiles();
    build_bg_map();

    // Clear OAM
    oam_init(obj_mem, 128);
}

static void init_player(void) {
    player.x = INT2FX(24);
    player.y = INT2FX(120);
    player.vx = 0;
    player.vy = 0;
    player.state = STATE_NORMAL;
    player.health = KIRBY_HEALTH;
    player.facing = 0;
    player.on_ground = 0;
    player.anim_frame = 0;
    player.anim_timer = 0;
    player.invincible = 0;
    player.inhale_timer = 0;
    player.has_enemy = FALSE;
    player.alive = TRUE;
}

static void init_enemies(void) {
    // Place enemies throughout the level
    int enemy_positions[][2] = {
        {100, 128}, {180, 128}, {280, 80}, {350, 128},
        {420, 128}, {500, 80},  {580, 128}, {700, 128},
    };

    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].x = INT2FX(enemy_positions[i][0]);
        enemies[i].y = INT2FX(enemy_positions[i][1]);
        enemies[i].vx = ENEMY_SPEED;
        enemies[i].dir = 0;
        enemies[i].alive = TRUE;
        enemies[i].active = TRUE;
        enemies[i].anim_timer = 0;
        enemies[i].type = 0;
        enemies[i].patrol_left = enemy_positions[i][0] - 30;
        enemies[i].patrol_right = enemy_positions[i][0] + 30;
    }
}

static void init_star(void) {
    star.active = FALSE;
    star.x = 0;
    star.y = 0;
    star.vx = 0;
    star.vy = 0;
    star.life = 0;
}

static void init_game(void) {
    game.cam_x = 0;
    game.cam_y = 0;
    game.scroll_x = 0;
    game.score = 0;
    game.paused = FALSE;
    game.frame_count = 0;

    init_player();
    init_enemies();
    init_star();
}

// ============================================================
// COLLISION WITH TILEMAP
// ============================================================

static void collide_player_map(void) {
    int px = FX2INT(player.x);
    int py = FX2INT(player.y);

    // Horizontal collision
    if (player.vx > 0) {
        // Moving right - check right edge
        int right = px + PLAYER_W;
        int tx = right / TILE_SIZE;
        int ty_top = py / TILE_SIZE;
        int ty_bot = (py + PLAYER_H - 1) / TILE_SIZE;
        for (int ty = ty_top; ty <= ty_bot; ty++) {
            if (is_solid(tx, ty)) {
                player.x = INT2FX(tx * TILE_SIZE - PLAYER_W);
                player.vx = 0;
                break;
            }
        }
    } else if (player.vx < 0) {
        // Moving left - check left edge
        int left = px;
        int tx = (left - 1) / TILE_SIZE;
        int ty_top = py / TILE_SIZE;
        int ty_bot = (py + PLAYER_H - 1) / TILE_SIZE;
        for (int ty = ty_top; ty <= ty_bot; ty++) {
            if (is_solid(tx, ty)) {
                player.x = INT2FX((tx + 1) * TILE_SIZE);
                player.vx = 0;
                break;
            }
        }
    }

    // Update position after horizontal correction
    px = FX2INT(player.x);
    py = FX2INT(player.y);

    // Vertical collision
    player.on_ground = 0;

    if (player.vy > 0) {
        // Falling - check bottom edge
        int bottom = py + PLAYER_H;
        int ty = bottom / TILE_SIZE;
        int tx_left = px / TILE_SIZE;
        int tx_right = (px + PLAYER_W - 1) / TILE_SIZE;

        for (int tx = tx_left; tx <= tx_right; tx++) {
            if (is_solid(tx, ty)) {
                player.y = INT2FX(ty * TILE_SIZE - PLAYER_H);
                player.vy = 0;
                player.on_ground = 1;
                if (player.state == STATE_FLOATING) {
                    player.state = player.has_enemy ? STATE_FULL : STATE_NORMAL;
                }
                break;
            }
            // Platform collision (only from above)
            if (is_platform(tx, ty) && (py + PLAYER_H - FX2INT(player.vy)) <= ty * TILE_SIZE) {
                player.y = INT2FX(ty * TILE_SIZE - PLAYER_H);
                player.vy = 0;
                player.on_ground = 1;
                if (player.state == STATE_FLOATING) {
                    player.state = player.has_enemy ? STATE_FULL : STATE_NORMAL;
                }
                break;
            }
        }
    } else if (player.vy < 0) {
        // Jumping - check top edge
        int top = py;
        int ty = (top - 1) / TILE_SIZE;
        int tx_left = px / TILE_SIZE;
        int tx_right = (px + PLAYER_W - 1) / TILE_SIZE;

        for (int tx = tx_left; tx <= tx_right; tx++) {
            if (is_solid(tx, ty)) {
                player.y = INT2FX((ty + 1) * TILE_SIZE);
                player.vy = 0;
                break;
            }
        }
    }

    // Check if fallen into pit
    if (FX2INT(player.y) > MAP_H * TILE_SIZE) {
        player.health = 0;
        player.alive = FALSE;
    }

    // Clamp to level bounds
    if (player.x < 0) {
        player.x = 0;
        player.vx = 0;
    }
    if (FX2INT(player.x) > MAP_W * TILE_SIZE - PLAYER_W) {
        player.x = INT2FX(MAP_W * TILE_SIZE - PLAYER_W);
        player.vx = 0;
    }
}

// Simple enemy-map collision
static void collide_enemy_map(Enemy *e) {
    int px = FX2INT(e->x);
    int py = FX2INT(e->y);

    // Check ground beneath
    int bottom = py + ENEMY_H;
    int ty = bottom / TILE_SIZE;
    int tx = (px + ENEMY_W / 2) / TILE_SIZE;

    if (is_solid(tx, ty) || is_platform(tx, ty)) {
        e->y = INT2FX(ty * TILE_SIZE - ENEMY_H);
    }

    // Check walls for direction reversal
    if (e->vx > 0) {
        int right = px + ENEMY_W;
        int wtx = right / TILE_SIZE;
        int wty = py / TILE_SIZE;
        if (is_solid(wtx, wty)) {
            e->dir = 1;
            e->vx = -ENEMY_SPEED;
        }
    } else {
        int left = px;
        int wtx = (left - 1) / TILE_SIZE;
        int wty = py / TILE_SIZE;
        if (is_solid(wtx, wty)) {
            e->dir = 0;
            e->vx = ENEMY_SPEED;
        }
    }

    // Check if at edge of platform (turn around)
    int ftx = (e->vx > 0) ? (px + ENEMY_W + 2) / TILE_SIZE : (px - 2) / TILE_SIZE;
    int fty = (py + ENEMY_H + 2) / TILE_SIZE;
    if (!is_solid(ftx, fty) && !is_platform(ftx, fty)) {
        e->dir = !e->dir;
        e->vx = e->dir ? -ENEMY_SPEED : ENEMY_SPEED;
    }
}

// ============================================================
// UPDATE FUNCTIONS
// ============================================================

static void update_player(void) {
    if (!player.alive) return;

    // Read input
    key_poll();

    // Horizontal movement
    player.vx = 0;
    if (key_is_down(KEY_LEFT)) {
        player.vx = -MOVE_SPEED;
        player.facing = 1;
    }
    if (key_is_down(KEY_RIGHT)) {
        player.vx = MOVE_SPEED;
        player.facing = 0;
    }

    // Jumping / Floating
    switch (player.state) {
        case STATE_NORMAL:
        case STATE_FULL:
            if (key_hit(KEY_A)) {
                if (player.on_ground) {
                    player.vy = JUMP_VEL;
                    player.on_ground = 0;
                } else {
                    // Start floating
                    player.state = STATE_FLOATING;
                    player.vy = FLOAT_VEL;
                }
            }
            break;
        case STATE_FLOATING:
            if (key_hit(KEY_A)) {
                player.vy = FLOAT_VEL;
            }
            // B to stop floating
            if (key_hit(KEY_B)) {
                player.state = player.has_enemy ? STATE_FULL : STATE_NORMAL;
            }
            break;
        case STATE_INHALE:
            break;
    }

    // Inhale (B button, only in normal state on ground or air)
    if (player.state == STATE_NORMAL) {
        if (key_is_down(KEY_B)) {
            player.state = STATE_INHALE;
            player.inhale_timer = 30; // inhale duration
        }
    }

    // Spit star (B when full)
    if (player.state == STATE_FULL && key_hit(KEY_B)) {
        // Shoot star
        if (!star.active) {
            star.active = TRUE;
            star.x = player.x + (player.facing ? INT2FX(-8) : INT2FX(16));
            star.y = player.y + INT2FX(4);
            star.vx = player.facing ? -600 : 600;
            star.vy = 0;
            star.life = 45;
        }
        player.has_enemy = FALSE;
        player.state = STATE_NORMAL;
    }

    // Swallow (Down when full)
    if (player.state == STATE_FULL && key_hit(KEY_DOWN)) {
        player.has_enemy = FALSE;
        player.state = STATE_NORMAL;
        // Could gain ability here
    }

    // Update inhale timer
    if (player.state == STATE_INHALE) {
        player.inhale_timer--;
        if (player.inhale_timer <= 0 || !key_is_down(KEY_B)) {
            player.state = player.has_enemy ? STATE_FULL : STATE_NORMAL;
        }
    }

    // Apply gravity
    if (player.state == STATE_FLOATING) {
        player.vy += FLOAT_GRAVITY;
        if (player.vy > FLOAT_MAX_FALL) player.vy = FLOAT_MAX_FALL;
    } else {
        player.vy += GRAVITY;
        if (player.vy > MAX_FALL_SPEED) player.vy = MAX_FALL_SPEED;
    }

    // Update position
    player.x += player.vx;
    player.y += player.vy;

    // Tilemap collision
    collide_player_map();

    // Update invincibility
    if (player.invincible > 0) {
        player.invincible--;
    }

    // Animation
    player.anim_timer++;
    if (player.state == STATE_FLOATING) {
        player.anim_frame = (player.anim_timer / 8) % 2 ? ANIM_FLOAT1 : ANIM_FLOAT2;
    } else if (player.state == STATE_INHALE) {
        player.anim_frame = ANIM_INHALE;
    } else if (player.state == STATE_FULL) {
        player.anim_frame = ANIM_FULL;
    } else if (!player.on_ground) {
        player.anim_frame = ANIM_JUMP;
    } else if (player.vx != 0) {
        player.anim_frame = (player.anim_timer / 6) % 2 ? ANIM_WALK1 : ANIM_WALK2;
    } else {
        player.anim_frame = ANIM_IDLE;
    }
}

static void update_enemies(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].alive) continue;

        // Simple patrol AI
        int ex = FX2INT(enemies[i].x);

        // Patrol range
        if (ex <= enemies[i].patrol_left) {
            enemies[i].dir = 0;
            enemies[i].vx = ENEMY_SPEED;
        } else if (ex >= enemies[i].patrol_right) {
            enemies[i].dir = 1;
            enemies[i].vx = -ENEMY_SPEED;
        }

        // Apply simple gravity
        int bottom = FX2INT(enemies[i].y) + ENEMY_H;
        int ty = bottom / TILE_SIZE;
        int tx = (FX2INT(enemies[i].x) + ENEMY_W / 2) / TILE_SIZE;
        if (!is_solid(tx, ty) && !is_platform(tx, ty)) {
            enemies[i].y += INT2FX(1); // fall slowly
        }

        enemies[i].x += enemies[i].vx;

        collide_enemy_map(&enemies[i]);

        enemies[i].anim_timer++;
    }
}

static void update_star(void) {
    if (!star.active) return;

    star.x += star.vx;
    star.y += star.vy;
    star.life--;

    if (star.life <= 0) {
        star.active = FALSE;
        return;
    }

    // Check collision with walls
    int sx = FX2INT(star.x);
    int sy = FX2INT(star.y);
    int tx = sx / TILE_SIZE;
    int ty = sy / TILE_SIZE;
    if (is_solid(tx, ty)) {
        star.active = FALSE;
        return;
    }

    // Check collision with enemies
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].alive) continue;
        int ex = FX2INT(enemies[i].x);
        int ey = FX2INT(enemies[i].y);
        if (aabb_check(sx, sy, 8, 8, ex, ey, ENEMY_W, ENEMY_H)) {
            enemies[i].alive = FALSE;
            star.active = FALSE;
            game.score += 100;
            break;
        }
    }
}

static void check_player_enemy_collision(void) {
    if (player.invincible > 0 || !player.alive) return;

    int px = FX2INT(player.x);
    int py = FX2INT(player.y);

    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].alive) continue;

        int ex = FX2INT(enemies[i].x);
        int ey = FX2INT(enemies[i].y);

        // Inhale check
        if (player.state == STATE_INHALE) {
            int inhale_x = player.facing ? (px - INHALE_RANGE) : (px + PLAYER_W);
            int inhale_y = py;
            if (aabb_check(inhale_x, inhale_y, INHALE_W, INHALE_H,
                          ex, ey, ENEMY_W, ENEMY_H)) {
                // Suck enemy toward player
                int pull = player.facing ? 200 : -200;
                enemies[i].x -= pull;

                // Check if close enough to swallow
                int edx = FX2INT(enemies[i].x) - px;
                if (edx < 0) edx = -edx;
                if (edx < 12) {
                    enemies[i].alive = FALSE;
                    player.has_enemy = TRUE;
                    player.state = STATE_FULL;
                    game.score += 50;
                }
                continue;
            }
        }

        // Regular collision (damage)
        if (aabb_check(px, py, PLAYER_W, PLAYER_H,
                      ex, ey, ENEMY_W, ENEMY_H)) {
            player.health--;
            player.invincible = 60; // 1 second of invincibility
            player.vy = JUMP_VEL / 2; // knockback

            if (player.health <= 0) {
                player.alive = FALSE;
            }
        }
    }
}

static void update_camera(void) {
    // Camera follows player with some smoothing
    int target_x = FX2INT(player.x) - SCREEN_W / 2 + PLAYER_W / 2;
    int target_y = FX2INT(player.y) - SCREEN_H / 2 + PLAYER_H / 2;

    // Clamp camera
    if (target_x < 0) target_x = 0;
    if (target_x > MAP_W * TILE_SIZE - SCREEN_W)
        target_x = MAP_W * TILE_SIZE - SCREEN_W;
    if (target_y < 0) target_y = 0;
    if (target_y > MAP_H * TILE_SIZE - SCREEN_H)
        target_y = MAP_H * TILE_SIZE - SCREEN_H;

    // Smooth scrolling
    game.cam_x += (target_x - game.cam_x) / 4;
    game.cam_y += (target_y - game.cam_y) / 4;

    // Set BG scroll
    REG_BG0HOFS = game.cam_x;
    REG_BG0VOFS = game.cam_y;

    // Parallax for BG1 (half speed)
    REG_BG1HOFS = game.cam_x / 2;
    REG_BG1VOFS = game.cam_y / 2;
}

// ============================================================
// RENDERING
// ============================================================

static void render_player(void) {
    if (!player.alive) {
        obj_hide(&obj_mem[OAM_KIRBY]);
        return;
    }

    int screen_x = FX2INT(player.x) - game.cam_x;
    int screen_y = FX2INT(player.y) - game.cam_y;

    // Blinking when invincible
    if (player.invincible > 0 && (player.invincible & 2)) {
        obj_hide(&obj_mem[OAM_KIRBY]);
        return;
    }

    // Calculate tile index based on animation frame
    int tile_id = SPR_KIRBY_BASE + (player.anim_frame * 4);

    OBJ_ATTR *obj = &obj_mem[OAM_KIRBY];

    obj_set_attr(obj,
        ATTR0_SQUARE | ATTR0_4BPP | ((screen_y & 0xFF)),
        ATTR1_SIZE_16 | ((player.facing) ? ATTR1_HFLIP : 0) | ((screen_x & 0x1FF)),
        ATTR2_ID(tile_id) | ATTR2_PRIO(0) | ATTR2_PALBANK(0));
}

static void render_enemies(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        OBJ_ATTR *obj = &obj_mem[OAM_ENEMY_START + i];

        if (!enemies[i].alive) {
            obj_hide(obj);
            continue;
        }

        int screen_x = FX2INT(enemies[i].x) - game.cam_x;
        int screen_y = FX2INT(enemies[i].y) - game.cam_y;

        // Off screen check
        if (screen_x < -16 || screen_x > SCREEN_W + 16 ||
            screen_y < -16 || screen_y > SCREEN_H + 16) {
            obj_hide(obj);
            continue;
        }

        obj_set_attr(obj,
            ATTR0_SQUARE | ATTR0_4BPP | ((screen_y & 0xFF)),
            ATTR1_SIZE_16 | ((enemies[i].dir) ? ATTR1_HFLIP : 0) | ((screen_x & 0x1FF)),
            ATTR2_ID(SPR_ENEMY_BASE) | ATTR2_PRIO(0) | ATTR2_PALBANK(0));
    }
}

static void render_star(void) {
    OBJ_ATTR *obj = &obj_mem[OAM_STAR];

    if (!star.active) {
        obj_hide(obj);
        return;
    }

    int screen_x = FX2INT(star.x) - game.cam_x;
    int screen_y = FX2INT(star.y) - game.cam_y;

    if (screen_x < -16 || screen_x > SCREEN_W + 16) {
        obj_hide(obj);
        return;
    }

    obj_set_attr(obj,
        ATTR0_SQUARE | ATTR0_4BPP | ((screen_y & 0xFF)),
        ATTR1_SIZE_16 | ((screen_x & 0x1FF)),
        ATTR2_ID(SPR_STAR_BASE) | ATTR2_PRIO(0) | ATTR2_PALBANK(0));
}

static void render_hud(void) {
    // Draw health hearts using sprites
    for (int i = 0; i < KIRBY_HEALTH; i++) {
        OBJ_ATTR *obj = &obj_mem[OAM_HUD_START + i];

        if (i < player.health) {
            obj_set_attr(obj,
                ATTR0_SQUARE | ATTR0_4BPP | (4),  // y=4
                ATTR1_SIZE_8 | ((4 + i * 10) & 0x1FF),  // x spaced
                ATTR2_ID(SPR_HUD_BASE) | ATTR2_PRIO(0) | ATTR2_PALBANK(0));
        } else {
            obj_hide(obj);
        }
    }
}

// ============================================================
// MAIN GAME LOOP
// ============================================================

int main(void) {
    // Initialize
    irq_init(NULL);
    irq_enable(II_VBLANK);

    init_gfx();
    init_game();

    // Main loop
    while (1) {
        // Wait for VBlank
        VBlankIntrWait();

        // Update
        game.frame_count++;

        key_poll();

        // Reset game on start
        if (key_hit(KEY_START)) {
            if (!player.alive) {
                init_game();
                build_bg_map();
                continue;
            }
            game.paused = !game.paused;
        }

        if (!game.paused && player.alive) {
            update_player();
            update_enemies();
            update_star();
            check_player_enemy_collision();
            update_camera();
        }

        // Render
        render_player();
        render_enemies();
        render_star();
        render_hud();

        // Copy OAM
        oam_copy(oam_mem, obj_mem, 128);
    }

    return 0;
}
