// main.c - single-file "Pokemon-style" mini RPG for GBA using tonc
// Features:
// - Top-down tile map with walls/grass/path
// - Player sprite you can move with D-pad (collision enabled)
// - Random encounters when stepping on grass
// - Simple turn-based battle UI (A: Attack, B: Run)
//
// Build (example; you may already have a Makefile/project):
//   arm-none-eabi-gcc -mthumb -mthumb-interwork -O2 -Wall -Wextra -std=c11 \
//     main.c -o game.elf -ltonc -specs=gba.specs
//   arm-none-eabi-objcopy -O binary game.elf game.gba
//
// Notes:
// - Requires devkitARM + tonc/tonclib properly installed and linkable.
// - No external art assets used; tiles/sprite are generated from simple solid colors.

#include <tonc.h>
#include <string.h>
#include <stdio.h>

// ----------------------------- Config ---------------------------------

#define MAP_W 32
#define MAP_H 32

#define TILE_EMPTY 0
#define TILE_GRASS 1
#define TILE_PATH  2
#define TILE_WALL  3

typedef enum GameMode
{
    MODE_WORLD = 0,
    MODE_BATTLE = 1,
} GameMode;

typedef struct Fighter
{
    const char* name;
    int hp, maxhp;
} Fighter;

// ----------------------------- Globals --------------------------------

static GameMode g_mode = MODE_WORLD;

static OBJ_ATTR obj_buffer[128];

static u8 g_terrain[MAP_W*MAP_H];   // collision/encounter classification
static u16 g_world_se[MAP_W*MAP_H]; // screen entries for BG1

static int g_px = 120;  // player x in pixels (0..239)
static int g_py = 72;   // player y in pixels (0..159)

static int g_last_tx = -1;
static int g_last_ty = -1;

static u32 g_rng = 0x12345678;

static Fighter g_player = { "HERO", 20, 20 };
static Fighter g_enemy  = { "TONCMON", 15, 15 };

static char g_battle_msg[64] = "Wild TONCMON appeared!";

static bool g_ui_dirty = true;

// ----------------------------- Tiny RNG --------------------------------

static inline u32 rng_next(void)
{
    // LCG (common parameters)
    g_rng = (u32)(g_rng * 1664525u + 1013904223u);
    return g_rng;
}

static inline int rng_range(int lo, int hi_inclusive)
{
    u32 r = rng_next();
    int span = hi_inclusive - lo + 1;
    return lo + (int)(r % (u32)span);
}

// ----------------------------- Tiles ----------------------------------
// 4bpp tile is 32 bytes = 8 u32 rows. For a solid palette index p (0..15),
// each byte is 0xPP and each u32 row is 0xPPPPPPPP.

#define SOLID_ROW(pp) ((u32)((pp) * 0x11111111u))

static const u32 bg_tiles[][8] =
{
    // TILE_EMPTY (index 0): all 0
    { 0,0,0,0,0,0,0,0 },

    // TILE_GRASS (index 1): palette index 2 (keep 0/1 for text)
    { SOLID_ROW(0x2),SOLID_ROW(0x2),SOLID_ROW(0x2),SOLID_ROW(0x2),
      SOLID_ROW(0x2),SOLID_ROW(0x2),SOLID_ROW(0x2),SOLID_ROW(0x2) },

    // TILE_PATH (index 2): palette index 3
    { SOLID_ROW(0x3),SOLID_ROW(0x3),SOLID_ROW(0x3),SOLID_ROW(0x3),
      SOLID_ROW(0x3),SOLID_ROW(0x3),SOLID_ROW(0x3),SOLID_ROW(0x3) },

    // TILE_WALL (index 3): palette index 4
    { SOLID_ROW(0x4),SOLID_ROW(0x4),SOLID_ROW(0x4),SOLID_ROW(0x4),
      SOLID_ROW(0x4),SOLID_ROW(0x4),SOLID_ROW(0x4),SOLID_ROW(0x4) },
};

// Simple 16x16 sprite: four identical solid tiles using palette index 1.
static const u32 obj_tiles[][8] =
{
    { SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),
      SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1) },

    { SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),
      SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1) },

    { SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),
      SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1) },

    { SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),
      SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1),SOLID_ROW(0x1) },
};

// ----------------------------- Map ------------------------------------

static inline u8 terrain_at(int tx, int ty)
{
    if(tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H)
        return TILE_WALL;
    return g_terrain[ty*MAP_W + tx];
}

static inline bool is_walkable_tile(u8 t)
{
    return (t != TILE_WALL);
}

