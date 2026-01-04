// main.c - Single-file "Link's Awakening-ish" top-down prototype using tonc
// Controls:
//   D-Pad: move
//   A    : sword attack
//
// Build notes (typical devkitARM + tonc setup):
//   - Put this in a tonc project (tonc template) as main.c and build normally, OR
//   - Compile/link against libtonc + libgba with your existing makefile/toolchain.
//
// This is a small prototype, not a full game.

#include <tonc.h>

// ---------------------------- Config ----------------------------

#define MAP_W      32
#define MAP_H      32
#define TILE_SIZE   8
#define MAP_PX_W   (MAP_W*TILE_SIZE)
#define MAP_PX_H   (MAP_H*TILE_SIZE)

#define SCREEN_W 240
#define SCREEN_H 160

// Player collision box inside 16x16 sprite
#define PL_OX 2
#define PL_OY 2
#define PL_W  12
#define PL_H  12

typedef enum { DIR_UP=0, DIR_DOWN=1, DIR_LEFT=2, DIR_RIGHT=3 } Dir;

typedef struct
{
    int x, y;          // world position (top-left of 16x16 sprite)
    Dir dir;
    int hp;            // 0..3
    int invuln;        // frames of invulnerability after taking damage
    int attack;        // frames remaining of sword swing (0=not attacking)
} Player;

typedef struct
{
    int x, y;
    int vx;            // simple patrol velocity
    int alive;
} Enemy;

// ---------------------------- Globals ----------------------------

OBJ_ATTR obj_buffer[128];

static u16 g_map[MAP_W*MAP_H];   // tile ids (0=floor, 1=wall)

// Sprite tile indices in OBJ VRAM (1D mapping)
enum
{
    OBJT_LINK_BASE  = 0,   // uses 4 tiles (16x16)
    OBJT_SWORD_BASE = 4,   // uses 4 tiles (16x16)
    OBJT_ENEMY_BASE = 8,   // uses 4 tiles (16x16)
    OBJT_HEART_FULL = 12,  // 1 tile (8x8)
    OBJT_HEART_EMPTY= 13   // 1 tile (8x8)
};

// OAM object indices
enum
{
    OAM_LINK  = 0,
    OAM_SWORD = 1,
    OAM_H0    = 2,
    OAM_H1    = 3,
    OAM_H2    = 4,
    OAM_ENEMY = 5
};

// ---------------------------- Helpers ----------------------------

