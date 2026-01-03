//------------------------------------------------------------------------------
// Simple Kirby-like side scroller demo for GBA using tonc (single C file)
//------------------------------------------------------------------------------
// Controls:
//   Left/Right : move
//   A          : jump
//   B          : short attack (if overlapping enemy, defeats it)
//
// Build (example, adjust paths as needed):
//   arm-none-eabi-gcc -mthumb-interwork -O2 -Wall -Wextra -std=c99 \
//       -I<path-to-tonc> -c main.c
//   arm-none-eabi-gcc -mthumb-interwork -nostartfiles -Wl,-Map,main.map \
//       main.o -L<path-to-tonc-lib> -ltonc -o main.elf
//   arm-none-eabi-objcopy -O binary main.elf main.gba
//------------------------------------------------------------------------------

#include <string.h>
#include <tonc.h>

//------------------------------------------------------------------------------
// Constants
//------------------------------------------------------------------------------

#define SCREEN_WIDTH       240
#define SCREEN_HEIGHT      160

#define TILE_SIZE          8
#define MAP_WIDTH_TILES    64
#define MAP_HEIGHT_TILES   32
#define WORLD_WIDTH        (MAP_WIDTH_TILES * TILE_SIZE)

#define PLAYER_WIDTH       16
#define PLAYER_HEIGHT      16

#define ENEMY_WIDTH        16
#define ENEMY_HEIGHT       16

// First solid ground row in tile coordinates (0-31)
#define GROUND_ROW         18

// Tile indices in charblock 0 (background tiles)
#define TILE_SKY           0
#define TILE_GROUND        1
#define TILE_PLATFORM      2

// Mask for tile index inside screen-entry (background map)
#define TILE_INDEX_MASK    0x03FF

//------------------------------------------------------------------------------
// Simple game structs
//------------------------------------------------------------------------------

typedef struct
{
    int x, y;          // world coordinates (pixels, top-left)
    int vx, vy;        // velocity in pixels/frame
    int onGround;      // boolean (0/1)
} Player;

typedef struct
{
    int x, y;          // world coordinates (pixels, top-left)
    int vx;            // horizontal speed
    int leftBound;     // patrol left bound (world x)
    int rightBound;    // patrol right bound
    int alive;         // boolean (0/1)
} Enemy;

//------------------------------------------------------------------------------
// Globals
//------------------------------------------------------------------------------

static Player player;
static Enemy enemy;

static int cameraX = 0;
static int attackTimer = 0;  // frames left of active attack

// Local OAM buffer to build sprite data each frame.
static OBJ_ATTR objBuffer[128];

//------------------------------------------------------------------------------
// Tile data (very simple solid-color tiles, 4bpp)
// Each 8x8 tile is 8 u32 words, each nibble is a pixel index 0-15.
//------------------------------------------------------------------------------

// Sky tile: color index 1
static const u32 tile_sky[8] =
{
    0x11111111, 0x11111111, 0x11111111, 0x11111111,
    0x11111111, 0x11111111, 0x11111111, 0x11111111
};

// Ground tile: color index 2
static const u32 tile_ground[8] =
{
    0x22222222, 0x22222222, 0x22222222, 0x22222222,
    0x22222222, 0x22222222, 0x22222222, 0x22222222
};

// Platform tile: color index 3
static const u32 tile_platform[8] =
{
    0x33333333, 0x33333333, 0x33333333, 0x33333333,
    0x33333333, 0x33333333, 0x33333333, 0x33333333
};

// 16x16 player sprite: 4 tiles (2x2), color index 1
static const u32 heroTiles[4][8] =
{
    {
        0x11111111, 0x11111111, 0x11111111, 0x11111111,
        0x11111111, 0x11111111, 0x11111111, 0x11111111
    },
    {
        0x11111111, 0x11111111, 0x11111111, 0x11111111,
        0x11111111, 0x11111111, 0x11111111, 0x11111111
    },
    {
        0x11111111, 0x11111111, 0x11111111, 0x11111111,
        0x11111111, 0x11111111, 0x11111111, 0x11111111
    },
    {
        0x11111111, 0x11111111, 0x11111111, 0x11111111,
        0x11111111, 0x11111111, 0x11111111, 0x11111111
    }
};

// 16x16 enemy sprite: 4 tiles (2x2), color index 2
static const u32 enemyTiles[4][8] =
{
    {
        0x22222222, 0x22222222, 0x22222222, 0x22222222,
        0x22222222, 0x22222222, 0x22222222, 0x22222222
    },
    {
        0x22222222, 0x22222222, 0x22222222, 0x22222222,
        0x22222222, 0x22222222, 0x22222222, 0x22222222
    },
    {
        0x22222222, 0x22222222, 0x22222222, 0x22222222,
        0x22222222, 0x22222222, 0x22222222, 0x22222222
    },
    {
        0x22222222, 0x22222222, 0x22222222, 0x22222222,
        0x22222222, 0x22222222, 0x22222222, 0x22222222
    }
};

