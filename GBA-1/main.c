// -------------------------------------------------------------------------
// GBA Side-Scroller (Kirby-style) using TONC
// Single file implementation
// -------------------------------------------------------------------------

#include <tonc.h>
#include <stdio.h>

// -------------------------------------------------------------------------
// Constants & Definitions
// -------------------------------------------------------------------------

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 160

// Physics Constants (Using 8.8 Fixed Point for smooth movement)
#define FIX_SHIFT     8
#define FIX_SCALE     (1 << FIX_SHIFT)
#define GRAVITY       (0x180)      // Downward acceleration
#define JUMP_FORCE    (-0x700)     // Initial jump velocity
#define MOVE_SPEED    (0x150)      // Horizontal acceleration
#define FRICTION      (0xE0)       // Deceleration (floor friction)
#define MAX_SPEED     (0x400)      // Terminal velocity
#define FLOAT_POWER   (-0x120)     // Upward force when floating

// Map Settings
#define TILE_SIZE     16
#define MAP_WIDTH     64
#define MAP_HEIGHT    8
#define TILE_SOLID    1
#define TILE_EMPTY    0

// -------------------------------------------------------------------------
// Assets (Procedurally generated in code to keep single file)
// -------------------------------------------------------------------------

// Simple Level Map: 1 = Block, 0 = Air
// A flat floor with some steps and a pit
const u8 levelMap[MAP_HEIGHT * MAP_WIDTH] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // Row 0
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // Row 1
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // Row 2
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // Row 3
    0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // Row 4 (Platform)
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0, // Row 5 (Platform)
    1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1, // Row 6 (Gaps)
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1  // Row 7 (Floor)
};

// -------------------------------------------------------------------------
// Player Structure
// -------------------------------------------------------------------------
typedef struct {
    FIXED x, y;       // Position (8.8 fixed point)
    FIXED vx, vy;     // Velocity
    int w, h;         // Width and Height in pixels
    BOOL grounded;    // Is touching ground?
    int facing;       // 1 = Right, -1 = Left
} Player;

Player player;

// -------------------------------------------------------------------------
// Helper Functions
// -------------------------------------------------------------------------

// Helper: Draw a single colored tile into VRAM
// Color 1 = Pink (Body), Color 2 = Black (Outline/Eyes), Color 3 = Grey (Brick)
void create_assets() {
    // --- Setup Palette (256 color mode) ---
    // 0: Transparent
    pal_bg_mem[0] = RGB15(0, 0, 0);
    pal_obj_mem[0] = RGB15(0, 0, 0);
    
    // 1: Kirby Pink
    pal_obj_mem[1] = RGB15(31, 15, 20); 
    // 2: Kirby Eye/Outline
    pal_obj_mem[2] = RGB15(0, 0, 5);    
    // 3: Ground Color (Grey)
    pal_bg_mem[1] = RGB15(15, 15, 15); 
    pal_bg_mem[2] = RGB15(10, 10, 10);

    // --- Create Sprite Tile (Tile 0 in OBJ VRAM) ---
    // 16x16 tile for Kirby (Simple circle face)
    u32* dst = (u32*)&tile_mem[4][0]; // Charblock 4 is standard for OBJs in TONC defaults
    // Note: Writing 32bits at a time. 1 tile (16x16 8bpp) = 256 bytes = 64 writes.
    // We will just fill raw pixel indices here manually for simplicity.
    
    // This is a hardcoded 16x16 bitmap (indices into palette)
    // 0=trans, 1=pink, 2=eye
    const u8 rawKirby[] = {
        0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,
        0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,2,1,1,1,1,1,1,1,1,1,1,2,1,1, // Eyes
        1,1,2,1,1,1,1,1,1,1,1,1,1,2,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
        0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0
    };
    // Copy to OBJ tile memory
    memcpy(&tile_mem[4][0], rawKirby, 16*16);

    // --- Create Background Tile (Tile 1 in BG VRAM) ---
    // 16x16 Brick pattern
    u8 brick[16*16];
    for(int i=0; i<16*16; i++) brick[i] = 1; // Fill Grey
    // Add black outline/border logic roughly
    for(int i=0; i<16; i++) {
        brick[i] = 2;             // Top edge
        brick[16*15+i] = 2;       // Bottom edge
        brick[i*16] = 2;          // Left edge
        brick[i*16+15] = 2;       // Right edge
    }
    // Middle line for brick look
    for(int i=0; i<7; i++) brick[8*16 + i] = 2;
    for(int i=9; i<16; i++) brick[8*16 + i] = 2;

    memcpy(&tile_mem[0][1], brick, 16*16);
}

// Check if a point in the map is solid
// Returns 1 if solid, 0 if air
bool is_solid(int x, int y) {
    // Convert pixel coordinates to tile coordinates
    int tx = x / TILE_SIZE;
    int ty = y / TILE_SIZE;

    // Boundary checks
    if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) {
        // Treat out of bounds as solid (or air if you prefer wrapping, but solid is safer for floors)
        return 0; 
    }

    return levelMap[ty * MAP_WIDTH + tx] == TILE_SOLID;
}

// -------------------------------------------------------------------------
// Main Logic
// -------------------------------------------------------------------------

