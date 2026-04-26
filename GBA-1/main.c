// main.c — Minimal 2D side-scroller starter for GBA using Tonc
// Build with devkitARM + Tonc (libtonc.a)
//
// Features:
// - Mode 0, 1 scrolling background (tilemap)
// - Simple metatile collision (8x8 or 16x16)
// - Player physics: gravity, jump, walk, ground detection
// - Camera follows player with clamping
// - Basic enemies (patrol) with simple sprite rendering
// - Single-file, easy to extend

#include <tonc.h>
#include <string.h>
#include <stdlib.h>

// ------------------------------
// Constants / tuning
// ------------------------------
#define SCREEN_W        240
#define SCREEN_H        160

// World / tilemap
#define TILE_W          8
#define TILE_H          8
#define MAP_W           64   // tiles wide
#define MAP_H           32   // tiles high
#define WORLD_W_PIX     (MAP_W * TILE_W)
#define WORLD_H_PIX     (MAP_H * TILE_H)

// Collision: use 16x16 metatiles for simplicity
#define MT_W            2    // tiles per metatile width
#define MT_H            2    // tiles per metatile height
#define C_MAP_W         (MAP_W / MT_W)
#define C_MAP_H         (MAP_H / MT_H)

// Physics
#define GRAV            0x0280  // fixed-point 8.8
#define MAX_FALL        0x3000
#define JUMP_FORCE      0x3800
#define MOVE_ACC        0x0120
#define MOVE_DEC        0x0100
#define MOVE_MAX        0x1800
#define FRICTION        0x0090

// Sprite/object limits
#define MAX_ENEMIES     8

// ------------------------------
// Types
// ------------------------------
typedef struct {
    int x, y;        // world position (fixed 8.8 optional, but ints fine here)
    int vx, vy;
    int w, h;
    int on_ground;
    int facing;      // 1 right, -1 left
    int anim_frame;
    int anim_timer;
} Player;

typedef struct {
    int x, y;
    int vx, vy;
    int w, h;
    int alive;
    int type;        // 0 = walker
    int dir;         // -1 left, 1 right
} Enemy;

// ------------------------------
// Assets (tiny placeholders — replace with your own)
// ------------------------------

// --- Background tileset (8x8, 16-color) ---
// Simple checker/ground tiles to get you running
static const u32 s_bgTiles[64*8/4] = {
    // Tile 0: empty
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    // Tile 1: ground block (solid)
    0x11111111, 0x12222221, 0x12333321, 0x12344321,
    0x12344321, 0x12333321, 0x12222221, 0x11111111,
    // Tile 2: decoration
    0x00000000, 0x00011000, 0x00122100, 0x01233210,
    0x01233210, 0x00122100, 0x00011000, 0x00000000,
};
static const u16 s_bgPal[16] = {
    RGB15(0,0,0),      // 0
    RGB15(12,8,4),     // 1 brown
    RGB15(24,16,8),    // 2
    RGB15(20,28,12),   // 3 green
    RGB15(16,20,28),   // 4 blue
    RGB15(31,31,31),   // 5 white
    RGB15(31,20,10),   // 6 orange
    0,0,0,0,0,0,0,0,0
};

// --- Tilemap (64x32) using tile indices 0/1/2 ---
// Make a simple level: ground line + some platforms
static u16 s_tilemap[MAP_W * MAP_H];

// --- Sprite tiles (16-color, 4bpp) — small player + enemy placeholders ---
static const u32 s_playerTiles[16*8/4] = {
    // 8x8 player dot (right-facing)
    0x00000000, 0x00033000, 0x00333300, 0x03333330,
    0x03333330, 0x00333300, 0x00033000, 0x00000000,
};
static const u32 s_enemyTiles[16*8/4] = {
    // 8x8 enemy
    0x00000000, 0x00066000, 0x00666600, 0x06666660,
    0x06666660, 0x00666600, 0x00066000, 0x00000000,
};
static const u16 s_spritePal[16] = {
    0,
    RGB15(31,20,20),   // player
    RGB15(20,31,20),   // enemy
    0,0,0,0,0,0,0,0,0,0,0,0
};

// ------------------------------
// Collision map (metatile-based)
// ------------------------------
// 0 = empty, 1 = solid
static u8 s_cmap[C_MAP_W * C_MAP_H];

// ------------------------------
// Game state
// ------------------------------
static Player s_player;
static Enemy  s_enemies[MAX_ENEMIES];
static int    s_camX = 0, s_camY = 0;

