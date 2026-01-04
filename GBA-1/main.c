//==============================================================================
// Crystal Tower Defense - A GBA Tower Defense Game
// Compile with devkitPro: make
// Uses tonc library
//==============================================================================

#include <tonc.h>
#include <string.h>

//------------------------------------------------------------------------------
// Constants
//------------------------------------------------------------------------------
#define TILE_SIZE       16
#define MAP_WIDTH       15
#define MAP_HEIGHT      10

#define MAX_ENEMIES     24
#define MAX_TOWERS      16
#define MAX_BULLETS     32
#define MAX_WAYPOINTS   16

#define TOWER_NONE      0
#define TOWER_ARCHER    1
#define TOWER_MAGE      2
#define TOWER_KNIGHT    3
#define TOWER_COUNT     4

#define ENEMY_GOBLIN    1
#define ENEMY_WOLF      2
#define ENEMY_ORC       3

#define TILE_EMPTY      0
#define TILE_PATH       1
#define TILE_WATER      2
#define TILE_TREE       3

#define FP_SHIFT        8
#define FP_ONE          (1 << FP_SHIFT)

//------------------------------------------------------------------------------
// Data Structures
//------------------------------------------------------------------------------
typedef struct {
    s32 x, y;           // Fixed point position (8.8)
    s16 hp, max_hp;
    s16 speed;
    u8 type;
    u8 path_idx;
    u8 active;
    u8 flash;
} Enemy;

typedef struct {
    s16 x, y;           // Grid position
    u8 type;
    u8 level;
    s16 range;
    s16 damage;
    s16 cooldown;
    s16 cooldown_max;
    u8 active;
} Tower;

typedef struct {
    s32 x, y;           // Fixed point
    s32 dx, dy;         // Velocity
    u8 active;
    u8 tower_type;
    u8 lifetime;
} Bullet;

typedef struct {
    s16 x, y;
} Waypoint;

//------------------------------------------------------------------------------
// Game State
//------------------------------------------------------------------------------
static s16 crystals = 20;
static s32 gold = 100;
static s16 wave = 0;
static s16 wave_delay = 0;
static s16 spawn_count = 0;
static s16 spawn_timer = 0;
static s16 spawn_max = 8;
static u8 wave_active = 0;
static u8 game_over = 0;
static u8 game_won = 0;

static s16 cursor_x = 7;
static s16 cursor_y = 5;
static u8 selected_tower = TOWER_ARCHER;
static u8 cursor_blink = 0;

static Enemy enemies[MAX_ENEMIES];
static Tower towers[MAX_TOWERS];
static Bullet bullets[MAX_BULLETS];

static Waypoint waypoints[MAX_WAYPOINTS];
static s16 waypoint_count = 0;

static u8 map_data[MAP_HEIGHT][MAP_WIDTH];

static const u16 tower_costs[TOWER_COUNT] = {0, 40, 80, 60};
static const char* tower_names[TOWER_COUNT] = {"", "ARCHER", "MAGE", "KNIGHT"};

