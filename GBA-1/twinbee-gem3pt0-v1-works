/*
 * TwinBee-like Vertical Shmup for GBA
 * 
 * COMPILATION:
 * This requires the devkitPro GBA toolchain and libtonc.
 * Compile using: 
 * arm-none-eabi-gcc -mthumb -mthumb-interwork -O2 -Wall -c main.c -o main.o
 * arm-none-eabi-gcc -mthumb -mthumb-interwork -O2 -specs=gba.specs main.o -ltonc -o game.elf
 * objcopy -O binary game.elf game.gba
 * gbafix game.gba
 */

#include <tonc.h>
#include <stdlib.h> // For rand()

// Constants
#define MAX_BULLETS 10
#define MAX_ENEMIES 8
#define PLAYER_SPEED 2
#define BULLET_SPEED 4
#define ENEMY_SPEED 1
#define SPAWN_RATE 60 // Frames

// Tile Indices (in VRAM)
#define TID_PLAYER  0
#define TID_ENEMY   2 // 16x16 sprites use 2 tiles index steps in 1D mapping if 4bpp? 
                      // Actually, for 16x16 4bpp, we need 4 tiles (32x8 bytes). 
                      // Let's keep it simple: 16x16 sprites.
#define TID_BULLET  4 
#define TID_CLOUD   1 // Background tile

// Entity Structures
typedef struct {
    int x, y;
    int w, h;
    int active;
} Bullet;

typedef struct {
    int x, y;
    int active;
    int type; // 0 = standard
} Enemy;

typedef struct {
    int x, y;
    int cooldown;
} Player;

// Global State
OBJ_ATTR obj_buffer[128];
Bullet bullets[MAX_BULLETS];
Enemy enemies[MAX_ENEMIES];
Player player;
int frame_count = 0;
int bg_scroll_y = 0;

// ==================================================================================
// GRAPHICS GENERATION
// Since we have no external assets, we paint tiles directly into VRAM.
// ==================================================================================

void plot_pixel_4bpp(TILE *tile_base, int x, int y, u8 clr_index) {
    // 4bpp = 2 pixels per byte.
    // Calculate tile index and offset within tile
    int tile_idx = (y / 8) * (2) + (x / 8); // simplified for single linear alloc
    // Note: This logic below is specifically for writing into a single 8x8 TILE struct
    // We will pass the specific address of the tile we want to edit.
    
    u32 *row = (u32*)&tile_base->data[(y%8) * 4]; // Get the row (4 bytes per row in 4bpp? No, 4 bytes = 8 pixels)
    // Actually, TILE struct in tonc is u32 data[8]. Each u32 is a row of 8 pixels (4 bits each).
    
    // Tonc TILE definition: typedef struct { u32 data[8]; } TILE;
    // 1 pixel = 4 bits.
    
    u32 shift = (x % 8) * 4;
    u32 mask = 0xF << shift;
    tile_base->data[y%8] = (tile_base->data[y%8] & ~mask) | ((clr_index & 0xF) << shift);
}

// Helper to fill a tile with a color
void fill_tile(TILE *t, u8 color) {
    u32 packed = (color << 28) | (color << 24) | (color << 20) | (color << 16) |
                 (color << 12) | (color << 8)  | (color << 4)  | color;
    for(int i=0; i<8; i++) t->data[i] = packed;
}

