#include <tonc.h>
#include <stdlib.h>

/* --- CONFIGURATION --- */
#define GRID_W      6
#define GRID_H      10
#define BLOCK_SIZE  16

// Screen positioning to center the grid
#define OFFSET_X    ((240 - (GRID_W * BLOCK_SIZE)) / 2)
#define OFFSET_Y    ((160 - (GRID_H * BLOCK_SIZE)) / 2)

// Colors
#define COLOR_BG    CLR_BLACK
#define COLOR_GRID  RGB15(5, 5, 5)

// Block Types
enum {
    TYPE_EMPTY = 0,
    TYPE_RED,
    TYPE_GREEN,
    TYPE_BLUE,
    TYPE_YELLOW,
    TYPE_PURPLE,
    TYPE_CYAN,
    TYPE_COUNT // Used for RNG
};

// Map colors to the enum
const u16 block_colors[] = {
    COLOR_GRID,   // Empty slot color
    CLR_RED,
    CLR_LIME,
    CLR_BLUE,
    CLR_YELLOW,
    CLR_MAG,
    CLR_CYAN
};

/* --- GLOBAL STATE --- */
u8 map[GRID_H][GRID_W];
int cursor_x = 2;
int cursor_y = 5;

// Simple state machine to handle animations/cascades
enum GameState {
    STATE_INPUT,
    STATE_RESOLVE
};
int state = STATE_INPUT;
int resolve_timer = 0;

/* --- DRAWING FUNCTIONS --- */

// Draw a filled rectangle in Mode 3
void draw_rect(int x, int y, int w, int h, u16 color) {
    for (int iy = 0; iy < h; iy++) {
        for (int ix = 0; ix < w; ix++) {
            m3_mem[((y + iy) * 240) + (x + ix)] = color;
        }
    }
}

// Draw a hollow rectangle (for the cursor)
void draw_frame(int x, int y, int w, int h, u16 color) {
    // Top & Bottom
    for(int ix=0; ix<w; ix++) {
        m3_mem[(y * 240) + (x + ix)] = color;
        m3_mem[((y + h - 1) * 240) + (x + ix)] = color;
    }
    // Left & Right
    for(int iy=0; iy<h; iy++) {
        m3_mem[((y + iy) * 240) + x] = color;
        m3_mem[((y + iy) * 240) + (x + w - 1)] = color;
    }
}

void draw_game() {
    // 1. Draw the board blocks
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            int px = OFFSET_X + (x * BLOCK_SIZE);
            int py = OFFSET_Y + (y * BLOCK_SIZE);
            
            // Draw block body
            // We subtract 1 from size to create a small grid line effect
            draw_rect(px, py, BLOCK_SIZE-1, BLOCK_SIZE-1, block_colors[map[y][x]]);
        }
    }

    // 2. Draw the Cursor (2 blocks wide)
    int cx = OFFSET_X + (cursor_x * BLOCK_SIZE);
    int cy = OFFSET_Y + (cursor_y * BLOCK_SIZE);
    
    // Draw a thick white outline (simulated by drawing 2 frames)
    u16 cursor_col = CLR_WHITE;
    draw_frame(cx - 1, cy - 1, (BLOCK_SIZE * 2) + 1, BLOCK_SIZE + 1, cursor_col);
    draw_frame(cx - 2, cy - 2, (BLOCK_SIZE * 2) + 3, BLOCK_SIZE + 3, cursor_col);
}

/* --- GAME LOGIC --- */

void init_map() {
    // Initialize with some random blocks, but leave top empty
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            if(y > 4) { // Only fill bottom half
                map[y][x] = (rand() % (TYPE_COUNT - 1)) + 1;
            } else {
                map[y][x] = TYPE_EMPTY;
            }
        }
    }
}

