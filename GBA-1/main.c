/*
 * Final Fantasy GBA RPG Demo
 * Uses tonc library
 * Compile with devkitPro/devkitARM and tonc
 */

#include <tonc.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// CONSTANTS AND DEFINES
// ============================================================================

#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   160
#define TILE_SIZE       16
#define MAP_WIDTH       32
#define MAP_HEIGHT      32
#define MAX_PARTY       4
#define MAX_ENEMIES     4

// Tile types
#define TILE_GRASS      0
#define TILE_WATER      1
#define TILE_FOREST     2
#define TILE_MOUNTAIN   3
#define TILE_TOWN       4
#define TILE_CASTLE     5
#define TILE_PATH       6
#define TILE_BRIDGE     7

// Game states
typedef enum {
    STATE_TITLE,
    STATE_FIELD,
    STATE_BATTLE,
    STATE_MENU,
    STATE_SHOP,
    STATE_GAME_OVER,
    STATE_VICTORY
} GameState;

// Battle states
typedef enum {
    BATTLE_START,
    BATTLE_PLAYER_SELECT,
    BATTLE_PLAYER_ACTION,
    BATTLE_ENEMY_TURN,
    BATTLE_RESULT,
    BATTLE_WIN,
    BATTLE_LOSE,
    BATTLE_RUN
} BattleState;

// Menu options
typedef enum {
    MENU_ITEMS,
    MENU_MAGIC,
    MENU_EQUIP,
    MENU_STATUS,
    MENU_SAVE,
    MENU_EXIT,
    MENU_COUNT
} MenuOption;

// Battle commands
typedef enum {
    CMD_ATTACK,
    CMD_MAGIC,
    CMD_ITEM,
    CMD_DEFEND,
    CMD_RUN,
    CMD_COUNT
} BattleCommand;

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef struct {
    char name[12];
    s32 hp, max_hp;
    s32 mp, max_mp;
    s32 attack;
    s32 defense;
    s32 magic;
    s32 speed;
    s32 level;
    s32 exp;
    s32 next_exp;
    u8 job;        // 0=Warrior, 1=Mage, 2=Thief, 3=Healer
    BOOL alive;
    BOOL defending;
} Character;

typedef struct {
    char name[12];
    s32 hp, max_hp;
    s32 attack;
    s32 defense;
    s32 magic;
    s32 speed;
    s32 exp_reward;
    s32 gold_reward;
    u8 sprite_id;
    BOOL alive;
} Enemy;

typedef struct {
    s32 x, y;
    s32 screen_x, screen_y;
    u8 facing;
    u16 steps;
} Player;

typedef struct {
    s32 gold;
    s32 potions;
    s32 ethers;
    s32 phoenix_downs;
    s32 tents;
} Inventory;

typedef struct {
    GameState state;
    BattleState battle_state;
    u8 current_char;
    u8 current_enemy;
    u8 menu_selection;
    u8 target_selection;
    u8 message_timer;
    char message[64];
    u16 encounter_counter;
    BOOL in_submenu;
} GameData;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

Character party[MAX_PARTY];
Enemy enemies[MAX_ENEMIES];
Player player;
Inventory inventory;
GameData game;
u8 party_size = 1;
u8 enemy_count = 0;

// Simple map data (32x32)
const u8 world_map[MAP_HEIGHT][MAP_WIDTH] = {
    {3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3},
    {3,2,2,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,2,3},
    {3,2,0,0,0,0,6,6,6,0,0,2,2,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,3},
    {3,0,0,4,0,0,6,0,6,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,2,2,2,2,2,2,2,3},
    {3,0,0,0,0,0,6,0,6,6,6,6,0,0,1,1,1,1,1,1,0,0,0,0,0,2,2,2,2,2,2,3},
    {3,0,0,0,0,0,0,0,0,0,0,6,0,0,1,1,1,1,1,1,0,0,0,0,0,0,2,2,2,2,2,3},
    {3,0,0,0,0,0,0,0,0,0,0,6,0,0,0,7,7,7,0,0,0,0,0,0,0,0,0,2,2,2,2,3},
    {3,0,2,2,0,0,0,0,0,0,0,6,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,2,2,2,3},
    {3,0,2,2,2,0,0,0,0,0,6,6,0,0,0,0,6,0,0,0,0,0,0,5,0,0,0,0,0,2,2,3},
    {3,0,0,2,2,0,0,0,0,0,6,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,2,3},
    {3,0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,6,6,6,6,6,0,0,0,0,0,0,0,0,0,0,3},
    {3,0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,3},
    {3,2,0,0,0,0,0,0,0,6,6,0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,3},
    {3,2,2,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,6,6,6,0,0,0,0,0,0,2,2,3},
    {3,2,2,2,0,0,0,0,6,6,0,0,0,4,0,0,0,0,0,0,0,0,6,0,0,0,0,0,2,2,2,3},
    {3,2,2,2,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,6,6,0,0,0,2,2,2,2,3},
    {3,2,2,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0,0,0,2,2,2,2,3},
    {3,2,0,0,0,0,0,6,6,0,0,0,0,0,0,2,2,0,0,0,0,0,0,6,0,0,2,2,2,2,2,3},
    {3,0,0,0,0,0,0,6,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,6,6,0,2,2,2,2,2,3},
    {3,0,0,0,0,0,6,6,0,0,0,0,0,2,2,2,2,2,2,0,0,0,0,0,6,0,0,2,2,2,2,3},
    {3,0,0,0,0,0,6,0,0,0,0,0,0,2,2,2,2,2,0,0,0,0,0,0,6,0,0,0,2,2,2,3},
    {3,0,0,0,0,0,6,0,0,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,6,0,0,0,0,2,2,3},
    {3,0,0,4,0,6,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,2,3},
    {3,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,6,6,6,6,0,0,3},
    {3,0,0,0,6,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0,0,3},
    {3,0,0,6,6,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,6,0,3},
    {3,0,6,6,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0,0,6,0,3},
    {3,0,6,0,0,0,0,2,2,2,2,2,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,6,0,3},
    {3,6,6,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,6,0,3},
    {3,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0,6,6,0,3},
    {3,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,0,0,3},
    {3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3},
};

// Tile colors
const u16 tile_colors[8] = {
    RGB15(8, 24, 8),    // Grass - green
    RGB15(4, 8, 28),    // Water - blue
    RGB15(0, 16, 0),    // Forest - dark green
    RGB15(16, 12, 8),   // Mountain - brown
    RGB15(28, 20, 8),   // Town - tan
    RGB15(24, 24, 24),  // Castle - white
    RGB15(20, 16, 8),   // Path - light brown
    RGB15(16, 12, 4),   // Bridge - wood
};

