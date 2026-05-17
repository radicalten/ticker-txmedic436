#include <tonc.h>

// --- Graphics Data (Embedded) ---
// A simple 16x16 pink square for our "Kirby"
const unsigned short kirby_tiles[] = {
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    // ... repeat for the second half of the 16x16 sprite
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
    0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F, 0x000F,
};

// A simple tile for the floor (green grass)
const unsigned short grass_tile[] = {
    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
};

// --- Game Constants ---
#define GRAVITY 0.2f
#define JUMP_FORCE -4.0f
#define MOVE_SPEED 1.5f
#define GROUND_Y 140

// --- Game State ---
float playerX = 40;
float playerY = GROUND_Y;
float velocityY = 0;
int cameraX = 0;

int main() {
    // 1. Setup Display: Mode 0 (Tiled), BG2 and Sprites enabled
    REG_DISPCNT = MODE_0 | OBJ_ENABLE | BG2_ENABLE;

    // 2. Load Palette
    // Sprite Palette: 0 = Transparent, 1 = Pink
    palSpriteMem[0] = RGB_FORMAT(0, 0, 0);
    palSpriteMem[1] = RGB_FORMAT(31, 0, 31); 
    
    // BG Palette: 0 = Transparent, 1 = Green
    palBGMem[0] = RGB_FORMAT(0, 0, 0);
    palBGMem[1] = RGB_FORMAT(0, 31, 0);

    // 3. Load Graphics into VRAM
    vramSet(kirby_tiles, 0, Sprite, 1); // Load Kirby sprite
    vramSet(grass_tile, 0, BG2, 1);     // Load Grass tile

    // 4. Fill BG2 Map with the grass tile (creating a floor)
    for (int i = 0; i < 30 * 20; i++) {
        bg2Map[i] = 1; // Fill the screen with grass tiles
    }

    // 5. Initialize Player Sprite
    obj_set(0, playerX, playerY, 0, 0, 0, 0, OBJ_ATRIBUTES_MAP);

    while (1) {
        scanline(); // Wait for VBlank

        // --- Input Handling ---
        uint16_t keys = *REG_KEYINPUT;

        if (!(keys & KEY_RIGHT)) playerX += MOVE_SPEED;
        if (!(keys & KEY_LEFT))  playerX -= MOVE_SPEED;
        
        // Jumping
        if (!(keys & KEY_A) && playerY >= GROUND_Y) {
            velocityY = JUMP_FORCE;
        }

        // --- Physics ---
        velocityY += GRAVITY;
        playerY += velocityY;

        // Collision with ground
        if (playerY > GROUND_Y) {
            playerY = GROUND_Y;
            velocityY = 0;
        }

        // Prevent player from going off the left side of the world
        if (playerX < 0) playerX = 0;

        // --- Camera Logic ---
        // If player moves past the middle of the screen, scroll the background
        if (playerX > 120) {
            cameraX = (int)playerX - 120;
        }

        // Set BG offset for scrolling
        REG_BG2OFFSET = cameraX;

        // --- Update Sprite Position ---
        // Sprite X = ActualX - CameraX
        obj_set(0, (int)playerX - cameraX, (int)playerY, 0, 0, 0, 0, OBJ_ATRIBUTES_MAP);
    }

    return 0;
}
