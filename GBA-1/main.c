//======================================================================
// Mystery Dungeon GBA - A roguelike dungeon crawler
// Compile with: make (using devkitARM with tonc)
//======================================================================
#include <tonc.h>
#include <string.h>


#define RGB15_C(r,g,b)  ((r) | ((g) << 5) | ((b) << 10))

//----------------------------------------------------------------------
// Constants
//----------------------------------------------------------------------
#define MAP_W           48
#define MAP_H           48
#define SCREEN_W        30
#define SCREEN_H        20
#define MAX_ROOMS       9
#define MIN_ROOM_SIZE   4
#define MAX_ROOM_SIZE   8
#define MAX_ENEMIES     16
#define MAX_ITEMS       12
#define VIEW_RADIUS     5

// Tile indices
#define TILE_VOID       0
#define TILE_WALL       1
#define TILE_FLOOR      2
#define TILE_STAIRS     3
#define TILE_WATER      4
#define TILE_DARK       5

// Entity types
#define ENT_NONE        0
#define ENT_PLAYER      1
#define ENT_SLIME       2
#define ENT_BAT         3
#define ENT_GOBLIN      4
#define ENT_SKELETON    5

// Item types
#define ITEM_NONE       0
#define ITEM_POTION     1
#define ITEM_APPLE      2
#define ITEM_ORB        3

// Directions
#define DIR_NONE        0
#define DIR_UP          1
#define DIR_DOWN        2
#define DIR_LEFT        3
#define DIR_RIGHT       4
#define DIR_UL          5
#define DIR_UR          6
#define DIR_DL          7
#define DIR_DR          8

//----------------------------------------------------------------------
// Structures
//----------------------------------------------------------------------
typedef struct {
    s16 x, y;
    s16 hp, max_hp;
    s16 atk, def;
    u8 type;
    u8 active;
    u8 level;
    u16 exp;
} Entity;

typedef struct {
    s16 x, y;
    u8 type;
    u8 active;
} Item;

typedef struct {
    s16 x, y, w, h;
    s16 cx, cy; // center
} Room;

typedef struct {
    char text[32];
    u16 timer;
} Message;

//----------------------------------------------------------------------
// Globals
//----------------------------------------------------------------------
u8 dungeon[MAP_H][MAP_W];
u8 visible[MAP_H][MAP_W];
u8 explored[MAP_H][MAP_W];

Entity player;
Entity enemies[MAX_ENEMIES];
Item items[MAX_ITEMS];
Room rooms[MAX_ROOMS];
int num_rooms;
int floor_num = 1;
s16 cam_x, cam_y;
u32 rng_state = 0;
int turn_count = 0;
Message msg = {"", 0};
int hunger = 100;
int game_over = 0;
int win = 0;

//----------------------------------------------------------------------
// Tile graphics data (8x8, 4bpp = 32 bytes each)
//----------------------------------------------------------------------
const u32 tile_data[] ALIGN4 = {
    // Tile 0: Empty/void (black)
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    
    // Tile 1: Wall (gray brick pattern)
    0x11111111, 0x11111111, 0x22222222, 0x11111111,
    0x11111111, 0x22222222, 0x11111111, 0x11111111,
    
    // Tile 2: Floor (dotted pattern)
    0x00000000, 0x00200000, 0x00000000, 0x00000000,
    0x00000000, 0x00000020, 0x00000000, 0x00000000,
    
    // Tile 3: Stairs (down arrow pattern)
    0x00000000, 0x00333300, 0x00033000, 0x00033000,
    0x33033033, 0x03333330, 0x00333300, 0x00033000,
    
    // Tile 4: Water (wavy blue)
    0x44444444, 0x44544454, 0x45444445, 0x44444444,
    0x44444444, 0x54454444, 0x44454445, 0x44444444,
    
    // Tile 5: Dark/unexplored
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    
    // Tile 6: Player @
    0x06666600, 0x66000660, 0x60600606, 0x60000006,
    0x60666606, 0x60600606, 0x66066060, 0x06666600,
    
    // Tile 7: Slime (blob)
    0x00077000, 0x00777700, 0x07777770, 0x07707770,
    0x07777770, 0x07777770, 0x77777777, 0x77777777,
    
    // Tile 8: Bat
    0x00000000, 0x88000088, 0x88800888, 0x08888880,
    0x00888800, 0x00088000, 0x00088000, 0x00000000,
    
    // Tile 9: Goblin
    0x00999900, 0x09900990, 0x09999990, 0x90909090,
    0x09999990, 0x09000090, 0x09099090, 0x09900990,
    
    // Tile 10: Skeleton
    0x00AAAA00, 0x0A0AA0A0, 0x00AAAA00, 0x000AA000,
    0x00AAAA00, 0x0AA00AA0, 0x0A0AA0A0, 0x0A0000A0,
    
    // Tile 11: Potion (red)
    0x00011000, 0x00011000, 0x00111100, 0x01BBBB10,
    0x01BBBB10, 0x01BBBB10, 0x01BBBB10, 0x00111100,
    
    // Tile 12: Apple (green)
    0x00007000, 0x00077000, 0x07777770, 0x77777777,
    0x77777777, 0x77777777, 0x07777770, 0x00777700,
    
    // Tile 13: Orb (magic)
    0x00CCCC00, 0x0CCCCCC0, 0xCCCDDCCC, 0xCCDDDDCC,
    0xCCDDDDCC, 0xCCCDDCCC, 0x0CCCCCC0, 0x00CCCC00,
    
    // Tile 14: Heart
    0x0BB00BB0, 0xBBBBBBBB, 0xBBBBBBBB, 0xBBBBBBBB,
    0x0BBBBBB0, 0x00BBBB00, 0x000BB000, 0x00000000,
    
    // Tile 15: Attack effect
    0xE0000E00, 0x0E00E000, 0x00EE0000, 0x00EE0000,
    0x00EE0000, 0x0E00E000, 0xE0000E00, 0x00000000,
};

