// main.c - tiny “Metroid-like” GBA prototype using tonc (single C file, no external art)
//
// Features:
// - side-scrolling tile world (64x32 tiles), camera follows player
// - solid tile collision + gravity + jumping
// - shoot bullets
// - simple patrolling enemies with HP
// - door tiles that switch between 2 “rooms” (regenerates the map)
//
// Build idea (one of many):
// - devkitARM + tonc set up
// - compile and link against tonc (or your tonclib template project)
// - drop this in place of your template main.c
//
// This is intentionally small and procedural (generates tiles/sprites in code).

#include <tonc.h>

// ----------------------------- Config -----------------------------

#define MAP_W       64
#define MAP_H       32
#define TILE_SZ     8

// Fixed-point 8.8
#define FP          8
#define F8(x)       ((x) << FP)
#define I8(x)       ((x) >> FP)

#define SCREEN_W    240
#define SCREEN_H    160

#define MAX_BULLETS 6
#define MAX_ENEMIES 6

// Tile ids (BG)
enum {
    T_EMPTY = 0,
    T_SOLID = 1,
    T_DOOR  = 2,
    T_SPIKE = 3
};

// OBJ tile ids (in OBJ VRAM)
enum {
    OT_PLAYER = 0,   // 16x16 => uses 4 tiles: 0..3
    OT_ENEMY  = 4,   // 16x16 => uses 4 tiles: 4..7
    OT_BULLET = 8    // 8x8   => uses 1 tile : 8
};

// Screenblock base used for 64x32 map (uses 2 screenblocks horizontally)
#define SBB_BASE 28

// ----------------------------- Low-level helpers -----------------------------

static inline u32 pack8(u8 a,u8 b,u8 c,u8 d,u8 e,u8 f,u8 g,u8 h)
{
    return (u32)(a) | ((u32)b<<4) | ((u32)c<<8) | ((u32)d<<12)
        | ((u32)e<<16) | ((u32)f<<20) | ((u32)g<<24) | ((u32)h<<28);
}

