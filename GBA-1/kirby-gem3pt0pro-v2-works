#include <tonc.h>
#include <string.h>
#include <stdlib.h>

// --- Constants ---
#define MAP_WIDTH   64 // Map width in tiles (Screenblock size)
#define MAP_HEIGHT  32 // Map height in tiles
#define TILE_SIZE   8

// Fixed point math for smooth physics (8.8 fixed point)
#define FIXED_SHIFT 8
#define F(x)        ((x) << FIXED_SHIFT)
#define I(x)        ((x) >> FIXED_SHIFT)

// Physics constants
#define GRAVITY     F(1) / 4
#define JUMP_FORCE  F(3)
#define FLY_FORCE   F(1) / 2
#define MOVE_SPEED  F(1) + (F(1)/2)
#define FRICTION    F(1) / 8

// Asset IDs in VRAM
#define TID_EMPTY   0
#define TID_GROUND  1
#define TID_BLOCK   2
#define TID_KIRBY   512 // Sprite tile start index

// Map Data Buffer (Screenblock)
u16 map_data[MAP_WIDTH * MAP_HEIGHT];

// --- Structs ---
typedef struct {
    int x, y;       // Position (Fixed point)
    int vx, vy;     // Velocity (Fixed point)
    int width, height;
    int facing_right;
    OBJ_ATTR *obj;  // Pointer to OAM entry
} Player;

typedef struct {
    int x;          // Camera X position (Integer)
} Camera;

// --- Global State ---
Player kirby;
Camera cam;

// --- Graphics Generation ---

// Generate simple 8x8 tiles directly into VRAM
void load_generated_assets() {
    // 1. Palettes
    // Background Palette
    pal_bg_mem[0] = CLR_SKYBLUE;  // Transparent/Backdrop
    pal_bg_mem[1] = CLR_MAG;  // Debug/Error
    pal_bg_mem[2] = RGB15(5, 20, 5); // Ground Green
    pal_bg_mem[3] = RGB15(10, 10, 5); // Block Brown

    // Sprite Palette
    pal_obj_mem[0] = CLR_FUCHSIA; // Transparent key
    pal_obj_mem[1] = RGB15(31, 20, 20); // Kirby Pink
    pal_obj_mem[2] = RGB15(31, 0, 0);   // Kirby Red (Shoes)
    pal_obj_mem[3] = RGB15(0, 0, 0);    // Black (Eyes)

    // 2. Tile Data (4bpp)
    TILE *bg_tiles = (TILE*)tile_mem[0];
    TILE *obj_tiles = (TILE*)tile_mem[4];

    // -- BG Tiles --
    // Tile 0: Empty (Sky) - All 0
    memset(&bg_tiles[TID_EMPTY], 0, sizeof(TILE));

    // Tile 1: Ground (Solid color with texture)
    for(int i=0; i<8; i++) {
        for(int j=0; j<8; j++) {
            // Simple dithering pattern
            u32 color = ((i+j)%2 == 0) ? 2 : 2; 
            if(i==0) color = 2; // Top border
            bg_tiles[TID_GROUND].data[i] = (bg_tiles[TID_GROUND].data[i] & ~(0xF << (j*4))) | (color << (j*4));
        }
    }

    // Tile 2: Block (Bricks)
    for(int i=0; i<8; i++) {
        bg_tiles[TID_BLOCK].data[i] = 0x33333333;
        if(i == 0 || i == 4) bg_tiles[TID_BLOCK].data[i] = 0x00000000; // Mortar lines
    }

    // -- Sprite Tiles (Kirby Placeholder - 16x16) --
    // We need 4 tiles for a 16x16 sprite (Indices 0, 1, 32, 33 in 1D mapping or linear)
    // To keep it simple, let's draw a ball
    u32 kirby_color = 1;
    for(int t=0; t<4; t++) {
        for(int i=0; i<8; i++) {
            obj_tiles[t].data[i] = 0x11111111; 
        }
    }
    // Add simple eyes to tile 0 and 1
    obj_tiles[0].data[2] &= 0xFFF33FFF; // Left Eye
    obj_tiles[1].data[2] &= 0xFFF33FFF; // Right Eye
    
    // Red shoes (bottom tiles)
    for(int i=5; i<8; i++) {
        obj_tiles[2].data[i] = 0x22222222;
        obj_tiles[3].data[i] = 0x22222222;
    }
}

