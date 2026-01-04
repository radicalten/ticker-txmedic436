/*
 * Panel de Pon / Puzzle League Clone for GBA
 * Uses TONC library
 * 
 * Build with devkitARM and TONC:
 * Makefile should link with -ltonc
 */

#include <tonc.h>
#include <string.h>

// ============================================================================
// Constants
// ============================================================================

#define GRID_COLS       6
#define GRID_ROWS       12
#define BLOCK_SIZE      16
#define GRID_X          72
#define GRID_Y          4
#define VISIBLE_ROWS    10

#define NUM_COLORS      5
#define EMPTY           0

#define STATE_IDLE      0
#define STATE_FALLING   1
#define STATE_CLEARING  2
#define STATE_HANG      3

#define CLEAR_FRAMES    45
#define HANG_FRAMES     12
#define RISE_SPEED_INIT 120
#define RISE_SPEED_MIN  20

// ============================================================================
// Types
// ============================================================================

typedef struct {
    u8 color;
    u8 state;
    u8 timer;
    u8 chain_id;
} Block;

typedef struct {
    Block grid[GRID_ROWS][GRID_COLS];
    u8 next_row[GRID_COLS];
    int cursor_x;
    int cursor_y;
    u32 score;
    u32 chain;
    u32 rise_counter;
    u32 rise_speed;
    int rise_offset;
    u8 stop_timer;
    BOOL game_over;
    BOOL paused;
    u32 frame_count;
} GameState;

// ============================================================================
// Globals
// ============================================================================

static GameState game;
static u32 rng_seed = 0x12345678;

// Block colors (5 colors + empty + flash)
static const u16 colors[] = {
    RGB15(2, 2, 4),     // 0: Empty/BG
    RGB15(31, 8, 8),    // 1: Red
    RGB15(8, 31, 8),    // 2: Green
    RGB15(8, 16, 31),   // 3: Blue
    RGB15(31, 31, 8),   // 4: Yellow
    RGB15(31, 8, 31),   // 5: Purple/Magenta
    RGB15(31, 31, 31),  // 6: Flash white
};

static const u16 colors_dark[] = {
    RGB15(1, 1, 2),
    RGB15(20, 4, 4),
    RGB15(4, 20, 4),
    RGB15(4, 10, 20),
    RGB15(20, 20, 4),
    RGB15(20, 4, 20),
    RGB15(20, 20, 20),
};

static const u16 colors_light[] = {
    RGB15(4, 4, 6),
    RGB15(31, 16, 16),
    RGB15(16, 31, 16),
    RGB15(16, 24, 31),
    RGB15(31, 31, 16),
    RGB15(31, 16, 31),
    RGB15(31, 31, 31),
};

// ============================================================================
// Utility Functions
// ============================================================================

static u32 rand_next(void) {
    rng_seed = rng_seed * 1103515245 + 12345;
    return (rng_seed >> 16) & 0x7FFF;
}

static u8 random_color(void) {
    return (rand_next() % NUM_COLORS) + 1;
}

static inline void plot_pixel(int x, int y, u16 color) {
    if (x >= 0 && x < 240 && y >= 0 && y < 160) {
        m3_mem[y][x] = color;
    }
}

static void fill_rect(int x, int y, int w, int h, u16 color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            plot_pixel(x + i, y + j, color);
        }
    }
}

static void draw_rect(int x, int y, int w, int h, u16 color) {
    for (int i = 0; i < w; i++) {
        plot_pixel(x + i, y, color);
        plot_pixel(x + i, y + h - 1, color);
    }
    for (int j = 0; j < h; j++) {
        plot_pixel(x, y + j, color);
        plot_pixel(x + w - 1, y + j, color);
    }
}