void init_graphics() {
    // 1. Setup Palettes
    // Background Palette (Banks 0-15)
    pal_bg_mem[0] = CLR_SKYBLUE; // Background color
    pal_bg_mem[1] = CLR_WHITE;   // Cloud color

    // Sprite Palette (Banks 0-15)
    pal_obj_mem[0] = CLR_MAG; // Transparent key
    pal_obj_mem[1] = CLR_BLUE;    // Player
    pal_obj_mem[2] = CLR_CYAN;    // Player Highlight
    pal_obj_mem[3] = CLR_RED;     // Enemy
    pal_obj_mem[4] = CLR_YELLOW;  // Bullet
    
    // 2. Generate Sprites (in tile_mem[4])
    // We are using 16x16 sprites, so we need blocks of 4 tiles per sprite.
    
    // --- PLAYER (Blue Ship) --- 
    // IDs 0, 1, 2, 3 (allocated sequentially in VRAM)
    TILE *pTile = &tile_mem_obj[0][TID_PLAYER]; 
    for(int y=0; y<16; y++) {
        for(int x=0; x<16; x++) {
            // Determine which of the 4 tiles we are in
            int tile_offset = (y/8)*2 + (x/8);
            // Draw a triangle shape
            if (x >= 8 - y/2 && x <= 7 + y/2 && y < 14) {
                plot_pixel_4bpp(&pTile[tile_offset], x%8, y%8, 1);
            }
            // Cockpit
            if (y >= 8 && y <= 11 && x >= 6 && x <= 9) {
                plot_pixel_4bpp(&pTile[tile_offset], x%8, y%8, 2);
            }
        }
    }

    // --- ENEMY (Red Bell/Bee) ---
    // IDs 4, 5, 6, 7 (But we defined TID_ENEMY as 4 because 16x16 uses 2 strides in 1D mapping? 
    // Let's rely on standard 32-byte tiles. 4 tiles = 128 bytes.
    // Index 4 in tile_mem_obj is the start.
    TILE *eTile = &tile_mem_obj[0][TID_ENEMY];
    for(int y=0; y<16; y++) {
        for(int x=0; x<16; x++) {
            int tile_offset = (y/8)*2 + (x/8);
            int dx = x - 7;
            int dy = y - 7;
            if (dx*dx + dy*dy < 40) { // Circle
                plot_pixel_4bpp(&eTile[tile_offset], x%8, y%8, 3);
            }
        }
    }

    // --- BULLET (Yellow Dot) ---
    // 8x8 sprite, so just 1 tile.
    TILE *bTile = &tile_mem_obj[0][TID_BULLET];
    fill_tile(bTile, 0); // Clear
    for(int y=2; y<6; y++) {
        for(int x=2; x<6; x++) {
             plot_pixel_4bpp(bTile, x, y, 4);
        }
    }

    // 3. Generate Background (in tile_mem[0])
    // Create a "Cloud" tile at index 1
    TILE *cloudTile = &tile_mem[0][TID_CLOUD];
    fill_tile(cloudTile, 0); // Fill with skyblue (index 0)
    // Draw a dithery blob
    for(int i=0; i<8; i++) cloudTile->data[i] = 0x11111111; // Simple pattern

    // 4. Fill Map
    // Screen Block 30 (for BG0)
    u16 *map = se_mem[30];
    for(int i=0; i<32*32; i++) {
        // Randomly place clouds
        if ((rand() % 20) == 0) map[i] = TID_CLOUD;
        else map[i] = 0; // Sky
    }
}

// ==================================================================================
// GAME LOGIC
// ==================================================================================

void init_game() {
    player.x = (SCREEN_WIDTH / 2) - 8;
    player.y = SCREEN_HEIGHT - 32;
    player.cooldown = 0;

    for(int i=0; i<MAX_BULLETS; i++) bullets[i].active = 0;
    for(int i=0; i<MAX_ENEMIES; i++) enemies[i].active = 0;
}

void spawn_enemy() {
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(!enemies[i].active) {
            enemies[i].active = 1;
            enemies[i].x = (rand() % (SCREEN_WIDTH - 16));
            enemies[i].y = -16;
            enemies[i].type = 0;
            break;
        }
    }
}

