//======================================================================
// Zelda: Link's Awakening Style Game for GBA
// Using tonc library - Single File Implementation
// Compile with: make (using standard tonc makefile)
//======================================================================

#include <tonc.h>
#include <string.h>

#define RGB15_C(r,g,b)  ((r) | ((g) << 5) | ((b) << 10))

//----------------------------------------------------------------------
// Constants
//----------------------------------------------------------------------
#define TILE_SIZE       16
#define MAP_WIDTH       16
#define MAP_HEIGHT      12
#define MAX_ENEMIES     6
#define MAX_ITEMS       4
#define PLAYER_SPEED    0x180
#define ENEMY_SPEED     0x80
#define ATTACK_TIME     20
#define HURT_TIME       60
#define FP_SHIFT        8
#define FP_ONE          (1 << FP_SHIFT)

// Tile IDs (in map data)
#define T_GRASS   0
#define T_WALL    1
#define T_WATER   2
#define T_TREE    3
#define T_FLOOR   4
#define T_BUSH    5
#define T_CHEST   6
#define T_STAIRS  7

// Directions
#define DIR_DOWN  0
#define DIR_UP    1
#define DIR_LEFT  2
#define DIR_RIGHT 3

// Entity types
#define ENT_NONE    0
#define ENT_OCTOROK 1
#define ENT_MOBLIN  2
#define ENT_HEART   3
#define ENT_RUPEE   4
#define ENT_KEY     5
#define ENT_SWORD   6

// Game states
#define GS_TITLE    0
#define GS_PLAYING  1
#define GS_PAUSED   2
#define GS_GAMEOVER 3
#define GS_WIN      4

//----------------------------------------------------------------------
// Structures
//----------------------------------------------------------------------
typedef struct {
    s32 x, y;           // Fixed point position (.8)
    s16 vx, vy;
    u8 dir;
    u8 state;
    u8 frame;
    u8 anim_timer;
    s8 health;
    s8 max_health;
    u8 attack_timer;
    u8 hurt_timer;
    u8 has_sword;
    u8 keys;
    u16 rupees;
} Player;

typedef struct {
    u8 active;
    u8 type;
    s32 x, y;
    s16 vx, vy;
    u8 dir;
    u8 frame;
    s8 health;
    u8 ai_timer;
    u8 hurt_timer;
    u8 shoot_timer;
} Entity;

typedef struct {
    u8 tiles[MAP_HEIGHT][MAP_WIDTH];
    Entity enemies[MAX_ENEMIES];
    Entity items[MAX_ITEMS];
    u8 room_x, room_y;
} Room;

typedef struct {
    Player player;
    Room room;
    Entity sword_hitbox;
    s32 cam_x, cam_y;
    u8 game_state;
    u8 current_room;
    u8 transition;
    s8 trans_dir;
    u16 frame_count;
} Game;

Game game;

//----------------------------------------------------------------------
// Palette Data (16 colors)
//----------------------------------------------------------------------
const COLOR palette_bg[16] = {
    RGB15_C(0,0,0),       // 0: Black/transparent
    RGB15_C(8,20,8),      // 1: Dark green (grass shadow)
    RGB15_C(12,24,8),     // 2: Green (grass)
    RGB15_C(16,28,12),    // 3: Light green
    RGB15_C(20,16,12),    // 4: Brown (dirt/wood)
    RGB15_C(16,16,20),    // 5: Gray (stone)
    RGB15_C(20,20,24),    // 6: Light gray
    RGB15_C(4,12,28),     // 7: Blue (water)
    RGB15_C(8,16,31),     // 8: Light blue
    RGB15_C(28,24,8),     // 9: Yellow
    RGB15_C(24,8,8),      // 10: Red
    RGB15_C(28,28,24),    // 11: White
    RGB15_C(12,8,4),      // 12: Dark brown
    RGB15_C(24,20,16),    // 13: Tan
    RGB15_C(4,20,4),      // 14: Dark green (trees)
    RGB15_C(8,8,8),       // 15: Dark gray
};