// Enemy templates
const struct {
    char name[12];
    s32 hp, attack, defense, magic, speed;
    s32 exp, gold;
} enemy_templates[] = {
    {"Goblin",      30,  8,  4,  2,  5,  15,  10},
    {"Wolf",        40, 12,  6,  0,  8,  20,  15},
    {"Skeleton",    50, 15,  8,  5,  4,  30,  25},
    {"Orc",         80, 20, 12,  3,  6,  50,  40},
    {"Dark Mage",   45, 10,  5, 18, 10,  60,  55},
    {"Ogre",       120, 25, 15,  0,  3,  80,  70},
    {"Dragon",     200, 35, 20, 25, 12, 200, 150},
    {"Demon",      250, 40, 25, 30, 15, 300, 200},
};

// Magic spells
const struct {
    char name[10];
    s32 mp_cost;
    s32 power;
    u8 type;  // 0=damage, 1=heal
} spells[] = {
    {"Fire",     4, 20, 0},
    {"Blizzard", 6, 30, 0},
    {"Thunder",  8, 40, 0},
    {"Cure",     3, 25, 1},
    {"Cura",     8, 60, 1},
    {"Life",    20,  1, 1},  // Revive
};

// ============================================================================
// GRAPHICS DATA (Simple 8x8 tiles)
// ============================================================================

// Simple tile graphics (8x8 pixels, 4bpp)
const u32 tile_gfx[8][8] = {
    // Grass
    {0x11111111, 0x11111111, 0x11111111, 0x11111111,
     0x11111111, 0x11111111, 0x11111111, 0x11111111},
    // Water
    {0x22222222, 0x22322222, 0x22222222, 0x22222322,
     0x22222222, 0x32222222, 0x22222222, 0x22222222},
    // Forest
    {0x11441111, 0x14444111, 0x44444411, 0x14444411,
     0x11444111, 0x11141111, 0x11111111, 0x11111111},
    // Mountain
    {0x11111111, 0x11155111, 0x11555511, 0x15555551,
     0x55555555, 0x55555555, 0x55555555, 0x55555555},
    // Town
    {0x66666666, 0x66777666, 0x67777766, 0x67777766,
     0x66666666, 0x66766766, 0x66766766, 0x66666666},
    // Castle
    {0x88188188, 0x88888888, 0x88788788, 0x88888888,
     0x88888888, 0x88877888, 0x88877888, 0x88888888},
    // Path
    {0x99999999, 0x99999999, 0x99999999, 0x99999999,
     0x99999999, 0x99999999, 0x99999999, 0x99999999},
    // Bridge
    {0xAAAAAAAA, 0xBBBBBBBB, 0xAAAAAAAA, 0xBBBBBBBB,
     0xAAAAAAAA, 0xBBBBBBBB, 0xAAAAAAAA, 0xBBBBBBBB},
};

// Player sprite (16x16 as 4 8x8 tiles)
const u32 player_sprite[4][8] = {
    // Top-left
    {0x00000000, 0x00011000, 0x00111100, 0x00111100,
     0x00011000, 0x01111110, 0x01111110, 0x00111100},
    // Top-right
    {0x00000000, 0x00000000, 0x00000000, 0x00000000,
     0x00000000, 0x00000000, 0x00000000, 0x00000000},
    // Bottom-left
    {0x00111100, 0x00111100, 0x00100100, 0x00100100,
     0x00110110, 0x00000000, 0x00000000, 0x00000000},
    // Bottom-right
    {0x00000000, 0x00000000, 0x00000000, 0x00000000,
     0x00000000, 0x00000000, 0x00000000, 0x00000000},
};

// Enemy sprites for battle (simple representations)
const u32 enemy_sprite[8] = {
    0x00111100, 0x01111110, 0x11011011, 0x11111111,
    0x11111111, 0x01100110, 0x01111110, 0x00111100,
};

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

void init_game(void);
void init_graphics(void);
void init_party(void);
void load_tiles(void);
void update_title(void);
void update_field(void);
void update_battle(void);
void update_menu(void);
void draw_title(void);
void draw_field(void);
void draw_battle(void);
void draw_menu(void);
void draw_hud(void);
void draw_text(int x, int y, const char* text, u16 color);
void draw_number(int x, int y, int num, u16 color);
void draw_box(int x, int y, int w, int h, u16 bg, u16 border);
void draw_hp_bar(int x, int y, int current, int max, u16 color);
void move_player(int dx, int dy);
BOOL check_collision(int x, int y);
void check_encounter(void);
void start_battle(void);
void end_battle(BOOL victory);
void player_attack(int target);
void player_magic(int spell, int target);
void player_item(int item);
void enemy_turn(int enemy_idx);
void apply_damage(Character* target, int damage);
void apply_heal(Character* target, int amount);
void level_up(Character* c);
int calc_damage(int attack, int defense);
int get_random(int max);
void set_message(const char* msg);
void copy_string(char* dest, const char* src);
int string_len(const char* s);

// ============================================================================
// INITIALIZATION
// ============================================================================

void init_game(void) {
    game.state = STATE_TITLE;
    game.battle_state = BATTLE_START;
    game.current_char = 0;
    game.current_enemy = 0;
    game.menu_selection = 0;
    game.message_timer = 0;
    game.encounter_counter = 0;
    game.in_submenu = FALSE;
    
    player.x = 3;
    player.y = 3;
    player.facing = 0;
    player.steps = 0;
    
    inventory.gold = 100;
    inventory.potions = 3;
    inventory.ethers = 1;
    inventory.phoenix_downs = 1;
    inventory.tents = 0;
    
    // Seed random with some varying value
    srand(REG_TM0CNT);
}

void init_party(void) {
    // Hero - Warrior
    copy_string(party[0].name, "Cloud");
    party[0].hp = party[0].max_hp = 100;
    party[0].mp = party[0].max_mp = 20;
    party[0].attack = 15;
    party[0].defense = 10;
    party[0].magic = 5;
    party[0].speed = 8;
    party[0].level = 1;
    party[0].exp = 0;
    party[0].next_exp = 100;
    party[0].job = 0;
    party[0].alive = TRUE;
    party[0].defending = FALSE;
    
    // Mage
    copy_string(party[1].name, "Vivi");
    party[1].hp = party[1].max_hp = 60;
    party[1].mp = party[1].max_mp = 50;
    party[1].attack = 8;
    party[1].defense = 5;
    party[1].magic = 18;
    party[1].speed = 6;
    party[1].level = 1;
    party[1].exp = 0;
    party[1].next_exp = 100;
    party[1].job = 1;
    party[1].alive = TRUE;
    party[1].defending = FALSE;
    
    // Thief
    copy_string(party[2].name, "Zidane");
    party[2].hp = party[2].max_hp = 80;
    party[2].mp = party[2].max_mp = 25;
    party[2].attack = 12;
    party[2].defense = 7;
    party[2].magic = 8;
    party[2].speed = 14;
    party[2].level = 1;
    party[2].exp = 0;
    party[2].next_exp = 100;
    party[2].job = 2;
    party[2].alive = TRUE;
    party[2].defending = FALSE;
    
    // Healer
    copy_string(party[3].name, "Rosa");
    party[3].hp = party[3].max_hp = 70;
    party[3].mp = party[3].max_mp = 60;
    party[3].attack = 6;
    party[3].defense = 6;
    party[3].magic = 15;
    party[3].speed = 7;
    party[3].level = 1;
    party[3].exp = 0;
    party[3].next_exp = 100;
    party[3].job = 3;
    party[3].alive = TRUE;
    party[3].defending = FALSE;
    
    party_size = 4;
}

