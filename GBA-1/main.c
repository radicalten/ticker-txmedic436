// game.c - Simple top-down Zelda-like demo for GBA using tonc (single file)

#include <tonc.h>
#include <stdbool.h>

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------

#define MAP_W       32
#define MAP_H       32
#define TILE_SIZE    8

#define HERO_W      16
#define HERO_H      16
#define HERO_SPEED   1

#define ENEMY_W     16
#define ENEMY_H     16
#define ENEMY_SPEED  1

#define MAX_HEARTS   3
#define HURT_TIME   30    // frames of invincibility after getting hit

// OBJ indices
#define OBJ_HERO     0
#define OBJ_ENEMY    1
#define OBJ_HEARTS   2    // 2,3,4 used

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

static u16 level[MAP_H][MAP_W];   // 0 = floor, 1 = wall

static OBJ_ATTR obj_buffer[128];

static int hero_x, hero_y;        // world coordinates (pixels, top-left)
static int enemy_x, enemy_y;
static int enemy_dir;             // -1 or +1 for horizontal patrol

static int cam_x, cam_y;          // camera offset in pixels

static int player_hp = MAX_HEARTS;
static int hurt_timer = 0;

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

static inline bool rects_overlap(
    int ax, int ay, int aw, int ah,
    int bx, int by, int bw, int bh)
{
    return !(ax + aw <= bx || bx + bw <= ax ||
             ay + ah <= by || by + bh <= ay);
}

// -----------------------------------------------------------------------------
// Map / collision
// -----------------------------------------------------------------------------

static bool is_solid_tile(int tx, int ty)
{
    if (tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H)
        return true;    // treat outside as solid
    return level[ty][tx] == 1;
}

// Check if a 16x16 box at (x,y) collides with wall tiles
static bool box_collides_map(int x, int y)
{
    int left   = x;
    int right  = x + 15;
    int top    = y;
    int bottom = y + 15;

    int tl_tx = left   / TILE_SIZE;
    int tl_ty = top    / TILE_SIZE;
    int tr_tx = right  / TILE_SIZE;
    int tr_ty = top    / TILE_SIZE;
    int bl_tx = left   / TILE_SIZE;
    int bl_ty = bottom / TILE_SIZE;
    int br_tx = right  / TILE_SIZE;
    int br_ty = bottom / TILE_SIZE;

    if (is_solid_tile(tl_tx, tl_ty)) return true;
    if (is_solid_tile(tr_tx, tr_ty)) return true;
    if (is_solid_tile(bl_tx, bl_ty)) return true;
    if (is_solid_tile(br_tx, br_ty)) return true;

    return false;
}

// -----------------------------------------------------------------------------
// Graphics init (BG + OBJ tiles & palettes)
// -----------------------------------------------------------------------------

static void init_bg_tiles_and_palette(void)
{
    // BG0 uses charblock 0, 4bpp tiles.
    u32 *bgTiles = (u32*)CHAR_BASE_BLOCK(0);

    // Simple palette: 0=transparent/black, 1=dark grass, 2=light grass, 3=wall
    pal_bg_mem[0] = RGB15(0, 0, 0);
    pal_bg_mem[1] = RGB15(4, 12, 4);   // dark grass
    pal_bg_mem[2] = RGB15(8, 20, 8);   // light grass / floor
    pal_bg_mem[3] = RGB15(12, 10, 4);  // wall

    // 4bpp tiles: each tile = 8 u32 (8 rows * 4 bytes)
    // Each nibble is a color index (0..15). Use 0x11111111 * color to fill.

    // Tile 0: floor (palette index 2)
    u32 val = 0x11111111u * 2;
    for (int i = 0; i < 8; i++)
        bgTiles[0 * 8 + i] = val;

    // Tile 1: wall (palette index 3)
    val = 0x11111111u * 3;
    for (int i = 0; i < 8; i++)
        bgTiles[1 * 8 + i] = val;
}

