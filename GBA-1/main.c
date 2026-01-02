/*
 * GBA Kirby-like Side-Scroller
 * Uses tonc-style register definitions
 * 
 * Compile with devkitARM:
 * arm-none-eabi-gcc -mthumb -mthumb-interwork -specs=gba.specs -O2 game.c -o game.elf
 * arm-none-eabi-objcopy -O binary game.elf game.gba
 * gbafix game.gba
 */

// ============================================================================
// TYPE DEFINITIONS
// ============================================================================

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;

typedef volatile u16 vu16;
typedef volatile u32 vu32;

// Fixed point (12.4 format for sub-pixel precision)
typedef s32 FIXED;
#define FIX_SHIFT   8
#define FIX_ONE     (1 << FIX_SHIFT)
#define INT2FIX(n)  ((n) << FIX_SHIFT)
#define FIX2INT(f)  ((f) >> FIX_SHIFT)

// ============================================================================
// GBA HARDWARE DEFINITIONS (TONC STYLE)
// ============================================================================

#define MEM_IO      0x04000000
#define MEM_PAL     0x05000000
#define MEM_VRAM    0x06000000
#define MEM_OAM     0x07000000

#define PAL_BG_MEM      ((u16*)MEM_PAL)
#define PAL_OBJ_MEM     ((u16*)(MEM_PAL + 0x200))
#define TILE_MEM        ((u32*)MEM_VRAM)
#define TILE_OBJ_MEM    ((u32*)(MEM_VRAM + 0x10000))
#define OAM_MEM         ((u32*)MEM_OAM)

// Display registers
#define REG_DISPCNT     (*(vu16*)(MEM_IO + 0x0000))
#define REG_DISPSTAT    (*(vu16*)(MEM_IO + 0x0004))
#define REG_VCOUNT      (*(vu16*)(MEM_IO + 0x0006))
#define REG_BG0CNT      (*(vu16*)(MEM_IO + 0x0008))
#define REG_BG0HOFS     (*(vu16*)(MEM_IO + 0x0010))
#define REG_BG0VOFS     (*(vu16*)(MEM_IO + 0x0012))
#define REG_KEYINPUT    (*(vu16*)(MEM_IO + 0x0130))

// Display control bits
#define DCNT_MODE0      0x0000
#define DCNT_OBJ        0x1000
#define DCNT_OBJ_1D     0x0040
#define DCNT_BG0        0x0100

// BG control bits
#define BG_CBB(n)       ((n) << 2)
#define BG_SBB(n)       ((n) << 8)
#define BG_4BPP         0x0000
#define BG_SIZE_64x32   0x4000

// Key bits
#define KEY_A           0x0001
#define KEY_B           0x0002
#define KEY_SELECT      0x0004
#define KEY_START       0x0008
#define KEY_RIGHT       0x0010
#define KEY_LEFT        0x0020
#define KEY_UP          0x0040
#define KEY_DOWN        0x0080

// OAM attribute bits
#define ATTR0_Y(n)          ((n) & 0xFF)
#define ATTR0_SQUARE        0x0000
#define ATTR0_4BPP          0x0000
#define ATTR0_HIDE          0x0200

#define ATTR1_X(n)          ((n) & 0x1FF)
#define ATTR1_SIZE_16       0x4000
#define ATTR1_HFLIP         0x1000

#define ATTR2_ID(n)         ((n) & 0x3FF)
#define ATTR2_PRIO(n)       ((n) << 10)
#define ATTR2_PALBANK(n)    ((n) << 12)

// Color macro
#define RGB15(r,g,b)    ((r) | ((g) << 5) | ((b) << 10))

// ============================================================================
// OAM OBJECT STRUCTURE
// ============================================================================

typedef struct {
    u16 attr0;
    u16 attr1;
    u16 attr2;
    u16 pad;
} __attribute__((aligned(4))) OBJ_ATTR;

#define oam_mem     ((OBJ_ATTR*)MEM_OAM)

// ============================================================================
// SPRITE GRAPHICS DATA (4BPP 16x16)
// Kirby-like character with multiple animation frames
// Each 8x8 tile = 32 bytes, 16x16 sprite = 4 tiles
// ============================================================================