// Generate a level map
void init_map() {
    // Clear map
    for(int i=0; i<MAP_WIDTH*MAP_HEIGHT; i++) map_data[i] = TID_EMPTY;

    // Floor
    for(int x=0; x<MAP_WIDTH; x++) {
        map_data[18 * 32 + x] = TID_GROUND; // Row 18 (near bottom)
        map_data[19 * 32 + x] = TID_GROUND; 
    }

    // Random blocks and platforms
    for(int x=5; x<MAP_WIDTH-5; x+=4) {
        if(x % 3 == 0) {
            // Floating platform
            int y = 12;
            map_data[y * 32 + x] = TID_BLOCK;
            map_data[y * 32 + x+1] = TID_BLOCK;
        }
        if (x % 7 == 0) {
             // Wall
             map_data[17 * 32 + x] = TID_BLOCK;
             map_data[16 * 32 + x] = TID_BLOCK;
        }
    }

    // Copy map to VRAM Screenblock Base 30 (entry for BG0)
    // Note: Standard map is 32x32. We use 64x32 (Size 1), taking up 2 screenblocks (30 and 31).
    // libtonc's se_mem is useful here.
    u16 *sb0 = (u16*)se_mem[30];
    u16 *sb1 = (u16*)se_mem[31];

    for(int y=0; y<MAP_HEIGHT; y++) {
        for(int x=0; x<MAP_WIDTH; x++) {
            u16 tile_id = map_data[y*32 + x]; // Simplified access for logic
            
            // Write to hardware screenblocks
            // If x < 32, write to SB 30. If x >= 32, write to SB 31 (at x-32).
            if(x < 32) {
                sb0[y*32 + x] = tile_id;
            } else {
                sb1[y*32 + (x-32)] = tile_id;
            }
        }
    }
}

// --- Logic ---

// Get tile ID at specific world coordinates
u16 get_tile_at(int x, int y) {
    int tx = x / TILE_SIZE;
    int ty = y / TILE_SIZE;
    
    if(tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) return TID_EMPTY;
    
    // Map data array is stored as one continuous block in logic, 
    // though split in VRAM. We read from our RAM buffer.
    // Our map buffer logic above was simplified: map_data is actually just a 1D array.
    // But since we filled map_data using y*32 logic, we access it similarly.
    // Wait, typical map logic for 64 wide is contiguous. 
    // Let's just fix the map buffer access:
    // Actually, for simplicity, let's read directly from VRAM to be safe regarding what we drew.
    
    if(tx < 32) {
        return se_mem[30][ty*32 + tx] & 0x03FF;
    } else {
        return se_mem[31][ty*32 + (tx-32)] & 0x03FF;
    }
}

int is_solid(int tile) {
    return (tile == TID_GROUND || tile == TID_BLOCK);
}

void init_game() {
    // Enable BG0 and OBJ
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // Setup BG0
    // CBB: 0 (Char Base Block - where tiles are)
    // SBB: 30 (Screen Base Block - where map is)
    // Size: 1 (64x32 tiles)
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_8BPP | BG_REG_64x32; 
    // Wait, I used 4bpp in generated assets. Change BG_8BPP to BG_4BPP (default 0)
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_REG_64x32;

    load_generated_assets();
    init_map();

    // Init Player
    kirby.x = F(30);
    kirby.y = F(50);
    kirby.vx = 0;
    kirby.vy = 0;
    kirby.width = 14;  // Slightly smaller than 16 for better feel
    kirby.height = 14;
    kirby.facing_right = 1;

    // Point to the first OAM entry
    kirby.obj = &oam_mem[0];
    
    // Init Sprite visual
    obj_set_attr(kirby.obj, 
        ATTR0_SQUARE | ATTR0_4BPP, // Shape, Colors
        ATTR1_SIZE_16,             // Size
        ATTR2_PALBANK(0) | 0       // Palette 0, Tile Index 0
    );

    cam.x = 0;
}

