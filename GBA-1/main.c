// metroid_like.c
// Single-file minimal Metroid-like for GBA using tonc + maxmod
//
// Build example (adjust paths/libs as needed):
//   arm-none-eabi-gcc -mthumb -mthumb-interwork -O2 -Wall -std=c99 \
//     metroid_like.c -o metroid_like.elf \
//     -ltonc -lmaxmod -lgba
//   arm-none-eabi-objcopy -O binary metroid_like.elf metroid_like.gba
//
// NOTE ABOUT AUDIO:
//  - This file contains a *dummy* Maxmod soundbank so it compiles.
//  - To get real sound, generate soundbank_bin + soundbank.h with mmutil,
//    then replace the dummy soundbank_bin & MOD_/SFX_ definitions here
//    with the generated ones.

#include <tonc.h>
#include <maxmod.h>
#include <string.h>
#include <stdbool.h>

#define RGB15_C(r,g,b)  ((r) | ((g) << 5) | ((b) << 10))

// --------------------------------------------------------------------
// Dummy Maxmod soundbank + IDs
// --------------------------------------------------------------------

// Minimal dummy soundbank so maxmod links. Replace with real data.
const unsigned char soundbank_bin[16] = { 0 };

// Replace these with IDs from your generated soundbank.h
#define MOD_MUSIC   0
#define SFX_JUMP    0
#define SFX_SHOOT   1

// --------------------------------------------------------------------
// Game constants
// --------------------------------------------------------------------

#define SCREEN_W    240
#define SCREEN_H    160

#define MAP_W       32
#define MAP_H       20    // logical map height; rows 20..31 unused/solid

#define TILE_EMPTY  0
#define TILE_SOLID  1
#define TILE_PLATFORM 2

#define PLAYER_W    16
#define PLAYER_H    16
#define PLAYER_SPEED    2
#define GRAVITY         1
#define JUMP_SPEED      8
#define MAX_FALL_SPEED  8

#define BULLET_SPEED    4
#define MAX_BULLETS     4

// Sprite indices in OAM
#define SPR_ID_PLAYER   0
#define SPR_ID_ENEMY    1
#define SPR_ID_BULLETS  2   // bullets use 2..(2+MAX_BULLETS-1)

// --------------------------------------------------------------------
// Simple data structures
// --------------------------------------------------------------------

typedef struct
{
    int x, y;          // top-left in pixels
    int dx, dy;
    int on_ground;
    int facing;        // -1 = left, +1 = right
} Player;

typedef struct
{
    int x, y;          // top-left in pixels
    int alive;
} Enemy;

typedef struct
{
    int x, y;
    int dx;
    int active;
} Bullet;

// --------------------------------------------------------------------
// Global game state
// --------------------------------------------------------------------

static Player player;
static Enemy enemy;
static Bullet bullets[MAX_BULLETS];

static u16 level_map[MAP_W * MAP_H];    // tile indices

static OBJ_ATTR obj_buffer[128];

// --------------------------------------------------------------------
// Graphics data (very tiny placeholder art)
// 4bpp tiles: each tile is 8 u32 = 32 bytes
// --------------------------------------------------------------------

// BG tiles (charblock 0)
// tile 0: empty
// tile 1: solid block
// tile 2: platform block
static const u32 bg_tile_data[3][8] =
{
    // Tile 0: empty (all 0 = color 0)
    {
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000
    },
    // Tile 1: solid block (color index 1)
    {
        0x11111111, 0x11111111, 0x11111111, 0x11111111,
        0x11111111, 0x11111111, 0x11111111, 0x11111111
    },
    // Tile 2: platform (color index 2, striped)
    {
        0x22222222, 0x22222222, 0x00000000, 0x22222222,
        0x22222222, 0x00000000, 0x22222222, 0x22222222
    }
};

// OBJ tiles (separate from BG tiles; stored in OBJ VRAM)
// Very simple colored squares/rectangles

// 16x16 player tile (color index 1)
static const u32 spr_tile_player[8] =
{
    0x11111111, 0x11111111, 0x11111111, 0x11111111,
    0x11111111, 0x11111111, 0x11111111, 0x11111111
};

