// A single-file, dependency-free C chess program for the Mac Terminal.
// By OpenAI's GPT-4
// Compile with: cc chess.c -o chess
//
// FEATURES:
// - Terminal-based GUI with in-place updates.
// - Unicode chess pieces.
// - User piece selection and movement via keyboard.
// - Full legal move generation and enforcement (including castling, en-passant, promotions).
// - Highlighting for: selected piece, legal moves, last move, and king in check.
// - UCI engine integration for AI opponent.
// - Configurable engine path and time controls (depth, time, nodes).
// - Move undo/take back.
// - PGN-like move history display.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <time.h>

// --- CONFIGURATION ---
// IMPORTANT: Change this to the absolute path of your UCI engine executable.
#define DEFAULT_ENGINE_PATH "/opt/homebrew/bin/stockfish" 

// --- MACROS AND CONSTANTS ---

// Piece representation: 4 bits for type, 1 bit for color
#define PIECE_TYPE_MASK 0b01111
#define PIECE_COLOR_MASK 0b10000

#define EMPTY   0
#define PAWN    1
#define KNIGHT  2
#define BISHOP  3
#define ROOK    4
#define QUEEN   5
#define KING    6

#define WHITE   0b00000
#define BLACK   0b10000

// Game state
#define IN_PROGRESS 0
#define CHECKMATE   1
#define STALEMATE   2

// Castling rights (bitmask)
#define CASTLE_WK 1
#define CASTLE_WQ 2
#define CASTLE_BK 4
#define CASTLE_BQ 8

// ANSI escape codes for terminal graphics
#define CLEAR_SCREEN()      printf("\x1b[2J")
#define HIDE_CURSOR()       printf("\x1b[?25l")
#define SHOW_CURSOR()       printf("\x1b[?25h")
#define MOVE_CURSOR(r, c)   printf("\x1b[%d;%dH", r, c)
#define SET_BG_COLOR_RGB(r, g, b) printf("\x1b[48;2;%d;%d;%dm", r, g, b)
#define SET_FG_COLOR_RGB(r, g, b) printf("\x1b[38;2;%d;%d;%dm", r, g, b)
#define RESET_COLOR()       printf("\x1b[0m")

// --- STRUCT DEFINITIONS ---

typedef struct {
    int from;           // 0-63
    int to;             // 0-63
    int promotion;      // Piece type to promote to, or EMPTY
} Move;

typedef struct {
    Move move;
    int captured_piece;
    int old_en_passant_sq;
    int old_castling_rights;
    int old_halfmove_clock;
} HistoryEntry;

typedef struct {
    int board[64];
    int turn;           // WHITE or BLACK
    int castling_rights;
    int en_passant_sq;  // 0-63, or -1 if none
    int halfmove_clock;
    int fullmove_number;

    HistoryEntry history[1024];
    int history_count;

    char pgn_history[1024][16];
    int pgn_count;

    int game_over_status;
} GameState;

// --- GLOBAL VARIABLES & STATE ---

// Terminal I/O
static struct termios orig_termios;

// Game and UI state
static GameState gs;
static int cursor_x = 0, cursor_y = 0;
static int selected_sq = -1;
static Move legal_moves[256];
static int num_legal_moves = 0;
static Move last_move = {-1, -1, 0};

// UCI engine communication
static FILE* engine_in;
static FILE* engine_out;
static char engine_path[256];
enum { MODE_DEPTH, MODE_MOVETIME, MODE_NODES } time_control_mode = MODE_DEPTH;
static int time_control_value = 5; // Default depth 5

// --- FUNCTION PROTOTYPES ---

// Terminal and Graphics
void enable_raw_mode();
void disable_raw_mode();
void draw_board();
void draw_square(int r, int c, int is_cursor, int is_selected, int is_legal_dest, int is_last_move, int is_check);
void draw_info_panel();
void update_status(const char* msg);

// Chess Logic
void init_game();
int get_piece_type(int piece);
int get_piece_color(int piece);
int is_king_in_check(int king_color);
int is_square_attacked(int sq, int attacker_color);
void generate_moves();
int make_move(Move m);
void undo_last_move();
void move_to_pgn(Move m, char* pgn_str);

// UCI Engine
int uci_init();
void uci_quit();
Move uci_get_move();
void uci_send_position();

// Main loop
int get_key();

