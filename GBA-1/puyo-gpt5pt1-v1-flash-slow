// puyo_gba.c
// Simple Puyo Puyo–like puzzle game for GBA using tonc (single file)
//
// Build (example):
//   arm-none-eabi-gcc -mthumb -mthumb-interwork -O2 puyo_gba.c -o puyo_gba.elf -ltonc
//   arm-none-eabi-objcopy -O binary puyo_gba.elf puyo_gba.gba
//
// Controls:
//   Left/Right : move pair
//   A / Up     : rotate clockwise
//   Down       : soft drop (faster fall)
//   Start      : restart after game over

#include <tonc.h>
#include <stdlib.h>     // for rand, srand
#include <string.h>     // for memset

#define RGB15_C(r,g,b)  ((r) | ((g) << 5) | ((b) << 10))

// -----------------------------------------------------------------------------
// Constants & configuration
// -----------------------------------------------------------------------------

#define BOARD_W        6
#define BOARD_H       12

#define CELL_SIZE     10
#define BOARD_ORIGIN_X 80
#define BOARD_ORIGIN_Y 10

#define NUM_COLORS     4
#define FALL_DELAY    20    // frames between automatic drops

// Screen dimensions for Mode 3
#define SCREEN_W     240
#define SCREEN_H     160

// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------

typedef struct
{
    int x;          // pivot grid x
    int y;          // pivot grid y
    int orient;     // 0=up, 1=right, 2=down, 3=left (position of second block)
    u8  colorA;     // color of pivot
    u8  colorB;     // color of second block
    int active;     // 1 if falling, 0 if not
} FallingPair;

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

static u8 board[BOARD_H][BOARD_W];   // 0 = empty, 1..NUM_COLORS = puyo color

static FallingPair curPair;
static int fallTimer = 0;
static int gameOver  = 0;
static int score     = 0;

// Simple color table (index 0 unused here)
static const u16 puyoColors[NUM_COLORS+1] =
{
    RGB15_C(0,0,0),            // 0: empty (unused)
    RGB15_C(31,0,0),           // 1: red
    RGB15_C(0,31,0),           // 2: green
    RGB15_C(0,0,31),           // 3: blue
    RGB15_C(31,31,0)           // 4: yellow
};

static const u16 COLOR_BG     = RGB15_C(0,0,0);
static const u16 COLOR_BORDER = RGB15_C(10,10,10);
static const u16 COLOR_TEXT   = RGB15_C(31,31,31);

// -----------------------------------------------------------------------------
// Low-level drawing helpers (Mode 3)
// -----------------------------------------------------------------------------

static inline u16* videoBuffer(void)
{
    return (u16*)MEM_VRAM;
}

static void clear_screen(u16 clr)
{
    u16 *dst = videoBuffer();
    int n = SCREEN_W * SCREEN_H;
    for(int i=0; i<n; i++)
        dst[i] = clr;
}

static void draw_rect(int x, int y, int w, int h, u16 clr)
{
    if(x < 0) { w += x; x = 0; }
    if(y < 0) { h += y; y = 0; }
    if(x + w > SCREEN_W)  w = SCREEN_W - x;
    if(y + h > SCREEN_H)  h = SCREEN_H - y;
    if(w <= 0 || h <= 0)  return;

    u16 *dst = videoBuffer() + y*SCREEN_W + x;
    for(int iy=0; iy<h; iy++)
    {
        for(int ix=0; ix<w; ix++)
            dst[ix] = clr;
        dst += SCREEN_W;
    }
}

// Very minimal “text” by drawing a few rectangles to say "GAME OVER"
static void draw_game_over_text(void)
{
    // Centered rectangles forming a simple blocky text
    int cx = 120;
    int cy = 80;
    int w  = 80;
    int h  = 10;

    // Just draw a bar and a small box to indicate "GAME OVER" roughly
    draw_rect(cx - w/2, cy - h/2, w, h, COLOR_TEXT);
    draw_rect(cx - 5,   cy - 20, 10, 10, COLOR_TEXT);
}

// -----------------------------------------------------------------------------
// Board / piece helpers
// -----------------------------------------------------------------------------

static void get_pair_blocks(const FallingPair *p,
                            int *ax, int *ay,
                            int *bx, int *by,
                            int orientOverride)
{
    int x = p->x;
    int y = p->y;
    int o = (orientOverride >= 0) ? orientOverride : p->orient;

    // A = pivot
    *ax = x;
    *ay = y;

    // B relative to pivot
    switch(o)
    {
        case 0: *bx = x;   *by = y-1; break; // up
        case 1: *bx = x+1; *by = y;   break; // right
        case 2: *bx = x;   *by = y+1; break; // down
        case 3: *bx = x-1; *by = y;   break; // left
    }
}