//------------------------------------------------------------------------------
// 4bpp Font Data (simplified 8x8 chars for 0-9, A-Z, some symbols)
//------------------------------------------------------------------------------
static const u32 font_data[64][8] = {
    // 0 (char 48 -> index 0)
    {0x00111100, 0x01000010, 0x01000110, 0x01001010, 0x01010010, 0x01100010, 0x00111100, 0x00000000},
    // 1
    {0x00001000, 0x00011000, 0x00001000, 0x00001000, 0x00001000, 0x00001000, 0x00011100, 0x00000000},
    // 2
    {0x00111100, 0x01000010, 0x00000100, 0x00001000, 0x00010000, 0x00100000, 0x01111110, 0x00000000},
    // 3
    {0x00111100, 0x01000010, 0x00000010, 0x00011100, 0x00000010, 0x01000010, 0x00111100, 0x00000000},
    // 4
    {0x00000100, 0x00001100, 0x00010100, 0x00100100, 0x01111110, 0x00000100, 0x00000100, 0x00000000},
    // 5
    {0x01111110, 0x01000000, 0x01111100, 0x00000010, 0x00000010, 0x01000010, 0x00111100, 0x00000000},
    // 6
    {0x00111100, 0x01000000, 0x01000000, 0x01111100, 0x01000010, 0x01000010, 0x00111100, 0x00000000},
    // 7
    {0x01111110, 0x00000010, 0x00000100, 0x00001000, 0x00010000, 0x00010000, 0x00010000, 0x00000000},
    // 8
    {0x00111100, 0x01000010, 0x01000010, 0x00111100, 0x01000010, 0x01000010, 0x00111100, 0x00000000},
    // 9
    {0x00111100, 0x01000010, 0x01000010, 0x00111110, 0x00000010, 0x00000010, 0x00111100, 0x00000000},
    // : (index 10)
    {0x00000000, 0x00011000, 0x00011000, 0x00000000, 0x00011000, 0x00011000, 0x00000000, 0x00000000},
    // space (index 11)
    {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    // - (index 12)
    {0x00000000, 0x00000000, 0x00000000, 0x01111110, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    // > (index 13)
    {0x00100000, 0x00010000, 0x00001000, 0x00000100, 0x00001000, 0x00010000, 0x00100000, 0x00000000},
    // < (index 14)
    {0x00000100, 0x00001000, 0x00010000, 0x00100000, 0x00010000, 0x00001000, 0x00000100, 0x00000000},
    // ! (index 15)
    {0x00001000, 0x00001000, 0x00001000, 0x00001000, 0x00001000, 0x00000000, 0x00001000, 0x00000000},
    // A (index 16)
    {0x00011000, 0x00100100, 0x01000010, 0x01000010, 0x01111110, 0x01000010, 0x01000010, 0x00000000},
    // B
    {0x01111100, 0x01000010, 0x01000010, 0x01111100, 0x01000010, 0x01000010, 0x01111100, 0x00000000},
    // C
    {0x00111100, 0x01000010, 0x01000000, 0x01000000, 0x01000000, 0x01000010, 0x00111100, 0x00000000},
    // D
    {0x01111000, 0x01000100, 0x01000010, 0x01000010, 0x01000010, 0x01000100, 0x01111000, 0x00000000},
    // E
    {0x01111110, 0x01000000, 0x01000000, 0x01111100, 0x01000000, 0x01000000, 0x01111110, 0x00000000},
    // F
    {0x01111110, 0x01000000, 0x01000000, 0x01111100, 0x01000000, 0x01000000, 0x01000000, 0x00000000},
    // G
    {0x00111100, 0x01000010, 0x01000000, 0x01001110, 0x01000010, 0x01000010, 0x00111100, 0x00000000},
    // H
    {0x01000010, 0x01000010, 0x01000010, 0x01111110, 0x01000010, 0x01000010, 0x01000010, 0x00000000},
    // I
    {0x00111100, 0x00001000, 0x00001000, 0x00001000, 0x00001000, 0x00001000, 0x00111100, 0x00000000},
    // J
    {0x00011110, 0x00000100, 0x00000100, 0x00000100, 0x00000100, 0x01000100, 0x00111000, 0x00000000},
    // K
    {0x01000010, 0x01000100, 0x01001000, 0x01110000, 0x01001000, 0x01000100, 0x01000010, 0x00000000},
    // L
    {0x01000000, 0x01000000, 0x01000000, 0x01000000, 0x01000000, 0x01000000, 0x01111110, 0x00000000},
    // M
    {0x01000010, 0x01100110, 0x01011010, 0x01000010, 0x01000010, 0x01000010, 0x01000010, 0x00000000},
    // N
    {0x01000010, 0x01100010, 0x01010010, 0x01001010, 0x01000110, 0x01000010, 0x01000010, 0x00000000},
    // O
    {0x00111100, 0x01000010, 0x01000010, 0x01000010, 0x01000010, 0x01000010, 0x00111100, 0x00000000},
    // P
    {0x01111100, 0x01000010, 0x01000010, 0x01111100, 0x01000000, 0x01000000, 0x01000000, 0x00000000},
    // Q
    {0x00111100, 0x01000010, 0x01000010, 0x01000010, 0x01001010, 0x01000100, 0x00111010, 0x00000000},
    // R
    {0x01111100, 0x01000010, 0x01000010, 0x01111100, 0x01001000, 0x01000100, 0x01000010, 0x00000000},
    // S
    {0x00111100, 0x01000010, 0x01000000, 0x00111100, 0x00000010, 0x01000010, 0x00111100, 0x00000000},
    // T
    {0x01111110, 0x00001000, 0x00001000, 0x00001000, 0x00001000, 0x00001000, 0x00001000, 0x00000000},
    // U
    {0x01000010, 0x01000010, 0x01000010, 0x01000010, 0x01000010, 0x01000010, 0x00111100, 0x00000000},
    // V
    {0x01000010, 0x01000010, 0x01000010, 0x01000010, 0x00100100, 0x00011000, 0x00011000, 0x00000000},
    // W
    {0x01000010, 0x01000010, 0x01000010, 0x01000010, 0x01011010, 0x01100110, 0x01000010, 0x00000000},
    // X
    {0x01000010, 0x00100100, 0x00011000, 0x00011000, 0x00100100, 0x01000010, 0x01000010, 0x00000000},
    // Y
    {0x01000010, 0x00100100, 0x00011000, 0x00001000, 0x00001000, 0x00001000, 0x00001000, 0x00000000},
    // Z
    {0x01111110, 0x00000100, 0x00001000, 0x00010000, 0x00100000, 0x01000000, 0x01111110, 0x00000000},
};

//------------------------------------------------------------------------------
// Tile Graphics Data
//------------------------------------------------------------------------------
static void create_bg_tiles(void) {
    // Clear tile memory first
    memset(&tile_mem[0][0], 0, sizeof(TILE) * 16);
    
    // Tile 0: Empty (grass) - dark green pattern
    for (int i = 0; i < 8; i++) {
        tile_mem[0][0].data[i] = 0x11111111;
    }
    
    // Tile 1: Path - tan/brown
    for (int i = 0; i < 8; i++) {
        tile_mem[0][1].data[i] = 0x22222222;
    }
    
    // Tile 2: Water - blue
    for (int i = 0; i < 8; i++) {
        tile_mem[0][2].data[i] = 0x33333333;
    }
    
    // Tile 3: Tree/blocked - dark
    for (int i = 0; i < 8; i++) {
        tile_mem[0][3].data[i] = 0x44444444;
    }
    
    // Tile 4: UI dark
    for (int i = 0; i < 8; i++) {
        tile_mem[0][4].data[i] = 0x55555555;
    }
}

static void create_font_tiles(void) {
    // Create font tiles starting at tile 64
    for (int c = 0; c < 42; c++) {
        for (int row = 0; row < 8; row++) {
            u32 src = font_data[c][row];
            u32 dst = 0;
            for (int px = 0; px < 8; px++) {
                if (src & (1 << (7 - px))) {
                    dst |= (0xF << (px * 4));
                }
            }
            tile_mem[0][64 + c].data[row] = dst;
        }
    }
}

static void create_sprite_tiles(void) {
    // Cursor sprite (8x8) - yellow outline
    u32 cursor[8] = {
        0xFFFFFFFF,
        0xF000000F,
        0xF000000F,
        0xF000000F,
        0xF000000F,
        0xF000000F,
        0xF000000F,
        0xFFFFFFFF
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][0].data[i] = cursor[i];
    }
    
    // Enemy sprite - goblin (green humanoid)
    u32 goblin[8] = {
        0x00222200,
        0x02222220,
        0x02F22F20,
        0x02222220,
        0x00022000,
        0x02222220,
        0x02200220,
        0x02200220
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][1].data[i] = goblin[i];
    }
    
    // Wolf sprite (gray)
    u32 wolf[8] = {
        0x00555000,
        0x05F5F500,
        0x05555550,
        0x55555555,
        0x05555550,
        0x05050500,
        0x05050500,
        0x00000000
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][2].data[i] = wolf[i];
    }
    
    // Orc sprite (brown/red)
    u32 orc[8] = {
        0x00666600,
        0x06666660,
        0x06F66F60,
        0x06666660,
        0x66066066,
        0x06666660,
        0x06600660,
        0x06600660
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][3].data[i] = orc[i];
    }
    
    // Archer tower (blue)
    u32 archer[8] = {
        0x00033000,
        0x00333300,
        0x03333330,
        0x03F33F30,
        0x03333330,
        0x00333300,
        0x03300330,
        0x03300330
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][4].data[i] = archer[i];
    }
    
    // Mage tower (purple)
    u32 mage[8] = {
        0x00077000,
        0x00777700,
        0x07777770,
        0x07F77F70,
        0x07777770,
        0x00777700,
        0x00777700,
        0x00777700
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][5].data[i] = mage[i];
    }
    
    // Knight tower (silver/gray)
    u32 knight[8] = {
        0x00555500,
        0x05555550,
        0x05F55F50,
        0x05555550,
        0x55555555,
        0x05555550,
        0x05500550,
        0x05500550
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][6].data[i] = knight[i];
    }
    
    // Bullet/arrow
    u32 bullet[8] = {
        0x00000000,
        0x00000000,
        0x000FF000,
        0x00FFFF00,
        0x00FFFF00,
        0x000FF000,
        0x00000000,
        0x00000000
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][7].data[i] = bullet[i];
    }
    
    // Magic bullet (for mage)
    u32 magic[8] = {
        0x00000000,
        0x00077000,
        0x00777700,
        0x07777770,
        0x07777770,
        0x00777700,
        0x00077000,
        0x00000000
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][8].data[i] = magic[i];
    }
    
    // Crystal sprite
    u32 crystal[8] = {
        0x00088000,
        0x00888800,
        0x08888880,
        0x88888888,
        0x08888880,
        0x00888800,
        0x00088000,
        0x00000000
    };
    for (int i = 0; i < 8; i++) {
        tile_mem[4][9].data[i] = crystal[i];
    }
}