void update_physics() {
    // 1. Input -> Velocity
    // Horizontal
    if (key_is_down(KEY_LEFT)) {
        kirby.vx = -MOVE_SPEED;
        kirby.facing_right = 0;
    } else if (key_is_down(KEY_RIGHT)) {
        kirby.vx = MOVE_SPEED;
        kirby.facing_right = 1;
    } else {
        // Simple friction
        if (kirby.vx > 0) kirby.vx -= FRICTION;
        if (kirby.vx < 0) kirby.vx += FRICTION;
        if (abs(kirby.vx) < FRICTION) kirby.vx = 0;
    }

    // Vertical (Gravity)
    kirby.vy += GRAVITY;

    // Jumping / Floating
    if (key_hit(KEY_A)) {
        // Check if grounded to jump, OR allow floating if in air
        // For Kirby style: always apply upward force, but capped
        kirby.vy = -JUMP_FORCE; 
    }

    // --- Collision ---
    
    // Horizontal Collision
    int next_x = kirby.x + kirby.vx;
    int pixel_x = I(next_x);
    int pixel_y = I(kirby.y);
    
    // Check Left/Right edges
    if (kirby.vx < 0) { // Moving Left
        if (is_solid(get_tile_at(pixel_x, pixel_y)) || 
            is_solid(get_tile_at(pixel_x, pixel_y + kirby.height))) {
            kirby.vx = 0;
            next_x = kirby.x; // Cancel move
        }
    } else if (kirby.vx > 0) { // Moving Right
        if (is_solid(get_tile_at(pixel_x + kirby.width, pixel_y)) || 
            is_solid(get_tile_at(pixel_x + kirby.width, pixel_y + kirby.height))) {
            kirby.vx = 0;
            next_x = kirby.x;
        }
    }
    kirby.x = next_x;

    // Vertical Collision
    int next_y = kirby.y + kirby.vy;
    pixel_x = I(kirby.x);
    int check_y_top = I(next_y);
    int check_y_bottom = I(next_y) + kirby.height;

    if (kirby.vy < 0) { // Moving Up
        if (is_solid(get_tile_at(pixel_x + 2, check_y_top)) || 
            is_solid(get_tile_at(pixel_x + kirby.width - 2, check_y_top))) {
            kirby.vy = 0;
            next_y = kirby.y;
        }
    } else if (kirby.vy > 0) { // Moving Down
        if (is_solid(get_tile_at(pixel_x + 2, check_y_bottom)) || 
            is_solid(get_tile_at(pixel_x + kirby.width - 2, check_y_bottom))) {
            kirby.vy = 0;
            // Snap to grid
            next_y = F((check_y_bottom / 8) * 8 - kirby.height - 1); 
            // Wait, simple snap:
            next_y = F(check_y_bottom / 8 * 8 - kirby.height);
            // This is "Grounded"
        }
    }
    
    // Floor boundary
    if (I(next_y) > (MAP_HEIGHT * 8)) {
        kirby.y = 0; // Respawn top
        kirby.vy = 0;
    } else {
        kirby.y = next_y;
    }
}

void update_camera() {
    int px = I(kirby.x);
    
    // Try to keep player in center
    int target_cam_x = px - (SCREEN_WIDTH / 2);
    
    // Clamping
    if (target_cam_x < 0) target_cam_x = 0;
    if (target_cam_x > (MAP_WIDTH * 8) - SCREEN_WIDTH) target_cam_x = (MAP_WIDTH * 8) - SCREEN_WIDTH;

    cam.x = target_cam_x;

    // Update Hardware Registers
    REG_BG0HOFS = cam.x;
}

void update_visuals() {
    // Screen coords
    int sx = I(kirby.x) - cam.x;
    int sy = I(kirby.y);

    // Update OAM
    obj_set_pos(kirby.obj, sx, sy);
    
    // Flip sprite if facing left
    if (!kirby.facing_right) {
        kirby.obj->attr1 |= ATTR1_HFLIP;
    } else {
        kirby.obj->attr1 &= ~ATTR1_HFLIP;
    }

    // Simple Animation: Squish when jumping
    // We only have one frame, so we change nothing here, 
    // but this is where you'd change tile index based on vy.
}

// --- Main ---

int main() {
    init_game();

    while(1) {
        // VSync
        vid_vsync();
        
        // Input
        key_poll();

        // Game Logic
        update_physics();
        update_camera();
        
        // Render
        update_visuals();
        
        // Flush OAM cache to VRAM
        oam_copy(oam_mem, oam_mem, 1);
    }

    return 0;
}
