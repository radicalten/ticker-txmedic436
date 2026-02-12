// Kirby-style 2D Side Scroller for GBA using tonc
// Compile with: gcc -o kirby.gba kirby.c -ltonc (with devkitPro/devkitARM)
// Or use a Makefile with devkitARM toolchain

#include <tonc.h>
#include <string.h>

// ============================================================
// CONSTANTS
// ============================================================
#define FIXED_SHIFT     8
#define FIXED_ONE       (1 << FIXED_SHIFT)
#define FLOAT_TO_FIXED(f) ((int)((f) * FIXED_ONE))
#define FIXED_TO_INT(x)   ((x) >> FIXED_SHIFT)
#define INT_TO_FIXED(x)   ((x) << FIXED_SHIFT)

#define GRAVITY         FLOAT_TO_FIXED(0.25)
#define MAX_FALL_SPEED  FLOAT_TO_FIXED(4.0)
#define WALK_SPEED      FLOAT_TO_FIXED(1.5)
#define RUN_SPEED       FLOAT_TO_FIXED(2.5)
#define JUMP_FORCE      FLOAT_TO_FIXED(-3.5)
#define FLOAT_FORCE     FLOAT_TO_FIXED(-0.5)
#define FLOAT_GRAVITY   FLOAT_TO_FIXED(0.1)
#define MAX_FLOAT_FALL  FLOAT_TO_FIXED(1.0)
#define INHALE_RANGE    48
#define STAR_SPEED      FLOAT_TO_FIXED(3.0)

#define MAP_WIDTH       128
#define MAP_HEIGHT      32
#define TILE_SIZE       8
#define SCREEN_W        240
#define SCREEN_H        160

#define MAX_ENEMIES     12
#define MAX_STARS       4
#define MAX_PARTICLES   16
#define MAX_HEALTH      6
#define INHALE_TIME     30
#define INVULN_TIME     60

// Entity states
#define STATE_IDLE      0
#define STATE_WALK      1
#define STATE_JUMP      2
#define STATE_FALL      3
#define STATE_FLOAT     4
#define STATE_INHALE    5
#define STATE_SWALLOW   6
#define STATE_HURT      7
#define STATE_DEAD      8

// Enemy types
#define ENEMY_WADDLE    0
#define ENEMY_BRONTO    1
#define ENEMY_SPARKY    2
#define ENEMY_NONE      255

// Copy abilities
#define ABILITY_NONE    0
#define ABILITY_FIRE    1
#define ABILITY_SPARK   2
#define ABILITY_SWORD   3

// Sprite OAM indices
#define OAM_KIRBY       0
#define OAM_ENEMIES     1
#define OAM_STARS       (OAM_ENEMIES + MAX_ENEMIES)
#define OAM_PARTICLES   (OAM_STARS + MAX_STARS)
#define OAM_HUD         (OAM_PARTICLES + MAX_PARTICLES)

// ============================================================
// DATA STRUCTURES
// ============================================================
typedef struct {
    int x, y;           // Fixed point position
    int vx, vy;         // Fixed point velocity
    int w, h;           // Pixel dimensions
    int state;
    int facing;         // 0=right, 1=left
    int anim_frame;
    int anim_timer;
    int health;
    int ability;
    int inhale_timer;
    int invuln_timer;
    int has_enemy;      // Has inhaled enemy
    int inhaled_type;   // Type of inhaled enemy
    int on_ground;
    int float_puffs;    // Number of float jumps remaining
    int attack_timer;
} Player;

typedef struct {
    int x, y;
    int vx, vy;
    int w, h;
    u8 type;
    u8 active;
    u8 health;
    int anim_frame;
    int anim_timer;
    int facing;
    int on_ground;
    int state_timer;
    int being_inhaled;
} Enemy;

typedef struct {
    int x, y;
    int vx, vy;
    u8 active;
    int life;
    u8 type; // 0 = star spit, 1 = fire, 2 = spark
} Projectile;

typedef struct {
    int x, y;
    int vx, vy;
    int life;
    u8 active;
    u16 color;
} Particle;

typedef struct {
    int x, y; // Camera position (pixels)
} Camera;

// ============================================================
// GLOBALS
// ============================================================
Player kirby;
Enemy enemies[MAX_ENEMIES];
Projectile stars[MAX_STARS];
Particle particles[MAX_PARTICLES];
Camera cam;
int score;
int level_complete;
int game_state; // 0=title, 1=play, 2=gameover, 3=win
int frame_count;

// ============================================================
// TILE DATA - 4bpp sprite tiles
// ============================================================

// Kirby sprite (16x16) - idle frame
// Pink round character
static const u32 kirby_idle_tiles[] = {
    // Tile 0 (top-left of 16x16)
    0x00000000, 0x00001100, 0x00011110, 0x00111110,
    0x01111110, 0x01112110, 0x01111110, 0x01111110,
    // Tile 1 (top-right)
    0x00000000, 0x00110000, 0x01111000, 0x01111100,
    0x01111100, 0x01121100, 0x01111100, 0x01111100,
    // Tile 2 (bottom-left)
    0x01111110, 0x01133110, 0x00133100, 0x00111100,
    0x00011000, 0x00111100, 0x00110100, 0x00000000,
    // Tile 3 (bottom-right)
    0x01111100, 0x01133100, 0x00133000, 0x00111000,
    0x00011000, 0x00111100, 0x00101100, 0x00000000,
};

// Kirby walking frame
static const u32 kirby_walk_tiles[] = {
    0x00000000, 0x00001100, 0x00011110, 0x00111110,
    0x01111110, 0x01112110, 0x01111110, 0x01111110,
    0x00000000, 0x00110000, 0x01111000, 0x01111100,
    0x01111100, 0x01121100, 0x01111100, 0x01111100,
    0x01111110, 0x01133110, 0x00133100, 0x00111100,
    0x00111100, 0x00110100, 0x00100000, 0x00000000,
    0x01111100, 0x01133100, 0x00133000, 0x00111000,
    0x00111000, 0x00010100, 0x00001000, 0x00000000,
};

// Kirby floating (puffed up)
static const u32 kirby_float_tiles[] = {
    0x00011100, 0x00111110, 0x01111110, 0x01111111,
    0x11111111, 0x11121111, 0x11111111, 0x11111111,
    0x00111000, 0x01111100, 0x01111110, 0x11111110,
    0x11111110, 0x11121110, 0x11111110, 0x11111110,
    0x11111111, 0x11133111, 0x01133110, 0x01111110,
    0x00111110, 0x00011100, 0x00001100, 0x00000000,
    0x11111110, 0x11133110, 0x01133100, 0x01111100,
    0x01111000, 0x00111000, 0x00110000, 0x00000000,
};

// Kirby inhaling
static const u32 kirby_inhale_tiles[] = {
    0x00000000, 0x00001100, 0x00011110, 0x00111110,
    0x01111110, 0x01112110, 0x01111110, 0x01111110,
    0x00000000, 0x00110000, 0x01111000, 0x01111100,
    0x01111100, 0x01121100, 0x01111100, 0x01444400,
    0x01111110, 0x01144110, 0x00144100, 0x00111100,
    0x00011000, 0x00111100, 0x00110100, 0x00000000,
    0x04444400, 0x04444400, 0x00444000, 0x00111000,
    0x00011000, 0x00111100, 0x00101100, 0x00000000,
};

// Waddle Dee (enemy) 16x16
static const u32 waddle_dee_tiles[] = {
    0x00000000, 0x00011000, 0x00111100, 0x01555510,
    0x01525510, 0x01555510, 0x00555100, 0x00555100,
    0x00000000, 0x00011000, 0x00111100, 0x01555510,
    0x01552510, 0x01555510, 0x00555100, 0x00555100,
    0x01555510, 0x01555510, 0x00555100, 0x00111100,
    0x00100100, 0x00110110, 0x00000000, 0x00000000,
    0x01555510, 0x01555510, 0x00555100, 0x00111100,
    0x00100100, 0x00110110, 0x00000000, 0x00000000,
};

