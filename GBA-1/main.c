// ==================================================================================
// GBA Gradius-Clone (Single File Example)
// Requires: devkitPro and libtonc
// Compile with: arm-none-eabi-gcc -mthumb -mthumb-interwork -c main.c
//               (Link with tonc.specs via Makefile usually)
// ==================================================================================

#include <tonc.h>
#include <string.h>
#include <stdlib.h>

// --- Constants ---
#define MAX_BULLETS 10
#define MAX_ENEMIES 8
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 160
#define PLAYER_SPEED 2
#define BULLET_SPEED 5
#define ENEMY_SPEED 1

// --- Graphics Data (Embedded to keep this a single file) ---
// 4bpp (16 colors) 8x8 tiles. 
// 0 = Transparent, 1 = Color 1, 2 = Color 2...

// Tile 0: The Player Ship (Sideways triangle)
const unsigned int tile_ship[8] = {
    0x00000000, 0x00000011, 0x00001111, 0x11111111, 
    0x11111111, 0x00001111, 0x00000011, 0x00000000
};

// Tile 1: The Enemy (Roundish shape)
const unsigned int tile_enemy[8] = {
    0x00222200, 0x02222220, 0x22200222, 0x22000022, 
    0x22000022, 0x22200222, 0x02222220, 0x00222200
};

// Tile 2: Bullet (Small line)
const unsigned int tile_bullet[8] = {
    0x00000000, 0x00000000, 0x00000000, 0x00333300, 
    0x00333300, 0x00000000, 0x00000000, 0x00000000
};

// Tile 3: Star (Background)
const unsigned int tile_star[8] = {
    0x00000000, 0x00004000, 0x00044400, 0x00004000, 
    0x00000000, 0x00000000, 0x00000000, 0x00000000
};

// --- Structures ---

typedef struct {
    int x, y;
    int w, h;
    bool active;
} Bullet;

typedef struct {
    int x, y;
    int w, h;
    bool active;
    int frame_offset; // For sine wave movement
} Enemy;

typedef struct {
    int x, y;
    int w, h;
    int cooldown;
} Player;

// --- Global State ---
Player player;
Bullet bullets[MAX_BULLETS];
Enemy enemies[MAX_ENEMIES];
OBJ_ATTR obj_buffer[128]; // OAM Buffer
int bg_scroll_x = 0;
u16* map_ptr; // Pointer to background map memory

// --- Functions ---

void init_graphics() {
    // 1. Load Palette
    // MEM_PAL_OBJ is strictly for sprites, MEM_PAL_BG for background
    pal_obj_mem[0] = CLR_MAGENTA; // Transparent
    pal_obj_mem[1] = CLR_LIME;    // Player Color
    pal_obj_mem[2] = CLR_RED;     // Enemy Color
    pal_obj_mem[3] = CLR_YELLOW;  // Bullet Color
    
    pal_bg_mem[0] = CLR_BLACK;    // BG Background
    pal_bg_mem[4] = CLR_WHITE;    // Star Color

    // 2. Load Tiles into VRAM (Character Block 4 for sprites)
    memcpy32(&tile_mem[4][0], tile_ship, 8);   // Sprite Tile 0
    memcpy32(&tile_mem[4][1], tile_enemy, 8);  // Sprite Tile 1
    memcpy32(&tile_mem[4][2], tile_bullet, 8); // Sprite Tile 2

    // Load Tiles into VRAM (Character Block 0 for BG)
    memcpy32(&tile_mem[0][1], tile_star, 8);   // BG Tile 1 (0 is empty)
}

void init_background() {
    // Set up a starfield in BG0
    // CBB = 0 (Tile data), SBB = 30 (Map data)
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32;
    
    map_ptr = (u16*)se_mem[30]; // Screen Entry Block 30

    // Clear map
    for(int i=0; i<1024; i++) map_ptr[i] = 0;

    // Scatter some stars randomly
    for(int i=0; i<50; i++) {
        int tx = qran_range(0, 32);
        int ty = qran_range(0, 32);
        // Set tile index 1, palette 0
        map_ptr[ty * 32 + tx] = SE_PALBANK(0) | 1; 
    }
}

void init_game() {
    // Player init
    player.x = 20;
    player.y = SCREEN_HEIGHT / 2;
    player.w = 8;
    player.h = 8;
    player.cooldown = 0;

    // Clear entities
    for(int i=0; i<MAX_BULLETS; i++) bullets[i].active = false;
    for(int i=0; i<MAX_ENEMIES; i++) enemies[i].active = false;

    // Reset OAM buffer
    oam_init(obj_buffer, 128);
}

// Simple AABB Collision
bool check_collision(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2) {
    return (x1 < x2 + w2 && x1 + w1 > x2 &&
            y1 < y2 + h2 && y1 + h1 > y2);
}

