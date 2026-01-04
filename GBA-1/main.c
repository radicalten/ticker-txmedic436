// panel_de_pon_like.c
// Simple single-file Panel de Pon / Puzzle League–style game for GBA using tonc.
//
// Mode 3 bitmap, no sprites or sound.
// Uses tonc for REGs, RGB15_C, input, etc.

#include <tonc.h>
#include <stdbool.h>
#define RGB15_C(r,g,b)  ((r) | ((g) << 5) | ((b) << 10))

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  160

#define BOARD_W        6
#define BOARD_H        12
#define BLOCK_SIZE     12        // pixel size of each block (square)

// Center the grid on screen
#define GRID_ORIGIN_X  ((SCREEN_WIDTH  - BOARD_W*BLOCK_SIZE)/2)
#define GRID_ORIGIN_Y  ((SCREEN_HEIGHT - BOARD_H*BLOCK_SIZE)/2)

#define NUM_COLORS     5         // number of block colors
#define RAISE_DELAY    90        // frames per automatic raise (~1.5s at 60fps)

// Colors
#define COLOR_BG       RGB15_C(0, 0, 2)
#define COLOR_CELL_BG  RGB15_C(3, 3, 5)
#define COLOR_CURSOR   RGB15_C(31, 31, 31)
#define COLOR_GAME_OVER_BORDER RGB15_C(31, 0, 0)

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

static u8  board[BOARD_H][BOARD_W];      // 0 = empty, 1..NUM_COLORS = block colors
static int cursor_x;                     // left block of 2-wide cursor
static int cursor_y;
static int raise_timer;                  // frame counter for raising
static bool game_over;
static u32 rng_state = 1;

// Block color table (index 0 unused/empty)
static const u16 block_colors[NUM_COLORS+1] =
{
    RGB15_C(0, 0, 0),        // 0: empty (not actually used for drawing)
    RGB15_C(31, 0, 0),       // 1: red
    RGB15_C(0, 31, 0),       // 2: green
    RGB15_C(0, 0, 31),       // 3: blue
    RGB15_C(31, 31, 0),      // 4: yellow
    RGB15_C(31, 0, 31)       // 5: magenta
};

// -----------------------------------------------------------------------------
// Prototypes
// -----------------------------------------------------------------------------

static void vsync(void);
static void clear_screen(u16 color);
static void draw_rect_filled(int x, int y, int w, int h, u16 color);
static void draw_rect_outline(int x, int y, int w, int h, u16 color);
static void draw_board(void);
static void draw_cursor(void);
static void draw_frame(void);
static void reset_game(void);
static bool clear_matches_once(void);
static void apply_gravity(void);
static bool process_all_matches(void);
static bool can_raise(void);
static void raise_stack(void);
static u32  rand_u32(void);
static int  rand_int(int max);
static void update_game(void);

// -----------------------------------------------------------------------------
// Basic helpers
// -----------------------------------------------------------------------------

// Wait for start of VBlank
static void vsync(void)
{
    while (REG_VCOUNT >= 160);
    while (REG_VCOUNT < 160);
}

// Simple RNG (LCG)
static u32 rand_u32(void)
{
    rng_state = rng_state * 1664525 + 1013904223;
    return rng_state;
}

static int rand_int(int max)
{
    // 0 .. max-1
    return (int)(rand_u32() % (u32)max);
}

// -----------------------------------------------------------------------------
// Drawing helpers (Mode 3)
// -----------------------------------------------------------------------------

static void draw_rect_filled(int x, int y, int w, int h, u16 color)
{
    if (w <= 0 || h <= 0) return;

    // Clip to screen
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH)  w = SCREEN_WIDTH  - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    u16 *base = (u16*)VRAM;
    for (int j = 0; j < h; j++)
    {
        u16 *row = base + (y + j)*SCREEN_WIDTH + x;
        for (int i = 0; i < w; i++)
            row[i] = color;
    }
}

static void draw_rect_outline(int x, int y, int w, int h, u16 color)
{
    if (w <= 0 || h <= 0) return;

    // Top
    draw_rect_filled(x, y, w, 1, color);
    // Bottom
    draw_rect_filled(x, y + h - 1, w, 1, color);
    // Left
    draw_rect_filled(x, y, 1, h, color);
    // Right
    draw_rect_filled(x + w - 1, y, 1, h, color);
}

// -----------------------------------------------------------------------------
// Board logic
// -----------------------------------------------------------------------------