// Bronto Burt (flying enemy) 16x16
static const u32 bronto_burt_tiles[] = {
    0x01000010, 0x01100110, 0x01110110, 0x00111100,
    0x00666600, 0x06626260, 0x06666660, 0x06666660,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x06666660, 0x06666660, 0x00666600, 0x00066000,
    0x00066000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Star projectile 8x8
static const u32 star_tile[] = {
    0x00070000, 0x00070000, 0x07777700, 0x00777000,
    0x00777000, 0x07070700, 0x07000700, 0x00000000,
};

// Particle 8x8
static const u32 particle_tile[] = {
    0x00000000, 0x00770000, 0x07770000, 0x07770000,
    0x00770000, 0x00000000, 0x00000000, 0x00000000,
};

// Health icon 8x8
static const u32 health_tile[] = {
    0x00000000, 0x06600660, 0x06666660, 0x06666660,
    0x00666600, 0x00066000, 0x00000000, 0x00000000,
};

// Ability icon 8x8
static const u32 ability_tile[] = {
    0x07777700, 0x70000070, 0x70077070, 0x70077070,
    0x70000070, 0x70077070, 0x70000070, 0x07777700,
};

// ============================================================
// BACKGROUND TILE DATA
// ============================================================

// Solid block tile 8x8
static const u32 solid_tile_data[] = {
    0x88888888, 0x89999998, 0x89AAAA98, 0x89AAAA98,
    0x89AAAA98, 0x89AAAA98, 0x89999998, 0x88888888,
};

// Ground surface tile
static const u32 ground_surface_data[] = {
    0x33333333, 0x3BBB3BBB, 0x88888888, 0x89999998,
    0x89AAAA98, 0x89AAAA98, 0x89999998, 0x88888888,
};

// Sky tile (empty)
static const u32 sky_tile_data[] = {
    0xCCCCCCCC, 0xCCCCCCCC, 0xCCCCCCCC, 0xCCCCCCCC,
    0xCCCCCCCC, 0xCCCCCCCC, 0xCCCCCCCC, 0xCCCCCCCC,
};

// Cloud tile
static const u32 cloud_tile_data[] = {
    0xCCCCCCCC, 0xCCCFFCCC, 0xCCFFFFCC, 0xCFFFFFFC,
    0xCFFFFFFC, 0xCCFFFFCC, 0xCCCCCCCC, 0xCCCCCCCC,
};

// Platform tile (one-way)
static const u32 platform_tile_data[] = {
    0x77777777, 0x77777777, 0xCCCCCCCC, 0xCCCCCCCC,
    0xCCCCCCCC, 0xCCCCCCCC, 0xCCCCCCCC, 0xCCCCCCCC,
};

// Door/goal tile
static const u32 door_tile_data[] = {
    0xDDDDDDDD, 0xD000000D, 0xD000000D, 0xD000000D,
    0xD000000D, 0xD000000D, 0xD00D000D, 0xDDDDDDDD,
};

// ============================================================
// LEVEL MAP DATA
// ============================================================
// 0 = empty, 1 = solid, 2 = surface (top of ground), 3 = platform, 4 = door
// Map is MAP_WIDTH x MAP_HEIGHT tiles

static const u8 level_map[MAP_HEIGHT][MAP_WIDTH] = {
    // Row 0-19: mostly sky
    {0},  // rows 0-17 are all zeros (sky)
    {0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},
    // Row 18: some floating platforms
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 3,3,3,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,3,3,3,0,0,
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,3,3,3,0,0,0,0,
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0},
    // Row 19: empty
    {0},
    // Row 20: more platforms
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0, 0,0,0,0,0,0,0,0,0,0,
     3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,3,3,3,
     0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0},
    // Rows 21-23: empty
    {0},{0},{0},
    // Row 24: high ground platforms
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0},
    // Row 25: below high ground
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0},
    // Row 26: surface row with gaps
    {2,2,2,2,2,2,2,2,2,2,2,2,0,0,2,2,2,2,2,2, 2,2,2,2,2,2,0,0,0,2,
     2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,0,0, 0,2,2,2,2,2,2,2,2,2,
     2,2,2,2,2,2,2,2,2,2,0,0,2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,2,2,
     2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,4,0,
     0,0,0,0,0,0,0,0},
    // Row 27-31: solid underground
    {1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,1,1,1,1,1, 1,1,1,1,1,1,0,0,0,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0, 0,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,0,0,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,0,
     0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,1,1,1,1,1, 1,1,1,1,1,1,0,0,0,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0, 0,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,0,0,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,0,
     0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
     1,1,1,1,1,1,1,1},
};

// Enemy spawn data: {tile_x, tile_y, type}
static const u8 enemy_spawns[][3] = {
    {10, 25, ENEMY_WADDLE},
    {18, 25, ENEMY_WADDLE},
    {25, 17, ENEMY_BRONTO},
    {33, 25, ENEMY_WADDLE},
    {40, 17, ENEMY_BRONTO},
    {45, 25, ENEMY_WADDLE},
    {55, 25, ENEMY_SPARKY},
    {63, 25, ENEMY_WADDLE},
    {70, 17, ENEMY_BRONTO},
    {80, 25, ENEMY_WADDLE},
    {90, 25, ENEMY_SPARKY},
    {100, 25, ENEMY_WADDLE},
};
#define NUM_ENEMY_SPAWNS (sizeof(enemy_spawns) / sizeof(enemy_spawns[0]))

// ============================================================
// PALETTE DATA
// ============================================================
static const u16 sprite_pal[] = {
    CLR_MAGENTA,        // 0: transparent (magenta key)
    RGB15(31,20,25),    // 1: Kirby pink
    RGB15(5,5,20),      // 2: Kirby eye (dark blue)
    RGB15(31,10,10),    // 3: Kirby mouth/feet (red)
    RGB15(31,31,15),    // 4: Inhale effect (yellow)
    RGB15(25,15,5),     // 5: Waddle Dee orange
    RGB15(28,15,28),    // 6: Bronto purple
    RGB15(31,31,0),     // 7: Star yellow
    RGB15(20,20,20),    // 8: gray
    RGB15(31,31,31),    // 9: white
    RGB15(0,0,31),      // A: blue
    RGB15(15,31,15),    // B: green
    RGB15(20,25,31),    // C: sky blue
    RGB15(22,12,5),     // D: door brown
    RGB15(31,15,0),     // E: orange/fire
    RGB15(0,31,31),     // F: cyan/spark
};

static const u16 bg_pal[] = {
    RGB15(12,20,31),    // 0: sky blue bg
    RGB15(0,0,0),       // 1: unused
    RGB15(0,0,0),       // 2: unused
    RGB15(10,25,5),     // 3: grass green
    RGB15(0,0,0),       // 4: unused
    RGB15(0,0,0),       // 5: unused
    RGB15(0,0,0),       // 6: unused
    RGB15(20,15,10),    // 7: platform brown
    RGB15(15,10,5),     // 8: dirt dark
    RGB15(18,13,8),     // 9: dirt medium
    RGB15(22,17,12),    // A: dirt light
    RGB15(12,30,8),     // B: grass light
    RGB15(20,25,31),    // C: sky
    RGB15(22,12,5),     // D: door brown
    RGB15(31,15,0),     // E: unused
    RGB15(31,31,31),    // F: cloud white
};

// ============================================================
// FUNCTION PROTOTYPES
// ============================================================
void init_game(void);
void init_gfx(void);
void load_sprites(void);
void load_bg(void);
void build_map(void);
void init_player(void);
void spawn_enemies(void);
void update_player(void);
void update_enemies(void);
void update_projectiles(void);
void update_particles(void);
void update_camera(void);
void render_sprites(void);
void render_hud(void);
int  tile_at(int tx, int ty);
int  is_solid(int tx, int ty);
int  check_collision(int x, int y, int w, int h, int ox, int oy, int ow, int oh);
void spawn_particle(int x, int y, int vx, int vy, u16 color, int life);
void spawn_star(int x, int y, int dir, int type);
void damage_player(void);
void kill_enemy(int i);
int  ability_from_enemy(int type);
void use_ability(void);
void game_title(void);
void game_play(void);
void game_over(void);
void game_win(void);