void update() {
    // 1. Player Movement
    key_poll();
    if(key_is_down(KEY_LEFT)) player.x -= PLAYER_SPEED;
    if(key_is_down(KEY_RIGHT)) player.x += PLAYER_SPEED;
    if(key_is_down(KEY_UP)) player.y -= PLAYER_SPEED;
    if(key_is_down(KEY_DOWN)) player.y += PLAYER_SPEED;

    // Clamp to screen
    if(player.x < 0) player.x = 0;
    if(player.x > SCREEN_WIDTH - 16) player.x = SCREEN_WIDTH - 16;
    if(player.y < 0) player.y = 0;
    if(player.y > SCREEN_HEIGHT - 16) player.y = SCREEN_HEIGHT - 16;

    // 2. Shooting
    if(player.cooldown > 0) player.cooldown--;
    if(key_is_down(KEY_A) && player.cooldown == 0) {
        for(int i=0; i<MAX_BULLETS; i++) {
            if(!bullets[i].active) {
                bullets[i].active = 1;
                bullets[i].x = player.x + 4; // Center of 16px ship
                bullets[i].y = player.y;
                bullets[i].w = 8;
                bullets[i].h = 8;
                player.cooldown = 10;
                break;
            }
        }
    }

    // 3. Update Bullets
    for(int i=0; i<MAX_BULLETS; i++) {
        if(bullets[i].active) {
            bullets[i].y -= BULLET_SPEED;
            if(bullets[i].y < -8) bullets[i].active = 0;
        }
    }

    // 4. Update Enemies
    if(frame_count % SPAWN_RATE == 0) spawn_enemy();
    
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(enemies[i].active) {
            enemies[i].y += ENEMY_SPEED;
            // Simple wavy movement
            enemies[i].x += (frame_count % 40 < 20) ? 1 : -1;

            if(enemies[i].y > SCREEN_HEIGHT) enemies[i].active = 0;

            // Collision with Player
            if(abs(player.x - enemies[i].x) < 12 && abs(player.y - enemies[i].y) < 12) {
                // Reset game on hit (very basic)
                init_game();
            }

            // Collision with Bullets
            for(int b=0; b<MAX_BULLETS; b++) {
                if(bullets[b].active) {
                    if(bullets[b].x < enemies[i].x + 16 &&
                       bullets[b].x + 8 > enemies[i].x &&
                       bullets[b].y < enemies[i].y + 16 &&
                       bullets[b].y + 8 > enemies[i].y) {
                        
                        enemies[i].active = 0;
                        bullets[b].active = 0;
                    }
                }
            }
        }
    }

    // 5. Scroll Background
    bg_scroll_y--;
    REG_BG0VOFS = bg_scroll_y >> 1; // Slower scroll

    frame_count++;
}

void draw() {
    // Initialize OAM buffer
    oam_init(obj_buffer, 128);

    int id = 0;

    // Draw Player
    obj_set_attr(&obj_buffer[id++], 
        ATTR0_SQUARE | ATTR0_Y(player.y), 
        ATTR1_SIZE_16 | ATTR1_X(player.x), 
        ATTR2_PALBANK(0) | TID_PLAYER);

    // Draw Enemies
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(enemies[i].active) {
            obj_set_attr(&obj_buffer[id++], 
                ATTR0_SQUARE | ATTR0_Y(enemies[i].y), 
                ATTR1_SIZE_16 | ATTR1_X(enemies[i].x), 
                ATTR2_PALBANK(0) | TID_ENEMY);
        }
    }

    // Draw Bullets
    for(int i=0; i<MAX_BULLETS; i++) {
        if(bullets[i].active) {
            obj_set_attr(&obj_buffer[id++], 
                ATTR0_SQUARE | ATTR0_Y(bullets[i].y), 
                ATTR1_SIZE_8 | ATTR1_X(bullets[i].x), 
                ATTR2_PALBANK(0) | TID_BULLET);
        }
    }

    // Copy buffer to real OAM during VBlank
    oam_copy(oam_mem, obj_buffer, id);
}

// ==================================================================================
// MAIN
// ==================================================================================

int main() {
    // 1. Enable VBlank Interrupts
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    // 2. Setup Graphics Hardware
    // Mode 0, BG0 on, OBJ on, 1D mapping for sprites
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // Setup BG0: CBB 0 (Character Base Block), SBB 30 (Screen Base Block), 4bpp, 256x256
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32;

    init_graphics();
    init_game();

    while(1) {
        VBlankIntrWait();
        update();
        draw();
    }

    return 0;
}