// Simple number drawing
static void draw_digit(int x, int y, int digit, u16 color) {
    static const u8 digits[10][5] = {
        {0x7, 0x5, 0x5, 0x5, 0x7}, // 0
        {0x2, 0x2, 0x2, 0x2, 0x2}, // 1
        {0x7, 0x1, 0x7, 0x4, 0x7}, // 2
        {0x7, 0x1, 0x7, 0x1, 0x7}, // 3
        {0x5, 0x5, 0x7, 0x1, 0x1}, // 4
        {0x7, 0x4, 0x7, 0x1, 0x7}, // 5
        {0x7, 0x4, 0x7, 0x5, 0x7}, // 6
        {0x7, 0x1, 0x1, 0x1, 0x1}, // 7
        {0x7, 0x5, 0x7, 0x5, 0x7}, // 8
        {0x7, 0x5, 0x7, 0x1, 0x7}, // 9
    };
    
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if (digits[digit][row] & (4 >> col)) {
                plot_pixel(x + col, y + row, color);
            }
        }
    }
}

static void draw_number(int x, int y, u32 num, u16 color) {
    char buf[12];
    int len = 0;
    
    if (num == 0) {
        draw_digit(x, y, 0, color);
        return;
    }
    
    u32 n = num;
    while (n > 0) {
        buf[len++] = n % 10;
        n /= 10;
    }
    
    for (int i = len - 1; i >= 0; i--) {
        draw_digit(x + (len - 1 - i) * 4, y, buf[i], color);
    }
}

// ============================================================================
// Drawing Functions
// ============================================================================

static void draw_block(int gx, int gy, u8 color_idx, int y_offset) {
    int px = GRID_X + gx * BLOCK_SIZE;
    int py = GRID_Y + gy * BLOCK_SIZE - y_offset;
    
    // Clip to visible area
    if (py < GRID_Y - BLOCK_SIZE || py >= GRID_Y + VISIBLE_ROWS * BLOCK_SIZE) {
        return;
    }
    
    u16 main_color = colors[color_idx];
    u16 dark = colors_dark[color_idx];
    u16 light = colors_light[color_idx];
    
    // Draw 3D-style block
    for (int by = 0; by < BLOCK_SIZE - 1; by++) {
        int screen_y = py + by;
        if (screen_y < GRID_Y || screen_y >= GRID_Y + VISIBLE_ROWS * BLOCK_SIZE) continue;
        
        for (int bx = 0; bx < BLOCK_SIZE - 1; bx++) {
            int screen_x = px + bx;
            u16 c;
            
            if (by < 2 || bx < 2) {
                c = light;
            } else if (by >= BLOCK_SIZE - 3 || bx >= BLOCK_SIZE - 3) {
                c = dark;
            } else {
                c = main_color;
            }
            
            plot_pixel(screen_x, screen_y, c);
        }
    }
}

static void draw_cursor(void) {
    int px = GRID_X + game.cursor_x * BLOCK_SIZE - 2;
    int py = GRID_Y + game.cursor_y * BLOCK_SIZE - game.rise_offset - 2;
    int w = BLOCK_SIZE * 2 + 3;
    int h = BLOCK_SIZE + 3;
    
    // Animated cursor color
    u16 cursor_color = (game.frame_count / 8) % 2 ? RGB15(31, 31, 31) : RGB15(20, 20, 31);
    
    // Draw thick cursor border
    for (int t = 0; t < 2; t++) {
        draw_rect(px + t, py + t, w - t * 2, h - t * 2, cursor_color);
    }
}

static void draw_grid_frame(void) {
    int x = GRID_X - 3;
    int y = GRID_Y - 3;
    int w = GRID_COLS * BLOCK_SIZE + 6;
    int h = VISIBLE_ROWS * BLOCK_SIZE + 6;
    
    // Frame
    for (int t = 0; t < 3; t++) {
        draw_rect(x + t, y + t, w - t * 2, h - t * 2, RGB15(10, 10, 15));
    }
    
    // Danger line (top)
    for (int i = 0; i < GRID_COLS * BLOCK_SIZE; i++) {
        if ((i / 4) % 2) {
            plot_pixel(GRID_X + i, GRID_Y, RGB15(31, 0, 0));
        }
    }
}