//------------------------------------------------------------------------------
// Palette Setup
//------------------------------------------------------------------------------
static void init_palettes(void) {
    // BG Palette
    pal_bg_mem[0] = RGB15(0, 0, 0);       // 0: transparent/black
    pal_bg_mem[1] = RGB15(4, 16, 4);      // 1: dark green (grass)
    pal_bg_mem[2] = RGB15(22, 18, 10);    // 2: tan (path)
    pal_bg_mem[3] = RGB15(6, 12, 24);     // 3: blue (water)
    pal_bg_mem[4] = RGB15(2, 8, 2);       // 4: very dark green (trees)
    pal_bg_mem[5] = RGB15(4, 4, 8);       // 5: UI dark
    pal_bg_mem[15] = RGB15(31, 31, 31);   // 15: white (font)
    
    // Sprite Palette
    pal_obj_mem[0] = RGB15(31, 0, 31);    // transparent (magenta)
    pal_obj_mem[1] = RGB15(8, 24, 8);     // green
    pal_obj_mem[2] = RGB15(10, 24, 8);    // light green
    pal_obj_mem[3] = RGB15(8, 12, 28);    // blue
    pal_obj_mem[4] = RGB15(20, 16, 8);    // brown
    pal_obj_mem[5] = RGB15(20, 20, 20);   // gray
    pal_obj_mem[6] = RGB15(28, 12, 12);   // red
    pal_obj_mem[7] = RGB15(24, 8, 24);    // purple
    pal_obj_mem[8] = RGB15(8, 28, 28);    // cyan (crystal)
    pal_obj_mem[15] = RGB15(31, 31, 0);   // yellow (cursor)
}

