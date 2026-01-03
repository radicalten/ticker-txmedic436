//======================================================================
// OutRun-style Racing Game for GBA
// Uses TONC library - Mode 4 bitmap graphics
// Compile with: make (with standard GBA makefile)
//======================================================================

#include <tonc.h>
#include <string.h>

//----------------------------------------------------------------------
// Constants
//----------------------------------------------------------------------
#define SCREEN_W        240
#define SCREEN_H        160
#define HORIZON         70
#define ROAD_SEGMENTS   300
#define SEG_LENGTH      200
#define ROAD_WIDTH      2000
#define RUMBLE_LEN      3
#define DRAW_DIST       100
#define CAM_HEIGHT      1000
#define CAM_DEPTH       120

// Fixed point (16.16)
#define FP_BITS         16
#define FP_ONE          (1 << FP_BITS)
#define FP_HALF         (FP_ONE >> 1)
#define TO_FP(x)        ((x) << FP_BITS)
#define FROM_FP(x)      ((x) >> FP_BITS)
#define FP_MUL(a,b)     ((((s64)(a)) * (b)) >> FP_BITS)
#define FP_DIV(a,b)     ((((s64)(a)) << FP_BITS) / (b))

// Palette indices
enum {
    COL_BLACK = 0,
    COL_SKY,
    COL_HORIZON,
    COL_GRASS1,
    COL_GRASS2,
    COL_ROAD1,
    COL_ROAD2,
    COL_RUMBLE1,
    COL_RUMBLE2,
    COL_LINE,
    COL_CAR_BODY,
    COL_CAR_WINDOW,
    COL_CAR_WHEEL,
    COL_TREE_TRUNK,
    COL_TREE_LEAVES,
    COL_WHITE
};

//----------------------------------------------------------------------
// Road Segment
//----------------------------------------------------------------------
typedef struct {
    s32 curve;              // Curve amount
    s32 hill;               // Hill amount (y delta)
    s32 y;                  // World y position
    u8  sprite_type;        // 0=none, 1=tree left, 2=tree right, 3=both
} Segment;

//----------------------------------------------------------------------
// Game State
//----------------------------------------------------------------------
static Segment  road[ROAD_SEGMENTS];
static s32      player_x;           // Player x offset (fixed point)
static s32      player_z;           // Distance traveled
static s32      player_speed;       // Current speed
static s32      max_speed;          // Maximum speed
static s32      accel;              // Acceleration
static s32      decel;              // Deceleration (natural)
static s32      brake_power;        // Braking force
static s32      offroad_limit;      // Speed limit when off road
static s32      centrifugal;        // Centrifugal force factor
static u32      frame_count;

//----------------------------------------------------------------------
// Z-buffer for sprites (simple version)
//----------------------------------------------------------------------
static s32      sprite_z[SCREEN_H];
static s32      sprite_scale[SCREEN_H];
static s32      sprite_x[SCREEN_H];

//----------------------------------------------------------------------
// Initialize palette
//----------------------------------------------------------------------
void init_palette(void)
{
    pal_bg_mem[COL_BLACK]      = RGB15(0, 0, 0);
    pal_bg_mem[COL_SKY]        = RGB15(8, 16, 28);
    pal_bg_mem[COL_HORIZON]    = RGB15(20, 24, 31);
    pal_bg_mem[COL_GRASS1]     = RGB15(0, 18, 0);
    pal_bg_mem[COL_GRASS2]     = RGB15(0, 22, 0);
    pal_bg_mem[COL_ROAD1]      = RGB15(6, 6, 6);
    pal_bg_mem[COL_ROAD2]      = RGB15(7, 7, 7);
    pal_bg_mem[COL_RUMBLE1]    = RGB15(31, 0, 0);
    pal_bg_mem[COL_RUMBLE2]    = RGB15(31, 31, 31);
    pal_bg_mem[COL_LINE]       = RGB15(31, 31, 31);
    pal_bg_mem[COL_CAR_BODY]   = RGB15(31, 8, 0);
    pal_bg_mem[COL_CAR_WINDOW] = RGB15(10, 16, 24);
    pal_bg_mem[COL_CAR_WHEEL]  = RGB15(3, 3, 3);
    pal_bg_mem[COL_TREE_TRUNK] = RGB15(12, 6, 2);
    pal_bg_mem[COL_TREE_LEAVES]= RGB15(0, 14, 0);
    pal_bg_mem[COL_WHITE]      = RGB15(31, 31, 31);
}