void init_graphics(void) {
    // Set up video mode (Mode 3 for simplicity - bitmap mode)
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;
    
    // Clear screen
    memset32(vid_mem, 0, SCREEN_WIDTH * SCREEN_HEIGHT / 2);
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

int get_random(int max) {
    if (max <= 0) return 0;
    return rand() % max;
}

void copy_string(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

int string_len(const char* s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

void set_message(const char* msg) {
    copy_string(game.message, msg);
    game.message_timer = 60;  // Show for 1 second
}

// ============================================================================
// DRAWING FUNCTIONS
// ============================================================================

// Simple pixel plotting for Mode 3
static inline void plot_pixel(int x, int y, u16 color) {
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        vid_mem[y * SCREEN_WIDTH + x] = color;
    }
}

void draw_rect(int x, int y, int w, int h, u16 color) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            plot_pixel(px, py, color);
        }
    }
}

void draw_box(int x, int y, int w, int h, u16 bg, u16 border) {
    // Draw background
    draw_rect(x, y, w, h, bg);
    
    // Draw border
    for (int px = x; px < x + w; px++) {
        plot_pixel(px, y, border);
        plot_pixel(px, y + h - 1, border);
    }
    for (int py = y; py < y + h; py++) {
        plot_pixel(x, py, border);
        plot_pixel(x + w - 1, py, border);
    }
}

// Simple 4x6 font rendering
static const u8 font_4x6[96][6] = {
    // Space through ~
    {0,0,0,0,0,0},    // Space
    {4,4,4,0,4,0},    // !
    {10,10,0,0,0,0},  // "
    {10,15,10,15,10,0}, // #
    {4,15,12,3,15,4}, // $
    {9,2,4,9,0,0},    // %
    {4,10,4,10,5,0},  // &
    {4,4,0,0,0,0},    // '
    {2,4,4,4,2,0},    // (
    {4,2,2,2,4,0},    // )
    {0,10,4,10,0,0},  // *
    {0,4,14,4,0,0},   // +
    {0,0,0,4,4,8},    // ,
    {0,0,14,0,0,0},   // -
    {0,0,0,0,4,0},    // .
    {1,2,4,8,0,0},    // /
    {6,9,9,9,6,0},    // 0
    {4,12,4,4,14,0},  // 1
    {6,9,2,4,15,0},   // 2
    {14,1,6,1,14,0},  // 3
    {2,6,10,15,2,0},  // 4
    {15,8,14,1,14,0}, // 5
    {6,8,14,9,6,0},   // 6
    {15,1,2,4,4,0},   // 7
    {6,9,6,9,6,0},    // 8
    {6,9,7,1,6,0},    // 9
    {0,4,0,4,0,0},    // :
    {0,4,0,4,8,0},    // ;
    {2,4,8,4,2,0},    // <
    {0,14,0,14,0,0},  // =
    {8,4,2,4,8,0},    // >
    {6,9,2,0,2,0},    // ?
    {6,9,11,8,6,0},   // @
    {6,9,15,9,9,0},   // A
    {14,9,14,9,14,0}, // B
    {6,9,8,9,6,0},    // C
    {14,9,9,9,14,0},  // D
    {15,8,14,8,15,0}, // E
    {15,8,14,8,8,0},  // F
    {6,8,11,9,6,0},   // G
    {9,9,15,9,9,0},   // H
    {14,4,4,4,14,0},  // I
    {7,2,2,10,4,0},   // J
    {9,10,12,10,9,0}, // K
    {8,8,8,8,15,0},   // L
    {9,15,15,9,9,0},  // M
    {9,13,15,11,9,0}, // N
    {6,9,9,9,6,0},    // O
    {14,9,14,8,8,0},  // P
    {6,9,9,10,5,0},   // Q
    {14,9,14,10,9,0}, // R
    {7,8,6,1,14,0},   // S
    {14,4,4,4,4,0},   // T
    {9,9,9,9,6,0},    // U
    {9,9,9,6,6,0},    // V
    {9,9,15,15,9,0},  // W
    {9,6,6,6,9,0},    // X
    {9,9,6,4,4,0},    // Y
    {15,2,4,8,15,0},  // Z
    {6,4,4,4,6,0},    // [
    {8,4,4,2,1,0},    // Backslash
    {6,2,2,2,6,0},    // ]
    {4,10,0,0,0,0},   // ^
    {0,0,0,0,15,0},   // _
    {4,2,0,0,0,0},    // `
    {0,6,10,10,7,0},  // a
    {8,14,9,9,14,0},  // b
    {0,7,8,8,7,0},    // c
    {1,7,9,9,7,0},    // d
    {0,6,15,8,6,0},   // e
    {2,4,14,4,4,0},   // f
    {0,7,9,7,1,6},    // g
    {8,14,9,9,9,0},   // h
    {4,0,4,4,4,0},    // i
    {2,0,2,2,10,4},   // j
    {8,9,14,10,9,0},  // k
    {4,4,4,4,2,0},    // l
    {0,10,15,9,9,0},  // m
    {0,14,9,9,9,0},   // n
    {0,6,9,9,6,0},    // o
    {0,14,9,14,8,8},  // p
    {0,7,9,7,1,1},    // q
    {0,7,8,8,8,0},    // r
    {0,7,12,3,14,0},  // s
    {4,14,4,4,2,0},   // t
    {0,9,9,9,7,0},    // u
    {0,9,9,6,6,0},    // v
    {0,9,9,15,6,0},   // w
    {0,9,6,6,9,0},    // x
    {0,9,9,7,1,6},    // y
    {0,15,2,4,15,0},  // z
    {2,4,12,4,2,0},   // {
    {4,4,4,4,4,0},    // |
    {8,4,6,4,8,0},    // }
    {0,4,10,0,0,0},   // ~
    {0,0,0,0,0,0},    // DEL
};

