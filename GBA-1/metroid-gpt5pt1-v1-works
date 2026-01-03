//-----------------------------------------------------------------------------
// Simple Metroid-like platformer demo for GBA using tonc
// Single-file C program, requires tonclib
//
// Controls:
//   D-Pad left/right : move
//   A                : jump
//   B                : shoot
//-----------------------------------------------------------------------------

#include <tonc.h>

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------

#define SCREEN_W      240
#define SCREEN_H      160

#define TILE_SIZE     8
#define LEVEL_W       30        // tiles (fits 240px)
#define LEVEL_H       20        // tiles (fits 160px)

#define GRAVITY       1
#define JUMP_SPEED   -6
#define MAX_FALL      6
#define MOVE_SPEED    2

#define BULLET_SPEED  4

//-----------------------------------------------------------------------------
// Level data (generated at runtime)
//-----------------------------------------------------------------------------

static u8 level[LEVEL_H][LEVEL_W];   // 0 = empty, 1 = solid

//-----------------------------------------------------------------------------
// Simple 4bpp tiles (8x8)
//-----------------------------------------------------------------------------

// Background solid tile (brick/block)
const TILE4 tile_solid =
{
    {
        0x11111111,     // ####....
        0x12222221,     // #......#
        0x12222221,
        0x12222221,
        0x12222221,
        0x12222221,
        0x12222221,
        0x11111111
    }
};

// Player 8x8 tile (solid square)
const TILE4 tile_player =
{
    {
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111
    }
};

// Bullet 8x8 tile (smaller dot in center)
const TILE4 tile_bullet =
{
    {
        0x00000000,
        0x00000000,
        0x00222000,
        0x02222200,
        0x02222200,
        0x00222000,
        0x00000000,
        0x00000000
    }
};

//-----------------------------------------------------------------------------
// Player and bullet state
//-----------------------------------------------------------------------------

typedef struct
{
    int x, y;        // position in pixels
    int vx, vy;      // velocity in pixels/frame
    int w, h;        // size in pixels
    int onGround;    // bool (0/1)
    int facing;      // -1 left, +1 right
} Player;

typedef struct
{
    int active;      // bool
    int x, y;        // position in pixels
    int vx;          // horizontal speed
} Bullet;

static Player player;
static Bullet bullet;

// OAM shadow
static OBJ_ATTR obj_buffer[128];

//-----------------------------------------------------------------------------
// Level generation and collision helpers
//-----------------------------------------------------------------------------

static void add_platform(int x0, int y, int length)
{
    if(y < 0 || y >= LEVEL_H)
        return;

    for(int x=x0; x<x0+length && x<LEVEL_W; x++)
    {
        if(x >= 0)
            level[y][x] = 1;
    }
}

static void build_level(void)
{
    // Clear
    for(int y=0; y<LEVEL_H; y++)
        for(int x=0; x<LEVEL_W; x++)
            level[y][x] = 0;

    // Border walls
    for(int x=0; x<LEVEL_W; x++)
    {
        level[LEVEL_H-1][x] = 1;      // floor
    }
    for(int y=0; y<LEVEL_H; y++)
    {
        level[y][0]           = 1;    // left wall
        level[y][LEVEL_W-1]   = 1;    // right wall
    }

    // Some platforms to jump on
    add_platform(4,  14, 10);
    add_platform(15, 12, 8);
    add_platform(8,  9,  6);
    add_platform(18, 7,  5);
    add_platform(3,  5,  4);
}

// Tile collision
static int is_solid_tile(int tx, int ty)
{
    if(tx < 0 || tx >= LEVEL_W || ty < 0 || ty >= LEVEL_H)
        return 1;           // treat out-of-bounds as solid

    return level[ty][tx] != 0;
}

static int is_solid_pixel(int px, int py)
{
    int tx = px / TILE_SIZE;
    int ty = py / TILE_SIZE;
    return is_solid_tile(tx, ty);
}

//-----------------------------------------------------------------------------
// Video / graphics initialization
//-----------------------------------------------------------------------------