//------------------------------------------------------------------------------
// Map Functions
//------------------------------------------------------------------------------
static void init_map(void) {
    // Initialize all as grass
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            map_data[y][x] = TILE_EMPTY;
        }
    }
    
    // Create the path (snake-like pattern)
    // Entry from left at row 2
    for (int x = 0; x < 4; x++) map_data[2][x] = TILE_PATH;
    // Down to row 5
    for (int y = 2; y <= 5; y++) map_data[y][3] = TILE_PATH;
    // Right to column 8
    for (int x = 3; x <= 8; x++) map_data[5][x] = TILE_PATH;
    // Up to row 2
    for (int y = 2; y <= 5; y++) map_data[y][8] = TILE_PATH;
    // Right to column 12
    for (int x = 8; x <= 12; x++) map_data[2][x] = TILE_PATH;
    // Down to row 7
    for (int y = 2; y <= 7; y++) map_data[y][12] = TILE_PATH;
    // Right to exit
    for (int x = 12; x < MAP_WIDTH; x++) map_data[7][x] = TILE_PATH;
    
    // Add some decorative water and trees
    map_data[0][6] = TILE_WATER;
    map_data[0][7] = TILE_WATER;
    map_data[1][6] = TILE_WATER;
    map_data[8][2] = TILE_TREE;
    map_data[9][3] = TILE_TREE;
    map_data[8][10] = TILE_TREE;
    
    // Set up waypoints for enemy pathing
    waypoint_count = 0;
    waypoints[waypoint_count++] = (Waypoint){0 * TILE_SIZE, 2 * TILE_SIZE};
    waypoints[waypoint_count++] = (Waypoint){3 * TILE_SIZE, 2 * TILE_SIZE};
    waypoints[waypoint_count++] = (Waypoint){3 * TILE_SIZE, 5 * TILE_SIZE};
    waypoints[waypoint_count++] = (Waypoint){8 * TILE_SIZE, 5 * TILE_SIZE};
    waypoints[waypoint_count++] = (Waypoint){8 * TILE_SIZE, 2 * TILE_SIZE};
    waypoints[waypoint_count++] = (Waypoint){12 * TILE_SIZE, 2 * TILE_SIZE};
    waypoints[waypoint_count++] = (Waypoint){12 * TILE_SIZE, 7 * TILE_SIZE};
    waypoints[waypoint_count++] = (Waypoint){15 * TILE_SIZE, 7 * TILE_SIZE};
}