void draw_char(int x, int y, char c, u16 color) {
    if (c < 32 || c > 127) c = ' ';
    int idx = c - 32;
    
    for (int row = 0; row < 6; row++) {
        u8 bits = font_4x6[idx][row];
        for (int col = 0; col < 4; col++) {
            if (bits & (8 >> col)) {
                plot_pixel(x + col, y + row, color);
            }
        }
    }
}

void draw_text(int x, int y, const char* text, u16 color) {
    while (*text) {
        draw_char(x, y, *text, color);
        x += 5;
        text++;
    }
}

void draw_number(int x, int y, int num, u16 color) {
    char buf[12];
    int i = 0;
    BOOL negative = FALSE;
    
    if (num < 0) {
        negative = TRUE;
        num = -num;
    }
    
    if (num == 0) {
        buf[i++] = '0';
    } else {
        while (num > 0) {
            buf[i++] = '0' + (num % 10);
            num /= 10;
        }
    }
    
    if (negative) buf[i++] = '-';
    
    // Reverse and draw
    while (i > 0) {
        draw_char(x, y, buf[--i], color);
        x += 5;
    }
}

void draw_hp_bar(int x, int y, int current, int max, u16 color) {
    int bar_width = 40;
    int filled = (current * bar_width) / max;
    if (filled < 0) filled = 0;
    if (filled > bar_width) filled = bar_width;
    
    // Draw background
    draw_rect(x, y, bar_width, 4, RGB15(4, 4, 4));
    // Draw filled portion
    draw_rect(x, y, filled, 4, color);
    // Draw border
    for (int i = 0; i < bar_width; i++) {
        plot_pixel(x + i, y, RGB15(16, 16, 16));
        plot_pixel(x + i, y + 3, RGB15(8, 8, 8));
    }
}

// ============================================================================
// TITLE SCREEN
// ============================================================================

void draw_title(void) {
    // Clear screen
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, RGB15(0, 0, 8));
    
    // Draw stars
    for (int i = 0; i < 50; i++) {
        int sx = (i * 47) % SCREEN_WIDTH;
        int sy = (i * 31) % SCREEN_HEIGHT;
        plot_pixel(sx, sy, RGB15(31, 31, 31));
    }
    
    // Title
    draw_text(60, 40, "FINAL FANTASY", RGB15(31, 31, 0));
    draw_text(80, 55, "GBA DEMO", RGB15(31, 31, 0));
    
    // Crystal (simple diamond shape)
    for (int i = 0; i < 10; i++) {
        for (int j = -i; j <= i; j++) {
            plot_pixel(120 + j, 80 + i, RGB15(16, 16, 31));
        }
    }
    for (int i = 9; i >= 0; i--) {
        for (int j = -i; j <= i; j++) {
            plot_pixel(120 + j, 100 - i, RGB15(16, 16, 31));
        }
    }
    
    // Menu options
    u16 color1 = (game.menu_selection == 0) ? RGB15(31, 31, 31) : RGB15(16, 16, 16);
    u16 color2 = (game.menu_selection == 1) ? RGB15(31, 31, 31) : RGB15(16, 16, 16);
    
    draw_text(88, 115, "NEW GAME", color1);
    draw_text(88, 130, "CONTINUE", color2);
    
    // Selection cursor
    draw_text(76, 115 + game.menu_selection * 15, ">", RGB15(31, 31, 31));
    
    draw_text(60, 150, "Press START", RGB15(20, 20, 20));
}

void update_title(void) {
    key_poll();
    
    if (key_hit(KEY_UP)) {
        if (game.menu_selection > 0) game.menu_selection--;
    }
    if (key_hit(KEY_DOWN)) {
        if (game.menu_selection < 1) game.menu_selection++;
    }
    if (key_hit(KEY_START) || key_hit(KEY_A)) {
        if (game.menu_selection == 0) {
            // New game
            init_party();
            game.state = STATE_FIELD;
        }
        // Continue would load save, not implemented
    }
}

// ============================================================================
// FIELD (OVERWORLD) SCREEN
// ============================================================================

void draw_field(void) {
    // Calculate camera offset
    int cam_x = player.x - 7;
    int cam_y = player.y - 5;
    if (cam_x < 0) cam_x = 0;
    if (cam_y < 0) cam_y = 0;
    if (cam_x > MAP_WIDTH - 15) cam_x = MAP_WIDTH - 15;
    if (cam_y > MAP_HEIGHT - 10) cam_y = MAP_HEIGHT - 10;
    
    // Draw visible tiles
    for (int ty = 0; ty < 11; ty++) {
        for (int tx = 0; tx < 16; tx++) {
            int map_x = cam_x + tx;
            int map_y = cam_y + ty;
            
            if (map_x >= 0 && map_x < MAP_WIDTH && map_y >= 0 && map_y < MAP_HEIGHT) {
                u8 tile = world_map[map_y][map_x];
                u16 color = tile_colors[tile];
                
                int screen_x = tx * 16;
                int screen_y = ty * 16;
                
                // Draw tile
                draw_rect(screen_x, screen_y, 16, 16, color);
                
                // Add some texture/detail
                if (tile == TILE_GRASS) {
                    // Grass tufts
                    plot_pixel(screen_x + 3, screen_y + 5, RGB15(4, 20, 4));
                    plot_pixel(screen_x + 11, screen_y + 9, RGB15(4, 20, 4));
                } else if (tile == TILE_WATER) {
                    // Wave effect (simple)
                    int wave = ((tx + ty + (game.encounter_counter >> 4)) & 3);
                    if (wave == 0) {
                        plot_pixel(screen_x + 8, screen_y + 4, RGB15(8, 12, 31));
                        plot_pixel(screen_x + 4, screen_y + 10, RGB15(8, 12, 31));
                    }
                } else if (tile == TILE_FOREST) {
                    // Tree top
                    draw_rect(screen_x + 4, screen_y + 2, 8, 6, RGB15(0, 20, 0));
                    draw_rect(screen_x + 6, screen_y + 8, 4, 6, RGB15(12, 8, 4));
                } else if (tile == TILE_MOUNTAIN) {
                    // Snow cap
                    plot_pixel(screen_x + 7, screen_y + 2, RGB15(31, 31, 31));
                    plot_pixel(screen_x + 8, screen_y + 2, RGB15(31, 31, 31));
                    plot_pixel(screen_x + 7, screen_y + 3, RGB15(31, 31, 31));
                    plot_pixel(screen_x + 8, screen_y + 3, RGB15(31, 31, 31));
                } else if (tile == TILE_TOWN) {
                    // House shape
                    draw_rect(screen_x + 4, screen_y + 6, 8, 8, RGB15(20, 16, 8));
                    // Roof
                    for (int r = 0; r < 4; r++) {
                        draw_rect(screen_x + 4 - r/2, screen_y + 5 - r, 8 + r, 1, RGB15(24, 8, 4));
                    }
                } else if (tile == TILE_CASTLE) {
                    // Castle towers
                    draw_rect(screen_x + 2, screen_y + 4, 4, 10, RGB15(20, 20, 24));
                    draw_rect(screen_x + 10, screen_y + 4, 4, 10, RGB15(20, 20, 24));
                    draw_rect(screen_x + 5, screen_y + 6, 6, 8, RGB15(20, 20, 24));
                    // Flag
                    draw_rect(screen_x + 7, screen_y + 2, 3, 2, RGB15(31, 0, 0));
                }
            }
        }
    }
    
    // Draw player
    int player_screen_x = (player.x - cam_x) * 16;
    int player_screen_y = (player.y - cam_y) * 16;
    
    // Body
    draw_rect(player_screen_x + 4, player_screen_y + 4, 8, 12, RGB15(8, 8, 24));
    // Head
    draw_rect(player_screen_x + 5, player_screen_y, 6, 6, RGB15(28, 24, 16));
    // Hair
    draw_rect(player_screen_x + 5, player_screen_y, 6, 2, RGB15(31, 31, 0));
    
    // Draw HUD
    draw_box(0, 144, 240, 16, RGB15(0, 0, 8), RGB15(16, 16, 16));
    draw_text(4, 148, "HP:", RGB15(31, 31, 31));
    draw_number(20, 148, party[0].hp, RGB15(0, 31, 0));
    draw_text(50, 148, "/", RGB15(31, 31, 31));
    draw_number(55, 148, party[0].max_hp, RGB15(31, 31, 31));
    
    draw_text(90, 148, "MP:", RGB15(31, 31, 31));
    draw_number(106, 148, party[0].mp, RGB15(0, 16, 31));
    
    draw_text(140, 148, "G:", RGB15(31, 31, 0));
    draw_number(152, 148, inventory.gold, RGB15(31, 31, 0));
    
    draw_text(200, 148, "Lv", RGB15(31, 31, 31));
    draw_number(212, 148, party[0].level, RGB15(31, 31, 31));
}