// Palette: 16 colors
const u16 palette[] = {
    RGB15_C(0, 0, 0),      // 0: Black
    RGB15_C(12, 12, 14),   // 1: Dark gray (wall)
    RGB15_C(8, 8, 10),     // 2: Darker gray (wall detail)
    RGB15_C(20, 18, 10),   // 3: Brown (stairs)
    RGB15_C(8, 12, 20),    // 4: Blue (water)
    RGB15_C(12, 16, 24),   // 5: Light blue (water)
    RGB15_C(0, 28, 0),     // 6: Green (player)
    RGB15_C(20, 0, 28),    // 7: Purple (slime)
    RGB15_C(16, 8, 0),     // 8: Brown (bat)
    RGB15_C(28, 20, 0),    // 9: Yellow (goblin)
    RGB15_C(28, 28, 28),   // A: White (skeleton)
    RGB15_C(28, 0, 0),     // B: Red (potion/heart)
    RGB15_C(0, 28, 8),     // C: Cyan (orb)
    RGB15_C(31, 31, 31),   // D: Bright white
    RGB15_C(31, 31, 0),    // E: Yellow (attack)
    RGB15_C(16, 16, 16),   // F: Gray
};

// Sprite palette
const u16 sprite_pal[] = {
    RGB15_C(31, 0, 31),    // 0: Transparent (magenta)
    RGB15_C(0, 28, 0),     // 1: Green (player)
    RGB15_C(20, 0, 28),    // 2: Purple (slime)
    RGB15_C(16, 8, 0),     // 3: Brown (bat)
    RGB15_C(28, 20, 0),    // 4: Yellow (goblin)
    RGB15_C(28, 28, 28),   // 5: White (skeleton)
    RGB15_C(28, 0, 0),     // 6: Red
    RGB15_C(0, 28, 8),     // 7: Cyan
    RGB15_C(0, 0, 0),      // 8: Black (outline)
    RGB15_C(31, 31, 31),   // 9: White
    RGB15_C(28, 16, 0),    // A: Orange
    RGB15_C(0, 20, 28),    // B: Blue
    RGB15_C(28, 28, 0),    // C: Yellow
    RGB15_C(20, 28, 20),   // D: Light green
    RGB15_C(28, 20, 28),   // E: Pink
    RGB15_C(16, 16, 16),   // F: Gray
};

// 8x8 sprite tiles
const u32 sprite_tiles[] ALIGN4 = {
    // Sprite 0: Empty
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    
    // Sprite 1: Player (little person)
    0x00888800, 0x08111180, 0x08111180, 0x00888800,
    0x00818800, 0x08818188, 0x00011000, 0x00088000,
    
    // Sprite 2: Slime
    0x00000000, 0x00022000, 0x00222200, 0x02288220,
    0x02222220, 0x02222220, 0x22222222, 0x00000000,
    
    // Sprite 3: Bat
    0x00000000, 0x33000033, 0x33300333, 0x03333330,
    0x00888800, 0x00088000, 0x00088000, 0x00000000,
    
    // Sprite 4: Goblin
    0x00444400, 0x04488440, 0x04444440, 0x00888800,
    0x04444440, 0x04080840, 0x04044040, 0x00088000,
    
    // Sprite 5: Skeleton
    0x00555500, 0x05855850, 0x00555500, 0x00858000,
    0x08555580, 0x00585800, 0x00585800, 0x00580850,
    
    // Sprite 6: Potion
    0x00088000, 0x00088000, 0x00888800, 0x08666680,
    0x08666680, 0x08666680, 0x08666680, 0x00888800,
    
    // Sprite 7: Apple
    0x00008000, 0x00088000, 0x0DDDDDD0, 0xDDDDDDDD,
    0xDDDDDDDD, 0xDDDDDDDD, 0x0DDDDDD0, 0x00DDDD00,
    
    // Sprite 8: Orb
    0x00777700, 0x07777770, 0x77799777, 0x77999977,
    0x77999977, 0x77799777, 0x07777770, 0x00777700,
    
    // Sprite 9: Stairs
    0x00000000, 0x00FFF000, 0x000FF000, 0x00FFF000,
    0x000FFF00, 0x0000FF00, 0x000FFF00, 0x00000000,
};