static const u32 kirby_gfx[] = {
    // === FRAME 0: STANDING (tiles 0-3) ===
    // Tile 0 (top-left)
    0x00000000, 0x00000000, 0x00011100, 0x00111110,
    0x01111111, 0x01122111, 0x11122211, 0x11112211,
    // Tile 1 (top-right)
    0x00000000, 0x00000000, 0x01110000, 0x11111000,
    0x11111110, 0x11122110, 0x11222110, 0x11221110,
    // Tile 2 (bottom-left)
    0x11111111, 0x11111111, 0x01111111, 0x00111111,
    0x00011441, 0x00001441, 0x00000110, 0x00000000,
    // Tile 3 (bottom-right)
    0x11111110, 0x11111110, 0x11111100, 0x11111000,
    0x14410000, 0x14410000, 0x01100000, 0x00000000,
    
    // === FRAME 1: WALKING 1 (tiles 4-7) ===
    // Tile 4 (top-left)
    0x00000000, 0x00000000, 0x00011100, 0x00111110,
    0x01111111, 0x01122111, 0x11122211, 0x11112211,
    // Tile 5 (top-right)
    0x00000000, 0x00000000, 0x01110000, 0x11111000,
    0x11111110, 0x11122110, 0x11222110, 0x11221110,
    // Tile 6 (bottom-left) - different feet
    0x11111111, 0x11111111, 0x01111111, 0x00111110,
    0x00014410, 0x00014400, 0x00011000, 0x00000000,
    // Tile 7 (bottom-right)
    0x11111110, 0x11111110, 0x11111100, 0x01111000,
    0x01441000, 0x00441000, 0x00011000, 0x00000000,
    
    // === FRAME 2: WALKING 2 (tiles 8-11) ===
    // Tile 8 (top-left)
    0x00000000, 0x00000000, 0x00011100, 0x00111110,
    0x01111111, 0x01122111, 0x11122211, 0x11112211,
    // Tile 9 (top-right)
    0x00000000, 0x00000000, 0x01110000, 0x11111000,
    0x11111110, 0x11122110, 0x11222110, 0x11221110,
    // Tile 10 (bottom-left)
    0x11111111, 0x11111111, 0x01111111, 0x00111110,
    0x00144100, 0x00044100, 0x00001100, 0x00000000,
    // Tile 11 (bottom-right)
    0x11111110, 0x11111110, 0x11111100, 0x01111000,
    0x00144100, 0x00144000, 0x00110000, 0x00000000,
    
    // === FRAME 3: JUMPING (tiles 12-15) ===
    // Tile 12 (top-left)
    0x00000000, 0x00000000, 0x00011100, 0x00111110,
    0x01111111, 0x01122111, 0x11122211, 0x11112211,
    // Tile 13 (top-right)
    0x00000000, 0x00000000, 0x01110000, 0x11111000,
    0x11111110, 0x11122110, 0x11222110, 0x11221110,
    // Tile 14 (bottom-left) - arms out
    0x11111111, 0x01111111, 0x01111111, 0x01111111,
    0x00111441, 0x00011110, 0x00000000, 0x00000000,
    // Tile 15 (bottom-right)
    0x11111110, 0x11111100, 0x11111110, 0x11111110,
    0x14411100, 0x01111000, 0x00000000, 0x00000000,
    
    // === FRAME 4: FLOATING/PUFFED (tiles 16-19) ===
    // Tile 16 (top-left) - rounder/bigger
    0x00000000, 0x00001110, 0x00111111, 0x01111111,
    0x11111111, 0x11122211, 0x11122211, 0x11112211,
    // Tile 17 (top-right)
    0x00000000, 0x01110000, 0x11111100, 0x11111110,
    0x11111111, 0x11222111, 0x11222111, 0x11221111,
    // Tile 18 (bottom-left)
    0x11111111, 0x11111111, 0x11111111, 0x01111111,
    0x00111441, 0x00011110, 0x00000110, 0x00000000,
    // Tile 19 (bottom-right)
    0x11111111, 0x11111111, 0x11111111, 0x11111110,
    0x14411100, 0x01111000, 0x01100000, 0x00000000,
};

// ============================================================================
// BACKGROUND TILE GRAPHICS (4BPP 8x8)
// ============================================================================