// 16x16 enemy tile (color index 2)
static const u32 spr_tile_enemy[8] =
{
    0x22222222, 0x22222222, 0x22222222, 0x22222222,
    0x22222222, 0x22222222, 0x22222222, 0x22222222
};

// 8x8 bullet tile (color index 3)
static const u32 spr_tile_bullet[8] =
{
    0x33333333, 0x33333333, 0x33333333, 0x33333333,
    0x33333333, 0x33333333, 0x33333333, 0x33333333
};

// BG palette (16 entries)
static const u16 bg_palette[16] =
{
    RGB15_C(0,0,0),       // 0: transparent/black
    RGB15_C(8,8,8),       // 1: dark grey (solid ground)
    RGB15_C(31,16,0),     // 2: orange (platform)
    RGB15_C(0,0,0),       // 3
    RGB15_C(0,0,0),       // 4
    RGB15_C(0,0,0),       // 5
    RGB15_C(0,0,0),       // 6
    RGB15_C(0,0,0),       // 7
    RGB15_C(0,0,0),       // 8
    RGB15_C(0,0,0),       // 9
    RGB15_C(0,0,0),       // 10
    RGB15_C(0,0,0),       // 11
    RGB15_C(0,0,0),       // 12
    RGB15_C(0,0,0),       // 13
    RGB15_C(0,0,0),       // 14
    RGB15_C(0,0,0),       // 15
};

// OBJ palette (16 entries)
static const u16 obj_palette[16] =
{
    RGB15_C(0,0,0),       // 0: transparent
    RGB15_C(0,31,0),      // 1: green player
    RGB15_C(31,0,0),      // 2: red enemy
    RGB15_C(31,31,0),     // 3: yellow bullet
    RGB15_C(0,0,0),       // 4
    RGB15_C(0,0,0),       // 5
    RGB15_C(0,0,0),       // 6
    RGB15_C(0,0,0),       // 7
    RGB15_C(0,0,0),       // 8
    RGB15_C(0,0,0),       // 9
    RGB15_C(0,0,0),       // 10
    RGB15_C(0,0,0),       // 11
    RGB15_C(0,0,0),       // 12
    RGB15_C(0,0,0),       // 13
    RGB15_C(0,0,0),       // 14
    RGB15_C(0,0,0)        // 15
};

// --------------------------------------------------------------------
// Helpers: map & collision
// --------------------------------------------------------------------

static inline bool tile_solid(int tx, int ty)
{
    if (tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H)
        return true; // outside map = solid
    u16 t = level_map[ty * MAP_W + tx];
    return (t == TILE_SOLID || t == TILE_PLATFORM);
}

static bool rect_collides_map(int x, int y, int w, int h)
{
    int left   = x / 8;
    int right  = (x + w - 1) / 8;
    int top    = y / 8;
    int bottom = (y + h - 1) / 8;

    for (int ty = top; ty <= bottom; ty++)
        for (int tx = left; tx <= right; tx++)
            if (tile_solid(tx, ty))
                return true;

    return false;
}

// --------------------------------------------------------------------
// Initialization
// --------------------------------------------------------------------

static void init_video(void)
{
    // Mode 0, BG0 enabled, OBJ enabled, 1D OBJ mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG0: charblock 0, screenblock 31, 4bpp, 32x32
    REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32;

    // Load BG palette
    memcpy(pal_bg_mem, bg_palette, sizeof(bg_palette));

    // Load OBJ palette
    memcpy(pal_obj_mem, obj_palette, sizeof(obj_palette));

    // Load BG tiles into charblock 0
    // Tiles 0..2
    memcpy(&tile_mem[0][0], bg_tile_data[0], sizeof(bg_tile_data));

    // Load sprite tiles into OBJ VRAM (tile_mem_obj)
    // Player: 16x16 uses 4 tiles starting at index 0..3
    for (int i = 0; i < 4; i++)
        memcpy(&tile_mem_obj[0][i], spr_tile_player, sizeof(spr_tile_player));

    // Enemy: 16x16 uses tiles 4..7
    for (int i = 4; i < 8; i++)
        memcpy(&tile_mem_obj[0][i], spr_tile_enemy, sizeof(spr_tile_enemy));

    // Bullet: 8x8 uses tile 8
    memcpy(&tile_mem_obj[0][8], spr_tile_bullet, sizeof(spr_tile_bullet));
}