static void apply_gravity(void)
{
    for (int x = 0; x < BOARD_W; x++)
    {
        int dst = BOARD_H - 1;  // destination from bottom
        for (int y = BOARD_H - 1; y >= 0; y--)
        {
            u8 c = board[y][x];
            if (c != 0)
            {
                board[dst][x] = c;
                if (dst != y)
                    board[y][x] = 0;
                dst--;
            }
        }
        // Clear any remaining cells above dst
        for (int y = dst; y >= 0; y--)
            board[y][x] = 0;
    }
}

// Find and clear all matches of length >= 3.
// Returns true if any blocks were cleared.
static bool clear_matches_once(void)
{
    u8 mark[BOARD_H][BOARD_W];
    for (int y = 0; y < BOARD_H; y++)
        for (int x = 0; x < BOARD_W; x++)
            mark[y][x] = 0;

    bool any = false;

    // Horizontal runs
    for (int y = 0; y < BOARD_H; y++)
    {
        int x = 0;
        while (x < BOARD_W)
        {
            u8 c = board[y][x];
            if (c == 0)
            {
                x++;
                continue;
            }
            int start = x;
            x++;
            while (x < BOARD_W && board[y][x] == c)
                x++;
            int run_len = x - start;
            if (run_len >= 3)
            {
                any = true;
                for (int i = start; i < x; i++)
                    mark[y][i] = 1;
            }
        }
    }

    // Vertical runs
    for (int x = 0; x < BOARD_W; x++)
    {
        int y = 0;
        while (y < BOARD_H)
        {
            u8 c = board[y][x];
            if (c == 0)
            {
                y++;
                continue;
            }
            int start = y;
            y++;
            while (y < BOARD_H && board[y][x] == c)
                y++;
            int run_len = y - start;
            if (run_len >= 3)
            {
                any = true;
                for (int i = start; i < y; i++)
                    mark[i][x] = 1;
            }
        }
    }

    if (!any)
        return false;

    // Clear marked blocks
    for (int y = 0; y < BOARD_H; y++)
        for (int x = 0; x < BOARD_W; x++)
            if (mark[y][x])
                board[y][x] = 0;

    return true;
}

// Repeatedly clear matches and apply gravity until stable.
// Returns true if any blocks were cleared at all.
static bool process_all_matches(void)
{
    bool any = false;
    while (true)
    {
        if (!clear_matches_once())
            break;
        any = true;
        apply_gravity();
    }
    return any;
}

// Can we raise the stack without overflowing the top row?
static bool can_raise(void)
{
    for (int x = 0; x < BOARD_W; x++)
    {
        if (board[0][x] != 0)
            return false;
    }
    return true;
}

// Raise the stack: shift all rows up, create a new bottom row.
// If the top row is occupied, sets game_over instead.
static void raise_stack(void)
{
    if (!can_raise())
    {
        game_over = true;
        return;
    }

    // Shift all rows up
    for (int y = 0; y < BOARD_H - 1; y++)
    {
        for (int x = 0; x < BOARD_W; x++)
            board[y][x] = board[y+1][x];
    }

    // Generate new bottom row with no immediate 3-in-a-row
    int y = BOARD_H - 1;
    for (int x = 0; x < BOARD_W; x++)
    {
        u8 color;
        do
        {
            color = 1 + rand_int(NUM_COLORS);
        }
        while (
            // Avoid horizontal 3+ within this new row
            (x >= 2 && board[y][x-1] == color && board[y][x-2] == color) ||
            // Avoid vertical 3+ ending at (y,x)
            (y >= 2 && board[y-1][x] == color && board[y-2][x] == color)
        );
        board[y][x] = color;
    }
}

// -----------------------------------------------------------------------------
// Game setup/reset
// -----------------------------------------------------------------------------

static void reset_game(void)
{
    // Clear board
    for (int y = 0; y < BOARD_H; y++)
        for (int x = 0; x < BOARD_W; x++)
            board[y][x] = 0;

    // Fill some bottom rows with random blocks, without starting matches
    int initial_rows = 6;  // half of the board
    for (int y = BOARD_H - 1; y >= BOARD_H - initial_rows; y--)
    {
        for (int x = 0; x < BOARD_W; x++)
        {
            u8 color;
            do
            {
                color = 1 + rand_int(NUM_COLORS);
            }
            while (
                // Avoid horizontal 3+
                (x >= 2 && board[y][x-1] == color && board[y][x-2] == color) ||
                // Avoid vertical 3+ (this row is above two others)
                (y <= BOARD_H-3 && board[y+1][x] == color && board[y+2][x] == color)
            );
            board[y][x] = color;
        }
    }

    // Center cursor near bottom
    cursor_x = BOARD_W/2 - 1;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_x > BOARD_W-2) cursor_x = BOARD_W-2;
    cursor_y = BOARD_H - 3;

    raise_timer = 0;
    game_over = false;
}