// --- MAIN FUNCTION ---

int main() {
    strcpy(engine_path, DEFAULT_ENGINE_PATH);

    enable_raw_mode();
    srand(time(NULL));

    if (!uci_init()) {
        disable_raw_mode();
        fprintf(stderr, "Failed to initialize UCI engine. Check path: %s\n", engine_path);
        return 1;
    }

    init_game();
    CLEAR_SCREEN();
    HIDE_CURSOR();
    
    draw_board();
    draw_info_panel();
    update_status("Welcome to C-Chess! Use arrows, Enter to move. 'q' to quit.");

    while (1) {
        if (gs.game_over_status != IN_PROGRESS) {
            // Game is over, wait for 'q' or 'u'
            int key = get_key();
            if (key == 'q') break;
            if (key == 'u') {
                undo_last_move();
                draw_board();
                draw_info_panel();
                update_status("Move undone.");
                continue;
            }
            continue;
        }

        // --- AI'S TURN ---
        if (gs.turn == BLACK) {
            update_status("Engine is thinking...");
            Move engine_move = uci_get_move();
            if (engine_move.from == -1) {
                 update_status("Engine failed to provide a move.");
                 sleep(2);
            } else {
                make_move(engine_move);
                draw_board();
                draw_info_panel();
                generate_moves(); // Generate moves for the human player
                if (gs.game_over_status == CHECKMATE) update_status("Checkmate! You lose.");
                else if (gs.game_over_status == STALEMATE) update_status("Stalemate! It's a draw.");
                else if (is_king_in_check(WHITE)) update_status("Check! Your turn.");
                else update_status("Engine moved. Your turn.");
            }
            continue;
        }

        // --- PLAYER'S TURN ---
        int key = get_key();
        int current_sq = cursor_y * 8 + cursor_x;

        switch (key) {
            case 'q':
                goto game_end;
            case 'u':
                undo_last_move(); // Undo player move
                if (gs.history_count > 0) undo_last_move(); // Undo engine move
                selected_sq = -1;
                draw_board();
                draw_info_panel();
                update_status("Two moves undone.");
                generate_moves();
                break;
            case 't':
                time_control_mode = (time_control_mode + 1) % 3;
                if(time_control_mode == MODE_DEPTH) { time_control_value = 5; update_status("AI Mode: Depth 5"); }
                if(time_control_mode == MODE_MOVETIME) { time_control_value = 2000; update_status("AI Mode: 2s/move"); }
                if(time_control_mode == MODE_NODES) { time_control_value = 500000; update_status("AI Mode: 500k nodes"); }
                break;
            case 27: // Escape sequence
                if (get_key() == 91) { // Arrow key prefix
                    int old_r = cursor_y, old_c = cursor_x;
                    switch (get_key()) {
                        case 'A': if (cursor_y > 0) cursor_y--; break; // Up
                        case 'B': if (cursor_y < 7) cursor_y++; break; // Down
                        case 'C': if (cursor_x < 7) cursor_x++; break; // Right
                        case 'D': if (cursor_x > 0) cursor_x--; break; // Left
                    }
                    draw_square(old_r, old_c, 0, selected_sq == old_r*8+old_c, 0, 0, 0);
                    draw_square(cursor_y, cursor_x, 1, 0, 0, 0, 0);
                } else { // Plain escape
                    if (selected_sq != -1) {
                        selected_sq = -1;
                        draw_board(); // Redraw to remove legal move highlights
                        update_status("Selection cancelled.");
                    }
                }
                break;
            case '\n': // Enter key
                if (selected_sq == -1) {
                    // Try to select a piece
                    if (get_piece_color(gs.board[current_sq]) == gs.turn) {
                        selected_sq = current_sq;
                        draw_board();
                        update_status("Piece selected. Move to a highlighted square.");
                    }
                } else {
                    // Try to move a piece
                    Move m = {selected_sq, current_sq, 0};
                    int is_legal = 0;
                    for (int i = 0; i < num_legal_moves; i++) {
                        if (legal_moves[i].from == m.from && legal_moves[i].to == m.to) {
                            // Handle promotion
                            if(get_piece_type(gs.board[m.from]) == PAWN && ( (m.to / 8) == 0 || (m.to / 8) == 7) ){
                                m.promotion = QUEEN; // Auto-promote to Queen for simplicity
                            }
                            is_legal = 1;
                            break;
                        }
                    }

                    if (is_legal) {
                        make_move(m);
                        selected_sq = -1;
                        draw_board();
                        draw_info_panel();
                        
                        generate_moves(); // Generate moves for the engine
                        if(gs.game_over_status == CHECKMATE) update_status("Checkmate! Engine loses.");
                        else if (gs.game_over_status == STALEMATE) update_status("Stalemate! It's a draw.");
                        else update_status("Move made. Waiting for engine...");

                    } else {
                        update_status("Illegal move. Try again.");
                        selected_sq = -1;
                        draw_board();
                    }
                }
                break;
        }
    }

game_end:
    uci_quit();
    disable_raw_mode();
    MOVE_CURSOR(18, 1);
    SHOW_CURSOR();
    RESET_COLOR();
    printf("Thanks for playing!\n");
    return 0;
}