//----------------------------------------------------------------------
// Initialize road
//----------------------------------------------------------------------
void init_road(void)
{
    s32 y = 0;
    
    for (int i = 0; i < ROAD_SEGMENTS; i++) {
        road[i].curve = 0;
        road[i].hill = 0;
        road[i].y = y;
        road[i].sprite_type = 0;
        
        // Create varied terrain
        // Straight section
        if (i >= 10 && i < 30) {
            road[i].curve = 0;
        }
        // Right curve
        else if (i >= 30 && i < 60) {
            road[i].curve = 8;
        }
        // Straight
        else if (i >= 60 && i < 80) {
            road[i].curve = 0;
        }
        // Left curve
        else if (i >= 80 && i < 120) {
            road[i].curve = -6;
        }
        // S-curve
        else if (i >= 120 && i < 150) {
            road[i].curve = 10;
        }
        else if (i >= 150 && i < 180) {
            road[i].curve = -10;
        }
        // Hills
        else if (i >= 180 && i < 200) {
            road[i].hill = 20;
        }
        else if (i >= 200 && i < 220) {
            road[i].hill = -20;
        }
        // Sharp turns
        else if (i >= 240 && i < 280) {
            road[i].curve = 15;
        }
        
        y += road[i].hill;
        
        // Add roadside trees
        if ((i % 10) == 0) {
            road[i].sprite_type = 1;  // Left tree
        }
        if ((i % 10) == 5) {
            road[i].sprite_type = 2;  // Right tree
        }
        if ((i % 25) == 0) {
            road[i].sprite_type = 3;  // Both sides
        }
    }
}

//----------------------------------------------------------------------
// Initialize game state
//----------------------------------------------------------------------
void init_game(void)
{
    player_x = 0;
    player_z = 0;
    player_speed = 0;
    max_speed = TO_FP(200);
    accel = TO_FP(1);
    decel = FP_ONE / 4;
    brake_power = TO_FP(2);
    offroad_limit = TO_FP(50);
    centrifugal = FP_ONE / 3;
    frame_count = 0;
}

//----------------------------------------------------------------------
// Fast horizontal line (Mode 4)
//----------------------------------------------------------------------
IWRAM_CODE
void hline_fast(int y, int x1, int x2, u8 color)
{
    if (x1 > x2) {
        int t = x1; x1 = x2; x2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x2 >= SCREEN_W) x2 = SCREEN_W - 1;
    if (x1 > x2) return;
    
    u16 *base = (u16*)vid_mem;
    int start = y * SCREEN_W + x1;
    int end = y * SCREEN_W + x2;
    
    u16 color16 = color | (color << 8);
    
    // Align to 16-bit boundary
    if (start & 1) {
        u16 *p = &base[start >> 1];
        *p = (*p & 0x00FF) | (color << 8);
        start++;
    }
    
    // Fill 16-bit aligned section
    u16 *dst = &base[start >> 1];
    int count = (end - start + 1) >> 1;
    
    while (count >= 8) {
        dst[0] = color16;
        dst[1] = color16;
        dst[2] = color16;
        dst[3] = color16;
        dst[4] = color16;
        dst[5] = color16;
        dst[6] = color16;
        dst[7] = color16;
        dst += 8;
        count -= 8;
    }
    while (count--) {
        *dst++ = color16;
    }
    
    // Handle odd end
    if (!((end - start + 1) & 1) && ((end + 1) & 1)) {
        u16 *p = &base[end >> 1];
        *p = (*p & 0xFF00) | color;
    }
}

//----------------------------------------------------------------------
// Put pixel (Mode 4)
//----------------------------------------------------------------------
IWRAM_CODE
static inline void put_pixel(int x, int y, u8 color)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    
    u16 *dst = &((u16*)vid_mem)[(y * SCREEN_W + x) >> 1];
    if (x & 1) {
        *dst = (*dst & 0x00FF) | (color << 8);
    } else {
        *dst = (*dst & 0xFF00) | color;
    }
}