//------------------------------------------------------------------------------
// Helper: get tile index at a world position (x,y) in pixels
//------------------------------------------------------------------------------

static inline u16 get_tile_at_world(int x, int y)
{
    if (x < 0 || x >= WORLD_WIDTH)
        return TILE_SKY;

    if (y < 0 || y >= MAP_HEIGHT_TILES * TILE_SIZE)
        return TILE_SKY;

    int tx = x / TILE_SIZE;
    int ty = y / TILE_SIZE;

    SCR_ENTRY *sb;
    int localX;

    if (tx < 32)
    {
        sb = se_mem[28];        // first screenblock
        localX = tx;
    }
    else
    {
        sb = se_mem[29];        // second screenblock (right half)
        localX = tx - 32;
    }

    int index = ty * 32 + localX;
    return sb[index] & TILE_INDEX_MASK;
}

//------------------------------------------------------------------------------
// Background initialization
//------------------------------------------------------------------------------

static void init_background(void)
{
    // Set background palette (BG palette 0)
    pal_bg_mem[0] = RGB15(0, 0, 0);        // color 0: black / transparent
    pal_bg_mem[1] = RGB15(10, 15, 31);     // sky blue
    pal_bg_mem[2] = RGB15(16, 8, 0);       // brown ground
    pal_bg_mem[3] = RGB15(31, 31, 0);      // yellow platform

    // Load tiles into charblock 0
    memcpy(&tile_mem[0][TILE_SKY],      tile_sky,      sizeof(tile_sky));
    memcpy(&tile_mem[0][TILE_GROUND],   tile_ground,   sizeof(tile_ground));
    memcpy(&tile_mem[0][TILE_PLATFORM], tile_platform, sizeof(tile_platform));

    // Configure BG0: charblock 0, screenblock 28, 4bpp, 64x32 tiles
    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_4BPP | BG_REG_64x32;

    SCR_ENTRY *map0 = se_mem[28]; // left  32x32 block
    SCR_ENTRY *map1 = se_mem[29]; // right 32x32 block

    for (int ty = 0; ty < MAP_HEIGHT_TILES; ty++)
    {
        for (int tx = 0; tx < MAP_WIDTH_TILES; tx++)
        {
            u16 tileIndex = TILE_SKY;

            // Solid ground from GROUND_ROW downward
            if (ty >= GROUND_ROW)
                tileIndex = TILE_GROUND;

            // A simple floating platform above ground
            if (ty == GROUND_ROW - 4 && tx >= 10 && tx < 16)
                tileIndex = TILE_PLATFORM;

            SCR_ENTRY *sb;
            int localX;

            if (tx < 32)
            {
                sb = map0;
                localX = tx;
            }
            else
            {
                sb = map1;
                localX = tx - 32;
            }

            int index = ty * 32 + localX;
            sb[index] = tileIndex;
        }
    }

    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
}

//------------------------------------------------------------------------------
// Sprite initialization
//------------------------------------------------------------------------------

static void init_sprites(void)
{
    // Object palette: indices 1 and 2 used
    pal_obj_mem[0] = RGB15(0, 0, 0);           // transparent
    pal_obj_mem[1] = RGB15(31, 10, 20);        // player (pink-ish)
    pal_obj_mem[2] = RGB15(8, 28, 8);          // enemy (green)

    // Load player tiles at OBJ charblock 4, tile indices 0-3
    memcpy(&tile_mem[4][0], heroTiles,  sizeof(heroTiles));

    // Load enemy tiles at OBJ charblock 4, tile indices 4-7
    memcpy(&tile_mem[4][4], enemyTiles, sizeof(enemyTiles));

    // Initialize local OAM buffer (hides all sprites)
    oam_init(objBuffer, 128);

    // Player sprite: OBJ 0, 16x16, starting at tile index 0, palette bank 0
    obj_set_attr(&objBuffer[0],
                 ATTR0_SQUARE | ATTR0_4BPP,
                 ATTR1_SIZE_16,
                 ATTR2_PALBANK(0) | 0);

    // Enemy sprite: OBJ 1, 16x16, starting at tile index 4, palette bank 0
    obj_set_attr(&objBuffer[1],
                 ATTR0_SQUARE | ATTR0_4BPP,
                 ATTR1_SIZE_16,
                 ATTR2_PALBANK(0) | 4);

    // Copy initial OAM state to hardware
    oam_copy(oam_mem, objBuffer, 128);
}

//------------------------------------------------------------------------------
// Game objects initialization
//------------------------------------------------------------------------------

static void init_player(void)
{
    player.x = 32;                            // world X
    player.y = GROUND_ROW * TILE_SIZE - PLAYER_HEIGHT; // on ground
    player.vx = 0;
    player.vy = 0;
    player.onGround = 1;
}

static void init_enemy(void)
{
    enemy.leftBound  = 160;
    enemy.rightBound = 260;
    enemy.x = 220;
    enemy.y = GROUND_ROW * TILE_SIZE - ENEMY_HEIGHT;
    enemy.vx = -1;
    enemy.alive = 1;
}

//------------------------------------------------------------------------------
// Player update
//------------------------------------------------------------------------------

