/*
 * METROID-LIKE GBA GAME
 * A single-file Metroid-inspired game using TONC and MaxMod
 * 
 * Build with devkitARM:
 * Create a Makefile that links with libtonc and libmm
 * 
 * Note: You'll need to create a soundbank.h with MaxMod's mmutil
 * For testing without sound, comment out MaxMod calls
 */

#include <tonc.h>
#include <string.h>

#define RGB15_C(r,g,b)  ((r) | ((g) << 5) | ((b) << 10))

// Uncomment these when you have MaxMod soundbank set up
// #include <maxmod.h>
// #include "soundbank.h"
// #include "soundbank_bin.h"

//=============================================================================
// DEFINES AND CONSTANTS
//=============================================================================

#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   160

#define TILE_SIZE       8
#define MAP_WIDTH       60
#define MAP_HEIGHT      20

#define PLAYER_WIDTH    16
#define PLAYER_HEIGHT   24
#define PLAYER_SPEED    FIX_ONE
#define PLAYER_JUMP     (3 * FIX_ONE)
#define GRAVITY         (FIX_ONE / 8)
#define MAX_FALL_SPEED  (4 * FIX_ONE)

#define MAX_BULLETS     8
#define BULLET_SPEED    4
#define MAX_ENEMIES     10
#define MAX_ITEMS       5

#define SPRITE_PLAYER   0
#define SPRITE_BULLET   10
#define SPRITE_ENEMY    20
#define SPRITE_ITEM     30

// Tile types
#define TILE_EMPTY      0
#define TILE_SOLID      1
#define TILE_PLATFORM   2
#define TILE_SPIKE      3
#define TILE_DOOR       4
#define TILE_SAVE       5

// Enemy types
#define ENEMY_CRAWLER   0
#define ENEMY_FLYER     1
#define ENEMY_TURRET    2

// Item types
#define ITEM_ENERGY     0
#define ITEM_MISSILE    1
#define ITEM_MORPH_BALL 2

// Game states
#define STATE_TITLE     0
#define STATE_PLAYING   1
#define STATE_PAUSED    2
#define STATE_GAMEOVER  3
#define STATE_WIN       4

//=============================================================================
// SPRITE DATA (4bpp format)
//=============================================================================

