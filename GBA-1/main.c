#include <tonc.h>

// ===========================================================================
// CONSTANTS & CONFIGURATION
// ===========================================================================

#define MAP_WIDTH   64    // Map width in tiles (64 * 8 = 512 pixels)
#define MAP_HEIGHT  32    // Map height in tiles (32 * 8 = 256 pixels)
#define TILE_SIZE   8

// Physics Constants (Fixed Point Math 8.8)
#define GRAVITY     0x40  // 0.25 pixels per frame
#define JUMP_FORCE  0x550 // Initial jump impulse
#define ACCEL       0x20  // Run acceleration
#define FRICTION    0x15  // Ground friction
#define MAX_SPEED   0x300 // Max run speed
#define TERMINAL_VEL 0x600 // Max fall speed

// Tile IDs
#define TID_EMPTY   0
#define TID_BRICK   1
#define TID_BLOCK   2

// ===========================================================================
// GLOBALS & ASSETS
// ===========================================================================

// The Level Map (Indices referring to tiles)
u16 map_data[MAP_WIDTH * MAP_HEIGHT];

// Represents the camera/viewport
typedef struct {
    int x;
    int y; // Keep y fixed for this demo, only scroll X
} Camera;

// Represents the Player (Mario)
typedef struct {
    int x, y;       // Position (Fixed point 24.8)
    int vx, vy;     // Velocity (Fixed point 24.8)
    int width, height;
    bool grounded;
    bool facingRight;
} Player;

Camera cam = {0, 0};
Player p = {0, 0, 0, 0, 16, 16, false, true};
OBJ_ATTR obj_buffer[128]; // OAM Buffer

// ===========================================================================
// GRAPHICS GENERATION (Procedural to keep file single)
// ===========================================================================

// Writes a simple brick pattern to VRAM
void load_tile_graphics() {
    // Palette Setup
    pal_bg_mem[0] = CLR_SKYBLUE;
    pal_bg_mem[1] = RGB15(20, 10, 5); // Brown Brick
    pal_bg_mem[2] = RGB15(31, 31, 0); // Yellow Block
    pal_bg_mem[3] = RGB15(0, 0, 0);   // Black outlines

    // Sprite Palette
    pal_obj_mem[0] = CLR_MAGENTA; // Transparent
    pal_obj_mem[1] = RGB15(31, 0, 0); // Mario Red
    pal_obj_mem[2] = RGB15(31, 20, 10); // Skin tone
    pal_obj_mem[3] = RGB15(0, 0, 31); // Overalls Blue

    // Get pointer to Tile VRAM (Char Block 0 for BG)
    TILE4 *tile_mem_bg = (TILE4*)tile_mem[0];
    
    // 1. Create Empty Tile (Sky)
    tonccpy(&tile_mem_bg[TID_EMPTY], &tile_mem_bg[TID_EMPTY], sizeof(TILE4)); // All 0

    // 2. Create Brick Tile
    u32 brick_pixels[8]; 
    for(int i=0; i<8; i++) brick_pixels[i] = 0x11111111; // Fill brown
    brick_pixels[0] = 0x33333333; // Top highlight
    brick_pixels[7] = 0x33333333; // Bottom shadow
    memcpy(&tile_mem_bg[TID_BRICK], brick_pixels, sizeof(TILE4));

    // 3. Create Block Tile (Question block)
    u32 block_pixels[8];
    for(int i=0; i<8; i++) block_pixels[i] = 0x22222222; // Fill yellow
    block_pixels[0] = 0x33333333; // Border
    block_pixels[7] = 0x33333333;
    memcpy(&tile_mem_bg[TID_BLOCK], block_pixels, sizeof(TILE4));

    // 4. Create Player Sprite (16x16 = 4 tiles) in Char Block 4
    TILE4 *tile_mem_obj = (TILE4*)tile_mem[4];
    
    // Simple 16x16 Mario-ish shape (Hardcoded pixel art)
    // 0=Trans, 1=Red, 2=Skin, 3=Blue
    // We fill 4 tiles (UpperL, UpperR, LowerL, LowerR)
    memset(tile_mem_obj, 0, sizeof(TILE4)*4);

    // Draw a red box with blue bottom manually for simplicity
    for(int t=0; t<4; t++) {
        u8 *px = (u8*)&tile_mem_obj[t];
        for(int i=0; i<64; i++) {
            // Logic to draw a simple person shape
            int r = i / 8; // row in tile
            int c = i % 8; // col in tile
            
            // Upper tiles
            if(t < 2) { 
                if (r > 2) px[i] = 1; // Red Hat/Shirt
                if (r > 4 && c > 2 && c < 6 && t==0) px[i] = 2; // Face
            } 
            // Lower tiles
            else {
                px[i] = 3; // Blue pants
            }
        }
    }
}

