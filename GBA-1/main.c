#include <tonc.h>
#include <stdlib.h>

// --- Constants ---
#define MAP_WIDTH   32
#define MAP_HEIGHT  32

// Tile Indices
#define TID_EMPTY   0
#define TID_GRASS   1
#define TID_BUSH    2 // Tall grass (encounters)
#define TID_HERO    4

// Palettes
#define PAL_BG      0
#define PAL_HERO    0

// Game States
typedef enum {
    STATE_OVERWORLD,
    STATE_BATTLE
} GameState;

// Player Struct
typedef struct {
    int x, y;       // Pixel coordinates
    u32 tileIndex;
} Player;

// Global Variables
GameState currentState = STATE_OVERWORLD;
Player hero;
u16 mapData[MAP_WIDTH * MAP_HEIGHT]; // The Background Map

// --- Helper Functions ---

// Generate a solid color tile in VRAM
void create_solid_tile(int tile_index, u16 color, int block_base) {
    u32 *tile_mem = (u32*)tile_mem_obj[block_base] + (tile_index * 8);
    u32 packed = color | (color << 16);
    for(int i=0; i<8; i++) {
        tile_mem[i] = packed;
    }
}

// Generate the map (mostly grass, some bushes)
void init_map() {
    for(int y=0; y<MAP_HEIGHT; y++) {
        for(int x=0; x<MAP_WIDTH; x++) {
            // Create a patch of tall grass in the middle
            if(x > 5 && x < 15 && y > 5 && y < 15) {
                mapData[y*MAP_WIDTH + x] = SE_PALBANK(0) | TID_BUSH;
            } else {
                mapData[y*MAP_WIDTH + x] = SE_PALBANK(0) | TID_GRASS;
            }
        }
    }
    // Copy map to Screen Block 30
    memcpy16(&se_mem[30][0], mapData, MAP_WIDTH*MAP_HEIGHT);
}

// Setup Graphics (Video mode, Palettes, Tiles)
void init_graphics() {
    // 1. Set Video Mode: Mode 0 (Tiled), BG0 enabled, OBJ (Sprites) enabled
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // 2. Setup Background Control (BG0)
    // Charblock 0 (Tiles), Screenblock 30 (Map), Size 32x32, 256 colors
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_8BPP | BG_REG_32x32;

    // 3. Setup Palettes
    // Background Palette
    pal_bg_mem[0] = CLR_LIME;       // Light Green (Grass)
    pal_bg_mem[1] = RGB15(0,15,0);  // Dark Green (Bush)
    pal_bg_mem[2] = CLR_SKYBLUE;    // Sky (Battle BG)
    
    // Sprite Palette
    pal_obj_mem[0] = CLR_RED;       // Hero Color
    pal_obj_mem[1] = CLR_WHITE;     // UI Borders

    // 4. Create "Assets" (Procedural Tiles)
    // We write directly into VRAM to create solid color squares
    
    // Background Tiles (Charblock 0)
    // TID_GRASS (Index 1) -> Light Green (Palette Index 0)
    // 8bpp tiles use bytes as palette indices. 
    // 0x00000000 = 4 pixels of palette index 0
    u32* bg_tile_mem = (u32*)tile_mem[0];
    
    // Fill Tile 1 (Grass) with 0s (Palette index 0)
    for(int i=0; i<16; i++) bg_tile_mem[1*16 + i] = 0x00000000; 
    
    // Fill Tile 2 (Bush) with 0x01010101 (Palette index 1)
    for(int i=0; i<16; i++) bg_tile_mem[2*16 + i] = 0x01010101; 

    // Sprite Tiles (Charblock 4 is default for sprites)
    u32* obj_tile_mem = (u32*)tile_mem[4];
    
    // Fill Hero Tile with 0 (Palette index 0 = Red)
    for(int i=0; i<8; i++) obj_tile_mem[TID_HERO*8 + i] = 0x00000000;
    
    // 5. Initialize Text Engine (TTE) using default BIOS font
    tte_init_se_default(0, BG_CBB(0)|BG_SBB(31)); 
    // Note: We use SBB 31 for text, SBB 30 for the map.
}

void update_overworld() {
    int speed = 1;
    bool moved = false;

    // Input Handling
    if(key_is_down(KEY_UP))    { hero.y -= speed; moved = true; }
    if(key_is_down(KEY_DOWN))  { hero.y += speed; moved = true; }
    if(key_is_down(KEY_LEFT))  { hero.x -= speed; moved = true; }
    if(key_is_down(KEY_RIGHT)) { hero.x += speed; moved = true; }

    // Update Sprite OAM
    OBJ_ATTR *hero_obj = &oam_mem[0];
    obj_set_attr(hero_obj, 
        ATTR0_SQUARE | ATTR0_8BPP,  // Square shape, 256 colors
        ATTR1_SIZE_8,               // 8x8 pixels
        ATTR2_PALBANK(0) | TID_HERO // Palette 0, Tile Index
    );
    obj_set_pos(hero_obj, hero.x, hero.y);

    // Collision & Random Encounters
    // Calculate which map tile center of hero is standing on
    int mapX = (hero.x + 4) / 8;
    int mapY = (hero.y + 4) / 8;
    int tileIdx = mapY * MAP_WIDTH + mapX;
    
    // Check if tile is a Bush (Tall Grass)
    // Mask off palette bits to get raw tile ID
    if(moved && (mapData[tileIdx] & 0x03FF) == TID_BUSH) {
        // 1 in 100 chance per frame while moving
        if((qran() % 100) == 0) {
            currentState = STATE_BATTLE;
            
            // Visual Flash effect
            REG_MOSAIC = MOS_BG0_H(8) | MOS_BG0_V(8);
            REG_BG0CNT |= BG_MOSAIC;
            
            // Clear Text Layer
            tte_erase_screen();
        }
    }
}

void update_battle() {
    // Hide the overworld map by disabling BG0 or changing priority.
    // For simplicity, we just draw a box using the Text Engine.
    
    tte_write("#{P:20,60}"); // Position cursor
    tte_write("#{ci:1}Wild PIDGEY appeared!\n\n");
    tte_write("   Press A to Run.");
    
    // Hide hero sprite
    obj_hide(&oam_mem[0]);

    if(key_hit(KEY_A)) {
        currentState = STATE_OVERWORLD;
        tte_erase_screen();
        // Turn off mosaic effect
        REG_BG0CNT &= ~BG_MOSAIC;
    }
}

// --- Main Loop ---

int main() {
    // Basic setup
    init_graphics();
    init_map();
    
    // Init Player
    hero.x = 20;
    hero.y = 20;
    hero.tileIndex = TID_HERO;
    
    // Seed random number generator
    sqran(1234);

    while(1) {
        vid_vsync();
        key_poll();

        if(currentState == STATE_OVERWORLD) {
            // Ensure Map is visible, Text is hidden (or transparent)
            REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
            update_overworld();
        } 
        else if (currentState == STATE_BATTLE) {
            // In battle, we use the text layer.
            // TTE writes to map 31.
            update_battle();
        }
    }

    return 0;
}