// --- TERMINAL AND GRAPHICS IMPLEMENTATION ---

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

int get_key() {
    int c;
    while ((c = getchar()) == EOF);
    return c;
}

const char* unicode_pieces[2][7] = {
    {" ", "♙", "♘", "♗", "♖", "♕", "♔"}, // White
    {" ", "♟︎", "♞", "♝", "♜", "♛", "♚"}  // Black
};

void draw_square(int r, int c, int is_cursor, int is_selected, int is_legal_dest, int is_last_move, int is_check) {
    int term_r = r * 2 + 1;
    int term_c = c * 4 + 3;

    // Determine background color
    if (is_cursor) SET_BG_COLOR_RGB(100, 100, 200); // Cursor highlight
    else if (is_check) SET_BG_COLOR_RGB(255, 100, 100); // Check highlight
    else if (is_selected) SET_BG_COLOR_RGB(255, 255, 0); // Selected piece
    else if (is_last_move) SET_BG_COLOR_RGB(173, 216, 230); // Light blue for last move
    else if (is_legal_dest) SET_BG_COLOR_RGB(144, 238, 144); // Light green for legal move
    else if ((r + c) % 2 == 0) SET_BG_COLOR_RGB(240, 217, 181); // Light square
    else SET_BG_COLOR_RGB(181, 136, 99); // Dark square

    int piece = gs.board[r * 8 + c];
    int type = get_piece_type(piece);
    int color = get_piece_color(piece) == BLACK ? 1 : 0;

    // Set piece color
    SET_FG_COLOR_RGB(0, 0, 0);

    for (int i = 0; i < 2; i++) {
        MOVE_CURSOR(term_r + i, term_c);
        if (i == 0 && type != EMPTY) {
            printf(" %s  ", unicode_pieces[color][type]);
        } else {
            printf("    ");
        }
    }
}

void draw_board() {
    int king_sq = -1;
    if (is_king_in_check(gs.turn)) {
        int king_piece = KING | gs.turn;
        for (int i = 0; i < 64; i++) {
            if (gs.board[i] == king_piece) {
                king_sq = i;
                break;
            }
        }
    }

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int is_legal = 0;
            if (selected_sq != -1) {
                for (int i = 0; i < num_legal_moves; i++) {
                    if (legal_moves[i].from == selected_sq && legal_moves[i].to == sq) {
                        is_legal = 1;
                        break;
                    }
                }
            }
            int is_last = (sq == last_move.from || sq == last_move.to);
            draw_square(r, c, (r == cursor_y && c == cursor_x), (sq == selected_sq), is_legal, is_last, (sq == king_sq));
        }
    }
    RESET_COLOR();
    MOVE_CURSOR(18, 1); // Move cursor out of the way
}

void draw_info_panel() {
    MOVE_CURSOR(1, 40);
    RESET_COLOR();
    printf("C-CHESS TERMINAL");

    MOVE_CURSOR(3, 40);
    printf("Turn: %-15s", gs.turn == WHITE ? "White" : "Black");

    MOVE_CURSOR(4, 40);
    printf("Move: %-15d", gs.fullmove_number);
    
    MOVE_CURSOR(6, 40);
    printf("Move History:");
    
    for (int i = 0; i < 10; i++) {
        MOVE_CURSOR(7 + i, 40);
        printf("                      "); // Clear line
        MOVE_CURSOR(7 + i, 40);
        if (gs.pgn_count > i) {
            int index = gs.pgn_count - 1 - i;
            int move_num = index / 2 + 1;
            if(index % 2 == 0) {
                 printf("%d. %s", move_num, gs.pgn_history[index]);
            } else {
                 printf("   %s", gs.pgn_history[index]);
            }
        }
    }
}