static inline int clampi(int v, int lo, int hi)
{
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

static inline void vsync(void)
{
    // Simple VBlank wait without IRQ setup
    while(REG_VCOUNT >= 160) {}
    while(REG_VCOUNT < 160) {}
}

// Basic input (avoids depending on tonc key_poll helpers)
static u16 g_keys, g_keys_prev;

static inline void keys_poll(void)
{
    g_keys_prev = g_keys;
    g_keys = (~REG_KEYS) & KEY_MASK;
}

static inline u16 key_down(u16 k) { return g_keys & k; }

// VRAM helpers (use raw pointers; works fine with tonc)
static inline u32* CHARBLOCK_U32(int cbb) { return (u32*)(0x06000000 + cbb*0x4000); }
static inline u16* SCREENBLOCK_U16(int sbb){ return (u16*)(0x06000000 + sbb*0x800); }
static inline u32* OBJ_TILES_U32(void)    { return (u32*)0x06010000; }

// ----------------------------- Game state -----------------------------

typedef struct Player
{
    int x, y;       // 8.8 fixed, top-left
    int vx, vy;     // 8.8 fixed
    int w, h;       // pixels
    int facing;     // -1 left, +1 right
    int on_ground;
    int hp;
    int invuln;
} Player;

typedef struct Bullet
{
    int active;
    int x, y;       // 8.8
    int vx;         // 8.8
} Bullet;

typedef struct Enemy
{
    int active;
    int x, y;       // 8.8
    int vx, vy;     // 8.8
    int w, h;
    int hp;
} Enemy;

static u16 g_world[MAP_H][MAP_W];
static int g_room_id=0;

static Player g_pl;
static Bullet g_bullets[MAX_BULLETS];
static Enemy  g_enemies[MAX_ENEMIES];

static int g_cam_x=0, g_cam_y=0; // pixels

static OBJ_ATTR g_oam_buf[128];

// ----------------------------- Tiles + map -----------------------------

static inline int in_bounds_tile(int tx, int ty)
{
    return (tx>=0 && tx<MAP_W && ty>=0 && ty<MAP_H);
}

static inline int tile_at_px(int px, int py)
{
    int tx = px / TILE_SZ;
    int ty = py / TILE_SZ;
    if(!in_bounds_tile(tx, ty))
        return T_SOLID; // treat outside as solid to keep player in-world
    return g_world[ty][tx];
}

static inline int is_solid_tile(int t)
{
    return (t == T_SOLID);
}

static inline int is_hazard_tile(int t)
{
    return (t == T_SPIKE);
}

static void upload_map_to_vram(void)
{
    // BG0 is 64x32 => 2 screenblocks horizontally: SBB_BASE and SBB_BASE+1
    u16 *sb0 = SCREENBLOCK_U16(SBB_BASE);
    u16 *sb1 = SCREENBLOCK_U16(SBB_BASE+1);

    for(int y=0; y<32; y++)
    {
        for(int x=0; x<64; x++)
        {
            u16 se = (u16)(g_world[y][x] & 0x03FF); // tile id
            if(x < 32) sb0[y*32 + x] = se;
            else       sb1[y*32 + (x-32)] = se;
        }
    }
}

static void clear_world(void)
{
    for(int y=0;y<MAP_H;y++)
        for(int x=0;x<MAP_W;x++)
            g_world[y][x]=T_EMPTY;
}

static void rect_fill_tiles(int x0,int y0,int x1,int y1, u16 tid)
{
    x0 = clampi(x0, 0, MAP_W-1);
    y0 = clampi(y0, 0, MAP_H-1);
    x1 = clampi(x1, 0, MAP_W-1);
    y1 = clampi(y1, 0, MAP_H-1);
    for(int y=y0;y<=y1;y++)
        for(int x=x0;x<=x1;x++)
            g_world[y][x]=tid;
}

static void put_tile(int tx,int ty, u16 tid)
{
    if(in_bounds_tile(tx,ty))
        g_world[ty][tx]=tid;
}

static void build_room(int id)
{
    g_room_id = id;
    clear_world();

    // Outer “bounds” feel: floor + some walls/platforms
    rect_fill_tiles(0, 30, 63, 31, T_SOLID);

    // Left wall chunk
    rect_fill_tiles(0, 0, 0, 31, T_SOLID);
    // Right wall chunk
    rect_fill_tiles(63, 0, 63, 31, T_SOLID);

    if(id == 0)
    {
        // Platforms
        rect_fill_tiles(6,  24, 18, 24, T_SOLID);
        rect_fill_tiles(22, 20, 30, 20, T_SOLID);
        rect_fill_tiles(36, 26, 50, 26, T_SOLID);

        // A small “shaft”
        rect_fill_tiles(10, 10, 12, 23, T_SOLID);

        // Spikes pit
        rect_fill_tiles(26, 29, 34, 29, T_SPIKE);

        // Door on far right
        put_tile(61, 29, T_DOOR);

        // Enemy spawns
        for(int i=0;i<MAX_ENEMIES;i++) g_enemies[i].active=0;
        g_enemies[0] = (Enemy){ .active=1, .x=F8(120), .y=F8(120), .vx=F8(1), .vy=0, .w=14, .h=14, .hp=3 };
        g_enemies[1] = (Enemy){ .active=1, .x=F8(360), .y=F8(160), .vx=-F8(1), .vy=0, .w=14, .h=14, .hp=3 };
    }
    else
    {
        // Room 1: more vertical
        rect_fill_tiles(6,  26, 20, 26, T_SOLID);
        rect_fill_tiles(24, 22, 40, 22, T_SOLID);
        rect_fill_tiles(10, 18, 18, 18, T_SOLID);
        rect_fill_tiles(44, 16, 58, 16, T_SOLID);

        // “Ceiling” partial
        rect_fill_tiles(8,  4, 56, 4,  T_SOLID);

        // Spikes
        rect_fill_tiles(14, 29, 22, 29, T_SPIKE);

        // Door on far left
        put_tile(2, 29, T_DOOR);

        // Enemy spawns
        for(int i=0;i<MAX_ENEMIES;i++) g_enemies[i].active=0;
        g_enemies[0] = (Enemy){ .active=1, .x=F8(200), .y=F8(80),  .vx=F8(1), .vy=0, .w=14, .h=14, .hp=4 };
        g_enemies[1] = (Enemy){ .active=1, .x=F8(440), .y=F8(120), .vx=-F8(1), .vy=0, .w=14, .h=14, .hp=4 };
    }

    upload_map_to_vram();
}

// ----------------------------- Art generation -----------------------------

static void init_palettes(void)
{
    // BG palette (index 0..15)
    pal_bg_mem[0] = RGB15(0,0,0);       // 0: black
    pal_bg_mem[1] = RGB15(3,3,6);       // 1: dark bluish
    pal_bg_mem[2] = RGB15(10,10,12);    // 2: border gray
    pal_bg_mem[3] = RGB15(18,18,20);    // 3: fill gray
    pal_bg_mem[4] = RGB15(20,12,0);     // 4: door orange
    pal_bg_mem[5] = RGB15(31,0,0);      // 5: spikes red

    // OBJ palette (index 0..15)
    pal_obj_mem[0] = RGB15(0,0,0);      // treated as transparent by sprites
    pal_obj_mem[1] = RGB15(31,20,8);    // player suit
    pal_obj_mem[2] = RGB15(6,4,2);      // outline
    pal_obj_mem[3] = RGB15(10,26,10);   // enemy green
    pal_obj_mem[4] = RGB15(31,31,31);   // highlight
    pal_obj_mem[5] = RGB15(31,0,0);     // bullet
}

static void write_bg_tile4(int tid, const u32 rows[8])
{
    // BG charblock 0
    u32 *cbb0 = CHARBLOCK_U32(0);
    u32 *dst = &cbb0[tid*8];
    for(int i=0;i<8;i++) dst[i]=rows[i];
}

static void init_bg_tiles(void)
{
    // Empty
    {
        u32 r[8]={0,0,0,0,0,0,0,0};
        write_bg_tile4(T_EMPTY, r);
    }

    // Solid: border color 2, fill color 3
    {
        u8 b=2, f=3;
        u32 r[8]={
            pack8(b,b,b,b,b,b,b,b),
            pack8(b,f,f,f,f,f,f,b),
            pack8(b,f,f,f,f,f,f,b),
            pack8(b,f,f,f,f,f,f,b),
            pack8(b,f,f,f,f,f,f,b),
            pack8(b,f,f,f,f,f,f,b),
            pack8(b,f,f,f,f,f,f,b),
            pack8(b,b,b,b,b,b,b,b),
        };
        write_bg_tile4(T_SOLID, r);
    }

    // Door: orange-ish
    {
        u8 o=4, b=2;
        u32 r[8]={
            pack8(0,b,b,b,b,b,b,0),
            pack8(b,o,o,o,o,o,o,b),
            pack8(b,o,b,b,b,b,o,b),
            pack8(b,o,b,o,o,b,o,b),
            pack8(b,o,b,o,o,b,o,b),
            pack8(b,o,b,b,b,b,o,b),
            pack8(b,o,o,o,o,o,o,b),
            pack8(0,b,b,b,b,b,b,0),
        };
        write_bg_tile4(T_DOOR, r);
    }

    // Spikes: red teeth
    {
        u8 rcol=5, base=2;
        u32 r[8]={
            pack8(0,0,0,0,0,0,0,0),
            pack8(0,0,0,0,0,0,0,0),
            pack8(0,0,0,0,0,0,0,0),
            pack8(0,0,0,0,0,0,0,0),
            pack8(0,0,0,0,0,0,0,0),
            pack8(0,0,rcol,0,0,rcol,0,0),
            pack8(0,rcol,base,rcol,rcol,base,rcol,0),
            pack8(base,base,base,base,base,base,base,base),
        };
        write_bg_tile4(T_SPIKE, r);
    }
}

static void write_obj_tile4(int tid, const u32 rows[8])
{
    u32 *obj = OBJ_TILES_U32();
    u32 *dst = &obj[tid*8];
    for(int i=0;i<8;i++) dst[i]=rows[i];
}

static void init_obj_tiles(void)
{
    // Player 16x16: 4 tiles (OT_PLAYER..OT_PLAYER+3)
    // Palette indices: 1 body, 2 outline, 4 highlight
    // Tile layout:
    // [0][1]
    // [2][3]
    //
    // Keep it simple: a chunky “samus-like” block.

    // top-left
    {
        u8 o=2, b=1, h=4;
        u32 r[8]={
            pack8(0,0,o,o,o,o,0,0),
            pack8(0,o,b,b,b,b,o,0),
            pack8(o,b,b,h,h,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(0,o,b,b,b,b,o,0),
            pack8(0,0,o,o,o,o,0,0),
        };
        write_obj_tile4(OT_PLAYER+0, r);
    }
    // top-right
    {
        u8 o=2, b=1, h=4;
        u32 r[8]={
            pack8(0,0,o,o,o,o,0,0),
            pack8(0,o,b,b,b,b,o,0),
            pack8(o,b,b,b,h,h,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(0,o,b,b,b,b,o,0),
            pack8(0,0,o,o,o,o,0,0),
        };
        write_obj_tile4(OT_PLAYER+1, r);
    }
    // bottom-left
    {
        u8 o=2, b=1;
        u32 r[8]={
            pack8(0,o,b,b,b,b,o,0),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(0,o,b,b,b,b,o,0),
            pack8(0,0,o,o,o,o,0,0),
            pack8(0,0,0,0,0,0,0,0),
        };
        write_obj_tile4(OT_PLAYER+2, r);
    }
    // bottom-right
    {
        u8 o=2, b=1;
        u32 r[8]={
            pack8(0,o,b,b,b,b,o,0),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(o,b,b,b,b,b,b,o),
            pack8(0,o,b,b,b,b,o,0),
            pack8(0,0,o,o,o,o,0,0),
            pack8(0,0,0,0,0,0,0,0),
        };
        write_obj_tile4(OT_PLAYER+3, r);
    }

    // Enemy 16x16 (green)
    {
        u8 o=2, g=3, h=4;
        u32 tl[8]={
            pack8(0,0,o,o,o,o,0,0),
            pack8(0,o,g,g,g,g,o,0),
            pack8(o,g,h,g,g,h,g,o),
            pack8(o,g,g,g,g,g,g,o),
            pack8(o,g,g,g,g,g,g,o),
            pack8(o,g,g,g,g,g,g,o),
            pack8(0,o,g,g,g,g,o,0),
            pack8(0,0,o,o,o,o,0,0),
        };
        u32 tr[8]={
            pack8(0,0,o,o,o,o,0,0),
            pack8(0,o,g,g,g,g,o,0),
            pack8(o,g,g,h,h,g,g,o),
            pack8(o,g,g,g,g,g,g,o),
            pack8(o,g,g,g,g,g,g,o),
            pack8(o,g,g,g,g,g,g,o),
            pack8(0,o,g,g,g,g,o,0),
            pack8(0,0,o,o,o,o,0,0),
        };
        // reuse for bottom too
        write_obj_tile4(OT_ENEMY+0, tl);
        write_obj_tile4(OT_ENEMY+1, tr);
        write_obj_tile4(OT_ENEMY+2, tl);
        write_obj_tile4(OT_ENEMY+3, tr);
    }

    // Bullet 8x8
    {
        u8 c=5;
        u32 r[8]={
            pack8(0,0,0,c,c,0,0,0),
            pack8(0,0,c,c,c,c,0,0),
            pack8(0,c,c,c,c,c,c,0),
            pack8(c,c,c,c,c,c,c,c),
            pack8(c,c,c,c,c,c,c,c),
            pack8(0,c,c,c,c,c,c,0),
            pack8(0,0,c,c,c,c,0,0),
            pack8(0,0,0,c,c,0,0,0),
        };
        write_obj_tile4(OT_BULLET, r);
    }
}

// ----------------------------- Physics & gameplay -----------------------------

static void player_respawn_left(void)
{
    g_pl.x = F8(24);
    g_pl.y = F8(80);
    g_pl.vx = g_pl.vy = 0;
    g_pl.facing = +1;
    g_pl.on_ground = 0;
}

static void player_respawn_right(void)
{
    g_pl.x = F8((MAP_W*TILE_SZ) - 48);
    g_pl.y = F8(80);
    g_pl.vx = g_pl.vy = 0;
    g_pl.facing = -1;
    g_pl.on_ground = 0;
}

static int aabb_overlap_px(int ax,int ay,int aw,int ah, int bx,int by,int bw,int bh)
{
    return (ax < bx+bw) && (ax+aw > bx) && (ay < by+bh) && (ay+ah > by);
}

static void spawn_bullet(void)
{
    for(int i=0;i<MAX_BULLETS;i++)
    {
        if(!g_bullets[i].active)
        {
            int px = I8(g_pl.x);
            int py = I8(g_pl.y);

            g_bullets[i].active = 1;
            g_bullets[i].x = F8(px + (g_pl.facing>0 ? g_pl.w : -2));
            g_bullets[i].y = F8(py + 6);
            g_bullets[i].vx = (g_pl.facing>0) ? F8(5) : -F8(5);
            return;
        }
    }
}

static void enemy_take_hit(Enemy *e)
{
    if(e->hp > 0) e->hp--;
    if(e->hp <= 0) e->active = 0;
}

static void player_take_damage(int knock_dir)
{
    if(g_pl.invuln > 0) return;
    g_pl.hp--;
    g_pl.invuln = 45;

    // knockback
    g_pl.vx = (knock_dir>0) ? F8(2) : -F8(2);
    g_pl.vy = -F8(3);

    if(g_pl.hp <= 0)
    {
        g_pl.hp = 6;
        // reset room + respawn
        build_room(g_room_id);
        if(g_room_id==0) player_respawn_left();
        else player_respawn_right();
    }
}

static void move_with_collision(int *x, int *y, int *vx, int *vy, int w, int h, int *on_ground)
{
    // Horizontal move
    int nx = *x + *vx;
    int px = I8(nx);
    int py = I8(*y);

    if(*vx > 0)
    {
        int right = px + w - 1;
        int top   = py;
        int bot   = py + h - 1;

        int t1 = tile_at_px(right, top);
        int t2 = tile_at_px(right, bot);

        if(is_solid_tile(t1) || is_solid_tile(t2))
        {
            int tx = (right / TILE_SZ);
            int new_right = tx*TILE_SZ - 1;
            px = new_right - (w - 1);
            nx = F8(px);
            *vx = 0;
        }
    }
    else if(*vx < 0)
    {
        int left = px;
        int top  = py;
        int bot  = py + h - 1;

        int t1 = tile_at_px(left, top);
        int t2 = tile_at_px(left, bot);

        if(is_solid_tile(t1) || is_solid_tile(t2))
        {
            int tx = (left / TILE_SZ);
            int new_left = (tx+1)*TILE_SZ;
            px = new_left;
            nx = F8(px);
            *vx = 0;
        }
    }
    *x = nx;

    // Vertical move
    int ny = *y + *vy;
    px = I8(*x);
    py = I8(ny);

    if(on_ground) *on_ground = 0;

    if(*vy > 0)
    {
        int left  = px;
        int right = px + w - 1;
        int bot   = py + h - 1;

        int t1 = tile_at_px(left, bot);
        int t2 = tile_at_px(right, bot);

        if(is_solid_tile(t1) || is_solid_tile(t2))
        {
            int ty = (bot / TILE_SZ);
            int new_bot = ty*TILE_SZ - 1;
            py = new_bot - (h - 1);
            ny = F8(py);
            *vy = 0;
            if(on_ground) *on_ground = 1;
        }
    }
    else if(*vy < 0)
    {
        int left  = px;
        int right = px + w - 1;
        int top   = py;

        int t1 = tile_at_px(left, top);
        int t2 = tile_at_px(right, top);

        if(is_solid_tile(t1) || is_solid_tile(t2))
        {
            int ty = (top / TILE_SZ);
            int new_top = (ty+1)*TILE_SZ;
            py = new_top;
            ny = F8(py);
            *vy = 0;
        }
    }
    *y = ny;
}

static void update_player(void)
{
    const int accel = F8(1);     // instant accel feel
    const int max_vx = F8(2);
    const int grav  = 0x0030;    // ~0.1875 px/f
    const int jumpv = F8(5);
    const int term  = F8(6);

    // Horizontal control
    if(key_down(KEY_LEFT))
    {
        g_pl.vx = -max_vx;
        g_pl.facing = -1;
    }
    else if(key_down(KEY_RIGHT))
    {
        g_pl.vx = +max_vx;
        g_pl.facing = +1;
    }
    else
    {
        // friction
        if(g_pl.vx > 0) { g_pl.vx -= accel; if(g_pl.vx < 0) g_pl.vx = 0; }
        if(g_pl.vx < 0) { g_pl.vx += accel; if(g_pl.vx > 0) g_pl.vx = 0; }
    }

    // Jump
    if(key_hit(KEY_A) && g_pl.on_ground)
    {
        g_pl.vy = -jumpv;
        g_pl.on_ground = 0;
    }

    // Shoot
    if(key_hit(KEY_B))
        spawn_bullet();

    // Gravity
    g_pl.vy += grav;
    if(g_pl.vy > term) g_pl.vy = term;

    // Move with collision
    move_with_collision(&g_pl.x, &g_pl.y, &g_pl.vx, &g_pl.vy, g_pl.w, g_pl.h, &g_pl.on_ground);

    // Hazards
    {
        int px = I8(g_pl.x), py = I8(g_pl.y);
        int midx = px + g_pl.w/2;
        int feet = py + g_pl.h - 1;
        int t = tile_at_px(midx, feet);
        if(is_hazard_tile(t))
            player_take_damage(-g_pl.facing);
    }

    // Door interaction: stand on door tile and press UP
    if(key_hit(KEY_UP))
    {
        int px = I8(g_pl.x), py = I8(g_pl.y);
        int midx = px + g_pl.w/2;
        int feet = py + g_pl.h - 1;
        int t = tile_at_px(midx, feet);

        if(t == T_DOOR)
        {
            if(g_room_id == 0)
            {
                build_room(1);
                player_respawn_left();
            }
            else
            {
                build_room(0);
                player_respawn_right();
            }
            // Clear bullets on room transition
            for(int i=0;i<MAX_BULLETS;i++) g_bullets[i].active=0;
        }
    }

    if(g_pl.invuln > 0) g_pl.invuln--;
}

static void update_bullets(void)
{
    for(int i=0;i<MAX_BULLETS;i++)
    {
        Bullet *b = &g_bullets[i];
        if(!b->active) continue;

        b->x += b->vx;

        int px = I8(b->x);
        int py = I8(b->y);

        // Hit wall?
        if(is_solid_tile(tile_at_px(px, py)) || is_solid_tile(tile_at_px(px+7, py+7)))
        {
            b->active=0;
            continue;
        }

        // Off world?
        if(px < 0 || px > MAP_W*TILE_SZ)
        {
            b->active=0;
            continue;
        }

        // Hit enemy?
        for(int e=0;e<MAX_ENEMIES;e++)
        {
            Enemy *en = &g_enemies[e];
            if(!en->active) continue;

            int ex = I8(en->x), ey = I8(en->y);
            if(aabb_overlap_px(px, py, 8, 8, ex, ey, en->w, en->h))
            {
                b->active=0;
                enemy_take_hit(en);
                break;
            }
        }
    }
}

static void update_enemies(void)
{
    const int grav = 0x0030;
    const int term = F8(6);

    for(int i=0;i<MAX_ENEMIES;i++)
    {
        Enemy *e = &g_enemies[i];
        if(!e->active) continue;

        // Gravity
        e->vy += grav;
        if(e->vy > term) e->vy = term;

        int on_ground=0;
        move_with_collision(&e->x, &e->y, &e->vx, &e->vy, e->w, e->h, &on_ground);

        // Turn around at ledges/walls (simple heuristic)
        int ex = I8(e->x), ey = I8(e->y);
        int front_x = ex + (e->vx>0 ? e->w : 0);
        int feet_y  = ey + e->h + 1;

        int ahead_wall = is_solid_tile(tile_at_px(front_x, ey)) || is_solid_tile(tile_at_px(front_x, ey+e->h-1));
        int ahead_floor = is_solid_tile(tile_at_px(front_x, feet_y));

        if(ahead_wall || (on_ground && !ahead_floor))
            e->vx = -e->vx;

        // Contact damage
        int px = I8(g_pl.x), py = I8(g_pl.y);
        if(aabb_overlap_px(px, py, g_pl.w, g_pl.h, ex, ey, e->w, e->h))
            player_take_damage((e->vx>=0) ? +1 : -1);
    }
}

static void update_camera(void)
{
    int px = I8(g_pl.x);
    int py = I8(g_pl.y);

    int target_x = px + g_pl.w/2 - SCREEN_W/2;
    int target_y = py + g_pl.h/2 - SCREEN_H/2;

    int max_x = MAP_W*TILE_SZ - SCREEN_W;
    int max_y = MAP_H*TILE_SZ - SCREEN_H;

    g_cam_x = clampi(target_x, 0, max_x);
    g_cam_y = clampi(target_y, 0, max_y);

    REG_BG0HOFS = (u16)g_cam_x;
    REG_BG0VOFS = (u16)g_cam_y;
}

// ----------------------------- Rendering -----------------------------

static void oam_clear(void)
{
    for(int i=0;i<128;i++)
        g_oam_buf[i].attr0 = ATTR0_HIDE;
}

static void draw_sprites(void)
{
    oam_clear();

    int o=0;

    // Player sprite
    {
        int sx = I8(g_pl.x) - g_cam_x;
        int sy = I8(g_pl.y) - g_cam_y;

        if(sx > -16 && sx < SCREEN_W && sy > -16 && sy < SCREEN_H)
        {
            // simple blink when invuln
            int visible = (g_pl.invuln==0) || ((g_pl.invuln & 4) == 0);
            if(visible)
            {
                u16 a0 = (sy & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP;
                u16 a1 = (sx & 0x1FF) | ATTR1_SIZE_16;
                // hflip when facing left
                if(g_pl.facing < 0) a1 |= ATTR1_HFLIP;
                u16 a2 = (OT_PLAYER & 0x3FF); // palbank 0

                g_oam_buf[o].attr0=a0;
                g_oam_buf[o].attr1=a1;
                g_oam_buf[o].attr2=a2;
                o++;
            }
        }
    }

    // Bullets
    for(int i=0;i<MAX_BULLETS && o<128;i++)
    {
        Bullet *b = &g_bullets[i];
        if(!b->active) continue;

        int sx = I8(b->x) - g_cam_x;
        int sy = I8(b->y) - g_cam_y;

        if(sx < -8 || sx > SCREEN_W || sy < -8 || sy > SCREEN_H)
            continue;

        g_oam_buf[o].attr0 = (sy & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP;
        g_oam_buf[o].attr1 = (sx & 0x1FF) | ATTR1_SIZE_8;
        g_oam_buf[o].attr2 = (OT_BULLET & 0x3FF);
        o++;
    }

    // Enemies
    for(int i=0;i<MAX_ENEMIES && o<128;i++)
    {
        Enemy *e = &g_enemies[i];
        if(!e->active) continue;

        int sx = I8(e->x) - g_cam_x;
        int sy = I8(e->y) - g_cam_y;

        if(sx < -16 || sx > SCREEN_W || sy < -16 || sy > SCREEN_H)
            continue;

        u16 a0 = (sy & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP;
        u16 a1 = (sx & 0x1FF) | ATTR1_SIZE_16;
        if(e->vx < 0) a1 |= ATTR1_HFLIP;
        u16 a2 = (OT_ENEMY & 0x3FF);

        g_oam_buf[o].attr0=a0;
        g_oam_buf[o].attr1=a1;
        g_oam_buf[o].attr2=a2;
        o++;
    }

    // Copy to real OAM
    // Each OBJ_ATTR is 4x u16; 128 entries => 512 u16
    memcpy16((void*)0x07000000, g_oam_buf, 128*4);
}

// ----------------------------- Init -----------------------------

static void init_video(void)
{
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG0: charblock 0, screenblock SBB_BASE, 4bpp, 64x32, priority 1
    REG_BG0CNT = BG_CBB(0) | BG_SBB(SBB_BASE) | BG_4BPP | BG_REG_64x32 | BG_PRIO(1);

    // Clear scroll
    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
}

static void init_game(void)
{
    // Player
    g_pl = (Player){
        .x=F8(24), .y=F8(80),
        .vx=0, .vy=0,
        .w=12, .h=16,
        .facing=+1,
        .on_ground=0,
        .hp=6,
        .invuln=0
    };

    for(int i=0;i<MAX_BULLETS;i++) g_bullets[i].active=0;
    for(int i=0;i<MAX_ENEMIES;i++) g_enemies[i].active=0;

    build_room(0);
}

int main(void)
{
    init_video();
    init_palettes();
    init_bg_tiles();
    init_obj_tiles();
    init_game();

    while(1)
    {
        keys_poll();

        update_player();
        update_bullets();
        update_enemies();
        update_camera();

        draw_sprites();

        vsync();
    }
}