static void init_video(void)
{
    // Mode 0, BG0 and sprites enabled, OBJ in 1D mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG0: 4bpp, charblock 0, screenblock 31, 32x32 tiles
    REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32;

    // Background palette
    pal_bg_mem[0] = RGB15(0, 0, 0);      // transparent / black
    pal_bg_mem[1] = RGB15(4, 4, 4);      // dark gray (block edge)
    pal_bg_mem[2] = RGB15(10, 10, 10);   // light gray (block interior)

    // Object palette
    pal_obj_mem[0] = RGB15(0, 0, 0);     // transparent
    pal_obj_mem[1] = RGB15(31, 31, 0);   // player color (yellow)
    pal_obj_mem[2] = RGB15(31, 0, 0);    // bullet color (red)

    // Copy tiles into charblocks
    // BG tiles: charblock 0
    TILE4 *bg_tiles = (TILE4*)tile_mem[0];
    bg_tiles[0] = (TILE4){ {0} };        // tile 0: empty
    bg_tiles[1] = tile_solid;            // tile 1: solid block

    // OBJ tiles: charblock 4 (object VRAM)
    TILE4 *obj_tiles = (TILE4*)tile_mem[4];
    obj_tiles[0] = tile_player;          // sprite tile 0: player
    obj_tiles[1] = tile_bullet;          // sprite tile 1: bullet
}

// Build BG map from level data
static void init_bg_map(void)
{
    u16 *map = se_mem[31];

    // Fill used rows with level data
    for(int y=0; y<LEVEL_H; y++)
    {
        for(int x=0; x<LEVEL_W; x++)
        {
            u16 tid = level[y][x] ? 1 : 0;
            map[y*32 + x] = tid;
        }
        // Clear unused part of row (columns 30..31)
        for(int x=LEVEL_W; x<32; x++)
            map[y*32 + x] = 0;
    }

    // Clear remaining rows (20..31)
    for(int y=LEVEL_H; y<32; y++)
        for(int x=0; x<32; x++)
            map[y*32 + x] = 0;
}

//-----------------------------------------------------------------------------
// Sprite (OAM) initialization
//-----------------------------------------------------------------------------

static void init_sprites(void)
{
    oam_init(obj_buffer, 128);

    // Player sprite: index 0
    OBJ_ATTR *player_obj = &obj_buffer[0];
    obj_set_attr(
        player_obj,
        ATTR0_SQUARE | ATTR0_4BPP,     // shape + 4bpp
        ATTR1_SIZE_8,                  // 8x8
        ATTR2_PALBANK(0) | 0           // use tile index 0, pal bank 0
    );
    obj_set_pos(player_obj, 0, 0);

    // Bullet sprite: index 1
    OBJ_ATTR *bullet_obj = &obj_buffer[1];
    obj_set_attr(
        bullet_obj,
        ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_SIZE_8,
        ATTR2_PALBANK(0) | 1           // tile index 1
    );
    obj_hide(bullet_obj);

    // Hide any others
    for(int i=2; i<128; i++)
        obj_hide(&obj_buffer[i]);
}

//-----------------------------------------------------------------------------
// Player / bullet initialization
//-----------------------------------------------------------------------------

static void init_player(void)
{
    player.w = 8;
    player.h = 8;
    player.x = 3 * TILE_SIZE;                       // start near left
    player.y = (LEVEL_H-2) * TILE_SIZE - player.h;  // just above floor
    player.vx = 0;
    player.vy = 0;
    player.onGround = 0;
    player.facing = 1;
}

static void init_bullet(void)
{
    bullet.active = 0;
    bullet.x = 0;
    bullet.y = 0;
    bullet.vx = 0;
}

//-----------------------------------------------------------------------------
// Movement / physics
//-----------------------------------------------------------------------------

static void move_horizontal(Player *p)
{
    if(p->vx == 0)
        return;

    int newX = p->x + p->vx;

    if(p->vx > 0)        // moving right
    {
        int right = newX + p->w - 1;
        int top    = p->y;
        int bottom = p->y + p->h - 1;

        if(is_solid_pixel(right, top) || is_solid_pixel(right, bottom))
        {
            int tileX = right / TILE_SIZE;
            newX = tileX * TILE_SIZE - p->w;
        }
    }
    else if(p->vx < 0)   // moving left
    {
        int left   = newX;
        int top    = p->y;
        int bottom = p->y + p->h - 1;

        if(is_solid_pixel(left, top) || is_solid_pixel(left, bottom))
        {
            int tileX = left / TILE_SIZE;
            newX = (tileX + 1) * TILE_SIZE;
        }
    }

    p->x = newX;
}

