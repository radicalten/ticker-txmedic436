#include <tonc.h>
#include <stdlib.h>

// --- Constants & Config ---
#define FIELD_X      80    // Screen X offset
#define FIELD_Y      8     // Screen Y offset
#define GRID_W       6     // Columns
#define GRID_H       12    // Rows
#define BLOCK_SIZE   12    // Pixel size of one block
#define DROP_SPEED   30    // Frames per gravity tick (lower = faster)

// Colors

// --- Global State ---
u8 grid[GRID_H][GRID_W];
u8 visited[GRID_H][GRID_W]; // For flood fill logic

typedef struct {
    int x, y;       // Grid coordinates of the MAIN puyo
    int rot;        // 0=Up, 1=Right, 2=Down, 3=Left (Sub puyo relative to Main)
    u8 cMain;       // Color of main puyo
    u8 cSub;        // Color of sub puyo
    int timer;      // Gravity timer
} Player;

Player p;
int score = 0;
bool gameOver = false;
bool isResolving = false; // True if chains/gravity are animating

// Puyo Colors Mapped to Tonc Colors
const u16 puyo_colors[] = { 
    CLR_BLACK, CLR_RED, CLR_LIME, CLR_BLUE, CLR_YELLOW, CLR_MAG 
};

// --- Helper Functions ---

// Draw a simple filled rectangle in Mode 3
void draw_rect(int x, int y, int w, int h, u16 color) {
    for(int iy=0; iy<h; iy++) {
        for(int ix=0; ix<w; ix++) {
            m3_plot(x+ix, y+iy, color);
        }
    }
}

void draw_block(int gx, int gy, u8 colorIdx) {
    int x = FIELD_X + (gx * BLOCK_SIZE);
    int y = FIELD_Y + (gy * BLOCK_SIZE);
    // Draw slightly smaller to show grid lines
    draw_rect(x, y, BLOCK_SIZE-1, BLOCK_SIZE-1, puyo_colors[colorIdx]);
}

void init_game() {
    // Reset Grid
    for(int y=0; y<GRID_H; y++) {
        for(int x=0; x<GRID_W; x++) {
            grid[y][x] = 0;
        }
    }
    score = 0;
    gameOver = false;
    isResolving = false;
}

void spawn_piece() {
    p.x = 2; 
    p.y = 0; // Start at top
    p.rot = 0; // Sub is above Main
    p.cMain = (qran_range(1, 6)); // Random color 1-5
    p.cSub  = (qran_range(1, 6)); 
    p.timer = 0;

    // Game Over check: if spawn point blocked
    if(grid[p.y][p.x] != 0) gameOver = true;
}

// Get coordinates of the Sub Puyo based on rotation
void get_sub_coords(int mx, int my, int rot, int *sx, int *sy) {
    *sx = mx; *sy = my;
    switch(rot) {
        case 0: (*sy)--; break; // Top
        case 1: (*sx)++; break; // Right
        case 2: (*sy)++; break; // Bottom
        case 3: (*sx)--; break; // Left
    }
}

// Check if a coordinate is valid and empty
bool can_move(int x, int y) {
    if(x < 0 || x >= GRID_W) return false;
    if(y < 0 || y >= GRID_H) return false;
    if(grid[y][x] != 0) return false;
    return true;
}

// --- Logic: Chain Reaction & Gravity ---

// Recursive flood fill to count connected blocks
int count_connected(int x, int y, u8 color) {
    if(x<0 || x>=GRID_W || y<0 || y>=GRID_H) return 0;
    if(visited[y][x] || grid[y][x] != color) return 0;

    visited[y][x] = 1;
    int count = 1;
    count += count_connected(x+1, y, color);
    count += count_connected(x-1, y, color);
    count += count_connected(x, y+1, color);
    count += count_connected(x, y-1, color);
    return count;
}

// Helper to remove marked blocks
void remove_connected(int x, int y, u8 color) {
    if(x<0 || x>=GRID_W || y<0 || y>=GRID_H) return;
    if(grid[y][x] != color) return;

    grid[y][x] = 0; // Remove
    remove_connected(x+1, y, color);
    remove_connected(x-1, y, color);
    remove_connected(x, y+1, color);
    remove_connected(x, y-1, color);
}