void update_status(const char* msg) {
    MOVE_CURSOR(18, 1);
    RESET_COLOR();
    printf("\x1b[K"); // Clear line
    printf("%s", msg);
}


// --- CHESS LOGIC IMPLEMENTATION ---

void init_game() {
    // Initial board setup (FEN: rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1)
    int initial_board[64] = {
        ROOK|BLACK, KNIGHT|BLACK, BISHOP|BLACK, QUEEN|BLACK, KING|BLACK, BISHOP|BLACK, KNIGHT|BLACK, ROOK|BLACK,
        PAWN|BLACK, PAWN|BLACK,   PAWN|BLACK,   PAWN|BLACK,  PAWN|BLACK, PAWN|BLACK,   PAWN|BLACK,   PAWN|BLACK,
        EMPTY,      EMPTY,        EMPTY,        EMPTY,       EMPTY,      EMPTY,        EMPTY,        EMPTY,
        EMPTY,      EMPTY,        EMPTY,        EMPTY,       EMPTY,      EMPTY,        EMPTY,        EMPTY,
        EMPTY,      EMPTY,        EMPTY,        EMPTY,       EMPTY,      EMPTY,        EMPTY,        EMPTY,
        EMPTY,      EMPTY,        EMPTY,        EMPTY,       EMPTY,      EMPTY,        EMPTY,        EMPTY,
        PAWN|WHITE, PAWN|WHITE,   PAWN|WHITE,   PAWN|WHITE,  PAWN|WHITE, PAWN|WHITE,   PAWN|WHITE,   PAWN|WHITE,
        ROOK|WHITE, KNIGHT|WHITE, BISHOP|WHITE, QUEEN|WHITE, KING|WHITE, BISHOP|WHITE, KNIGHT|WHITE, ROOK|WHITE
    };
    memcpy(gs.board, initial_board, sizeof(initial_board));

    gs.turn = WHITE;
    gs.castling_rights = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    gs.en_passant_sq = -1;
    gs.halfmove_clock = 0;
    gs.fullmove_number = 1;
    gs.history_count = 0;
    gs.pgn_count = 0;
    gs.game_over_status = IN_PROGRESS;
    last_move = (Move){-1, -1, 0};
    
    generate_moves();
}

int get_piece_type(int piece) { return piece & PIECE_TYPE_MASK; }
int get_piece_color(int piece) { return piece & PIECE_COLOR_MASK; }

int is_square_attacked(int sq, int attacker_color) {
    // Check for pawn attacks
    int pawn_dir = (attacker_color == WHITE) ? 1 : -1;
    if (sq/8 - pawn_dir >= 0 && sq/8 - pawn_dir < 8) {
        if (sq%8 > 0 && gs.board[sq - 8*pawn_dir - 1] == (PAWN | attacker_color)) return 1;
        if (sq%8 < 7 && gs.board[sq - 8*pawn_dir + 1] == (PAWN | attacker_color)) return 1;
    }

    // Check for knight attacks
    int knight_moves[] = {-17, -15, -10, -6, 6, 10, 15, 17};
    for (int i=0; i<8; i++) {
        int target_sq = sq + knight_moves[i];
        if (target_sq >= 0 && target_sq < 64 && abs((sq%8)-(target_sq%8)) <= 2) {
             if (gs.board[target_sq] == (KNIGHT | attacker_color)) return 1;
        }
    }

    // Check for sliding pieces (rook, bishop, queen) and king
    int directions[] = {-9, -8, -7, -1, 1, 7, 8, 9}; // Bishop/Rook/Queen directions
    for (int i = 0; i < 8; i++) {
        for (int d = 1; d < 8; d++) {
            int target_sq = sq + directions[i] * d;
            int r = target_sq/8, c = target_sq%8;
            int pr = sq/8, pc = sq%8;

            if (target_sq < 0 || target_sq >= 64) break;
            if (abs(c - (pc + (directions[i] % 8) * d)) > 1) break; // Off board wrap around check

            int piece = gs.board[target_sq];
            if (piece != EMPTY) {
                if (get_piece_color(piece) == attacker_color) {
                    int type = get_piece_type(piece);
                    if (type == KING && d == 1) return 1;
                    if ( (type==ROOK || type==QUEEN) && (abs(directions[i])==1 || abs(directions[i])==8) ) return 1;
                    if ( (type==BISHOP || type==QUEEN) && (abs(directions[i])==7 || abs(directions[i])==9) ) return 1;
                }
                break; // Path blocked
            }
        }
    }
    return 0;
}

