// main.c - Simple 2D side scroller demo using tonc
// Controls: Left/Right to move, A or B to jump.

#include <tonc.h>

//------------------------------------------------------------------------------
// Fixed-point helpers
//------------------------------------------------------------------------------

#define FIX_SHIFT   8
#define INT2FIX(n)  ((n) << FIX_SHIFT)
#define FIX2INT(x)  ((x) >> FIX_SHIFT)

//------------------------------------------------------------------------------
// Game constants
//------------------------------------------------------------------------------

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  160

// World is 64 tiles wide (64 * 8 = 512 pixels)
#define WORLD_WIDTH    (64*8)

// Put the top of the ground a bit above the bottom of the screen
#define GROUND_Y       (SCREEN_HEIGHT - 32)

// Player parameters
#define PLAYER_W       16
#define PLAYER_H       16

#define MOVE_SPEED     INT2FIX(1)      // 1 pixel/frame
#define GRAVITY        (INT2FIX(1)/4)  // 0.25 px/frame^2
#define JUMP_VEL       (-INT2FIX(4))   // -4 px/frame
#define MAX_FALL_SPEED INT2FIX(4)

//------------------------------------------------------------------------------
// Player structure
//------------------------------------------------------------------------------

typedef struct {
    int x, y;       // fixed-point position
    int vx, vy;     // fixed-point velocity
    int w, h;
    int onGround;   // 1 if touching ground
} Player;

//------------------------------------------------------------------------------
// Globals
//------------------------------------------------------------------------------

OBJ_ATTR obj_buffer[128];
Player player;

//------------------------------------------------------------------------------
// Background setup: sky and ground tiles in a 64x32 map
//------------------------------------------------------------------------------

static void init_background(void)
{
    // Enable mode 0, BG0 and objects, 1D mapping for sprites
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    // BG palette: index 0 = sky, 1 = ground
    pal_bg_mem[0] = RGB15(8, 16, 31);   // light blue sky
    pal_bg_mem[1] = RGB15(6, 10, 0);    // darkish green ground

    // Tile 0: sky (all color 0)
    TILE *skyTile = &tile_mem[0][0];
    for(int i=0; i<8; i++)
        skyTile->data[i] = 0x00000000;

    // Tile 1: ground (all color 1)
    TILE *groundTile = &tile_mem[0][1];
    for(int i=0; i<8; i++)
        groundTile->data[i] = 0x11111111;

    // BG0: charblock 0, screenblock 28, 4bpp, 64x32 map
    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_4BPP | BG_REG_64x32;

    // Create a simple level:
    // - top half: sky (tile 0)
    // - bottom half: ground (tile 1)
    u16 *se_left  = se_mem[28]; // first 32x32 block (x = 0..31)
    u16 *se_right = se_mem[29]; // second 32x32 block (x = 32..63)

    for(int y=0; y<32; y++)
    {
        for(int x=0; x<64; x++)
        {
            u16 tid = (y >= 16) ? 1 : 0;   // ground below row 16
            if(x < 32)
                se_left[y*32 + x] = tid;
            else
                se_right[y*32 + (x-32)] = tid;
        }
    }

    // Initial scroll
    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
}

//------------------------------------------------------------------------------
// Sprite (player) setup: simple 16x16 pink square
//------------------------------------------------------------------------------

static void init_player_sprite(void)
{
    // Init OAM buffer (hides all sprites)
    oam_init(obj_buffer, 128);

    // Object palette: index 1 = pink
    pal_obj_mem[1] = RGB15(31, 15, 23); // pink

    // Create a 16x16 square using 4 tiles (4bpp) in OBJ VRAM block 4
    // Tiles 0..3 form a 2x2 block for 16x16 in 1D mapping.
    for(int t=0; t<4; t++)
    {
        TILE *tile = &tile_mem[4][t];
        for(int i=0; i<8; i++)
            tile->data[i] = 0x11111111;  // all pixels use color index 1
    }

    // Use obj_buffer[0] as our player sprite
    OBJ_ATTR *obj = &obj_buffer[0];

    // 16x16 square, 4bpp, regular object
    obj_set_attr(obj,
        ATTR0_SQUARE | ATTR0_4BPP | ATTR0_REG,
        ATTR1_SIZE_16,
        ATTR2_BUILD(0, 0, 0)); // tile index 0, prio 0, palbank 0

    // Initial on-screen position (will be updated in game loop)
    obj_set_pos(obj, SCREEN_WIDTH/2 - PLAYER_W/2, GROUND_Y - PLAYER_H);

    // Copy to OAM
    oam_copy(oam_mem, obj_buffer, 1);
}

//------------------------------------------------------------------------------
// Player initialization
//------------------------------------------------------------------------------

static void init_player(void)
{
    player.w = PLAYER_W;
    player.h = PLAYER_H;

    int start_x = 40;                 // world x
    int start_y = GROUND_Y - PLAYER_H;

    player.x = INT2FIX(start_x);
    player.y = INT2FIX(start_y);

    player.vx = 0;
    player.vy = 0;
    player.onGround = 1;
}

//------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------

int main(void)
{
    irq_init(NULL);
    irq_enable(II_VBLANK);

    init_background();
    init_player_sprite();
    init_player();

    int cameraX = 0;

    while(1)
    {
        // Wait for VBlank
        vid_vsync();
        key_poll();

        //----------------------------------------------------------------------------
        // Input: horizontal movement
        //----------------------------------------------------------------------------
        player.vx = 0;
        if(key_is_down(KEY_LEFT))
            player.vx = -MOVE_SPEED;
        if(key_is_down(KEY_RIGHT))
            player.vx =  MOVE_SPEED;

        // Jump
        if(player.onGround && (key_hit(KEY_A) || key_hit(KEY_B)))
        {
            player.vy = JUMP_VEL;
            player.onGround = 0;
        }

        //----------------------------------------------------------------------------
        // Physics
        //----------------------------------------------------------------------------
        // Gravity
        player.vy += GRAVITY;
        if(player.vy > MAX_FALL_SPEED)
            player.vy = MAX_FALL_SPEED;

        // Integrate position
        player.x += player.vx;
        player.y += player.vy;

        // Clamp horizontal position within world
        int px = FIX2INT(player.x);
        int py = FIX2INT(player.y);

        int maxX = WORLD_WIDTH - player.w;
        if(px < 0)      px = 0;
        if(px > maxX)   px = maxX;
        player.x = INT2FIX(px);

        // Ground collision (flat ground at GROUND_Y)
        if(py + player.h >= GROUND_Y)
        {
            py = GROUND_Y - player.h;
            player.y = INT2FIX(py);
            player.vy = 0;
            player.onGround = 1;
        }
        else
        {
            player.onGround = 0;
        }

        //----------------------------------------------------------------------------
        // Camera: follow player, clamp to level bounds
        //----------------------------------------------------------------------------
        cameraX = px + player.w/2 - SCREEN_WIDTH/2;
        if(cameraX < 0)
            cameraX = 0;
        if(cameraX > WORLD_WIDTH - SCREEN_WIDTH)
            cameraX = WORLD_WIDTH - SCREEN_WIDTH;

        REG_BG0HOFS = cameraX;

        //----------------------------------------------------------------------------
        // Update sprite position relative to camera
        //----------------------------------------------------------------------------
        OBJ_ATTR *obj = &obj_buffer[0];

        int sx = px - cameraX;
        int sy = py;     // no vertical scroll

        obj_set_pos(obj, sx, sy);
        oam_copy(oam_mem, obj_buffer, 1);
    }

    return 0;
}