static void move_vertical(Player *p)
{
    p->onGround = 0;

    if(p->vy == 0)
        return;

    int newY = p->y + p->vy;

    if(p->vy > 0)        // falling
    {
        int bottom = newY + p->h - 1;
        int left   = p->x;
        int right  = p->x + p->w - 1;

        if(is_solid_pixel(left, bottom) || is_solid_pixel(right, bottom))
        {
            int tileY = bottom / TILE_SIZE;
            newY = tileY * TILE_SIZE - p->h;
            p->vy = 0;
            p->onGround = 1;
        }
    }
    else if(p->vy < 0)   // rising (jumping)
    {
        int top   = newY;
        int left  = p->x;
        int right = p->x + p->w - 1;

        if(is_solid_pixel(left, top) || is_solid_pixel(right, top))
        {
            int tileY = top / TILE_SIZE;
            newY = (tileY + 1) * TILE_SIZE;
            p->vy = 0;
        }
    }

    p->y = newY;
}

//-----------------------------------------------------------------------------
// Game logic update
//-----------------------------------------------------------------------------

static void update_player(void)
{
    key_poll();

    // Horizontal input
    player.vx = 0;
    if(key_is_down(KEY_LEFT))
    {
        player.vx = -MOVE_SPEED;
        player.facing = -1;
    }
    if(key_is_down(KEY_RIGHT))
    {
        player.vx = MOVE_SPEED;
        player.facing = 1;
    }

    // Jump
    if(player.onGround && key_hit(KEY_A))
    {
        player.vy = JUMP_SPEED;
        player.onGround = 0;
    }

    // Gravity
    player.vy += GRAVITY;
    if(player.vy > MAX_FALL)
        player.vy = MAX_FALL;

    // Apply movement with collision
    move_horizontal(&player);
    move_vertical(&player);
}

static void update_bullet(void)
{
    // Fire new bullet
    if(key_hit(KEY_B) && !bullet.active)
    {
        bullet.active = 1;
        bullet.vx = (player.facing > 0) ? BULLET_SPEED : -BULLET_SPEED;

        // Spawn at player center
        bullet.x = player.x + player.w/2 - 4;
        bullet.y = player.y + player.h/2 - 4;
    }

    if(!bullet.active)
        return;

    // Move bullet
    bullet.x += bullet.vx;

    // Off-screen?
    if(bullet.x < 0 || bullet.x > SCREEN_W - 8)
    {
        bullet.active = 0;
        return;
    }

    // Hit a solid tile?
    int cx = bullet.x + 4;
    int cy = bullet.y + 4;
    if(is_solid_pixel(cx, cy))
    {
        bullet.active = 0;
        return;
    }
}

//-----------------------------------------------------------------------------
// Sprite drawing
//-----------------------------------------------------------------------------

static void draw_sprites(void)
{
    // Player sprite
    OBJ_ATTR *player_obj = &obj_buffer[0];
    obj_set_pos(player_obj, player.x, player.y);

    // Bullet sprite
    OBJ_ATTR *bullet_obj = &obj_buffer[1];
    if(bullet.active)
    {
        obj_unhide(bullet_obj, ATTR0_REG);
        obj_set_pos(bullet_obj, bullet.x, bullet.y);
    }
    else
    {
        obj_hide(bullet_obj);
    }

    // Copy shadow OAM to hardware
    oam_copy(oam_mem, obj_buffer, 128);
}

//-----------------------------------------------------------------------------
// Main
//-----------------------------------------------------------------------------

int main(void)
{
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    build_level();
    init_video();
    init_bg_map();
    init_sprites();
    init_player();
    init_bullet();

    while(1)
    {
        vid_vsync();       // wait for VBlank
        update_player();
        update_bullet();
        draw_sprites();
    }

    return 0;
}