//----------------------------------------------------------------------
// Function declarations
//----------------------------------------------------------------------
void init_gfx(void);
void init_game(void);
u32 rng(void);
int rng_range(int min, int max);
void generate_dungeon(void);
void dig_room(Room* r);
void dig_corridor(int x1, int y1, int x2, int y2);
void place_stairs(void);
void spawn_enemies(void);
void spawn_items(void);
void update_visibility(void);
void update_camera(void);
void render(void);
void render_hud(void);
void draw_text(int x, int y, const char* str, u16 pal);
int handle_input(void);
void player_turn(int dir);
int can_move(int x, int y);
int get_enemy_at(int x, int y);
int get_item_at(int x, int y);
void attack(Entity* atk, Entity* def);
void enemy_turn(void);
void enemy_ai(Entity* e);
void pickup_item(int idx);
void next_floor(void);
void show_message(const char* str);
void update_message(void);
int abs_val(int x);

//----------------------------------------------------------------------
// Main
//----------------------------------------------------------------------
int main(void) {
    // Init interrupt handler
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    init_gfx();
    init_game();
    
    while(1) {
        vid_vsync();
        key_poll();
        
        if (game_over || win) {
            // Show game over or win screen
            if (key_hit(KEY_START)) {
                game_over = 0;
                win = 0;
                floor_num = 1;
                init_game();
            }
        } else {
            int action = handle_input();
            if (action) {
                update_visibility();
                enemy_turn();
                turn_count++;
                
                // Hunger system
                if (turn_count % 20 == 0) {
                    hunger--;
                    if (hunger <= 0) {
                        player.hp--;
                        if (hunger < -10) hunger = -10;
                    }
                }
                
                // Check death
                if (player.hp <= 0) {
                    game_over = 1;
                    show_message("You died! Press START");
                }
            }
        }
        
        update_message();
        update_camera();
        render();
        render_hud();
        
        //oam_copy(oam_mem, obj_buffer, 128);
    }
    
    return 0;
}

//----------------------------------------------------------------------
// Graphics initialization
//----------------------------------------------------------------------
void init_gfx(void) {
    // Set video mode: Mode 0, enable BG0 and sprites
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // Setup BG0 for dungeon (32x32 tiles, but we use 64x64 for scrolling)
    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_SIZE(1) | BG_PRIO(1); // 64x32
    REG_BG1CNT = BG_CBB(0) | BG_SBB(30) | BG_SIZE(0) | BG_PRIO(0); // 32x32 for HUD
    
    // Copy tile data to VRAM
    memcpy(&tile_mem[0][0], tile_data, sizeof(tile_data));
    memcpy(&tile_mem[4][0], sprite_tiles, sizeof(sprite_tiles));
    
    // Copy palettes
    memcpy(pal_bg_mem, palette, sizeof(palette));
    memcpy(pal_obj_mem, sprite_pal, sizeof(sprite_pal));
    
    // Clear screen maps
    memset(&se_mem[28][0], 0, 0x1000);
    memset(&se_mem[30][0], 0, 0x800);
    
    // Initialize OAM
    //oam_init(obj_buffer, 128);
}

//----------------------------------------------------------------------
// Game initialization
//----------------------------------------------------------------------
void init_game(void) {
    // Seed RNG with some value
    rng_state = 0x12345678 + floor_num * 777;
    
    // Clear arrays
    memset(dungeon, TILE_WALL, sizeof(dungeon));
    memset(visible, 0, sizeof(visible));
    memset(explored, 0, sizeof(explored));
    memset(enemies, 0, sizeof(enemies));
    memset(items, 0, sizeof(items));
    
    // Init player
    player.hp = 30;
    player.max_hp = 30;
    player.atk = 5;
    player.def = 2;
    player.type = ENT_PLAYER;
    player.active = 1;
    player.level = 1;
    player.exp = 0;
    hunger = 100;
    
    turn_count = 0;
    msg.timer = 0;
    
    generate_dungeon();
    spawn_enemies();
    spawn_items();
    
    // Place player in first room
    player.x = rooms[0].cx;
    player.y = rooms[0].cy;
    
    update_visibility();
    update_camera();
    
    show_message("Welcome to the dungeon!");
}