// Player sprite (16x24, 6 tiles) - Standing frame
const u32 playerSpriteTiles[48] = {
    // Row 1 (2 tiles)
    0x00000000, 0x00011100, 0x00122210, 0x01233321, 
    0x01233321, 0x00122210, 0x00011100, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    // Row 2 (2 tiles)
    0x00011100, 0x00133310, 0x01333331, 0x01344431, 
    0x01344431, 0x01333331, 0x00133310, 0x00111110,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    // Row 3 (2 tiles)
    0x00111110, 0x00133310, 0x00133310, 0x00111110, 
    0x00011100, 0x00022200, 0x00022200, 0x00022200,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Player running animation frame
const u32 playerRunTiles[48] = {
    0x00000000, 0x00011100, 0x00122210, 0x01233321, 
    0x01233321, 0x00122210, 0x00011100, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00011100, 0x00133310, 0x01333331, 0x01344431, 
    0x01344431, 0x01333331, 0x00133310, 0x00111110,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00111110, 0x00133310, 0x00133310, 0x00111110, 
    0x00022200, 0x00011100, 0x00022200, 0x00011100,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Ball mode sprite (8x8)
const u32 ballSpriteTiles[8] = {
    0x00111100, 0x01333310, 0x13344331, 0x13344331,
    0x13344331, 0x13344331, 0x01333310, 0x00111100,
};

// Bullet sprite (8x8)
const u32 bulletSpriteTiles[8] = {
    0x00000000, 0x00055000, 0x00566500, 0x05666650,
    0x05666650, 0x00566500, 0x00055000, 0x00000000,
};

// Enemy crawler sprite (16x16)
const u32 enemyCrawlerTiles[32] = {
    0x00777700, 0x07888870, 0x78899887, 0x78899887,
    0x78888887, 0x07888870, 0x00777700, 0x00700700,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00700700, 0x00700700, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// Energy tank item sprite (8x8)
const u32 itemEnergyTiles[8] = {
    0x00AAAA00, 0x0AAAAAA0, 0xAABBBBAA, 0xAABBBBAA,
    0xAABBBBAA, 0xAABBBBAA, 0x0AAAAAA0, 0x00AAAA00,
};

// Background tile data
const u32 bgTileData[64] = {
    // Tile 0: Empty (transparent)
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    // Tile 1: Solid block
    0x11111111, 0x12222221, 0x12111121, 0x12111121,
    0x12111121, 0x12111121, 0x12222221, 0x11111111,
    // Tile 2: Platform
    0x33333333, 0x33333333, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    // Tile 3: Spike
    0x00400400, 0x00440440, 0x04444444, 0x44444444,
    0x44444444, 0x44444444, 0x44444444, 0x44444444,
    // Tile 4: Door
    0x55555555, 0x55555555, 0x55000055, 0x55000055,
    0x55000055, 0x55000055, 0x55555555, 0x55555555,
    // Tile 5: Save station
    0x66666666, 0x60000006, 0x60666606, 0x60666606,
    0x60666606, 0x60666606, 0x60000006, 0x66666666,
    // Tile 6: Background detail
    0x00000000, 0x00100000, 0x00010000, 0x00000100,
    0x00000010, 0x00001000, 0x00000000, 0x01000000,
    // Tile 7: Platform edge
    0x33333333, 0x33333333, 0x30000003, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

//=============================================================================
// PALETTE DATA
//=============================================================================

// Sprite palette (16 colors)
const u16 spritePal[16] = {
    RGB15_C(31, 0, 31),   // 0: Transparent (magenta)
    RGB15_C(8, 8, 12),    // 1: Dark blue (armor shadow)
    RGB15_C(12, 12, 20),  // 2: Mid blue
    RGB15_C(20, 12, 8),   // 3: Orange (visor)
    RGB15_C(28, 20, 8),   // 4: Yellow highlight
    RGB15_C(28, 28, 20),  // 5: Bullet yellow
    RGB15_C(31, 31, 24),  // 6: Bullet bright
    RGB15_C(16, 4, 4),    // 7: Enemy dark red
    RGB15_C(24, 8, 8),    // 8: Enemy red
    RGB15_C(28, 16, 16),  // 9: Enemy light red
    RGB15_C(8, 24, 8),    // A: Item green
    RGB15_C(16, 31, 16),  // B: Item bright green
    RGB15_C(0, 0, 0),     // C: Black
    RGB15_C(31, 31, 31),  // D: White
    RGB15_C(16, 16, 16),  // E: Gray
    RGB15_C(24, 24, 24),  // F: Light gray
};

// Background palette
const u16 bgPal[16] = {
    RGB15_C(0, 0, 4),     // 0: Dark background
    RGB15_C(8, 8, 12),    // 1: Block dark
    RGB15_C(12, 12, 20),  // 2: Block mid
    RGB15_C(16, 12, 8),   // 3: Platform
    RGB15_C(24, 8, 8),    // 4: Spike red
    RGB15_C(8, 8, 24),    // 5: Door blue
    RGB15_C(8, 24, 8),    // 6: Save green
    RGB15_C(4, 4, 8),     // 7: Background detail
    RGB15_C(0, 0, 0),
    RGB15_C(0, 0, 0),
    RGB15_C(0, 0, 0),
    RGB15_C(0, 0, 0),
    RGB15_C(0, 0, 0),
    RGB15_C(0, 0, 0),
    RGB15_C(0, 0, 0),
    RGB15_C(0, 0, 0),
};

//=============================================================================
// LEVEL DATA
//=============================================================================

// Level map (60x20 tiles = 480x160 pixels)
const u8 levelMap[MAP_HEIGHT][MAP_WIDTH] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

//=============================================================================
// STRUCTURES
//=============================================================================

typedef struct {
    FIXED x, y;
    FIXED vx, vy;
    int width, height;
    int health;
    int maxHealth;
    int missiles;
    int maxMissiles;
    int facing;         // 0=right, 1=left
    int state;          // 0=stand, 1=run, 2=jump, 3=fall, 4=ball
    int animFrame;
    int animTimer;
    int shootTimer;
    int invincibleTimer;
    int hasMorphBall;
    int inBallMode;
} Player;

typedef struct {
    int active;
    FIXED x, y;
    int dx, dy;
    int life;
    int isMissile;
} Bullet;

typedef struct {
    int active;
    int type;
    FIXED x, y;
    FIXED vx, vy;
    int width, height;
    int health;
    int facing;
    int animFrame;
    int animTimer;
    int state;
} Enemy;

typedef struct {
    int active;
    int type;
    int x, y;
    int collected;
} Item;

typedef struct {
    int scrollX;
    int scrollY;
    int gameState;
    int frameCount;
    int score;
    int bossDefeated;
} GameState;

//=============================================================================
// GLOBALS
//=============================================================================

Player player;
Bullet bullets[MAX_BULLETS];
Enemy enemies[MAX_ENEMIES];
Item items[MAX_ITEMS];
GameState game;

OBJ_ATTR obj_buffer[128];
OBJ_AFFINE *obj_aff_buffer = (OBJ_AFFINE*)obj_buffer;

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

void initGame(void);
void initGraphics(void);
void initPlayer(void);
void initEnemies(void);
void initItems(void);
void updateGame(void);
void updatePlayer(void);
void updateBullets(void);
void updateEnemies(void);
void updateItems(void);
void updateCamera(void);
void handleInput(void);
void drawGame(void);
void drawHUD(void);
void drawTitleScreen(void);
void drawGameOver(void);
int checkTileCollision(int x, int y);
int checkTileCollisionRect(int x, int y, int w, int h);
void spawnBullet(int x, int y, int dx, int dy, int isMissile);
void spawnEnemy(int type, int x, int y);
void takeDamage(int amount);
void playSound(int sfxId);

//=============================================================================
// SOUND FUNCTIONS (MaxMod wrappers)
//=============================================================================

void initSound(void) {
    // Initialize MaxMod
    // mmInitDefault((mm_addr)soundbank_bin, 8);
    // mmStart(MOD_MUSIC, MM_PLAY_LOOP);
}

void playSound(int sfxId) {
    // Play a sound effect
    // mmEffect(sfxId);
    (void)sfxId; // Suppress unused warning
}

//=============================================================================
// GRAPHICS INITIALIZATION
//=============================================================================

void initGraphics(void) {
    // Set video mode: Mode 0 with BG0 for tiles and OBJ
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // Initialize OAM
    oam_init(obj_buffer, 128);
    
    // Load sprite palette
    memcpy(pal_obj_mem, spritePal, sizeof(spritePal));
    
    // Load BG palette
    memcpy(pal_bg_mem, bgPal, sizeof(bgPal));
    
    // Load sprite tiles to VRAM
    // Player tiles (starting at tile 0)
    memcpy(&tile_mem[4][0], playerSpriteTiles, sizeof(playerSpriteTiles));
    memcpy(&tile_mem[4][6], playerRunTiles, sizeof(playerRunTiles));
    memcpy(&tile_mem[4][12], ballSpriteTiles, sizeof(ballSpriteTiles));
    
    // Bullet tiles
    memcpy(&tile_mem[4][16], bulletSpriteTiles, sizeof(bulletSpriteTiles));
    
    // Enemy tiles
    memcpy(&tile_mem[4][20], enemyCrawlerTiles, sizeof(enemyCrawlerTiles));
    
    // Item tiles
    memcpy(&tile_mem[4][28], itemEnergyTiles, sizeof(itemEnergyTiles));
    
    // Load background tiles
    memcpy(&tile_mem[0][0], bgTileData, sizeof(bgTileData));
    
    // Set up BG0 for the level tilemap
    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_SIZE0 | BG_PRIO(1);
    
    // Build the initial tilemap
    u16 *map = (u16*)se_mem[28];
    for(int y = 0; y < 32; y++) {
        for(int x = 0; x < 32; x++) {
            int mapX = x;
            int mapY = y;
            if(mapX < MAP_WIDTH && mapY < MAP_HEIGHT) {
                map[y * 32 + x] = levelMap[mapY][mapX];
            } else {
                map[y * 32 + x] = 0;
            }
        }
    }
}

//=============================================================================
// GAME INITIALIZATION
//=============================================================================

void initGame(void) {
    game.scrollX = 0;
    game.scrollY = 0;
    game.gameState = STATE_TITLE;
    game.frameCount = 0;
    game.score = 0;
    game.bossDefeated = 0;
    
    initGraphics();
    initSound();
    initPlayer();
    initEnemies();
    initItems();
}

void initPlayer(void) {
    player.x = int2fx(24);
    player.y = int2fx(120);
    player.vx = 0;
    player.vy = 0;
    player.width = 14;
    player.height = 22;
    player.health = 99;
    player.maxHealth = 99;
    player.missiles = 10;
    player.maxMissiles = 10;
    player.facing = 0;
    player.state = 0;
    player.animFrame = 0;
    player.animTimer = 0;
    player.shootTimer = 0;
    player.invincibleTimer = 0;
    player.hasMorphBall = 0;
    player.inBallMode = 0;
    
    // Clear bullets
    for(int i = 0; i < MAX_BULLETS; i++) {
        bullets[i].active = 0;
    }
}

void initEnemies(void) {
    for(int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].active = 0;
    }
    
    // Spawn some enemies at predefined positions
    spawnEnemy(ENEMY_CRAWLER, 180, 128);
    spawnEnemy(ENEMY_CRAWLER, 280, 128);
    spawnEnemy(ENEMY_CRAWLER, 350, 40);
    spawnEnemy(ENEMY_FLYER, 200, 60);
    spawnEnemy(ENEMY_FLYER, 380, 80);
}

void initItems(void) {
    for(int i = 0; i < MAX_ITEMS; i++) {
        items[i].active = 0;
        items[i].collected = 0;
    }
    
    // Place items
    items[0].active = 1;
    items[0].type = ITEM_ENERGY;
    items[0].x = 120;
    items[0].y = 32;
    
    items[1].active = 1;
    items[1].type = ITEM_MISSILE;
    items[1].x = 320;
    items[1].y = 96;
    
    items[2].active = 1;
    items[2].type = ITEM_MORPH_BALL;
    items[2].x = 240;
    items[2].y = 128;
}

//=============================================================================
// COLLISION DETECTION
//=============================================================================

int checkTileCollision(int pixelX, int pixelY) {
    int tileX = pixelX / TILE_SIZE;
    int tileY = pixelY / TILE_SIZE;
    
    if(tileX < 0 || tileX >= MAP_WIDTH || tileY < 0 || tileY >= MAP_HEIGHT) {
        return TILE_SOLID;
    }
    
    return levelMap[tileY][tileX];
}

int isTileSolid(int tileType) {
    return (tileType == TILE_SOLID || tileType == TILE_PLATFORM);
}

int checkTileCollisionRect(int x, int y, int w, int h) {
    // Check all four corners
    if(isTileSolid(checkTileCollision(x, y))) return 1;
    if(isTileSolid(checkTileCollision(x + w - 1, y))) return 1;
    if(isTileSolid(checkTileCollision(x, y + h - 1))) return 1;
    if(isTileSolid(checkTileCollision(x + w - 1, y + h - 1))) return 1;
    
    return 0;
}

int checkPlatformBelow(int x, int y, int w) {
    int tileY = (y + 1) / TILE_SIZE;
    int tile1 = checkTileCollision(x, y + 1);
    int tile2 = checkTileCollision(x + w - 1, y + 1);
    
    return (tile1 == TILE_PLATFORM || tile2 == TILE_PLATFORM);
}

//=============================================================================
// INPUT HANDLING
//=============================================================================

void handleInput(void) {
    key_poll();
    
    if(game.gameState == STATE_TITLE) {
        if(key_hit(KEY_START)) {
            game.gameState = STATE_PLAYING;
            initPlayer();
            initEnemies();
            initItems();
            playSound(0); // SFX_START
        }
        return;
    }
    
    if(game.gameState == STATE_GAMEOVER || game.gameState == STATE_WIN) {
        if(key_hit(KEY_START)) {
            game.gameState = STATE_TITLE;
        }
        return;
    }
    
    if(game.gameState == STATE_PAUSED) {
        if(key_hit(KEY_START)) {
            game.gameState = STATE_PLAYING;
        }
        return;
    }
    
    // Pause
    if(key_hit(KEY_START)) {
        game.gameState = STATE_PAUSED;
        return;
    }
    
    // Movement
    player.vx = 0;
    
    if(player.inBallMode) {
        // Ball mode controls
        if(key_is_down(KEY_LEFT)) {
            player.vx = -PLAYER_SPEED;
            player.facing = 1;
        }
        if(key_is_down(KEY_RIGHT)) {
            player.vx = PLAYER_SPEED;
            player.facing = 0;
        }
        
        // Exit ball mode
        if(key_hit(KEY_UP) && player.hasMorphBall) {
            // Check if there's room to stand
            int px = fx2int(player.x);
            int py = fx2int(player.y) - 16;
            if(!checkTileCollisionRect(px, py, player.width, player.height)) {
                player.inBallMode = 0;
                player.y -= int2fx(8);
                player.height = 22;
            }
        }
    } else {
        // Normal mode controls
        if(key_is_down(KEY_LEFT)) {
            player.vx = -PLAYER_SPEED;
            player.facing = 1;
            player.state = 1; // Running
        }
        if(key_is_down(KEY_RIGHT)) {
            player.vx = PLAYER_SPEED;
            player.facing = 0;
            player.state = 1;
        }
        if(!key_is_down(KEY_LEFT) && !key_is_down(KEY_RIGHT)) {
            player.state = 0; // Standing
        }
        
        // Jump
        if(key_hit(KEY_A) && player.vy == 0) {
            int px = fx2int(player.x);
            int py = fx2int(player.y);
            int tileBelow = checkTileCollision(px + 4, py + player.height);
            int tileBelow2 = checkTileCollision(px + player.width - 4, py + player.height);
            
            if(isTileSolid(tileBelow) || isTileSolid(tileBelow2)) {
                player.vy = -PLAYER_JUMP;
                player.state = 2;
                playSound(1); // SFX_JUMP
            }
        }
        
        // Shoot
        if(key_hit(KEY_B) && player.shootTimer == 0) {
            int bx = fx2int(player.x) + (player.facing ? -4 : player.width);
            int by = fx2int(player.y) + 8;
            int bdx = player.facing ? -BULLET_SPEED : BULLET_SPEED;
            spawnBullet(bx, by, bdx, 0, 0);
            player.shootTimer = 10;
            playSound(2); // SFX_SHOOT
        }
        
        // Missile (hold R + B)
        if(key_is_down(KEY_R) && key_hit(KEY_B) && player.missiles > 0 && player.shootTimer == 0) {
            int bx = fx2int(player.x) + (player.facing ? -4 : player.width);
            int by = fx2int(player.y) + 8;
            int bdx = player.facing ? -BULLET_SPEED * 2 : BULLET_SPEED * 2;
            spawnBullet(bx, by, bdx, 0, 1);
            player.missiles--;
            player.shootTimer = 20;
            playSound(3); // SFX_MISSILE
        }
        
        // Morph ball (down)
        if(key_hit(KEY_DOWN) && player.hasMorphBall && !player.inBallMode && player.vy == 0) {
            player.inBallMode = 1;
            player.height = 8;
            player.y += int2fx(14);
        }
    }
}

//=============================================================================
// BULLET FUNCTIONS
//=============================================================================

void spawnBullet(int x, int y, int dx, int dy, int isMissile) {
    for(int i = 0; i < MAX_BULLETS; i++) {
        if(!bullets[i].active) {
            bullets[i].active = 1;
            bullets[i].x = int2fx(x);
            bullets[i].y = int2fx(y);
            bullets[i].dx = dx;
            bullets[i].dy = dy;
            bullets[i].life = 60;
            bullets[i].isMissile = isMissile;
            return;
        }
    }
}

void updateBullets(void) {
    for(int i = 0; i < MAX_BULLETS; i++) {
        if(!bullets[i].active) continue;
        
        bullets[i].x += int2fx(bullets[i].dx);
        bullets[i].y += int2fx(bullets[i].dy);
        bullets[i].life--;
        
        int bx = fx2int(bullets[i].x);
        int by = fx2int(bullets[i].y);
        
        // Check tile collision
        if(checkTileCollision(bx, by) == TILE_SOLID) {
            bullets[i].active = 0;
            continue;
        }
        
        // Check enemy collision
        for(int j = 0; j < MAX_ENEMIES; j++) {
            if(!enemies[j].active) continue;
            
            int ex = fx2int(enemies[j].x);
            int ey = fx2int(enemies[j].y);
            
            if(bx >= ex && bx < ex + enemies[j].width &&
               by >= ey && by < ey + enemies[j].height) {
                int damage = bullets[i].isMissile ? 30 : 10;
                enemies[j].health -= damage;
                bullets[i].active = 0;
                playSound(4); // SFX_HIT
                
                if(enemies[j].health <= 0) {
                    enemies[j].active = 0;
                    game.score += 100;
                    playSound(5); // SFX_ENEMY_DIE
                }
                break;
            }
        }
        
        // Timeout or off screen
        if(bullets[i].life <= 0 || bx < 0 || bx > MAP_WIDTH * TILE_SIZE) {
            bullets[i].active = 0;
        }
    }
}

//=============================================================================
// ENEMY FUNCTIONS
//=============================================================================

void spawnEnemy(int type, int x, int y) {
    for(int i = 0; i < MAX_ENEMIES; i++) {
        if(!enemies[i].active) {
            enemies[i].active = 1;
            enemies[i].type = type;
            enemies[i].x = int2fx(x);
            enemies[i].y = int2fx(y);
            enemies[i].facing = 0;
            enemies[i].animFrame = 0;
            enemies[i].animTimer = 0;
            enemies[i].state = 0;
            
            switch(type) {
                case ENEMY_CRAWLER:
                    enemies[i].width = 16;
                    enemies[i].height = 16;
                    enemies[i].health = 30;
                    enemies[i].vx = int2fx(1) / 2;
                    enemies[i].vy = 0;
                    break;
                case ENEMY_FLYER:
                    enemies[i].width = 16;
                    enemies[i].height = 16;
                    enemies[i].health = 20;
                    enemies[i].vx = int2fx(1) / 2;
                    enemies[i].vy = 0;
                    break;
                default:
                    enemies[i].width = 16;
                    enemies[i].height = 16;
                    enemies[i].health = 50;
                    enemies[i].vx = 0;
                    enemies[i].vy = 0;
                    break;
            }
            return;
        }
    }
}

void updateEnemies(void) {
    for(int i = 0; i < MAX_ENEMIES; i++) {
        if(!enemies[i].active) continue;
        
        Enemy *e = &enemies[i];
        
        switch(e->type) {
            case ENEMY_CRAWLER:
                // Move horizontally
                e->x += e->vx;
                
                // Apply gravity
                e->vy += GRAVITY / 2;
                if(e->vy > MAX_FALL_SPEED) e->vy = MAX_FALL_SPEED;
                e->y += e->vy;
                
                // Check floor collision
                {
                    int ex = fx2int(e->x);
                    int ey = fx2int(e->y);
                    int tile = checkTileCollision(ex + 8, ey + e->height);
                    
                    if(isTileSolid(tile)) {
                        e->y = int2fx((ey / 8) * 8);
                        e->vy = 0;
                    }
                    
                    // Check wall collision / edge detection
                    int wallTile = checkTileCollision(ex + (e->facing ? 0 : e->width), ey + 8);
                    int edgeTile = checkTileCollision(ex + (e->facing ? 0 : e->width), ey + e->height + 4);
                    
                    if(isTileSolid(wallTile) || !isTileSolid(edgeTile)) {
                        e->facing = !e->facing;
                        e->vx = -e->vx;
                    }
                }
                break;
                
            case ENEMY_FLYER:
                // Sine wave movement
                e->x += e->vx;
                e->y = int2fx(fx2int(e->y)) + lu_sin(game.frameCount * 128) / 32;
                
                // Bounce off walls
                {
                    int ex = fx2int(e->x);
                    int ey = fx2int(e->y);
                    int tile = checkTileCollision(ex + (e->facing ? 0 : e->width), ey + 8);
                    
                    if(isTileSolid(tile)) {
                        e->facing = !e->facing;
                        e->vx = -e->vx;
                    }
                }
                break;
        }
        
        // Check collision with player
        if(player.invincibleTimer == 0) {
            int px = fx2int(player.x);
            int py = fx2int(player.y);
            int ex = fx2int(e->x);
            int ey = fx2int(e->y);
            
            if(px < ex + e->width && px + player.width > ex &&
               py < ey + e->height && py + player.height > ey) {
                takeDamage(10);
            }
        }
        
        // Animation
        e->animTimer++;
        if(e->animTimer >= 8) {
            e->animTimer = 0;
            e->animFrame = (e->animFrame + 1) % 2;
        }
    }
}

//=============================================================================
// ITEM FUNCTIONS
//=============================================================================

void updateItems(void) {
    int px = fx2int(player.x);
    int py = fx2int(player.y);
    
    for(int i = 0; i < MAX_ITEMS; i++) {
        if(!items[i].active || items[i].collected) continue;
        
        // Check collision with player
        if(px < items[i].x + 8 && px + player.width > items[i].x &&
           py < items[i].y + 8 && py + player.height > items[i].y) {
            
            items[i].collected = 1;
            items[i].active = 0;
            
            switch(items[i].type) {
                case ITEM_ENERGY:
                    player.health += 50;
                    if(player.health > player.maxHealth) {
                        player.health = player.maxHealth;
                    }
                    break;
                case ITEM_MISSILE:
                    player.missiles += 5;
                    if(player.missiles > player.maxMissiles) {
                        player.maxMissiles += 5;
                        player.missiles = player.maxMissiles;
                    }
                    break;
                case ITEM_MORPH_BALL:
                    player.hasMorphBall = 1;
                    break;
            }
            
            playSound(6); // SFX_ITEM
        }
    }
}

//=============================================================================
// PLAYER UPDATE
//=============================================================================

void takeDamage(int amount) {
    if(player.invincibleTimer > 0) return;
    
    player.health -= amount;
    player.invincibleTimer = 60;
    playSound(7); // SFX_HURT
    
    if(player.health <= 0) {
        player.health = 0;
        game.gameState = STATE_GAMEOVER;
        playSound(8); // SFX_DEATH
    }
}

void updatePlayer(void) {
    // Apply velocity
    player.x += player.vx;
    
    // Horizontal collision
    int px = fx2int(player.x);
    int py = fx2int(player.y);
    
    if(player.vx > 0) {
        if(checkTileCollision(px + player.width, py + 4) == TILE_SOLID ||
           checkTileCollision(px + player.width, py + player.height - 4) == TILE_SOLID) {
            player.x = int2fx((px / 8) * 8 + 8 - player.width + 6);
            player.vx = 0;
        }
    } else if(player.vx < 0) {
        if(checkTileCollision(px, py + 4) == TILE_SOLID ||
           checkTileCollision(px, py + player.height - 4) == TILE_SOLID) {
            player.x = int2fx((px / 8 + 1) * 8);
            player.vx = 0;
        }
    }
    
    // Apply gravity
    player.vy += GRAVITY;
    if(player.vy > MAX_FALL_SPEED) {
        player.vy = MAX_FALL_SPEED;
    }
    
    player.y += player.vy;
    
    // Vertical collision
    px = fx2int(player.x);
    py = fx2int(player.y);
    
    if(player.vy > 0) {
        int tile1 = checkTileCollision(px + 2, py + player.height);
        int tile2 = checkTileCollision(px + player.width - 2, py + player.height);
        
        if(isTileSolid(tile1) || isTileSolid(tile2)) {
            player.y = int2fx((py / 8) * 8);
            player.vy = 0;
            if(player.state == 2 || player.state == 3) {
                player.state = 0;
            }
        } else {
            if(!player.inBallMode && player.vy > 0) {
                player.state = 3; // Falling
            }
        }
    } else if(player.vy < 0) {
        int tile1 = checkTileCollision(px + 2, py);
        int tile2 = checkTileCollision(px + player.width - 2, py);
        
        if(tile1 == TILE_SOLID || tile2 == TILE_SOLID) {
            player.y = int2fx((py / 8 + 1) * 8);
            player.vy = 0;
        }
    }
    
    // Check for spikes
    px = fx2int(player.x);
    py = fx2int(player.y);
    if(checkTileCollision(px + player.width/2, py + player.height) == TILE_SPIKE) {
        takeDamage(20);
    }
    
    // Clamp to level bounds
    if(player.x < int2fx(8)) player.x = int2fx(8);
    if(fx2int(player.x) + player.width > (MAP_WIDTH - 1) * 8) {
        player.x = int2fx((MAP_WIDTH - 1) * 8 - player.width);
    }
    
    // Update timers
    if(player.shootTimer > 0) player.shootTimer--;
    if(player.invincibleTimer > 0) player.invincibleTimer--;
    
    // Animation
    player.animTimer++;
    if(player.animTimer >= 8) {
        player.animTimer = 0;
        player.animFrame = (player.animFrame + 1) % 4;
    }
    
    // Check for win condition (reach save point on right side)
    px = fx2int(player.x);
    py = fx2int(player.y);
    if(checkTileCollision(px + player.width/2, py + player.height/2) == TILE_SAVE) {
        if(px > 400) { // Near the end of level
            game.gameState = STATE_WIN;
        }
    }
}

//=============================================================================
// CAMERA UPDATE
//=============================================================================

void updateCamera(void) {
    int px = fx2int(player.x);
    int py = fx2int(player.y);
    
    // Center camera on player
    int targetX = px - SCREEN_WIDTH / 2;
    int targetY = py - SCREEN_HEIGHT / 2;
    
    // Smooth scrolling
    game.scrollX += (targetX - game.scrollX) / 8;
    game.scrollY += (targetY - game.scrollY) / 8;
    
    // Clamp to level bounds
    if(game.scrollX < 0) game.scrollX = 0;
    if(game.scrollX > MAP_WIDTH * TILE_SIZE - SCREEN_WIDTH) {
        game.scrollX = MAP_WIDTH * TILE_SIZE - SCREEN_WIDTH;
    }
    if(game.scrollY < 0) game.scrollY = 0;
    if(game.scrollY > MAP_HEIGHT * TILE_SIZE - SCREEN_HEIGHT) {
        game.scrollY = MAP_HEIGHT * TILE_SIZE - SCREEN_HEIGHT;
    }
    
    // Apply scroll to background
    REG_BG0HOFS = game.scrollX;
    REG_BG0VOFS = game.scrollY;
}

//=============================================================================
// DRAWING FUNCTIONS
//=============================================================================

void drawGame(void) {
    int spriteIdx = 0;
    
    // Draw player
    int px = fx2int(player.x) - game.scrollX;
    int py = fx2int(player.y) - game.scrollY;
    
    // Hide if invincible (flashing)
    int showPlayer = 1;
    if(player.invincibleTimer > 0 && (game.frameCount % 4) < 2) {
        showPlayer = 0;
    }
    
    if(showPlayer && px > -16 && px < SCREEN_WIDTH && py > -24 && py < SCREEN_HEIGHT) {
        if(player.inBallMode) {
            // Draw ball sprite
            obj_set_attr(&obj_buffer[spriteIdx],
                ATTR0_Y(py) | ATTR0_SQUARE,
                ATTR1_X(px) | ATTR1_SIZE_8,
                ATTR2_ID(12) | ATTR2_PALBANK(0));
        } else {
            // Draw player sprite (16x32 for proper height, but we use 16x24 area)
            u16 tileId = (player.state == 1 && player.animFrame % 2) ? 6 : 0;
            u16 flipFlag = player.facing ? ATTR1_HFLIP : 0;
            
            obj_set_attr(&obj_buffer[spriteIdx],
                ATTR0_Y(py) | ATTR0_TALL,
                ATTR1_X(px) | ATTR1_SIZE_16 | flipFlag,
                ATTR2_ID(tileId) | ATTR2_PALBANK(0));
        }
    } else {
        obj_hide(&obj_buffer[spriteIdx]);
    }
    spriteIdx++;
    
    // Draw bullets
    for(int i = 0; i < MAX_BULLETS; i++) {
        if(bullets[i].active) {
            int bx = fx2int(bullets[i].x) - game.scrollX;
            int by = fx2int(bullets[i].y) - game.scrollY;
            
            if(bx > -8 && bx < SCREEN_WIDTH && by > -8 && by < SCREEN_HEIGHT) {
                obj_set_attr(&obj_buffer[spriteIdx],
                    ATTR0_Y(by) | ATTR0_SQUARE,
                    ATTR1_X(bx) | ATTR1_SIZE_8,
                    ATTR2_ID(16) | ATTR2_PALBANK(0));
            } else {
                obj_hide(&obj_buffer[spriteIdx]);
            }
        } else {
            obj_hide(&obj_buffer[spriteIdx]);
        }
        spriteIdx++;
    }
    
    // Draw enemies
    for(int i = 0; i < MAX_ENEMIES; i++) {
        if(enemies[i].active) {
            int ex = fx2int(enemies[i].x) - game.scrollX;
            int ey = fx2int(enemies[i].y) - game.scrollY;
            
            if(ex > -16 && ex < SCREEN_WIDTH && ey > -16 && ey < SCREEN_HEIGHT) {
                u16 flipFlag = enemies[i].facing ? ATTR1_HFLIP : 0;
                obj_set_attr(&obj_buffer[spriteIdx],
                    ATTR0_Y(ey) | ATTR0_SQUARE,
                    ATTR1_X(ex) | ATTR1_SIZE_16 | flipFlag,
                    ATTR2_ID(20) | ATTR2_PALBANK(0));
            } else {
                obj_hide(&obj_buffer[spriteIdx]);
            }
        } else {
            obj_hide(&obj_buffer[spriteIdx]);
        }
        spriteIdx++;
    }
    
    // Draw items
    for(int i = 0; i < MAX_ITEMS; i++) {
        if(items[i].active && !items[i].collected) {
            int ix = items[i].x - game.scrollX;
            int iy = items[i].y - game.scrollY;
            
            // Bobbing animation
            iy += (fxsin(game.frameCount * 256) >> 13);
            
            if(ix > -8 && ix < SCREEN_WIDTH && iy > -8 && iy < SCREEN_HEIGHT) {
                obj_set_attr(&obj_buffer[spriteIdx],
                    ATTR0_Y(iy) | ATTR0_SQUARE,
                    ATTR1_X(ix) | ATTR1_SIZE_8,
                    ATTR2_ID(28) | ATTR2_PALBANK(0));
            } else {
                obj_hide(&obj_buffer[spriteIdx]);
            }
        } else {
            obj_hide(&obj_buffer[spriteIdx]);
        }
        spriteIdx++;
    }
    
    // Hide remaining sprites
    while(spriteIdx < 128) {
        obj_hide(&obj_buffer[spriteIdx]);
        spriteIdx++;
    }
    
    // Draw HUD
    drawHUD();
}

void drawHUD(void) {
    // Simple HUD using remaining sprites or BG text
    // For simplicity, we'll just update palettes to show health visually
    // A full implementation would use a text background layer
    
    // Health bar color indication (change BG color based on health)
    if(player.health < 25) {
        pal_bg_mem[0] = RGB15_C(8, 0, 0); // Red tint when low health
    } else if(player.health < 50) {
        pal_bg_mem[0] = RGB15_C(4, 2, 0); // Orange tint
    } else {
        pal_bg_mem[0] = RGB15_C(0, 0, 4); // Normal dark blue
    }
}

void drawTitleScreen(void) {
    // Clear all sprites
    for(int i = 0; i < 128; i++) {
        obj_hide(&obj_buffer[i]);
    }
    
    // Set background to title color
    pal_bg_mem[0] = RGB15_C(0, 0, 8);
    
    // Flash "PRESS START" using background color cycling
    if((game.frameCount / 30) % 2) {
        pal_bg_mem[0] = RGB15_C(4, 4, 12);
    }
}

void drawGameOver(void) {
    // Red tinted background
    pal_bg_mem[0] = RGB15_C(12, 0, 0);
    
    for(int i = 0; i < 128; i++) {
        obj_hide(&obj_buffer[i]);
    }
}

void drawWinScreen(void) {
    // Green tinted background
    pal_bg_mem[0] = RGB15_C(0, 12, 0);
    
    for(int i = 0; i < 128; i++) {
        obj_hide(&obj_buffer[i]);
    }
}

//=============================================================================
// MAIN GAME UPDATE
//=============================================================================

void updateGame(void) {
    game.frameCount++;
    
    switch(game.gameState) {
        case STATE_TITLE:
            drawTitleScreen();
            break;
            
        case STATE_PLAYING:
            updatePlayer();
            updateBullets();
            updateEnemies();
            updateItems();
            updateCamera();
            drawGame();
            break;
            
        case STATE_PAUSED:
            // Just show current frame, maybe flash
            if((game.frameCount / 15) % 2) {
                pal_bg_mem[0] = RGB15_C(4, 4, 8);
            } else {
                pal_bg_mem[0] = RGB15_C(0, 0, 4);
            }
            break;
            
        case STATE_GAMEOVER:
            drawGameOver();
            break;
            
        case STATE_WIN:
            drawWinScreen();
            break;
    }
}

//=============================================================================
// MAIN FUNCTION
//=============================================================================

int main(void) {
    // Initialize interrupt handling
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    // Initialize the game
    initGame();
    
    // Main game loop
    while(1) {
        VBlankIntrWait();
        
        handleInput();
        updateGame();
        
        // Copy OAM buffer to hardware OAM
        oam_copy(oam_mem, obj_buffer, 128);
    }
    
    return 0;
}