static void draw_map_bg(void) {
    // Draw map tiles (each map tile = 2x2 bg tiles for 16x16)
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            u8 tile = map_data[y][x];
            int bx = x * 2;
            int by = y * 2;
            se_mem[31][by * 32 + bx] = tile;
            se_mem[31][by * 32 + bx + 1] = tile;
            se_mem[31][(by + 1) * 32 + bx] = tile;
            se_mem[31][(by + 1) * 32 + bx + 1] = tile;
        }
    }
}

//------------------------------------------------------------------------------
// Text Drawing
//------------------------------------------------------------------------------
static int char_to_tile(char c) {
    if (c >= '0' && c <= '9') return 64 + (c - '0');
    if (c == ':') return 64 + 10;
    if (c == ' ') return 64 + 11;
    if (c == '-') return 64 + 12;
    if (c == '>') return 64 + 13;
    if (c == '<') return 64 + 14;
    if (c == '!') return 64 + 15;
    if (c >= 'A' && c <= 'Z') return 64 + 16 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 64 + 16 + (c - 'a');
    return 64 + 11;  // space for unknown
}

static void draw_text(int x, int y, const char* str) {
    int i = 0;
    while (str[i] != '\0' && x + i < 32) {
        se_mem[30][y * 32 + x + i] = char_to_tile(str[i]) | SE_PALBANK(1);
        i++;
    }
}

static void draw_number(int x, int y, int num) {
    char buf[12];
    int i = 0;
    int n = num;
    
    if (n == 0) {
        buf[i++] = '0';
    } else {
        if (n < 0) {
            buf[i++] = '-';
            n = -n;
        }
        char temp[10];
        int j = 0;
        while (n > 0) {
            temp[j++] = '0' + (n % 10);
            n /= 10;
        }
        while (j > 0) {
            buf[i++] = temp[--j];
        }
    }
    buf[i] = '\0';
    draw_text(x, y, buf);
}

//------------------------------------------------------------------------------
// UI Drawing
//------------------------------------------------------------------------------
static void draw_ui(void) {
    // Clear UI areas (top and bottom)
    for (int x = 0; x < 30; x++) {
        se_mem[30][x] = 4;  // Top row dark
        se_mem[30][32 + x] = 4;
        se_mem[30][19 * 32 + x] = 4;  // Bottom rows dark
    }
    
    // Draw UI text
    draw_text(0, 0, "CRYSTALS:");
    draw_number(10, 0, crystals);
    
    draw_text(15, 0, "GOLD:");
    draw_number(21, 0, gold);
    
    draw_text(0, 1, "WAVE:");
    draw_number(6, 1, wave);
    
    // Tower selection
    draw_text(15, 1, tower_names[selected_tower]);
    draw_number(26, 1, tower_costs[selected_tower]);
    
    if (game_over) {
        draw_text(8, 10, "GAME OVER!");
        draw_text(5, 12, "PRESS START");
    } else if (game_won) {
        draw_text(8, 10, "YOU WIN!");
        draw_text(5, 12, "PRESS START");
    } else if (!wave_active) {
        draw_text(2, 19, "START-WAVE  A-BUILD  LR-SELECT");
    } else {
        draw_text(6, 19, "A-BUILD  LR-SELECT TOWER");
    }
}

//------------------------------------------------------------------------------
// Game Logic
//------------------------------------------------------------------------------
static void init_game(void) {
    crystals = 20;
    gold = 100;
    wave = 0;
    wave_delay = 0;
    spawn_count = 0;
    spawn_timer = 0;
    spawn_max = 6;
    wave_active = 0;
    game_over = 0;
    game_won = 0;
    cursor_x = 5;
    cursor_y = 3;
    selected_tower = TOWER_ARCHER;
    
    memset(enemies, 0, sizeof(enemies));
    memset(towers, 0, sizeof(towers));
    memset(bullets, 0, sizeof(bullets));
    
    init_map();
    draw_map_bg();
}

static int get_empty_enemy_slot(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) return i;
    }
    return -1;
}

static int get_empty_tower_slot(void) {
    for (int i = 0; i < MAX_TOWERS; i++) {
        if (!towers[i].active) return i;
    }
    return -1;
}

static int get_empty_bullet_slot(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) return i;
    }
    return -1;
}