// ===========================================================================
// MAP LOGIC
// ===========================================================================

void init_map() {
    // Clear map
    for(int i=0; i<MAP_WIDTH*MAP_HEIGHT; i++) map_data[i] = TID_EMPTY;

    // Create Floor
    for(int x=0; x<MAP_WIDTH; x++) {
        map_data[18 * MAP_WIDTH + x] = TID_BRICK;
        map_data[19 * MAP_WIDTH + x] = TID_BRICK;
    }

    // Create some platforms and pipes
    for(int x=10; x<15; x++) map_data[14 * MAP_WIDTH + x] = TID_BRICK;
    for(int x=20; x<22; x++) map_data[12 * MAP_WIDTH + x] = TID_BLOCK;
    
    // A wall
    for(int y=14; y<18; y++) map_data[y * MAP_WIDTH + 30] = TID_BRICK;

    // Steps
    for(int i=0; i<5; i++) {
        map_data[(17-i) * MAP_WIDTH + (40+i)] = TID_BRICK;
        // fill below
        for(int j=17; j>(17-i); j--) map_data[j * MAP_WIDTH + (40+i)] = TID_BRICK;
    }

    // Copy map to Screenblock 30 (Reg BG0 points here)
    SCR_ENTRY *sbb = (SCR_ENTRY*)se_mem[30];
    for(int y=0; y<32; y++) {
        for(int x=0; x<32; x++) { // Only load first screen initially
            sbb[y*32+x] = map_data[y*MAP_WIDTH + x];
        }
    }
}

// Update the hardware map based on camera position (Simple column loading)
void update_scroll_map() {
    SCR_ENTRY *sbb = (SCR_ENTRY*)se_mem[30];
    
    // We are using a wide map in memory, but the GBA hardware map is 32x32 or 64x32.
    // For this single file demo, we use a 64x32 background size (Size 1).
    // This allows 512 pixels wide without complex texture swapping.
    
    for(int y=0; y<32; y++) {
        for(int x=0; x<64; x++) {
            sbb[y*32+x] = map_data[y*MAP_WIDTH + x];
        }
    }
}

// ===========================================================================
// PHYSICS & COLLISION
// ===========================================================================

// Get tile ID at specific pixel coordinate
u16 get_tile_at(int x, int y) {
    int tx = x / 8;
    int ty = y / 8;
    if(tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) return 0;
    return map_data[ty * MAP_WIDTH + tx];
}