static void draw_game(void) {
    // Clear screen
    for (int y = 0; y < 160; y++) {
        for (int x = 0; x < 240; x++) {
            m3_mem[y][x] = RGB15(1, 1, 3);
        }
    }
    
    // Draw frame
    draw_grid_frame();
    
    // Draw blocks
    for (int gy = 0; gy < GRID_ROWS; gy++) {
        for (int gx = 0; gx < GRID_COLS; gx++) {
            Block *b = &game.grid[gy][gx];
            if (b->color != EMPTY) {
                u8 draw_color = b->color;
                
                // Flash when clearing
                if (b->state == STATE_CLEARING) {
                    if ((b->timer / 3) % 2) {
                        draw_color = 6; // White flash
                    }
                }
                
                draw_block(gx, gy, draw_color, game.rise_offset);
            }
        }
    }
    
    // Draw rising next row (preview)
    if (game.rise_offset > 0 && !game.paused) {
        for (int gx = 0; gx < GRID_COLS; gx++) {
            if (game.next_row[gx] != EMPTY) {
                int px = GRID_X + gx * BLOCK_SIZE;
                int py = GRID_Y + VISIBLE_ROWS * BLOCK_SIZE - game.rise_offset;
                
                u16 c = colors[game.next_row[gx]];
                for (int by = 0; by < game.rise_offset && by < BLOCK_SIZE; by++) {
                    for (int bx = 0; bx < BLOCK_SIZE - 1; bx++) {
                        plot_pixel(px + bx, py + by, c);
                    }
                }
            }
        }
    }
    
    // Draw cursor
    if (!game.game_over) {
        draw_cursor();
    }
    
    // Draw UI
    u16 text_color = RGB15(31, 31, 31);
    
    // Score label area
    fill_rect(4, 10, 60, 30, RGB15(3, 3, 6));
    draw_rect(4, 10, 60, 30, RGB15(10, 10, 15));
    draw_number(10, 20, game.score, text_color);
    
    // Chain display
    if (game.chain > 1) {
        fill_rect(4, 50, 60, 20, RGB15(6, 3, 3));
        draw_rect(4, 50, 60, 20, RGB15(15, 10, 10));
        draw_number(10, 56, game.chain, RGB15(31, 31, 0));
    }
    
    // Speed indicator
    fill_rect(180, 10, 55, 20, RGB15(3, 3, 6));
    draw_rect(180, 10, 55, 20, RGB15(10, 10, 15));
    draw_number(186, 16, game.rise_speed, text_color);
    
    // Game over overlay
    if (game.game_over) {
        fill_rect(60, 60, 120, 40, RGB15(20, 0, 0));
        draw_rect(60, 60, 120, 40, RGB15(31, 31, 31));
        draw_rect(61, 61, 118, 38, RGB15(31, 31, 31));
        
        // "GAME OVER" - simplified
        for (int i = 0; i < 80; i++) {
            plot_pixel(80 + i, 80, RGB15(31, 31, 31));
        }
    }
    
    // Paused overlay
    if (game.paused && !game.game_over) {
        fill_rect(80, 70, 80, 20, RGB15(0, 0, 15));
        draw_rect(80, 70, 80, 20, RGB15(31, 31, 31));
    }
}

// ============================================================================
// Game Logic
// ============================================================================

static void generate_next_row(void) {
    for (int x = 0; x < GRID_COLS; x++) {
        u8 color;
        int attempts = 0;
        do {
            color = random_color();
            attempts++;
        } while (attempts < 10 && 
                 x >= 2 && 
                 game.next_row[x-1] == color && 
                 game.next_row[x-2] == color);
        game.next_row[x] = color;
    }
}

