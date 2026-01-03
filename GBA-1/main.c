#include <tonc.h>
#include <string.h>

// -------------------------------------------------------------------------
// CONSTANTS & CONFIGURATION
// -------------------------------------------------------------------------

// Screen is 30 tiles wide (240px) by 20 tiles high (160px)
#define MAP_WIDTH  30
#define MAP_HEIGHT 20

// Tile IDs for our procedural graphics
#define TID_GRASS   0
#define TID_DIRT    1
#define TID_WALL    2
#define TID_JUMP    3
#define TID_CAR     0 // Sprite Tile ID

// Physics Constants (Fixed Point 24.8)
// We use .8 fixed point for smoother sub-pixel physics on GBA
#define FRICTION    8    // Deceleration
#define ACCEL       12   // Acceleration force
#define MAX_SPEED   768  // Max speed (3.0 in fixed point)
#define TURN_SPEED  300  // Rotation speed

// -------------------------------------------------------------------------
// ASSETS (Procedural Generation)
// -------------------------------------------------------------------------

// A hardcoded 30x20 tilemap representing the track.
// 0=Grass, 1=Dirt, 2=Wall, 3=Jump
const u8 track_map[MAP_HEIGHT * MAP_WIDTH] = {
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,0,0,0,0,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,0,0,0,0,0,0,0,2,
    2,0,0,0,0,2,1,1,3,3,1,1,1,1,1,1,1,1,1,1,1,2,0,0,0,0,0,0,0,2,
    2,0,0,0,0,2,1,1,1,1,2,2,2,2,2,2,2,2,1,1,1,2,0,0,0,0,0,0,0,2,
    2,0,0,0,0,2,1,1,1,2,2,0,0,0,0,0,0,2,2,1,1,2,0,0,0,0,0,0,0,2,
    2,0,0,0,0,2,1,1,1,2,0,0,0,0,0,0,0,0,2,1,1,2,0,0,0,0,0,0,0,2,
    2,0,0,0,0,2,1,1,1,2,0,0,0,0,0,0,0,0,2,1,1,2,0,0,0,0,0,0,0,2,
    2,2,2,2,2,2,1,1,1,2,0,0,0,0,0,0,0,0,2,1,1,2,2,2,2,2,2,2,2,2,
    2,1,1,1,1,1,1,1,1,2,0,0,0,0,0,0,0,0,2,1,1,1,1,1,1,1,1,1,1,2,
    2,1,3,3,1,1,1,1,1,2,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,1,1,1,2,
    2,1,1,1,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,1,2,
    2,1,1,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,1,2,
    2,1,1,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,1,2,
    2,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,0,0,0,0,0,0,0,0,2,1,1,1,2,
    2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,0,0,0,0,0,0,0,0,2,1,1,1,2,
    2,1,1,3,3,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,1,1,1,2,
    2,1,1,1,1,2,2,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,
    2,1,1,1,1,2,0,0,0,0,0,0,2,1,1,3,3,1,1,1,1,1,1,1,1,1,1,1,1,2,
    2,1,1,1,1,2,0,0,0,0,0,0,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2
};