static inline int i_clamp(int v, int lo, int hi)
{
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

static inline int rects_overlap(int ax, int ay, int aw, int ah,
                                int bx, int by, int bw, int bh)
{
    return (ax < bx+bw) && (ax+aw > bx) && (ay < by+bh) && (ay+ah > by);
}

// 4bpp tile row with repeated nibble color (8 pixels => 8 nibbles => 32 bits)
static inline u32 row_fill_nibble(int c4)
{
    // c4 must be 0..15
    u32 n = (u32)(c4 & 15);
    return n * 0x11111111u;
}

static void make_tile_solid(TILE *t, int c4)
{
    for(int i=0; i<8; i++)
        t->data[i] = row_fill_nibble(c4);
}

static void make_tile_outline(TILE *t, int fill, int border)
{
    // Simple 8x8 outline. Pixels are nibbles packed in u32 rows.
    // We'll build each row nibble-by-nibble for clarity.
    for(int y=0; y<8; y++)
    {
        u32 row = 0;
        for(int x=0; x<8; x++)
        {
            int c = fill;
            if(x==0 || x==7 || y==0 || y==7) c = border;
            row |= (u32)(c & 15) << (x*4);
        }
        t->data[y] = row;
    }
}

static void make_tile_heart(TILE *t, int fill, int outline)
{
    // Tiny stylized 8x8 "heart-ish" blob.
    // 0 is transparent in OBJ; we still draw outline/fill.
    static const u8 px[8][8] =
    {
        {0,1,1,0,0,1,1,0},
        {1,1,1,1,1,1,1,1},
        {1,1,1,1,1,1,1,1},
        {1,1,1,1,1,1,1,1},
        {0,1,1,1,1,1,1,0},
        {0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0},
        {0,0,0,0,0,0,0,0},
    };

    for(int y=0; y<8; y++)
    {
        u32 row=0;
        for(int x=0; x<8; x++)
        {
            int c=0;
            if(px[y][x])
            {
                // outline if near an empty pixel
                int nearEmpty=0;
                for(int dy=-1; dy<=1; dy++)
                for(int dx=-1; dx<=1; dx++)
                {
                    int nx=x+dx, ny=y+dy;
                    if(nx<0||nx>=8||ny<0||ny>=8) continue;
                    if(!px[ny][nx]) nearEmpty=1;
                }
                c = nearEmpty ? outline : fill;
            }
            row |= (u32)(c & 15) << (x*4);
        }
        t->data[y]=row;
    }
}

static void init_palettes(void)
{
    // BG palette
    pal_bg_mem[0] = RGB15(0,0,0);          // background color
    pal_bg_mem[1] = RGB15(10,18,10);       // floor green
    pal_bg_mem[2] = RGB15(14,14,16);       // wall gray
    pal_bg_mem[3] = RGB15(6,10,6);         // darker accent

    // OBJ palette (index 0 must be transparent)
    pal_obj_mem[0] = RGB15(0,0,0);         // transparent (treated as 0)
    pal_obj_mem[1] = RGB15(0,20,0);        // Link green
    pal_obj_mem[2] = RGB15(31,25,18);      // light (face-ish)
    pal_obj_mem[3] = RGB15(0,10,0);        // dark green outline
    pal_obj_mem[4] = RGB15(31,31,0);       // sword yellow
    pal_obj_mem[5] = RGB15(31,0,0);        // enemy red
    pal_obj_mem[6] = RGB15(12,12,12);      // empty heart gray
    pal_obj_mem[7] = RGB15(31,0,12);       // heart fill (pink-red)
}

static void init_bg_tiles_and_map(void)
{
    // BG tiles go in charblock 0 (tile_mem[0])
    TILE *bgtiles = tile_mem[0];

    // Tile 0: floor
    make_tile_solid(&bgtiles[0], 1);

    // Tile 1: wall (outline-ish)
    make_tile_outline(&bgtiles[1], 2, 3);

    // Fill map with floor + border walls + some obstacles
    for(int y=0; y<MAP_H; y++)
    for(int x=0; x<MAP_W; x++)
    {
        int id = 0; // floor

        if(x==0 || y==0 || x==MAP_W-1 || y==MAP_H-1)
            id = 1; // border wall

        // a few interior blocks
        if((x==8 && y>4 && y<20) || (y==14 && x>10 && x<28) ||
           (x==20 && y>8 && y<28) || (y==6 && x>3 && x<12))
            id = 1;

        // small "room" on right side
        if(x>=24 && x<=29 && y>=20 && y<=27)
        {
            if(x==24 || x==29 || y==20 || y==27) id=1;
        }

        g_map[y*MAP_W + x] = (u16)id;
    }

    // Copy map into screenblock 31
    u16 *mapdst = se_mem[31];
    for(int i=0; i<MAP_W*MAP_H; i++)
        mapdst[i] = g_map[i]; // tile id only, no flips, palbank 0
}

static void init_obj_tiles(void)
{
    // OBJ tiles go in charblock 4 (tile_mem[4]) when using 1D mapping.
    TILE *ot = tile_mem[4];

    // Link: a green outlined square (4 tiles for 16x16)
    for(int i=0; i<4; i++)
        make_tile_outline(&ot[OBJT_LINK_BASE + i], 1, 3);

    // Sword: bright yellow outlined square (4 tiles)
    for(int i=0; i<4; i++)
        make_tile_outline(&ot[OBJT_SWORD_BASE + i], 4, 3);

    // Enemy: red outlined square (4 tiles)
    for(int i=0; i<4; i++)
        make_tile_outline(&ot[OBJT_ENEMY_BASE + i], 5, 3);

    // Heart tiles: 8x8 each (single tile)
    make_tile_heart(&ot[OBJT_HEART_FULL], 7, 5);   // fill pink-red, outline red
    make_tile_heart(&ot[OBJT_HEART_EMPTY], 6, 3);  // fill gray, outline dark
}

static inline int map_is_solid_tile(int tx, int ty)
{
    if(tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H)
        return 1;
    return (g_map[ty*MAP_W + tx] == 1);
}

static inline int map_is_solid_px(int px, int py)
{
    int tx = px >> 3;
    int ty = py >> 3;
    return map_is_solid_tile(tx, ty);
}

static int player_collides_at(Player *pl, int nx, int ny)
{
    int x0 = nx + PL_OX;
    int y0 = ny + PL_OY;
    int x1 = x0 + PL_W - 1;
    int y1 = y0 + PL_H - 1;

    // Check 4 corners against wall tiles
    if(map_is_solid_px(x0, y0)) return 1;
    if(map_is_solid_px(x1, y0)) return 1;
    if(map_is_solid_px(x0, y1)) return 1;
    if(map_is_solid_px(x1, y1)) return 1;
    return 0;
}

static void move_player(Player *pl, int dx, int dy)
{
    // Move X then Y for nicer wall sliding
    int nx = pl->x + dx;
    int ny = pl->y;

    if(!player_collides_at(pl, nx, ny))
        pl->x = nx;

    nx = pl->x;
    ny = pl->y + dy;

    if(!player_collides_at(pl, nx, ny))
        pl->y = ny;

    pl->x = i_clamp(pl->x, 0, MAP_PX_W-16);
    pl->y = i_clamp(pl->y, 0, MAP_PX_H-16);
}

static void set_obj16(int oam_id, int x, int y, int tile_id)
{
    OBJ_ATTR *oa = &obj_buffer[oam_id];
    obj_set_attr(oa,
        ATTR0_SQUARE,
        ATTR1_SIZE_16,
        ATTR2_PALBANK(0) | ATTR2_ID(tile_id));
    obj_set_pos(oa, x, y);
}

static void set_obj8(int oam_id, int x, int y, int tile_id)
{
    OBJ_ATTR *oa = &obj_buffer[oam_id];
    obj_set_attr(oa,
        ATTR0_SQUARE,
        ATTR1_SIZE_8,
        ATTR2_PALBANK(0) | ATTR2_ID(tile_id));
    obj_set_pos(oa, x, y);
}

// ---------------------------- Main ----------------------------

int main(void)
{
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    // Display setup: Mode 0, BG0, OBJ, 1D object tiles
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG0: charblock 0, screenblock 31, 4bpp, 32x32
    REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32 | BG_PRIO(1);

    init_palettes();
    init_bg_tiles_and_map();
    init_obj_tiles();

    // Init OAM shadow
    oam_init(obj_buffer, 128);

    Player pl = { .x=32, .y=32, .dir=DIR_DOWN, .hp=3, .invuln=0, .attack=0 };
    Enemy en  = { .x=160, .y=96, .vx=1, .alive=1 };

    int camx=0, camy=0;

    while(1)
    {
        key_poll();

        // --- Input + player update ---
        int dx=0, dy=0;
        if(key_is_down(KEY_LEFT))  { dx -= 1; pl.dir=DIR_LEFT; }
        if(key_is_down(KEY_RIGHT)) { dx += 1; pl.dir=DIR_RIGHT; }
        if(key_is_down(KEY_UP))    { dy -= 1; pl.dir=DIR_UP; }
        if(key_is_down(KEY_DOWN))  { dy += 1; pl.dir=DIR_DOWN; }

        if(pl.invuln > 0) pl.invuln--;

        // Attack trigger
        if(key_hit(KEY_A) && pl.attack == 0)
            pl.attack = 10;  // frames

        if(pl.attack > 0)
            pl.attack--;

        // Movement (you can still move while attacking, like classic top-down Zelda)
        if(dx || dy)
            move_player(&pl, dx, dy);

        // --- Enemy update ---
        if(en.alive)
        {
            int ex2 = en.x + en.vx;
            // simple wall bounce using a slightly smaller collision sample
            if(map_is_solid_px(ex2+2, en.y+8) || map_is_solid_px(ex2+13, en.y+8))
                en.vx = -en.vx;
            else
                en.x = ex2;

            // keep within map bounds
            en.x = i_clamp(en.x, 0, MAP_PX_W-16);
        }

        // --- Sword hitbox & enemy hit detection ---
        int sword_visible = (pl.attack > 0);
        int swx=0, swy=0;

        if(sword_visible)
        {
            // Place sword adjacent to player in facing direction
            swx = pl.x;
            swy = pl.y;

            switch(pl.dir)
            {
                case DIR_UP:    swy -= 12; break;
                case DIR_DOWN:  swy += 12; break;
                case DIR_LEFT:  swx -= 12; break;
                case DIR_RIGHT: swx += 12; break;
            }

            // If sword overlaps enemy, "defeat" it
            if(en.alive && rects_overlap(swx+2, swy+2, 12, 12, en.x+2, en.y+2, 12, 12))
                en.alive = 0;
        }

        // --- Player/enemy damage ---
        if(en.alive && pl.invuln==0)
        {
            if(rects_overlap(pl.x+PL_OX, pl.y+PL_OY, PL_W, PL_H, en.x+2, en.y+2, 12, 12))
            {
                pl.hp--;
                pl.invuln = 45; // ~0.75s at 60fps
                if(pl.hp <= 0)
                {
                    // simple respawn
                    pl.hp = 3;
                    pl.x = 32; pl.y = 32;
                }
            }
        }

        // --- Camera follow ---
        camx = i_clamp(pl.x + 8 - SCREEN_W/2, 0, MAP_PX_W - SCREEN_W);
        camy = i_clamp(pl.y + 8 - SCREEN_H/2, 0, MAP_PX_H - SCREEN_H);

        REG_BG0HOFS = (u16)camx;
        REG_BG0VOFS = (u16)camy;

        // --- Draw sprites (convert world -> screen by subtracting camera) ---
        int plsx = pl.x - camx;
        int plsy = pl.y - camy;

        // Link (blink when invulnerable)
        if(pl.invuln > 0 && ((pl.invuln/4) & 1))
            obj_hide(&obj_buffer[OAM_LINK]);
        else
            set_obj16(OAM_LINK, plsx, plsy, OBJT_LINK_BASE);

        // Sword
        if(sword_visible)
            set_obj16(OAM_SWORD, swx - camx, swy - camy, OBJT_SWORD_BASE);
        else
            obj_hide(&obj_buffer[OAM_SWORD]);

        // Enemy
        if(en.alive)
            set_obj16(OAM_ENEMY, en.x - camx, en.y - camy, OBJT_ENEMY_BASE);
        else
            obj_hide(&obj_buffer[OAM_ENEMY]);

        // Hearts UI (screen-space, not world-space)
        for(int i=0; i<3; i++)
        {
            int tile = (pl.hp > i) ? OBJT_HEART_FULL : OBJT_HEART_EMPTY;
            set_obj8(OAM_H0+i, 6 + i*10, 6, tile);
        }

        // --- Present ---
        vid_vsync();
        oam_copy(oam_mem, obj_buffer, 128);
    }
}