// ============================================================
// HELPER FUNCTIONS
// ============================================================

int tile_at(int tx, int ty) {
    if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT)
        return (ty >= MAP_HEIGHT) ? 1 : 0; // below map = solid
    return level_map[ty][tx];
}

int is_solid(int tx, int ty) {
    int t = tile_at(tx, ty);
    return (t == 1 || t == 2);
}

int is_platform(int tx, int ty) {
    return tile_at(tx, ty) == 3;
}

int check_collision(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2) {
    return (x1 < x2 + w2 && x1 + w1 > x2 && y1 < y2 + h2 && y1 + h1 > y2);
}

// ============================================================
// INITIALIZATION
// ============================================================

void init_gfx(void) {
    // Set video mode: Mode 0, enable BG0 and OBJ, 1D OBJ mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG0: Main tilemap (foreground)
    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_SIZE(BG_SIZE3) | BG_PRIO(1);
    // BG_SIZE3 = 64x64 tiles = 512x512 pixels

    // BG1: Parallax background
    REG_BG1CNT = BG_CBB(1) | BG_SBB(24) | BG_SIZE(BG_SIZE0) | BG_PRIO(2);

    // Load background palette
    memcpy(pal_bg_mem, bg_pal, sizeof(bg_pal));

    // Load sprite palette
    memcpy(pal_obj_mem, sprite_pal, sizeof(sprite_pal));

    // Initialize OAM
    oam_init(obj_mem, 128);
}

void load_sprites(void) {
    // Load sprite tiles into OBJ VRAM
    // Each 16x16 4bpp sprite = 4 tiles = 128 bytes

    // Tile index 0-3: Kirby idle
    memcpy(&tile_mem_obj[0][0], kirby_idle_tiles, sizeof(kirby_idle_tiles));
    // Tile index 4-7: Kirby walk
    memcpy(&tile_mem_obj[0][4], kirby_walk_tiles, sizeof(kirby_walk_tiles));
    // Tile index 8-11: Kirby float
    memcpy(&tile_mem_obj[0][8], kirby_float_tiles, sizeof(kirby_float_tiles));
    // Tile index 12-15: Kirby inhale
    memcpy(&tile_mem_obj[0][12], kirby_inhale_tiles, sizeof(kirby_inhale_tiles));
    // Tile index 16-19: Waddle Dee
    memcpy(&tile_mem_obj[0][16], waddle_dee_tiles, sizeof(waddle_dee_tiles));
    // Tile index 20-23: Bronto Burt
    memcpy(&tile_mem_obj[0][20], bronto_burt_tiles, sizeof(bronto_burt_tiles));
    // Tile index 24: Star
    memcpy(&tile_mem_obj[0][24], star_tile, sizeof(star_tile));
    // Tile index 25: Particle
    memcpy(&tile_mem_obj[0][25], particle_tile, sizeof(particle_tile));
    // Tile index 26: Health
    memcpy(&tile_mem_obj[0][26], health_tile, sizeof(health_tile));
    // Tile index 27: Ability icon
    memcpy(&tile_mem_obj[0][27], ability_tile, sizeof(ability_tile));
}

void load_bg(void) {
    // Load BG tiles into charblock 0
    // Tile 0: empty/sky
    memcpy(&tile_mem[0][0], sky_tile_data, sizeof(sky_tile_data));
    // Tile 1: solid block
    memcpy(&tile_mem[0][1], solid_tile_data, sizeof(solid_tile_data));
    // Tile 2: ground surface
    memcpy(&tile_mem[0][2], ground_surface_data, sizeof(ground_surface_data));
    // Tile 3: platform
    memcpy(&tile_mem[0][3], platform_tile_data, sizeof(platform_tile_data));
    // Tile 4: door
    memcpy(&tile_mem[0][4], door_tile_data, sizeof(door_tile_data));
    // Tile 5: cloud
    memcpy(&tile_mem[0][5], cloud_tile_data, sizeof(cloud_tile_data));

    // Load BG1 tiles (parallax sky)
    memcpy(&tile_mem[1][0], sky_tile_data, sizeof(sky_tile_data));
    memcpy(&tile_mem[1][1], cloud_tile_data, sizeof(cloud_tile_data));
}

void build_map(void) {
    // Build the BG0 screenblock map
    // BG_SIZE3 uses 4 screenblocks (28,29,30,31) for 64x64 tiles
    // Screenblock layout for 64x64:
    // SBB+0 = top-left 32x32, SBB+1 = top-right 32x32
    // SBB+2 = bottom-left 32x32, SBB+3 = bottom-right 32x32

    int tx, ty;
    for (ty = 0; ty < MAP_HEIGHT; ty++) {
        for (tx = 0; tx < MAP_WIDTH; tx++) {
            int tile = level_map[ty][tx];
            // Determine which screenblock and position
            int sbb_x = tx / 32;
            int sbb_y = ty / 32;
            int sbb = 28 + sbb_x + sbb_y * 2;
            int local_x = tx % 32;
            int local_y = ty % 32;

            se_mem[sbb][local_y * 32 + local_x] = tile;
        }
    }

    // Fill remaining with sky (tile 0)
    // The map is 128 wide but BG is only 64 tiles wide in SIZE3
    // We'll just use what fits - actually MAP_WIDTH=128 but SIZE3=64x64
    // Let's use SIZE3 which is 64x64 = 512x512 pixels
    // But our map is 128 tiles wide = 1024 pixels
    // We need to handle scrolling manually since BG wraps at 512

    // Actually for a proper scrolling map larger than BG size,
    // we'd need to update the tilemap dynamically as camera moves.
    // Let's do that in the update loop.

    // Build BG1 parallax (just clouds)
    for (ty = 0; ty < 32; ty++) {
        for (tx = 0; tx < 32; tx++) {
            int tile = 0; // sky
            if (ty >= 3 && ty <= 5) {
                if ((tx % 12) >= 2 && (tx % 12) <= 5)
                    tile = 1; // cloud
            }
            if (ty >= 8 && ty <= 10) {
                if ((tx % 16) >= 7 && (tx % 16) <= 11)
                    tile = 1; // cloud
            }
            se_mem[24][ty * 32 + tx] = tile;
        }
    }
}

// Dynamic map update - copies visible portion of level_map to BG screenblocks
void update_bg_scroll(void) {
    int cam_tx = cam.x / TILE_SIZE;
    int cam_ty = cam.y / TILE_SIZE;

    // We need to update tiles around the edges as camera moves
    // Update a column of tiles at the leading edge
    int tx, ty;
    int view_w = (SCREEN_W / TILE_SIZE) + 2;
    int view_h = (SCREEN_H / TILE_SIZE) + 2;

    for (ty = cam_ty; ty < cam_ty + view_h && ty < MAP_HEIGHT; ty++) {
        for (tx = cam_tx; tx < cam_tx + view_w && tx < MAP_WIDTH; tx++) {
            int map_tile = level_map[ty][tx];
            // Map tx,ty to BG tile position (wrapping)
            int bg_x = tx % 64;
            int bg_y = ty % 64;
            int sbb_x = bg_x / 32;
            int sbb_y = bg_y / 32;
            int sbb = 28 + sbb_x + sbb_y * 2;
            int local_x = bg_x % 32;
            int local_y = bg_y % 32;

            se_mem[sbb][local_y * 32 + local_x] = map_tile;
        }
    }

    // Set BG scroll
    REG_BG0HOFS = cam.x;
    REG_BG0VOFS = cam.y;

    // Parallax BG scrolls slower
    REG_BG1HOFS = cam.x / 3;
    REG_BG1VOFS = cam.y / 3;
}

