#include <tonc.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Simple Kirby-like side scroller prototype for GBA using tonc
// Single file, no external assets.
// -----------------------------------------------------------------------------

#define SCREEN_W 240
#define SCREEN_H 160

#define MAP_W_TILES 64
#define MAP_H_TILES 32

#define TILE_EMPTY   0
#define TILE_GROUND  1
#define TILE_DIRT    2
#define TILE_BLOCK   3

#define PLAYER_W 16
#define PLAYER_H 16

#define FIX_SHIFT 8
#define FIX(n) ((n) << FIX_SHIFT)

#define GRAVITY      20
#define JUMP_VEL    -280
#define MOVE_SPEED    96
#define MAX_FALL     240

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

static u8 world[MAP_H_TILES][MAP_W_TILES];
OBJ_ATTR obj_buffer[128];

typedef struct Player
{
    int x, y;
    int vx, vy;
    int onGround;
    int facing;
} Player;

static Player player;

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

INLINE int clampi(int v, int lo, int hi)
{
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

static int is_solid_tile(int tx, int ty)
{
    if(tx < 0 || ty < 0 || tx >= MAP_W_TILES || ty >= MAP_H_TILES)
        return 1;
    return world[ty][tx] != TILE_EMPTY;
}

static int solid_at_pixel(int px, int py)
{
    int tx = px >> 3;
    int ty = py >> 3;
    return is_solid_tile(tx, ty);
}

// -----------------------------------------------------------------------------
// World generation
// -----------------------------------------------------------------------------

static void build_world(void)
{
    memset(world, 0, sizeof(world));

    for(int y = 24; y < MAP_H_TILES; y++)
        for(int x = 0; x < MAP_W_TILES; x++)
            world[y][x] = (y == 24) ? TILE_GROUND : TILE_DIRT;

    for(int x = 8; x < 14; x++)
        world[20][x] = TILE_BLOCK;

    for(int x = 20; x < 28; x++)
        world[18][x] = TILE_BLOCK;

    for(int x = 34; x < 40; x++)
        world[21][x] = TILE_BLOCK;

    for(int x = 45; x < 52; x++)
        world[16][x] = TILE_BLOCK;

    world[23][16] = TILE_BLOCK;
    world[22][16] = TILE_BLOCK;

    world[23][30] = TILE_BLOCK;
    world[22][30] = TILE_BLOCK;
    world[21][30] = TILE_BLOCK;

    world[23][56] = TILE_BLOCK;
    world[22][57] = TILE_BLOCK;
    world[21][58] = TILE_BLOCK;
    world[20][59] = TILE_BLOCK;
    world[19][60] = TILE_BLOCK;
}

// -----------------------------------------------------------------------------
// Graphics generation
// -----------------------------------------------------------------------------

static void build_bg_tiles(void)
{
    memset(&tile_mem[0][0], 0, 16*32);

    // Tile 1: grass top
    {
        u32 *t = (u32*)&tile_mem[0][1];
        for(int i = 0; i < 8; i++)
        {
            u32 row = 0;
            int c = (i < 3) ? 2 : 3;
            for(int p = 0; p < 8; p++)
                row |= (c & 15) << (p * 4);
            t[i] = row;
        }
    }

    // Tile 2: dirt
    {
        u32 *t = (u32*)&tile_mem[0][2];
        for(int i = 0; i < 8; i++)
        {
            u32 row = 0;
            for(int p = 0; p < 8; p++)
            {
                int c = 3;
                if(((i + p) & 3) == 0) c = 4;
                row |= (c & 15) << (p * 4);
            }
            t[i] = row;
        }
    }

    // Tile 3: block
    {
        u32 *t = (u32*)&tile_mem[0][3];
        for(int y = 0; y < 8; y++)
        {
            u32 row = 0;
            for(int x = 0; x < 8; x++)
            {
                int c = (x == 0 || x == 7 || y == 0 || y == 7) ? 5 : 6;
                row |= (c & 15) << (x * 4);
            }
            t[y] = row;
        }
    }
}

static void build_bg_palette(void)
{
    pal_bg_mem[0] = RGB15(20, 24, 31);
    pal_bg_mem[1] = RGB15(31, 31, 31);
    pal_bg_mem[2] = RGB15( 5, 26,  8);
    pal_bg_mem[3] = RGB15(18, 10,  4);
    pal_bg_mem[4] = RGB15(22, 14,  8);
    pal_bg_mem[5] = RGB15(10, 10, 14);
    pal_bg_mem[6] = RGB15(20, 20, 24);
}

static void build_bg_map(void)
{
    SCR_ENTRY *se = se_mem[28];

    for(int y = 0; y < MAP_H_TILES; y++)
    {
        for(int x = 0; x < MAP_W_TILES; x++)
        {
            int tile = 0;
            switch(world[y][x])
            {
                case TILE_GROUND: tile = 1; break;
                case TILE_DIRT:   tile = 2; break;
                case TILE_BLOCK:  tile = 3; break;
                case TILE_EMPTY:
                default:          tile = 0; break;
            }

            se[y * 64 + x] = tile;
        }
    }
}

static void build_obj_graphics(void)
{
    pal_obj_mem[0] = RGB15(0, 0, 0);
    pal_obj_mem[1] = RGB15(31, 20, 24);
    pal_obj_mem[2] = RGB15(31, 26, 28);
    pal_obj_mem[3] = RGB15(20,  0,  4);
    pal_obj_mem[4] = RGB15(0,   0,  0);
    pal_obj_mem[5] = RGB15(31, 31, 31);

    memset(&tile_mem_obj[0][0], 0, 4 * 32);

    for(int ty = 0; ty < 2; ty++)
    {
        for(int tx = 0; tx < 2; tx++)
        {
            u32 *t = (u32*)&tile_mem_obj[0][ty * 2 + tx];

            for(int py = 0; py < 8; py++)
            {
                u32 row = 0;
                for(int px = 0; px < 8; px++)
                {
                    int x = tx * 8 + px;
                    int y = ty * 8 + py;

                    int dx = x - 8;
                    int dy = y - 8;

                    int color = 0;

                    if(dx * dx + dy * dy <= 52)
                        color = 1;

                    if(dx * dx + dy * dy <= 40 && x < 7 && y < 7)
                        color = 2;

                    if((x == 6 || x == 10) && y >= 6 && y <= 9)
                        color = 4;

                    if((y >= 12) && ((x >= 3 && x <= 6) || (x >= 9 && x <= 12)))
                        color = 3;

                    row |= (color & 15) << (px * 4);
                }
                t[py] = row;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------------

static void init_video(void)
{
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    build_bg_palette();
    build_bg_tiles();
    build_obj_graphics();

    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_4BPP | BG_REG_64x32;

    oam_init(obj_buffer, 128);
}

static void init_game(void)
{
    build_world();
    build_bg_map();

    player.x = FIX(24);
    player.y = FIX(120);
    player.vx = 0;
    player.vy = 0;
    player.onGround = 0;
    player.facing = 1;
}

// -----------------------------------------------------------------------------
// Collision
// -----------------------------------------------------------------------------

static void player_move_x(int dx)
{
    player.x += dx;

    int left   = player.x >> FIX_SHIFT;
    int right  = (player.x >> FIX_SHIFT) + PLAYER_W - 1;
    int top    = player.y >> FIX_SHIFT;
    int bottom = (player.y >> FIX_SHIFT) + PLAYER_H - 1;

    if(dx > 0)
    {
        if(solid_at_pixel(right, top + 1) || solid_at_pixel(right, bottom - 1))
        {
            int tx = right >> 3;
            player.x = FIX((tx << 3) - PLAYER_W);
            player.vx = 0;
        }
    }
    else if(dx < 0)
    {
        if(solid_at_pixel(left, top + 1) || solid_at_pixel(left, bottom - 1))
        {
            int tx = left >> 3;
            player.x = FIX((tx + 1) << 3);
            player.vx = 0;
        }
    }
}

static void player_move_y(int dy)
{
    player.y += dy;
    player.onGround = 0;

    int left   = (player.x >> FIX_SHIFT) + 2;
    int right  = (player.x >> FIX_SHIFT) + PLAYER_W - 3;
    int top    = player.y >> FIX_SHIFT;
    int bottom = (player.y >> FIX_SHIFT) + PLAYER_H - 1;

    if(dy > 0)
    {
        if(solid_at_pixel(left, bottom) || solid_at_pixel(right, bottom))
        {
            int ty = bottom >> 3;
            player.y = FIX((ty << 3) - PLAYER_H);
            player.vy = 0;
            player.onGround = 1;
        }
    }
    else if(dy < 0)
    {
        if(solid_at_pixel(left, top) || solid_at_pixel(right, top))
        {
            int ty = top >> 3;
            player.y = FIX((ty + 1) << 3);
            player.vy = 0;
        }
    }
}

// -----------------------------------------------------------------------------
// Update
// -----------------------------------------------------------------------------

static void update_player(void)
{
    key_poll();

    player.vx = 0;

    if(key_is_down(KEY_LEFT))
    {
        player.vx = -MOVE_SPEED;
        player.facing = 0;
    }
    if(key_is_down(KEY_RIGHT))
    {
        player.vx = MOVE_SPEED;
        player.facing = 1;
    }

    if(player.onGround && key_hit(KEY_A))
    {
        player.vy = JUMP_VEL;
        player.onGround = 0;
    }

    player.vy += GRAVITY;
    if(player.vy > MAX_FALL)
        player.vy = MAX_FALL;

    player_move_x(player.vx >> FIX_SHIFT);
    player_move_y(player.vy >> FIX_SHIFT);

    int max_x = MAP_W_TILES * 8 - PLAYER_W;
    int max_y = MAP_H_TILES * 8 - PLAYER_H;

    int px = player.x >> FIX_SHIFT;
    int py = player.y >> FIX_SHIFT;

    px = clampi(px, 0, max_x);
    py = clampi(py, 0, max_y);

    player.x = FIX(px);
    player.y = FIX(py);
}

static void update_camera_and_sprite(void)
{
    int px = player.x >> FIX_SHIFT;
    int py = player.y >> FIX_SHIFT;

    int cam_x = px - SCREEN_W / 2 + PLAYER_W / 2;
    int cam_y = py - SCREEN_H / 2 + PLAYER_H / 2;

    cam_x = clampi(cam_x, 0, MAP_W_TILES * 8 - SCREEN_W);
    cam_y = clampi(cam_y, 0, MAP_H_TILES * 8 - SCREEN_H);

    REG_BG0HOFS = cam_x;
    REG_BG0VOFS = cam_y;

    int sx = px - cam_x;
    int sy = py - cam_y;

    OBJ_ATTR *obj = &obj_buffer[0];
    obj_set_attr(obj,
        ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_SIZE_16,
        ATTR2_ID(0) | ATTR2_PALBANK(0));

    if(player.facing == 0)
        obj->attr1 |= ATTR1_HFLIP;

    obj_set_pos(obj, sx, sy);
}

static void hide_unused_objs(void)
{
    for(int i = 1; i < 128; i++)
        obj_hide(&obj_buffer[i]);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(void)
{
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    init_video();
    init_game();
    hide_unused_objs();

    while(1)
    {
        VBlankIntrWait();

        update_player();
        update_camera_and_sprite();

        oam_copy(oam_mem, obj_buffer, 1);
    }

    return 0;
}