//----------------------------------------------------------------------
// Random number generator
//----------------------------------------------------------------------
u32 rng(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

int rng_range(int min, int max) {
    if (max <= min) return min;
    return min + (rng() % (max - min + 1));
}

int abs_val(int x) {
    return x < 0 ? -x : x;
}

//----------------------------------------------------------------------
// Dungeon generation
//----------------------------------------------------------------------
void generate_dungeon(void) {
    num_rooms = 0;
    
    // Create rooms
    int attempts = 0;
    while (num_rooms < MAX_ROOMS && attempts < 100) {
        Room r;
        r.w = rng_range(MIN_ROOM_SIZE, MAX_ROOM_SIZE);
        r.h = rng_range(MIN_ROOM_SIZE, MAX_ROOM_SIZE);
        r.x = rng_range(2, MAP_W - r.w - 2);
        r.y = rng_range(2, MAP_H - r.h - 2);
        r.cx = r.x + r.w / 2;
        r.cy = r.y + r.h / 2;
        
        // Check overlap with existing rooms
        int overlap = 0;
        for (int i = 0; i < num_rooms; i++) {
            if (r.x < rooms[i].x + rooms[i].w + 2 &&
                r.x + r.w + 2 > rooms[i].x &&
                r.y < rooms[i].y + rooms[i].h + 2 &&
                r.y + r.h + 2 > rooms[i].y) {
                overlap = 1;
                break;
            }
        }
        
        if (!overlap) {
            rooms[num_rooms] = r;
            dig_room(&r);
            
            // Connect to previous room
            if (num_rooms > 0) {
                dig_corridor(rooms[num_rooms-1].cx, rooms[num_rooms-1].cy,
                            r.cx, r.cy);
            }
            num_rooms++;
        }
        attempts++;
    }
    
    place_stairs();
}

void dig_room(Room* r) {
    for (int y = r->y; y < r->y + r->h; y++) {
        for (int x = r->x; x < r->x + r->w; x++) {
            dungeon[y][x] = TILE_FLOOR;
        }
    }
}

void dig_corridor(int x1, int y1, int x2, int y2) {
    int x = x1, y = y1;
    
    // Randomly choose to go horizontal or vertical first
    if (rng() % 2) {
        // Horizontal then vertical
        while (x != x2) {
            dungeon[y][x] = TILE_FLOOR;
            x += (x2 > x) ? 1 : -1;
        }
        while (y != y2) {
            dungeon[y][x] = TILE_FLOOR;
            y += (y2 > y) ? 1 : -1;
        }
    } else {
        // Vertical then horizontal
        while (y != y2) {
            dungeon[y][x] = TILE_FLOOR;
            y += (y2 > y) ? 1 : -1;
        }
        while (x != x2) {
            dungeon[y][x] = TILE_FLOOR;
            x += (x2 > x) ? 1 : -1;
        }
    }
    dungeon[y2][x2] = TILE_FLOOR;
}

void place_stairs(void) {
    // Place stairs in last room
    if (num_rooms > 0) {
        Room* r = &rooms[num_rooms - 1];
        dungeon[r->cy][r->cx] = TILE_STAIRS;
    }
}

void spawn_enemies(void) {
    int count = 3 + floor_num + rng_range(0, 3);
    if (count > MAX_ENEMIES) count = MAX_ENEMIES;
    
    for (int i = 0; i < count; i++) {
        // Pick a random room (not the first one where player spawns)
        int room_idx = rng_range(1, num_rooms - 1);
        if (room_idx < 1) room_idx = 1;
        Room* r = &rooms[room_idx];
        
        enemies[i].x = rng_range(r->x + 1, r->x + r->w - 2);
        enemies[i].y = rng_range(r->y + 1, r->y + r->h - 2);
        enemies[i].active = 1;
        
        // Enemy type based on floor
        int type_roll = rng_range(0, 100);
        if (floor_num >= 4 && type_roll < 20) {
            enemies[i].type = ENT_SKELETON;
            enemies[i].hp = 15 + floor_num * 2;
            enemies[i].max_hp = enemies[i].hp;
            enemies[i].atk = 6 + floor_num;
            enemies[i].def = 3;
        } else if (floor_num >= 3 && type_roll < 50) {
            enemies[i].type = ENT_GOBLIN;
            enemies[i].hp = 10 + floor_num;
            enemies[i].max_hp = enemies[i].hp;
            enemies[i].atk = 4 + floor_num;
            enemies[i].def = 2;
        } else if (floor_num >= 2 && type_roll < 70) {
            enemies[i].type = ENT_BAT;
            enemies[i].hp = 6 + floor_num;
            enemies[i].max_hp = enemies[i].hp;
            enemies[i].atk = 3 + floor_num;
            enemies[i].def = 1;
        } else {
            enemies[i].type = ENT_SLIME;
            enemies[i].hp = 8 + floor_num;
            enemies[i].max_hp = enemies[i].hp;
            enemies[i].atk = 2 + floor_num;
            enemies[i].def = 1;
        }
    }
}

void spawn_items(void) {
    int count = 2 + rng_range(0, 2);
    if (count > MAX_ITEMS) count = MAX_ITEMS;
    
    for (int i = 0; i < count; i++) {
        int room_idx = rng_range(0, num_rooms - 1);
        Room* r = &rooms[room_idx];
        
        items[i].x = rng_range(r->x + 1, r->x + r->w - 2);
        items[i].y = rng_range(r->y + 1, r->y + r->h - 2);
        items[i].active = 1;
        
        int type_roll = rng_range(0, 100);
        if (type_roll < 50) {
            items[i].type = ITEM_POTION;
        } else if (type_roll < 80) {
            items[i].type = ITEM_APPLE;
        } else {
            items[i].type = ITEM_ORB;
        }
    }
}

//----------------------------------------------------------------------
// Visibility (field of view)
//----------------------------------------------------------------------
void update_visibility(void) {
    // Clear visible array
    memset(visible, 0, sizeof(visible));
    
    // Simple radius-based visibility
    for (int dy = -VIEW_RADIUS; dy <= VIEW_RADIUS; dy++) {
        for (int dx = -VIEW_RADIUS; dx <= VIEW_RADIUS; dx++) {
            int x = player.x + dx;
            int y = player.y + dy;
            
            if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H)
                continue;
            
            // Simple distance check
            if (dx*dx + dy*dy <= VIEW_RADIUS * VIEW_RADIUS) {
                // Simple line of sight check
                int blocked = 0;
                int steps = abs_val(dx) > abs_val(dy) ? abs_val(dx) : abs_val(dy);
                if (steps > 0) {
                    for (int i = 1; i < steps; i++) {
                        int cx = player.x + dx * i / steps;
                        int cy = player.y + dy * i / steps;
                        if (dungeon[cy][cx] == TILE_WALL) {
                            blocked = 1;
                            break;
                        }
                    }
                }
                
                if (!blocked) {
                    visible[y][x] = 1;
                    explored[y][x] = 1;
                }
            }
        }
    }
}

