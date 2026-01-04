#include <tonc.h>

// ===========================================================================
// 1. GRAPHICS DATA (EMBEDDED)
// ===========================================================================

#define RGB15_C(r,g,b)  ((r) | ((g) << 5) | ((b) << 10))

// A simple 16-color palette
// Color 0: Transparent/Backdrop
// Color 1: Dark Green (Grass)
// Color 2: Light Green (Grass Highlight)
// Color 3: Skin Tone
// Color 4: Tunic Green
// Color 5: Brown (Boots/Belt)
// Color 6: Silver (Sword)
const unsigned short game_palette[] = {
    RGB15_C(0,0,0),    RGB15_C(0,15,0),   RGB15_C(5,20,5),   RGB15_C(31,25,15), 
    RGB15_C(0,31,0),   RGB15_C(15,10,0),  RGB15_C(25,25,25), RGB15_C(31,31,31),
    0,0,0,0,0,0,0,0 // Padding
};

// Tile: Grass (8x8 pixels, 4bpp)
// Represented as integers where each hex digit is a pixel color index
const unsigned int tile_grass[8] = {
    0x11111111, 0x12111121, 0x11111111, 0x11121111,
    0x11111111, 0x11211211, 0x11111111, 0x12111111
};

// Meta-Tile: Player (16x16 pixels -> 4 tiles of 8x8)
// Top-Left, Top-Right, Bottom-Left, Bottom-Right
const unsigned int tile_link[32] = {
    // Tile 0: Head Left
    0x00044400, 0x00444440, 0x04444444, 0x04444444,
    0x04444444, 0x00333333, 0x00333303, 0x00033300,
    // Tile 1: Head Right
    0x00444000, 0x04444400, 0x44444440, 0x44444440,
    0x44444440, 0x33333300, 0x30333300, 0x00333000,
    // Tile 2: Body Left
    0x00044400, 0x00444440, 0x00055500, 0x00044400,
    0x00044400, 0x00444440, 0x00440440, 0x00550550,
    // Tile 3: Body Right
    0x00444000, 0x04444400, 0x00555000, 0x00444000,
    0x00444000, 0x04444400, 0x04404400, 0x05505500
};

// Meta-Tile: Player Attacking (Sword out)
const unsigned int tile_link_atk[32] = {
    // Tile 0: Head Left
    0x00044400, 0x00444440, 0x04444444, 0x04444444,
    0x04444444, 0x00333333, 0x00333303, 0x00033300,
    // Tile 1: Head Right (Sword Hand)
    0x00444000, 0x04444400, 0x44444440, 0x44444440,
    0x44444440, 0x33333300, 0x30333366, 0x00333066,
    // Tile 2: Body Left
    0x00044400, 0x00444440, 0x00055500, 0x00044400,
    0x00044400, 0x00444440, 0x00440440, 0x00550550,
    // Tile 3: Body Right (Sword Tip)
    0x00444000, 0x04444400, 0x00555066, 0x00444066,
    0x00444000, 0x04444400, 0x04404400, 0x05505500
};

// ===========================================================================
// 2. GAME CONSTANTS & STRUCTS
// ===========================================================================

#define PLAYER_SPEED 2
#define ATK_DURATION 15

typedef enum {
    STATE_IDLE,
    STATE_WALK,
    STATE_ATTACK
} State;

typedef struct {
    int x, y;
    int dx, dy;
    State state;
    int facing_right; // 1 = Right, 0 = Left
    int timer;        // For attack duration
} Player;

OBJ_ATTR obj_buffer[128]; // OAM Buffer
Player link;

// ===========================================================================
// 3. INITIALIZATION
// ===========================================================================