// ------------------------------
// Prototypes
// ------------------------------
static void init_level(void);
static void init_player(void);
static void init_enemies(void);
static void update_player(void);
static void update_enemies(void);
static void update_camera(void);
static void draw_player(void);
static void draw_enemies(void);
static inline int tile_is_solid(int tx, int ty);
static inline int metatile_is_solid(int mx, int my);
static void set_bg(void);
static void set_sprites(void);

// ------------------------------
// Helpers: collision queries
// ------------------------------
static inline int metatile_is_solid(int mx, int my) {
    if (mx < 0 || my < 0 || mx >= C_MAP_W || my >= C_MAP_H) return 1; // solid out of bounds
    return s_cmap[my * C_MAP_W + mx] != 0;
}

static inline int tile_is_solid(int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H) return 1;
    // derive metatile
    int mx = tx / MT_W;
    int my = ty / MT_H;
    return metatile_is_solid(mx, my);
}

// Axis-aligned rectangle vs world (metatile) collision test helper
static int rect_vs_solid(int rx, int ry, int rw, int rh) {
    // test corners + inner points for efficiency (coarse but fine for 8/16)
    // sample edges
    int left   = rx / TILE_W;
    int right  = (rx + rw - 1) / TILE_W;
    int top    = ry / TILE_H;
    int bottom = (ry + rh - 1) / TILE_H;
    for (int ty = top; ty <= bottom; ++ty) {
        for (int tx = left; tx <= right; ++tx) {
            if (tile_is_solid(tx, ty)) return 1;
        }
    }
    return 0;
}

// ------------------------------
// Init
// ------------------------------
static void init_level(void) {
    // Fill tilemap with empty
    memset(s_tilemap, 0, sizeof(s_tilemap));

    // Build a simple level: floor + a few platforms
    // Floor: bottom rows
    for (int tx = 0; tx < MAP_W; ++tx) {
        for (int ty = MAP_H - 4; ty < MAP_H; ++ty) {
            s_tilemap[ty * MAP_W + tx] = 1; // ground tile
        }
    }
    // Platforms
    for (int tx = 10; tx < 18; ++tx) {
        s_tilemap[(MAP_H - 10) * MAP_W + tx] = 1;
    }
    for (int tx = 25; tx < 33; ++tx) {
        s_tilemap[(MAP_H - 14) * MAP_W + tx] = 1;
    }
    for (int tx = 40; tx < 48; ++tx) {
        s_tilemap[(MAP_H - 8)  * MAP_W + tx] = 1;
    }

    // Decoration dots
    for (int i = 0; i < 20; ++i) {
        int tx = 5 + i*2;
        int ty = MAP_H - 6;
        if (tx < MAP_W) s_tilemap[ty * MAP_W + tx] = 2;
    }

    // Build collision map from tilemap (metatile solid if any tile != 0)
    memset(s_cmap, 0, sizeof(s_cmap));
    for (int my = 0; my < C_MAP_H; ++my) {
        for (int mx = 0; mx < C_MAP_W; ++mx) {
            // check 2x2 tiles
            int solid = 0;
            for (int ty = 0; ty < MT_H && !solid; ++ty) {
                for (int tx = 0; tx < MT_W && !solid; ++tx) {
                    int t = s_tilemap[(my*MT_H + ty) * MAP_W + (mx*MT_W + tx)];
                    if (t != 0) solid = 1;
                }
            }
            s_cmap[my * C_MAP_W + mx] = solid ? 1 : 0;
        }
    }
}

static void init_player(void) {
    s_player.x = 32 << 0;  // pixel units (integer coords for simplicity)
    s_player.y = 32 << 0;
    s_player.vx = 0; s_player.vy = 0;
    s_player.w = 12; s_player.h = 12;
    s_player.on_ground = 0;
    s_player.facing = 1;
    s_player.anim_frame = 0; s_player.anim_timer = 0;
}

static void init_enemies(void) {
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        s_enemies[i].alive = 0;
    }
    // Spawn a couple
    s_enemies[0] = (Enemy){ .x=160, .y=(MAP_H*8 - 40), .vx=0, .vy=0, .w=10, .h=10, .alive=1, .type=0, .dir=-1 };
    s_enemies[1] = (Enemy){ .x=300, .y=(MAP_H*8 - 40), .vx=0, .vy=0, .w=10, .h=10, .alive=1, .type=0, .dir=1  };
}