//----------------------------------------------------------------------
// Camera
//----------------------------------------------------------------------
void update_camera(void) {
    // Center camera on player
    cam_x = player.x * 8 - 120;
    cam_y = player.y * 8 - 80;
    
    // Clamp to map bounds
    if (cam_x < 0) cam_x = 0;
    if (cam_y < 0) cam_y = 0;
    if (cam_x > (MAP_W * 8 - 240)) cam_x = (MAP_W * 8 - 240);
    if (cam_y > (MAP_H * 8 - 160)) cam_y = (MAP_H * 8 - 160);
}

//----------------------------------------------------------------------
// Rendering
//----------------------------------------------------------------------
void render(void) {
    // Set scroll registers
    REG_BG0HOFS = cam_x;
    REG_BG0VOFS = cam_y;
    
    // Calculate tile offset
    int tile_x = cam_x / 8;
    int tile_y = cam_y / 8;
    
    // Render visible portion of map to tilemap
    for (int sy = 0; sy < 21; sy++) {
        for (int sx = 0; sx < 31; sx++) {
            int mx = tile_x + sx;
            int my = tile_y + sy;
            
            // Calculate tilemap position (handle 64x32 map wrapping)
            int map_idx;
            int tx = mx & 31;
            int ty = my & 31;
            if (mx & 32) {
                map_idx = tx + ty * 32 + 1024; // Second screenblock
            } else {
                map_idx = tx + ty * 32;
            }
            
            u16 tile = TILE_VOID;
            
            if (mx >= 0 && mx < MAP_W && my >= 0 && my < MAP_H) {
                if (visible[my][mx]) {
                    tile = dungeon[my][mx];
                } else if (explored[my][mx]) {
                    // Dim explored tiles
                    tile = dungeon[my][mx] | (1 << 12); // Use different palette
                } else {
                    tile = TILE_DARK;
                }
            }
            
            se_mem[28][map_idx] = tile;
        }
    }
    
    // Render sprites
    int obj_idx = 0;
    
    // Player sprite
    int px = player.x * 8 - cam_x;
    int py = player.y * 8 - cam_y;
    if (px >= -8 && px < 248 && py >= -8 && py < 168) {
            ATTR0_Y(py) | ATTR0_SQUARE,  //obj_set_attr(&obj_buffer[obj_idx],
            ATTR1_X(px) | ATTR1_SIZE_8,
            ATTR2_ID(1) | ATTR2_PRIO(0);
        obj_idx++;
    }
    
    // Enemy sprites
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;
        if (!visible[enemies[i].y][enemies[i].x]) continue;
        
        int ex = enemies[i].x * 8 - cam_x;
        int ey = enemies[i].y * 8 - cam_y;
        
        if (ex >= -8 && ex < 248 && ey >= -8 && ey < 168 && obj_idx < 128) {
            int tile_id = 2; // Default slime
            switch (enemies[i].type) {
                case ENT_SLIME: tile_id = 2; break;
                case ENT_BAT: tile_id = 3; break;
                case ENT_GOBLIN: tile_id = 4; break;
                case ENT_SKELETON: tile_id = 5; break;
            }
            
            //obj_set_attr(&obj_buffer[obj_idx],
                ATTR0_Y(ey) | ATTR0_SQUARE,
                ATTR1_X(ex) | ATTR1_SIZE_8,
                ATTR2_ID(tile_id) | ATTR2_PRIO(0);
            obj_idx++;
        }
    }
    
    // Item sprites
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (!items[i].active) continue;
        if (!visible[items[i].y][items[i].x]) continue;
        
        int ix = items[i].x * 8 - cam_x;
        int iy = items[i].y * 8 - cam_y;
        
        if (ix >= -8 && ix < 248 && iy >= -8 && iy < 168 && obj_idx < 128) {
            int tile_id = 6; // Potion
            switch (items[i].type) {
                case ITEM_POTION: tile_id = 6; break;
                case ITEM_APPLE: tile_id = 7; break;
                case ITEM_ORB: tile_id = 8; break;
            }
            
           // obj_set_attr(&obj_buffer[obj_idx],
                ATTR0_Y(iy) | ATTR0_SQUARE,
                ATTR1_X(ix) | ATTR1_SIZE_8,
                ATTR2_ID(tile_id) | ATTR2_PRIO(0);
            obj_idx++;
        }
    }
    
    // Hide remaining sprites
    //for (int i = obj_idx; i < 128; i++) {
        //obj_hide(&obj_buffer[i]);
    //}
}