const COLOR palette_sprite[16] = {
    RGB15_C(31,0,31),     // 0: Transparent (magenta)
    RGB15_C(24,28,16),    // 1: Skin
    RGB15_C(8,24,8),      // 2: Green (tunic)
    RGB15_C(4,16,4),      // 3: Dark green
    RGB15_C(28,24,8),     // 4: Yellow/blonde
    RGB15_C(20,16,4),     // 5: Dark yellow
    RGB15_C(24,8,8),      // 6: Red
    RGB15_C(16,4,4),      // 7: Dark red
    RGB15_C(20,20,24),    // 8: Silver (sword)
    RGB15_C(12,12,16),    // 9: Dark silver
    RGB15_C(4,12,28),     // 10: Blue
    RGB15_C(28,28,28),    // 11: White
    RGB15_C(8,8,8),       // 12: Dark gray
    RGB15_C(20,12,4),     // 13: Brown
    RGB15_C(28,20,12),    // 14: Orange
    RGB15_C(0,0,0),       // 15: Black
};

//----------------------------------------------------------------------
// Tile Graphics (4bpp, 8x8 tiles = 32 bytes each)
//----------------------------------------------------------------------
// Grass tile
const u32 tile_grass[] = {
    0x22222222, 0x22212222, 0x22222222, 0x22222212,
    0x22222222, 0x12222222, 0x22222222, 0x22222222,
};

// Wall tile
const u32 tile_wall[] = {
    0x55655565, 0x65556555, 0x55555555, 0x56565656,
    0x55555555, 0x55655565, 0x65556555, 0x56565656,
};

// Water tile
const u32 tile_water[] = {
    0x77777777, 0x77877777, 0x78777877, 0x77777777,
    0x77777777, 0x77778777, 0x77877787, 0x77777777,
};

// Tree tile
const u32 tile_tree[] = {
    0x00EEE000, 0x0EEEEE00, 0xEE2EEEE0, 0xEEEEE2EE,
    0xEEEEEEEE, 0x0E2EEEE0, 0x004CC000, 0x004CC000,
};

// Floor tile
const u32 tile_floor[] = {
    0x44444444, 0x44444444, 0x44444444, 0x44C44444,
    0x44444444, 0x44444444, 0x44444C44, 0x44444444,
};

// Bush tile
const u32 tile_bush[] = {
    0x002E2000, 0x02EEEE00, 0x2EE2EE20, 0xEEEEEEE2,
    0x2EEEEE2E, 0x0EE2EE20, 0x002E2E00, 0x00000000,
};

// Chest tile
const u32 tile_chest[] = {
    0x00000000, 0x0CCCCCC0, 0xC999999C, 0xC999999C,
    0xCCCCCCCC, 0xC999A99C, 0xC999999C, 0x0CCCCCC0,
};

// Stairs tile
const u32 tile_stairs[] = {
    0xCCCCCCCC, 0x44444444, 0x4CCCCCCC, 0x44444444,
    0x44CCCCCC, 0x44444444, 0x444CCCCC, 0x44444444,
};

