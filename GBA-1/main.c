// mario_like.c
// Simple side-scrolling platformer example for GBA using tonc
// Requires: tonc (https://www.coranac.com/tonc/)

#include <tonc.h>
#include <stdbool.h>

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  160

#define LEVEL_WIDTH    64      // in tiles (8px each) -> 512px wide
#define LEVEL_HEIGHT   20      // in tiles

#define TILE_SIZE      8

// Player size in pixels
#define PLAYER_W       16
#define PLAYER_H       16

// Physics (pixels per frame, integer)
#define MOVE_SPEED     2
#define GRAVITY        1
#define MAX_FALL_SPEED 8
#define JUMP_VELOCITY  (-10)

// -----------------------------------------------------------------------------
// Global level data
// -----------------------------------------------------------------------------

// 0 = empty, 1 = solid block
static u16 level_map[LEVEL_HEIGHT][LEVEL_WIDTH];

// Camera position in world pixels
static int camera_x = 0;

// -----------------------------------------------------------------------------
// Player
// -----------------------------------------------------------------------------

typedef struct
{
    int x, y;       // world position in pixels (top-left of sprite)
    int vx, vy;     // velocity in pixels per frame
    bool on_ground;
} Player;

static Player player;

// -----------------------------------------------------------------------------
// Level & background setup
// -----------------------------------------------------------------------------

static void init_level_map(void)
{
    int x, y;

    // Clear
    for (y = 0; y < LEVEL_HEIGHT; y++)
        for (x = 0; x < LEVEL_WIDTH; x++)
            level_map[y][x] = 0;

    // Ground row at bottom
    for (x = 0; x < LEVEL_WIDTH; x++)
        level_map[LEVEL_HEIGHT - 1][x] = 1;

    // A simple platform
    for (x = 5; x < 12; x++)
        level_map[LEVEL_HEIGHT - 5][x] = 1;

    // Some steps
    level_map[LEVEL_HEIGHT - 2][20] = 1;
    level_map[LEVEL_HEIGHT - 3][21] = 1;
    level_map[LEVEL_HEIGHT - 4][22] = 1;
    level_map[LEVEL_HEIGHT - 5][23] = 1;

    // Copy to BG0 map (64x32 tiles using screenblocks 28-29)
    //u16 *bg_map = (u16*)SCREEN_BASE_BLOCK(28);   // 2 consecutive SBBs (28,29)
    const int BG_MAP_W = 64;
    const int BG_MAP_H = 32;

    for (y = 0; y < BG_MAP_H; y++)
    {
        for (x = 0; x < BG_MAP_W; x++)
        {
            u16 tile = 0;
            if (y < LEVEL_HEIGHT && x < LEVEL_WIDTH)
                tile = level_map[y][x];   // 0 or 1
            else
                tile = 0;
            //bg_map[y * BG_MAP_W + x] = tile;
        }
    }
}

static void init_background(void)
{
    // BG palette: simple colors
    pal_bg_mem[0] = RGB15(0, 0, 0);          // index 0: black (unused)
    pal_bg_mem[1] = RGB15(10, 15, 31);       // index 1: sky blue
    pal_bg_mem[2] = RGB15(20, 10, 0);        // index 2: brown ground
    pal_bg_mem[3] = RGB15(25, 20, 5);        // index 3: darker ground (unused here)

    // BG tiles in charblock 0
    // 4bpp tile is 32 bytes = 8 u32 words
    //u32 *bg_tiles = (u32*)CHAR_BASE_BLOCK(0);
    const int WORDS_PER_TILE = 8;

    int i;

    // Tile 0: sky (color index 1)
    //for (i = 0; i < WORDS_PER_TILE; i++)
        //bg_tiles[0 * WORDS_PER_TILE + i] = 0x11111111;

    // Tile 1: solid ground (color index 2)
    //for (i = 0; i < WORDS_PER_TILE; i++)
        //bg_tiles[1 * WORDS_PER_TILE + i] = 0x22222222;

    // Set BG0: charblock 0, screenblock 28, 4bpp, size 64x32
    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_4BPP | BG_REG_64x32 | BG_PRIO(1);

    init_level_map();
}

// -----------------------------------------------------------------------------
// Sprite / OBJ setup
// -----------------------------------------------------------------------------

static void init_player_sprite(void)
{
    // Object (sprite) palette
    pal_obj_mem[0] = RGB15(0, 0, 0);     // transparent
    pal_obj_mem[1] = RGB15(31, 0, 0);    // red

    // 4bpp OBJ tiles start at MEM_VRAM_OBJ
    u32 *obj_tiles = (u32*)MEM_VRAM_OBJ;
    const int WORDS_PER_TILE = 8;
    int i;

    // Tile 0: simple red square (color index 1)
    for (i = 0; i < WORDS_PER_TILE; i++)
        obj_tiles[0 * WORDS_PER_TILE + i] = 0x11111111;

    // For a 16x16 sprite in 1D mapping, we need 4 tiles (0,1,2,3).
    // Here we just duplicate the same tile.
    for (i = 0; i < WORDS_PER_TILE; i++)
        obj_tiles[1 * WORDS_PER_TILE + i] = 0x11111111;
    for (i = 0; i < WORDS_PER_TILE; i++)
        obj_tiles[2 * WORDS_PER_TILE + i] = 0x11111111;
    for (i = 0; i < WORDS_PER_TILE; i++)
        obj_tiles[3 * WORDS_PER_TILE + i] = 0x11111111;

    // Init OAM: hide all sprites
    oam_init(oam_mem, 128);

    // Configure player sprite in OAM slot 0
    OBJ_ATTR *obj = &oam_mem[0];
    obj_set_attr(
        obj,
        ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG,
        ATTR1_SIZE_16,
        ATTR2_PALBANK(0) | 0          // palbank 0, tile index 0
    );

    // Initial position (will be updated each frame)
    obj_set_pos(obj, SCREEN_WIDTH/2 - PLAYER_W/2, SCREEN_HEIGHT/2 - PLAYER_H/2);
}

