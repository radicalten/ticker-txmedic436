// main.c - tiny “Kirby-ish” 2D side scroller starter for GBA using tonc
//
// Features:
// - Mode 0 tiled background (64x32 tile map = 512x256 px) with horizontal scrolling camera
// - One 16x16 sprite player with simple animation + facing flip
// - Platform collision (solid tiles), gravity, jump + midair jumps, and Kirby-like float (hold A to fall slower)
//
// Art/level are generated at runtime (no external assets), so this is truly a single C file.
//
// Build (example):
//   arm-none-eabi-gcc -mthumb -mthumb-interwork -O2 -Wall -Wextra -I<tonc_include> main.c -L<tonc_lib> -ltonc -o game.elf
// Then convert to .gba as usual in your toolchain.
//
// Notes:
// - Requires tonc (headers+lib) in your build environment.
// - This is a starter, not a full Kirby clone (no enemies, inhale, slopes, etc).

#include <tonc.h>
#include <string.h>

// ---------------------------- Config ----------------------------

#define SCREEN_W 240
#define SCREEN_H 160

#define MAP_W 64
#define MAP_H 32
#define TILE_SZ 8

// Player bounding box (pixels)
#define PL_W 12
#define PL_H 14

// Fixed-point (8.8)
#define FP_SHIFT 8
#define FP_ONE   (1 << FP_SHIFT)
#define TO_FP(x) ((x) << FP_SHIFT)
#define FP_TO_I(x) ((x) >> FP_SHIFT)

// Physics (in 8.8 fixed, so “pixels per frame” * 256)
#define PL_SPEED      (320)   // 1.25 px/f
#define PL_GRAV       (64)    // 0.25 px/f^2
#define PL_GRAV_FLOAT (24)    // slower gravity while holding A
#define PL_JUMP       (-1024) // -4.0 px/f
#define PL_MAX_FALL   (1024)  // 4.0 px/f

#define PL_MAX_JUMPS  5       // Kirby-ish multi-jump

// BG setup: BG0 charblock 0, screenblocks 28 & 29 (for 64x32 map)
#define BG_CBB_IDX 0
#define BG_SBB_LEFT 28
#define BG_SBB_RIGHT 29

// ---------------------------- Types ----------------------------

typedef struct Player
{
    int x, y;       // 8.8 fixed, top-left
    int vx, vy;     // 8.8 fixed
    int facing;     // -1 left, +1 right
    int onGround;   // bool
    int jumpCount;  // resets on landing
    int animTick;
    int animFrame;  // 0/1
} Player;

// ---------------------------- Globals ----------------------------

static OBJ_ATTR obj_buf[128];

static u16 level_map[MAP_W * MAP_H]; // tile IDs (0=air, 1=dirt, 2=grass)

// ---------------------------- Helpers ----------------------------