static void init_level(void)
{
    // Clear logical map
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            level_map[y * MAP_W + x] = TILE_EMPTY;

    // Ground: last logical row (y = MAP_H-1)
    int ground_y = MAP_H - 1;
    for (int x = 0; x < MAP_W; x++)
        level_map[ground_y * MAP_W + x] = TILE_SOLID;

    // Simple platforms
    // Platform 1 at y=14, x=4..10
    for (int x = 4; x <= 10; x++)
        level_map[14 * MAP_W + x] = TILE_PLATFORM;

    // Platform 2 at y=10, x=16..23
    for (int x = 16; x <= 23; x++)
        level_map[10 * MAP_W + x] = TILE_PLATFORM;

    // Write map to BG screenblock 31 (32x32 entries)
    for (int y = 0; y < 32; y++)
    {
        for (int x = 0; x < 32; x++)
        {
            u16 tile = TILE_SOLID;   // default for unused rows
            if (y < MAP_H && x < MAP_W)
                tile = level_map[y * MAP_W + x];

            se_mem[31][y * 32 + x] = tile;
        }
    }

    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
}

static void init_player(void)
{
    player.x = 16;
    player.y = (MAP_H - 2) * 8 - PLAYER_H;  // near ground
    player.dx = 0;
    player.dy = 0;
    player.on_ground = 0;
    player.facing = 1;
}

static void init_enemy(void)
{
    enemy.x = 160;
    enemy.y = (MAP_H - 2) * 8 - PLAYER_H;
    enemy.alive = 1;
}

static void init_bullets(void)
{
    for (int i = 0; i < MAX_BULLETS; i++)
        bullets[i].active = 0;
}

static void init_sprites(void)
{
    oam_init(obj_buffer, 128);

    // Player sprite: 16x16, tile index 0, palette bank 0
    obj_set_attr(&obj_buffer[SPR_ID_PLAYER],
        ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_SIZE_16,
        ATTR2_PALBANK(0) | 0);

    // Enemy sprite: 16x16, tile index 4, palette bank 0
    obj_set_attr(&obj_buffer[SPR_ID_ENEMY],
        ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_SIZE_16,
        ATTR2_PALBANK(0) | 4);

    // Bullet sprites: 8x8, tile index 8, palette bank 0
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        OBJ_ATTR *obj = &obj_buffer[SPR_ID_BULLETS + i];
        obj_set_attr(obj,
            ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_SIZE_8,
            ATTR2_PALBANK(0) | 8);

        obj_hide(obj);
    }

    oam_copy(oam_mem, obj_buffer, 128);
}


// --------------------------------------------------------------------
// Gameplay logic
// --------------------------------------------------------------------

static void spawn_bullet(void)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        if (!bullets[i].active)
        {
            bullets[i].active = 1;
            bullets[i].dx = (player.facing > 0) ? BULLET_SPEED : -BULLET_SPEED;
            bullets[i].x = player.x + PLAYER_W / 2 - 4;
            bullets[i].y = player.y + PLAYER_H / 2 - 4;

            // Play shoot sound
            mmEffect(SFX_SHOOT);
            return;
        }
    }
}

