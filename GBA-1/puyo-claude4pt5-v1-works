//======================================================================
// Puyo Puyo GBA - Single File Implementation using TONC
// Build with: make (using a standard GBA Makefile with TONC)
//======================================================================

#include <tonc.h>
#include <string.h>

//----------------------------------------------------------------------
// Constants
//----------------------------------------------------------------------
#define BOARD_W     6
#define BOARD_H     12
#define CELL_SIZE   8
#define BOARD_X     11      // Screen tile offset X
#define BOARD_Y     2       // Screen tile offset Y

#define EMPTY       0
#define NUM_COLORS  4

// Game states
enum {
    ST_FALLING,
    ST_LOCK_DELAY,
    ST_DROPPING,
    ST_CHECKING,
    ST_POPPING,
    ST_GAMEOVER
};

//----------------------------------------------------------------------
// Structures
//----------------------------------------------------------------------
typedef struct {
    int x1, y1;     // Main puyo position
    int x2, y2;     // Sub puyo position
    int c1, c2;     // Colors
    int rot;        // Rotation state (0-3)
} FallingPair;

//----------------------------------------------------------------------
// Globals
//----------------------------------------------------------------------
static u8 board[BOARD_H][BOARD_W];
static u8 mark[BOARD_H][BOARD_W];
static FallingPair fall;
static int nextC1, nextC2;

static int state;
static int fallTimer;
static int fallSpeed;
static int lockTimer;
static int popTimer;
static int chain;
static u32 score;
static u32 randState;

//----------------------------------------------------------------------
// Random number generator
//----------------------------------------------------------------------
static u32 randNext(void) {
    randState = randState * 1664525u + 1013904223u;
    return randState;
}

static int randColor(void) {
    return (randNext() % NUM_COLORS) + 1;
}

//----------------------------------------------------------------------
// Board helpers
//----------------------------------------------------------------------
static int inBounds(int x, int y) {
    return x >= 0 && x < BOARD_W && y >= 0 && y < BOARD_H;
}

static int isEmpty(int x, int y) {
    if (y < 0) return 1;  // Above board is empty
    if (x < 0 || x >= BOARD_W || y >= BOARD_H) return 0;
    return board[y][x] == EMPTY;
}

//----------------------------------------------------------------------
// Spawn and rotation
//----------------------------------------------------------------------
static void calcSubPos(int x1, int y1, int rot, int *x2, int *y2) {
    // rot: 0=up, 1=right, 2=down, 3=left
    const int dx[] = { 0, 1, 0, -1 };
    const int dy[] = { -1, 0, 1, 0 };
    *x2 = x1 + dx[rot];
    *y2 = y1 + dy[rot];
}

static int canPlace(int x1, int y1, int x2, int y2) {
    return isEmpty(x1, y1) && isEmpty(x2, y2);
}

static void spawnPair(void) {
    fall.x1 = 2;
    fall.y1 = 0;
    fall.rot = 0;
    fall.c1 = nextC1;
    fall.c2 = nextC2;
    calcSubPos(fall.x1, fall.y1, fall.rot, &fall.x2, &fall.y2);
    
    nextC1 = randColor();
    nextC2 = randColor();
    
    // Check game over
    if (!isEmpty(fall.x1, fall.y1)) {
        state = ST_GAMEOVER;
    }
}

static void tryRotate(int dir) {
    int newRot = (fall.rot + dir + 4) % 4;
    int nx2, ny2;
    calcSubPos(fall.x1, fall.y1, newRot, &nx2, &ny2);
    
    // Try normal rotation
    if (canPlace(fall.x1, fall.y1, nx2, ny2)) {
        fall.rot = newRot;
        fall.x2 = nx2;
        fall.y2 = ny2;
        return;
    }
    
    // Wall kick: try shifting
    int kicks[] = { -1, 1, 0 };
    for (int i = 0; i < 2; i++) {
        int kx = fall.x1 + kicks[i];
        int kx2, ky2;
        calcSubPos(kx, fall.y1, newRot, &kx2, &ky2);
        if (canPlace(kx, fall.y1, kx2, ky2)) {
            fall.x1 = kx;
            fall.rot = newRot;
            fall.x2 = kx2;
            fall.y2 = ky2;
            return;
        }
    }
    
    // Floor kick for upward rotation
    if (newRot == 0) {
        int ny1 = fall.y1 - 1;
        calcSubPos(fall.x1, ny1, newRot, &nx2, &ny2);
        if (canPlace(fall.x1, ny1, nx2, ny2)) {
            fall.y1 = ny1;
            fall.rot = newRot;
            fall.x2 = nx2;
            fall.y2 = ny2;
        }
    }
}