static void spawn_enemy(void) {
    int slot = get_empty_enemy_slot();
    if (slot < 0) return;
    
    Enemy* e = &enemies[slot];
    e->active = 1;
    e->x = waypoints[0].x << FP_SHIFT;
    e->y = waypoints[0].y << FP_SHIFT;
    e->path_idx = 0;
    e->flash = 0;
    
    // Vary enemy type based on wave
    int type_roll = (spawn_count + wave) % 6;
    if (type_roll < 3) {
        e->type = ENEMY_GOBLIN;
        e->hp = 20 + wave * 8;
        e->speed = 180;
    } else if (type_roll < 5) {
        e->type = ENEMY_WOLF;
        e->hp = 15 + wave * 5;
        e->speed = 280;
    } else {
        e->type = ENEMY_ORC;
        e->hp = 45 + wave * 15;
        e->speed = 120;
    }
    e->max_hp = e->hp;
}

static void place_tower(void) {
    if (map_data[cursor_y][cursor_x] != TILE_EMPTY) return;
    if (gold < tower_costs[selected_tower]) return;
    
    // Check if tower already exists at cursor
    for (int i = 0; i < MAX_TOWERS; i++) {
        if (towers[i].active && towers[i].x == cursor_x && towers[i].y == cursor_y) {
            return;
        }
    }
    
    int slot = get_empty_tower_slot();
    if (slot < 0) return;
    
    Tower* t = &towers[slot];
    t->active = 1;
    t->x = cursor_x;
    t->y = cursor_y;
    t->type = selected_tower;
    t->level = 1;
    t->cooldown = 0;
    
    switch (selected_tower) {
        case TOWER_ARCHER:
            t->range = 50;
            t->damage = 12;
            t->cooldown_max = 25;
            break;
        case TOWER_MAGE:
            t->range = 45;
            t->damage = 30;
            t->cooldown_max = 55;
            break;
        case TOWER_KNIGHT:
            t->range = 35;
            t->damage = 20;
            t->cooldown_max = 35;
            break;
    }
    
    gold -= tower_costs[selected_tower];
}

static int isqrt(int n) {
    if (n < 0) return 0;
    int x = n;
    int y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

static void fire_bullet(Tower* t, Enemy* e) {
    int slot = get_empty_bullet_slot();
    if (slot < 0) return;
    
    Bullet* b = &bullets[slot];
    b->active = 1;
    b->x = (t->x * TILE_SIZE + 8) << FP_SHIFT;
    b->y = (t->y * TILE_SIZE + 8) << FP_SHIFT;
    b->tower_type = t->type;
    b->lifetime = 60;
    
    // Calculate direction to enemy
    s32 ex = e->x + (8 << FP_SHIFT);
    s32 ey = e->y + (8 << FP_SHIFT);
    s32 dx = ex - b->x;
    s32 dy = ey - b->y;
    
    // Normalize and scale velocity
    int dist = isqrt((dx >> 4) * (dx >> 4) + (dy >> 4) * (dy >> 4));
    if (dist > 0) {
        int speed = 512;  // Bullet speed in fixed point
        b->dx = (dx / dist) * (speed >> 4);
        b->dy = (dy / dist) * (speed >> 4);
    }
    
    t->cooldown = t->cooldown_max;
}

static void update_enemies(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy* e = &enemies[i];
        if (!e->active) continue;
        
        if (e->flash > 0) e->flash--;
        
        // Move towards current waypoint
        if (e->path_idx >= waypoint_count) {
            // Reached end - damage crystals
            e->active = 0;
            crystals--;
            if (crystals <= 0) {
                game_over = 1;
            }
            continue;
        }
        
        s32 target_x = (waypoints[e->path_idx].x + 4) << FP_SHIFT;
        s32 target_y = (waypoints[e->path_idx].y + 4) << FP_SHIFT;
        
        s32 dx = target_x - e->x;
        s32 dy = target_y - e->y;
        
        // Simple movement
        if (dx > e->speed) e->x += e->speed;
        else if (dx < -e->speed) e->x -= e->speed;
        else e->x = target_x;
        
        if (dy > e->speed) e->y += e->speed;
        else if (dy < -e->speed) e->y -= e->speed;
        else e->y = target_y;
        
        // Check if reached waypoint
        if (e->x == target_x && e->y == target_y) {
            e->path_idx++;
        }
        
        // Check if dead
        if (e->hp <= 0) {
            e->active = 0;
            gold += 5 + e->type * 3;
        }
    }
}

