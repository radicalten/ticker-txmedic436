// main.c - Single-file “mini Gradius” horizontal shmup for GBA using tonc.
// - Mode 0 tiled starfield background with horizontal scrolling
// - Sprites: player, bullets, enemies
// - Simple collisions, score + lives UI using a tiny tile font
//
// Drop this into an existing tonc/devkitARM project as main.c and build normally.
//
// Controls:
//  D-Pad: move
//  A: fire (auto-fire)
//  START: reset

#include <tonc.h>
#include <string.h>

#define SCREEN_W 240
#define SCREEN_H 160

// ---------------------------- Game tuning ----------------------------
#define PLAYER_W 16
#define PLAYER_H 16
#define PLAYER_SPEED 2

#define BULLET_W 8
#define BULLET_H 8
#define BULLET_SPEED 4
#define FIRE_COOLDOWN_FRAMES 6

#define ENEMY_W 16
#define ENEMY_H 16
#define ENEMY_MIN_SPD 1
#define ENEMY_MAX_SPD 2
#define ENEMY_SPAWN_FRAMES 40

#define MAX_BULLETS 16
#define MAX_ENEMIES  12

#define INVULN_FRAMES 90

// BG screenblocks
#define SBB_STAR 31
#define SBB_UI   30

// BG charblocks
#define CBB_STAR 0
#define CBB_UI   1

// Sprite tiles (OBJ VRAM tile indices, 4bpp, 1D mapping)
#define TID_PLAYER 0            // 4 tiles: 0..3
#define TID_BULLET 4            // 1 tile : 4
#define TID_ENEMY  5            // 4 tiles: 5..8

// UI font tiles (BG charblock 1)
#define FT_BLANK 0
#define FT_0     1              // digits: 1..10 represent 0..9
#define FT_9     (FT_0+9)

#define FT_S     11
#define FT_C     12
#define FT_O     13
#define FT_R     14
#define FT_E     15
#define FT_L     16
#define FT_I     17
#define FT_V     18

static OBJ_ATTR obj_buffer[128];