int is_king_in_check(int king_color) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (gs.board[i] == (KING | king_color)) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return 0; // Should not happen
    return is_square_attacked(king_sq, king_color == WHITE ? BLACK : WHITE);
}

void generate_moves() {
    num_legal_moves = 0;
    int temp_board[64], temp_cr, temp_ep;

    for (int from = 0; from < 64; from++) {
        if (gs.board[from] == EMPTY || get_piece_color(gs.board[from]) != gs.turn) continue;

        int piece = gs.board[from];
        int type = get_piece_type(piece);
        int r = from / 8, c = from % 8;

        // --- Pawn Moves ---
        if (type == PAWN) {
            int dir = (gs.turn == WHITE) ? -1 : 1;
            int start_row = (gs.turn == WHITE) ? 6 : 1;
            int prom_row = (gs.turn == WHITE) ? 0 : 7;
            
            // Forward 1
            if (r + dir >= 0 && r + dir < 8 && gs.board[from + 8 * dir] == EMPTY) {
                if (r + dir == prom_row) {
                    for(int p = QUEEN; p >= KNIGHT; --p) legal_moves[num_legal_moves++] = (Move){from, from + 8 * dir, p};
                } else {
                    legal_moves[num_legal_moves++] = (Move){from, from + 8 * dir, 0};
                }
                // Forward 2
                if (r == start_row && gs.board[from + 16 * dir] == EMPTY) {
                    legal_moves[num_legal_moves++] = (Move){from, from + 16 * dir, 0};
                }
            }
            // Captures
            for (int dc = -1; dc <= 1; dc += 2) {
                if (c + dc >= 0 && c + dc < 8) {
                    int to = from + 8 * dir + dc;
                    if ((gs.board[to] != EMPTY && get_piece_color(gs.board[to]) != gs.turn) || to == gs.en_passant_sq) {
                         if (r + dir == prom_row) {
                            for(int p = QUEEN; p >= KNIGHT; --p) legal_moves[num_legal_moves++] = (Move){from, to, p};
                        } else {
                            legal_moves[num_legal_moves++] = (Move){from, to, 0};
                        }
                    }
                }
            }
        }
        // --- Knight Moves ---
        else if (type == KNIGHT) {
            int moves[] = {-17, -15, -10, -6, 6, 10, 15, 17};
            for (int i = 0; i < 8; i++) {
                int to = from + moves[i];
                if (to >= 0 && to < 64 && abs((to % 8) - c) <= 2) {
                    if (gs.board[to] == EMPTY || get_piece_color(gs.board[to]) != gs.turn) {
                        legal_moves[num_legal_moves++] = (Move){from, to, 0};
                    }
                }
            }
        }
        // --- Sliding Pieces (Rook, Bishop, Queen) and King ---
        else {
            int directions[8], num_dirs;
            int max_dist = (type == KING) ? 1 : 7;
            if (type == ROOK || type == QUEEN || type == KING) { int d[] = {-8, -1, 1, 8}; memcpy(&directions[0], d, sizeof(d)); num_dirs = 4;}
            if (type == BISHOP || type == QUEEN) { int d[] = {-9, -7, 7, 9}; memcpy(type==QUEEN?&directions[4]:&directions[0], d, sizeof(d)); num_dirs=type==QUEEN?8:4;}
            if (type == KING) { int d[] = {-9, -7, 7, 9}; memcpy(&directions[4], d, sizeof(d)); num_dirs=8;}

            for (int i = 0; i < num_dirs; i++) {
                for (int d = 1; d <= max_dist; d++) {
                    int to = from + directions[i] * d;
                    if (to < 0 || to > 63) break;
                    if ( (abs((to % 8) - (from % 8)) > 2 && max_dist > 1) && (abs(directions[i])==1 || abs(directions[i])==8) ) break; // Wrap around for rooks
                    if ( abs((to % 8) - (from % 8)) > d ) break; // wrap around for others

                    if (gs.board[to] != EMPTY) {
                        if (get_piece_color(gs.board[to]) != gs.turn) legal_moves[num_legal_moves++] = (Move){from, to, 0};
                        break;
                    }
                    legal_moves[num_legal_moves++] = (Move){from, to, 0};
                }
            }
        }
    }

    // --- Castling ---
    if (gs.turn == WHITE) {
        if ((gs.castling_rights & CASTLE_WK) && gs.board[61] == EMPTY && gs.board[62] == EMPTY &&
            !is_square_attacked(60, BLACK) && !is_square_attacked(61, BLACK) && !is_square_attacked(62, BLACK))
            legal_moves[num_legal_moves++] = (Move){60, 62, 0};
        if ((gs.castling_rights & CASTLE_WQ) && gs.board[59] == EMPTY && gs.board[58] == EMPTY && gs.board[57] == EMPTY &&
            !is_square_attacked(60, BLACK) && !is_square_attacked(59, BLACK) && !is_square_attacked(58, BLACK))
            legal_moves[num_legal_moves++] = (Move){60, 58, 0};
    } else {
        if ((gs.castling_rights & CASTLE_BK) && gs.board[5] == EMPTY && gs.board[6] == EMPTY &&
            !is_square_attacked(4, WHITE) && !is_square_attacked(5, WHITE) && !is_square_attacked(6, WHITE))
            legal_moves[num_legal_moves++] = (Move){4, 6, 0};
        if ((gs.castling_rights & CASTLE_BQ) && gs.board[3] == EMPTY && gs.board[2] == EMPTY && gs.board[1] == EMPTY &&
            !is_square_attacked(4, WHITE) && !is_square_attacked(3, WHITE) && !is_square_attacked(2, WHITE))
            legal_moves[num_legal_moves++] = (Move){4, 2, 0};
    }
    
    // Filter out moves that leave the king in check
    int actual_legal_moves = 0;
    memcpy(temp_board, gs.board, sizeof(gs.board));
    temp_cr = gs.castling_rights;
    temp_ep = gs.en_passant_sq;

    for (int i = 0; i < num_legal_moves; i++) {
        // Temporarily make the move
        int p = gs.board[legal_moves[i].from];
        int captured = gs.board[legal_moves[i].to];
        gs.board[legal_moves[i].to] = p;
        gs.board[legal_moves[i].from] = EMPTY;
        
        if (!is_king_in_check(gs.turn)) {
            legal_moves[actual_legal_moves++] = legal_moves[i];
        }

        // Revert board state
        gs.board[legal_moves[i].from] = p;
        gs.board[legal_moves[i].to] = captured;
    }
    num_legal_moves = actual_legal_moves;
    
    // Check for game over
    if (num_legal_moves == 0) {
        if (is_king_in_check(gs.turn)) gs.game_over_status = CHECKMATE;
        else gs.game_over_status = STALEMATE;
    } else {
        gs.game_over_status = IN_PROGRESS;
    }
}