void move_player(int dx, int dy) {
    int new_x = player.x + dx;
    int new_y = player.y + dy;
    
    if (!check_collision(new_x, new_y)) {
        player.x = new_x;
        player.y = new_y;
        player.steps++;
        
        check_encounter();
    }
}

BOOL check_collision(int x, int y) {
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) {
        return TRUE;
    }
    
    u8 tile = world_map[y][x];
    
    // Water and mountains are impassable (except on bridge)
    if (tile == TILE_WATER) return TRUE;
    if (tile == TILE_MOUNTAIN) return TRUE;
    
    return FALSE;
}

void check_encounter(void) {
    game.encounter_counter++;
    
    // Get current tile
    u8 tile = world_map[player.y][player.x];
    
    // Only random encounters on grass and forest
    if (tile == TILE_GRASS || tile == TILE_FOREST) {
        // Random encounter chance
        int chance = (tile == TILE_FOREST) ? 15 : 8;  // Higher in forest
        
        if (get_random(100) < chance) {
            start_battle();
        }
    }
}

void update_field(void) {
    key_poll();
    
    if (key_hit(KEY_UP)) {
        player.facing = 1;
        move_player(0, -1);
    }
    if (key_hit(KEY_DOWN)) {
        player.facing = 0;
        move_player(0, 1);
    }
    if (key_hit(KEY_LEFT)) {
        player.facing = 2;
        move_player(-1, 0);
    }
    if (key_hit(KEY_RIGHT)) {
        player.facing = 3;
        move_player(1, 0);
    }
    
    // Open menu
    if (key_hit(KEY_START) || key_hit(KEY_B)) {
        game.state = STATE_MENU;
        game.menu_selection = 0;
    }
    
    // Heal at town/castle
    if (key_hit(KEY_A)) {
        u8 tile = world_map[player.y][player.x];
        if (tile == TILE_TOWN || tile == TILE_CASTLE) {
            // Full heal
            for (int i = 0; i < party_size; i++) {
                party[i].hp = party[i].max_hp;
                party[i].mp = party[i].max_mp;
                party[i].alive = TRUE;
            }
            set_message("Party restored!");
        }
    }
}

// ============================================================================
// BATTLE SYSTEM
// ============================================================================

void start_battle(void) {
    game.state = STATE_BATTLE;
    game.battle_state = BATTLE_START;
    game.current_char = 0;
    game.current_enemy = 0;
    game.menu_selection = 0;
    game.target_selection = 0;
    game.in_submenu = FALSE;
    
    // Reset defending status
    for (int i = 0; i < party_size; i++) {
        party[i].defending = FALSE;
    }
    
    // Generate enemies based on player level
    int max_enemy_type = party[0].level / 2;
    if (max_enemy_type > 7) max_enemy_type = 7;
    if (max_enemy_type < 0) max_enemy_type = 0;
    
    enemy_count = 1 + get_random(3);  // 1-3 enemies
    
    for (int i = 0; i < enemy_count; i++) {
        int type = get_random(max_enemy_type + 1);
        copy_string(enemies[i].name, enemy_templates[type].name);
        enemies[i].hp = enemy_templates[type].hp;
        enemies[i].max_hp = enemy_templates[type].hp;
        enemies[i].attack = enemy_templates[type].attack;
        enemies[i].defense = enemy_templates[type].defense;
        enemies[i].magic = enemy_templates[type].magic;
        enemies[i].speed = enemy_templates[type].speed;
        enemies[i].exp_reward = enemy_templates[type].exp;
        enemies[i].gold_reward = enemy_templates[type].gold;
        enemies[i].sprite_id = type;
        enemies[i].alive = TRUE;
    }
    
    set_message("Monsters appeared!");
}