// Generates 8x8 tiles directly into VRAM
void load_procedural_graphics() {
    // 1. Set Background Palettes (16 colors per palette)
    // Palette 0: Backgrounds
    pal_bg_mem[0] = CLR_BLACK;         // Transparent/Back
    pal_bg_mem[1] = RGB15(2, 12, 4);   // Grass Dark
    pal_bg_mem[2] = RGB15(5, 18, 5);   // Grass Light
    pal_bg_mem[3] = RGB15(18, 10, 5);  // Dirt
    pal_bg_mem[4] = RGB15(22, 14, 8);  // Dirt Highlight
    pal_bg_mem[5] = RGB15(15, 15, 15); // Wall
    pal_bg_mem[6] = RGB15(10, 10, 10); // Wall Dark
    pal_bg_mem[7] = RGB15(25, 25, 0);  // Jump Arrows

    // Palette 0 for Sprites
    pal_obj_mem[0] = CLR_MAG;      // Transparent
    pal_obj_mem[1] = CLR_RED;          // Car Body
    pal_obj_mem[2] = CLR_BLUE;         // Windshield
    pal_obj_mem[3] = RGB15(5,5,5);     // Shadow/Tires

    // 2. Create Tiles in VRAM (Char Block 0 for BG, Char Block 4 for Obj)
    // We use 4bpp tiles (32 bytes per tile)
    
    // --- BG Tile 0: Grass (Checkerboard) ---
    TILE *tile_grass = &tile_mem[0][TID_GRASS];
    for(int i=0; i<8; i++) {
        // Simple pixel pattern generation
        tile_grass->data[i] = (i%2==0) ? 0x12121212 : 0x21212121; 
    }

    // --- BG Tile 1: Dirt (Random-ish noise) ---
    TILE *tile_dirt = &tile_mem[0][TID_DIRT];
    for(int i=0; i<8; i++) {
        tile_dirt->data[i] = 0x33433343 + (i*0x12003); 
    }

    // --- BG Tile 2: Wall (Bricks) ---
    TILE *tile_wall = &tile_mem[0][TID_WALL];
    for(int i=0; i<8; i++) {
        if(i==0 || i==4) tile_wall->data[i] = 0x66666666; // Mortar line
        else if (i < 4)  tile_wall->data[i] = 0x56555565; // Brick offset A
        else             tile_wall->data[i] = 0x55565555; // Brick offset B
    }

    // --- BG Tile 3: Jump (Arrows) ---
    TILE *tile_jump = &tile_mem[0][TID_JUMP];
    // Fill with dirt first
    for(int i=0; i<8; i++) tile_jump->data[i] = 0x33333333;
    // Draw an arrow pointing Right (simplistic)
    tile_jump->data[2] = 0x33770033;
    tile_jump->data[3] = 0x33377003;
    tile_jump->data[4] = 0x33377003;
    tile_jump->data[5] = 0x33770033;

    // --- Sprite Tile: Car ---
    // We will use a 16x16 sprite (4 tiles in VRAM). 
    // Since we are using Affine rotation (Mode 0 Rot/Scale), 
    // the sprite points RIGHT by default (angle 0).
    // Charblock 4 is for sprites.
    
    // Clear the 4 tiles first
    memset(&tile_mem[4][0], 0, 32*4);

    u32 *car_gfx = (u32*)&tile_mem[4][0];
    
    // Drawing a crude car shape into the 16x16 space (Tiles 0-3)
    // This is raw hex pixel pushing for a 16x16 4bpp image.
    // Row 4-11 (Body)
    // Columns 2-14
    
    // This is hard to hand-code pixel by pixel, so we'll do a simple loop fill
    // then punch out details.
    
    // Fill central body (Red = 1)
    for(int t=0; t<4; t++) { // 4 tiles
        for(int l=0; l<8; l++) { // 8 lines per tile
            // Logic to determine if pixel is inside "Car box"
            // Visualizing 16x16 space...
            // Let's just draw a red box for simplicity of the single file
            // with a blue stripe for windshield to show orientation.
            
            // Tile 0 (Top Left), 1 (Top Right), 2 (Bot Left), 3 (Bot Right)
            // Actually in 1D mapping (which we set), it's linear:
            // 0: top 8x8, 1: bottom 8x8 (Wait, 16x16 sprite is 2x2 tiles)
            // In 1D mapping: Tile 0 is top-left, Tile 1 is top-right, etc? 
            // Standard 16x16 sprite layout in 1D mode:
            // Tile N   : Top row, left half
            // Tile N+1 : Top row, right half
            // Tile N+2 : Bottom row, left half... (depends on screen width usually, but for sprites it's linear blocks)
            
            // Let's keep it extremely simple: Fill everything with Red
            // Then overwrite windshield.
             tile_mem[4][t].data[l] = 0x11111111;
        }
    }
    
    // Add Windshield (Blue = 2) to indicate "Front" (Right side)
    // Right side tiles are 1 and 3.
    // Let's modify Tile 1 (Top Right)
    tile_mem[4][1].data[2] = 0x11222211;
    tile_mem[4][1].data[3] = 0x11222211;
    tile_mem[4][1].data[4] = 0x11222211;
    tile_mem[4][1].data[5] = 0x11222211;
    // Bottom Right (Tile 3)
    tile_mem[4][3].data[2] = 0x11222211;
    tile_mem[4][3].data[3] = 0x11222211;
    tile_mem[4][3].data[4] = 0x11222211;
    tile_mem[4][3].data[5] = 0x11222211;
    
    // Add Tires (Black/Dark = 3)
    // Top Left (Tile 0) top rows
    tile_mem[4][0].data[1] = 0x33333111; 
    tile_mem[4][0].data[2] = 0x33333111;
    // Bot Left (Tile 2) bot rows
    tile_mem[4][2].data[5] = 0x33333111;
    tile_mem[4][2].data[6] = 0x33333111;
}