static const u32 bg_tiles[] = {
    // Tile 0: Empty (sky)
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    
    // Tile 1: Grass top
    0x33223322, 0x22332233, 0x22222222, 0x22222222,
    0x22222222, 0x22222222, 0x22222222, 0x22222222,
    
    // Tile 2: Dirt
    0x22222222, 0x22222222, 0x22222222, 0x22222222,
    0x22222222, 0x22222222, 0x22222222, 0x22222222,
    
    // Tile 3: Platform
    0x55555555, 0x54444445, 0x44444444, 0x44444444,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    
    // Tile 4: Cloud piece
    0x00000000, 0x00066000, 0x00666600, 0x06666660,
    0x66666666, 0x66666666, 0x06666660, 0x00000000,
    
    // Tile 5: Star/Collectible
    0x00010000, 0x00111000, 0x01111100, 0x11111110,
    0x01111100, 0x00111000, 0x00010000, 0x00000000,
};

// ============================================================================
// LEVEL MAP DATA (64x32 tiles, but we only use 64x20)
// ============================================================================

#define MAP_W   64
#define MAP_H   20

static const u8 level_map[MAP_H * MAP_W] = {
    // Sky rows (0-6)
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Platforms at row 7
    0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Platforms at row 10
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Platforms at row 13
    0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Ground rows (17-19)
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
};

// ============================================================================
// GAME STRUCTURES
// ============================================================================

typedef struct {
    FIXED x, y;         // Position (fixed-point)
    FIXED vx, vy;       // Velocity
    s32 on_ground;
    s32 facing_right;
    s32 is_floating;
    s32 float_count;
    s32 anim_frame;
    s32 anim_timer;
} Player;

typedef struct {
    s32 x, y;           // Pixel position
} Camera;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static Player player;
static Camera camera;
static u16 keys_curr = 0;
static u16 keys_prev = 0;
static OBJ_ATTR obj_buffer[128];

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

#define GRAVITY         16
#define JUMP_POWER      (INT2FIX(4))
#define FLOAT_POWER     (INT2FIX(2))
#define MAX_FALL        (INT2FIX(5))
#define WALK_SPEED      (INT2FIX(1) + 128)
#define FLOAT_GRAVITY   6
#define MAX_FLOATS      6

#define PLAYER_W        12
#define PLAYER_H        14

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static inline void vsync(void) {
    while(REG_VCOUNT >= 160);
    while(REG_VCOUNT < 160);
}

static void memcpy32(void* dst, const void* src, u32 words) {
    u32* d = (u32*)dst;
    const u32* s = (const u32*)src;
    while(words--) {
        *d++ = *s++;
    }
}

