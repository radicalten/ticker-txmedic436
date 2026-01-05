#include <tonc.h>
#include <stdlib.h>

/* 
 * MYSTERY DUNGEON GBA SINGLE FILE DEMO
 * 
 * Mechanics:
 * - Procedural Map Generation (Rooms + Corridors)
 * - Turn-based movement
 * - Camera follows player
 * - Win condition: Find the stairs
 */

// --- Constants ---
#define MAP_WIDTH  32
#define MAP_HEIGHT 32
#define TILE_SIZE  8

// Tile IDs (Logic)
#define TID_FLOOR  0
#define TID_WALL   1
#define TID_STAIRS 2

// Screen Block Base and Character Block Base
#define BG0_SBB    30
#define BG0_CBB    0

// --- Embedded Graphics Data (4bpp Tiles) ---
// 1 Tile = 8x8 pixels = 32 bytes = 8 integers

// 1. Floor (Dotted pattern)
const unsigned int tile_floor[8] = {
    0x22222222, 0x22222222, 0x22222222, 0x22222222,
    0x22222222, 0x22222222, 0x22222222, 0x22222222
};

// 2. Wall (Bricks)
const unsigned int tile_wall[8] = {
    0x33333333, 0x31111113, 0x31111113, 0x33333333,
    0x33333333, 0x31111113, 0x31111113, 0x33333333
};

// 3. Stairs (Stripes)
const unsigned int tile_stairs[8] = {
    0x00000000, 0x00004444, 0x00444444, 0x44444444,
    0x44444444, 0x44444400, 0x44440000, 0x00000000
};

// 4. Player Sprite (Smiley Face)
const unsigned int tile_player[8] = {
    0x00055500, 0x05555550, 0x55055055, 0x55055055,
    0x55555555, 0x55955955, 0x05599550, 0x00555500
};

// Palette (16 colors)
const unsigned short game_pal[16] = {
    CLR_BLACK,      // 0: Transparent/Background
    RGB15(10,10,10),// 1: Dark Grey (Wall inner)
    RGB15(5, 5, 5), // 2: Floor Grey
    RGB15(15,10, 5),// 3: Brown (Wall border)
    RGB15(31,31, 0),// 4: Yellow (Stairs)
    RGB15(31, 0, 0),// 5: Red (Player Body)
    RGB15(31,31,31),// 6: White
    0,0,0,0,0,0,0,0, // Unused
    RGB15(31,31,31) // 15: White (Debug/Text)
};

// --- Game State ---
typedef struct {
    int x, y;
} Entity;

u8 map[MAP_WIDTH * MAP_HEIGHT];
Entity player;
Entity stairs;
int level = 1;

// --- Helper Functions ---

// Simple Pseudo-Random Number Generator wrapper
int rand_range(int min, int max) {
    return min + (qran() % (max - min + 1));
}

// Draw a specific tile to the screen block
void set_bg_tile(int x, int y, int tile_id) {
    // Screen Block 30 is at se_mem[30]
    se_mem[BG0_SBB][y * 32 + x] = tile_id;
}

// Initialize Graphics
void init_graphics() {
    // Load Palette
    memcpy16(pal_bg_mem, game_pal, 16);
    memcpy16(pal_obj_mem, game_pal, 16);

    // Load Tiles into Character Block 0 (Backgrounds)
    memcpy32(&tile_mem[BG0_CBB][0], tile_floor, 8);  // ID 0
    memcpy32(&tile_mem[BG0_CBB][1], tile_wall, 8);   // ID 1
    memcpy32(&tile_mem[BG0_CBB][2], tile_stairs, 8); // ID 2

    // Load Player Sprite into OBJ memory (start of tile_mem[4])
    memcpy32(&tile_mem[4][0], tile_player, 8);

    // Set up BG0
    REG_BG0CNT = BG_CBB(BG0_CBB) | BG_SBB(BG0_SBB) | BG_4BPP | BG_REG_32x32;
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
}