static void init_game(void) {
    memset(&game, 0, sizeof(game));
    
    game.cursor_x = 2;
    game.cursor_y = 6;
    game.rise_speed = RISE_SPEED_INIT;
    
    // Seed RNG with frame count or timer
    rng_seed = REG_TM0CNT_L | ((u32)REG_TM1CNT_L << 16) | 0x12345;
    
    // Fill initial blocks (bottom 5-6 rows)
    for (int y = GRID_ROWS - 6; y < GRID_ROWS; y++) {
        for (int x = 0; x < GRID_COLS; x++) {
            u8 color;
            int attempts = 0;
            
            // Avoid creating initial matches
            do {
                color = random_color();
                attempts++;
                
                BOOL h_match = (x >= 2 && 
                               game.grid[y][x-1].color == color && 
                               game.grid[y][x-2].color == color);
                BOOL v_match = (y >= 2 && 
                               game.grid[y-1][x].color == color && 
                               game.grid[y-2][x].color == color);
                
                if (!h_match && !v_match) break;
            } while (attempts < 20);
            
            game.grid[y][x].color = color;
            game.grid[y][x].state = STATE_IDLE;
        }
    }
    
    generate_next_row();
}

static void swap_blocks(void) {
    int x = game.cursor_x;
    int y = game.cursor_y;
    
    Block *left = &game.grid[y][x];
    Block *right = &game.grid[y][x + 1];
    
    // Can't swap clearing blocks
    if (left->state == STATE_CLEARING || right->state == STATE_CLEARING) {
        return;
    }
    
    // Can swap falling blocks or one empty + one block
    u8 temp_color = left->color;
    u8 temp_state = left->state;
    
    left->color = right->color;
    left->state = (right->color == EMPTY) ? STATE_IDLE : right->state;
    
    right->color = temp_color;
    right->state = (temp_color == EMPTY) ? STATE_IDLE : temp_state;
}

static BOOL check_and_mark_matches(void) {
    BOOL match_grid[GRID_ROWS][GRID_COLS] = {0};
    BOOL found_any = FALSE;
    
    // Check horizontal matches
    for (int y = 0; y < GRID_ROWS; y++) {
        for (int x = 0; x < GRID_COLS - 2; x++) {
            Block *b = &game.grid[y][x];
            if (b->color == EMPTY || b->state != STATE_IDLE) continue;
            
            // Count matching blocks
            int count = 1;
            while (x + count < GRID_COLS && 
                   game.grid[y][x + count].color == b->color &&
                   game.grid[y][x + count].state == STATE_IDLE) {
                count++;
            }
            
            if (count >= 3) {
                for (int i = 0; i < count; i++) {
                    match_grid[y][x + i] = TRUE;
                }
                found_any = TRUE;
            }
        }
    }
    
    // Check vertical matches
    for (int x = 0; x < GRID_COLS; x++) {
        for (int y = 0; y < GRID_ROWS - 2; y++) {
            Block *b = &game.grid[y][x];
            if (b->color == EMPTY || b->state != STATE_IDLE) continue;
            
            int count = 1;
            while (y + count < GRID_ROWS && 
                   game.grid[y + count][x].color == b->color &&
                   game.grid[y + count][x].state == STATE_IDLE) {
                count++;
            }
            
            if (count >= 3) {
                for (int i = 0; i < count; i++) {
                    match_grid[y + i][x] = TRUE;
                }
                found_any = TRUE;
            }
        }
    }
    
    // Mark matched blocks for clearing
    if (found_any) {
        int cleared_count = 0;
        BOOL was_chain = FALSE;
        
        for (int y = 0; y < GRID_ROWS; y++) {
            for (int x = 0; x < GRID_COLS; x++) {
                if (match_grid[y][x]) {
                    if (game.grid[y][x].chain_id > 0) {
                        was_chain = TRUE;
                    }
                    game.grid[y][x].state = STATE_CLEARING;
                    game.grid[y][x].timer = CLEAR_FRAMES;
                    cleared_count++;
                }
            }
        }
        
        // Update chain counter
        if (was_chain || game.chain > 0) {
            game.chain++;
        } else {
            game.chain = 1;
        }
        
        // Calculate score
        int base_score = cleared_count * 10;
        int chain_bonus = (game.chain > 1) ? (game.chain - 1) * 50 : 0;
        int combo_bonus = (cleared_count > 3) ? (cleared_count - 3) * 20 : 0;
        
        game.score += base_score + chain_bonus + combo_bonus;
        
        // Add stop time when clearing
        game.stop_timer += CLEAR_FRAMES + cleared_count * 2;
        if (game.stop_timer > 180) game.stop_timer = 180;
    }
    
    return found_any;
}