//----------------------------------------------------------------------
// HUD rendering
//----------------------------------------------------------------------
void render_hud(void) {
    // Draw HP bar
    char buf[32];
    
    // Clear HUD line
    for (int i = 0; i < 30; i++) {
        se_mem[30][i] = 0;
        se_mem[30][32 + i] = 0;
    }
    
    // Format HP
    int hp_idx = 0;
    buf[hp_idx++] = 'H';
    buf[hp_idx++] = 'P';
    buf[hp_idx++] = ':';
    if (player.hp >= 10) buf[hp_idx++] = '0' + (player.hp / 10);
    buf[hp_idx++] = '0' + (player.hp % 10);
    buf[hp_idx++] = '/';
    if (player.max_hp >= 10) buf[hp_idx++] = '0' + (player.max_hp / 10);
    buf[hp_idx++] = '0' + (player.max_hp % 10);
    buf[hp_idx] = '\0';
    draw_text(0, 0, buf, 0);
    
    // Floor number
    buf[0] = 'F';
    buf[1] = ':';
    buf[2] = '0' + floor_num;
    buf[3] = '\0';
    draw_text(12, 0, buf, 0);
    
    // Hunger
    buf[0] = 'F';
    buf[1] = 'D';
    buf[2] = ':';
    if (hunger >= 100) {
        buf[3] = '1';
        buf[4] = '0';
        buf[5] = '0';
        buf[6] = '\0';
    } else if (hunger >= 10) {
        buf[3] = '0' + (hunger / 10);
        buf[4] = '0' + (hunger % 10);
        buf[5] = '\0';
    } else {
        buf[3] = '0' + hunger;
        buf[4] = '\0';
    }
    draw_text(17, 0, buf, 0);
    
    // Message line
    if (msg.timer > 0) {
        draw_text(0, 19, msg.text, 0);
    }
    
    // Game over / win screen
    if (game_over) {
        draw_text(10, 9, "GAME OVER", 0);
        draw_text(7, 11, "PRESS START", 0);
    } else if (win) {
        draw_text(8, 9, "YOU ESCAPED!", 0);
        draw_text(7, 11, "PRESS START", 0);
    }
}