void init_player(void) {
    kirby.x = INT_TO_FIXED(16);
    kirby.y = INT_TO_FIXED(25 * 8 - 16);
    kirby.vx = 0;
    kirby.vy = 0;
    kirby.w = 12;
    kirby.h = 14;
    kirby.state = STATE_IDLE;
    kirby.facing = 0;
    kirby.anim_frame = 0;
    kirby.anim_timer = 0;
    kirby.health = MAX_HEALTH;
    kirby.ability = ABILITY_NONE;
    kirby.inhale_timer = 0;
    kirby.invuln_timer = 0;
    kirby.has_enemy = 0;
    kirby.inhaled_type = ENEMY_NONE;
    kirby.on_ground = 0;
    kirby.float_puffs = 5;
    kirby.attack_timer = 0;
}

void spawn_enemies(void) {
    int i;
    for (i = 0; i < MAX_ENEMIES && i < (int)NUM_ENEMY_SPAWNS; i++) {
        enemies[i].active = 1;
        enemies[i].type = enemy_spawns[i][2];
        enemies[i].x = INT_TO_FIXED(enemy_spawns[i][0] * TILE_SIZE);
        enemies[i].y = INT_TO_FIXED(enemy_spawns[i][1] * TILE_SIZE - 16);
        enemies[i].vx = FLOAT_TO_FIXED(0.5);
        enemies[i].vy = 0;
        enemies[i].w = 12;
        enemies[i].h = 14;
        enemies[i].health = 1;
        enemies[i].anim_frame = 0;
        enemies[i].anim_timer = 0;
        enemies[i].facing = 0;
        enemies[i].on_ground = 0;
        enemies[i].state_timer = 0;
        enemies[i].being_inhaled = 0;

        if (enemies[i].type == ENEMY_BRONTO) {
            enemies[i].vy = FLOAT_TO_FIXED(0.3);
        }
    }
    for (; i < MAX_ENEMIES; i++) {
        enemies[i].active = 0;
    }
}

void init_game(void) {
    init_gfx();
    load_sprites();
    load_bg();
    build_map();
    init_player();
    spawn_enemies();

    // Clear projectiles and particles
    int i;
    for (i = 0; i < MAX_STARS; i++) stars[i].active = 0;
    for (i = 0; i < MAX_PARTICLES; i++) particles[i].active = 0;

    cam.x = 0;
    cam.y = 0;
    score = 0;
    level_complete = 0;
    frame_count = 0;
}

// ============================================================
// PLAYER UPDATE
// ============================================================