// -----------------------------------------------------------------------------
// Collision helpers
// -----------------------------------------------------------------------------

static bool is_solid_at_pixel(int px, int py)
{
    if (px < 0 || py < 0)
        return false;

    int tx = px / TILE_SIZE;
    int ty = py / TILE_SIZE;

    if (tx < 0 || tx >= LEVEL_WIDTH || ty < 0 || ty >= LEVEL_HEIGHT)
        return false;      // outside defined level -> empty / fall

    return level_map[ty][tx] != 0;
}

// -----------------------------------------------------------------------------
// Player update
// -----------------------------------------------------------------------------

static void update_player(void)
{
    key_poll();

    // Horizontal input
    int move = 0;
    if (key_is_down(KEY_LEFT))
        move -= MOVE_SPEED;
    if (key_is_down(KEY_RIGHT))
        move += MOVE_SPEED;
    player.vx = move;

    // Jump (only when on ground)
    if (key_hit(KEY_A) && player.on_ground)
    {
        player.vy = JUMP_VELOCITY;
        player.on_ground = false;
    }

    // Gravity
    player.vy += GRAVITY;
    if (player.vy > MAX_FALL_SPEED)
        player.vy = MAX_FALL_SPEED;

    // --- Horizontal movement & collision ---
    int new_x = player.x + player.vx;
    if (player.vx > 0)       // moving right
    {
        // Check right edge: top and bottom
        if (!is_solid_at_pixel(new_x + PLAYER_W - 1, player.y + 1) &&
            !is_solid_at_pixel(new_x + PLAYER_W - 1, player.y + PLAYER_H - 1))
        {
            player.x = new_x;
        }
    }
    else if (player.vx < 0)  // moving left
    {
        // Check left edge: top and bottom
        if (!is_solid_at_pixel(new_x, player.y + 1) &&
            !is_solid_at_pixel(new_x, player.y + PLAYER_H - 1))
        {
            player.x = new_x;
        }
    }

    // --- Vertical movement & collision ---
    int new_y = player.y + player.vy;
    if (player.vy > 0)       // moving down
    {
        if (is_solid_at_pixel(player.x + 1, new_y + PLAYER_H - 1) ||
            is_solid_at_pixel(player.x + PLAYER_W - 2, new_y + PLAYER_H - 1))
        {
            // Land on top of a tile
            int tile_y = (new_y + PLAYER_H - 1) / TILE_SIZE;
            player.y = tile_y * TILE_SIZE - PLAYER_H;
            player.vy = 0;
            player.on_ground = true;
        }
        else
        {
            player.y = new_y;
            player.on_ground = false;
        }
    }
    else if (player.vy < 0)  // moving up
    {
        if (is_solid_at_pixel(player.x + 1, new_y) ||
            is_solid_at_pixel(player.x + PLAYER_W - 2, new_y))
        {
            // Hit head on a tile
            int tile_y = new_y / TILE_SIZE;
            player.y = tile_y * TILE_SIZE + TILE_SIZE;
            player.vy = 0;
        }
        else
        {
            player.y = new_y;
        }
    }

    // Keep player within level bounds horizontally
    if (player.x < 0) player.x = 0;
    int max_x = LEVEL_WIDTH * TILE_SIZE - PLAYER_W;
    if (player.x > max_x) player.x = max_x;

    // If player falls off bottom of level, respawn at start
    if (player.y > LEVEL_HEIGHT * TILE_SIZE)
    {
        player.x = 16;
        player.y = (LEVEL_HEIGHT - 2) * TILE_SIZE - PLAYER_H;
        player.vx = player.vy = 0;
        player.on_ground = false;
    }

    // Update camera: try to center on player
    camera_x = player.x + PLAYER_W/2 - SCREEN_WIDTH/2;
    if (camera_x < 0) camera_x = 0;
    int max_cam_x = LEVEL_WIDTH * TILE_SIZE - SCREEN_WIDTH;
    if (max_cam_x < 0) max_cam_x = 0;
    if (camera_x > max_cam_x) camera_x = max_cam_x;

    // Scroll BG0
    REG_BG0HOFS = camera_x;
    REG_BG0VOFS = 0;

    // Update sprite screen position (world - camera)
    int screen_x = player.x - camera_x;
    int screen_y = player.y;
    obj_set_pos(&oam_mem[0], screen_x, screen_y);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(void)
{
    // Init interrupts for VBlank
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);
    irq_enable(II_VBLANK);

    // Display control: Mode 0, BG0 + OBJ, 1D OBJ mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    init_background();
    init_player_sprite();

    // Init player somewhere on the ground
    player.x = 16;
    player.y = (LEVEL_HEIGHT - 2) * TILE_SIZE - PLAYER_H;
    player.vx = 0;
    player.vy = 0;
    player.on_ground = false;

    while (1)
    {
        VBlankIntrWait();   // wait for VBlank
        update_player();
    }

    return 0;
}