static void tryMove(int dx) {
    int nx1 = fall.x1 + dx;
    int nx2 = fall.x2 + dx;
    if (canPlace(nx1, fall.y1, nx2, fall.y2)) {
        fall.x1 = nx1;
        fall.x2 = nx2;
    }
}

static int canFall(void) {
    return isEmpty(fall.x1, fall.y1 + 1) && isEmpty(fall.x2, fall.y2 + 1);
}

//----------------------------------------------------------------------
// Lock and gravity
//----------------------------------------------------------------------
static void lockPair(void) {
    // Drop main puyo
    int y = fall.y1;
    while (y + 1 < BOARD_H && board[y + 1][fall.x1] == EMPTY) y++;
    if (y >= 0 && y < BOARD_H) board[y][fall.x1] = fall.c1;
    
    // Drop sub puyo
    y = fall.y2;
    while (y + 1 < BOARD_H && board[y + 1][fall.x2] == EMPTY) y++;
    if (y >= 0 && y < BOARD_H) board[y][fall.x2] = fall.c2;
    
    chain = 0;
    state = ST_DROPPING;
}

static int applyGravity(void) {
    int moved = 0;
    for (int x = 0; x < BOARD_W; x++) {
        for (int y = BOARD_H - 2; y >= 0; y--) {
            if (board[y][x] != EMPTY && board[y + 1][x] == EMPTY) {
                int ny = y;
                while (ny + 1 < BOARD_H && board[ny + 1][x] == EMPTY) ny++;
                board[ny][x] = board[y][x];
                board[y][x] = EMPTY;
                moved = 1;
            }
        }
    }
    return moved;
}

//----------------------------------------------------------------------
// Match detection (flood fill)
//----------------------------------------------------------------------
static int floodCount(int x, int y, int color, u8 visited[BOARD_H][BOARD_W]) {
    if (!inBounds(x, y) || visited[y][x] || board[y][x] != color)
        return 0;
    visited[y][x] = 1;
    return 1 + floodCount(x+1, y, color, visited)
             + floodCount(x-1, y, color, visited)
             + floodCount(x, y+1, color, visited)
             + floodCount(x, y-1, color, visited);
}

static void floodMark(int x, int y, int color) {
    if (!inBounds(x, y) || mark[y][x] || board[y][x] != color)
        return;
    mark[y][x] = 1;
    floodMark(x+1, y, color);
    floodMark(x-1, y, color);
    floodMark(x, y+1, color);
    floodMark(x, y-1, color);
}

static int checkMatches(void) {
    u8 visited[BOARD_H][BOARD_W] = {0};
    memset(mark, 0, sizeof(mark));
    int found = 0;
    
    for (int y = 0; y < BOARD_H; y++) {
        for (int x = 0; x < BOARD_W; x++) {
            if (board[y][x] != EMPTY && !visited[y][x]) {
                int col = board[y][x];
                u8 temp[BOARD_H][BOARD_W];
                memcpy(temp, visited, sizeof(temp));
                int cnt = floodCount(x, y, col, temp);
                if (cnt >= 4) {
                    floodMark(x, y, col);
                    found = 1;
                }
                memcpy(visited, temp, sizeof(visited));
            }
        }
    }
    return found;
}

static void removeMarked(void) {
    int cnt = 0;
    for (int y = 0; y < BOARD_H; y++) {
        for (int x = 0; x < BOARD_W; x++) {
            if (mark[y][x]) {
                board[y][x] = EMPTY;
                cnt++;
            }
        }
    }
    chain++;
    score += cnt * 10 * chain * chain;
}