// ------------------------------
// Video setup
// ------------------------------
static void set_bg(void) {
    // Load tiles + pal
    memcpy(pal_bg_mem, s_bgPal, sizeof(s_bgPal));
    memcpy(&tile_mem[0][0], s_bgTiles, sizeof(s_bgTiles));

    // Setup BG0 as tilemap
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_8BPP | BG_REG_64x32;
    // Fill screenblock 30 with tilemap
    memcpy(&se_mem[30][0], s_tilemap, sizeof(s_tilemap));

    // Enable BG0 + sprites
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
}

static void set_sprites(void) {
    // Load sprite tiles + pal
    memcpy(pal_obj_mem, s_spritePal, sizeof(s_spritePal));
    memcpy(&tile_mem[4][0], s_playerTiles, sizeof(s_playerTiles));
    memcpy(&tile_mem[4][1], s_enemyTiles,  sizeof(s_enemyTiles)); // next tile

    // Clear OAM
    oam_init(obj_mem, 128);
}

// ------------------------------
// Update
// ------------------------------
static void update_player(void) {
    // Input
    key_poll();
    int left  = key_is_down(KEY_LEFT);
    int right = key_is_down(KEY_RIGHT);
    int jump  = key_is_down(KEY_A) || key_is_down(KEY_UP);

    // Horizontal
    if (left && !right) {
        s_player.vx -= MOVE_ACC;
        if (s_player.vx < -MOVE_MAX) s_player.vx = -MOVE_MAX;
        s_player.facing = -1;
    } else if (right && !left) {
        s_player.vx += MOVE_ACC;
        if (s_player.vx >  MOVE_MAX) s_player.vx =  MOVE_MAX;
        s_player.facing = 1;
    } else {
        // friction
        if (s_player.vx > 0) {
            s_player.vx -= FRICTION;
            if (s_player.vx < 0) s_player.vx = 0;
        }
        if (s_player.vx < 0) {
            s_player.vx += FRICTION;
            if (s_player.vx > 0) s_player.vx = 0;
        }
    }

    // Jump
    if (jump && s_player.on_ground) {
        s_player.vy = -JUMP_FORCE;
        s_player.on_ground = 0;
    }

    // Gravity
    s_player.vy += GRAV;
    if (s_player.vy > MAX_FALL) s_player.vy = MAX_FALL;

    // Integrate X with collision
    {
        int nx = s_player.x + s_player.vx / 256; // approx step
        // test horizontal
        if (!rect_vs_solid(nx, s_player.y, s_player.w, s_player.h)) {
            s_player.x = nx;
        } else {
            // wall slide: stop
            s_player.vx = 0;
        }
    }

    // Integrate Y with collision
    {
        int ny = s_player.y + s_player.vy / 256;
        if (!rect_vs_solid(s_player.x, ny, s_player.w, s_player.h)) {
            s_player.y = ny;
            s_player.on_ground = 0;
        } else {
            // landed or hit ceiling
            if (s_player.vy > 0) {
                // snap to ground
                // find floor
                int ty0 = (s_player.y + s_player.h - 1) / TILE_H;
                int ty1 = (ny + s_player.h - 1) / TILE_H;
                for (int ty = ty0; ty <= ty1; ++ty) {
                    if (tile_is_solid(s_player.x / TILE_W, ty) ||
                        tile_is_solid((s_player.x + s_player.w - 1) / TILE_W, ty)) {
                        s_player.y = ty * TILE_H - s_player.h;
                        break;
                    }
                }
                s_player.vy = 0;
                s_player.on_ground = 1;
            } else {
                // hit ceiling
                s_player.vy = 0;
            }
        }
    }

    // Clamp to world bounds (simple)
    if (s_player.x < 0) s_player.x = 0;
    if (s_player.y < 0) s_player.y = 0;
    if (s_player.x > WORLD_W_PIX - s_player.w) s_player.x = WORLD_W_PIX - s_player.w;
    if (s_player.y > WORLD_H_PIX - s_player.h) {
        s_player.y = WORLD_H_PIX - s_player.h;
        s_player.vy = 0;
        s_player.on_ground = 1;
    }

    // Animation (very simple bob)
    s_player.anim_timer++;
    if (s_player.anim_timer > 10) {
        s_player.anim_timer = 0;
        s_player.anim_frame = 1 - s_player.anim_frame;
    }
}