//----------------------------------------------------------------------
// Sprite Graphics (4bpp, 16x16 = 128 bytes, stored as 4 8x8 tiles)
//----------------------------------------------------------------------
// Player facing down - 4 tiles (top-left, top-right, bottom-left, bottom-right)
const u32 sprite_player_down[] = {
    // Top-left
    0x00044000, 0x00444400, 0x04455440, 0x04451440,
    0x00444400, 0x00222200, 0x02222220, 0x22211222,
    // Top-right (empty for 8x8 half)
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    // Bottom-left
    0x22222222, 0x02222220, 0x02222220, 0x022CC220,
    0x022CC220, 0x00C00C00, 0x00C00C00, 0x0CC00CC0,
    // Bottom-right
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Player facing up
const u32 sprite_player_up[] = {
    // Top-left
    0x00044000, 0x00444400, 0x04444440, 0x04444440,
    0x00444400, 0x00333300, 0x03333330, 0x33322333,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    // Bottom-left
    0x33333333, 0x03333330, 0x03333330, 0x033CC330,
    0x033CC330, 0x00C00C00, 0x00C00C00, 0x0CC00CC0,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Player facing left
const u32 sprite_player_left[] = {
    0x00440000, 0x04444000, 0x44554400, 0x44514400,
    0x04444000, 0x02222000, 0x22222200, 0x22211200,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x22222200, 0x22222000, 0x02222000, 0x02CC2000,
    0x00CC2000, 0x00C0C000, 0x00C0C000, 0x0CC0CC00,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Player facing right
const u32 sprite_player_right[] = {
    0x00004400, 0x00044440, 0x00445544, 0x00441544,
    0x00044440, 0x00022220, 0x00222222, 0x00211222,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00222222, 0x00022222, 0x00022220, 0x0002CC20,
    0x0002CC00, 0x000C0C00, 0x000C0C00, 0x00CC0CC0,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Sword swing (horizontal)
const u32 sprite_sword_h[] = {
    0x00000000, 0x00000089, 0x00000988, 0x00009880,
    0x00098800, 0x00988000, 0x09880000, 0x98800000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x88000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Sword swing (vertical)
const u32 sprite_sword_v[] = {
    0x00000880, 0x00008890, 0x00008890, 0x00008890,
    0x00008890, 0x00008890, 0x00008890, 0x00008800,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Enemy (Octorok-like)
const u32 sprite_enemy[] = {
    0x00066000, 0x00666600, 0x06677660, 0x06677660,
    0x06666660, 0x66666666, 0x67666676, 0x66666666,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x06666660, 0x66600666, 0x66000066, 0x06000060,
    0x00600600, 0x00600600, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Heart item
const u32 sprite_heart[] = {
    0x00000000, 0x06600660, 0x06666660, 0x66666666,
    0x66666666, 0x06666660, 0x00666600, 0x00066000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Rupee item
const u32 sprite_rupee[] = {
    0x00000000, 0x00099000, 0x00999900, 0x09999990,
    0x09999990, 0x00999900, 0x00099000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

//----------------------------------------------------------------------
// Room/Map Data
//----------------------------------------------------------------------
const u8 room_0[MAP_HEIGHT][MAP_WIDTH] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,5,0,1},
    {1,0,0,0,0,0,0,0,0,0,3,0,0,0,0,1},
    {1,0,0,3,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,5,0,0,0,0,0,3,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,0,0,0,0,0,0,0,0,0,5,0,0,0,0,1},
    {1,0,0,0,0,0,3,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,3,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

const u8 room_1[MAP_HEIGHT][MAP_WIDTH] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,2,2,2,0,0,0,0,0,0,0,2,2,0,1},
    {1,0,2,2,2,0,0,0,0,0,0,0,2,2,0,1},
    {1,0,0,0,0,0,0,5,0,0,0,0,0,0,0,1},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {0,0,0,0,0,0,0,0,0,0,5,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,2,2,0,0,0,0,0,0,0,0,2,2,0,1},
    {1,0,2,2,0,0,0,6,0,0,0,0,2,2,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

const u8 room_2[MAP_HEIGHT][MAP_WIDTH] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1},
    {1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1},
    {1,4,4,4,4,4,4,6,4,4,4,4,4,4,4,1},
    {1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1},
    {1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1},
    {1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1},
    {1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1},
    {1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1},
    {1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1},
    {1,4,4,4,4,4,4,7,4,4,4,4,4,4,4,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

//----------------------------------------------------------------------
// Function Prototypes
//----------------------------------------------------------------------
void init_graphics(void);
void init_game(void);
void load_room(int room_id);
void update_player(void);
void update_enemies(void);
void update_items(void);
void check_room_transition(void);
void render(void);
void draw_hud(void);
int check_tile_collision(int x, int y, int w, int h);
int check_entity_collision(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2);
void spawn_enemy(int type, int x, int y);
void spawn_item(int type, int x, int y);
void player_attack(void);
void damage_player(int dmg);
void damage_enemy(Entity *e, int dmg);

//----------------------------------------------------------------------
// Initialize Graphics
//----------------------------------------------------------------------
void init_graphics(void) {
    // Set video mode: Mode 0, enable BG0, enable sprites
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // Setup BG0 for tilemap (256x256 pixels, 32x32 tiles)
    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_4BPP | BG_REG_32x32 | BG_PRIO(1);
    
    // Load background palette
    memcpy(pal_bg_mem, palette_bg, sizeof(palette_bg));
    
    // Load sprite palette
    memcpy(pal_obj_mem, palette_sprite, sizeof(palette_sprite));
    
    // Load background tiles into charblock 0
    memcpy(&tile_mem[0][0], tile_grass, 32);
    memcpy(&tile_mem[0][1], tile_wall, 32);
    memcpy(&tile_mem[0][2], tile_water, 32);
    memcpy(&tile_mem[0][3], tile_tree, 32);
    memcpy(&tile_mem[0][4], tile_floor, 32);
    memcpy(&tile_mem[0][5], tile_bush, 32);
    memcpy(&tile_mem[0][6], tile_chest, 32);
    memcpy(&tile_mem[0][7], tile_stairs, 32);
    
    // Load sprite tiles into charblock 4 (OBJ tiles)
    memcpy(&tile_mem[4][0], sprite_player_down, 128);
    memcpy(&tile_mem[4][4], sprite_player_up, 128);
    memcpy(&tile_mem[4][8], sprite_player_left, 128);
    memcpy(&tile_mem[4][12], sprite_player_right, 128);
    memcpy(&tile_mem[4][16], sprite_sword_h, 128);
    memcpy(&tile_mem[4][20], sprite_sword_v, 128);
    memcpy(&tile_mem[4][24], sprite_enemy, 128);
    memcpy(&tile_mem[4][28], sprite_heart, 128);
    memcpy(&tile_mem[4][32], sprite_rupee, 128);
    
    // Initialize OAM - hide all sprites initially
    oam_init(obj_mem, 128);
}

//----------------------------------------------------------------------
// Initialize Game State
//----------------------------------------------------------------------
void init_game(void) {
    memset(&game, 0, sizeof(Game));
    
    game.game_state = GS_PLAYING;
    
    // Initialize player
    game.player.x = 120 << FP_SHIFT;
    game.player.y = 80 << FP_SHIFT;
    game.player.dir = DIR_DOWN;
    game.player.health = 6;
    game.player.max_health = 6;
    game.player.has_sword = 1;  // Start with sword
    
    game.current_room = 0;
    load_room(0);
}

//----------------------------------------------------------------------
// Load Room Data
//----------------------------------------------------------------------
void load_room(int room_id) {
    const u8 *room_data;
    
    // Select room data
    switch(room_id) {
        case 1: room_data = &room_1[0][0]; break;
        case 2: room_data = &room_2[0][0]; break;
        default: room_data = &room_0[0][0]; break;
    }
    
    // Copy room tiles
    memcpy(game.room.tiles, room_data, MAP_WIDTH * MAP_HEIGHT);
    
    // Clear entities
    memset(game.room.enemies, 0, sizeof(game.room.enemies));
    memset(game.room.items, 0, sizeof(game.room.items));
    
    // Update tilemap in VRAM (screenblock 28)
    for(int y = 0; y < MAP_HEIGHT; y++) {
        for(int x = 0; x < MAP_WIDTH; x++) {
            u8 tile = game.room.tiles[y][x];
            se_mem[28][y * 32 + x] = tile;
        }
    }
    
    // Spawn enemies based on room
    if(room_id == 0) {
        spawn_enemy(ENT_OCTOROK, 100, 60);
        spawn_enemy(ENT_OCTOROK, 180, 100);
    } else if(room_id == 1) {
        spawn_enemy(ENT_OCTOROK, 80, 80);
        spawn_enemy(ENT_MOBLIN, 160, 80);
        spawn_item(ENT_RUPEE, 120, 120);
    }
}

//----------------------------------------------------------------------
// Spawn Enemy
//----------------------------------------------------------------------
void spawn_enemy(int type, int x, int y) {
    for(int i = 0; i < MAX_ENEMIES; i++) {
        if(!game.room.enemies[i].active) {
            game.room.enemies[i].active = 1;
            game.room.enemies[i].type = type;
            game.room.enemies[i].x = x << FP_SHIFT;
            game.room.enemies[i].y = y << FP_SHIFT;
            game.room.enemies[i].health = (type == ENT_MOBLIN) ? 4 : 2;
            game.room.enemies[i].dir = DIR_DOWN;
            game.room.enemies[i].ai_timer = 0;
            break;
        }
    }
}

//----------------------------------------------------------------------
// Spawn Item
//----------------------------------------------------------------------
void spawn_item(int type, int x, int y) {
    for(int i = 0; i < MAX_ITEMS; i++) {
        if(!game.room.items[i].active) {
            game.room.items[i].active = 1;
            game.room.items[i].type = type;
            game.room.items[i].x = x << FP_SHIFT;
            game.room.items[i].y = y << FP_SHIFT;
            break;
        }
    }
}

//----------------------------------------------------------------------
// Check Tile Collision
//----------------------------------------------------------------------
int check_tile_collision(int x, int y, int w, int h) {
    // Check corners
    int tx1 = x / TILE_SIZE;
    int ty1 = y / TILE_SIZE;
    int tx2 = (x + w - 1) / TILE_SIZE;
    int ty2 = (y + h - 1) / TILE_SIZE;
    
    // Bounds check
    if(tx1 < 0 || tx2 >= MAP_WIDTH || ty1 < 0 || ty2 >= MAP_HEIGHT) {
        return 1;
    }
    
    // Check all covered tiles
    for(int ty = ty1; ty <= ty2; ty++) {
        for(int tx = tx1; tx <= tx2; tx++) {
            u8 tile = game.room.tiles[ty][tx];
            if(tile == T_WALL || tile == T_TREE || tile == T_WATER) {
                return 1;
            }
        }
    }
    return 0;
}

//----------------------------------------------------------------------
// Check Entity Collision (AABB)
//----------------------------------------------------------------------
int check_entity_collision(int x1, int y1, int w1, int h1, 
                           int x2, int y2, int w2, int h2) {
    return (x1 < x2 + w2 && x1 + w1 > x2 && 
            y1 < y2 + h2 && y1 + h1 > y2);
}

//----------------------------------------------------------------------
// Player Attack
//----------------------------------------------------------------------
void player_attack(void) {
    if(game.player.attack_timer > 0 || !game.player.has_sword) return;
    
    game.player.attack_timer = ATTACK_TIME;
    
    // Set sword hitbox based on direction
    int px = game.player.x >> FP_SHIFT;
    int py = game.player.y >> FP_SHIFT;
    
    game.sword_hitbox.active = 1;
    
    switch(game.player.dir) {
        case DIR_DOWN:
            game.sword_hitbox.x = (px + 2) << FP_SHIFT;
            game.sword_hitbox.y = (py + 16) << FP_SHIFT;
            break;
        case DIR_UP:
            game.sword_hitbox.x = (px + 2) << FP_SHIFT;
            game.sword_hitbox.y = (py - 12) << FP_SHIFT;
            break;
        case DIR_LEFT:
            game.sword_hitbox.x = (px - 12) << FP_SHIFT;
            game.sword_hitbox.y = (py + 4) << FP_SHIFT;
            break;
        case DIR_RIGHT:
            game.sword_hitbox.x = (px + 16) << FP_SHIFT;
            game.sword_hitbox.y = (py + 4) << FP_SHIFT;
            break;
    }
}

//----------------------------------------------------------------------
// Damage Player
//----------------------------------------------------------------------
void damage_player(int dmg) {
    if(game.player.hurt_timer > 0) return;
    
    game.player.health -= dmg;
    game.player.hurt_timer = HURT_TIME;
    
    if(game.player.health <= 0) {
        game.game_state = GS_GAMEOVER;
    }
}

//----------------------------------------------------------------------
// Damage Enemy
//----------------------------------------------------------------------
void damage_enemy(Entity *e, int dmg) {
    if(e->hurt_timer > 0) return;
    
    e->health -= dmg;
    e->hurt_timer = 30;
    
    if(e->health <= 0) {
        e->active = 0;
        // Random drop
        if((qran() & 3) == 0) {
            spawn_item(ENT_HEART, e->x >> FP_SHIFT, e->y >> FP_SHIFT);
        } else if((qran() & 3) == 1) {
            spawn_item(ENT_RUPEE, e->x >> FP_SHIFT, e->y >> FP_SHIFT);
        }
    }
}

//----------------------------------------------------------------------
// Update Player
//----------------------------------------------------------------------
void update_player(void) {
    Player *p = &game.player;
    
    // Decrement timers
    if(p->attack_timer > 0) p->attack_timer--;
    if(p->hurt_timer > 0) p->hurt_timer--;
    
    // Clear sword hitbox after attack ends
    if(p->attack_timer == 0) {
        game.sword_hitbox.active = 0;
    }
    
    // Only allow movement if not attacking
    if(p->attack_timer == 0) {
        p->vx = 0;
        p->vy = 0;
        
        // Read input
        key_poll();
        
        if(key_is_down(KEY_LEFT)) {
            p->vx = -PLAYER_SPEED;
            p->dir = DIR_LEFT;
        }
        if(key_is_down(KEY_RIGHT)) {
            p->vx = PLAYER_SPEED;
            p->dir = DIR_RIGHT;
        }
        if(key_is_down(KEY_UP)) {
            p->vy = -PLAYER_SPEED;
            p->dir = DIR_UP;
        }
        if(key_is_down(KEY_DOWN)) {
            p->vy = PLAYER_SPEED;
            p->dir = DIR_DOWN;
        }
        
        // Attack with A button
        if(key_hit(KEY_A)) {
            player_attack();
        }
    }
    
    // Apply velocity with collision detection
    int new_x = (p->x + p->vx) >> FP_SHIFT;
    int new_y = (p->y + p->vy) >> FP_SHIFT;
    int px = p->x >> FP_SHIFT;
    int py = p->y >> FP_SHIFT;
    
    // Check X movement
    if(!check_tile_collision(new_x + 2, py + 8, 12, 8)) {
        p->x += p->vx;
    }
    
    // Check Y movement
    px = p->x >> FP_SHIFT;
    if(!check_tile_collision(px + 2, new_y + 8, 12, 8)) {
        p->y += p->vy;
    }
    
    // Animation
    if(p->vx != 0 || p->vy != 0) {
        p->anim_timer++;
        if(p->anim_timer >= 8) {
            p->anim_timer = 0;
            p->frame = (p->frame + 1) & 1;
        }
    }
    
    // Check sword hits on enemies
    if(game.sword_hitbox.active) {
        int sx = game.sword_hitbox.x >> FP_SHIFT;
        int sy = game.sword_hitbox.y >> FP_SHIFT;
        
        for(int i = 0; i < MAX_ENEMIES; i++) {
            Entity *e = &game.room.enemies[i];
            if(e->active) {
                int ex = e->x >> FP_SHIFT;
                int ey = e->y >> FP_SHIFT;
                
                if(check_entity_collision(sx, sy, 12, 12, ex, ey, 12, 14)) {
                    damage_enemy(e, 1);
                }
            }
        }
    }
}

//----------------------------------------------------------------------
// Update Enemies
//----------------------------------------------------------------------
void update_enemies(void) {
    int px = game.player.x >> FP_SHIFT;
    int py = game.player.y >> FP_SHIFT;
    
    for(int i = 0; i < MAX_ENEMIES; i++) {
        Entity *e = &game.room.enemies[i];
        if(!e->active) continue;
        
        if(e->hurt_timer > 0) e->hurt_timer--;
        
        // Simple AI: move toward player occasionally
        e->ai_timer++;
        if(e->ai_timer >= 30) {
            e->ai_timer = 0;
            
            int ex = e->x >> FP_SHIFT;
            int ey = e->y >> FP_SHIFT;
            
            // Random direction with bias toward player
            int dx = px - ex;
            int dy = py - ey;
            
            if((qran() & 1)) {
                if(dx > 0) e->vx = ENEMY_SPEED;
                else if(dx < 0) e->vx = -ENEMY_SPEED;
                else e->vx = 0;
                e->vy = 0;
                e->dir = (dx > 0) ? DIR_RIGHT : DIR_LEFT;
            } else {
                if(dy > 0) e->vy = ENEMY_SPEED;
                else if(dy < 0) e->vy = -ENEMY_SPEED;
                else e->vy = 0;
                e->vx = 0;
                e->dir = (dy > 0) ? DIR_DOWN : DIR_UP;
            }
        }
        
        // Move with collision
        int new_x = (e->x + e->vx) >> FP_SHIFT;
        int new_y = (e->y + e->vy) >> FP_SHIFT;
        int ex = e->x >> FP_SHIFT;
        int ey = e->y >> FP_SHIFT;
        
        if(!check_tile_collision(new_x + 2, ey + 4, 12, 12)) {
            e->x += e->vx;
        }
        ex = e->x >> FP_SHIFT;
        if(!check_tile_collision(ex + 2, new_y + 4, 12, 12)) {
            e->y += e->vy;
        }
        
        // Screen bounds
        if(e->x < (2 << FP_SHIFT)) e->x = 2 << FP_SHIFT;
        if(e->x > (224 << FP_SHIFT)) e->x = 224 << FP_SHIFT;
        if(e->y < (2 << FP_SHIFT)) e->y = 2 << FP_SHIFT;
        if(e->y > (144 << FP_SHIFT)) e->y = 144 << FP_SHIFT;
        
        // Check collision with player
        ex = e->x >> FP_SHIFT;
        ey = e->y >> FP_SHIFT;
        if(check_entity_collision(px + 2, py + 4, 12, 12, ex + 2, ey + 4, 12, 12)) {
            damage_player(1);
        }
        
        // Animation
        e->frame = (game.frame_count >> 4) & 1;
    }
}

//----------------------------------------------------------------------
// Update Items
//----------------------------------------------------------------------
void update_items(void) {
    int px = game.player.x >> FP_SHIFT;
    int py = game.player.y >> FP_SHIFT;
    
    for(int i = 0; i < MAX_ITEMS; i++) {
        Entity *item = &game.room.items[i];
        if(!item->active) continue;
        
        int ix = item->x >> FP_SHIFT;
        int iy = item->y >> FP_SHIFT;
        
        // Check collection
        if(check_entity_collision(px, py, 16, 16, ix, iy, 8, 8)) {
            switch(item->type) {
                case ENT_HEART:
                    if(game.player.health < game.player.max_health) {
                        game.player.health += 2;
                        if(game.player.health > game.player.max_health) {
                            game.player.health = game.player.max_health;
                        }
                    }
                    break;
                case ENT_RUPEE:
                    game.player.rupees++;
                    break;
                case ENT_KEY:
                    game.player.keys++;
                    break;
                case ENT_SWORD:
                    game.player.has_sword = 1;
                    break;
            }
            item->active = 0;
        }
    }
}

//----------------------------------------------------------------------
// Check Room Transitions
//----------------------------------------------------------------------
void check_room_transition(void) {
    int px = game.player.x >> FP_SHIFT;
    int py = game.player.y >> FP_SHIFT;
    
    // Right edge -> Room 1
    if(px > 232 && game.current_room == 0) {
        game.current_room = 1;
        game.player.x = 8 << FP_SHIFT;
        load_room(1);
    }
    // Left edge -> Room 0
    else if(px < 4 && game.current_room == 1) {
        game.current_room = 0;
        game.player.x = 220 << FP_SHIFT;
        load_room(0);
    }
    
    // Stairs in room 2
    int tx = (px + 8) / TILE_SIZE;
    int ty = (py + 12) / TILE_SIZE;
    if(tx >= 0 && tx < MAP_WIDTH && ty >= 0 && ty < MAP_HEIGHT) {
        if(game.room.tiles[ty][tx] == T_STAIRS) {
            if(game.current_room == 2) {
                game.current_room = 0;
                game.player.x = 120 << FP_SHIFT;
                game.player.y = 80 << FP_SHIFT;
                load_room(0);
            }
        }
    }
}

//----------------------------------------------------------------------
// Render Game
//----------------------------------------------------------------------
void render(void) {
    int oam_idx = 0;
    
    // Player sprite
    int px = game.player.x >> FP_SHIFT;
    int py = game.player.y >> FP_SHIFT;
    
    // Flicker when hurt
    int show_player = 1;
    if(game.player.hurt_timer > 0 && (game.frame_count & 2)) {
        show_player = 0;
    }
    
    if(show_player) {
        // Select tile based on direction
        int tile_id = game.player.dir * 4;  // 4 tiles per direction
        
        obj_set_attr(&obj_mem[oam_idx],
            ATTR0_Y(py) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(px) | ATTR1_SIZE_16,
            ATTR2_ID(tile_id) | ATTR2_PRIO(0));
        oam_idx++;
        
        // Draw sword during attack
        if(game.player.attack_timer > 0) {
            int sx = game.sword_hitbox.x >> FP_SHIFT;
            int sy = game.sword_hitbox.y >> FP_SHIFT;
            int sword_tile = (game.player.dir == DIR_LEFT || game.player.dir == DIR_RIGHT) ? 16 : 20;
            
            u16 attr1 = ATTR1_X(sx) | ATTR1_SIZE_16;
            // Flip sword based on direction
            if(game.player.dir == DIR_LEFT) attr1 |= ATTR1_HFLIP;
            if(game.player.dir == DIR_UP) attr1 |= ATTR1_VFLIP;
            
            obj_set_attr(&obj_mem[oam_idx],
                ATTR0_Y(sy) | ATTR0_SQUARE | ATTR0_4BPP,
                attr1,
                ATTR2_ID(sword_tile) | ATTR2_PRIO(0));
            oam_idx++;
        }
    }
    
    // Enemy sprites
    for(int i = 0; i < MAX_ENEMIES; i++) {
        Entity *e = &game.room.enemies[i];
        if(!e->active) continue;
        
        // Flicker when hurt
        if(e->hurt_timer > 0 && (game.frame_count & 2)) continue;
        
        int ex = e->x >> FP_SHIFT;
        int ey = e->y >> FP_SHIFT;
        
        obj_set_attr(&obj_mem[oam_idx],
            ATTR0_Y(ey) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(ex) | ATTR1_SIZE_16,
            ATTR2_ID(24) | ATTR2_PRIO(0));
        oam_idx++;
    }
    
    // Item sprites
    for(int i = 0; i < MAX_ITEMS; i++) {
        Entity *item = &game.room.items[i];
        if(!item->active) continue;
        
        int ix = item->x >> FP_SHIFT;
        int iy = item->y >> FP_SHIFT;
        int tile_id = (item->type == ENT_HEART) ? 28 : 32;
        
        obj_set_attr(&obj_mem[oam_idx],
            ATTR0_Y(iy) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(ix) | ATTR1_SIZE_8,
            ATTR2_ID(tile_id) | ATTR2_PRIO(0));
        oam_idx++;
    }
    
    // HUD - Hearts
    for(int i = 0; i < game.player.max_health / 2; i++) {
        int heart_x = 8 + i * 10;
        
        // Full, half, or empty heart
        if(game.player.health >= (i + 1) * 2) {
            obj_set_attr(&obj_mem[oam_idx],
                ATTR0_Y(4) | ATTR0_SQUARE | ATTR0_4BPP,
                ATTR1_X(heart_x) | ATTR1_SIZE_8,
                ATTR2_ID(28) | ATTR2_PRIO(0));
        } else if(game.player.health >= i * 2 + 1) {
            // Half heart - just show normal for simplicity
            obj_set_attr(&obj_mem[oam_idx],
                ATTR0_Y(4) | ATTR0_SQUARE | ATTR0_4BPP,
                ATTR1_X(heart_x) | ATTR1_SIZE_8,
                ATTR2_ID(28) | ATTR2_PRIO(0));
        }
        oam_idx++;
    }
    
    // Rupee counter
    obj_set_attr(&obj_mem[oam_idx],
        ATTR0_Y(4) | ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_X(200) | ATTR1_SIZE_8,
        ATTR2_ID(32) | ATTR2_PRIO(0));
    oam_idx++;
    
    // Hide remaining sprites
    for(; oam_idx < 128; oam_idx++) {
        obj_hide(&obj_mem[oam_idx]);
    }
}

//----------------------------------------------------------------------
// Main
//----------------------------------------------------------------------
int main(void) {
    // Initialize
    init_graphics();
    init_game();
    
    // Main game loop
    while(1) {
        VBlankIntrWait();
        
        if(game.game_state == GS_PLAYING) {
            key_poll();
            
            update_player();
            update_enemies();
            update_items();
            check_room_transition();
            
            game.frame_count++;
        }
        else if(game.game_state == GS_GAMEOVER) {
            key_poll();
            if(key_hit(KEY_START)) {
                init_game();
            }
        }
        
        render();
    }
    
    return 0;
}
