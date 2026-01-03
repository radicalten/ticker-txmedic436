#include <tonc.h>

// --- Player ---
typedef struct {
    int x, y;           // world position (fixed point 8.8)
    int dx, dy;         // velocity
    OBJ_ATTR *obj;      // pointer to OAM entry
    int grounded;
} Player;

Player player;

// --- Background ---
#define BG_WIDTH  512
#define BG_HEIGHT 256
u16 bg_data[BG_WIDTH * BG_HEIGHT / 2] __attribute__((aligned(4)));
u16 tile_data[16 * 16] __attribute__((aligned(4))); // 16-color palette-based tiles

// --- Tile/Map Helpers ---
void fill_tile(u16 *tile, u16 color) {
    for(int i = 0; i < 8*8; i++) tile[i] = color;
}

void draw_bg_tile(int tx, int ty, u16 color) {
    int base = (ty * BG_WIDTH + tx) * 8 * 8;
    for(int y = 0; y < 8; y++) {
        for(int x = 0; x < 8; x++) {
            bg_data[(ty*8+y)*BG_WIDTH + (tx*8+x)] = color;
        }
    }
}

// --- Init Background ---
void init_bg() {
    // Create simple gradient sky and ground
    for(int y = 0; y < BG_HEIGHT; y++) {
        u16 color = (y < 128) ? RGB5(135, 206, 235) : RGB5(34, 139, 34); // sky / grass
        for(int x = 0; x < BG_WIDTH; x++) {
            bg_data[y * BG_WIDTH + x] = color;
        }
    }

    // Draw "hills" and platforms
    for(int x = 0; x < BG_WIDTH; x += 32) {
        int h = 32 + (x/16)%16;
        for(int y = BG_HEIGHT - h; y < BG_HEIGHT; y++) {
            bg_data[y * BG_WIDTH + x] = RGB5(101, 67, 33); // brown dirt
        }
    }

    // Setup background control
    REG_BG2CNT = BG_CBB(0) | BG_SBB(30) | BG_8BPP | BG_REG_32x32;
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    
    // Copy data to VRAM
    memcpy(&tile_mem[0][0], bg_data, sizeof(bg_data));
    
    // Identity map (each 8x8 tile maps to sequential tile index)
    u16 *map = &se_mem[30][0];
    for(int i = 0; i < 32*32; i++) map[i] = i;
}

// --- Init Player Sprite ---
void init_player() {
    player.x = 64 << 8;
    player.y = (SCREEN_HEIGHT - 32) << 8;
    player.dx = 0;
    player.dy = 0;
    player.grounded = 1;

    // Create a simple circular sprite (4bpp 32x32)
    OBJ_ATTR *oam = &player.obj;
    oam_init(oam, 1);
    obj_set_attr(oam, ATTR0_SQUARE, ATTR1_SIZE_32, ATTR2_PRIO(0));
    obj_set_pos(oam, 64, SCREEN_HEIGHT - 32);

    // Create sprite graphics: white circle on pink
    u16 *gfx = &tile_mem[4][0];
    for(int y = 0; y < 32; y++) {
        for(int x = 0; x < 32; x++) {
            int dx = x - 16, dy = y - 16;
            if(dx*dx + dy*dy <= 225) { // radius ~15
                gfx[y*32 + x] = 1; // white
            } else {
                gfx[y*32 + x] = 0; // transparent
            }
        }
    }

    // Set palette
    pal_obj_bank[0][0] = RGB5(0,0,0);     // transparent
    pal_obj_bank[0][1] = RGB5(255,255,255); // white body
    pal_obj_bank[0][2] = RGB5(255,105,180); // pink (unused here)

    obj_set_attr(oam, ATTR0_SQUARE | ATTR0_COLOR_4BPP, ATTR1_SIZE_32, ATTR2_ID(0) | ATTR2_PRIO(0));
}

// --- Game Logic ---
void update_player() {
    static int camera_x = 0;

    key_poll();
    
    // Movement
    if(key_is_down(KEY_LEFT))  player.dx = -128;
    else if(key_is_down(KEY_RIGHT)) player.dx = 128;
    else player.dx = 0;

    // Jump
    if(key_hit(KEY_A) && player.grounded) {
        player.dy = -768; // upward impulse
        player.grounded = 0;
    }

    // Apply gravity
    if(!player.grounded) {
        player.dy += 32; // gravity
    }

    // Update position
    player.x += player.dx;
    player.y += player.dy;

    // World bounds
    if(player.x < 0) player.x = 0;
    if(player.x > ((BG_WIDTH - 32) << 8)) player.x = (BG_WIDTH - 32) << 8;

    // Ground collision
    int py = player.y >> 8;
    if(py >= SCREEN_HEIGHT - 32 - 1) {
        player.y = (SCREEN_HEIGHT - 32 - 1) << 8;
        player.dy = 0;
        player.grounded = 1;
    }

    // Camera follows player
    camera_x = (player.x >> 8) - SCREEN_WIDTH/2;
    if(camera_x < 0) camera_x = 0;
    if(camera_x > BG_WIDTH - SCREEN_WIDTH) camera_x = BG_WIDTH - SCREEN_WIDTH;

    // Update sprite position (screen space)
    int screen_x = (player.x >> 8) - camera_x;
    int screen_y = player.y >> 8;
    obj_set_pos(player.obj, screen_x, screen_y);

    // Update background scroll
    REG_BG2HOFS = camera_x;
}

// --- Main ---
int main(void) {
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);
    irq_enable(II_VBLANK);

    videoInit();
    init_bg();
    init_player();

    while(1) {
        VBlankIntrWait();
        key_poll();
        update_player();
        oam_copy(oam_mem, oam_buffer, 1);
    }

    return 0;
}