void draw_battle(void) {
    // Background
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, RGB15(4, 4, 8));
    
    // Ground
    draw_rect(0, 80, SCREEN_WIDTH, 40, RGB15(8, 12, 4));
    
    // Draw enemies
    int enemy_spacing = SCREEN_WIDTH / (enemy_count + 1);
    for (int i = 0; i < enemy_count; i++) {
        int ex = enemy_spacing * (i + 1) - 16;
        int ey = 30;
        
        if (enemies[i].alive) {
            // Simple enemy sprite
            u16 e_color;
            switch (enemies[i].sprite_id) {
                case 0: e_color = RGB15(0, 24, 0); break;    // Goblin - green
                case 1: e_color = RGB15(20, 20, 20); break;  // Wolf - gray
                case 2: e_color = RGB15(28, 28, 24); break;  // Skeleton - bone
                case 3: e_color = RGB15(16, 24, 0); break;   // Orc - olive
                case 4: e_color = RGB15(16, 0, 24); break;   // Dark Mage - purple
                case 5: e_color = RGB15(24, 16, 8); break;   // Ogre - brown
                case 6: e_color = RGB15(31, 0, 0); break;    // Dragon - red
                default: e_color = RGB15(8, 0, 16); break;   // Demon - dark purple
            }
            
            // Body
            draw_rect(ex, ey, 32, 40, e_color);
            // Eyes
            draw_rect(ex + 6, ey + 8, 6, 6, RGB15(31, 31, 31));
            draw_rect(ex + 20, ey + 8, 6, 6, RGB15(31, 31, 31));
            plot_pixel(ex + 8, ey + 10, RGB15(0, 0, 0));
            plot_pixel(ex + 22, ey + 10, RGB15(0, 0, 0));
            
            // HP bar
            draw_hp_bar(ex, ey + 44, enemies[i].hp, enemies[i].max_hp, RGB15(31, 8, 8));
            
            // Selection highlight
            if (game.in_submenu && game.target_selection == i) {
                draw_rect(ex - 2, ey - 2, 36, 4, RGB15(31, 31, 0));
            }
            
            // Name
            draw_text(ex, ey + 52, enemies[i].name, RGB15(31, 31, 31));
        }
    }
    
    // Command menu box
    draw_box(0, 100, 80, 60, RGB15(0, 0, 16), RGB15(20, 20, 20));
    
    const char* commands[] = {"Attack", "Magic", "Item", "Defend", "Run"};
    for (int i = 0; i < CMD_COUNT; i++) {
        u16 color = (game.menu_selection == i && !game.in_submenu) ? RGB15(31, 31, 31) : RGB15(16, 16, 16);
        draw_text(12, 105 + i * 10, commands[i], color);
    }
    if (!game.in_submenu) {
        draw_text(4, 105 + game.menu_selection * 10, ">", RGB15(31, 31, 0));
    }
    
    // Party status
    draw_box(80, 100, 160, 60, RGB15(0, 0, 16), RGB15(20, 20, 20));
    
    for (int i = 0; i < party_size; i++) {
        int py = 103 + i * 14;
        u16 name_color = party[i].alive ? RGB15(31, 31, 31) : RGB15(16, 8, 8);
        
        // Highlight current character
        if (i == game.current_char && game.battle_state == BATTLE_PLAYER_SELECT) {
            draw_rect(82, py - 1, 155, 12, RGB15(4, 4, 12));
        }
        
        draw_text(84, py, party[i].name, name_color);
        
        // HP
        draw_text(130, py, "HP", RGB15(20, 20, 20));
        u16 hp_color = party[i].hp < party[i].max_hp / 4 ? RGB15(31, 8, 8) : RGB15(0, 31, 0);
        draw_number(145, py, party[i].hp, hp_color);
        
        // MP
        draw_text(175, py, "MP", RGB15(20, 20, 20));
        draw_number(190, py, party[i].mp, RGB15(0, 16, 31));
        
        // Defending indicator
        if (party[i].defending) {
            draw_text(215, py, "D", RGB15(31, 31, 0));
        }
    }
    
    // Message display
    if (game.message_timer > 0) {
        draw_box(20, 40, 200, 20, RGB15(0, 0, 0), RGB15(31, 31, 31));
        draw_text(28, 46, game.message, RGB15(31, 31, 31));
    }
    
    // Sub-menu for magic/item selection
    if (game.in_submenu && game.menu_selection == CMD_MAGIC) {
        draw_box(80, 30, 80, 50, RGB15(0, 0, 20), RGB15(24, 24, 24));
        for (int i = 0; i < 4; i++) {
            u16 color = (game.target_selection == i) ? RGB15(31, 31, 31) : RGB15(16, 16, 16);
            draw_text(88, 35 + i * 10, spells[i].name, color);
            draw_number(140, 35 + i * 10, spells[i].mp_cost, RGB15(0, 16, 31));
        }
        draw_text(82, 35 + game.target_selection * 10, ">", RGB15(31, 31, 0));
    }
}

void player_attack(int target) {
    Character* attacker = &party[game.current_char];
    Enemy* defender = &enemies[target];
    
    int damage = calc_damage(attacker->attack, defender->defense);
    damage += get_random(damage / 4 + 1);  // Some variance
    
    defender->hp -= damage;
    
    char msg[64];
    copy_string(msg, attacker->name);
    int len = string_len(msg);
    copy_string(msg + len, " hits ");
    len = string_len(msg);
    copy_string(msg + len, defender->name);
    len = string_len(msg);
    copy_string(msg + len, " for ");
    set_message(msg);
    
    if (defender->hp <= 0) {
        defender->hp = 0;
        defender->alive = FALSE;
    }
}

void player_magic(int spell_idx, int target) {
    Character* caster = &party[game.current_char];
    
    if (caster->mp < spells[spell_idx].mp_cost) {
        set_message("Not enough MP!");
        return;
    }
    
    caster->mp -= spells[spell_idx].mp_cost;
    int power = spells[spell_idx].power + caster->magic;
    
    if (spells[spell_idx].type == 0) {
        // Damage spell
        enemies[target].hp -= power;
        if (enemies[target].hp <= 0) {
            enemies[target].hp = 0;
            enemies[target].alive = FALSE;
        }
    } else {
        // Heal spell
        if (spell_idx == 5) {
            // Life spell - revive
            party[target].alive = TRUE;
            party[target].hp = party[target].max_hp / 4;
        } else {
            party[target].hp += power;
            if (party[target].hp > party[target].max_hp) {
                party[target].hp = party[target].max_hp;
            }
        }
    }
    
    set_message(spells[spell_idx].name);
}

void player_item(int item) {
    switch (item) {
        case 0:  // Potion
            if (inventory.potions > 0) {
                inventory.potions--;
                party[game.current_char].hp += 50;
                if (party[game.current_char].hp > party[game.current_char].max_hp) {
                    party[game.current_char].hp = party[game.current_char].max_hp;
                }
                set_message("Used Potion!");
            }
            break;
        case 1:  // Ether
            if (inventory.ethers > 0) {
                inventory.ethers--;
                party[game.current_char].mp += 30;
                if (party[game.current_char].mp > party[game.current_char].max_mp) {
                    party[game.current_char].mp = party[game.current_char].max_mp;
                }
                set_message("Used Ether!");
            }
            break;
        case 2:  // Phoenix Down
            if (inventory.phoenix_downs > 0) {
                // Find dead ally
                for (int i = 0; i < party_size; i++) {
                    if (!party[i].alive) {
                        party[i].alive = TRUE;
                        party[i].hp = party[i].max_hp / 4;
                        inventory.phoenix_downs--;
                        set_message("Revived!");
                        break;
                    }
                }
            }
            break;
    }
}