// Check if a pair with given (x,y,orient) can be placed on the board
static int can_place_pair(int x, int y, int orient, u8 colorA, u8 colorB)
{
    FallingPair tmp = { x, y, orient, colorA, colorB, 1 };
    int ax, ay, bx, by;
    get_pair_blocks(&tmp, &ax, &ay, &bx, &by, orient);

    // Both blocks must be within the board
    if(ax < 0 || ax >= BOARD_W || ay < 0 || ay >= BOARD_H) return 0;
    if(bx < 0 || bx >= BOARD_W || by < 0 || by >= BOARD_H) return 0;

    // And must not collide with existing puyos
    if(board[ay][ax] != 0) return 0;
    if(board[by][bx] != 0) return 0;

    return 1;
}

// -----------------------------------------------------------------------------
// Match detection & gravity
// -----------------------------------------------------------------------------

// Find and clear groups of >=4 connected puyos of same color.
// Returns number of cleared cells.
static int find_and_clear_matches(void)
{
    int clearedCount = 0;
    int visited[BOARD_H][BOARD_W];
    int toClear[BOARD_H][BOARD_W];

    memset(visited, 0, sizeof(visited));
    memset(toClear, 0, sizeof(toClear));

    typedef struct { u8 x, y; } Pos;
    Pos queue[BOARD_W * BOARD_H];
    int qHead, qTail;

    for(int y=0; y<BOARD_H; y++)
    {
        for(int x=0; x<BOARD_W; x++)
        {
            if(board[y][x] == 0 || visited[y][x])
                continue;

            u8 color = board[y][x];
            qHead = qTail = 0;

            // BFS flood fill
            visited[y][x] = 1;
            queue[qTail++] = (Pos){ (u8)x, (u8)y };

            Pos cluster[BOARD_W * BOARD_H];
            int clusterSize = 0;

            while(qHead < qTail)
            {
                Pos p = queue[qHead++];
                cluster[clusterSize++] = p;

                static const int dx[4] = { 1, -1, 0, 0 };
                static const int dy[4] = { 0, 0, 1, -1 };

                for(int d=0; d<4; d++)
                {
                    int nx = p.x + dx[d];
                    int ny = p.y + dy[d];

                    if(nx < 0 || nx >= BOARD_W || ny < 0 || ny >= BOARD_H)
                        continue;
                    if(visited[ny][nx])
                        continue;
                    if(board[ny][nx] != color)
                        continue;

                    visited[ny][nx] = 1;
                    queue[qTail++] = (Pos){ (u8)nx, (u8)ny };
                }
            }

            if(clusterSize >= 4)
            {
                for(int i=0; i<clusterSize; i++)
                {
                    toClear[cluster[i].y][cluster[i].x] = 1;
                }
            }
        }
    }

    // Clear marked cells
    for(int y=0; y<BOARD_H; y++)
    {
        for(int x=0; x<BOARD_W; x++)
        {
            if(toClear[y][x])
            {
                board[y][x] = 0;
                clearedCount++;
            }
        }
    }

    if(clearedCount > 0)
        score += clearedCount * 10;

    return clearedCount;
}

// Apply gravity: make all puyos fall down in each column
static void apply_gravity(void)
{
    for(int x=0; x<BOARD_W; x++)
    {
        int writeY = BOARD_H - 1;
        for(int y=BOARD_H-1; y>=0; y--)
        {
            if(board[y][x] != 0)
            {
                if(y != writeY)
                {
                    board[writeY][x] = board[y][x];
                    board[y][x] = 0;
                }
                writeY--;
            }
        }
    }
}

// Resolve all matches and cascades
static void resolve_cascades(void)
{
    while(1)
    {
        int cleared = find_and_clear_matches();
        if(cleared <= 0)
            break;
        apply_gravity();
        // Optionally, you could insert a small delay/animation here
    }
}

// -----------------------------------------------------------------------------
// Game logic
// -----------------------------------------------------------------------------

static void spawn_new_pair(void)
{
    curPair.colorA = (u8)(1 + rand() % NUM_COLORS);
    curPair.colorB = (u8)(1 + rand() % NUM_COLORS);
    curPair.x      = BOARD_W / 2;
    curPair.y      = 1;
    curPair.orient = 0;      // second block above pivot
    curPair.active = 1;

    if(!can_place_pair(curPair.x, curPair.y, curPair.orient,
                       curPair.colorA, curPair.colorB))
    {
        // Cannot spawn => game over
        curPair.active = 0;
        gameOver = 1;
    }
}

static void init_game(void)
{
    memset(board, 0, sizeof(board));
    score     = 0;
    gameOver  = 0;
    fallTimer = 0;

    srand(1);       // fixed seed for repeatable behavior
    spawn_new_pair();
}