// -----------------------------------------------------------------------------
// Drawing the board and cursor
// -----------------------------------------------------------------------------

static void draw_board(void)
{
    // Draw each cell
    for (int y = 0; y < BOARD_H; y++)
    {
        for (int x = 0; x < BOARD_W; x++)
        {
            int px = GRID_ORIGIN_X + x*BLOCK_SIZE;
            int py = GRID_ORIGIN_Y + y*BLOCK_SIZE;

            // Cell background
            draw_rect_filled(px, py, BLOCK_SIZE-1, BLOCK_SIZE-1, COLOR_CELL_BG);

            if (board[y][x] != 0)
            {
                u16 col = block_colors[board[y][x]];
                draw_rect_filled(px+1, py+1, BLOCK_SIZE-3, BLOCK_SIZE-3, col);
            }
            else
            {
                // Empty cell interior
                draw_rect_filled(px+1, py+1, BLOCK_SIZE-3, BLOCK_SIZE-3, COLOR_BG);
            }
        }
    }

    // Outer border around grid
    draw_rect_outline(
        GRID_ORIGIN_X-1, GRID_ORIGIN_Y-1,
        BOARD_W*BLOCK_SIZE+2, BOARD_H*BLOCK_SIZE+2,
        COLOR_CELL_BG
    );
}

static void draw_cursor(void)
{
    int x = cursor_x;
    int y = cursor_y;

    int px = GRID_ORIGIN_X + x*BLOCK_SIZE;
    int py = GRID_ORIGIN_Y + y*BLOCK_SIZE;
    int w  = BLOCK_SIZE*2 - 1;
    int h  = BLOCK_SIZE - 1;

    draw_rect_outline(px, py, w, h, COLOR_CURSOR);
}

static void draw_frame(void)
{
    clear_screen(COLOR_BG);
    draw_board();
    draw_cursor();

    if (game_over)
    {
        // Draw a double red border to indicate game over
        draw_rect_outline(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_GAME_OVER_BORDER);
        draw_rect_outline(2, 2, SCREEN_WIDTH-4, SCREEN_HEIGHT-4, COLOR_GAME_OVER_BORDER);
    }
}

// -----------------------------------------------------------------------------
// Input & game update
// -----------------------------------------------------------------------------

static void update_game(void)
{
    if (game_over)
    {
        // Press START to reset after game over
        if (key_hit(KEY_START))
            reset_game();
        return;
    }

    // Movement: cursor selects left block of a 2-wide pair
    if (key_hit(KEY_LEFT) && cursor_x > 0)
        cursor_x--;
    if (key_hit(KEY_RIGHT) && cursor_x < BOARD_W-2)
        cursor_x++;
    if (key_hit(KEY_UP) && cursor_y > 0)
        cursor_y--;
    if (key_hit(KEY_DOWN) && cursor_y < BOARD_H-1)
        cursor_y++;

    // Swap blocks under cursor with A
    if (key_hit(KEY_A))
    {
        int x1 = cursor_x;
        int y1 = cursor_y;
        int x2 = x1 + 1;

        // Perform swap
        u8 tmp = board[y1][x1];
        board[y1][x1] = board[y1][x2];
        board[y1][x2] = tmp;

        // If no matches result, undo swap
        if (!process_all_matches())
        {
            tmp = board[y1][x1];
            board[y1][x1] = board[y1][x2];
            board[y1][x2] = tmp;
        }
    }

    // Stack raising
    raise_timer++;
    // Hold R to speed up raising
    if (key_is_down(KEY_R))
        raise_timer += 2;

    if (raise_timer >= RAISE_DELAY)
    {
        raise_timer = 0;
        raise_stack();          // may set game_over
        if (!game_over)
            process_all_matches();
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(void)
{
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    rng_state = 1;      // fixed seed; change if you want different patterns
    reset_game();

    while (1)
    {
        vsync();
        key_poll();
        update_game();
        draw_frame();
    }

    return 0;
}