void enemy_turn(int enemy_idx) {
    Enemy* attacker = &enemies[enemy_idx];
    
    if (!attacker->alive) return;
    
    // Find alive target
    int target = -1;
    for (int i = 0; i < party_size; i++) {
        if (party[i].alive) {
            target = i;
            break;
        }
    }
    
    if (target < 0) return;
    
    // Random target selection
    for (int tries = 0; tries < 10; tries++) {
        int t = get_random(party_size);
        if (party[t].alive) {
            target = t;
            break;
        }
    }
    
    Character* defender = &party[target];
    int def = defender->defense;
    if (defender->defending) def *= 2;
    
    int damage = calc_damage(attacker->attack, def);
    damage += get_random(damage / 4 + 1);
    
    defender->hp -= damage;
    
    if (defender->hp <= 0) {
        defender->hp = 0;
        defender->alive = FALSE;
    }
    
    char msg[64];
    copy_string(msg, attacker->name);
    int len = string_len(msg);
    copy_string(msg + len, " attacks!");
    set_message(msg);
}

int calc_damage(int attack, int defense) {
    int damage = attack * 3 - defense * 2;
    if (damage < 1) damage = 1;
    return damage;
}

void level_up(Character* c) {
    c->level++;
    c->max_hp += 10 + get_random(10);
    c->max_mp += 3 + get_random(5);
    c->hp = c->max_hp;
    c->mp = c->max_mp;
    c->attack += 2;
    c->defense += 1;
    c->magic += 2;
    c->speed += 1;
    c->next_exp = c->next_exp * 3 / 2;
}

void end_battle(BOOL victory) {
    if (victory) {
        int total_exp = 0;
        int total_gold = 0;
        
        for (int i = 0; i < enemy_count; i++) {
            total_exp += enemies[i].exp_reward;
            total_gold += enemies[i].gold_reward;
        }
        
        inventory.gold += total_gold;
        
        // Distribute exp
        for (int i = 0; i < party_size; i++) {
            if (party[i].alive) {
                party[i].exp += total_exp / party_size;
                if (party[i].exp >= party[i].next_exp) {
                    party[i].exp -= party[i].next_exp;
                    level_up(&party[i]);
                }
            }
        }
        
        game.state = STATE_FIELD;
    } else {
        game.state = STATE_GAME_OVER;
    }
}

void update_battle(void) {
    key_poll();
    
    // Decrement message timer
    if (game.message_timer > 0) {
        game.message_timer--;
        if (game.message_timer > 0) return;  // Wait for message
    }
    
    switch (game.battle_state) {
        case BATTLE_START:
            game.battle_state = BATTLE_PLAYER_SELECT;
            game.current_char = 0;
            // Find first alive character
            while (game.current_char < party_size && !party[game.current_char].alive) {
                game.current_char++;
            }
            break;
            
        case BATTLE_PLAYER_SELECT:
            if (!game.in_submenu) {
                if (key_hit(KEY_UP)) {
                    if (game.menu_selection > 0) game.menu_selection--;
                }
                if (key_hit(KEY_DOWN)) {
                    if (game.menu_selection < CMD_COUNT - 1) game.menu_selection++;
                }
                if (key_hit(KEY_A)) {
                    switch (game.menu_selection) {
                        case CMD_ATTACK:
                            game.in_submenu = TRUE;
                            game.target_selection = 0;
                            // Find first alive enemy
                            while (game.target_selection < enemy_count && !enemies[game.target_selection].alive) {
                                game.target_selection++;
                            }
                            break;
                        case CMD_MAGIC:
                            game.in_submenu = TRUE;
                            game.target_selection = 0;
                            break;
                        case CMD_ITEM:
                            if (inventory.potions > 0) {
                                player_item(0);
                                game.battle_state = BATTLE_PLAYER_ACTION;
                            }
                            break;
                        case CMD_DEFEND:
                            party[game.current_char].defending = TRUE;
                            set_message("Defending!");
                            game.battle_state = BATTLE_PLAYER_ACTION;
                            break;
                        case CMD_RUN:
                            if (get_random(100) < 50) {
                                game.battle_state = BATTLE_RUN;
                            } else {
                                set_message("Can't escape!");
                                game.battle_state = BATTLE_PLAYER_ACTION;
                            }
                            break;
                    }
                }
            } else {
                // Sub-menu active
                if (game.menu_selection == CMD_ATTACK) {
                    // Target enemy selection
                    if (key_hit(KEY_LEFT)) {
                        do {
                            if (game.target_selection > 0) game.target_selection--;
                        } while (game.target_selection > 0 && !enemies[game.target_selection].alive);
                    }
                    if (key_hit(KEY_RIGHT)) {
                        do {
                            if (game.target_selection < enemy_count - 1) game.target_selection++;
                        } while (game.target_selection < enemy_count - 1 && !enemies[game.target_selection].alive);
                    }
                    if (key_hit(KEY_A)) {
                        player_attack(game.target_selection);
                        game.in_submenu = FALSE;
                        game.battle_state = BATTLE_PLAYER_ACTION;
                    }
                } else if (game.menu_selection == CMD_MAGIC) {
                    // Spell selection
                    if (key_hit(KEY_UP)) {
                        if (game.target_selection > 0) game.target_selection--;
                    }
                    if (key_hit(KEY_DOWN)) {
                        if (game.target_selection < 3) game.target_selection++;
                    }
                    if (key_hit(KEY_A)) {
                        int spell = game.target_selection;
                        int target = 0;
                        if (spells[spell].type == 0) {
                            // Damage - target enemy
                            for (int i = 0; i < enemy_count; i++) {
                                if (enemies[i].alive) { target = i; break; }
                            }
                        } else {
                            // Heal - target party member
                            target = game.current_char;
                        }
                        player_magic(spell, target);
                        game.in_submenu = FALSE;
                        game.battle_state = BATTLE_PLAYER_ACTION;
                    }
                }
                
                if (key_hit(KEY_B)) {
                    game.in_submenu = FALSE;
                }
            }
            break;
            
        case BATTLE_PLAYER_ACTION:
            // Move to next character or enemy turn
            game.current_char++;
            while (game.current_char < party_size && !party[game.current_char].alive) {
                game.current_char++;
            }
            
            if (game.current_char >= party_size) {
                game.battle_state = BATTLE_ENEMY_TURN;
                game.current_enemy = 0;
            } else {
                game.battle_state = BATTLE_PLAYER_SELECT;
                game.menu_selection = 0;
            }
            
            // Check for victory
            BOOL all_enemies_dead = TRUE;
            for (int i = 0; i < enemy_count; i++) {
                if (enemies[i].alive) {
                    all_enemies_dead = FALSE;
                    break;
                }
            }
            if (all_enemies_dead) {
                game.battle_state = BATTLE_WIN;
                set_message("Victory!");
            }
            break;
            
        case BATTLE_ENEMY_TURN:
            if (game.current_enemy < enemy_count) {
                enemy_turn(game.current_enemy);
                game.current_enemy++;
            } else {
                // Check for game over
                BOOL all_dead = TRUE;
                for (int i = 0; i < party_size; i++) {
                    if (party[i].alive) {
                        all_dead = FALSE;
                        break;
                    }
                }
                
                if (all_dead) {
                    game.battle_state = BATTLE_LOSE;
                    set_message("Game Over...");
                } else {
                    // Reset for next round
                    for (int i = 0; i < party_size; i++) {
                        party[i].defending = FALSE;
                    }
                    game.battle_state = BATTLE_PLAYER_SELECT;
                    game.current_char = 0;
                    while (game.current_char < party_size && !party[game.current_char].alive) {
                        game.current_char++;
                    }
                    game.menu_selection = 0;
                }
            }
            break;
            
        case BATTLE_WIN:
            if (key_hit(KEY_A)) {
                end_battle(TRUE);
            }
            break;
            
        case BATTLE_LOSE:
            if (key_hit(KEY_A)) {
                end_battle(FALSE);
            }
            break;
            
        case BATTLE_RUN:
            set_message("Escaped!");
            if (game.message_timer == 0) {
                game.state = STATE_FIELD;
            }
            break;
            
        default:
            break;
    }
}

