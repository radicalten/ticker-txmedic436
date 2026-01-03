#include <tonc.h>

// --- Constants ---
#define SCREEN_W      240
#define SCREEN_H      160
#define HORIZON       60
#define ROAD_WIDTH    2000  // Virtual road width
#define CAM_HEIGHT    100   // Camera height from ground
#define SEGMENT_LEN   50    // Distance of one road segment

// Colors
#define CLR_SKY       RGB15(5, 20, 31)
#define CLR_GRASS_L   RGB15(2, 18, 2)
#define CLR_GRASS_D   RGB15(1, 14, 1)
#define CLR_ROAD_L    RGB15(12, 12, 12)
#define CLR_ROAD_D    RGB15(10, 10, 10)
#define CLR_RUMBLE_W  RGB15(31, 31, 31)
#define CLR_RUMBLE_R  RGB15(31, 0, 0)
#define CLR_CAR       RGB15(31, 0, 0)

// --- Global State ---
FIXED pos_z = 0;         // Camera Z position
FIXED pos_x = 0;         // Camera X position (lateral)
FIXED speed = 0;         // Current Speed
FIXED current_curve = 0; // Current road curvature
int   track_curve = 0;   // Target curvature of the track section

// --- Simple Car Drawing ---
// Draws a pixel-art style car directly to VRAM
void draw_car(int x, int y) {
    // Simple 32x16 car shape centered at x,y
    int cw = 30; // Car width
    int ch = 14; // Car height
    int left = x - cw/2;
    int top = y - ch;
    
    // Bounds check
    if (left < 0) left = 0;
    if (left + cw > SCREEN_W) return;
    if (top < 0) top = 0;

    // Draw main body (Red)
    m3_rect(left, top + 4, left + cw, top + ch, CLR_CAR);
    
    // Draw cabin/windshield (Darker)
    m3_rect(left + 4, top, left + cw - 4, top + 6, RGB15(10,0,0));
    
    // Draw tires
    m3_rect(left - 2, top + 8, left + 2, top + 14, 0); // BL
    m3_rect(left + cw - 2, top + 8, left + cw + 2, top + 14, 0); // BR
    
    // Lights
    m3_plot(left + 2, top + 6, RGB15(31,31,0));
    m3_plot(left + cw - 3, top + 6, RGB15(31,31,0));
}