// Fill the screen block with the map data
void load_map() {
    // Screen Block 30 (standard for BG0)
    u16 *dst = se_mem[30];
    for(int i=0; i<MAP_WIDTH*MAP_HEIGHT; i++) {
        // Just map straight values to tile indices
        dst[i] = track_map[i];
    }
}

// -------------------------------------------------------------------------
// GAME OBJECTS & LOGIC
// -------------------------------------------------------------------------

typedef struct {
    s32 x, y;       // Position (24.8 Fixed Point)
    s32 vx, vy;     // Velocity (24.8 Fixed Point)
    u16 angle;      // 0 - 0xFFFF
    s32 z;          // Height (for jumps)
    s32 z_vel;      // Vertical velocity
} Car;

Car car;
OBJ_ATTR obj_buffer[128];
OBJ_AFFINE *obj_aff = (OBJ_AFFINE*)obj_buffer;

void init_car() {
    // Start position (Approx Tile 10, 15)
    car.x = int2fx(30); 
    car.y = int2fx(120);
    car.vx = 0;
    car.vy = 0;
    car.angle = 0; // Pointing Right
    car.z = 0;
    car.z_vel = 0;
}

void update_physics() {
    // 1. Input Handling
    // Steering
    if(key_is_down(KEY_LEFT))  car.angle += TURN_SPEED;
    if(key_is_down(KEY_RIGHT)) car.angle -= TURN_SPEED;

    // Acceleration (A button)
    if(key_is_down(KEY_A)) {
        // Calculate acceleration vector based on angle
        // lu_cos/sin take 0-0xFFFF angle, return .12 fixed point
        car.vx += (lu_cos(car.angle) * ACCEL) >> 12;
        car.vy += (lu_sin(car.angle) * ACCEL) >> 12; // Y is inverted in screen coords? No, Down is +, Sin is -? 
        // Tonc's sin/cos: angle rotates counter-clockwise.
        // Screen Y grows Down. 
        // We want 0 to be Right. 
        // If angle increases (Left Turn), we go Counter Clockwise.
        // Standard Unit Circle: 0 is Right (1,0). 90 is Up (0,1).
        // Screen: 0 is Right (1,0). 90 is Down (0,1)? No, on screen Y grows down.
        // So actually, we want -sin for Y to behave like math class, 
        // but for screen coords:
        // Angle 0: x=1, y=0.
        // Angle 90 (CCW): x=0, y=-1 (Up).
        // Since we subtract 90 to turn right visually, let's just stick to standard and see.
        // In GBA: +Y is Down. So -Sin(angle) moves UP. +Sin(angle) moves DOWN.
        // This effectively mirrors the Y axis relative to standard math.
        car.vy -= (lu_sin(car.angle) * ACCEL) >> 12; 
    }

    // Brake / Reverse (B button)
    if(key_is_down(KEY_B)) {
         car.vx -= (lu_cos(car.angle) * (ACCEL/2)) >> 12;
         car.vy += (lu_sin(car.angle) * (ACCEL/2)) >> 12;
    }

    // 2. Apply Friction
    car.vx -= car.vx >> 4; // Simple drag
    car.vy -= car.vy >> 4;

    // 3. Update Position
    car.x += car.vx;
    car.y -= car.vy; // Subtract because +Sin was Down, but we inverted logic above

    // 4. Map Collision
    // Convert Fixed Point X/Y to Tile Coords
    int tx = fx2int(car.x) / 8;
    int ty = fx2int(car.y) / 8;

    // Boundary Check
    if(tx < 0) tx = 0; if(tx >= MAP_WIDTH) tx = MAP_WIDTH-1;
    if(ty < 0) ty = 0; if(ty >= MAP_HEIGHT) ty = MAP_HEIGHT-1;

    u8 tile_id = track_map[ty * MAP_WIDTH + tx];

    // Jump Logic (Pseudo-3D)
    if(tile_id == TID_JUMP) {
        if(car.z == 0) car.z_vel = int2fx(4); // Pop up
    }

    // Gravity
    if(car.z > 0 || car.z_vel != 0) {
        car.z += car.z_vel;
        car.z_vel -= 32; // Gravity constant
        if(car.z < 0) {
            car.z = 0;
            car.z_vel = 0;
            // Bounce logic could go here
        }
    }

    // Wall Collision (Only if on ground)
    if(car.z < int2fx(5)) { // Can fly over walls if high enough
        if(tile_id == TID_WALL) {
            // Very basic bounce: invert velocity
            car.vx = -car.vx;
            car.vy = -car.vy;
            // Push out of wall slightly to prevent sticking
            car.x += car.vx * 2;
            car.y -= car.vy * 2;
        }
    }
}