// Move pair horizontally if possible
static void move_pair_horiz(int dx)
{
    if(!curPair.active)
        return;

    int newX = curPair.x + dx;
    if(can_place_pair(newX, curPair.y, curPair.orient,
                      curPair.colorA, curPair.colorB))
    {
        curPair.x = newX;
    }
}

// Rotate pair clockwise if possible
static void rotate_pair(void)
{
    if(!curPair.active)
        return;

    int newOrient = (curPair.orient + 1) & 3;

    if(can_place_pair(curPair.x, curPair.y, newOrient,
                      curPair.colorA, curPair.colorB))
    {
        curPair.orient = newOrient;
    }
}

// Drop pair by one cell; lock if it can't fall further
static void step_fall(void)
{
    if(!curPair.active)
        return;

    if(can_place_pair(curPair.x, curPair.y+1, curPair.orient,
                      curPair.colorA, curPair.colorB))
    {
        curPair.y++;
    }
    else
    {
        // Lock into board
        int ax, ay, bx, by;
        get_pair_blocks(&curPair, &ax, &ay, &bx, &by, -1);

        if(ay >= 0 && ay < BOARD_H && ax >= 0 && ax < BOARD_W)
            board[ay][ax] = curPair.colorA;
        if(by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W)
            board[by][bx] = curPair.colorB;

        curPair.active = 0;

        // Resolve matches and cascades
        resolve_cascades();

        // Spawn next pair if not game over
        if(!gameOver)
            spawn_new_pair();
    }
}

// Called once per frame
static void update_game(void)
{
    if(gameOver)
    {
        // Press START to restart
        if(key_hit(KEY_START))
            init_game();
        return;
    }

    // Input
    if(key_is_down(KEY_LEFT))
        move_pair_horiz(-1);
    if(key_is_down(KEY_RIGHT))
        move_pair_horiz(+1);
    if(key_hit(KEY_A) || key_hit(KEY_UP))
        rotate_pair();

    // Soft drop
    int softDrop = key_is_down(KEY_DOWN) ? 1 : 0;

    // Falling timing
    fallTimer++;
    if(fallTimer >= FALL_DELAY || softDrop)
    {
        step_fall();
        fallTimer = 0;
    }
}

// -----------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------

static void draw_board(void)
{
    // Draw border
    int bw = BOARD_W * CELL_SIZE;
    int bh = BOARD_H * CELL_SIZE;
    draw_rect(BOARD_ORIGIN_X-1, BOARD_ORIGIN_Y-1, bw+2, bh+2, COLOR_BORDER);

    // Draw cells
    for(int y=0; y<BOARD_H; y++)
    {
        for(int x=0; x<BOARD_W; x++)
        {
            u8 c = board[y][x];
            if(c != 0)
            {
                int sx = BOARD_ORIGIN_X + x*CELL_SIZE;
                int sy = BOARD_ORIGIN_Y + y*CELL_SIZE;
                u16 clr = puyoColors[c];
                draw_rect(sx+1, sy+1, CELL_SIZE-2, CELL_SIZE-2, clr);
            }
        }
    }
}

static void draw_falling_pair(void)
{
    if(!curPair.active)
        return;

    int ax, ay, bx, by;
    get_pair_blocks(&curPair, &ax, &ay, &bx, &by, -1);

    // Draw pivot
    if(ax >= 0 && ax < BOARD_W && ay >= 0 && ay < BOARD_H)
    {
        int sx = BOARD_ORIGIN_X + ax*CELL_SIZE;
        int sy = BOARD_ORIGIN_Y + ay*CELL_SIZE;
        draw_rect(sx+1, sy+1, CELL_SIZE-2, CELL_SIZE-2,
                  puyoColors[curPair.colorA]);
    }

    // Draw second block
    if(bx >= 0 && bx < BOARD_W && by >= 0 && by < BOARD_H)
    {
        int sx = BOARD_ORIGIN_X + bx*CELL_SIZE;
        int sy = BOARD_ORIGIN_Y + by*CELL_SIZE;
        draw_rect(sx+1, sy+1, CELL_SIZE-2, CELL_SIZE-2,
                  puyoColors[curPair.colorB]);
    }
}

static void draw_game(void)
{
    clear_screen(COLOR_BG);
    draw_board();
    draw_falling_pair();

    if(gameOver)
        draw_game_over_text();
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

static inline void wait_vblank(void)
{
    // Busy-wait for start of VBlank (line >= 160)
    while(REG_VCOUNT >= 160);
    while(REG_VCOUNT < 160);
}

int main(void)
{
    // Set video mode: Mode 3, BG2 on
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    // Initialize keys & game state
    //key_init();
    init_game();

    while(1)
    {
        wait_vblank();
        key_poll();

        update_game();
        draw_game();
    }

    return 0;
}