void init_graphics() {
    // 1. Load Palette (Background and Sprite)
    memcpy32(pal_bg_mem, game_palette, sizeof(game_palette)/4);
    memcpy32(pal_obj_mem, game_palette, sizeof(game_palette)/4);

    // 2. Load Tiles into VRAM
    // Background tiles go to Charblock 0
    memcpy32(&tile_mem[0][1], tile_grass, sizeof(tile_grass)/4);
    
    // Sprite tiles go to Charblock 4
    // We load Normal Link at index 0, Attack Link at index 4
    memcpy32(&tile_mem[4][0], tile_link, sizeof(tile_link)/4);
    memcpy32(&tile_mem[4][4], tile_link_atk, sizeof(tile_link_atk)/4);

    // 3. Create Background Map (Screenblock 30)
    // Fill the screen (32x32 tiles) with the grass tile (Index 1)
    u16 *map_ptr = se_mem[30];
    for(int i=0; i<32*32; i++) {
        map_ptr[i] = 1; // Use tile index 1
    }

    // 4. Initialize OAM
    oam_init(obj_buffer, 128);
}

void init_game() {
    link.x = 104; // Center X (approx)
    link.y = 64;  // Center Y (approx)
    link.dx = 0;
    link.dy = 0;
    link.state = STATE_IDLE;
    link.facing_right = 1;
    link.timer = 0;
}

// ===========================================================================
// 4. MAIN LOOP LOGIC
// ===========================================================================

void update() {
    // Handle Input
    key_poll();

    // Attack Logic
    if(link.state == STATE_ATTACK) {
        link.timer--;
        if(link.timer <= 0) {
            link.state = STATE_IDLE;
        }
    } 
    else {
        // Movement Input
        link.dx = key_tri_horz() * PLAYER_SPEED;
        link.dy = key_tri_vert() * PLAYER_SPEED;

        // Start Attack
        if(key_hit(KEY_A)) {
            link.state = STATE_ATTACK;
            link.timer = ATK_DURATION;
            link.dx = 0; // Stop moving when attacking
            link.dy = 0;
        }

        // Determine Facing
        if(link.dx > 0) link.facing_right = 1;
        if(link.dx < 0) link.facing_right = 0;

        // Apply Movement
        link.x += link.dx;
        link.y += link.dy;

        // Screen Boundaries (0 to 240 width, 0 to 160 height)
        // Sprite is 16x16
        if(link.x < 0) link.x = 0;
        if(link.x > 240 - 16) link.x = 240 - 16;
        if(link.y < 0) link.y = 0;
        if(link.y > 160 - 16) link.y = 160 - 16;
    }
}

void draw() {
    OBJ_ATTR *obj = &obj_buffer[0];

    // Configure Sprite Attribute
    u16 tid = 0; // Tile ID
    u16 attr1_flips = 0;

    // Handle Animation Frame
    if(link.state == STATE_ATTACK) {
        tid = 4; // Start at tile index 4 for attack gfx
    } else {
        tid = 0; // Standard walk gfx
    }

    // Handle flipping (Sprite Memory is optimized for one direction)
    // If facing left, we flip horizontally
    if(!link.facing_right) {
        attr1_flips = ATTR1_HFLIP;
    }

    // Set OAM attributes
    obj_set_attr(obj, 
        ATTR0_SQUARE | ATTR0_Y(link.y),      // Shape and Y pos
        ATTR1_SIZE_16 | ATTR1_X(link.x) | attr1_flips, // Size and X pos
        ATTR2_PALBANK(0) | tid               // Palette 0, Tile Index
    );

    // Copy buffer to actual OAM memory
    oam_copy(oam_mem, obj_buffer, 1);
}

// ===========================================================================
// 5. ENTRY POINT
// ===========================================================================

int main() {
    // Enable VBlank Interrupt (Good practice for vsync)
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    // Setup Video Mode
    // Mode 0: Tiled Backgrounds + Sprites
    // BG0: Active
    // OBJ: Active, 1D Mapping (Standard)
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // Setup Background Control
    // Size: 32x32 tiles (256x256 pixels)
    // Charblock: 0 (Where tiles are stored)
    // Screenblock: 30 (Where map is stored)
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32;

    init_graphics();
    init_game();

    while(1) {
        vid_vsync(); // Wait for Vertical Blank (60 FPS cap)
        update();
        draw();
    }

    return 0;
}