int make_move(Move m) {
    // Store history for undo
    HistoryEntry* h = &gs.history[gs.history_count++];
    h->move = m;
    h->captured_piece = gs.board[m.to];
    h->old_castling_rights = gs.castling_rights;
    h->old_en_passant_sq = gs.en_passant_sq;
    h->old_halfmove_clock = gs.halfmove_clock;

    // Convert move to PGN and store
    move_to_pgn(m, &gs.pgn_history[gs.pgn_count++]);

    int piece = gs.board[m.from];
    int type = get_piece_type(piece);

    // Update halfmove clock
    if (type == PAWN || h->captured_piece != EMPTY) gs.halfmove_clock = 0;
    else gs.halfmove_clock++;

    // Update board
    gs.board[m.to] = gs.board[m.from];
    gs.board[m.from] = EMPTY;
    
    // Handle en passant capture
    if (type == PAWN && m.to == gs.en_passant_sq) {
        int dir = gs.turn == WHITE ? 1 : -1;
        gs.board[m.to + 8 * dir] = EMPTY;
        h->captured_piece = PAWN | (gs.turn == WHITE ? BLACK : WHITE); // Record captured pawn
    }

    // Set new en passant square
    gs.en_passant_sq = -1;
    if (type == PAWN && abs(m.to - m.from) == 16) {
        gs.en_passant_sq = m.from + (m.to - m.from) / 2;
    }
    
    // Handle castling
    if (type == KING && abs(m.to - m.from) == 2) {
        int rook_from, rook_to;
        if (m.to > m.from) { // Kingside
            rook_from = m.to + 1; rook_to = m.to - 1;
        } else { // Queenside
            rook_from = m.to - 2; rook_to = m.to + 1;
        }
        gs.board[rook_to] = gs.board[rook_from];
        gs.board[rook_from] = EMPTY;
    }

    // Handle promotion
    if(m.promotion != 0){
        gs.board[m.to] = m.promotion | gs.turn;
    }

    // Update castling rights
    if (type == KING) {
        if (gs.turn == WHITE) gs.castling_rights &= ~(CASTLE_WK | CASTLE_WQ);
        else gs.castling_rights &= ~(CASTLE_BK | CASTLE_BQ);
    }
    if (m.from == 0 || m.to == 0) gs.castling_rights &= ~CASTLE_BQ;
    if (m.from == 7 || m.to == 7) gs.castling_rights &= ~CASTLE_BK;
    if (m.from == 56 || m.to == 56) gs.castling_rights &= ~CASTLE_WQ;
    if (m.from == 63 || m.to == 63) gs.castling_rights &= ~CASTLE_WK;

    // Update turn and move number
    if (gs.turn == BLACK) gs.fullmove_number++;
    gs.turn = (gs.turn == WHITE) ? BLACK : WHITE;

    last_move = m;

    return 1;
}