int main() {
    // 1. Initialize TONC
    irq_init(NULL);
    irq_enable(IE_VBLANK);

    // 2. Setup Video Mode
    // Mode 0: BG0 (Text/Tilemap) and Objects enabled
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // 3. Setup Background (BG0)
    // Using Charblock 0, Screenblock 30, 8bpp
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_8BPP | BG_REG_32x32;

    // 4. Generate Assets
    create_assets();

    // 5. Load Map into Screenblock 30
    // Map is 64x8. Screen Entry is 32x32. We'll fill the relevant parts.
    u16* se_mem = se_mem[30];
    for (int i = 0; i < MAP_WIDTH * MAP_HEIGHT; i++) {
        // Palette 0, Tile index 1 if solid, 0 (empty) if air
        se_mem[i] = (levelMap[i] == TILE_SOLID) ? 1 : 0;
    }

    // 6. Initialize Player
    player.x = int2fix(50); // Start at pixel 50
    player.y = int2fix(50);
    player.vx = 0;
    player.vy = 0;
    player.w = 16;
    player.h = 16;
    player.grounded = false;
    player.facing = 1;

    // Camera
    FIXED cam_x = 0;

    // Main Game Loop
    while (1) {
        // --- Input Handling ---
        key_poll();

        // Horizontal Movement
        if (key_is_down(KEY_LEFT)) {
            player.vx -= MOVE_SPEED;
            player.facing = -1;
        }
        if (key_is_down(KEY_RIGHT)) {
            player.vx += MOVE_SPEED;
            player.facing = 1;
        }

        // Friction
        player.vx = (player.vx * FRICTION) >> FIX_SHIFT;

        // Jump
        if (key_hit(KEY_A)) {
            if (player.grounded) {
                player.vy = JUMP_FORCE;
                player.grounded = false;
            }
        }

        // KIRBY FLOAT: Holding B in air reduces gravity/adds lift
        if (key_is_down(KEY_B) && !player.grounded) {
            player.vy += FLOAT_POWER; 
        } else {
            player.vy += GRAVITY;
        }

        // Terminal Velocity Cap
        if (player.vy > MAX_SPEED) player.vy = MAX_SPEED;
        if (player.vx > MAX_SPEED) player.vx = MAX_SPEED;
        if (player.vx < -MAX_SPEED) player.vx = -MAX_SPEED;

        // --- Physics Update (X Axis) ---
        player.x += player.vx;
        
        // X Collision
        // Check Left and Right edges
        int pixel_x = fix2int(player.x);
        int pixel_y = fix2int(player.y);
        
        // Determine edges based on facing direction or simple box
        if (player.vx < 0) { // Moving Left
            if (is_solid(pixel_x, pixel_y + 2) || is_solid(pixel_x, pixel_y + player.h - 2)) {
                player.x = int2fix((pixel_x / TILE_SIZE) + 1) * TILE_SIZE;
                player.vx = 0;
            }
        } else if (player.vx > 0) { // Moving Right
            if (is_solid(pixel_x + player.w, pixel_y + 2) || is_solid(pixel_x + player.w, pixel_y + player.h - 2)) {
                player.x = int2fix((pixel_x + player.w) / TILE_SIZE) * TILE_SIZE - int2fix(player.w);
                player.vx = 0;
            }
        }

        // --- Physics Update (Y Axis) ---
        player.y += player.vy;
        pixel_x = fix2int(player.x);
        pixel_y = fix2int(player.y);
        player.grounded = false;

        // Y Collision
        if (player.vy < 0) { // Moving Up (Head bump)
            if (is_solid(pixel_x + 2, pixel_y) || is_solid(pixel_x + player.w - 2, pixel_y)) {
                player.y = int2fix((pixel_y / TILE_SIZE) + 1) * TILE_SIZE;
                player.vy = 0;
            }
        } else if (player.vy > 0) { // Moving Down (Falling)
            if (is_solid(pixel_x + 2, pixel_y + player.h) || is_solid(pixel_x + player.w - 2, pixel_y + player.h)) {
                player.y = int2fix((pixel_y + player.h) / TILE_SIZE) * TILE_SIZE - int2fix(player.h);
                player.vy = 0;
                player.grounded = true;
            }
        }

        // --- Camera Logic ---
        // Keep player in center-left of screen
        int target_cam = fix2int(player.x) - 100;
        
        // Simple linear interpolation or just hard lock
        // Hard lock with bounds
        if (target_cam < 0) target_cam = 0;
        if (target_cam > (MAP_WIDTH * TILE_SIZE) - SCREEN_WIDTH) 
            target_cam = (MAP_WIDTH * TILE_SIZE) - SCREEN_WIDTH;
        
        cam_x = int2fix(target_cam);

        // Update BG Offset
        REG_BG0HOFS = fix2int(cam_x);

        // --- Rendering ---
        // Update Sprite 0
        obj_set_attr(&oam_mem[0],
            ATTR0_SHAPE(0) | ATTR0_8BPP | ATTR0_REG | ATTR0_Y(fix2int(player.y)),
            ATTR1_SIZE(0) | ATTR1_X(fix2int(player.x) - fix2int(cam_x)) | (player.facing == -1 ? ATTR1_HFLIP : 0),
            ATTR2_PALBANK(0) | ATTR2_ID(0) // Using Tile ID 0
        );

        // Wait for VBlank
        vid_vsync();
    }

    return 0;
}