static void update_player(void)
{
    // Horizontal input
    player.vx = 0;
    if (key_is_down(KEY_LEFT))
        player.vx = -1;
    else if (key_is_down(KEY_RIGHT))
        player.vx = 1;

    // Jump
    if (player.onGround && key_hit(KEY_A))
    {
        player.vy = -5;       // jump impulse
        player.onGround = 0;
    }

    // Simple gravity
    player.vy += 1;
    if (player.vy > 6)
        player.vy = 6;

    // Apply movement
    player.x += player.vx;
    player.y += player.vy;

    // Clamp horizontal to world bounds
    if (player.x < 0)
        player.x = 0;
    if (player.x > WORLD_WIDTH - PLAYER_WIDTH)
        player.x = WORLD_WIDTH - PLAYER_WIDTH;

    // Tile-based collision below feet (supports ground + platform)
    int feetX = player.x + PLAYER_WIDTH / 2;
    int feetY = player.y + PLAYER_HEIGHT;

    u16 tileBelow = get_tile_at_world(feetX, feetY);

    if (tileBelow == TILE_GROUND || tileBelow == TILE_PLATFORM)
    {
        int tileTop = (feetY / TILE_SIZE) * TILE_SIZE;
        player.y = tileTop - PLAYER_HEIGHT;
        player.vy = 0;
        player.onGround = 1;
    }
    else
    {
        player.onGround = 0;
    }

    // Attack logic (short active window when B is pressed)
    if (key_hit(KEY_B))
        attackTimer = 10;

    if (attackTimer > 0)
        attackTimer--;
}

//------------------------------------------------------------------------------
// Enemy update
//------------------------------------------------------------------------------

static void update_enemy(void)
{
    if (!enemy.alive)
        return;

    // Simple back-and-forth patrol on the ground
    enemy.x += enemy.vx;

    if (enemy.x < enemy.leftBound)
    {
        enemy.x = enemy.leftBound;
        enemy.vx = -enemy.vx;
    }
    else if (enemy.x > enemy.rightBound)
    {
        enemy.x = enemy.rightBound;
        enemy.vx = -enemy.vx;
    }

    // Keep enemy snapped to solid ground/platform below
    int feetX = enemy.x + ENEMY_WIDTH / 2;
    int feetY = enemy.y + ENEMY_HEIGHT;

    u16 tileBelow = get_tile_at_world(feetX, feetY);

    if (tileBelow == TILE_GROUND || tileBelow == TILE_PLATFORM)
    {
        int tileTop = (feetY / TILE_SIZE) * TILE_SIZE;
        enemy.y = tileTop - ENEMY_HEIGHT;
    }
}

//------------------------------------------------------------------------------
// Attack vs enemy
//------------------------------------------------------------------------------

static void handle_attack_vs_enemy(void)
{
    if (!enemy.alive || attackTimer <= 0)
        return;

    // Simple AABB overlap test between player and enemy
    int px0 = player.x;
    int py0 = player.y;
    int px1 = player.x + PLAYER_WIDTH;
    int py1 = player.y + PLAYER_HEIGHT;

    int ex0 = enemy.x;
    int ey0 = enemy.y;
    int ex1 = enemy.x + ENEMY_WIDTH;
    int ey1 = enemy.y + ENEMY_HEIGHT;

    int overlap =
        (px0 < ex1) && (px1 > ex0) &&
        (py0 < ey1) && (py1 > ey0);

    if (overlap)
        enemy.alive = 0;
}

//------------------------------------------------------------------------------
// Camera and sprite rendering
//------------------------------------------------------------------------------

static void update_camera(void)
{
    // Keep player roughly centered, clamped to world
    cameraX = player.x + PLAYER_WIDTH / 2 - SCREEN_WIDTH / 2;

    if (cameraX < 0)
        cameraX = 0;
    if (cameraX > WORLD_WIDTH - SCREEN_WIDTH)
        cameraX = WORLD_WIDTH - SCREEN_WIDTH;

    REG_BG0HOFS = cameraX;
}

static void update_sprites(void)
{
    // Player screen position
    int px = player.x - cameraX;
    int py = player.y;

    obj_set_pos(&objBuffer[0], px, py);

    // Enemy screen position or hide if not alive
    if (enemy.alive)
    {
        int ex = enemy.x - cameraX;
        int ey = enemy.y;
        obj_set_pos(&objBuffer[1], ex, ey);
    }
    else
    {
        obj_hide(&objBuffer[1]);
    }

    // Push updated OAM buffer to hardware
    oam_copy(oam_mem, objBuffer, 128);
}

//------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------

int main(void)
{
    // Video mode: Mode 0, BG0 on, sprites on, 1D OBJ mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    init_background();
    init_sprites();
    init_player();
    init_enemy();

    while (1)
    {
        vid_vsync();    // wait for VBlank

        key_poll();     // update key state

        update_player();
        update_enemy();
        handle_attack_vs_enemy();
        update_camera();
        update_sprites();
    }

    // Not reached
    return 0;
}