// --- Dungeon Generation ---
// Uses a simple "Rectangular Room Carving" algorithm
void generate_dungeon() {
    // 1. Fill map with walls
    for(int i = 0; i < MAP_WIDTH * MAP_HEIGHT; i++) {
        map[i] = TID_WALL;
    }

    int room_count = rand_range(5, 8);
    int prev_x = 0, prev_y = 0;

    for (int r = 0; r < room_count; r++) {
        int w = rand_range(4, 8);
        int h = rand_range(4, 8);
        int x = rand_range(1, MAP_WIDTH - w - 1);
        int y = rand_range(1, MAP_HEIGHT - h - 1);

        // Carve Room
        for (int iy = y; iy < y + h; iy++) {
            for (int ix = x; ix < x + w; ix++) {
                map[iy * MAP_WIDTH + ix] = TID_FLOOR;
            }
        }

        int center_x = x + w / 2;
        int center_y = y + h / 2;

        if (r == 0) {
            // Place player in first room
            player.x = center_x;
            player.y = center_y;
        } else {
            // Connect to previous room with corridor
            // Horizontal then Vertical
            int cx = prev_x;
            int cy = prev_y;
            
            while(cx != center_x) {
                map[cy * MAP_WIDTH + cx] = TID_FLOOR;
                cx += (center_x > cx) ? 1 : -1;
            }
            while(cy != center_y) {
                map[cy * MAP_WIDTH + cx] = TID_FLOOR;
                cy += (center_y > cy) ? 1 : -1;
            }
        }
        
        prev_x = center_x;
        prev_y = center_y;

        // Place stairs in the last room generated
        if (r == room_count - 1) {
            stairs.x = center_x;
            stairs.y = center_y;
            map[stairs.y * MAP_WIDTH + stairs.x] = TID_STAIRS;
        }
    }

    // Render Logic Map to VRAM
    for(int y = 0; y < MAP_HEIGHT; y++) {
        for(int x = 0; x < MAP_WIDTH; x++) {
            set_bg_tile(x, y, map[y * MAP_WIDTH + x]);
        }
    }
}

// --- Main Loop ---

int main() {
    // Init Interrupts and Tonc
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    // Seed random (using a volatile register for entropy)
    sqran(REG_VCOUNT); 

    init_graphics();
    generate_dungeon();

    OBJ_ATTR *player_obj = &oam_mem[0];
    int camera_x = 0;
    int camera_y = 0;

    while(1) {
        VBlankIntrWait();
        key_poll();

        int dx = 0;
        int dy = 0;
        bool moved = false;

        // Input Handling (Turn Based: Only move on Key Hit, not held)
        if(key_hit(KEY_UP))    dy = -1;
        if(key_hit(KEY_DOWN))  dy = 1;
        if(key_hit(KEY_LEFT))  dx = -1;
        if(key_hit(KEY_RIGHT)) dx = 1;

        // Logic Update
        if (dx != 0 || dy != 0) {
            int target_x = player.x + dx;
            int target_y = player.y + dy;

            // Bounds Check
            if (target_x >= 0 && target_x < MAP_WIDTH && 
                target_y >= 0 && target_y < MAP_HEIGHT) {
                
                u8 tile = map[target_y * MAP_WIDTH + target_x];

                // Wall Collision
                if (tile != TID_WALL) {
                    player.x = target_x;
                    player.y = target_y;
                    moved = true;

                    // Stair Collision (Next Level)
                    if (player.x == stairs.x && player.y == stairs.y) {
                        level++;
                        // Flash screen effect
                        REG_MOSAIC = MOS_BG_H(15) | MOS_BG_V(15);
                        REG_BG0CNT |= BG_MOSAIC;
                        for(int i=0; i<30; i++) VBlankIntrWait();
                        REG_BG0CNT &= ~BG_MOSAIC;
                        
                        generate_dungeon();
                        // Force camera update immediately
                        moved = true; 
                    }
                }
            }
        }

        // Camera Logic (Center on Player)
        // Screen is 240x160. Player is at pixels p_x*8, p_y*8
        // Desired TopLeft = Player - Screen/2
        int target_cam_x = (player.x * 8) - (240 / 2) + 4;
        int target_cam_y = (player.y * 8) - (160 / 2) + 4;

        // Clamp Camera to Map Bounds
        if (target_cam_x < 0) target_cam_x = 0;
        if (target_cam_y < 0) target_cam_y = 0;
        if (target_cam_x > (MAP_WIDTH * 8) - 240) target_cam_x = (MAP_WIDTH * 8) - 240;
        if (target_cam_y > (MAP_HEIGHT * 8) - 160) target_cam_y = (MAP_HEIGHT * 8) - 160;

        camera_x = target_cam_x;
        camera_y = target_cam_y;

        REG_BG0HOFS = camera_x;
        REG_BG0VOFS = camera_y;

        // Update Sprite (Relative to Camera)
        int sprite_screen_x = (player.x * 8) - camera_x;
        int sprite_screen_y = (player.y * 8) - camera_y;

        // Set OBJ attributes
        // Attr0: Y pos, Shape (Square), 4bpp
        // Attr1: X pos, Size (8x8)
        // Attr2: Tile Index (0), Priority, Palette
        player_obj->attr0 = (sprite_screen_y & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP;
        player_obj->attr1 = (sprite_screen_x & 0x1FF) | ATTR1_SIZE_8;
        player_obj->attr2 = 0 | ATTR2_PRIO(0); 

        // Reroll random to prevent static patterns if player waits
        qran(); 
    }

    return 0;
}