static inline int clamp_i(int v, int lo, int hi)
{
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

static inline void tile4_set_px(TILE *t, int x, int y, u8 c)
{
    // 4bpp: each row is one u32 with 8 nibbles (x=0..7)
    u32 mask = (u32)0xF << (x * 4);
    u32 val  = (u32)c   << (x * 4);
    t->data[y] = (t->data[y] & ~mask) | val;
}

static inline void tile4_fill(TILE *t, u8 c)
{
    // Repeat nibble across 32-bit word: cccccccc in nibbles
    u32 v = (u32)c * 0x11111111u;
    for(int y=0; y<8; y++)
        t->data[y] = v;
}

static inline u16 get_tile_id(int tx, int ty)
{
    // Treat out of bounds left/right/bottom as solid boundary.
    if(ty < 0) return 0;
    if(tx < 0 || tx >= MAP_W) return 1;
    if(ty >= MAP_H) return 1;
    return level_map[ty*MAP_W + tx];
}

static inline int tile_is_solid(u16 tid)
{
    // 0 = air/sky, 1 = dirt, 2 = grass
    return tid != 0;
}

static inline int solid_at_px(int px, int py)
{
    int tx = px >> 3;
    int ty = py >> 3;
    return tile_is_solid(get_tile_id(tx, ty));
}

// ---------------------------- BG/Level generation ----------------------------

static void build_bg_tiles(void)
{
    // BG palette (bank 0)
    // index 0: sky
    // index 1: sky highlight
    // index 2: dirt
    // index 3: dirt dark speck
    // index 4: grass
    // index 5: grass dark
    pal_bg_mem[0] = RGB15(17, 24, 31);
    pal_bg_mem[1] = RGB15(22, 28, 31);
    pal_bg_mem[2] = RGB15(16, 10,  6);
    pal_bg_mem[3] = RGB15(12,  7,  4);
    pal_bg_mem[4] = RGB15( 6, 20,  6);
    pal_bg_mem[5] = RGB15( 4, 14,  4);

    // Tile 0: sky (fill with color 0)
    tile4_fill(&tile_mem[BG_CBB_IDX][0], 0);

    // Tile 1: dirt
    tile4_fill(&tile_mem[BG_CBB_IDX][1], 2);
    // Add some specks
    for(int y=0; y<8; y++)
    for(int x=0; x<8; x++)
    {
        if(((x*17 + y*31) & 15) == 0)
            tile4_set_px(&tile_mem[BG_CBB_IDX][1], x, y, 3);
    }

    // Tile 2: grass-top (top 2 rows grass, rest dirt)
    tile4_fill(&tile_mem[BG_CBB_IDX][2], 2);
    for(int y=0; y<2; y++)
        for(int x=0; x<8; x++)
            tile4_set_px(&tile_mem[BG_CBB_IDX][2], x, y, (x & 1) ? 5 : 4);
}

static int ground_height_for_col(int tx)
{
    // Return the first solid tile row (0..MAP_H), where MAP_H means “no ground (pit)”.
    int h = 24;

    // A couple bumps/platform-y shapes
    if(tx >= 10 && tx <= 14) h = 20;
    if(tx >= 22 && tx <= 26) h = 18;
    if(tx >= 32 && tx <= 35) h = 22;

    // A pit
    if(tx >= 42 && tx <= 45) h = MAP_H;

    // Another pit
    if(tx >= 54 && tx <= 56) h = MAP_H;

    return h;
}

static void build_level_map(void)
{
    // Fill with sky
    for(int i=0; i<MAP_W*MAP_H; i++)
        level_map[i] = 0;

    // Ground columns
    for(int x=0; x<MAP_W; x++)
    {
        int h = ground_height_for_col(x);
        if(h >= MAP_H) continue;

        // Surface grass at h-1, dirt from h..MAP_H-1
        int surface = h-1;
        if(surface >= 0 && surface < MAP_H)
            level_map[surface*MAP_W + x] = 2;

        for(int y=h; y<MAP_H; y++)
            level_map[y*MAP_W + x] = 1;
    }

    // A floating platform (grass tiles)
    for(int x=18; x<=25; x++)
        level_map[14*MAP_W + x] = 2;

    // A small block “stair”
    for(int y=19; y<=23; y++)
        level_map[y*MAP_W + 28] = 1;
}

static void upload_map_to_vram(void)
{
    // 64x32 map uses two 32x32 screenblocks side-by-side.
    // Left half -> SBB_LEFT, right half -> SBB_RIGHT.
    for(int y=0; y<32; y++)
    for(int x=0; x<64; x++)
    {
        u16 tid = level_map[y*MAP_W + x];
        u16 se  = tid; // palbank 0, no flips

        if(x < 32)
            se_mem[BG_SBB_LEFT][y*32 + x] = se;
        else
            se_mem[BG_SBB_RIGHT][y*32 + (x-32)] = se;
    }
}

// ---------------------------- OBJ (player sprite) generation ----------------------------

static void obj_set_px_16x16(int baseTile, int x, int y, u8 c)
{
    // baseTile points to first of 4 tiles (2x2) for a 16x16 sprite in 1D mapping.
    // Tile offsets: (0,0)=0, (1,0)=1, (0,1)=2, (1,1)=3
    int tileX = x >> 3;
    int tileY = y >> 3;
    int tileOff = tileY*2 + tileX;

    int lx = x & 7;
    int ly = y & 7;

    TILE *t = &tile_mem[4][baseTile + tileOff];
    tile4_set_px(t, lx, ly, c);
}

static void clear_obj_16x16(int baseTile)
{
    for(int i=0; i<4; i++)
        tile4_fill(&tile_mem[4][baseTile + i], 0);
}

static void build_player_frame(int baseTile, int blink, int step)
{
    // OBJ palette (bank 0)
    // 0 transparent
    // 1 body pink
    // 2 outline darker
    // 3 black
    // 4 foot red
    // 5 white highlight
    pal_obj_mem[0] = RGB15(0,0,0); // ignored as transparent
    pal_obj_mem[1] = RGB15(31, 18, 24);
    pal_obj_mem[2] = RGB15(22,  9, 14);
    pal_obj_mem[3] = RGB15( 2,  2,  2);
    pal_obj_mem[4] = RGB15(31,  6,  8);
    pal_obj_mem[5] = RGB15(31, 31, 31);

    clear_obj_16x16(baseTile);

    // Simple circle body with outline + face + feet
    const float cx = 7.5f, cy = 7.0f;
    const float r  = 7.2f;

    for(int y=0; y<16; y++)
    for(int x=0; x<16; x++)
    {
        float dx = (float)x - cx;
        float dy = (float)y - cy;
        float d2 = dx*dx + dy*dy;

        float r2 = r*r;
        if(d2 > r2 + 1.0f)
            continue;

        // outline band
        if(d2 > r2 - 8.0f)
            obj_set_px_16x16(baseTile, x, y, 2);
        else
            obj_set_px_16x16(baseTile, x, y, 1);
    }

    // Eyes
    if(!blink)
    {
        // left eye
        obj_set_px_16x16(baseTile, 6, 6, 3);
        obj_set_px_16x16(baseTile, 6, 7, 3);
        obj_set_px_16x16(baseTile, 7, 6, 5); // highlight

        // right eye
        obj_set_px_16x16(baseTile, 10, 6, 3);
        obj_set_px_16x16(baseTile, 10, 7, 3);
        obj_set_px_16x16(baseTile, 11, 6, 5);
    }
    else
    {
        // blink line
        for(int x=5; x<=11; x++)
            obj_set_px_16x16(baseTile, x, 7, 3);
    }

    // Feet (two ovals). Step anim shifts them a bit.
    int footShift = step ? 1 : 0;

    for(int y=12; y<16; y++)
    for(int x=0; x<16; x++)
    {
        // left foot center
        int lx = 5 + footShift;
        int ly = 13;
        int dx1 = x - lx, dy1 = y - ly;

        // right foot center
        int rx = 11 - footShift;
        int ry = 13;
        int dx2 = x - rx, dy2 = y - ry;

        if(dx1*dx1 + dy1*dy1 <= 6 || dx2*dx2 + dy2*dy2 <= 6)
            obj_set_px_16x16(baseTile, x, y, 4);
    }
}

static void build_player_sprite_tiles(void)
{
    // Two 16x16 frames:
    // frame 0 uses tiles 0..3
    // frame 1 uses tiles 4..7
    build_player_frame(0, 0, 0);
    build_player_frame(4, 0, 1);
}

// ---------------------------- Collision + movement ----------------------------

static void player_move_and_collide(Player *p)
{
    // Apply gravity (Kirby-ish float if holding A)
    int holdingA = key_is_down(KEY_A);

    if(!p->onGround && holdingA && p->vy > 0)
        p->vy += PL_GRAV_FLOAT;
    else
        p->vy += PL_GRAV;

    if(p->vy > PL_MAX_FALL) p->vy = PL_MAX_FALL;

    // --- Horizontal move ---
    int newX = p->x + p->vx;

    // clamp within world bounds
    int worldW = MAP_W * TILE_SZ;
    int minX = 0;
    int maxX = worldW - PL_W;
    newX = clamp_i(newX, TO_FP(minX), TO_FP(maxX));

    if(p->vx != 0)
    {
        int top    = FP_TO_I(p->y) + 1;
        int bottom = FP_TO_I(p->y) + PL_H - 2;

        if(p->vx > 0)
        {
            int right = FP_TO_I(newX) + PL_W - 1;
            int tx = right >> 3;

            if(tile_is_solid(get_tile_id(tx, top>>3)) || tile_is_solid(get_tile_id(tx, bottom>>3)))
            {
                newX = TO_FP((tx<<3) - PL_W);
            }
        }
        else
        {
            int left = FP_TO_I(newX);
            int tx = left >> 3;

            if(tile_is_solid(get_tile_id(tx, top>>3)) || tile_is_solid(get_tile_id(tx, bottom>>3)))
            {
                newX = TO_FP(((tx+1)<<3));
            }
        }
    }
    p->x = newX;

    // --- Vertical move ---
    int newY = p->y + p->vy;
    p->onGround = 0;

    // (Optional) clamp upper bound a bit to keep things sane
    if(newY < TO_FP(-32)) newY = TO_FP(-32);

    if(p->vy > 0)
    {
        // Falling: check bottom corners
        int bottom = FP_TO_I(newY) + PL_H - 1;
        int leftPx = FP_TO_I(p->x) + 2;
        int rightPx= FP_TO_I(p->x) + PL_W - 3;

        if(solid_at_px(leftPx, bottom) || solid_at_px(rightPx, bottom))
        {
            int ty = bottom >> 3;
            newY = TO_FP((ty<<3) - PL_H);
            p->vy = 0;
            p->onGround = 1;
            p->jumpCount = 0;
        }
    }
    else if(p->vy < 0)
    {
        // Rising: check top corners
        int top = FP_TO_I(newY);
        int leftPx = FP_TO_I(p->x) + 2;
        int rightPx= FP_TO_I(p->x) + PL_W - 3;

        if(solid_at_px(leftPx, top) || solid_at_px(rightPx, top))
        {
            int ty = top >> 3;
            newY = TO_FP(((ty+1)<<3));
            p->vy = 0;
        }
    }

    // Clamp bottom (in case of pits, we still have a “solid boundary” in get_tile_id,
    // but this keeps the value reasonable.)
    int worldH = MAP_H * TILE_SZ;
    int maxY = worldH - PL_H;
    if(newY > TO_FP(maxY))
    {
        newY = TO_FP(maxY);
        p->vy = 0;
        p->onGround = 1;
        p->jumpCount = 0;
    }

    p->y = newY;
}

// ---------------------------- Main ----------------------------

int main(void)
{
    // Display: mode 0, BG0, sprites, 1D OBJ tile mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG0: 4bpp tiles, charblock 0, screenblock 28, size 64x32
    REG_BG0CNT = BG_CBB(BG_CBB_IDX)
               | BG_SBB(BG_SBB_LEFT)
               | BG_4BPP
               | BG_REG_64x32
               | BG_PRIO(3);

    // Build assets + level
    build_bg_tiles();
    build_level_map();
    upload_map_to_vram();

    build_player_sprite_tiles();

    // OBJ init
    oam_init(obj_buf, 128);

    Player pl = {0};
    pl.x = TO_FP(16);
    pl.y = TO_FP(32);
    pl.facing = +1;

    int camx = 0, camy = 0;

    while(1)
    {
        key_poll();

        // Horizontal input
        int move = 0;
        if(key_is_down(KEY_LEFT))  move -= 1;
        if(key_is_down(KEY_RIGHT)) move += 1;

        if(move != 0)
        {
            pl.vx = move * PL_SPEED;
            pl.facing = (move < 0) ? -1 : +1;
        }
        else
        {
            pl.vx = 0;
        }

        // Jump (multi-jump)
        if(key_hit(KEY_A))
        {
            if(pl.onGround || pl.jumpCount < PL_MAX_JUMPS)
            {
                pl.vy = PL_JUMP;
                if(!pl.onGround)
                    pl.jumpCount++;
                pl.onGround = 0;
            }
        }

        player_move_and_collide(&pl);

        // Camera follow (x only; y fixed)
        int worldW = MAP_W * TILE_SZ;
        int maxCamX = worldW - SCREEN_W;
        int targetCamX = (FP_TO_I(pl.x) + PL_W/2) - SCREEN_W/2;
        camx = clamp_i(targetCamX, 0, maxCamX);

        REG_BG0HOFS = (u16)camx;
        REG_BG0VOFS = (u16)camy;

        // Animation
        pl.animTick++;
        if(pl.onGround && pl.vx != 0)
        {
            if((pl.animTick & 15) == 0)
                pl.animFrame ^= 1;
        }
        else
        {
            pl.animFrame = 0;
        }

        int baseTile = (pl.animFrame == 0) ? 0 : 4;

        // Sprite position relative to camera
        int sx = FP_TO_I(pl.x) - camx;
        int sy = FP_TO_I(pl.y) - camy;

        // Set sprite
        u16 a0 = ATTR0_SQUARE | ATTR0_4BPP;
        u16 a1 = ATTR1_SIZE_16;
        if(pl.facing < 0) a1 |= ATTR1_HFLIP;

        obj_set_attr(&obj_buf[0],
            a0,
            a1,
            ATTR2_PALBANK(0) | baseTile);

        obj_set_pos(&obj_buf[0], sx, sy);

        // Upload OAM during VBlank
        vid_vsync();
        oam_copy(oam_mem, obj_buf, 1);
    }
}
