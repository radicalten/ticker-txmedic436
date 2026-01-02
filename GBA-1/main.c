#include <tonc.h>

// --------------------------------------------------------------------------
// CONSTANTS & PHYSICS
// --------------------------------------------------------------------------
#define GRAVITY         0x40    // Fixed point 8.8 (0.25 pixels/frame)
#define JUMP_FORCE      0x500   // Initial jump
#define FLOAT_FORCE     0x250   // The "puff" jump in mid-air
#define WALK_ACCEL      0x40
#define FRICTION        0x20
#define MAX_SPEED       0x300
#define FLOOR_Y         (120 << 8) // Y position of the ground (Fixed point)

// --------------------------------------------------------------------------
// GRAPHICS DATA
// --------------------------------------------------------------------------

// A simple 16-color palette: Transparent, Black, Kirby Pink, Dark Pink, Red (Shoes)
const unsigned short kirby_pal[] = {
    CL_TRANS, RGB15(0,0,0), RGB15(31,20,24), RGB15(28,10,18), RGB15(31,0,5), 
    RGB15(31,31,31), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// 16x16 Sprite Tiles (4bpp). A simple round "ball" character.
// 4 tiles total (TopL, TopR, BotL, BotR)
const unsigned int kirby_tiles[32] = {
    // Tile 0: Top Left
    0x00000000, 0x00000000, 0x00022200, 0x00222220, 0x02222552, 0x02222552, 0x22222222, 0x22222222,
    // Tile 1: Top Right
    0x00000000, 0x00000000, 0x00222000, 0x02222200, 0x25522220, 0x25522220, 0x22222222, 0x22222222,
    // Tile 2: Bottom Left
    0x22222222, 0x22222222, 0x22223322, 0x02222222, 0x00222220, 0x04442200, 0x04444000, 0x00000000,
    // Tile 3: Bottom Right
    0x22222222, 0x22222222, 0x22332222, 0x22222220, 0x02222200, 0x00224440, 0x00044440, 0x00000000
};

// Simple Ground Tile (8x8) - Chequered pattern
const unsigned int ground_tile[8] = {
    0x11111111, 0x22222222, 0x11111111, 0x22222222,
    0x11111111, 0x22222222, 0x11111111, 0x22222222
};

// --------------------------------------------------------------------------
// GAME STATE
// --------------------------------------------------------------------------
typedef struct {
    int x, y;       // Position (Fixed point 24.8)
    int vx, vy;     // Velocity (Fixed point 24.8)
    int facing;     // 0 = Right, 1 = Left
    bool grounded;
    OBJ_ATTR *obj;  // Pointer to OAM entry
} Player;

Player p;
int cam_x = 0;
int scroll_x = 0;

// --------------------------------------------------------------------------
// FUNCTIONS
// --------------------------------------------------------------------------

void load_graphics() {
    // 1. Load Palette to OBJ palette mem and BG palette mem
    memcpy32(pal_obj_mem, kirby_pal, 8); // 8 words = 32 bytes = 16 colors
    memcpy32(pal_bg_mem, kirby_pal, 8);

    // 2. Load Sprite Tiles into OAM Tile Memory
    // We copy 4 tiles (16x16 sprite)
    memcpy32(&tile_mem[4][0], kirby_tiles, 32); 

    // 3. Load Ground Tile into BG Tile Memory (Charblock 0)
    // We put it at index 1 (index 0 is usually transparent/empty)
    memcpy32(&tile_mem[0][1], ground_tile, 8);
}

void setup_background() {
    // Setup BG0 control: Priority 0, Charblock 0, Screenblock 30, 16 colors, 32x32 size
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32;
    
    // Fill the bottom rows of Screenblock 30 with the ground tile
    SCR_ENTRY *map = se_mem[30];
    int i;
    for(i=0; i<1024; i++) {
        // Row 15, 16, 17 are floor
        int y_tile = i / 32;
        if(y_tile >= 15) {
            map[i] = 1; // Use tile index 1 (Ground)
        } else {
            map[i] = 0; // Empty
        }
    }
}

void init_player() {
    p.x = 40 << 8;
    p.y = 0;
    p.vx = 0;
    p.vy = 0;
    p.facing = 0;
    p.grounded = false;
    
    // Assign first OAM entry
    p.obj = &oam_mem[0];
    
    // Init Sprite Attributes
    // ATTR0: Y=0, 4bpp, Square shape
    // ATTR1: X=0, Size 16x16 (Medium)
    // ATTR2: Tile Index 0, Priority 0, Palette 0
    p.obj->attr0 = OBJ_Y(0) | ATTR0_COLOR_16 | ATTR0_SQUARE;
    p.obj->attr1 = OBJ_X(0) | ATTR1_SIZE_16;
    p.obj->attr2 = ATTR2_PALBANK(0) | 0; 
}

void update_physics() {
    // 1. Horizontal Movement (Acceleration/Friction)
    if (key_is_down(KEY_RIGHT)) {
        p.vx += WALK_ACCEL;
        p.facing = 0; // Face Right
    } else if (key_is_down(KEY_LEFT)) {
        p.vx -= WALK_ACCEL;
        p.facing = 1; // Face Left (Flip H)
    } else {
        // Friction
        if (p.vx > 0) p.vx -= FRICTION;
        if (p.vx < 0) p.vx += FRICTION;
        if (abs(p.vx) < FRICTION) p.vx = 0;
    }

    // Clamp speed
    if (p.vx > MAX_SPEED) p.vx = MAX_SPEED;
    if (p.vx < -MAX_SPEED) p.vx = -MAX_SPEED;

    // 2. Vertical Movement (Gravity & Jump)
    p.vy += GRAVITY;

    // Jump / Float Logic
    if (key_hit(KEY_A)) {
        if (p.grounded) {
            // Normal Jump
            p.vy = -JUMP_FORCE;
            p.grounded = false;
        } else {
            // Kirby Float (Mid-air jump)
            p.vy = -FLOAT_FORCE;
        }
    }

    // 3. Apply Velocity
    p.x += p.vx;
    p.y += p.vy;

    // 4. Floor Collision
    if (p.y > FLOOR_Y) {
        p.y = FLOOR_Y;
        p.vy = 0;
        p.grounded = true;
    }
    
    // Left wall collision (can't go back past 0)
    if (p.x < 0) {
        p.x = 0;
        p.vx = 0;
    }
}

void update_camera() {
    // Convert fixed point player X to integer
    int px = p.x >> 8;
    
    // Simple camera: Keep player in center-ish
    // If player is past 100px on screen, scroll map
    int screen_x = px - scroll_x;
    
    if (screen_x > 100) {
        scroll_x = px - 100;
    }
    
    // Update Hardware Scroll Register
    REG_BG0HOFS = scroll_x;
    
    // Update Sprite Position relative to camera
    // Mask with 0x1FF for 9-bit wrapping (hardware sprite requirement)
    int render_x = (px - scroll_x) & 0x1FF;
    int render_y = p.y >> 8;

    p.obj->attr0 = (p.obj->attr0 & ~ATTR0_Y_MASK) | OBJ_Y(render_y);
    p.obj->attr1 = (p.obj->attr1 & ~(ATTR1_X_MASK | ATTR1_HFLIP)) | OBJ_X(render_x);
    
    // Horizontal Flip based on facing
    if (p.facing) {
        p.obj->attr1 |= ATTR1_HFLIP;
    }
}

// --------------------------------------------------------------------------
// MAIN LOOP
// --------------------------------------------------------------------------

int main() {
    // Enable interrupts for VBlank (good practice for timing)
    irq_init(NULL);
    irq_enable(II_VBLANK);

    // Init Graphics
    load_graphics();
    setup_background();
    init_player();

    // Set Display Control: Mode 0, Enable BG0, Enable OBJ, 1D Obj mapping
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    while(1) {
        // Read keys
        key_poll();

        // Game Logic
        update_physics();
        update_camera();

        // Wait for VBlank (prevents tearing)
        vid_vsync();
        
        // Use tonc's OAM copy (puts our sprite struct into actual hardware memory)
        // We only have 1 sprite to update, so count is 1.
        // Note: In a real engine, you'd manage an OAM buffer.
        // Since we are writing directly to &oam_mem[0] via the pointer, 
        // we technically don't need a copy function, but sync is good.
    }

    return 0;
}