void update_visuals() {
    // Use OAM index 0
    OBJ_ATTR *car_obj = &obj_buffer[0];

    // Calc screen pos
    int scr_x = fx2int(car.x) - 8; // Center sprite (16x16)
    int scr_y = fx2int(car.y) - 8 - fx2int(car.z); // Apply Z height to Y pos

    // Configure Sprite using Affine Matrix for rotation
    // Attribute 0: Shape(Square), Mode(Affine), ColorMode(4bpp), Y
    obj_set_attr(car_obj, 
        ATTR0_SQUARE | ATTR0_AFF | ATTR0_4BPP | (scr_y & 0x00FF), // Mask Y
        ATTR1_SIZE_16 | ATTR1_AFF_ID(0) | (scr_x & 0x01FF),       // Mask X, Set Affine Index 0
        ATTR2_PALBANK(0) | 0                                      // Tile Index 0
    );

    // Update Affine Matrix (Rotation)
    // We need to calculate the matrix based on car.angle
    // P_A = cos(a) * sx, P_B = -sin(a) * sx
    // P_C = sin(a) * sy, P_D =  cos(a) * sy
    // Since scale is 1.0 (0x0100), we just use the LUT directly.
    
    // Note: Tonc's obj_aff_rotate is convenient
    obj_aff_rotate(&obj_aff[0], car.angle);
}


// -------------------------------------------------------------------------
// MAIN
// -------------------------------------------------------------------------
int main() {
    // 1. Initialize System
    // Mode 0, BG0 enabled, OBJ enabled, 1D Sprite Mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // 2. Initialize BG0 Control
    // CBB=0 (Tiles), SBB=30 (Map), 4bpp, Size=0 (256x256)
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32;

    // 3. Load Assets
    load_procedural_graphics();
    load_map();
    
    // 4. Init OAM
    oam_init(obj_buffer, 128);
    init_car();

    // 5. Main Loop
    while(1) {
        vid_vsync();
        key_poll();

        update_physics();
        update_visuals();

        // Copy OAM buffer to actual OAM memory
        oam_copy(oam_mem, obj_buffer, 128);
    }

    return 0;
}