static void update_towers(void) {
    for (int i = 0; i < MAX_TOWERS; i++) {
        Tower* t = &towers[i];
        if (!t->active) continue;
        
        if (t->cooldown > 0) {
            t->cooldown--;
            continue;
        }
        
        // Find nearest enemy in range
        int tx = t->x * TILE_SIZE + 8;
        int ty = t->y * TILE_SIZE + 8;
        Enemy* target = NULL;
        int min_dist = t->range * t->range;
        
        for (int j = 0; j < MAX_ENEMIES; j++) {
            Enemy* e = &enemies[j];
            if (!e->active) continue;
            
            int ex = (e->x >> FP_SHIFT) + 4;
            int ey = (e->y >> FP_SHIFT) + 4;
            int dx = ex - tx;
            int dy = ey - ty;
            int dist = dx * dx + dy * dy;
            
            if (dist < min_dist) {
                min_dist = dist;
                target = e;
            }
        }
        
        if (target != NULL) {
            fire_bullet(t, target);
        }
    }
}

static void update_bullets(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet* b = &bullets[i];
        if (!b->active) continue;
        
        b->x += b->dx;
        b->y += b->dy;
        b->lifetime--;
        
        if (b->lifetime <= 0) {
            b->active = 0;
            continue;
        }
        
        // Check collision with enemies
        int bx = b->x >> FP_SHIFT;
        int by = b->y >> FP_SHIFT;
        
        if (bx < 0 || bx >= 240 || by < 0 || by >= 160) {
            b->active = 0;
            continue;
        }
        
        for (int j = 0; j < MAX_ENEMIES; j++) {
            Enemy* e = &enemies[j];
            if (!e->active) continue;
            
            int ex = e->x >> FP_SHIFT;
            int ey = e->y >> FP_SHIFT;
            
            // Simple box collision
            if (bx >= ex && bx < ex + 12 && by >= ey && by < ey + 12) {
                // Hit!
                int damage = 10;
                switch (b->tower_type) {
                    case TOWER_ARCHER: damage = 12; break;
                    case TOWER_MAGE: damage = 30; break;
                    case TOWER_KNIGHT: damage = 20; break;
                }
                e->hp -= damage;
                e->flash = 4;
                b->active = 0;
                
                // Mage AOE damage
                if (b->tower_type == TOWER_MAGE) {
                    for (int k = 0; k < MAX_ENEMIES; k++) {
                        if (k == j || !enemies[k].active) continue;
                        int kx = enemies[k].x >> FP_SHIFT;
                        int ky = enemies[k].y >> FP_SHIFT;
                        int dx = ex - kx;
                        int dy = ey - ky;
                        if (dx * dx + dy * dy < 24 * 24) {
                            enemies[k].hp -= 15;
                            enemies[k].flash = 4;
                        }
                    }
                }
                break;
            }
        }
    }
}

static void update_waves(void) {
    if (game_over || game_won) return;
    
    if (!wave_active) {
        wave_delay++;
        return;
    }
    
    // Spawn enemies
    spawn_timer++;
    if (spawn_timer >= 45 && spawn_count < spawn_max) {
        spawn_enemy();
        spawn_count++;
        spawn_timer = 0;
    }
    
    // Check if wave is complete
    int active_count = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (enemies[i].active) active_count++;
    }
    
    if (spawn_count >= spawn_max && active_count == 0) {
        wave_active = 0;
        gold += 30 + wave * 10;  // Wave completion bonus
        
        if (wave >= 10) {
            game_won = 1;
        }
    }
}

static void handle_input(void) {
    key_poll();
    
    if (game_over || game_won) {
        if (key_hit(KEY_START)) {
            init_game();
        }
        return;
    }
    
    // Cursor movement
    if (key_hit(KEY_LEFT) && cursor_x > 0) cursor_x--;
    if (key_hit(KEY_RIGHT) && cursor_x < MAP_WIDTH - 1) cursor_x++;
    if (key_hit(KEY_UP) && cursor_y > 0) cursor_y--;
    if (key_hit(KEY_DOWN) && cursor_y < MAP_HEIGHT - 1) cursor_y++;
    
    // Tower selection
    if (key_hit(KEY_L)) {
        selected_tower--;
        if (selected_tower < TOWER_ARCHER) selected_tower = TOWER_KNIGHT;
    }
    if (key_hit(KEY_R)) {
        selected_tower++;
        if (selected_tower > TOWER_KNIGHT) selected_tower = TOWER_ARCHER;
    }
    
    // Place tower
    if (key_hit(KEY_A)) {
        place_tower();
    }
    
    // Start wave
    if (key_hit(KEY_START) && !wave_active) {
        wave++;
        wave_active = 1;
        spawn_count = 0;
        spawn_timer = 0;
        spawn_max = 6 + wave * 2;
        wave_delay = 0;
    }
}