static void build_world(void)
{
    // Simple layout:
    // - solid wall border
    // - inside is mostly grass
    // - a cross-shaped path through the middle
    for(int y=0; y<MAP_H; y++)
    {
        for(int x=0; x<MAP_W; x++)
        {
            u8 t = TILE_GRASS;

            if(x==0 || y==0 || x==MAP_W-1 || y==MAP_H-1)
                t = TILE_WALL;

            // Horizontal path
            if(y == 10 && x>1 && x<MAP_W-2)
                t = TILE_PATH;

            // Vertical path
            if(x == 15 && y>1 && y<MAP_H-2)
                t = TILE_PATH;

            // Add a small "room" wall block
            if(x>=22 && x<=28 && y>=4 && y<=8)
            {
                if(x==22 || x==28 || y==4 || y==8)
                    t = TILE_WALL;
                else
                    t = TILE_PATH;
            }

            g_terrain[y*MAP_W + x] = t;
            g_world_se[y*MAP_W + x] = (u16)t; // tile index == terrain id here
        }
    }
}

static void load_world_bg(void)
{
    // Copy map to BG1 screenblock 30
    memcpy16(se_mem[30], g_world_se, sizeof(g_world_se));
}

static void fill_bg1(u16 tile_id)
{
    for(int i=0; i<MAP_W*MAP_H; i++)
        se_mem[30][i] = tile_id;
}

// ----------------------------- UI -------------------------------------

static void ui_world(void)
{
    // BG0 is text layer via TTE
    tte_erase_screen();

    tte_set_pos(0, 0);
    tte_printf("HP %d/%d", g_player.hp, g_player.maxhp);

    tte_set_pos(0, 144);
    tte_write("D-Pad: Move   (grass = encounters)");

    tte_set_pos(0, 152);
    tte_write("A: (nothing)  B: (nothing)");
}

static void ui_battle(void)
{
    tte_erase_screen();

    tte_set_pos(0, 0);
    tte_printf("Battle!");

    tte_set_pos(0, 20);
    tte_printf("%s HP %d/%d", g_enemy.name, g_enemy.hp, g_enemy.maxhp);

    tte_set_pos(0, 40);
    tte_printf("%s HP %d/%d", g_player.name, g_player.hp, g_player.maxhp);

    tte_set_pos(0, 80);
    tte_write(g_battle_msg);

    tte_set_pos(0, 144);
    tte_write("A: Attack   B: Run");
}

// ----------------------------- Mode transitions ------------------------

static void enter_world(void)
{
    g_mode = MODE_WORLD;
    load_world_bg();

    // show player sprite
    obj_buffer[0].attr0 = ATTR0_SQUARE | ATTR0_Y(g_py);
    obj_buffer[0].attr1 = ATTR1_SIZE_16 | ATTR1_X(g_px);
    obj_buffer[0].attr2 = ATTR2_PALBANK(0) | ATTR2_ID(0);

    g_ui_dirty = true;
}

static void enter_battle(void)
{
    g_mode = MODE_BATTLE;

    g_enemy.name = "TONCMON";
    g_enemy.maxhp = 15;
    g_enemy.hp = 15;

    strncpy(g_battle_msg, "Wild TONCMON appeared!", sizeof(g_battle_msg)-1);
    g_battle_msg[sizeof(g_battle_msg)-1] = '\0';

    // simple battle background
    fill_bg1(TILE_PATH);

    // hide player sprite
    obj_buffer[0].attr0 = ATTR0_HIDE;

    g_ui_dirty = true;
}

// ----------------------------- World update ----------------------------

static bool would_collide(int newx, int newy)
{
    // Collision point: near the player's "feet" center.
    // Sprite is 16x16, so feet center ~ (x+8, y+14).
    int cx = newx + 8;
    int cy = newy + 14;

    int tx = cx >> 3;
    int ty = cy >> 3;

    u8 t = terrain_at(tx, ty);
    return !is_walkable_tile(t);
}

static void world_update(void)
{
    key_poll();

    int speed = 2;
    int nx = g_px;
    int ny = g_py;

    if(key_is_down(KEY_LEFT))  nx -= speed;
    if(key_is_down(KEY_RIGHT)) nx += speed;
    if(key_is_down(KEY_UP))    ny -= speed;
    if(key_is_down(KEY_DOWN))  ny += speed;

    // clamp to screen (no camera)
    if(nx < 0) nx = 0;
    if(ny < 0) ny = 0;
    if(nx > 240-16) nx = 240-16;
    if(ny > 160-16) ny = 160-16;

    // collision
    if(!would_collide(nx, ny))
    {
        g_px = nx;
        g_py = ny;
    }

    // tile transition detection (use center point)
    int tx = (g_px + 8) >> 3;
    int ty = (g_py + 8) >> 3;

    if(tx != g_last_tx || ty != g_last_ty)
    {
        g_last_tx = tx;
        g_last_ty = ty;

        // Random encounter on grass
        if(terrain_at(tx, ty) == TILE_GRASS)
        {
            // 1/16 chance per new tile stepped onto
            if((rng_next() & 0x0Fu) == 0)
            {
                enter_battle();
                return;
            }
        }
    }

    // update sprite position
    obj_buffer[0].attr0 = ATTR0_SQUARE | ATTR0_Y(g_py);
    obj_buffer[0].attr1 = ATTR1_SIZE_16 | ATTR1_X(g_px);

    // UI refresh (cheap enough to do occasionally; here only if dirty)
    if(g_ui_dirty)
    {
        ui_world();
        g_ui_dirty = false;
    }
}