//----------------------------------------------------------------------
// Graphics setup
//----------------------------------------------------------------------

// Simple 8x8 tile patterns (4bpp = 32 bytes per tile)
static void createTile(int idx, u8 colorIdx) {
    u32 fill = colorIdx * 0x11111111u;
    for (int i = 0; i < 8; i++) {
        tile_mem[0][idx].data[i] = fill;
    }
}

// Create a puyo tile with border
static void createPuyoTile(int idx, u8 colorIdx) {
    u32 fill = colorIdx * 0x11111111u;
    u32 dark = (colorIdx == 0) ? 0 : ((colorIdx) * 0x11111111u);
    
    // Simple filled square - could be enhanced with shading
    for (int i = 0; i < 8; i++) {
        tile_mem[0][idx].data[i] = fill;
    }
    // Add simple border effect
    tile_mem[0][idx].data[0] = dark;
    tile_mem[0][idx].data[7] = dark;
}

static void initGraphics(void) {
    // Video mode 0, BG0 enabled
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // BG0: charblock 0, screenblock 31
    REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32 | BG_PRIO(1);
    
    // Set up palette
    pal_bg_mem[0]  = RGB15(1, 1, 2);      // BG color (dark blue)
    pal_bg_mem[1]  = RGB15(31, 6, 6);     // Red puyo
    pal_bg_mem[2]  = RGB15(6, 31, 6);     // Green puyo
    pal_bg_mem[3]  = RGB15(6, 10, 31);    // Blue puyo
    pal_bg_mem[4]  = RGB15(31, 31, 6);    // Yellow puyo
    pal_bg_mem[5]  = RGB15(3, 3, 8);      // Board BG
    pal_bg_mem[6]  = RGB15(18, 18, 22);   // Border
    pal_bg_mem[7]  = RGB15(31, 31, 31);   // White (text)
    pal_bg_mem[8]  = RGB15(20, 4, 4);     // Dark red
    pal_bg_mem[9]  = RGB15(4, 20, 4);     // Dark green
    pal_bg_mem[10] = RGB15(4, 6, 20);     // Dark blue
    pal_bg_mem[11] = RGB15(20, 20, 4);    // Dark yellow
    pal_bg_mem[12] = RGB15(10, 10, 10);   // Gray
    
    // Create tiles
    createTile(0, 0);   // Empty
    createPuyoTile(1, 1);  // Red
    createPuyoTile(2, 2);  // Green
    createPuyoTile(3, 3);  // Blue
    createPuyoTile(4, 4);  // Yellow
    createTile(5, 5);   // Board BG
    createTile(6, 6);   // Border
    createTile(7, 7);   // White block
    createTile(8, 12);  // Gray block
    
    // Clear screen map
    memset16(&se_mem[31][0], 0, 32*32);
}

//----------------------------------------------------------------------
// Drawing
//----------------------------------------------------------------------
static void drawBoard(void) {
    // Draw border
    for (int y = 0; y <= BOARD_H + 1; y++) {
        se_mem[31][(BOARD_Y + y) * 32 + BOARD_X - 1] = 6;
        se_mem[31][(BOARD_Y + y) * 32 + BOARD_X + BOARD_W] = 6;
    }
    for (int x = 0; x < BOARD_W; x++) {
        se_mem[31][(BOARD_Y + BOARD_H + 1) * 32 + BOARD_X + x] = 6;
    }
    
    // Draw board contents
    for (int y = 0; y < BOARD_H; y++) {
        for (int x = 0; x < BOARD_W; x++) {
            int tile = 5;  // Board background
            if (board[y][x] != EMPTY) {
                tile = board[y][x];
                // Flash during pop
                if (state == ST_POPPING && mark[y][x]) {
                    tile = ((popTimer >> 2) & 1) ? 0 : board[y][x];
                }
            }
            se_mem[31][(BOARD_Y + y) * 32 + BOARD_X + x] = tile;
        }
    }
    
    // Draw falling pair
    if (state == ST_FALLING || state == ST_LOCK_DELAY) {
        if (fall.y1 >= 0) {
            se_mem[31][(BOARD_Y + fall.y1) * 32 + BOARD_X + fall.x1] = fall.c1;
        }
        if (fall.y2 >= 0) {
            se_mem[31][(BOARD_Y + fall.y2) * 32 + BOARD_X + fall.x2] = fall.c2;
        }
    }
    
    // Draw next piece preview
    int nx = BOARD_X + BOARD_W + 2;
    int ny = BOARD_Y + 1;
    se_mem[31][ny * 32 + nx] = nextC2;
    se_mem[31][(ny + 1) * 32 + nx] = nextC1;
    
    // Draw score indicator (chain count as blocks)
    for (int i = 0; i < 8; i++) {
        int tile = (i < chain) ? 7 : 8;
        se_mem[31][(BOARD_Y + BOARD_H) * 32 + nx + i] = tile;
    }
}