// Returns true if changes happened (gravity or pops)
bool resolve_board() {
    bool changed = false;

    // 1. Gravity: Pull blocks down
    for(int x=0; x<GRID_W; x++) {
        for(int y=GRID_H-1; y>=0; y--) {
            if(grid[y][x] == 0) {
                // Look for a block above to pull down
                for(int k=y-1; k>=0; k--) {
                    if(grid[k][x] != 0) {
                        grid[y][x] = grid[k][x];
                        grid[k][x] = 0;
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    if(changed) return true; // Let gravity finish before checking matches

    // 2. Check Matches (Connect 4)
    bool popped = false;
    
    // Clear visited
    for(int i=0; i<GRID_H*GRID_W; i++) ((u8*)visited)[i] = 0;

    for(int y=0; y<GRID_H; y++) {
        for(int x=0; x<GRID_W; x++) {
            if(grid[y][x] != 0 && !visited[y][x]) {
                // Temporarily store visited state for this check
                // We need to pass a clean visited map or handle it carefully.
                // Simplified: reset visited, run check. 
                // Optimization: Actually, we just need to know if this group > 4.
                
                // Clear local visited for accurate count
                for(int i=0; i<GRID_H*GRID_W; i++) ((u8*)visited)[i] = 0;
                
                int count = count_connected(x, y, grid[y][x]);
                if(count >= 4) {
                    remove_connected(x, y, grid[y][x]);
                    score += (count * 10);
                    popped = true;
                }
            }
        }
    }

    return popped;
}

// --- Main Loop Functions ---

void update() {
    if(gameOver) {
        if(key_hit(KEY_START)) init_game();
        return;
    }

    // If we are in chain-reaction mode
    if(isResolving) {
        // Slow down the resolution so player can see it
        static int resolveDelay = 0;
        if(++resolveDelay > 15) {
            resolveDelay = 0;
            if(!resolve_board()) {
                isResolving = false; // Board settled
                spawn_piece();
            }
        }
        return;
    }

    // Player Control
    int dx = 0;
    if(key_hit(KEY_LEFT)) dx = -1;
    if(key_hit(KEY_RIGHT)) dx = 1;

    int sx, sy;
    get_sub_coords(p.x, p.y, p.rot, &sx, &sy);

    // Horizontal Movement
    if(dx != 0) {
        if(can_move(p.x + dx, p.y) && can_move(sx + dx, sy)) {
            p.x += dx;
        }
    }

    // Rotation (A = Clockwise)
    if(key_hit(KEY_A)) {
        int newRot = (p.rot + 1) % 4;
        int nsx, nsy;
        get_sub_coords(p.x, p.y, newRot, &nsx, &nsy);
        
        if(can_move(nsx, nsy)) {
            p.rot = newRot;
        } else {
            // Wall kick: Try moving main piece left/right to accommodate rotation
            if(can_move(p.x-1, p.y) && can_move(nsx-1, nsy)) {
                p.x--; p.rot = newRot;
            } else if (can_move(p.x+1, p.y) && can_move(nsx+1, nsy)) {
                p.x++; p.rot = newRot;
            }
        }
    }

    // Gravity / Soft Drop
    bool drop = false;
    p.timer++;
    if(key_is_down(KEY_DOWN) || p.timer > DROP_SPEED) {
        p.timer = 0;
        
        // Check if can move down
        get_sub_coords(p.x, p.y, p.rot, &sx, &sy);
        
        if(can_move(p.x, p.y+1) && can_move(sx, sy+1)) {
            p.y++;
        } else {
            // Lock piece
            grid[p.y][p.x] = p.cMain;
            grid[sy][sx]   = p.cSub;
            isResolving = true; // Start chain checks
        }
    }
}

void draw() {
    // Clear Field area
    draw_rect(FIELD_X, FIELD_Y, GRID_W*BLOCK_SIZE, GRID_H*BLOCK_SIZE, CLR_BLACK);
    
    // Draw Border
    draw_rect(FIELD_X-1, FIELD_Y-1, (GRID_W*BLOCK_SIZE)+2, (GRID_H*BLOCK_SIZE)+2, CLR_WHITE);
    // Redraw inner black to clear previous frame mess (lazy clearing)
    draw_rect(FIELD_X, FIELD_Y, GRID_W*BLOCK_SIZE, GRID_H*BLOCK_SIZE, CLR_BLACK);

    // Draw Static Grid
    for(int y=0; y<GRID_H; y++) {
        for(int x=0; x<GRID_W; x++) {
            if(grid[y][x] != 0) {
                draw_block(x, y, grid[y][x]);
            }
        }
    }

    // Draw Active Piece
    if(!gameOver && !isResolving) {
        draw_block(p.x, p.y, p.cMain);
        int sx, sy;
        get_sub_coords(p.x, p.y, p.rot, &sx, &sy);
        draw_block(sx, sy, p.cSub);
    }

    // Draw Score (Visual representation since we don't have font assets)
    // Draw a bar at the bottom representing score
    int barLen = (score / 10);
    if(barLen > 240) barLen = 240;
    draw_rect(0, 150, barLen, 5, CLR_LIME);

    if(gameOver) {
        draw_rect(FIELD_X + 10, FIELD_Y + 50, 50, 20, CLR_RED); // "Game Over" box
    }
}

int main() {
    // Init GBA Mode 3 (240x160 Bitmap) and BG2
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    // Seed random
    sqran(1); 

    init_game();
    spawn_piece();

    while(1) {
        VBlankIntrWait();
        key_poll();

        update();
        draw();
    }

    return 0;
}