static void update_enemies(void) {
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Enemy *e = &s_enemies[i];
        if (!e->alive) continue;
        // simple patrol + gravity
        e->vy += GRAV;
        if (e->vy > MAX_FALL) e->vy = MAX_FALL;

        // horizontal patrol
        int nx = e->x + e->dir * 1; // slow
        if (!rect_vs_solid(nx, e->y, e->w, e->h)) {
            e->x = nx;
        } else {
            e->dir = -e->dir;
        }

        // vertical
        int ny = e->y + e->vy / 256;
        if (!rect_vs_solid(e->x, ny, e->w, e->h)) {
            e->y = ny;
        } else {
            if (e->vy > 0) {
                // snap
                int ty0 = (e->y + e->h - 1) / TILE_H;
                int ty1 = (ny + e->h - 1) / TILE_H;
                for (int ty = ty0; ty <= ty1; ++ty) {
                    if (tile_is_solid(e->x / TILE_W, ty) ||
                        tile_is_solid((e->x + e->w - 1) / TILE_W, ty)) {
                        e->y = ty * TILE_H - e->h;
                        break;
                    }
                }
                e->vy = 0;
            } else {
                e->vy = 0;
            }
        }

        // bounds
        if (e->x < 0) { e->x = 0; e->dir = 1; }
        if (e->x > WORLD_W_PIX - e->w) { e->x = WORLD_W_PIX - e->w; e->dir = -1; }
    }
}

static void update_camera(void) {
    // Follow player with margin
    int targetX = s_player.x + s_player.w/2 - SCREEN_W/2;
    int targetY = s_player.y + s_player.h/2 - SCREEN_H/2;

    // Smooth-ish follow (optional): lerp
    s_camX += (targetX - s_camX) / 8;
    s_camY += (targetY - s_camY) / 8;

    // Clamp
    if (s_camX < 0) s_camX = 0;
    if (s_camY < 0) s_camY = 0;
    if (s_camX > WORLD_W_PIX - SCREEN_W) s_camX = WORLD_W_PIX - SCREEN_W;
    if (s_camY > WORLD_H_PIX - SCREEN_H) s_camY = WORLD_H_PIX - SCREEN_H;

    // Set scroll
    REG_BG0HOFS = s_camX;
    REG_BG0VOFS = s_camY;
}

// ------------------------------
// Draw
// ------------------------------
static void draw_player(void) {
    // Find free OAM slot (simple: slot 0)
    OBJ_ATTR *attr = &obj_mem[0];
    int px = s_player.x - s_camX;
    int py = s_player.y - s_camY;
    // Hide if off-screen (cheap)
    if (px < -16 || py < -16 || px > SCREEN_W || py > SCREEN_H) {
        attr->attr0 = ATTR0_HIDE;
        return;
    }
    // attr0: y, shape
    attr->attr0 = ATTR0_Y(py) | ATTR0_SQUARE;
    // attr1: x, size, hflip
    u16 a1 = ATTR1_X(px) | ATTR1_SIZE_8x8;
    if (s_player.facing < 0) a1 |= ATTR1_HFLIP;
    attr->attr1 = a1;
    // attr2: tile, pal
    attr->attr2 = ATTR2_PALBANK(0) | ATTR2_ID(0);
}

static void draw_enemies(void) {
    int slot = 1;
    for (int i = 0; i < MAX_ENEMIES && slot < 128; ++i) {
        Enemy *e = &s_enemies[i];
        if (!e->alive) continue;
        OBJ_ATTR *attr = &obj_mem[slot++];
        int px = e->x - s_camX;
        int py = e->y - s_camY;
        if (px < -16 || py < -16 || px > SCREEN_W || py > SCREEN_H) {
            attr->attr0 = ATTR0_HIDE;
            continue;
        }
        attr->attr0 = ATTR0_Y(py) | ATTR0_SQUARE;
        u16 a1 = ATTR1_X(px) | ATTR1_SIZE_8x8;
        if (e->dir < 0) a1 |= ATTR1_HFLIP;
        attr->attr1 = a1;
        attr->attr2 = ATTR2_PALBANK(0) | ATTR2_ID(1);
    }
    // Hide remaining
    for (; slot < 128; ++slot) {
        obj_mem[slot].attr0 = ATTR0_HIDE;
    }
}

// ------------------------------
// Main
// ------------------------------
int main(void) {
    // Init systems
    irq_init(NULL);
    irq_enable(II_VBLANK);

    // Video + assets
    set_bg();
    set_sprites();

    // Game init
    init_level();
    init_player();
    init_enemies();

    // Game loop
    while (1) {
        VBlankIntrWait();

        // Update
        update_player();
        update_enemies();
        update_camera();

        // Draw
        draw_player();
        draw_enemies();

        // OAM copy
        oam_copy(oam_mem, obj_mem, 128);
    }

    return 0;
}