static BOOL update_clearing(void) {
    BOOL any_clearing = FALSE;
    
    for (int y = 0; y < GRID_ROWS; y++) {
        for (int x = 0; x < GRID_COLS; x++) {
            Block *b = &game.grid[y][x];
            if (b->state == STATE_CLEARING) {
                any_clearing = TRUE;
                if (b->timer > 0) {
                    b->timer--;
                } else {
                    // Clear the block
                    b->color = EMPTY;
                    b->state = STATE_IDLE;
                    b->chain_id = 0;
                    
                    // Mark block above for hang time (chain detection)
                    if (y > 0 && game.grid[y-1][x].color != EMPTY) {
                        game.grid[y-1][x].state = STATE_HANG;
                        game.grid[y-1][x].timer = HANG_FRAMES;
                        game.grid[y-1][x].chain_id = game.chain;
                    }
                }
            }
        }
    }
    
    return any_clearing;
}

static BOOL apply_gravity(void) {
    BOOL any_falling = FALSE;
    
    // Process from bottom to top
    for (int y = GRID_ROWS - 2; y >= 0; y--) {
        for (int x = 0; x < GRID_COLS; x++) {
            Block *b = &game.grid[y][x];
            Block *below = &game.grid[y + 1][x];
            
            if (b->color == EMPTY) continue;
            if (b->state == STATE_CLEARING) continue;
            
            // Handle hang state
            if (b->state == STATE_HANG) {
                if (b->timer > 0) {
                    b->timer--;
                    continue;
                }
                b->state = STATE_IDLE;
            }
            
            // Check if can fall
            if (below->color == EMPTY && below->state == STATE_IDLE) {
                // Move block down
                below->color = b->color;
                below->state = STATE_FALLING;
                below->timer = 2;
                below->chain_id = b->chain_id;
                
                b->color = EMPTY;
                b->state = STATE_IDLE;
                b->chain_id = 0;
                
                any_falling = TRUE;
            }
        }
    }
    
    // Update falling blocks
    for (int y = 0; y < GRID_ROWS; y++) {
        for (int x = 0; x < GRID_COLS; x++) {
            Block *b = &game.grid[y][x];
            if (b->state == STATE_FALLING) {
                if (b->timer > 0) {
                    b->timer--;
                } else {
                    b->state = STATE_IDLE;
                }
                any_falling = TRUE;
            }
        }
    }
    
    return any_falling;
}

static void raise_stack(void) {
    // Check if top row has blocks (game over condition)
    for (int x = 0; x < GRID_COLS; x++) {
        if (game.grid[0][x].color != EMPTY) {
            game.game_over = TRUE;
            return;
        }
    }
    
    // Shift everything up
    for (int y = 0; y < GRID_ROWS - 1; y++) {
        for (int x = 0; x < GRID_COLS; x++) {
            game.grid[y][x] = game.grid[y + 1][x];
        }
    }
    
    // Add new row at bottom
    for (int x = 0; x < GRID_COLS; x++) {
        game.grid[GRID_ROWS - 1][x].color = game.next_row[x];
        game.grid[GRID_ROWS - 1][x].state = STATE_IDLE;
        game.grid[GRID_ROWS - 1][x].timer = 0;
        game.grid[GRID_ROWS - 1][x].chain_id = 0;
    }
    
    // Keep cursor in bounds
    if (game.cursor_y > 0) {
        game.cursor_y--;
    }
    
    // Generate next row
    generate_next_row();
    
    // Increase speed over time
    if (game.rise_speed > RISE_SPEED_MIN) {
        game.rise_speed--;
    }
}