//------------------------------------------------------------------------------
// Rendering
//------------------------------------------------------------------------------
static void render_sprites(void) {
    int spr = 0;
    
    // Cursor (blinking)
    cursor_blink++;
    if ((cursor_blink / 8) & 1) {
        obj_set_attr(&obj_mem[spr],
            ATTR0_Y(cursor_y * TILE_SIZE) | ATTR0_SQUARE,
            ATTR1_X(cursor_x * TILE_SIZE) | ATTR1_SIZE_8,
            ATTR2_ID(0) | ATTR2_PRIO(0) | ATTR2_PALBANK(1));
    } else {
        obj_hide(&obj_mem[spr]);
    }
    spr++;
    
    // Enemies
    for (int i = 0; i < MAX_ENEMIES && spr < 120; i++) {
        Enemy* e = &enemies[i];
        if (!e->active) continue;
        
        int x = (e->x >> FP_SHIFT) - 4;
        int y = (e->y >> FP_SHIFT) - 4;
        int tile = e->type;  // 1=goblin, 2=wolf, 3=orc
        int pal = (e->flash > 0) ? 0 : 0;  // Flash white when hit
        
        obj_set_attr(&obj_mem[spr],
            ATTR0_Y(y & 0xFF) | ATTR0_SQUARE,
            ATTR1_X(x & 0x1FF) | ATTR1_SIZE_8,
            ATTR2_ID(tile) | ATTR2_PRIO(1) | ATTR2_PALBANK(pal));
        spr++;
        
        // Draw HP bar above enemy
        if (e->hp < e->max_hp && spr < 120) {
            // Use a simple colored block
            int hp_width = (e->hp * 8) / e->max_hp;
            if (hp_width > 0) {
                // We'll skip HP bars for simplicity - they need more sprites
            }
        }
    }
    
    // Towers
    for (int i = 0; i < MAX_TOWERS && spr < 120; i++) {
        Tower* t = &towers[i];
        if (!t->active) continue;
        
        int x = t->x * TILE_SIZE + 4;
        int y = t->y * TILE_SIZE + 4;
        int tile = 3 + t->type;  // 4=archer, 5=mage, 6=knight
        
        obj_set_attr(&obj_mem[spr],
            ATTR0_Y(y) | ATTR0_SQUARE,
            ATTR1_X(x) | ATTR1_SIZE_8,
            ATTR2_ID(tile) | ATTR2_PRIO(2) | ATTR2_PALBANK(0));
        spr++;
    }
    
    // Bullets
    for (int i = 0; i < MAX_BULLETS && spr < 120; i++) {
        Bullet* b = &bullets[i];
        if (!b->active) continue;
        
        int x = (b->x >> FP_SHIFT) - 4;
        int y = (b->y >> FP_SHIFT) - 4;
        int tile = (b->tower_type == TOWER_MAGE) ? 8 : 7;
        
        obj_set_attr(&obj_mem[spr],
            ATTR0_Y(y & 0xFF) | ATTR0_SQUARE,
            ATTR1_X(x & 0x1FF) | ATTR1_SIZE_8,
            ATTR2_ID(tile) | ATTR2_PRIO(0) | ATTR2_PALBANK(0));
        spr++;
    }
    
    // Crystal indicator at exit (just for show)
    obj_set_attr(&obj_mem[spr],
        ATTR0_Y(7 * TILE_SIZE + 4) | ATTR0_SQUARE,
        ATTR1_X(14 * TILE_SIZE + 4) | ATTR1_SIZE_8,
        ATTR2_ID(9) | ATTR2_PRIO(1) | ATTR2_PALBANK(0));
    spr++;
    
    // Hide remaining sprites
    for (; spr < 128; spr++) {
        obj_hide(&obj_mem[spr]);
    }
}

//------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------
int main(void) {
    // Set up interrupts for VBlank
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    // Initialize display
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // BG0: Map tiles
    REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32 | BG_PRIO(2);
    
    // BG1: UI text overlay
    REG_BG1CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32 | BG_PRIO(0);
    pal_bg_mem[16 + 15] = RGB15(31, 31, 31);  // Text palette bank 1
    
    // Initialize palettes
    init_palettes();
    
    // Create graphics
    create_bg_tiles();
    create_font_tiles();
    create_sprite_tiles();
    
    // Initialize OAM
    oam_init(obj_mem, 128);
    
    // Initialize game
    init_game();
    
    // Main game loop
    while (1) {
        VBlankIntrWait();
        
        handle_input();
        update_waves();
        update_enemies();
        update_towers();
        update_bullets();
        
        render_sprites();
        draw_ui();
    }
    
    return 0;
}