// --- Main Rendering Loop ---
int main() {
    // Initialize Video Mode: Mode 3 (240x160 Bitmap), BG2 enabled
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    // Game loop variables
    int y;
    FIXED z, scale, screen_x, screen_w;
    int half_w = SCREEN_W / 2;
    
    while (1) {
        // 1. Input Handling
        key_poll();
        
        // Accelerate with A, Brake with B
        if (key_is_down(KEY_A)) speed += 20;
        else speed -= 5;
        
        if (key_is_down(KEY_B)) speed -= 20;

        // Cap speed
        if (speed < 0) speed = 0;
        if (speed > 1200) speed = 1200; // Max speed

        // Steer with D-Pad
        // Car moves visually, but actually we move the world (pos_x)
        if (key_is_down(KEY_LEFT))  pos_x -= 60;
        if (key_is_down(KEY_RIGHT)) pos_x += 60;

        // Apply speed to Z position
        pos_z += speed;

        // Handle Curvature (Procedurally change curve every 2000 units)
        int track_section = (pos_z / 2000) / 1000; // Change occasionally
        if (track_section % 4 == 0) track_curve = 0;        // Straight
        else if (track_section % 4 == 1) track_curve = 200; // Right
        else if (track_section % 4 == 2) track_curve = -150;// Left
        else track_curve = 50;                              // Slight Right

        // Smoothly interpolate current curve to target track curve
        if (current_curve < track_curve) current_curve += 2;
        if (current_curve > track_curve) current_curve -= 2;

        // Auto-move X if in a curve (centrifugal force simulation)
        if (speed > 0) pos_x -= (current_curve * speed) >> 12;

        // 2. Rendering Phase
        vid_vsync(); // Wait for VBlank to prevent tearing

        // Draw Sky (Simple gradient or flat color fill)
        // DMA Fill is fast: (value, destination, count)
        // Note: Mode 3 is a u16 array.
        // We clear the top half (sky)
       // DMA_TRANSFER(&vid_mem[0], &CLR_SKY, (SCREEN_W * HORIZON), 
                     DMA_16 | DMA_ENABLE | DMA_SRC_FIXED);

        // Render Road Line by Line (Bottom to Horizon)
        // We use simple projection math: ScreenX = WorldX / Z
        
        FIXED dx = 0;     // Accumulated curve offset
        FIXED ddx = 0;    // Curve increment per line
        
        // Calculate curve intensity based on current_curve
        ddx = current_curve; 

        for (y = SCREEN_H - 1; y >= HORIZON; y--) {
            // Perspective Math
            // Z depth increases as Y goes up the screen.
            // Formula: Z = Y_Camera * Scale / (Y_Screen - Horizon)
            int line_y = y - HORIZON;
            if (line_y == 0) line_y = 1; // Prevent divide by zero at infinity
            
            // Map screen Y to World Z
            // Note: Shifts (<<) used for fixed point precision
            z = (CAM_HEIGHT << 8) / line_y; 
            
            // Scale factor for width at this Z
            scale = (1 << 8) / z; // Inverse Z
            
            // Screen width of the road at this scanline
            screen_w = (ROAD_WIDTH * 256) / z; 

            // Calculate World Position for color alternation
            // Add current Z position to find "texture" coordinate
            int total_z = pos_z + z;
            
            // Alternating Colors (Checkerboard effect)
            int segment = (total_z / SEGMENT_LEN) % 2;
            
            u16 grass_color = (segment == 0) ? CLR_GRASS_L : CLR_GRASS_D;
            u16 road_color  = (segment == 0) ? CLR_ROAD_L  : CLR_ROAD_D;
            u16 rumble_color= (segment == 0) ? CLR_RUMBLE_W : CLR_RUMBLE_R;

            // Calculate Center X of the road on screen
            // Start at center, subtract camera X (scaled by depth), add curve offset
            screen_x = half_w - ((pos_x * 256) / z) + dx;

            // Apply curve accumulation for the next line up
            dx += ddx; 
            // Perspective makes curves sharper closer to horizon? 
            // Actually in this simple projection, linear addition works for a parabolic look.
            ddx += current_curve / 10; 

            // Drawing the Scanline
            // We calculate left and right edges
            int l_grass = 0;
            int l_rumble = screen_x - screen_w - (screen_w/6);
            int l_road = screen_x - screen_w;
            int r_road = screen_x + screen_w;
            int r_rumble = screen_x + screen_w + (screen_w/6);
            int r_grass = SCREEN_W;

            // Clamp values to screen bounds to prevent wrapping
            if (l_rumble < 0) l_rumble = 0; if (l_rumble > SCREEN_W) l_rumble = SCREEN_W;
            if (l_road < 0) l_road = 0;     if (l_road > SCREEN_W) l_road = SCREEN_W;
            if (r_road < 0) r_road = 0;     if (r_road > SCREEN_W) r_road = SCREEN_W;
            if (r_rumble < 0) r_rumble = 0; if (r_rumble > SCREEN_W) r_rumble = SCREEN_W;

            // Optimization: Get pointer to the start of this scanline in VRAM
            u16 *line_ptr = &vid_mem[y * SCREEN_W];

            // Use DMA for fast horizontal filling (or fast loops)
            // 1. Left Grass
            if (l_rumble > 0) 
                dma3_fill(line_ptr, grass_color, l_rumble);
            
            // 2. Left Rumble Strip
            if (l_road > l_rumble) 
                dma3_fill(line_ptr + l_rumble, rumble_color, l_road - l_rumble);
            
            // 3. Road
            if (r_road > l_road) 
                dma3_fill(line_ptr + l_road, road_color, r_road - l_road);
            
            // 4. Right Rumble Strip
            if (r_rumble > r_road) 
                dma3_fill(line_ptr + r_road, rumble_color, r_rumble - r_road);
            
            // 5. Right Grass
            if (SCREEN_W > r_rumble) 
                dma3_fill(line_ptr + r_rumble, grass_color, SCREEN_W - r_rumble);
        }

        // Draw Player Car
        // Bounce car up and down slightly based on speed
        int bounce = (speed > 0) ? ((pos_z / 500) % 2) : 0;
        draw_car(half_w, SCREEN_H - 10 + bounce);
        
        // Draw HUD (Simple Speed Bar)
        m3_rect(10, 10, 10 + (speed/10), 15, RGB15(0,31,31));
    }

    return 0;
}