static void update_rise(BOOL allow_rise) {
    if (!allow_rise || game.stop_timer > 0) {
        if (game.stop_timer > 0) game.stop_timer--;
        return;
    }
    
    game.rise_counter++;
    
    if (game.rise_counter >= game.rise_speed) {
        game.rise_counter = 0;
        game.rise_offset = 0;
        raise_stack();
    } else {
        game.rise_offset = (game.rise_counter * BLOCK_SIZE) / game.rise_speed;
    }
}

static void update_game(void) {
    if (game.game_over || game.paused) return;
    
    game.frame_count++;
    
    // Update clearing blocks
    BOOL clearing = update_clearing();
    
    // Apply gravity
    BOOL falling = apply_gravity();
    
    // Check for new matches (only when nothing is moving)
    if (!clearing && !falling) {
        BOOL new_matches = check_and_mark_matches();
        
        // Reset chain if no action
        if (!new_matches) {
            // Clear chain IDs
            for (int y = 0; y < GRID_ROWS; y++) {
                for (int x = 0; x < GRID_COLS; x++) {
                    game.grid[y][x].chain_id = 0;
                }
            }
            game.chain = 0;
        }
    }
    
    // Rising stack (pause during clears/falls)
    update_rise(!clearing && !falling);
}

// ============================================================================
// Input Handling
// ============================================================================

static void handle_input(void) {
    key_poll();
    
    if (game.game_over) {
        if (key_hit(KEY_START) || key_hit(KEY_A)) {
            init_game();
        }
        return;
    }
    
    // Pause
    if (key_hit(KEY_START)) {
        game.paused = !game.paused;
        return;
    }
    
    if (game.paused) return;
    
    // Cursor movement
    if (key_hit(KEY_LEFT) || key_is_down(KEY_LEFT) && (game.frame_count % 6 == 0)) {
        if (game.cursor_x > 0) game.cursor_x--;
    }
    if (key_hit(KEY_RIGHT) || key_is_down(KEY_RIGHT) && (game.frame_count % 6 == 0)) {
        if (game.cursor_x < GRID_COLS - 2) game.cursor_x++;
    }
    if (key_hit(KEY_UP) || key_is_down(KEY_UP) && (game.frame_count % 6 == 0)) {
        if (game.cursor_y > 0) game.cursor_y--;
    }
    if (key_hit(KEY_DOWN) || key_is_down(KEY_DOWN) && (game.frame_count % 6 == 0)) {
        if (game.cursor_y < GRID_ROWS - 2) game.cursor_y++;
    }
    
    // Swap blocks
    if (key_hit(KEY_A) || key_hit(KEY_B)) {
        swap_blocks();
    }
    
    // Manual speed up
    if (key_is_down(KEY_R) || key_is_down(KEY_L)) {
        if (game.stop_timer == 0) {
            game.rise_counter += 3;
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    // Initialize interrupts
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    // Set up display: Mode 3, enable BG2
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;
    
    // Initialize timer for RNG seed
    REG_TM0CNT_L = 0;
    REG_TM0CNT_H = TM_ENABLE;
    REG_TM1CNT_L = 0;
    REG_TM1CNT_H = TM_ENABLE | TM_CASCADE;
    
    // Initialize game
    init_game();
    
    // Main loop
    while (1) {
        VBlankIntrWait();
        
        handle_input();
        update_game();
        draw_game();
    }
    
    return 0;
}