// ============================================================================
// MENU SCREEN
// ============================================================================

void draw_menu(void) {
    // Background
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, RGB15(0, 0, 12));
    
    // Menu box
    draw_box(10, 10, 100, 80, RGB15(0, 0, 16), RGB15(20, 20, 20));
    
    const char* menu_items[] = {"Items", "Magic", "Equip", "Status", "Save", "Exit"};
    for (int i = 0; i < MENU_COUNT; i++) {
        u16 color = (game.menu_selection == i) ? RGB15(31, 31, 31) : RGB15(16, 16, 16);
        draw_text(24, 18 + i * 11, menu_items[i], color);
    }
    draw_text(14, 18 + game.menu_selection * 11, ">", RGB15(31, 31, 0));
    
    // Party status
    draw_box(120, 10, 110, 100, RGB15(0, 0, 16), RGB15(20, 20, 20));
    
    for (int i = 0; i < party_size; i++) {
        int y = 16 + i * 24;
        draw_text(125, y, party[i].name, RGB15(31, 31, 31));
        
        draw_text(125, y + 8, "HP", RGB15(20, 20, 20));
        draw_number(140, y + 8, party[i].hp, RGB15(0, 31, 0));
        draw_text(165, y + 8, "/", RGB15(16, 16, 16));
        draw_number(172, y + 8, party[i].max_hp, RGB15(31, 31, 31));
        
        draw_text(125, y + 15, "MP", RGB15(20, 20, 20));
        draw_number(140, y + 15, party[i].mp, RGB15(0, 16, 31));
        draw_text(165, y + 15, "/", RGB15(16, 16, 16));
        draw_number(172, y + 15, party[i].max_mp, RGB15(31, 31, 31));
    }
    
    // Inventory info
    draw_box(10, 95, 100, 55, RGB15(0, 0, 16), RGB15(20, 20, 20));
    draw_text(16, 102, "Gold:", RGB15(31, 31, 0));
    draw_number(50, 102, inventory.gold, RGB15(31, 31, 0));
    draw_text(16, 114, "Potions:", RGB15(31, 31, 31));
    draw_number(60, 114, inventory.potions, RGB15(31, 31, 31));
    draw_text(16, 126, "Ethers:", RGB15(31, 31, 31));
    draw_number(55, 126, inventory.ethers, RGB15(31, 31, 31));
    draw_text(16, 138, "P.Down:", RGB15(31, 31, 31));
    draw_number(55, 138, inventory.phoenix_downs, RGB15(31, 31, 31));
    
    // Location info
    draw_box(120, 115, 110, 35, RGB15(0, 0, 16), RGB15(20, 20, 20));
    draw_text(125, 122, "Location:", RGB15(20, 20, 20));
    u8 tile = world_map[player.y][player.x];
    const char* loc_names[] = {"Plains", "Shore", "Forest", "Mountains", "Town", "Castle", "Road", "Bridge"};
    draw_text(125, 134, loc_names[tile], RGB15(31, 31, 31));
}

void update_menu(void) {
    key_poll();
    
    if (key_hit(KEY_UP)) {
        if (game.menu_selection > 0) game.menu_selection--;
    }
    if (key_hit(KEY_DOWN)) {
        if (game.menu_selection < MENU_COUNT - 1) game.menu_selection++;
    }
    
    if (key_hit(KEY_A)) {
        switch (game.menu_selection) {
            case MENU_ITEMS:
                // Use a potion on current party member in menu
                if (inventory.potions > 0 && party[0].hp < party[0].max_hp) {
                    inventory.potions--;
                    party[0].hp += 50;
                    if (party[0].hp > party[0].max_hp) party[0].hp = party[0].max_hp;
                }
                break;
            case MENU_EXIT:
                game.state = STATE_FIELD;
                break;
            default:
                // Other menu options not fully implemented
                break;
        }
    }
    
    if (key_hit(KEY_B) || key_hit(KEY_START)) {
        game.state = STATE_FIELD;
    }
}

// ============================================================================
// GAME OVER SCREEN
// ============================================================================

void draw_game_over(void) {
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, RGB15(4, 0, 0));
    draw_text(80, 70, "GAME OVER", RGB15(31, 8, 8));
    draw_text(60, 100, "Press START", RGB15(16, 16, 16));
}

void update_game_over(void) {
    key_poll();
    
    if (key_hit(KEY_START)) {
        // Reset game
        init_game();
    }
}

// ============================================================================
// MAIN LOOP
// ============================================================================

int main(void) {
    // Initialize
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    init_graphics();
    init_game();
    
    // Main game loop
    while (1) {
        VBlankIntrWait();
        
        // Update
        switch (game.state) {
            case STATE_TITLE:
                update_title();
                break;
            case STATE_FIELD:
                update_field();
                break;
            case STATE_BATTLE:
                update_battle();
                break;
            case STATE_MENU:
                update_menu();
                break;
            case STATE_GAME_OVER:
                update_game_over();
                break;
            default:
                break;
        }
        
        // Draw
        switch (game.state) {
            case STATE_TITLE:
                draw_title();
                break;
            case STATE_FIELD:
                draw_field();
                break;
            case STATE_BATTLE:
                draw_battle();
                break;
            case STATE_MENU:
                draw_menu();
                break;
            case STATE_GAME_OVER:
                draw_game_over();
                break;
            default:
                break;
        }
    }
    
    return 0;
}