void update_player() {
    // 1. Input Handling
    key_poll();
    
    // Horizontal Movement
    if(key_is_down(KEY_RIGHT)) {
        p.vx += ACCEL;
        p.facingRight = true;
    } else if (key_is_down(KEY_LEFT)) {
        p.vx -= ACCEL;
        p.facingRight = false;
    } else {
        // Friction
        if(p.vx > 0) p.vx = MAX(0, p.vx - FRICTION);
        else if(p.vx < 0) p.vx = MIN(0, p.vx + FRICTION);
    }

    // Cap Speed
    p.vx = CLAMP(p.vx, -MAX_SPEED, MAX_SPEED);

    // Jump
    if(key_hit(KEY_A) && p.grounded) {
        p.vy = -JUMP_FORCE;
        p.grounded = false;
    }

    // Variable Jump Height (Gravity is stronger if A is not held)
    if(p.vy < 0 && !key_is_down(KEY_A)) {
        p.vy += GRAVITY * 2; 
    } else {
        p.vy += GRAVITY;
    }
    
    // Terminal Velocity
    p.vy = MIN(p.vy, TERMINAL_VEL);

    // 2. X Axis Collision
    // Calculate potential new X position
    int new_x = p.x + p.vx;
    
    // Check bounding box corners in direction of movement
    int left   = fx2int(new_x);
    int right  = fx2int(new_x) + p.width - 1;
    int top    = fx2int(p.y);
    int bottom = fx2int(p.y) + p.height - 1;

    bool collision = false;
    if (p.vx > 0) { // Moving Right
        if (get_tile_at(right, top) || get_tile_at(right, bottom)) {
            p.x = int2fx((right / 8) * 8 - p.width);
            p.vx = 0;
            collision = true;
        }
    } else if (p.vx < 0) { // Moving Left
        if (get_tile_at(left, top) || get_tile_at(left, bottom)) {
            p.x = int2fx((left / 8) * 8 + 8);
            p.vx = 0;
            collision = true;
        }
    }

    if (!collision) p.x = new_x;

    // 3. Y Axis Collision
    int new_y = p.y + p.vy;
    
    left   = fx2int(p.x);
    right  = fx2int(p.x) + p.width - 1;
    top    = fx2int(new_y);
    bottom = fx2int(new_y) + p.height - 1;

    p.grounded = false;
    collision = false;

    if (p.vy > 0) { // Falling
        if (get_tile_at(left, bottom) || get_tile_at(right, bottom)) {
            p.y = int2fx((bottom / 8) * 8 - p.height);
            p.vy = 0;
            p.grounded = true;
            collision = true;
        }
    } else if (p.vy < 0) { // Jumping Up
        if (get_tile_at(left, top) || get_tile_at(right, top)) {
            p.y = int2fx((top / 8) * 8 + 8);
            p.vy = 0;
            collision = true;
        }
    }

    if (!collision) p.y = new_y;

    // Map Boundaries
    if(p.x < 0) p.x = 0;
    if(p.x > int2fx(MAP_WIDTH*8 - p.width)) p.x = int2fx(MAP_WIDTH*8 - p.width);
    if(p.y > int2fx(SCREEN_HEIGHT*2)) { // Death pit reset
        p.x = int2fx(32); 
        p.y = int2fx(64);
        p.vy = 0;
    }
}

// ===========================================================================
// MAIN
// ===========================================================================

int main() {
    // 1. Hardware Init
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // BG0 Setup: Size 1 (512x256), 256 colors, CharBase 0, ScreenBase 30
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_8BPP | BG_REG_64x32;
    
    // Init OAM
    oam_init(obj_buffer, 128);

    // 2. Assets & Data
    load_tile_graphics();
    init_map();
    update_scroll_map(); // Load map to VRAM

    // Init Player Pos
    p.x = int2fx(32);
    p.y = int2fx(100);

    // 3. Main Loop
    while(1) {
        vid_vsync();

        // -- Logic --
        update_player();

        // Camera Logic (Center player)
        int px = fx2int(p.x);
        cam.x = px - (SCREEN_WIDTH / 2) + (p.width / 2);
        
        // Clamp Camera
        if(cam.x < 0) cam.x = 0;
        if(cam.x > (MAP_WIDTH * 8) - SCREEN_WIDTH) cam.x = (MAP_WIDTH * 8) - SCREEN_WIDTH;

        // -- Rendering --
        
        // Update Scroll Registers
        REG_BG0HOFS = cam.x;
        REG_BG0VOFS = cam.y;

        // Draw Player Sprite
        OBJ_ATTR *sprite = &obj_buffer[0];
        
        // To achieve 16x16, we use Shape=Square, Size=2 (16x16)
        // Attribute 0: Y pos, Shape, Mode
        // Attribute 1: X pos, Size, Flip
        // Attribute 2: Tile Index, Priority, Palette
        
        int screen_x = px - cam.x;
        int screen_y = fx2int(p.y) - cam.y;

        u32 attr1 = ATTR1_SIZE_16; // 16x16 sprite
        if(!p.facingRight) attr1 |= ATTR1_HFLIP;

        obj_set_attr(sprite, 
            ATTR0_SQUARE | (screen_y & 0x00FF),  // Y
            attr1 | (screen_x & 0x01FF),         // X
            ATTR2_PALBANK(0) | 0                 // Tile 0 in CBB 4
        );

        // Push OAM to hardware
        oam_copy(oam_mem, obj_buffer, 1);
    }

    return 0;
}