void update_player(void) {
    if (kirby.state == STATE_DEAD) return;

    // Invulnerability timer
    if (kirby.invuln_timer > 0) kirby.invuln_timer--;
    if (kirby.attack_timer > 0) kirby.attack_timer--;

    // Input
    int move_speed = (key_is_down(KEY_B)) ? RUN_SPEED : WALK_SPEED;
    int moving = 0;

    if (kirby.state != STATE_HURT) {
        // Horizontal movement
        if (key_is_down(KEY_LEFT)) {
            kirby.vx = -move_speed;
            kirby.facing = 1;
            moving = 1;
        } else if (key_is_down(KEY_RIGHT)) {
            kirby.vx = move_speed;
            kirby.facing = 0;
            moving = 1;
        } else {
            kirby.vx = 0;
        }

        // Jump
        if (key_hit(KEY_A)) {
            if (kirby.on_ground) {
                kirby.vy = JUMP_FORCE;
                kirby.state = STATE_JUMP;
                kirby.on_ground = 0;
                kirby.float_puffs = 5;
            } else if (kirby.float_puffs > 0 && kirby.state != STATE_INHALE) {
                // Float/fly
                kirby.vy = FLOAT_FORCE;
                kirby.state = STATE_FLOAT;
                kirby.float_puffs--;
                // Spawn puff particle
                spawn_particle(
                    FIXED_TO_INT(kirby.x) + kirby.w/2,
                    FIXED_TO_INT(kirby.y) + kirby.h + 4,
                    0, FLOAT_TO_FIXED(1.0),
                    RGB15(31,31,31), 15
                );
            }
        }

        // Inhale (R button) or use ability
        if (key_is_down(KEY_R) || key_is_down(KEY_SELECT)) {
            if (kirby.ability != ABILITY_NONE) {
                // Use copy ability
                use_ability();
            } else if (!kirby.has_enemy) {
                kirby.state = STATE_INHALE;
                kirby.inhale_timer++;
                kirby.vx = 0; // Can't move while inhaling
            }
        } else {
            if (kirby.state == STATE_INHALE) {
                kirby.state = kirby.on_ground ? STATE_IDLE : STATE_FALL;
                kirby.inhale_timer = 0;
            }
        }

        // Swallow inhaled enemy (Down)
        if (key_hit(KEY_DOWN) && kirby.has_enemy) {
            kirby.ability = ability_from_enemy(kirby.inhaled_type);
            kirby.has_enemy = 0;
            kirby.inhaled_type = ENEMY_NONE;
            // Swallow effect
            int i;
            for (i = 0; i < 4; i++) {
                spawn_particle(
                    FIXED_TO_INT(kirby.x) + kirby.w/2,
                    FIXED_TO_INT(kirby.y) + kirby.h/2,
                    FLOAT_TO_FIXED((qran() % 30 - 15) / 10.0),
                    FLOAT_TO_FIXED(-(qran() % 20) / 10.0),
                    RGB15(31,31,0), 20
                );
            }
        }

        // Spit out enemy as star (B when has enemy)
        if (key_hit(KEY_B) && kirby.has_enemy) {
            int dir = kirby.facing ? -1 : 1;
            int sx = FIXED_TO_INT(kirby.x) + (kirby.facing ? -8 : kirby.w);
            int sy = FIXED_TO_INT(kirby.y) + kirby.h / 2 - 4;
            spawn_star(sx, sy, dir, 0);
            kirby.has_enemy = 0;
            kirby.inhaled_type = ENEMY_NONE;
        }

        // Drop ability (L button)
        if (key_hit(KEY_L) && kirby.ability != ABILITY_NONE) {
            kirby.ability = ABILITY_NONE;
            // Star particles when dropping ability
            int i;
            for (i = 0; i < 6; i++) {
                spawn_particle(
                    FIXED_TO_INT(kirby.x) + kirby.w/2,
                    FIXED_TO_INT(kirby.y) + kirby.h/2,
                    FLOAT_TO_FIXED((qran() % 40 - 20) / 10.0),
                    FLOAT_TO_FIXED(-(qran() % 30) / 10.0),
                    RGB15(31,31,0), 25
                );
            }
        }
    }

    // Apply gravity
    if (kirby.state == STATE_FLOAT) {
        kirby.vy += FLOAT_GRAVITY;
        if (kirby.vy > MAX_FLOAT_FALL) kirby.vy = MAX_FLOAT_FALL;
    } else {
        kirby.vy += GRAVITY;
        if (kirby.vy > MAX_FALL_SPEED) kirby.vy = MAX_FALL_SPEED;
    }

    // Hurt state recovery
    if (kirby.state == STATE_HURT) {
        kirby.vy += GRAVITY;
        if (kirby.vy > MAX_FALL_SPEED) kirby.vy = MAX_FALL_SPEED;
        if (kirby.on_ground && kirby.invuln_timer < INVULN_TIME - 15) {
            kirby.state = STATE_IDLE;
        }
    }

    // Horizontal collision
    int new_x = kirby.x + kirby.vx;
    int px = FIXED_TO_INT(new_x);
    int py = FIXED_TO_INT(kirby.y);

    // Check left/right
    int top_ty = py / TILE_SIZE;
    int bot_ty = (py + kirby.h - 1) / TILE_SIZE;
    int check_tx;

    if (kirby.vx > 0) {
        check_tx = (px + kirby.w) / TILE_SIZE;
        int ty;
        int blocked = 0;
        for (ty = top_ty; ty <= bot_ty; ty++) {
            if (is_solid(check_tx, ty)) {
                new_x = INT_TO_FIXED(check_tx * TILE_SIZE - kirby.w);
                kirby.vx = 0;
                blocked = 1;
                break;
            }
        }
    } else if (kirby.vx < 0) {
        check_tx = px / TILE_SIZE;
        int ty;
        for (ty = top_ty; ty <= bot_ty; ty++) {
            if (is_solid(check_tx, ty)) {
                new_x = INT_TO_FIXED((check_tx + 1) * TILE_SIZE);
                kirby.vx = 0;
                break;
            }
        }
    }
    kirby.x = new_x;

    // Vertical collision
    int new_y = kirby.y + kirby.vy;
    px = FIXED_TO_INT(kirby.x);
    py = FIXED_TO_INT(new_y);

    int left_tx = px / TILE_SIZE;
    int right_tx = (px + kirby.w - 1) / TILE_SIZE;
    int check_ty;
    kirby.on_ground = 0;

    if (kirby.vy > 0) {
        // Falling - check below
        check_ty = (py + kirby.h) / TILE_SIZE;
        int tx;
        for (tx = left_tx; tx <= right_tx; tx++) {
            if (is_solid(tx, check_ty)) {
                new_y = INT_TO_FIXED(check_ty * TILE_SIZE - kirby.h);
                kirby.vy = 0;
                kirby.on_ground = 1;
                kirby.float_puffs = 5;
                if (kirby.state == STATE_FLOAT || kirby.state == STATE_JUMP || kirby.state == STATE_FALL) {
                    kirby.state = STATE_IDLE;
                }
                break;
            }
            // Platform check (only when falling)
            if (is_platform(tx, check_ty)) {
                int plat_top = check_ty * TILE_SIZE;
                int prev_bottom = FIXED_TO_INT(kirby.y) + kirby.h;
                if (prev_bottom <= plat_top) {
                    new_y = INT_TO_FIXED(plat_top - kirby.h);
                    kirby.vy = 0;
                    kirby.on_ground = 1;
                    kirby.float_puffs = 5;
                    if (kirby.state == STATE_FLOAT || kirby.state == STATE_JUMP || kirby.state == STATE_FALL) {
                        kirby.state = STATE_IDLE;
                    }
                    break;
                }
            }
        }
    } else if (kirby.vy < 0) {
        // Rising - check above
        check_ty = py / TILE_SIZE;
        int tx;
        for (tx = left_tx; tx <= right_tx; tx++) {
            if (is_solid(tx, check_ty)) {
                new_y = INT_TO_FIXED((check_ty + 1) * TILE_SIZE);
                kirby.vy = 0;
                break;
            }
        }
    }
    kirby.y = new_y;

    // Update state based on movement
    if (kirby.state != STATE_HURT && kirby.state != STATE_INHALE && kirby.state != STATE_FLOAT) {
        if (!kirby.on_ground) {
            if (kirby.vy < 0)
                kirby.state = STATE_JUMP;
            else
                kirby.state = STATE_FALL;
        } else if (moving) {
            kirby.state = STATE_WALK;
        } else {
            kirby.state = STATE_IDLE;
        }
    }

    // Animation
    kirby.anim_timer++;
    if (kirby.anim_timer >= 8) {
        kirby.anim_timer = 0;
        kirby.anim_frame = (kirby.anim_frame + 1) % 4;
    }

    // Clamp position
    if (kirby.x < 0) { kirby.x = 0; kirby.vx = 0; }
    if (FIXED_TO_INT(kirby.x) > MAP_WIDTH * TILE_SIZE - kirby.w) {
        kirby.x = INT_TO_FIXED(MAP_WIDTH * TILE_SIZE - kirby.w);
    }

    // Fall into pit = death
    if (FIXED_TO_INT(kirby.y) > MAP_HEIGHT * TILE_SIZE + 32) {
        kirby.health = 0;
        kirby.state = STATE_DEAD;
    }

    // Check for door (level complete)
    int kpx = FIXED_TO_INT(kirby.x) + kirby.w / 2;
    int kpy = FIXED_TO_INT(kirby.y) + kirby.h / 2;
    if (tile_at(kpx / TILE_SIZE, kpy / TILE_SIZE) == 4) {
        if (key_hit(KEY_UP)) {
            level_complete = 1;
        }
    }

    // Inhale enemies
    if (kirby.state == STATE_INHALE && !kirby.has_enemy) {
        int i;
        int inhale_x = FIXED_TO_INT(kirby.x) + (kirby.facing ? -INHALE_RANGE : kirby.w);
        int inhale_y = FIXED_TO_INT(kirby.y);
        int inhale_w = INHALE_RANGE;
        int inhale_h = kirby.h;

        for (i = 0; i < MAX_ENEMIES; i++) {
            if (!enemies[i].active) continue;
            int ex = FIXED_TO_INT(enemies[i].x);
            int ey = FIXED_TO_INT(enemies[i].y);

            if (check_collision(inhale_x, inhale_y, inhale_w, inhale_h,
                              ex, ey, enemies[i].w, enemies[i].h)) {
                // Pull enemy toward Kirby
                int pull_dir = (ex < FIXED_TO_INT(kirby.x) + kirby.w/2) ? 1 : -1;
                if (kirby.facing) pull_dir = -pull_dir;
                enemies[i].x += INT_TO_FIXED(pull_dir * 2);
                enemies[i].being_inhaled = 1;

                // Check if close enough to swallow
                int dist = ex - FIXED_TO_INT(kirby.x);
                if (dist < 0) dist = -dist;
                if (dist < 12) {
                    kirby.has_enemy = 1;
                    kirby.inhaled_type = enemies[i].type;
                    enemies[i].active = 0;
                    kirby.state = STATE_IDLE;
                    kirby.inhale_timer = 0;
                }
            } else {
                enemies[i].being_inhaled = 0;
            }
        }
    }
}

void use_ability(void) {
    if (kirby.attack_timer > 0) return;

    int dir = kirby.facing ? -1 : 1;
    int sx = FIXED_TO_INT(kirby.x) + (kirby.facing ? -8 : kirby.w);
    int sy = FIXED_TO_INT(kirby.y) + kirby.h / 2 - 4;

    switch (kirby.ability) {
        case ABILITY_FIRE:
            spawn_star(sx, sy, dir, 1);
            kirby.attack_timer = 8;
            // Fire particles
            spawn_particle(sx, sy, FLOAT_TO_FIXED(dir * 2.0),
                          FLOAT_TO_FIXED(-0.5), RGB15(31,15,0), 10);
            spawn_particle(sx, sy + 4, FLOAT_TO_FIXED(dir * 1.5),
                          FLOAT_TO_FIXED(0.5), RGB15(31,31,0), 8);
            break;
        case ABILITY_SPARK:
            // Spark aura - damage all nearby enemies
            kirby.attack_timer = 15;
            {
                int i;
                int kx = FIXED_TO_INT(kirby.x) + kirby.w/2;
                int ky = FIXED_TO_INT(kirby.y) + kirby.h/2;
                for (i = 0; i < MAX_ENEMIES; i++) {
                    if (!enemies[i].active) continue;
                    int ex = FIXED_TO_INT(enemies[i].x) + enemies[i].w/2;
                    int ey = FIXED_TO_INT(enemies[i].y) + enemies[i].h/2;
                    int dx = kx - ex;
                    int dy = ky - ey;
                    if (dx*dx + dy*dy < 32*32) {
                        kill_enemy(i);
                    }
                }
                // Spark particles
                int j;
                for (j = 0; j < 8; j++) {
                    int angle = j * 32;
                    spawn_particle(kx, ky,
                        FLOAT_TO_FIXED(lu_cos(angle * 256) / 8192.0),
                        FLOAT_TO_FIXED(lu_sin(angle * 256) / 8192.0),
                        RGB15(0,31,31), 12);
                }
            }
            break;
        case ABILITY_SWORD:
            // Sword slash
            kirby.attack_timer = 12;
            {
                int slash_x = FIXED_TO_INT(kirby.x) + (kirby.facing ? -16 : kirby.w);
                int slash_y = FIXED_TO_INT(kirby.y) - 4;
                int i;
                for (i = 0; i < MAX_ENEMIES; i++) {
                    if (!enemies[i].active) continue;
                    int ex = FIXED_TO_INT(enemies[i].x);
                    int ey = FIXED_TO_INT(enemies[i].y);
                    if (check_collision(slash_x, slash_y, 20, kirby.h + 8,
                                      ex, ey, enemies[i].w, enemies[i].h)) {
                        kill_enemy(i);
                    }
                }
                // Sword slash particles
                int j;
                for (j = 0; j < 4; j++) {
                    spawn_particle(slash_x + (kirby.facing ? 0 : 10), slash_y + j*4,
                        FLOAT_TO_FIXED(dir * 3.0), FLOAT_TO_FIXED((j-2)*0.3),
                        RGB15(31,31,31), 8);
                }
            }
            break;
    }
}