//----------------------------------------------------------------------
// Draw filled rectangle
//----------------------------------------------------------------------
void fill_rect(int x, int y, int w, int h, u8 color)
{
    for (int j = 0; j < h; j++) {
        if (y + j >= 0 && y + j < SCREEN_H) {
            hline_fast(y + j, x, x + w - 1, color);
        }
    }
}

//----------------------------------------------------------------------
// Draw a simple tree sprite
//----------------------------------------------------------------------
void draw_tree(int screen_x, int screen_y, int scale)
{
    if (scale <= 0) return;
    
    int trunk_w = (4 * scale) >> 8;
    int trunk_h = (20 * scale) >> 8;
    int leaves_w = (16 * scale) >> 8;
    int leaves_h = (24 * scale) >> 8;
    
    if (trunk_w < 1) trunk_w = 1;
    if (trunk_h < 1) trunk_h = 1;
    if (leaves_w < 2) leaves_w = 2;
    if (leaves_h < 2) leaves_h = 2;
    
    int base_y = screen_y;
    int trunk_x = screen_x - trunk_w / 2;
    int trunk_y = base_y - trunk_h;
    
    // Draw trunk
    for (int dy = 0; dy < trunk_h; dy++) {
        int py = trunk_y + dy;
        if (py >= HORIZON && py < SCREEN_H) {
            for (int dx = 0; dx < trunk_w; dx++) {
                put_pixel(trunk_x + dx, py, COL_TREE_TRUNK);
            }
        }
    }
    
    // Draw leaves (triangle-ish shape)
    int leaves_y = trunk_y - leaves_h;
    for (int dy = 0; dy < leaves_h; dy++) {
        int py = leaves_y + dy;
        if (py >= 0 && py < SCREEN_H) {
            int row_width = leaves_w * (leaves_h - dy) / leaves_h;
            int lx = screen_x - row_width / 2;
            for (int dx = 0; dx < row_width; dx++) {
                put_pixel(lx + dx, py, COL_TREE_LEAVES);
            }
        }
    }
}