// Check for matches (horizontal and vertical)
// Returns 1 if matches were found and cleared
int check_matches() {
    u8 to_remove[GRID_H][GRID_W];
    // Clear remove mask
    for(int i=0; i<GRID_H * GRID_W; i++) ((u8*)to_remove)[i] = 0;

    int match_found = 0;

    // Horizontal Checks
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W - 2; x++) {
            u8 t = map[y][x];
            if (t == TYPE_EMPTY) continue;
            if (map[y][x+1] == t && map[y][x+2] == t) {
                to_remove[y][x] = 1;
                to_remove[y][x+1] = 1;
                to_remove[y][x+2] = 1;
                match_found = 1;
            }
        }
    }

    // Vertical Checks
    for (int x = 0; x < GRID_W; x++) {
        for (int y = 0; y < GRID_H - 2; y++) {
            u8 t = map[y][x];
            if (t == TYPE_EMPTY) continue;
            if (map[y+1][x] == t && map[y+2][x] == t) {
                to_remove[y][x] = 1;
                to_remove[y+1][x] = 1;
                to_remove[y+2][x] = 1;
                match_found = 1;
            }
        }
    }

    // Apply removal
    if (match_found) {
        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                if (to_remove[y][x]) {
                    map[y][x] = TYPE_EMPTY;
                }
            }
        }
    }

    return match_found;
}

// Handle gravity
// Returns 1 if any blocks moved down
int handle_gravity() {
    int moved = 0;
    // Iterate from bottom up, column by column
    for (int x = 0; x < GRID_W; x++) {
        for (int y = GRID_H - 1; y > 0; y--) {
            // If current is empty and above is not
            if (map[y][x] == TYPE_EMPTY && map[y-1][x] != TYPE_EMPTY) {
                map[y][x] = map[y-1][x];
                map[y-1][x] = TYPE_EMPTY;
                moved = 1;
            }
        }
    }
    return moved;
}

/* --- MAIN --- */

int main() {
    // 1. Init Graphics (Mode 3: 240x160 Bitmap, BG2 enabled)
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    // 2. Setup initial state
    init_map();
    
    // Seed RNG (crude)
    srand(1234);

    while (1) {
        vid_vsync(); // Wait for VBlank
        key_poll();  // Read input

        // Clear screen (Simple fill)
        // Optimization: In a real game, only redraw changed areas.
        // For this demo, we clear the specific grid area to avoid full screen clear flicker
        draw_rect(OFFSET_X-5, OFFSET_Y-5, (GRID_W*BLOCK_SIZE)+10, (GRID_H*BLOCK_SIZE)+10, COLOR_BG);

        /* GAME LOOP */
        
        if (state == STATE_INPUT) {
            // MOVEMENT
            if (key_hit(KEY_UP) && cursor_y > 0) cursor_y--;
            if (key_hit(KEY_DOWN) && cursor_y < GRID_H - 1) cursor_y++;
            if (key_hit(KEY_LEFT) && cursor_x > 0) cursor_x--;
            if (key_hit(KEY_RIGHT) && cursor_x < GRID_W - 2) cursor_x++; // -2 because cursor is width 2

            // SWAPPING (Button A)
            if (key_hit(KEY_A)) {
                u8 temp = map[cursor_y][cursor_x];
                map[cursor_y][cursor_x] = map[cursor_y][cursor_x + 1];
                map[cursor_y][cursor_x + 1] = temp;
                
                // After a swap, we check if physics need to happen
                state = STATE_RESOLVE;
                resolve_timer = 10; // Initial delay
            }

            // RESET (Button Select)
            if (key_hit(KEY_SELECT)) {
                init_map();
            }
            
            // Check if blocks are floating (e.g. after a manual clear or swap)
            if(handle_gravity()) state = STATE_RESOLVE;
            // Check if matches exist spontaneously
            if(check_matches()) state = STATE_RESOLVE;

        } 
        else if (state == STATE_RESOLVE) {
            // This state handles Gravity and Matching cascades automatically.
            // We add a small timer so the player can see the blocks falling/clearing.
            
            if (resolve_timer > 0) {
                resolve_timer--;
            } else {
                // 1. Try Gravity
                int fell = handle_gravity();
                
                if (fell) {
                    // If things fell, wait a bit, then loop again
                    resolve_timer = 5; 
                } else {
                    // 2. If stable, Check Matches
                    int matched = check_matches();
                    
                    if (matched) {
                        // If things popped, gravity might need to happen next
                        resolve_timer = 15; // Longer pause on pop
                    } else {
                        // 3. Stable board, return to input
                        state = STATE_INPUT;
                    }
                }
            }
        }

        draw_game();
    }

    return 0;
}