static void memset16(void* dst, u16 val, u32 count) {
    u16* d = (u16*)dst;
    while(count--) {
        *d++ = val;
    }
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

static void input_poll(void) {
    keys_prev = keys_curr;
    keys_curr = ~REG_KEYINPUT & 0x03FF;
}

static inline u16 key_down(u16 key) {
    return keys_curr & key;
}

static inline u16 key_pressed(u16 key) {
    return (keys_curr & ~keys_prev) & key;
}

// ============================================================================
// COLLISION DETECTION
// ============================================================================

static u8 get_tile(s32 px, s32 py) {
    s32 tx = px >> 3;   // Convert pixel to tile
    s32 ty = py >> 3;
    
    if(tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H) {
        return 0;
    }
    return level_map[ty * MAP_W + tx];
}

static s32 is_solid(u8 tile) {
    return (tile >= 1 && tile <= 3);
}

static s32 check_collision_point(s32 px, s32 py) {
    return is_solid(get_tile(px, py));
}

static s32 check_collision_box(s32 x, s32 y, s32 w, s32 h) {
    // Check corners and edges
    return check_collision_point(x + 2, y + 2) ||
           check_collision_point(x + w - 3, y + 2) ||
           check_collision_point(x + 2, y + h - 3) ||
           check_collision_point(x + w - 3, y + h - 3) ||
           check_collision_point(x + w/2, y + 2) ||
           check_collision_point(x + w/2, y + h - 3);
}

// ============================================================================
// PLAYER LOGIC
// ============================================================================

static void player_init(void) {
    player.x = INT2FIX(40);
    player.y = INT2FIX(120);
    player.vx = 0;
    player.vy = 0;
    player.on_ground = 0;
    player.facing_right = 1;
    player.is_floating = 0;
    player.float_count = 0;
    player.anim_frame = 0;
    player.anim_timer = 0;
}

static void player_update(void) {
    // Horizontal movement
    player.vx = 0;
    
    if(key_down(KEY_LEFT)) {
        player.vx = -WALK_SPEED;
        player.facing_right = 0;
    }
    if(key_down(KEY_RIGHT)) {
        player.vx = WALK_SPEED;
        player.facing_right = 1;
    }
    
    // Jumping (A button)
    if(key_pressed(KEY_A)) {
        if(player.on_ground) {
            // Normal jump from ground
            player.vy = -JUMP_POWER;
            player.on_ground = 0;
            player.is_floating = 0;
        } else if(player.float_count < MAX_FLOATS) {
            // Float/puff jump in air (like Kirby!)
            player.is_floating = 1;
            player.vy = -FLOAT_POWER;
            player.float_count++;
        }
    }
    
    // Cancel float with B
    if(key_pressed(KEY_B) && player.is_floating) {
        player.is_floating = 0;
    }
    
    // Apply gravity
    if(player.is_floating) {
        player.vy += FLOAT_GRAVITY;
        if(player.vy > INT2FIX(1)) {
            player.vy = INT2FIX(1);  // Slower fall when floating
        }
    } else {
        player.vy += GRAVITY;
        if(player.vy > MAX_FALL) {
            player.vy = MAX_FALL;
        }
    }
    
    // Horizontal collision
    s32 new_x = FIX2INT(player.x + player.vx);
    s32 curr_y = FIX2INT(player.y);
    
    if(!check_collision_box(new_x, curr_y, PLAYER_W, PLAYER_H)) {
        player.x += player.vx;
    }
    
    // Vertical collision
    s32 curr_x = FIX2INT(player.x);
    s32 new_y = FIX2INT(player.y + player.vy);
    
    if(!check_collision_box(curr_x, new_y, PLAYER_W, PLAYER_H)) {
        player.y += player.vy;
        player.on_ground = 0;
    } else {
        if(player.vy > 0) {
            // Landing on ground
            player.on_ground = 1;
            player.is_floating = 0;
            player.float_count = 0;
            // Snap to tile
            player.y = INT2FIX(((new_y + PLAYER_H) >> 3) * 8 - PLAYER_H);
        } else {
            // Hit ceiling
            player.y = INT2FIX(((new_y >> 3) + 1) * 8);
        }
        player.vy = 0;
    }
    
    // Keep in bounds
    if(player.x < 0) player.x = 0;
    if(player.x > INT2FIX(MAP_W * 8 - PLAYER_W)) {
        player.x = INT2FIX(MAP_W * 8 - PLAYER_W);
    }
    if(player.y < 0) player.y = 0;
    if(player.y > INT2FIX(MAP_H * 8 - PLAYER_H)) {
        player.y = INT2FIX(MAP_H * 8 - PLAYER_H);
        player.on_ground = 1;
        player.is_floating = 0;
        player.float_count = 0;
    }
    
    // Animation
    player.anim_timer++;
    if(player.anim_timer >= 8) {
        player.anim_timer = 0;
        player.anim_frame = (player.anim_frame + 1) % 2;
    }
}

// ============================================================================
// CAMERA
// ============================================================================

static void camera_update(void) {
    // Target center of screen on player
    s32 target_x = FIX2INT(player.x) - 120 + PLAYER_W/2;
    s32 target_y = FIX2INT(player.y) - 80 + PLAYER_H/2;
    
    // Smooth follow
    camera.x += (target_x - camera.x) >> 3;
    camera.y += (target_y - camera.y) >> 3;
    
    // Clamp to level bounds
    if(camera.x < 0) camera.x = 0;
    if(camera.x > MAP_W * 8 - 240) camera.x = MAP_W * 8 - 240;
    if(camera.y < 0) camera.y = 0;
    if(camera.y > MAP_H * 8 - 160) camera.y = MAP_H * 8 - 160;
}

// ============================================================================
// RENDERING
// ============================================================================

static void render_player(void) {
    s32 screen_x = FIX2INT(player.x) - camera.x;
    s32 screen_y = FIX2INT(player.y) - camera.y;
    
    // Choose animation frame
    u32 tile_id;
    
    if(player.is_floating) {
        tile_id = 16;   // Puffed up frame
    } else if(!player.on_ground) {
        tile_id = 12;   // Jump frame
    } else if(player.vx != 0) {
        tile_id = 4 + player.anim_frame * 4;  // Walk frames
    } else {
        tile_id = 0;    // Standing frame
    }
    
    // Set sprite attributes
    obj_buffer[0].attr0 = ATTR0_Y(screen_y) | ATTR0_SQUARE | ATTR0_4BPP;
    obj_buffer[0].attr1 = ATTR1_X(screen_x) | ATTR1_SIZE_16 | 
                          (player.facing_right ? 0 : ATTR1_HFLIP);
    obj_buffer[0].attr2 = ATTR2_ID(tile_id) | ATTR2_PRIO(0) | ATTR2_PALBANK(0);
}

static void oam_update(void) {
    // Copy OAM buffer to hardware
    memcpy32((void*)MEM_OAM, obj_buffer, sizeof(OBJ_ATTR) * 128 / 4);
}

// ============================================================================
// INITIALIZATION
// ============================================================================

static void init_palettes(void) {
    // Background palette
    PAL_BG_MEM[0]  = RGB15(14, 20, 28);  // Sky blue
    PAL_BG_MEM[1]  = RGB15(0, 0, 0);     // Unused
    PAL_BG_MEM[2]  = RGB15(10, 6, 2);    // Dirt brown
    PAL_BG_MEM[3]  = RGB15(4, 20, 4);    // Grass green
    PAL_BG_MEM[4]  = RGB15(16, 10, 4);   // Platform tan
    PAL_BG_MEM[5]  = RGB15(20, 14, 8);   // Platform highlight
    PAL_BG_MEM[6]  = RGB15(31, 31, 31);  // Cloud white
    
    // Sprite palette (Kirby colors!)
    PAL_OBJ_MEM[0] = RGB15(31, 0, 31);   // Transparent (magenta)
    PAL_OBJ_MEM[1] = RGB15(31, 18, 22);  // Pink body
    PAL_OBJ_MEM[2] = RGB15(4, 4, 16);    // Dark blue eyes
    PAL_OBJ_MEM[3] = RGB15(28, 8, 8);    // Red cheeks
    PAL_OBJ_MEM[4] = RGB15(31, 10, 14);  // Red feet
    PAL_OBJ_MEM[5] = RGB15(24, 12, 16);  // Darker pink
}

static void init_sprites(void) {
    // Copy sprite graphics to OBJ VRAM
    memcpy32(TILE_OBJ_MEM, kirby_gfx, sizeof(kirby_gfx) / 4);
    
    // Initialize all sprites as hidden
    for(s32 i = 0; i < 128; i++) {
        obj_buffer[i].attr0 = ATTR0_HIDE;
        obj_buffer[i].attr1 = 0;
        obj_buffer[i].attr2 = 0;
        obj_buffer[i].pad = 0;
    }
}

static void init_background(void) {
    // Copy BG tiles to VRAM charblock 0
    memcpy32(TILE_MEM, bg_tiles, sizeof(bg_tiles) / 4);
    
    // Build tilemap in screenblock 28 (at 0x0600E000)
    // 64x32 map uses screenblocks 28 and 29
    u16* map0 = (u16*)(MEM_VRAM + 28 * 0x800);  // First 32 columns
    u16* map1 = (u16*)(MEM_VRAM + 29 * 0x800);  // Next 32 columns
    
    for(s32 y = 0; y < 32; y++) {
        for(s32 x = 0; x < 32; x++) {
            u8 tile = 0;
            if(y < MAP_H && x < MAP_W) {
                tile = level_map[y * MAP_W + x];
            }
            map0[y * 32 + x] = tile;
        }
        for(s32 x = 32; x < 64; x++) {
            u8 tile = 0;
            if(y < MAP_H && x < MAP_W) {
                tile = level_map[y * MAP_W + x];
            }
            map1[y * 32 + (x - 32)] = tile;
        }
    }
    
    // Configure BG0
    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_4BPP | BG_SIZE_64x32;
    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main(void) {
    // Set up display: Mode 0, BG0 enabled, OBJ enabled, 1D OBJ mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // Initialize everything
    init_palettes();
    init_sprites();
    init_background();
    player_init();
    
    camera.x = 0;
    camera.y = 0;
    
    // Main game loop
    while(1) {
        vsync();
        
        // Update
        input_poll();
        player_update();
        camera_update();
        
        // Render
        render_player();
        oam_update();
        
        // Update background scroll
        REG_BG0HOFS = camera.x;
        REG_BG0VOFS = camera.y;
    }
    
    return 0;
}