// ----------------------------- Battle update ---------------------------

static void battle_update(void)
{
    key_poll();

    if(g_ui_dirty)
    {
        ui_battle();
        g_ui_dirty = false;
    }

    if(key_hit(KEY_A))
    {
        int dmg = rng_range(3, 6);
        g_enemy.hp -= dmg;
        if(g_enemy.hp < 0) g_enemy.hp = 0;

        snprintf(g_battle_msg, sizeof(g_battle_msg),
                 "You hit %s for %d!", g_enemy.name, dmg);

        if(g_enemy.hp <= 0)
        {
            strncpy(g_battle_msg, "Enemy fainted! Back to world.", sizeof(g_battle_msg)-1);
            g_battle_msg[sizeof(g_battle_msg)-1] = '\0';
            g_ui_dirty = true;
            ui_battle();

            // Immediately return (keeps this simple)
            enter_world();
            return;
        }

        // Enemy counter-attack
        int edmg = rng_range(2, 5);
        g_player.hp -= edmg;
        if(g_player.hp < 0) g_player.hp = 0;

        // Show combined message (simple)
        char tmp[64];
        snprintf(tmp, sizeof(tmp), " %s hits %d!", g_enemy.name, edmg);

        // Append if room
        size_t len = strnlen(g_battle_msg, sizeof(g_battle_msg));
        strncat(g_battle_msg, tmp, sizeof(g_battle_msg)-len-1);

        if(g_player.hp <= 0)
        {
            strncpy(g_battle_msg, "You fainted... Restored to full.", sizeof(g_battle_msg)-1);
            g_battle_msg[sizeof(g_battle_msg)-1] = '\0';
            g_player.hp = g_player.maxhp;
            g_ui_dirty = true;
            ui_battle();
            enter_world();
            return;
        }

        g_ui_dirty = true;
    }

    if(key_hit(KEY_B))
    {
        // 50% chance to run
        if(rng_next() & 1u)
        {
            strncpy(g_battle_msg, "Got away safely!", sizeof(g_battle_msg)-1);
            g_battle_msg[sizeof(g_battle_msg)-1] = '\0';
            g_ui_dirty = true;
            ui_battle();
            enter_world();
            return;
        }
        else
        {
            int edmg = rng_range(2, 5);
            g_player.hp -= edmg;
            if(g_player.hp < 0) g_player.hp = 0;

            snprintf(g_battle_msg, sizeof(g_battle_msg),
                     "Couldn't run! %s hits %d!", g_enemy.name, edmg);

            if(g_player.hp <= 0)
            {
                strncpy(g_battle_msg, "You fainted... Restored to full.", sizeof(g_battle_msg)-1);
                g_battle_msg[sizeof(g_battle_msg)-1] = '\0';
                g_player.hp = g_player.maxhp;
                g_ui_dirty = true;
                ui_battle();
                enter_world();
                return;
            }

            g_ui_dirty = true;
        }
    }
}

// ----------------------------- Init ------------------------------------

static void video_init(void)
{
    // Interrupts for VBlankIntrWait
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);
    irq_enable(II_VBLANK);

    // Mode 0, BG0 text, BG1 world, sprites on
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;

    // Text engine on BG0, screenblock 31, charblock 0
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));

    // World on BG1, screenblock 30, charblock 1
    REG_BG1CNT = BG_CBB(1) | BG_SBB(30) | BG_4BPP | BG_REG_32x32 | BG_PRIO(2);

    // Keep BG palette indices 0/1 friendly to text (black/white).
    pal_bg_mem[0] = RGB15(0, 0, 0);
    pal_bg_mem[1] = RGB15(31, 31, 31);

    // World colors (indices 2..4)
    pal_bg_mem[2] = RGB15(10, 24, 10);  // grass green
    pal_bg_mem[3] = RGB15(24, 20, 10);  // path tan
    pal_bg_mem[4] = RGB15(12, 12, 12);  // wall gray

    // Load BG1 tiles into charblock 1
    dma3_cpy(tile_mem[1], bg_tiles, sizeof(bg_tiles));

    // OBJ palette
    pal_obj_mem[0] = RGB15(0,0,0);      // transparent index (treated as 0)
    pal_obj_mem[1] = RGB15(31, 6, 6);   // player color (red-ish)

    // Load sprite tiles into OBJ tile memory
    dma3_cpy(tile_mem[4], obj_tiles, sizeof(obj_tiles));

    // Init OAM shadow
    oam_init(obj_buffer, 128);

    // Seed RNG with some changing register bits
    g_rng ^= ((u32)REG_VCOUNT << 16) ^ (u32)REG_TM0CNT_L;
}

// ----------------------------- Main ------------------------------------

int main(void)
{
    video_init();
    build_world();
    enter_world();

    while(1)
    {
        VBlankIntrWait();

        // Update game logic
        if(g_mode == MODE_WORLD)
            world_update();
        else
            battle_update();

        // Push OAM shadow to hardware
        oam_copy(oam_mem, obj_buffer, 1);
    }
}