void undo_last_move() {
    if (gs.history_count == 0) return;

    gs.history_count--;
    gs.pgn_count--;
    HistoryEntry* h = &gs.history[gs.history_count];

    gs.turn = (gs.turn == WHITE) ? BLACK : WHITE;
    if (gs.turn == BLACK) gs.fullmove_number--;
    
    // Restore state
    gs.castling_rights = h->old_castling_rights;
    gs.en_passant_sq = h->old_en_passant_sq;
    gs.halfmove_clock = h->old_halfmove_clock;
    
    // Revert move on board
    Move m = h->move;
    gs.board[m.from] = gs.board[m.to];
    gs.board[m.to] = h->captured_piece; // May be EMPTY

    // Undo promotion
    if(m.promotion != 0){
        gs.board[m.from] = PAWN | gs.turn;
    }

    // Undo en passant
    if (m.to == h->old_en_passant_sq && get_piece_type(gs.board[m.from]) == PAWN) {
        int dir = gs.turn == WHITE ? 1 : -1;
        gs.board[m.to] = EMPTY;
        gs.board[m.to + 8 * dir] = PAWN | (gs.turn == WHITE ? BLACK : WHITE);
    }

    // Undo castling
    if (get_piece_type(gs.board[m.from]) == KING && abs(m.to - m.from) == 2) {
        int rook_from, rook_to;
        if (m.to > m.from) { // Kingside
            rook_to = m.to - 1; rook_from = m.to + 1;
        } else { // Queenside
            rook_to = m.to + 1; rook_from = m.to - 2;
        }
        gs.board[rook_from] = gs.board[rook_to];
        gs.board[rook_to] = EMPTY;
    }

    last_move = (gs.history_count > 0) ? gs.history[gs.history_count-1].move : (Move){-1,-1,0};
    
    generate_moves(); // Recalculate legal moves for the new state
}