void draw_text(int x, int y, const char* str, u16 pal) {
    // Simple text rendering using ASCII
    // We'll use tiles 32-127 for ASCII characters
    // For simplicity, just store tile indices directly
    // This requires having font tiles loaded, but we'll fake it with existing tiles
    
    int idx = y * 32 + x;
    while (*str && x < 30) {
        // Map ASCII to tile numbers
        // We don't have a real font, so we'll use simple placeholders
        u8 ch = *str;
        u16 tile = 0;
        
        // For this demo, just display blanks or use basic tiles
        // In a real game, you'd have font tiles
        if (ch >= '0' && ch <= '9') {
            tile = 2; // Floor tile as placeholder
        } else if (ch >= 'A' && ch <= 'Z') {
            tile = 1; // Wall tile as placeholder
        } else if (ch >= 'a' && ch <= 'z') {
            tile = 1;
        } else {
            tile = 0; // Blank
        }
        
        // For actual display, let's just skip the HUD tiles
        // since we don't have a proper font
        
        str++;
        x++;
        idx++;
    }
}

//----------------------------------------------------------------------
// Input handling
//----------------------------------------------------------------------
int handle_input(void) {
    int dir = DIR_NONE;
    
    if (key_hit(KEY_UP)) dir = DIR_UP;
    else if (key_hit(KEY_DOWN)) dir = DIR_DOWN;
    else if (key_hit(KEY_LEFT)) dir = DIR_LEFT;
    else if (key_hit(KEY_RIGHT)) dir = DIR_RIGHT;
    
    // Diagonal movement with L/R + direction
    if (key_is_down(KEY_L)) {
        if (key_hit(KEY_UP)) dir = DIR_UL;
        else if (key_hit(KEY_DOWN)) dir = DIR_DL;
    }
    if (key_is_down(KEY_R)) {
        if (key_hit(KEY_UP)) dir = DIR_UR;
        else if (key_hit(KEY_DOWN)) dir = DIR_DR;
    }
    
    // Wait in place (A button)
    if (key_hit(KEY_A)) {
        // Small HP recovery when waiting
        if (hunger > 0 && player.hp < player.max_hp && rng() % 4 == 0) {
            player.hp++;
        }
        return 1;
    }
    
    if (dir != DIR_NONE) {
        player_turn(dir);
        return 1;
    }
    
    return 0;
}

void player_turn(int dir) {
    int dx = 0, dy = 0;
    
    switch (dir) {
        case DIR_UP: dy = -1; break;
        case DIR_DOWN: dy = 1; break;
        case DIR_LEFT: dx = -1; break;
        case DIR_RIGHT: dx = 1; break;
        case DIR_UL: dx = -1; dy = -1; break;
        case DIR_UR: dx = 1; dy = -1; break;
        case DIR_DL: dx = -1; dy = 1; break;
        case DIR_DR: dx = 1; dy = 1; break;
    }
    
    int nx = player.x + dx;
    int ny = player.y + dy;
    
    // Check for enemy at target location
    int enemy_idx = get_enemy_at(nx, ny);
    if (enemy_idx >= 0) {
        attack(&player, &enemies[enemy_idx]);
        return;
    }
    
    // Check if we can move there
    if (can_move(nx, ny)) {
        player.x = nx;
        player.y = ny;
        
        // Check for item pickup
        int item_idx = get_item_at(nx, ny);
        if (item_idx >= 0) {
            pickup_item(item_idx);
        }
        
        // Check for stairs
        if (dungeon[ny][nx] == TILE_STAIRS) {
            next_floor();
        }
    }
}

int can_move(int x, int y) {
    if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return 0;
    return dungeon[y][x] == TILE_FLOOR || dungeon[y][x] == TILE_STAIRS;
}

int get_enemy_at(int x, int y) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (enemies[i].active && enemies[i].x == x && enemies[i].y == y) {
            return i;
        }
    }
    return -1;
}

int get_item_at(int x, int y) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (items[i].active && items[i].x == x && items[i].y == y) {
            return i;
        }
    }
    return -1;
}