static void init_obj_tiles_and_palette(void)
{
    // Simple OBJ palette: 0=transparent, 1=hero, 2=enemy, 3=heart
    pal_obj_mem[0] = RGB15(0, 0, 0);      // transparent
    pal_obj_mem[1] = RGB15(4, 20, 10);    // hero green
    pal_obj_mem[2] = RGB15(20, 8, 0);     // enemy orange
    pal_obj_mem[3] = RGB15(31, 0, 0);     // heart red

    u32 *objTiles = (u32*)MEM_OBJ_TILES;

    // Hero: 16x16 (4 tiles: 0..3), palette index 1
    u32 val = 0x11111111u * 1;
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 8; i++)
            objTiles[(0 + t) * 8 + i] = val;

    // Enemy: 16x16 (4 tiles: 4..7), palette index 2
    val = 0x11111111u * 2;
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 8; i++)
            objTiles[(4 + t) * 8 + i] = val;

    // Heart: 8x8 (1 tile: 8), palette index 3
    // Just a solid red square for now.
    val = 0x11111111u * 3;
    for (int i = 0; i < 8; i++)
        objTiles[8 * 8 + i] = val;
}

// -----------------------------------------------------------------------------
// Level generation (simple room with some walls)
// -----------------------------------------------------------------------------

static void init_level(void)
{
    u16 *mapBase = SCREEN_BASE_BLOCK(31);   // BG0 map in screenblock 31

    for (int y = 0; y < MAP_H; y++)
    {
        for (int x = 0; x < MAP_W; x++)
        {
            u16 tile = 0; // floor

            // Border walls
            if (x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1)
                tile = 1;

            // Vertical wall in the middle
            if (x == 10 && y > 2 && y < MAP_H - 3)
                tile = 1;

            // Horizontal wall across
            if (y == 15 && x > 5 && x < MAP_W - 6)
                tile = 1;

            level[y][x] = tile;
            mapBase[y * MAP_W + x] = tile;
        }
    }
}

// -----------------------------------------------------------------------------
// Objects & game state init
// -----------------------------------------------------------------------------

static void init_objects(void)
{
    oam_init(obj_buffer, 128);

    // Hero starting position
    hero_x = 4 * TILE_SIZE;
    hero_y = 4 * TILE_SIZE;

    // Enemy starting position (on other side of some walls)
    enemy_x = 20 * TILE_SIZE;
    enemy_y = 10 * TILE_SIZE;
    enemy_dir = 1;

    // Hero sprite
    obj_set_attr(&obj_buffer[OBJ_HERO],
        ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG,
        ATTR1_SIZE_16,
        ATTR2_BUILD(0, 0, 0));      // tile index 0, priority 0, palbank 0

    // Enemy sprite
    obj_set_attr(&obj_buffer[OBJ_ENEMY],
        ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG,
        ATTR1_SIZE_16,
        ATTR2_BUILD(4, 0, 0));      // tile index 4

    // Heart sprites (3 hearts)
    for (int i = 0; i < MAX_HEARTS; i++)
    {
        OBJ_ATTR *ho = &obj_buffer[OBJ_HEARTS + i];
        obj_set_attr(ho,
            ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG,
            ATTR1_SIZE_8,
            ATTR2_BUILD(8, 0, 0));  // tile index 8
        // Position will be set in update_sprites()
    }

    // Hide remaining OBJ
    for (int i = OBJ_HEARTS + MAX_HEARTS; i < 128; i++)
        obj_hide(&obj_buffer[i]);

    player_hp = MAX_HEARTS;
    hurt_timer = 0;
}

static void init_video(void)
{
    // Video mode: Mode 0, BG0 enabled, OBJ enabled, 1D OBJ mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG0: charblock 0, screenblock 31, 4bpp, 32x32
    REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32;

    init_bg_tiles_and_palette();
    init_obj_tiles_and_palette();
    init_level();

    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
}

// -----------------------------------------------------------------------------
// Game logic
// -----------------------------------------------------------------------------