//----------------------------------------------------------------------
// Render the road
//----------------------------------------------------------------------
IWRAM_CODE
void render_road(void)
{
    s32 base_seg = player_z / SEG_LENGTH;
    s32 base_offset = player_z % SEG_LENGTH;
    
    s32 x_offset = 0;
    s32 dx = 0;
    s32 y_offset = 0;
    s32 dy = 0;
    
    // Get current segment's curve for camera
    int cur_seg = base_seg % ROAD_SEGMENTS;
    s32 cam_curve = road[cur_seg].curve;
    
    // Clear sky with gradient
    for (int y = 0; y < HORIZON; y++) {
        u8 sky_col = (y < 30) ? COL_SKY : COL_HORIZON;
        hline_fast(y, 0, SCREEN_W - 1, sky_col);
    }
    
    // Render road from horizon to bottom
    for (int y = HORIZON; y < SCREEN_H; y++) {
        // Perspective calculation
        s32 perspective = y - HORIZON;
        if (perspective < 1) perspective = 1;
        
        // Z depth for this scanline
        s32 z = (CAM_HEIGHT * CAM_DEPTH) / perspective;
        
        // Which segment are we looking at?
        s32 seg_z = base_offset + z;
        s32 seg_index = (base_seg + seg_z / SEG_LENGTH) % ROAD_SEGMENTS;
        
        Segment *seg = &road[seg_index];
        
        // Accumulate curve and hill
        dx += seg->curve;
        x_offset += dx;
        dy += seg->hill;
        y_offset += dy;
        
        // Project road width
        s32 proj_w = (ROAD_WIDTH * CAM_DEPTH) / (z + 1);
        if (proj_w < 4) proj_w = 4;
        
        // Road center with perspective curve
        s32 center_x = SCREEN_W / 2;
        center_x += (x_offset >> 6);                    // Curve offset
        center_x -= FROM_FP(FP_MUL(player_x, proj_w));  // Player offset
        
        s32 road_l = center_x - proj_w / 2;
        s32 road_r = center_x + proj_w / 2;
        
        // Rumble strip width
        s32 rumble_w = proj_w / 10;
        if (rumble_w < 2) rumble_w = 2;
        
        // Alternating colors based on segment index
        int alt = (seg_index / RUMBLE_LEN) & 1;
        
        u8 grass_col  = alt ? COL_GRASS1 : COL_GRASS2;
        u8 road_col   = alt ? COL_ROAD1 : COL_ROAD2;
        u8 rumble_col = alt ? COL_RUMBLE1 : COL_RUMBLE2;
        
        // Draw scanline
        // Left grass
        hline_fast(y, 0, road_l - rumble_w - 1, grass_col);
        // Left rumble
        hline_fast(y, road_l - rumble_w, road_l - 1, rumble_col);
        // Road
        hline_fast(y, road_l, road_r, road_col);
        // Right rumble  
        hline_fast(y, road_r + 1, road_r + rumble_w, rumble_col);
        // Right grass
        hline_fast(y, road_r + rumble_w + 1, SCREEN_W - 1, grass_col);
        
        // Center line (dashed)
        if (!alt && proj_w > 30) {
            s32 line_w = proj_w / 30;
            if (line_w < 1) line_w = 1;
            if (line_w > 4) line_w = 4;
            hline_fast(y, center_x - line_w, center_x + line_w, COL_LINE);
        }
        
        // Store info for sprites
        sprite_z[y] = z;
        sprite_scale[y] = (256 * CAM_DEPTH) / (z + 1);
        sprite_x[y] = center_x;
    }
    
    // Draw roadside sprites (back to front)
    for (int y = HORIZON + 1; y < SCREEN_H; y++) {
        s32 z = sprite_z[y];
        s32 seg_z = base_offset + z;
        s32 seg_index = (base_seg + seg_z / SEG_LENGTH) % ROAD_SEGMENTS;
        
        Segment *seg = &road[seg_index];
        
        if (seg->sprite_type && (seg_z % SEG_LENGTH) < 50) {
            s32 scale = sprite_scale[y];
            s32 proj_w = (ROAD_WIDTH * CAM_DEPTH) / (z + 1);
            
            // Tree offset from road
            s32 tree_offset = proj_w / 2 + (proj_w / 4);
            
            if (seg->sprite_type == 1 || seg->sprite_type == 3) {
                // Left tree
                int tx = sprite_x[y] - tree_offset;
                draw_tree(tx, y, scale);
            }
            if (seg->sprite_type == 2 || seg->sprite_type == 3) {
                // Right tree
                int tx = sprite_x[y] + tree_offset;
                draw_tree(tx, y, scale);
            }
        }
    }
}

//----------------------------------------------------------------------
// Draw player car
//----------------------------------------------------------------------
void draw_player_car(void)
{
    // Car dimensions
    const int CAR_W = 40;
    const int CAR_H = 24;
    const int CAR_Y = SCREEN_H - CAR_H - 6;
    
    // Car position (centered, slight x offset based on steering)
    int car_x = SCREEN_W / 2 - CAR_W / 2;
    car_x += FROM_FP(player_x * 20);
    
    // Bounce based on speed
    int bounce = 0;
    if (player_speed > 0) {
        bounce = ((frame_count * FROM_FP(player_speed)) >> 5) & 1;
    }
    
    // Car body (main rectangle)
    fill_rect(car_x + 4, CAR_Y + 8 - bounce, CAR_W - 8, 14, COL_CAR_BODY);
    
    // Car hood
    fill_rect(car_x + 8, CAR_Y + 16 - bounce, CAR_W - 16, 6, COL_CAR_BODY);
    
    // Windshield/cabin
    fill_rect(car_x + 10, CAR_Y + 4 - bounce, CAR_W - 20, 8, COL_CAR_WINDOW);
    
    // Wheels
    fill_rect(car_x + 2, CAR_Y + 14 - bounce, 6, 8, COL_CAR_WHEEL);
    fill_rect(car_x + CAR_W - 8, CAR_Y + 14 - bounce, 6, 8, COL_CAR_WHEEL);
    
    // Wheel shine (animation)
    if ((frame_count >> 1) & 1) {
        put_pixel(car_x + 4, CAR_Y + 16 - bounce, COL_WHITE);
        put_pixel(car_x + CAR_W - 6, CAR_Y + 16 - bounce, COL_WHITE);
    }
}