static void update_player(void)
{
    // Poll input
    key_poll();

    // Horizontal movement
    player.dx = 0;
    if (key_is_down(KEY_LEFT))
    {
        player.dx = -PLAYER_SPEED;
        player.facing = -1;
    }
    else if (key_is_down(KEY_RIGHT))
    {
        player.dx = PLAYER_SPEED;
        player.facing = 1;
    }

    // Jump
    if (key_hit(KEY_A) && player.on_ground)
    {
        player.dy = -JUMP_SPEED;
        player.on_ground = 0;
        mmEffect(SFX_JUMP);
    }

    // Shoot
    if (key_hit(KEY_B))
        spawn_bullet();

    // Apply gravity
    player.dy += GRAVITY;
    if (player.dy > MAX_FALL_SPEED)
        player.dy = MAX_FALL_SPEED;

    // Horizontal move with collision
    int new_x = player.x + player.dx;
    if (!rect_collides_map(new_x, player.y, PLAYER_W, PLAYER_H))
    {
        player.x = new_x;
    }
    else
    {
        // Blocked horizontally
        player.dx = 0;
    }

    // Vertical move with collision
    int new_y = player.y + player.dy;
    if (!rect_collides_map(player.x, new_y, PLAYER_W, PLAYER_H))
    {
        player.y = new_y;
        player.on_ground = 0;
    }
    else
    {
        // Resolve collision step-by-step (down or up)
        if (player.dy > 0)
        {
            // Falling: move down until just before collision
            while (!rect_collides_map(player.x, player.y + 1, PLAYER_W, PLAYER_H))
                player.y++;

            player.on_ground = 1;
        }
        else if (player.dy < 0)
        {
            // Going up: move up until just before collision
            while (!rect_collides_map(player.x, player.y - 1, PLAYER_W, PLAYER_H))
                player.y--;
        }

        player.dy = 0;
    }

    // Clamp to screen bounds (simple safety)
    if (player.x < 0) player.x = 0;
    if (player.x > SCREEN_W - PLAYER_W) player.x = SCREEN_W - PLAYER_W;
    if (player.y < 0) player.y = 0;
    if (player.y > SCREEN_H - PLAYER_H) player.y = SCREEN_H - PLAYER_H;
}

static void update_bullets(void)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        if (!bullets[i].active)
            continue;

        bullets[i].x += bullets[i].dx;

        // Off-screen?
        if (bullets[i].x < -8 || bullets[i].x > SCREEN_W)
        {
            bullets[i].active = 0;
            continue;
        }

        // Hit solid tile?
        int tx = bullets[i].x / 8;
        int ty = bullets[i].y / 8;
        if (tile_solid(tx, ty))
        {
            bullets[i].active = 0;
            continue;
        }

        // Hit enemy?
        if (enemy.alive)
        {
            int ex0 = enemy.x;
            int ey0 = enemy.y;
            int ex1 = ex0 + PLAYER_W;
            int ey1 = ey0 + PLAYER_H;

            int bx0 = bullets[i].x;
            int by0 = bullets[i].y;
            int bx1 = bx0 + 8;
            int by1 = by0 + 8;

            if (bx0 < ex1 && bx1 > ex0 && by0 < ey1 && by1 > ey0)
            {
                enemy.alive = 0;
                bullets[i].active = 0;
                // You could play a hit sound here if you have one
            }
        }
    }
}

static void update_enemy(void)
{
    // Simple: enemy stands still if alive
    (void)0;
}

// --------------------------------------------------------------------
// Rendering
// --------------------------------------------------------------------

static void draw_sprites(void)
{
    // Player
    obj_set_pos(&obj_buffer[SPR_ID_PLAYER], player.x, player.y);

    // Enemy
    if (enemy.alive)
        obj_set_pos(&obj_buffer[SPR_ID_ENEMY], enemy.x, enemy.y);
    else
        obj_hide(&obj_buffer[SPR_ID_ENEMY]);

    // Bullets
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        OBJ_ATTR *obj = &obj_buffer[SPR_ID_BULLETS + i];
        if (bullets[i].active)
            obj_set_pos(obj, bullets[i].x, bullets[i].y);
        else
            obj_hide(obj);
    }

    // Copy to OAM
    oam_copy(oam_mem, obj_buffer, 128);
}

// --------------------------------------------------------------------
// Main
// --------------------------------------------------------------------

int main(void)
{
    REG_IME = 0;

    init_video();
    init_level();
    init_player();
    init_enemy();
    init_bullets();
    init_sprites();
    init_sound();

    REG_IME = 1;

    while (1)
    {
        VBlankIntrWait();

        update_player();
        update_bullets();
        update_enemy();
        draw_sprites();
    }

    return 0;
}