// ---------------------------- Small helpers ----------------------------
static inline int clampi(int v, int lo, int hi)
{
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

static inline bool aabb(int ax,int ay,int aw,int ah, int bx,int by,int bw,int bh)
{
    return (ax < bx+bw) && (ax+aw > bx) && (ay < by+bh) && (ay+ah > by);
}

// Set a pixel in a 4bpp 8x8 tile stored as 32 bytes
static inline void tile4_setpix(u8 tile[32], int x, int y, u8 c)
{
    // Each row is 4 bytes = 8 pixels (2 pixels per byte)
    int idx = y*4 + (x>>1);
    u8 v = tile[idx];
    if(x & 1) tile[idx] = (v & 0x0F) | (c<<4);
    else      tile[idx] = (v & 0xF0) | (c & 0x0F);
}

static inline void tile4_clear(u8 tile[32]) { memset(tile, 0, 32); }

// Draw a 5x7 glyph into an 8x8 tile using bit rows (bit4..bit0)
static void make_glyph_5x7(u8 out[32], const u8 rows[7], u8 color)
{
    tile4_clear(out);
    const int ox = 1;  // center-ish in 8x8
    const int oy = 0;
    for(int y=0; y<7; y++)
    {
        u8 r = rows[y];
        for(int x=0; x<5; x++)
        {
            if(r & (1<<(4-x)))
                tile4_setpix(out, ox+x, oy+y, color);
        }
    }
}

// ---------------------------- Entities ----------------------------
typedef struct Bullet
{
    int x, y;
    int vx;
    bool active;
} Bullet;

typedef struct Enemy
{
    int x, y;
    int vx;
    bool active;
} Enemy;

typedef struct Player
{
    int x, y;
    int fire_cd;
    int invuln;
    int lives;
} Player;

static Player g_plr;
static Bullet g_bullets[MAX_BULLETS];
static Enemy  g_enemies[MAX_ENEMIES];

static int g_frame = 0;
static int g_score = 0;

// Starfield scroll state
static int g_bg0_scroll_x = 0;

// ---------------------------- Tile/VRAM setup ----------------------------
static void build_obj_tiles(void)
{
    // OBJ palette (index 0 is transparent for sprites)
    pal_obj_mem[0]  = RGB15(0,0,0);         // transparent
    pal_obj_mem[1]  = RGB15(8,20,31);       // player body (blue)
    pal_obj_mem[2]  = RGB15(31,28,10);      // cockpit (yellow)
    pal_obj_mem[3]  = RGB15(31,6,6);        // enemy border (red)
    pal_obj_mem[4]  = RGB15(18,8,26);       // enemy fill (purple)
    pal_obj_mem[15] = RGB15(0,0,0);         // “black” details

    // Build sprite tiles into OBJ VRAM (tile_mem[4])
    // We'll generate into temporary byte tiles, then copy into TILE structs.
    u8 t[32];

    // --- Player 16x16: 4 tiles (0..3) ---
    // A simple right-pointing wedge. Color 1 body, color 2 cockpit.
    u8 player16[16*16];
    memset(player16, 0, sizeof(player16));

    for(int y=0; y<16; y++)
    {
        int maxX = 8 + (7 - ( (y<=7) ? (7-y) : (y-8) )); // peaks at y≈7/8
        if(maxX > 15) maxX = 15;
        for(int x=0; x<=maxX; x++)
            player16[y*16+x] = 1;
    }
    // cockpit blob
    for(int y=7; y<=8; y++)
        for(int x=4; x<=6; x++)
            player16[y*16+x] = 2;

    // Convert 16x16 into 4 sequential 8x8 tiles (1D mapping order)
    // tile 0: (0,0), tile 1: (8,0), tile 2: (0,8), tile 3: (8,8)
    for(int ty=0; ty<2; ty++)
    for(int tx=0; tx<2; tx++)
    {
        tile4_clear(t);
        for(int y=0; y<8; y++)
        for(int x=0; x<8; x++)
        {
            u8 c = player16[(ty*8+y)*16 + (tx*8+x)];
            if(c) tile4_setpix(t, x, y, c);
        }
        memcpy(&tile_mem[4][TID_PLAYER + ty*2 + tx], t, 32);
    }

    // --- Bullet 8x8: tile 4 ---
    tile4_clear(t);
    for(int x=1; x<=6; x++)
    {
        tile4_setpix(t, x, 3, 2);
        tile4_setpix(t, x, 4, 2);
    }
    memcpy(&tile_mem[4][TID_BULLET], t, 32);

    // --- Enemy 16x16: tiles 5..8 ---
    u8 enemy16[16*16];
    memset(enemy16, 0, sizeof(enemy16));
    for(int y=0; y<16; y++)
    for(int x=0; x<16; x++)
    {
        bool border = (x==0 || x==15 || y==0 || y==15);
        enemy16[y*16+x] = border ? 3 : 4;
    }
    // eyes
    enemy16[6*16+5]  = 15;
    enemy16[6*16+10] = 15;
    enemy16[7*16+5]  = 15;
    enemy16[7*16+10] = 15;

    for(int ty=0; ty<2; ty++)
    for(int tx=0; tx<2; tx++)
    {
        tile4_clear(t);
        for(int y=0; y<8; y++)
        for(int x=0; x<8; x++)
        {
            u8 c = enemy16[(ty*8+y)*16 + (tx*8+x)];
            if(c) tile4_setpix(t, x, y, c);
        }
        memcpy(&tile_mem[4][TID_ENEMY + ty*2 + tx], t, 32);
    }
}

static void build_bg_starfield(void)
{
    // BG palette (shared by BG0/BG1)
    pal_bg_mem[0] = RGB15(0,0,0);           // background
    pal_bg_mem[1] = RGB15(31,31,31);        // stars
    pal_bg_mem[2] = RGB15(0,31,0);          // UI text (green)

    // BG0 tiles (charblock 0): tile 0 blank, tile 1 star
    {
        u8 t0[32], t1[32];
        tile4_clear(t0);
        tile4_clear(t1);
        // Put a single white pixel (palette index 1)
        tile4_setpix(t1, 4, 4, 1);

        memcpy(&tile_mem[CBB_STAR][0], t0, 32);
        memcpy(&tile_mem[CBB_STAR][1], t1, 32);
    }

    // Fill star map (screenblock 31) with sparse stars
    SCR_ENTRY *map = se_mem[SBB_STAR];
    for(int y=0; y<32; y++)
    for(int x=0; x<32; x++)
    {
        // ~1/12 stars
        int star = ((qran() & 0xFF) < 22) ? 1 : 0;
        map[y*32 + x] = SE_BUILD(star, 0, 0, 0);
    }
}

static void build_ui_font(void)
{
    // Build a tiny 5x7 font in BG charblock 1.
    // Tile 0 blank; tiles 1..10 digits; tiles 11.. letters.
    u8 t[32];

    // Helper: write TILE by index
    auto void put_tile(int tid, const u8 rows[7])
    {
        make_glyph_5x7(t, rows, 2); // green (palette index 2)
        memcpy(&tile_mem[CBB_UI][tid], t, 32);
    }

    // Blank
    tile4_clear(t);
    memcpy(&tile_mem[CBB_UI][FT_BLANK], t, 32);

    // Digits 0-9 (5x7)
    static const u8 DIG[10][7] =
    {
        { 0x1E,0x11,0x13,0x15,0x19,0x11,0x1E }, // 0
        { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E }, // 1
        { 0x1E,0x01,0x01,0x1E,0x10,0x10,0x1F }, // 2
        { 0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E }, // 3
        { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 }, // 4
        { 0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E }, // 5
        { 0x0E,0x10,0x10,0x1E,0x11,0x11,0x1E }, // 6
        { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 }, // 7
        { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E }, // 8
        { 0x1E,0x11,0x11,0x1F,0x01,0x01,0x0E }, // 9
    };
    for(int d=0; d<10; d++)
        put_tile(FT_0 + d, DIG[d]);

    // Letters needed: S C O R E L I V
    static const u8 GL_S[7] = { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E };
    static const u8 GL_C[7] = { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E };
    static const u8 GL_O[7] = { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E };
    static const u8 GL_R[7] = { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 };
    static const u8 GL_E[7] = { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F };
    static const u8 GL_L[7] = { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F };
    static const u8 GL_I[7] = { 0x1F,0x04,0x04,0x04,0x04,0x04,0x1F };
    static const u8 GL_V[7] = { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 };

    put_tile(FT_S, GL_S);
    put_tile(FT_C, GL_C);
    put_tile(FT_O, GL_O);
    put_tile(FT_R, GL_R);
    put_tile(FT_E, GL_E);
    put_tile(FT_L, GL_L);
    put_tile(FT_I, GL_I);
    put_tile(FT_V, GL_V);

    // Clear UI map
    SCR_ENTRY *ui = se_mem[SBB_UI];
    for(int i=0; i<32*32; i++)
        ui[i] = SE_BUILD(FT_BLANK, 0, 0);

    // Write static labels: "SCORE" and "LIVES"
    ui[0*32 + 0] = SE_BUILD(FT_S, 0, 0);
    ui[0*32 + 1] = SE_BUILD(FT_C, 0, 0);
    ui[0*32 + 2] = SE_BUILD(FT_O, 0, 0);
    ui[0*32 + 3] = SE_BUILD(FT_R, 0, 0);
    ui[0*32 + 4] = SE_BUILD(FT_E, 0, 0);

    ui[0*32 + 20] = SE_BUILD(FT_L, 0, 0);
    ui[0*32 + 21] = SE_BUILD(FT_I, 0, 0);
    ui[0*32 + 22] = SE_BUILD(FT_V, 0, 0);
    ui[0*32 + 23] = SE_BUILD(FT_E, 0, 0);
    ui[0*32 + 24] = SE_BUILD(FT_S, 0, 0);
}

static void ui_draw_number(int x, int y, int value, int digits)
{
    // Draw right-aligned number with fixed digits at (x..x+digits-1, y)
    SCR_ENTRY *ui = se_mem[SBB_UI];
    if(value < 0) value = 0;

    for(int i=digits-1; i>=0; i--)
    {
        int d = value % 10;
        value /= 10;
        ui[y*32 + (x+i)] = SE_BUILD(FT_0 + d, 0, 0);
    }
}

// ---------------------------- Spawning ----------------------------
static void reset_game(void)
{
    g_plr.x = 16;
    g_plr.y = (SCREEN_H - PLAYER_H)/2;
    g_plr.fire_cd = 0;
    g_plr.invuln = 0;
    g_plr.lives = 3;

    for(int i=0; i<MAX_BULLETS; i++) g_bullets[i].active = false;
    for(int i=0; i<MAX_ENEMIES;  i++) g_enemies[i].active = false;

    g_score = 0;
    g_frame = 0;
}

static void spawn_bullet(int x, int y)
{
    for(int i=0; i<MAX_BULLETS; i++)
    {
        if(!g_bullets[i].active)
        {
            g_bullets[i].active = true;
            g_bullets[i].x = x;
            g_bullets[i].y = y;
            g_bullets[i].vx = BULLET_SPEED;
            return;
        }
    }
}

static void spawn_enemy(void)
{
    for(int i=0; i<MAX_ENEMIES; i++)
    {
        if(!g_enemies[i].active)
        {
            g_enemies[i].active = true;
            g_enemies[i].x = SCREEN_W; // spawn just off the right edge
            g_enemies[i].y = (qran() % (SCREEN_H-ENEMY_H));
            g_enemies[i].vx = -(ENEMY_MIN_SPD + (qran() % (ENEMY_MAX_SPD-ENEMY_MIN_SPD+1)));
            return;
        }
    }
}

// ---------------------------- Background scrolling ----------------------------
static void starfield_update(void)
{
    // Scroll right in bg space => stars move left on screen.
    g_bg0_scroll_x++;
    REG_BG0HOFS = g_bg0_scroll_x;

    // Every 8 px, update the new rightmost column entering view.
    if((g_bg0_scroll_x & 7) == 0)
    {
        int tileScroll = (g_bg0_scroll_x >> 3);         // tiles scrolled
        int newCol = (tileScroll + 30) & 31;            // rightmost visible tile column
        SCR_ENTRY *map = se_mem[SBB_STAR];

        for(int row=0; row<32; row++)
        {
            int star = ((qran() & 0xFF) < 22) ? 1 : 0;
            map[row*32 + newCol] = SE_BUILD(star, 0, 0);
        }
    }
}

// ---------------------------- Update + render ----------------------------
static void game_update(void)
{
    key_poll();

    if(key_hit(KEY_START))
        reset_game();

    // Player movement
    int dx = 0, dy = 0;
    if(key_is_down(KEY_LEFT))  dx -= PLAYER_SPEED;
    if(key_is_down(KEY_RIGHT)) dx += PLAYER_SPEED;
    if(key_is_down(KEY_UP))    dy -= PLAYER_SPEED;
    if(key_is_down(KEY_DOWN))  dy += PLAYER_SPEED;

    g_plr.x = clampi(g_plr.x + dx, 0, SCREEN_W - PLAYER_W);
    g_plr.y = clampi(g_plr.y + dy, 0, SCREEN_H - PLAYER_H);

    // Fire
    if(g_plr.fire_cd > 0) g_plr.fire_cd--;
    if(key_is_down(KEY_A) && g_plr.fire_cd == 0)
    {
        spawn_bullet(g_plr.x + 14, g_plr.y + 7);
        g_plr.fire_cd = FIRE_COOLDOWN_FRAMES;
    }

    // Spawn enemies
    if((g_frame % ENEMY_SPAWN_FRAMES) == 0)
        spawn_enemy();

    // Update bullets
    for(int i=0; i<MAX_BULLETS; i++)
    {
        if(!g_bullets[i].active) continue;
        g_bullets[i].x += g_bullets[i].vx;
        if(g_bullets[i].x > SCREEN_W)
            g_bullets[i].active = false;
    }

    // Update enemies
    for(int i=0; i<MAX_ENEMIES; i++)
    {
        if(!g_enemies[i].active) continue;
        g_enemies[i].x += g_enemies[i].vx;
        if(g_enemies[i].x < -ENEMY_W)
            g_enemies[i].active = false;
    }

    // Collisions: bullets vs enemies
    for(int e=0; e<MAX_ENEMIES; e++)
    {
        if(!g_enemies[e].active) continue;
        for(int b=0; b<MAX_BULLETS; b++)
        {
            if(!g_bullets[b].active) continue;
            if(aabb(g_bullets[b].x, g_bullets[b].y, BULLET_W, BULLET_H,
                    g_enemies[e].x,  g_enemies[e].y,  ENEMY_W,  ENEMY_H))
            {
                g_bullets[b].active = false;
                g_enemies[e].active = false;
                g_score += 10;
                break;
            }
        }
    }

    // Collisions: player vs enemies
    if(g_plr.invuln > 0) g_plr.invuln--;
    else
    {
        for(int e=0; e<MAX_ENEMIES; e++)
        {
            if(!g_enemies[e].active) continue;
            if(aabb(g_plr.x, g_plr.y, PLAYER_W, PLAYER_H,
                    g_enemies[e].x, g_enemies[e].y, ENEMY_W, ENEMY_H))
            {
                g_enemies[e].active = false;
                g_plr.lives--;
                g_plr.invuln = INVULN_FRAMES;

                if(g_plr.lives <= 0)
                    reset_game();
                break;
            }
        }
    }

    // Background scroll
    starfield_update();

    // UI update
    ui_draw_number(6, 0, g_score, 6);     // after "SCORE" at x=0..4, keep a space at x=5
    ui_draw_number(26,0, g_plr.lives, 2); // after "LIVES" at x=20..24, keep a space at x=25

    g_frame++;
}

static void game_render(void)
{
    // Hide everything by default
    for(int i=0; i<128; i++)
        obj_hide(&obj_buffer[i]);

    int o = 0;

    // Player sprite (blink during invuln)
    bool draw_player = true;
    if(g_plr.invuln > 0)
        draw_player = (((g_plr.invuln >> 2) & 1) == 0);

    if(draw_player)
    {
        OBJ_ATTR *spr = &obj_buffer[o++];
        obj_set_attr(spr,
            ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_SIZE_16,
            ATTR2_BUILD(TID_PLAYER, 0, 0));
        obj_set_pos(spr, g_plr.x, g_plr.y);
    }

    // Bullets
    for(int i=0; i<MAX_BULLETS && o < 128; i++)
    {
        if(!g_bullets[i].active) continue;

        OBJ_ATTR *spr = &obj_buffer[o++];
        obj_set_attr(spr,
            ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_SIZE_8,
            ATTR2_BUILD(TID_BULLET, 0, 0));
        obj_set_pos(spr, g_bullets[i].x, g_bullets[i].y);
    }

    // Enemies
    for(int i=0; i<MAX_ENEMIES && o < 128; i++)
    {
        if(!g_enemies[i].active) continue;

        OBJ_ATTR *spr = &obj_buffer[o++];
        obj_set_attr(spr,
            ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_SIZE_16,
            ATTR2_BUILD(TID_ENEMY, 0, 0));
        obj_set_pos(spr, g_enemies[i].x, g_enemies[i].y);
    }

    // Copy to OAM during VBlank (do the copy after vid_vsync() in main loop)
}

// ---------------------------- Main ----------------------------
int main(void)
{
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    // Video mode: Mode 0 (tiled), BG0 starfield, BG1 UI, sprites enabled, 1D OBJ mapping.
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG0: starfield (lower priority)
    REG_BG0CNT = BG_CBB(CBB_STAR) | BG_SBB(SBB_STAR) | BG_4BPP | BG_REG_32x32 | BG_PRIO(1);

    // BG1: UI text (higher priority)
    REG_BG1CNT = BG_CBB(CBB_UI) | BG_SBB(SBB_UI) | BG_4BPP | BG_REG_32x32 | BG_PRIO(0);

    REG_BG0HOFS = 0; REG_BG0VOFS = 0;
    REG_BG1HOFS = 0; REG_BG1VOFS = 0;

    // Deterministic RNG seed; you can change this if you want.
    sqran(0xC0FFEE);

    oam_init(obj_buffer, 128);

    build_obj_tiles();
    build_bg_starfield();
    build_ui_font();

    reset_game();

    while(1)
    {
        game_update();
        game_render();

        vid_vsync();
        oam_copy(oam_mem, obj_buffer, 128);
    }
}