int ability_from_enemy(int type) {
    switch (type) {
        case ENEMY_BRONTO: return ABILITY_NONE; // No ability from Bronto
        case ENEMY_SPARKY: return ABILITY_SPARK;
        default: return ABILITY_NONE;
    }
}

void damage_player(void) {
    if (kirby.invuln_timer > 0) return;
    if (kirby.state == STATE_DEAD) return;

    kirby.health--;
    kirby.invuln_timer = INVULN_TIME;

    if (kirby.ability != ABILITY_NONE) {
        kirby.ability = ABILITY_NONE;
        // Don't lose health when losing ability
        kirby.health++;
    }

    if (kirby.health <= 0) {
        kirby.state = STATE_DEAD;
        kirby.vy = JUMP_FORCE;
    } else {
        kirby.state = STATE_HURT;
        kirby.vy = FLOAT_TO_FIXED(-2.0);
        kirby.vx = kirby.facing ? WALK_SPEED : -WALK_SPEED;
    }

    // Damage particles
    int i;
    for (i = 0; i < 4; i++) {
        spawn_particle(
            FIXED_TO_INT(kirby.x) + kirby.w/2,
            FIXED_TO_INT(kirby.y) + kirby.h/2,
            FLOAT_TO_FIXED((qran() % 40 - 20) / 10.0),
            FLOAT_TO_FIXED(-(qran() % 20) / 10.0),
            RGB15(31,0,0), 20
        );
    }
}

// ============================================================
// ENEMY UPDATE
// ============================================================

void update_enemies(void) {
    int i;
    for (i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;

        Enemy *e = &enemies[i];

        // Don't update if being inhaled (handled in player)
        if (e->being_inhaled) continue;

        // AI based on type
        switch (e->type) {
            case ENEMY_WADDLE:
                // Simple walking back and forth
                e->vy += GRAVITY;
                if (e->vy > MAX_FALL_SPEED) e->vy = MAX_FALL_SPEED;

                // Move
                e->x += e->vx;

                // Check wall collision
                {
                    int ex = FIXED_TO_INT(e->x);
                    int ey = FIXED_TO_INT(e->y);
                    int check_x = (e->vx > 0) ? (ex + e->w) / TILE_SIZE : ex / TILE_SIZE;
                    int check_y = (ey + e->h / 2) / TILE_SIZE;
                    if (is_solid(check_x, check_y)) {
                        e->vx = -e->vx;
                        e->facing = (e->vx < 0) ? 1 : 0;
                    }

                    // Check edge (don't walk off platforms)
                    int foot_x = (e->vx > 0) ? (ex + e->w) / TILE_SIZE : ex / TILE_SIZE;
                    int foot_y = (ey + e->h + 2) / TILE_SIZE;
                    if (!is_solid(foot_x, foot_y) && e->on_ground) {
                        e->vx = -e->vx;
                        e->facing = (e->vx < 0) ? 1 : 0;
                    }
                }

                // Vertical collision
                {
                    int new_y = e->y + e->vy;
                    int ex = FIXED_TO_INT(e->x);
                    int ey = FIXED_TO_INT(new_y);
                    int check_y = (ey + e->h) / TILE_SIZE;
                    int check_x = (ex + e->w / 2) / TILE_SIZE;
                    e->on_ground = 0;
                    if (is_solid(check_x, check_y)) {
                        new_y = INT_TO_FIXED(check_y * TILE_SIZE - e->h);
                        e->vy = 0;
                        e->on_ground = 1;
                    }
                    e->y = new_y;
                }
                break;

            case ENEMY_BRONTO:
                // Flying enemy - sine wave movement
                e->state_timer++;
                e->x += e->vx;
                e->y += FLOAT_TO_FIXED(lu_sin(e->state_timer * 512) / 16384.0);

                // Bounce off walls
                {
                    int ex = FIXED_TO_INT(e->x);
                    int ey = FIXED_TO_INT(e->y);
                    int check_x = (e->vx > 0) ? (ex + e->w) / TILE_SIZE : ex / TILE_SIZE;
                    int check_y = ey / TILE_SIZE;
                    if (is_solid(check_x, check_y)) {
                        e->vx = -e->vx;
                        e->facing = (e->vx < 0) ? 1 : 0;
                    }
                }
                break;

            case ENEMY_SPARKY:
                // Hops around
                e->vy += GRAVITY;
                if (e->vy > MAX_FALL_SPEED) e->vy = MAX_FALL_SPEED;

                e->state_timer++;
                if (e->on_ground && e->state_timer > 30) {
                    e->vy = FLOAT_TO_FIXED(-2.5);
                    e->on_ground = 0;
                    e->state_timer = 0;
                    // Random direction
                    e->vx = (qran() & 1) ? WALK_SPEED : -WALK_SPEED;
                    e->facing = (e->vx < 0) ? 1 : 0;
                }

                e->x += e->vx;
                // Wall check
                {
                    int ex = FIXED_TO_INT(e->x);
                    int ey = FIXED_TO_INT(e->y);
                    int check_x = (e->vx > 0) ? (ex + e->w) / TILE_SIZE : ex / TILE_SIZE;
                    int check_y = (ey + e->h / 2) / TILE_SIZE;
                    if (is_solid(check_x, check_y)) {
                        e->vx = -e->vx;
                        e->facing = (e->vx < 0) ? 1 : 0;
                    }
                }

                // Vertical collision
                {
                    int new_y = e->y + e->vy;
                    int ex = FIXED_TO_INT(e->x);
                    int ey = FIXED_TO_INT(new_y);
                    int check_y = (ey + e->h) / TILE_SIZE;
                    int check_x = (ex + e->w / 2) / TILE_SIZE;
                    e->on_ground = 0;
                    if (is_solid(check_x, check_y)) {
                        new_y = INT_TO_FIXED(check_y * TILE_SIZE - e->h);
                        e->vy = 0;
                        e->on_ground = 1;
                        e->vx = 0;
                    }
                    e->y = new_y;
                }
                break;
        }

        // Animation
        e->anim_timer++;
        if (e->anim_timer >= 12) {
            e->anim_timer = 0;
            e->anim_frame = (e->anim_frame + 1) % 2;
        }

        // Check collision with player
        if (kirby.state != STATE_DEAD && kirby.state != STATE_HURT) {
            int kx = FIXED_TO_INT(kirby.x);
            int ky = FIXED_TO_INT(kirby.y);
            int ex = FIXED_TO_INT(e->x);
            int ey = FIXED_TO_INT(e->y);

            if (check_collision(kx, ky, kirby.w, kirby.h, ex, ey, e->w, e->h)) {
                // Check if Kirby is jumping on top
                if (kirby.vy > 0 && ky + kirby.h - 8 < ey) {
                    // Bounce off enemy
                    kirby.vy = FLOAT_TO_FIXED(-2.0);
                    kill_enemy(i);
                } else if (kirby.state != STATE_INHALE) {
                    damage_player();
                }
            }
        }

        // Remove if fallen off map
        if (FIXED_TO_INT(e->y) > MAP_HEIGHT * TILE_SIZE + 32) {
            e->active = 0;
        }
    }
}