void update() {
    // 1. Player Movement
    int dx = key_tri_horz();
    int dy = key_tri_vert();

    player.x += dx * PLAYER_SPEED;
    player.y += dy * PLAYER_SPEED;

    // Clamp to screen
    if(player.x < 0) player.x = 0;
    if(player.x > SCREEN_WIDTH - 8) player.x = SCREEN_WIDTH - 8;
    if(player.y < 0) player.y = 0;
    if(player.y > SCREEN_HEIGHT - 8) player.y = SCREEN_HEIGHT - 8;

    // 2. Shooting
    if(player.cooldown > 0) player.cooldown--;
    if(key_held(KEY_A) && player.cooldown == 0) {
        for(int i=0; i<MAX_BULLETS; i++) {
            if(!bullets[i].active) {
                bullets[i].active = true;
                bullets[i].x = player.x + 4;
                bullets[i].y = player.y;
                bullets[i].w = 8;
                bullets[i].h = 8;
                player.cooldown = 15; // Fire rate
                break;
            }
        }
    }

    // 3. Update Bullets
    for(int i=0; i<MAX_BULLETS; i++) {
        if(bullets[i].active) {
            bullets[i].x += BULLET_SPEED;
            if(bullets[i].x > SCREEN_WIDTH) bullets[i].active = false;
        }
    }

    // 4. Update Enemies (Spawn and Move)
    if(qran_range(0, 100) < 2) { // 2% chance to spawn per frame
        for(int i=0; i<MAX_ENEMIES; i++) {
            if(!enemies[i].active) {
                enemies[i].active = true;
                enemies[i].x = SCREEN_WIDTH;
                enemies[i].y = qran_range(10, SCREEN_HEIGHT - 10);
                enemies[i].w = 8;
                enemies[i].h = 8;
                enemies[i].frame_offset = qran_range(0, 360);
                break;
            }
        }
    }

    for(int i=0; i<MAX_ENEMIES; i++) {
        if(enemies[i].active) {
            enemies[i].x -= ENEMY_SPEED;
            // Sine wave motion
            enemies[i].y += (lu_sin(enemies[i].frame_offset) >> 10); 
            enemies[i].frame_offset += 500; // Advance sine wave

            if(enemies[i].x < -8) enemies[i].active = false;

            // Collision: Bullet vs Enemy
            for(int b=0; b<MAX_BULLETS; b++) {
                if(bullets[b].active && check_collision(
                    bullets[b].x, bullets[b].y, bullets[b].w, bullets[b].h,
                    enemies[i].x, enemies[i].y, enemies[i].w, enemies[i].h
                )) {
                    bullets[b].active = false;
                    enemies[i].active = false;
                    // Could add score here
                }
            }

            // Collision: Player vs Enemy
            if(check_collision(
                player.x, player.y, player.w, player.h,
                enemies[i].x, enemies[i].y, enemies[i].w, enemies[i].h
            )) {
                // Die / Reset
                init_game();
            }
        }
    }

    // 5. Scroll Background
    bg_scroll_x++;
}

void draw() {
    int oam_id = 0;

    // Draw Player (Tile 0)
    obj_set_attr(&obj_buffer[oam_id++], 
                 ATTR0_SQUARE,      // Shape
                 ATTR1_SIZE_8,      // Size
                 ATTR2_PALBANK(0) | 0); // Tile Index 0
    
    obj_set_pos(&obj_buffer[oam_id-1], player.x, player.y);

    // Draw Bullets (Tile 2)
    for(int i=0; i<MAX_BULLETS; i++) {
        if(bullets[i].active) {
            obj_set_attr(&obj_buffer[oam_id++], 
                         ATTR0_SQUARE, 
                         ATTR1_SIZE_8, 
                         ATTR2_PALBANK(0) | 2); // Tile 2
            obj_set_pos(&obj_buffer[oam_id-1], bullets[i].x, bullets[i].y);
        }
    }

    // Draw Enemies (Tile 1)
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(enemies[i].active) {
            obj_set_attr(&obj_buffer[oam_id++], 
                         ATTR0_SQUARE, 
                         ATTR1_SIZE_8, 
                         ATTR2_PALBANK(0) | 1); // Tile 1
            obj_set_pos(&obj_buffer[oam_id-1], enemies[i].x, enemies[i].y);
        }
    }

    // Hide remaining sprites in buffer
    for(int i = oam_id; i < 128; i++) {
        obj_hide(&obj_buffer[i]);
    }

    // Copy buffer to actual OAM memory
    oam_copy(oam_mem, obj_buffer, 128);

    // Update Background scroll register
    REG_BG0HOFS = bg_scroll_x;
}

int main() {
    // 1. Initialization
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
    
    init_graphics();
    init_background();
    init_game();
    
    // Seed random number generator
    sqran(0);

    // 2. Main Loop
    while(1) {
        vid_vsync(); // Wait for VBlank (60 FPS cap)
        key_poll();  // Update inputs
        
        update();
        draw();
    }

    return 0;
}