static void drawGameOver(void) {
    // Simple X pattern in center of board
    int cx = BOARD_X + BOARD_W / 2;
    int cy = BOARD_Y + BOARD_H / 2;
    for (int i = -2; i <= 2; i++) {
        se_mem[31][(cy + i) * 32 + cx + i] = 1;
        se_mem[31][(cy + i) * 32 + cx - i] = 1;
    }
}

//----------------------------------------------------------------------
// Game logic update
//----------------------------------------------------------------------
static void initGame(void) {
    memset(board, EMPTY, sizeof(board));
    memset(mark, 0, sizeof(mark));
    
    randState = 12345;  // Could seed from timer
    nextC1 = randColor();
    nextC2 = randColor();
    
    state = ST_FALLING;
    fallTimer = 0;
    fallSpeed = 40;  // Frames per drop
    lockTimer = 0;
    popTimer = 0;
    chain = 0;
    score = 0;
    
    spawnPair();
}

static void updateGame(void) {
    key_poll();
    
    switch (state) {
    case ST_FALLING:
        // Input handling
        if (key_hit(KEY_LEFT))  tryMove(-1);
        if (key_hit(KEY_RIGHT)) tryMove(1);
        if (key_hit(KEY_A))     tryRotate(1);
        if (key_hit(KEY_B))     tryRotate(-1);
        if (key_hit(KEY_UP))    tryRotate(1);
        
        // Faster drop
        if (key_is_down(KEY_DOWN)) fallTimer += 5;
        
        // Auto-fall
        fallTimer++;
        if (fallTimer >= fallSpeed) {
            fallTimer = 0;
            if (canFall()) {
                fall.y1++;
                fall.y2++;
            } else {
                lockTimer = 20;
                state = ST_LOCK_DELAY;
            }
        }
        break;
        
    case ST_LOCK_DELAY:
        // Allow last-second moves
        if (key_hit(KEY_LEFT))  tryMove(-1);
        if (key_hit(KEY_RIGHT)) tryMove(1);
        if (key_hit(KEY_A))     tryRotate(1);
        if (key_hit(KEY_B))     tryRotate(-1);
        
        if (canFall()) {
            state = ST_FALLING;
        } else {
            lockTimer--;
            if (lockTimer <= 0 || key_is_down(KEY_DOWN)) {
                lockPair();
            }
        }
        break;
        
    case ST_DROPPING:
        if (!applyGravity()) {
            state = ST_CHECKING;
        }
        break;
        
    case ST_CHECKING:
        if (checkMatches()) {
            popTimer = 25;
            state = ST_POPPING;
        } else {
            spawnPair();
            if (state != ST_GAMEOVER) {
                state = ST_FALLING;
                fallTimer = 0;
            }
        }
        break;
        
    case ST_POPPING:
        popTimer--;
        if (popTimer <= 0) {
            removeMarked();
            state = ST_DROPPING;
        }
        break;
        
    case ST_GAMEOVER:
        if (key_hit(KEY_START)) {
            initGame();
        }
        break;
    }
}

//----------------------------------------------------------------------
// Main
//----------------------------------------------------------------------
int main(void) {
    // Set up interrupts for VBlank
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    initGraphics();
    initGame();
    
    while (1) {
        VBlankIntrWait();
        updateGame();
        drawBoard();
        
        if (state == ST_GAMEOVER) {
            drawGameOver();
        }
    }
    
    return 0;
}
