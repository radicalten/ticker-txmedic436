#include <tonc.h>

// --- Game Constants ---
#define MAX_BULLETS 10
#define MAX_ENEMIES 5
#define PLAYER_SPEED 2
#define BULLET_SPEED 4
#define ENEMY_SPEED 1

// --- Simple Tile Data (8x8 pixels) ---
// Player ship (a small triangle/arrow)
const unsigned short player_tile[] = {
    0x0000, 0x0000, 0x0400, 0x0C00, 0x0C00, 0x1E00, 0x1E00, 0x3F00 
};
// Enemy ship (a small block/cross)
const unsigned short enemy_tile[] = {
    0x0000, 0x1800, 0x3C00, 0x3C00, 0x7E00, 0x3C00, 0x3C00, 0x1800 
};
// Bullet (a small dot)
const unsigned short bullet_tile[] = {
    0x0000, 0x0000, 0x0000, 0x1800, 0x1800, 0x0000, 0x0000, 0x0000 
};

// --- Game Entities ---
typedef struct {
    int x, y;
    int active;
} Entity;

Entity player = {120, 130, 1};
Entity bullets[MAX_BULLETS];
Entity enemies[MAX_ENEMIES];

// --- Initialization ---
void init_game() {
    // Set video mode 0 (Tiled)
    REG_DISPCNT = MODE_0 | BG2_ON;

    // Load Palettes
    // Palette 0: Player (Blue), Palette 1: Enemy (Red), Palette 2: Bullet (Yellow)
    pal_set(0, RGB(31, 0, 0));   // Blue
    pal_set(1, RGB(0, 31, 0));   // Red
    pal_set(2, RGB(31, 31, 0));  // Yellow

    // Load Tiles into VRAM
    // We use the OAM tile memory area
    memcpy(VRAM, player_tile, sizeof(player_tile));        // Tile 0
    memcpy(VRAM + 16, enemy_tile, sizeof(enemy_tile));     // Tile 1
    memcpy(VRAM + 32, bullet_tile, sizeof(bullet_tile));   // Tile 2

    // Init enemies
    for(int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].x = (i * 40) + 40;
        enemies[i].y = -(i * 30); // Staggered start
        enemies[i].active = 1;
    }
    
    // Init bullets
    for(int i = 0; i < MAX_BULLETS; i++) bullets[i].active = 0;
}

// --- Input Handling ---
void handle_input() {
    scan keys = scan_keys();

    if (keys & KEY_LEFT && player.x > 0) player.x -= PLAYER_SPEED;
    if (keys & KEY_RIGHT && player.x < 224) player.x += PLAYER_SPEED;
    if (keys & KEY_UP && player.y > 0) player.y -= PLAYER_SPEED;
    if (keys & KEY_DOWN && player.y < 144) player.y += PLAYER_SPEED;

    // Shooting (A Button)
    if (keys & KEY_A) {
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (!bullets[i].active) {
                bullets[i].x = player.x + 7; // Center of 16x16 sprite
                bullets[i].y = player.y - 8;
                bullets[i].active = 1;
                break; // Only fire one bullet per frame
            }
        }
    }
}

// --- Game Logic ---
void update() {
    // Update Bullets
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            bullets[i].y -= BULLET_SPEED;
            if (bullets[i].y < 0) bullets[i].active = 0;
        }
    }

    // Update Enemies
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (enemies[i].active) {
            enemies[i].y += ENEMY_SPEED;
            if (enemies[i].y > 160) {
                enemies[i].y = -16;
                enemies[i].x = (irand() % 200) + 20;
            }
        }
    }

    // Collision Detection (Bullet vs Enemy)
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) continue;
        for (int j = 0; j < MAX_ENEMIES; j++) {
            if (!enemies[j].active) continue;
            
            // Simple AABB check
            if (bullets[i].x < enemies[j].x + 16 &&
                bullets[i].x + 8 > enemies[j].x &&
                bullets[i].y < enemies[j].y + 16 &&
                bullets[i].y + 8 > enemies[j].y) {
                
                bullets[i].active = 0;
                enemies[j].y = -16; // Respawn enemy
                enemies[j].x = (irand() % 200) + 20;
            }
        }
    }
}

// --- Rendering ---
void draw() {
    obj_clear(); // Clear OAM

    // Draw Player
    obj_set(0, player.x, player.y, 0, 0, 0); // Sprite 0, tile 0, palette 0

    // Draw Bullets
    int b_idx = 1;
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            obj_set(b_idx++, bullets[i].x, bullets[i].y, 0, 2, 0); // tile 2, palette 2
        }
    }

    // Draw Enemies
    int e_idx = 1 + MAX_BULLETS;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (enemies[i].active) {
            obj_set(e_idx++, enemies[i].x, enemies[i].y, 0, 1, 0); // tile 1, palette 1
        }
    }
}

int main() {
    init_game();

    while (1) {
        VBlankInt WaitForVBlank(); // Sync to 60fps
        handle_input();
        update();
        draw();
    }

    return 0;
}