void kill_enemy(int i) {
    enemies[i].active = 0;
    score += 100;

    // Death particles
    int ex = FIXED_TO_INT(enemies[i].x) + enemies[i].w / 2;
    int ey = FIXED_TO_INT(enemies[i].y) + enemies[i].h / 2;
    int j;
    for (j = 0; j < 4; j++) {
        spawn_particle(ex, ey,
            FLOAT_TO_FIXED((qran() % 40 - 20) / 10.0),
            FLOAT_TO_FIXED(-(qran() % 30) / 10.0),
            RGB15(31,31,0), 20);
    }
}

// ============================================================
// PROJECTILE UPDATE
// ============================================================

void spawn_star(int x, int y, int dir, int type) {
    int i;
    for (i = 0; i < MAX_STARS; i++) {
        if (!stars[i].active) {
            stars[i].active = 1;
            stars[i].x = INT_TO_FIXED(x);
            stars[i].y = INT_TO_FIXED(y);
            stars[i].vx = STAR_SPEED * dir;
            stars[i].vy = 0;
            stars[i].life = 40;
            stars[i].type = type;
            return;
        }
    }
}

void update_projectiles(void) {
    int i;
    for (i = 0; i < MAX_STARS; i++) {
        if (!stars[i].active) continue;

        stars[i].x += stars[i].vx;
        stars[i].y += stars[i].vy;
        stars[i].life--;

        if (stars[i].life <= 0) {
            stars[i].active = 0;
            continue;
        }

        int sx = FIXED_TO_INT(stars[i].x);
        int sy = FIXED_TO_INT(stars[i].y);

        // Check wall collision
        if (is_solid(sx / TILE_SIZE, sy / TILE_SIZE) ||
            is_solid((sx + 7) / TILE_SIZE, sy / TILE_SIZE)) {
            stars[i].active = 0;
            // Impact particles
            spawn_particle(sx + 4, sy + 4, 0, FLOAT_TO_FIXED(-1.0),
                          RGB15(31,31,0), 10);
            continue;
        }

        // Check enemy collision
        int j;
        for (j = 0; j < MAX_ENEMIES; j++) {
            if (!enemies[j].active) continue;
            int ex = FIXED_TO_INT(enemies[j].x);
            int ey = FIXED_TO_INT(enemies[j].y);
            if (check_collision(sx, sy, 8, 8, ex, ey, enemies[j].w, enemies[j].h)) {
                kill_enemy(j);
                stars[i].active = 0;
                break;
            }
        }
    }
}

// ============================================================
// PARTICLE SYSTEM
// ============================================================

void spawn_particle(int x, int y, int vx, int vy, u16 color, int life) {
    int i;
    for (i = 0; i < MAX_PARTICLES; i++) {
        if (!particles[i].active) {
            particles[i].active = 1;
            particles[i].x = INT_TO_FIXED(x);
            particles[i].y = INT_TO_FIXED(y);
            particles[i].vx = vx;
            particles[i].vy = vy;
            particles[i].color = color;
            particles[i].life = life;
            return;
        }
    }
}

void update_particles(void) {
    int i;
    for (i = 0; i < MAX_PARTICLES; i++) {
        if (!particles[i].active) continue;

        particles[i].x += particles[i].vx;
        particles[i].y += particles[i].vy;
        particles[i].vy += FLOAT_TO_FIXED(0.1); // Slight gravity on particles
        particles[i].life--;

        if (particles[i].life <= 0) {
            particles[i].active = 0;
        }
    }
}

// ============================================================
// CAMERA
// ============================================================

void update_camera(void) {
    // Target: center Kirby on screen
    int target_x = FIXED_TO_INT(kirby.x) - SCREEN_W / 2 + kirby.w / 2;
    int target_y = FIXED_TO_INT(kirby.y) - SCREEN_H / 2 + kirby.h / 2;

    // Smooth follow
    cam.x += (target_x - cam.x) / 8;
    cam.y += (target_y - cam.y) / 8;

    // Clamp
    if (cam.x < 0) cam.x = 0;
    if (cam.y < 0) cam.y = 0;
    if (cam.x > MAP_WIDTH * TILE_SIZE - SCREEN_W)
        cam.x = MAP_WIDTH * TILE_SIZE - SCREEN_W;
    if (cam.y > MAP_HEIGHT * TILE_SIZE - SCREEN_H)
        cam.y = MAP_HEIGHT * TILE_SIZE - SCREEN_H;
}

// ============================================================
// RENDERING
// ============================================================

void render_sprites(void) {
    int i;

    // --- KIRBY ---
    {
        int sx = FIXED_TO_INT(kirby.x) - cam.x - 2; // Offset for 16x16 sprite on 12px hitbox
        int sy = FIXED_TO_INT(kirby.y) - cam.y - 2;

        // Blinking when invulnerable
        int visible = 1;
        if (kirby.invuln_timer > 0 && (kirby.invuln_timer & 2))
            visible = 0;

        if (kirby.state == STATE_DEAD)
            visible = (frame_count & 4) ? 0 : 1;

        if (visible && sx > -16 && sx < SCREEN_W && sy > -16 && sy < SCREEN_H) {
            // Choose tile based on state
            int tile_id = 0; // idle
            switch (kirby.state) {
                case STATE_IDLE:
                    tile_id = (kirby.has_enemy) ? 8 : 0; // Puffed if has enemy
                    break;
                case STATE_WALK:
                    tile_id = (kirby.anim_frame & 1) ? 0 : 4; // Alternate idle/walk
                    break;
                case STATE_JUMP:
                case STATE_FALL:
                    tile_id = 4;
                    break;
                case STATE_FLOAT:
                    tile_id = 8;
                    break;
                case STATE_INHALE:
                    tile_id = 12;
                    break;
                case STATE_HURT:
                    tile_id = 4;
                    break;
                default:
                    tile_id = 0;
                    break;
            }

            u16 attr1_flip = kirby.facing ? ATTR1_HFLIP : 0;
            obj_set_attr(&obj_mem[OAM_KIRBY],
                ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | ((sy & 0xFF)),
                ATTR1_SIZE(1) | attr1_flip | ((sx & 0x1FF)),
                ATTR2_ID(tile_id) | ATTR2_PRIO(1));
        } else {
            obj_hide(&obj_mem[OAM_KIRBY]);
        }
    }

    // --- ENEMIES ---
    for (i = 0; i < MAX_ENEMIES; i++) {
        int oam_id = OAM_ENEMIES + i;
        if (!enemies[i].active) {
            obj_hide(&obj_mem[oam_id]);
            continue;
        }

        int sx = FIXED_TO_INT(enemies[i].x) - cam.x - 2;
        int sy = FIXED_TO_INT(enemies[i].y) - cam.y - 2;

        if (sx > -16 && sx < SCREEN_W && sy > -16 && sy < SCREEN_H) {
            int tile_id = 16; // Waddle Dee
            if (enemies[i].type == ENEMY_BRONTO) tile_id = 20;
            else if (enemies[i].type == ENEMY_SPARKY) tile_id = 16; // Reuse waddle sprite

            u16 attr1_flip = enemies[i].facing ? ATTR1_HFLIP : 0;
            obj_set_attr(&obj_mem[oam_id],
                ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | ((sy & 0xFF)),
                ATTR1_SIZE(1) | attr1_flip | ((sx & 0x1FF)),
                ATTR2_ID(tile_id) | ATTR2_PRIO(1));
        } else {
            obj_hide(&obj_mem[oam_id]);
        }
    }

    // --- PROJECTILES ---
    for (i = 0; i < MAX_STARS; i++) {
        int oam_id = OAM_STARS + i;
        if (!stars[i].active) {
            obj_hide(&obj_mem[oam_id]);
            continue;
        }

        int sx = FIXED_TO_INT(stars[i].x) - cam.x;
        int sy = FIXED_TO_INT(stars[i].y) - cam.y;

        if (sx > -8 && sx < SCREEN_W && sy > -8 && sy < SCREEN_H) {
            obj_set_attr(&obj_mem[oam_id],
                ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | ((sy & 0xFF)),
                ATTR1_SIZE(0) | ((sx & 0x1FF)),
                ATTR2_ID(24) | ATTR2_PRIO(1));
        } else {
            obj_hide(&obj_mem[oam_id]);
        }
    }

    // --- PARTICLES ---
    for (i = 0; i < MAX_PARTICLES; i++) {
        int oam_id = OAM_PARTICLES + i;
        if (!particles[i].active) {
            obj_hide(&obj_mem[oam_id]);
            continue;
        }

        int sx = FIXED_TO_INT(particles[i].x) - cam.x;
        int sy = FIXED_TO_INT(particles[i].y) - cam.y;

        if (sx > -8 && sx < SCREEN_W && sy > -8 && sy < SCREEN_H) {
            obj_set_attr(&obj_mem[oam_id],
                ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | ((sy & 0xFF)),
                ATTR1_SIZE(0) | ((sx & 0x1FF)),
                ATTR2_ID(25) | ATTR2_PRIO(0));
        } else {
            obj_hide(&obj_mem[oam_id]);
        }
    }
}