static void move_hero(int dx, int dy)
{
    if (dx != 0)
    {
        int newX = hero_x + dx;
        if (!box_collides_map(newX, hero_y))
            hero_x = newX;
    }

    if (dy != 0)
    {
        int newY = hero_y + dy;
        if (!box_collides_map(hero_x, newY))
            hero_y = newY;
    }
}

static void handle_input(void)
{
    int dx = 0, dy = 0;

    if (key_is_down(KEY_LEFT))  dx -= HERO_SPEED;
    if (key_is_down(KEY_RIGHT)) dx += HERO_SPEED;
    if (key_is_down(KEY_UP))    dy -= HERO_SPEED;
    if (key_is_down(KEY_DOWN))  dy += HERO_SPEED;

    // Prevent movement after "death"
    if (player_hp > 0)
        move_hero(dx, dy);
}

static void update_enemy(void)
{
    // Simple horizontal patrol, bounce off walls
    int newX = enemy_x + enemy_dir * ENEMY_SPEED;

    if (box_collides_map(newX, enemy_y))
    {
        enemy_dir = -enemy_dir;
    }
    else
    {
        enemy_x = newX;
    }

    // Hero takes damage on contact, with invincibility window
    if (player_hp > 0)
    {
        if (hurt_timer > 0)
        {
            hurt_timer--;
        }
        else
        {
            if (rects_overlap(hero_x, hero_y, HERO_W, HERO_H,
                              enemy_x, enemy_y, ENEMY_W, ENEMY_H))
            {
                player_hp--;
                if (player_hp < 0) player_hp = 0;
                hurt_timer = HURT_TIME;
            }
        }
    }
}

static void update_camera(void)
{
    int map_w_px = MAP_W * TILE_SIZE;
    int map_h_px = MAP_H * TILE_SIZE;

    cam_x = hero_x + HERO_W / 2 - 120; // center hero
    cam_y = hero_y + HERO_H / 2 - 80;

    if (cam_x < 0) cam_x = 0;
    if (cam_y < 0) cam_y = 0;

    int max_cam_x = map_w_px - 240;
    int max_cam_y = map_h_px - 160;

    if (max_cam_x < 0) max_cam_x = 0;
    if (max_cam_y < 0) max_cam_y = 0;

    if (cam_x > max_cam_x) cam_x = max_cam_x;
    if (cam_y > max_cam_y) cam_y = max_cam_y;

    REG_BG0HOFS = cam_x;
    REG_BG0VOFS = cam_y;
}

static void update_sprites(void)
{
    // Hero screen position
    int hero_sx = hero_x - cam_x;
    int hero_sy = hero_y - cam_y;

    // Flash hero when hurt
    if (hurt_timer > 0 && (hurt_timer & 2))
        obj_hide(&obj_buffer[OBJ_HERO]);
    else
        obj_unhide(&obj_buffer[OBJ_HERO], ATTR0_REG);

    obj_set_pos(&obj_buffer[OBJ_HERO], hero_sx, hero_sy);

    // Enemy screen position
    int enemy_sx = enemy_x - cam_x;
    int enemy_sy = enemy_y - cam_y;
    obj_set_pos(&obj_buffer[OBJ_ENEMY], enemy_sx, enemy_sy);

    // Hearts (HUD, fixed to screen, not camera)
    for (int i = 0; i < MAX_HEARTS; i++)
    {
        OBJ_ATTR *ho = &obj_buffer[OBJ_HEARTS + i];
        if (i < player_hp)
        {
            obj_unhide(ho, ATTR0_REG);
            obj_set_pos(ho, 4 + i * 10, 4);
        }
        else
        {
            obj_hide(ho);
        }
    }

    // Commit to hardware OAM
    oam_copy(OAM, obj_buffer, 128);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(void)
{
    irq_init(NULL);
    irq_enable(IRQ_VBLANK);

    init_video();
    init_objects();

    while (1)
    {
        vid_vsync();
        key_poll();

        handle_input();
        update_enemy();
        update_camera();
        update_sprites();
    }

    return 0;
}