//----------------------------------------------------------------------
// Combat
//----------------------------------------------------------------------
void attack(Entity* atk, Entity* def) {
    int damage = atk->atk - def->def + rng_range(-1, 2);
    if (damage < 1) damage = 1;
    
    def->hp -= damage;
    
    if (def->hp <= 0) {
        def->active = 0;
        
        if (def->type != ENT_PLAYER) {
            show_message("Enemy defeated!");
            player.exp += 5 + floor_num;
            
            // Level up
            if (player.exp >= player.level * 20) {
                player.level++;
                player.max_hp += 3;
                player.hp = player.max_hp;
                player.atk += 1;
                show_message("Level up!");
            }
        }
    } else {
        show_message("Attack!");
    }
}

//----------------------------------------------------------------------
// Item pickup
//----------------------------------------------------------------------
void pickup_item(int idx) {
    Item* item = &items[idx];
    
    switch (item->type) {
        case ITEM_POTION:
            player.hp += 10;
            if (player.hp > player.max_hp) player.hp = player.max_hp;
            show_message("Healed 10 HP!");
            break;
            
        case ITEM_APPLE:
            hunger += 30;
            if (hunger > 100) hunger = 100;
            show_message("Food restored!");
            break;
            
        case ITEM_ORB:
            player.atk += 1;
            show_message("Attack up!");
            break;
    }
    
    item->active = 0;
}

//----------------------------------------------------------------------
// Enemy AI
//----------------------------------------------------------------------
void enemy_turn(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;
        enemy_ai(&enemies[i]);
    }
}

void enemy_ai(Entity* e) {
    // Only move if visible or close to player
    int dx = player.x - e->x;
    int dy = player.y - e->y;
    int dist = abs_val(dx) + abs_val(dy);
    
    if (dist > VIEW_RADIUS + 2) return; // Too far, don't move
    
    // Adjacent to player? Attack!
    if (abs_val(dx) <= 1 && abs_val(dy) <= 1 && dist > 0) {
        attack(e, &player);
        return;
    }
    
    // Move towards player
    int move_x = 0, move_y = 0;
    
    if (dx != 0) move_x = dx > 0 ? 1 : -1;
    if (dy != 0) move_y = dy > 0 ? 1 : -1;
    
    // Try to move (randomly pick x or y direction)
    int nx = e->x, ny = e->y;
    
    if (rng() % 2) {
        // Try X first
        if (move_x && can_move(e->x + move_x, e->y) && 
            get_enemy_at(e->x + move_x, e->y) < 0 &&
            (e->x + move_x != player.x || e->y != player.y)) {
            nx = e->x + move_x;
        } else if (move_y && can_move(e->x, e->y + move_y) && 
                   get_enemy_at(e->x, e->y + move_y) < 0 &&
                   (e->x != player.x || e->y + move_y != player.y)) {
            ny = e->y + move_y;
        }
    } else {
        // Try Y first
        if (move_y && can_move(e->x, e->y + move_y) && 
            get_enemy_at(e->x, e->y + move_y) < 0 &&
            (e->x != player.x || e->y + move_y != player.y)) {
            ny = e->y + move_y;
        } else if (move_x && can_move(e->x + move_x, e->y) && 
                   get_enemy_at(e->x + move_x, e->y) < 0 &&
                   (e->x + move_x != player.x || e->y != player.y)) {
            nx = e->x + move_x;
        }
    }
    
    e->x = nx;
    e->y = ny;
}

//----------------------------------------------------------------------
// Floor transition
//----------------------------------------------------------------------
void next_floor(void) {
    floor_num++;
    
    if (floor_num > 5) {
        win = 1;
        show_message("You escaped!");
        return;
    }
    
    // Clear and regenerate
    memset(dungeon, TILE_WALL, sizeof(dungeon));
    memset(visible, 0, sizeof(visible));
    memset(explored, 0, sizeof(explored));
    memset(enemies, 0, sizeof(enemies));
    memset(items, 0, sizeof(items));
    
    rng_state += 12345 + floor_num * 777;
    
    generate_dungeon();
    spawn_enemies();
    spawn_items();
    
    player.x = rooms[0].cx;
    player.y = rooms[0].cy;
    
    update_visibility();
    
    char buf[32];
    buf[0] = 'F';
    buf[1] = 'l';
    buf[2] = 'o';
    buf[3] = 'o';
    buf[4] = 'r';
    buf[5] = ' ';
    buf[6] = '0' + floor_num;
    buf[7] = '\0';
    show_message(buf);
}

//----------------------------------------------------------------------
// Message system
//----------------------------------------------------------------------
void show_message(const char* str) {
    int i = 0;
    while (str[i] && i < 31) {
        msg.text[i] = str[i];
        i++;
    }
    msg.text[i] = '\0';
    msg.timer = 90; // 1.5 seconds at 60fps
}

void update_message(void) {
    if (msg.timer > 0) {
        msg.timer--;
    }
}