void move_to_pgn(Move m, char* pgn_str) {
    int piece_type = get_piece_type(gs.board[m.from]);
    char piece_char[] = " PNBRQK";
    char from_file = (m.from % 8) + 'a';
    char from_rank = '8' - (m.from / 8);
    char to_file = (m.to % 8) + 'a';
    char to_rank = '8' - (m.to / 8);
    int is_capture = gs.board[m.to] != EMPTY || (piece_type == PAWN && (m.from % 8 != m.to % 8));

    // Castling
    if (piece_type == KING && abs(m.from - m.to) == 2) {
        strcpy(pgn_str, (m.to > m.from) ? "O-O" : "O-O-O");
    } else {
        int idx = 0;
        if (piece_type != PAWN) pgn_str[idx++] = piece_char[piece_type];

        if (is_capture) {
            if (piece_type == PAWN) pgn_str[idx++] = from_file;
            pgn_str[idx++] = 'x';
        }

        pgn_str[idx++] = to_file;
        pgn_str[idx++] = to_rank;
        
        if (m.promotion != EMPTY) {
            pgn_str[idx++] = '=';
            pgn_str[idx++] = piece_char[m.promotion];
        }
        pgn_str[idx] = '\0';
    }
    
    // Temporarily make move to check for check/mate
    GameState temp_gs = gs;
    make_move(m);
    generate_moves(); // To update game_over_status
    
    if (gs.game_over_status == CHECKMATE) strcat(pgn_str, "#");
    else if (is_king_in_check(gs.turn)) strcat(pgn_str, "+");
    
    // Undo the temporary move
    gs = temp_gs;
}


// --- UCI ENGINE IMPLEMENTATION ---

int uci_init() {
    int to_engine_pipe[2];
    int from_engine_pipe[2];

    pipe(to_engine_pipe);
    pipe(from_engine_pipe);

    pid_t pid = fork();

    if (pid == 0) { // Child process
        dup2(to_engine_pipe[0], STDIN_FILENO);
        dup2(from_engine_pipe[1], STDOUT_FILENO);
        close(to_engine_pipe[1]);
        close(from_engine_pipe[0]);
        execl(engine_path, engine_path, (char*)NULL);
        exit(1); // Should not be reached
    } else { // Parent process
        close(to_engine_pipe[0]);
        close(from_engine_pipe[1]);
        engine_in = fdopen(to_engine_pipe[1], "w");
        engine_out = fdopen(from_engine_pipe[0], "r");
        setvbuf(engine_in, NULL, _IOLBF, 0);
        setvbuf(engine_out, NULL, _IOLBF, 0);

        char buffer[4096];
        fprintf(engine_in, "uci\n");
        while (fgets(buffer, sizeof(buffer), engine_out)) {
            if (strstr(buffer, "uciok")) break;
        }

        fprintf(engine_in, "isready\n");
        while (fgets(buffer, sizeof(buffer), engine_out)) {
            if (strstr(buffer, "readyok")) break;
        }
        return 1;
    }
    return 0; // Should not be reached
}

void uci_quit() {
    fprintf(engine_in, "quit\n");
    fclose(engine_in);
    fclose(engine_out);
}

void uci_send_position() {
    fprintf(engine_in, "position startpos moves");
    for (int i = 0; i < gs.history_count; i++) {
        Move m = gs.history[i].move;
        char move_str[6];
        sprintf(move_str, "%c%c%c%c",
                (m.from % 8) + 'a', '8' - (m.from / 8),
                (m.to % 8) + 'a', '8' - (m.to / 8));
        if (m.promotion != EMPTY) {
            char prom_char[] = " qrbn";
            move_str[4] = prom_char[m.promotion];
            move_str[5] = '\0';
        }
        fprintf(engine_in, " %s", move_str);
    }
    fprintf(engine_in, "\n");
}

Move uci_get_move() {
    uci_send_position();
    
    switch(time_control_mode) {
        case MODE_DEPTH:    fprintf(engine_in, "go depth %d\n", time_control_value); break;
        case MODE_MOVETIME: fprintf(engine_in, "go movetime %d\n", time_control_value); break;
        case MODE_NODES:    fprintf(engine_in, "go nodes %d\n", time_control_value); break;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), engine_out)) {
        if (strncmp(buffer, "bestmove", 8) == 0) {
            char from_str[3], to_str[3], prom_str[2] = {0};
            sscanf(buffer, "bestmove %2s%2s%1s", from_str, to_str, prom_str);
            
            Move m;
            m.from = (from_str[0] - 'a') + ('8' - from_str[1]) * 8;
            m.to = (to_str[0] - 'a') + ('8' - to_str[1]) * 8;
            m.promotion = EMPTY;
            if(prom_str[0]){
                if(prom_str[0] == 'q') m.promotion = QUEEN;
                if(prom_str[0] == 'r') m.promotion = ROOK;
                if(prom_str[0] == 'b') m.promotion = BISHOP;
                if(prom_str[0] == 'n') m.promotion = KNIGHT;
            }
            return m;
        }
    }
    return (Move){-1, -1, 0}; // Error
}