void render_hud(void) {
    int i;
    int oam_base = OAM_HUD;

    // Health hearts
    for (i = 0; i < MAX_HEALTH; i++) {
        int oam_id = oam_base + i;
        if (oam_id >= 128) break;

        if (i < kirby.health) {
            obj_set_attr(&obj_mem[oam_id],
                ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | (4),
                ATTR1_SIZE(0) | (4 + i * 10),
                ATTR2_ID(26) | ATTR2_PRIO(0));
        } else {
            obj_hide(&obj_mem[oam_id]);
        }
    }

    // Ability icon
    {
        int oam_id = oam_base + MAX_HEALTH;
        if (oam_id < 128) {
            if (kirby.ability != ABILITY_NONE) {
                obj_set_attr(&obj_mem[oam_id],
                    ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | (4),
                    ATTR1_SIZE(0) | (SCREEN_W - 20),
                    ATTR2_ID(27) | ATTR2_PRIO(0));
            } else {
                obj_hide(&obj_mem[oam_id]);
            }
        }
    }

    // Inhaled enemy indicator
    {
        int oam_id = oam_base + MAX_HEALTH + 1;
        if (oam_id < 128) {
            if (kirby.has_enemy) {
                obj_set_attr(&obj_mem[oam_id],
                    ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | (4),
                    ATTR1_SIZE(0) | (SCREEN_W - 32),
                    ATTR2_ID(24) | ATTR2_PRIO(0)); // Star icon to show has enemy
            } else {
                obj_hide(&obj_mem[oam_id]);
            }
        }
    }

    // Hide remaining HUD OAMs
    for (i = oam_base + MAX_HEALTH + 2; i < 128; i++) {
        obj_hide(&obj_mem[i]);
    }
}

// ============================================================
// GAME STATES
// ============================================================

void game_title(void) {
    // Simple title screen using BG text
    // We'll use a basic approach - write to BG
    VBlankIntrWait();

    // Clear screen with sky color
    pal_bg_mem[0] = RGB15(8,16,31);

    // Wait for start button
    key_poll();
    if (key_hit(KEY_START)) {
        game_state = 1; // Go to gameplay
        init_game();
    }

    // Animate title
    frame_count++;

    // Use sprites to spell "KIRBY" (simplified - just show Kirby bouncing)
    int bounce_y = 60 + (lu_sin(frame_count * 512) >> 13);

    obj_set_attr(&obj_mem[0],
        ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | (bounce_y & 0xFF),
        ATTR1_SIZE(1) | (112),
        ATTR2_ID(0) | ATTR2_PRIO(0));

    // "PRESS START" blinking
    if (frame_count & 32) {
        obj_set_attr(&obj_mem[1],
            ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | (120),
            ATTR1_SIZE(0) | (112),
            ATTR2_ID(24) | ATTR2_PRIO(0));
    } else {
        obj_hide(&obj_mem[1]);
    }

    // Hide all other sprites
    int i;
    for (i = 2; i < 128; i++) {
        obj_hide(&obj_mem[i]);
    }
}

void game_play(void) {
    key_poll();

    update_player();
    update_enemies();
    update_projectiles();
    update_particles();
    update_camera();

    VBlankIntrWait();

    update_bg_scroll();
    render_sprites();
    render_hud();

    frame_count++;

    if (kirby.state == STATE_DEAD) {
        if (frame_count % 120 == 0) {
            game_state = 2; // Game over
        }
    }

    if (level_complete) {
        game_state = 3; // Win!
    }
}

void game_over(void) {
    VBlankIntrWait();
    key_poll();

    pal_bg_mem[0] = RGB15(0, 0, 0);

    // Show Kirby falling
    int sy = 80 + (frame_count % 60);
    if (sy > SCREEN_H) sy = SCREEN_H - 16;

    obj_set_attr(&obj_mem[0],
        ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | (sy & 0xFF),
        ATTR1_SIZE(1) | ATTR1_HFLIP | (112),
        ATTR2_ID(4) | ATTR2_PRIO(0));

    int i;
    for (i = 1; i < 128; i++) {
        obj_hide(&obj_mem[i]);
    }

    frame_count++;

    if (key_hit(KEY_START)) {
        game_state = 0; // Back to title
        frame_count = 0;
    }
}

void game_win(void) {
    VBlankIntrWait();
    key_poll();

    pal_bg_mem[0] = RGB15(20, 25, 31);

    // Kirby victory dance!
    int bounce_y = 80 + (lu_sin(frame_count * 1024) >> 13) * 2;

    int tile = (frame_count / 8) & 1 ? 0 : 4;
    int flip = (frame_count / 16) & 1 ? ATTR1_HFLIP : 0;

    obj_set_attr(&obj_mem[0],
        ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | (bounce_y & 0xFF),
        ATTR1_SIZE(1) | flip | (112),
        ATTR2_ID(tile) | ATTR2_PRIO(0));

    // Victory stars
    int i;
    for (i = 1; i < 8; i++) {
        int star_x = 112 + lu_cos((frame_count * 512 + i * 8192) & 0xFFFF) / 512;
        int star_y = bounce_y - 8 + lu_sin((frame_count * 512 + i * 8192) & 0xFFFF) / 512;
        obj_set_attr(&obj_mem[i],
            ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG | (star_y & 0xFF),
            ATTR1_SIZE(0) | (star_x & 0x1FF),
            ATTR2_ID(24) | ATTR2_PRIO(0));
    }

    for (i = 8; i < 128; i++) {
        obj_hide(&obj_mem[i]);
    }

    frame_count++;

    if (key_hit(KEY_START)) {
        game_state = 0;
        frame_count = 0;
    }
}

// ============================================================
// MAIN
// ============================================================

int main(void) {
    // Enable VBlank interrupt
    irq_init(NULL);
    irq_enable(II_VBLANK);

    // Initialize graphics
    init_gfx();
    load_sprites();
    load_bg();

    game_state = 0; // Start at title
    frame_count = 0;

    while (1) {
        switch (game_state) {
            case 0: game_title(); break;
            case 1: game_play();  break;
            case 2: game_over();  break;
            case 3: game_win();   break;
        }
    }

    return 0;
}