//----------------------------------------------------------------------
// Draw HUD
//----------------------------------------------------------------------
void draw_hud(void)
{
    // Speed bar background
    fill_rect(8, 8, 52, 8, COL_ROAD1);
    
    // Speed bar fill
    s32 speed_pct = FP_MUL(player_speed, FP_DIV(FP_ONE, max_speed));
    int bar_w = FROM_FP(speed_pct * 50);
    if (bar_w < 0) bar_w = 0;
    if (bar_w > 50) bar_w = 50;
    
    for (int i = 0; i < bar_w; i++) {
        u8 col = (i < 25) ? COL_GRASS2 : ((i < 40) ? COL_RUMBLE1 : COL_CAR_BODY);
        fill_rect(9 + i, 9, 1, 6, col);
    }
}

//----------------------------------------------------------------------
// Update game logic
//----------------------------------------------------------------------
void update_game(void)
{
    // Get current segment
    int seg_idx = (player_z / SEG_LENGTH) % ROAD_SEGMENTS;
    Segment *cur_seg = &road[seg_idx];
    
    // Check if player is on road
    s32 road_half = TO_FP(1);  // Road boundary
    int offroad = (player_x < -road_half || player_x > road_half);
    
    // Acceleration
    if (key_is_down(KEY_A)) {
        player_speed += accel;
    } else if (key_is_down(KEY_B)) {
        player_speed -= brake_power;
    } else {
        player_speed -= decel;
    }
    
    // Off-road slowdown
    if (offroad && player_speed > offroad_limit) {
        player_speed -= accel * 3;
    }
    
    // Clamp speed
    if (player_speed > max_speed) player_speed = max_speed;
    if (player_speed < 0) player_speed = 0;
    
    // Steering
    s32 steer_amount = FP_ONE / 40;
    if (player_speed > 0) {
        // Steering is more effective at lower speeds
        s32 steer_factor = FP_DIV(max_speed, player_speed + (max_speed / 4));
        steer_amount = FP_MUL(steer_amount, steer_factor);
    }
    
    if (key_is_down(KEY_LEFT)) {
        player_x -= steer_amount;
    }
    if (key_is_down(KEY_RIGHT)) {
        player_x += steer_amount;
    }
    
    // Centrifugal force (push player to outside of curves)
    if (player_speed > 0) {
        s32 curve_force = FP_MUL(cur_seg->curve * 64, player_speed);
        curve_force = FP_MUL(curve_force, centrifugal);
        player_x += curve_force >> 16;
    }
    
    // Limit player position (can go a bit off road)
    s32 limit = TO_FP(2);
    if (player_x < -limit) player_x = -limit;
    if (player_x > limit) player_x = limit;
    
    // Move forward
    player_z += FROM_FP(player_speed);
    
    // Wrap around track
    if (player_z >= ROAD_SEGMENTS * SEG_LENGTH) {
        player_z -= ROAD_SEGMENTS * SEG_LENGTH;
    }
    
    frame_count++;
}

//----------------------------------------------------------------------
// Main
//----------------------------------------------------------------------
int main(void)
{
    // Initialize display - Mode 4 (8bpp indexed)
    REG_DISPCNT = DCNT_MODE4 | DCNT_BG2;
    
    // Initialize game
    init_palette();
    init_road();
    init_game();
    
    // Main loop
    while (1) {
        // Wait for VBlank
        vid_vsync();
        
        // Poll keys
        key_poll();
        
        // Update
        update_game();
        
        // Render (to back buffer)
        // In Mode 4, we're writing directly to vid_mem (page 0)
        // For proper double buffering, you'd use vid_flip()
        render_road();
        draw_player_car();
        draw_hud();
    }
    
    return 0;
}
