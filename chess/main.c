#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/ioctl.h>

// ===== ANSI Color Codes =====
#define RESET "\033[0m"
#define BOLD "\033[1m"
#define BLACK_BG "\033[40m"
#define WHITE_BG "\033[47m"
#define HIGHLIGHT_BG "\033[43m"
#define LEGAL_MOVE_BG "\033[42m"
#define CURSOR_BG "\033[46m"
#define BLACK_FG "\033[30m"
#define WHITE_FG "\033[37m"

// ===== Chess Piece Constants =====
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define WHITE 8
#define BLACK 16

// ===== Move Structure =====
typedef struct {
    int from_rank, from_file;
    int to_rank, to_file;
    int piece;
    int captured;
    int promotion;
    int is_castle;
    int is_en_passant;
    int en_passant_square[2];
    int castle_rights[4];
    char notation[10];
} Move;

// ===== Game State =====
typedef struct {
    int board[8][8];
    int to_move;
    int castle_rights[4]; // 0=white king, 1=white queen, 2=black king, 3=black queen
    int en_passant_square[2]; // rank, file (-1 if none)
    Move move_history[512];
    int move_count;
    int cursor_rank;
    int cursor_file;
    int selected;
    int selected_rank;
    int selected_file;
    int legal_moves[64][2];
    int legal_move_count;
} GameState;

// ===== UCI Engine State =====
typedef struct {
    FILE *to_engine;
    FILE *from_engine;
    pid_t pid;
    int active;
    char name[256];
} UCIEngine;

// ===== Global Variables =====
GameState game;
UCIEngine engine;
struct termios orig_termios;

// ===== Unicode Chess Pieces =====
const char *piece_symbols[] = {
    " ", "♙", "♘", "♗", "♖", "♕", "♔", "",
    "", "♟", "♞", "♝", "♜", "♛", "♚"
};

// ===== Terminal Functions =====
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void clear_screen() {
    printf("\033[2J\033[H");
}

char read_key() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        return c;
    }
    return 0;
}

// ===== Initialize Board =====
void init_board() {
    // Clear board
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            game.board[i][j] = EMPTY;
        }
    }
    
    // Set up pieces
    int back_rank[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    
    for (int i = 0; i < 8; i++) {
        game.board[1][i] = BLACK | PAWN;
        game.board[6][i] = WHITE | PAWN;
        game.board[0][i] = BLACK | back_rank[i];
        game.board[7][i] = WHITE | back_rank[i];
    }
    
    game.to_move = WHITE;
    game.castle_rights[0] = 1; // White kingside
    game.castle_rights[1] = 1; // White queenside
    game.castle_rights[2] = 1; // Black kingside
    game.castle_rights[3] = 1; // Black queenside
    game.en_passant_square[0] = -1;
    game.en_passant_square[1] = -1;
    game.move_count = 0;
    game.cursor_rank = 6;
    game.cursor_file = 4;
    game.selected = 0;
    game.legal_move_count = 0;
}

// ===== Helper Functions =====
int get_color(int piece) {
    return piece & (WHITE | BLACK);
}

int get_type(int piece) {
    return piece & 7;
}

int is_valid_square(int rank, int file) {
    return rank >= 0 && rank < 8 && file >= 0 && file < 8;
}

// ===== Legal Move Generation =====
int is_attacked(int rank, int file, int by_color);

void add_move(int moves[][2], int *count, int rank, int file) {
    moves[*count][0] = rank;
    moves[*count][1] = file;
    (*count)++;
}

void generate_pawn_moves(int rank, int file, int moves[][2], int *count) {
    int piece = game.board[rank][file];
    int color = get_color(piece);
    int direction = (color == WHITE) ? -1 : 1;
    int start_rank = (color == WHITE) ? 6 : 1;
    
    // Forward move
    if (is_valid_square(rank + direction, file) && 
        game.board[rank + direction][file] == EMPTY) {
        add_move(moves, count, rank + direction, file);
        
        // Double move from start
        if (rank == start_rank && 
            game.board[rank + 2 * direction][file] == EMPTY) {
            add_move(moves, count, rank + 2 * direction, file);
        }
    }
    
    // Captures
    for (int df = -1; df <= 1; df += 2) {
        int new_file = file + df;
        int new_rank = rank + direction;
        
        if (is_valid_square(new_rank, new_file)) {
            int target = game.board[new_rank][new_file];
            if (target != EMPTY && get_color(target) != color) {
                add_move(moves, count, new_rank, new_file);
            }
            
            // En passant
            if (game.en_passant_square[0] == new_rank && 
                game.en_passant_square[1] == new_file) {
                add_move(moves, count, new_rank, new_file);
            }
        }
    }
}

void generate_knight_moves(int rank, int file, int moves[][2], int *count) {
    int knight_moves[8][2] = {
        {-2, -1}, {-2, 1}, {-1, -2}, {-1, 2},
        {1, -2}, {1, 2}, {2, -1}, {2, 1}
    };
    
    int color = get_color(game.board[rank][file]);
    
    for (int i = 0; i < 8; i++) {
        int new_rank = rank + knight_moves[i][0];
        int new_file = file + knight_moves[i][1];
        
        if (is_valid_square(new_rank, new_file)) {
            int target = game.board[new_rank][new_file];
            if (target == EMPTY || get_color(target) != color) {
                add_move(moves, count, new_rank, new_file);
            }
        }
    }
}

void generate_sliding_moves(int rank, int file, int moves[][2], int *count, 
                           int directions[][2], int num_directions) {
    int color = get_color(game.board[rank][file]);
    
    for (int d = 0; d < num_directions; d++) {
        int dr = directions[d][0];
        int df = directions[d][1];
        
        for (int i = 1; i < 8; i++) {
            int new_rank = rank + i * dr;
            int new_file = file + i * df;
            
            if (!is_valid_square(new_rank, new_file)) break;
            
            int target = game.board[new_rank][new_file];
            
            if (target == EMPTY) {
                add_move(moves, count, new_rank, new_file);
            } else {
                if (get_color(target) != color) {
                    add_move(moves, count, new_rank, new_file);
                }
                break;
            }
        }
    }
}

void generate_bishop_moves(int rank, int file, int moves[][2], int *count) {
    int directions[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    generate_sliding_moves(rank, file, moves, count, directions, 4);
}

void generate_rook_moves(int rank, int file, int moves[][2], int *count) {
    int directions[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    generate_sliding_moves(rank, file, moves, count, directions, 4);
}

void generate_queen_moves(int rank, int file, int moves[][2], int *count) {
    int directions[8][2] = {
        {-1, -1}, {-1, 0}, {-1, 1}, {0, -1},
        {0, 1}, {1, -1}, {1, 0}, {1, 1}
    };
    generate_sliding_moves(rank, file, moves, count, directions, 8);
}

void generate_king_moves(int rank, int file, int moves[][2], int *count) {
    int directions[8][2] = {
        {-1, -1}, {-1, 0}, {-1, 1}, {0, -1},
        {0, 1}, {1, -1}, {1, 0}, {1, 1}
    };
    
    int color = get_color(game.board[rank][file]);
    
    for (int d = 0; d < 8; d++) {
        int new_rank = rank + directions[d][0];
        int new_file = file + directions[d][1];
        
        if (is_valid_square(new_rank, new_file)) {
            int target = game.board[new_rank][new_file];
            if (target == EMPTY || get_color(target) != color) {
                add_move(moves, count, new_rank, new_file);
            }
        }
    }
    
    // Castling
    if (color == WHITE && rank == 7 && file == 4) {
        // Kingside
        if (game.castle_rights[0] && 
            game.board[7][5] == EMPTY && 
            game.board[7][6] == EMPTY &&
            !is_attacked(7, 4, BLACK) &&
            !is_attacked(7, 5, BLACK) &&
            !is_attacked(7, 6, BLACK)) {
            add_move(moves, count, 7, 6);
        }
        // Queenside
        if (game.castle_rights[1] && 
            game.board[7][1] == EMPTY && 
            game.board[7][2] == EMPTY && 
            game.board[7][3] == EMPTY &&
            !is_attacked(7, 4, BLACK) &&
            !is_attacked(7, 3, BLACK) &&
            !is_attacked(7, 2, BLACK)) {
            add_move(moves, count, 7, 2);
        }
    } else if (color == BLACK && rank == 0 && file == 4) {
        // Kingside
        if (game.castle_rights[2] && 
            game.board[0][5] == EMPTY && 
            game.board[0][6] == EMPTY &&
            !is_attacked(0, 4, WHITE) &&
            !is_attacked(0, 5, WHITE) &&
            !is_attacked(0, 6, WHITE)) {
            add_move(moves, count, 0, 6);
        }
        // Queenside
        if (game.castle_rights[3] && 
            game.board[0][1] == EMPTY && 
            game.board[0][2] == EMPTY && 
            game.board[0][3] == EMPTY &&
            !is_attacked(0, 4, WHITE) &&
            !is_attacked(0, 3, WHITE) &&
            !is_attacked(0, 2, WHITE)) {
            add_move(moves, count, 0, 2);
        }
    }
}

void generate_pseudo_legal_moves(int rank, int file, int moves[][2], int *count) {
    *count = 0;
    int piece = game.board[rank][file];
    
    if (piece == EMPTY) return;
    
    int type = get_type(piece);
    
    switch (type) {
        case PAWN:   generate_pawn_moves(rank, file, moves, count); break;
        case KNIGHT: generate_knight_moves(rank, file, moves, count); break;
        case BISHOP: generate_bishop_moves(rank, file, moves, count); break;
        case ROOK:   generate_rook_moves(rank, file, moves, count); break;
        case QUEEN:  generate_queen_moves(rank, file, moves, count); break;
        case KING:   generate_king_moves(rank, file, moves, count); break;
    }
}

// ===== Check Detection =====
int is_attacked(int rank, int file, int by_color) {
    // Check for pawn attacks
    int pawn_dir = (by_color == WHITE) ? -1 : 1;
    for (int df = -1; df <= 1; df += 2) {
        int r = rank + pawn_dir;
        int f = file + df;
        if (is_valid_square(r, f)) {
            int piece = game.board[r][f];
            if (piece == (by_color | PAWN)) return 1;
        }
    }
    
    // Check for knight attacks
    int knight_moves[8][2] = {
        {-2, -1}, {-2, 1}, {-1, -2}, {-1, 2},
        {1, -2}, {1, 2}, {2, -1}, {2, 1}
    };
    for (int i = 0; i < 8; i++) {
        int r = rank + knight_moves[i][0];
        int f = file + knight_moves[i][1];
        if (is_valid_square(r, f)) {
            int piece = game.board[r][f];
            if (piece == (by_color | KNIGHT)) return 1;
        }
    }
    
    // Check for sliding piece attacks (bishop, rook, queen)
    int directions[8][2] = {
        {-1, -1}, {-1, 0}, {-1, 1}, {0, -1},
        {0, 1}, {1, -1}, {1, 0}, {1, 1}
    };
    
    for (int d = 0; d < 8; d++) {
        int dr = directions[d][0];
        int df = directions[d][1];
        
        for (int i = 1; i < 8; i++) {
            int r = rank + i * dr;
            int f = file + i * df;
            
            if (!is_valid_square(r, f)) break;
            
            int piece = game.board[r][f];
            if (piece == EMPTY) continue;
            
            if (get_color(piece) == by_color) {
                int type = get_type(piece);
                
                // Diagonal attacks
                if ((d == 0 || d == 2 || d == 5 || d == 7) && 
                    (type == BISHOP || type == QUEEN)) {
                    return 1;
                }
                
                // Straight attacks
                if ((d == 1 || d == 3 || d == 4 || d == 6) && 
                    (type == ROOK || type == QUEEN)) {
                    return 1;
                }
                
                // King attacks (only 1 square away)
                if (i == 1 && type == KING) {
                    return 1;
                }
            }
            break;
        }
    }
    
    return 0;
}

int is_in_check(int color) {
    // Find king position
    int king_rank = -1, king_file = -1;
    
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            if (game.board[r][f] == (color | KING)) {
                king_rank = r;
                king_file = f;
                break;
            }
        }
        if (king_rank != -1) break;
    }
    
    if (king_rank == -1) return 0; // No king found
    
    int enemy_color = (color == WHITE) ? BLACK : WHITE;
    return is_attacked(king_rank, king_file, enemy_color);
}

// ===== Legal Move Validation =====
int is_legal_move(int from_rank, int from_file, int to_rank, int to_file) {
    // Save game state
    int saved_board[8][8];
    int saved_en_passant[2];
    
    memcpy(saved_board, game.board, sizeof(saved_board));
    saved_en_passant[0] = game.en_passant_square[0];
    saved_en_passant[1] = game.en_passant_square[1];
    
    int piece = game.board[from_rank][from_file];
    int color = get_color(piece);
    int type = get_type(piece);
    
    // Make move
    game.board[to_rank][to_file] = piece;
    game.board[from_rank][from_file] = EMPTY;
    
    // Handle en passant capture
    if (type == PAWN && 
        to_rank == game.en_passant_square[0] && 
        to_file == game.en_passant_square[1]) {
        int capture_rank = (color == WHITE) ? to_rank + 1 : to_rank - 1;
        game.board[capture_rank][to_file] = EMPTY;
    }
    
    // Check if king is in check
    int legal = !is_in_check(color);
    
    // Restore game state
    memcpy(game.board, saved_board, sizeof(saved_board));
    game.en_passant_square[0] = saved_en_passant[0];
    game.en_passant_square[1] = saved_en_passant[1];
    
    return legal;
}

void generate_legal_moves(int rank, int file) {
    int pseudo_legal[64][2];
    int pseudo_count = 0;
    
    generate_pseudo_legal_moves(rank, file, pseudo_legal, &pseudo_count);
    
    game.legal_move_count = 0;
    
    for (int i = 0; i < pseudo_count; i++) {
        if (is_legal_move(rank, file, pseudo_legal[i][0], pseudo_legal[i][1])) {
            game.legal_moves[game.legal_move_count][0] = pseudo_legal[i][0];
            game.legal_moves[game.legal_move_count][1] = pseudo_legal[i][1];
            game.legal_move_count++;
        }
    }
}

// ===== Move Execution =====
char* square_to_algebraic(int rank, int file) {
    static char sq[3];
    sq[0] = 'a' + file;
    sq[1] = '8' - rank;
    sq[2] = '\0';
    return sq;
}

void algebraic_to_square(const char *sq, int *rank, int *file) {
    *file = sq[0] - 'a';
    *rank = '8' - sq[1];
}

void generate_pgn_notation(Move *move) {
    int piece_type = get_type(move->piece);
    int is_capture = (move->captured != EMPTY || move->is_en_passant);
    
    strcpy(move->notation, "");
    
    // Castling
    if (move->is_castle) {
        if (move->to_file == 6) {
            strcpy(move->notation, "O-O");
        } else {
            strcpy(move->notation, "O-O-O");
        }
        return;
    }
    
    // Piece symbol (except for pawns)
    char piece_char[7] = {'\0', '\0', 'N', 'B', 'R', 'Q', 'K'};
    if (piece_type != PAWN) {
        sprintf(move->notation, "%c", piece_char[piece_type]);
    }
    
    // For pawns, include file if capturing
    if (piece_type == PAWN && is_capture) {
        char temp[10];
        sprintf(temp, "%c", 'a' + move->from_file);
        strcat(move->notation, temp);
    }
    
    // Capture notation
    if (is_capture) {
        strcat(move->notation, "x");
    }
    
    // Destination square
    strcat(move->notation, square_to_algebraic(move->to_rank, move->to_file));
    
    // Promotion
    if (move->promotion != EMPTY) {
        char promo[3];
        sprintf(promo, "=%c", piece_char[get_type(move->promotion)]);
        strcat(move->notation, promo);
    }
}

int make_move(int from_rank, int from_file, int to_rank, int to_file, int promotion_piece) {
    Move *move = &game.move_history[game.move_count];
    
    move->from_rank = from_rank;
    move->from_file = from_file;
    move->to_rank = to_rank;
    move->to_file = to_file;
    move->piece = game.board[from_rank][from_file];
    move->captured = game.board[to_rank][to_file];
    move->promotion = EMPTY;
    move->is_castle = 0;
    move->is_en_passant = 0;
    
    // Save state
    move->en_passant_square[0] = game.en_passant_square[0];
    move->en_passant_square[1] = game.en_passant_square[1];
    memcpy(move->castle_rights, game.castle_rights, sizeof(game.castle_rights));
    
    int piece = move->piece;
    int color = get_color(piece);
    int type = get_type(piece);
    
    // Reset en passant
    game.en_passant_square[0] = -1;
    game.en_passant_square[1] = -1;
    
    // Pawn double move - set en passant
    if (type == PAWN && abs(to_rank - from_rank) == 2) {
        game.en_passant_square[0] = (from_rank + to_rank) / 2;
        game.en_passant_square[1] = from_file;
    }
    
    // En passant capture
    if (type == PAWN && 
        to_rank == move->en_passant_square[0] && 
        to_file == move->en_passant_square[1] &&
        move->captured == EMPTY) {
        move->is_en_passant = 1;
        int capture_rank = (color == WHITE) ? to_rank + 1 : to_rank - 1;
        move->captured = game.board[capture_rank][to_file];
        game.board[capture_rank][to_file] = EMPTY;
    }
    
    // Castling
    if (type == KING && abs(to_file - from_file) == 2) {
        move->is_castle = 1;
        
        if (to_file == 6) { // Kingside
            game.board[from_rank][5] = game.board[from_rank][7];
            game.board[from_rank][7] = EMPTY;
        } else { // Queenside
            game.board[from_rank][3] = game.board[from_rank][0];
            game.board[from_rank][0] = EMPTY;
        }
    }
    
    // Update castling rights
    if (type == KING) {
        if (color == WHITE) {
            game.castle_rights[0] = 0;
            game.castle_rights[1] = 0;
        } else {
            game.castle_rights[2] = 0;
            game.castle_rights[3] = 0;
        }
    }
    
    if (type == ROOK) {
        if (color == WHITE) {
            if (from_file == 7) game.castle_rights[0] = 0;
            if (from_file == 0) game.castle_rights[1] = 0;
        } else {
            if (from_file == 7) game.castle_rights[2] = 0;
            if (from_file == 0) game.castle_rights[3] = 0;
        }
    }
    
    // Make move
    game.board[to_rank][to_file] = piece;
    game.board[from_rank][from_file] = EMPTY;
    
    // Pawn promotion
    if (type == PAWN && (to_rank == 0 || to_rank == 7)) {
        if (promotion_piece == EMPTY) {
            promotion_piece = QUEEN; // Default to queen
        }
        game.board[to_rank][to_file] = color | promotion_piece;
        move->promotion = color | promotion_piece;
    }
    
    generate_pgn_notation(move);
    
    game.move_count++;
    game.to_move = (color == WHITE) ? BLACK : WHITE;
    
    return 1;
}

void undo_move() {
    if (game.move_count == 0) return;
    
    game.move_count--;
    Move *move = &game.move_history[game.move_count];
    
    // Restore piece
    game.board[move->from_rank][move->from_file] = move->piece;
    game.board[move->to_rank][move->to_file] = move->captured;
    
    // Undo promotion
    if (move->promotion != EMPTY) {
        game.board[move->from_rank][move->from_file] = move->piece;
    }
    
    // Undo en passant
    if (move->is_en_passant) {
        int color = get_color(move->piece);
        int capture_rank = (color == WHITE) ? move->to_rank + 1 : move->to_rank - 1;
        game.board[capture_rank][move->to_file] = move->captured;
        game.board[move->to_rank][move->to_file] = EMPTY;
    }
    
    // Undo castling
    if (move->is_castle) {
        if (move->to_file == 6) { // Kingside
            game.board[move->from_rank][7] = game.board[move->from_rank][5];
            game.board[move->from_rank][5] = EMPTY;
        } else { // Queenside
            game.board[move->from_rank][0] = game.board[move->from_rank][3];
            game.board[move->from_rank][3] = EMPTY;
        }
    }
    
    // Restore state
    game.en_passant_square[0] = move->en_passant_square[0];
    game.en_passant_square[1] = move->en_passant_square[1];
    memcpy(game.castle_rights, move->castle_rights, sizeof(game.castle_rights));
    
    int color = get_color(move->piece);
    game.to_move = color;
    
    game.selected = 0;
    game.legal_move_count = 0;
}

// ===== Display Functions =====
void display_board() {
    clear_screen();
    
    printf("\n  Chess Game - %s to move\n\n", 
           (game.to_move == WHITE) ? "White" : "Black");
    
    // Display board
    printf("    a  b  c  d  e  f  g  h\n");
    printf("  ┌");
    for (int i = 0; i < 8; i++) printf("──%s", i < 7 ? "┬" : "┐\n");
    
    for (int rank = 0; rank < 8; rank++) {
        printf("%d │", 8 - rank);
        
        for (int file = 0; file < 8; file++) {
            int is_light = (rank + file) % 2 == 0;
            int is_cursor = (rank == game.cursor_rank && file == game.cursor_file);
            int is_selected = (game.selected && rank == game.selected_rank && 
                              file == game.selected_file);
            int is_legal = 0;
            
            // Check if this is a legal move square
            for (int i = 0; i < game.legal_move_count; i++) {
                if (game.legal_moves[i][0] == rank && 
                    game.legal_moves[i][1] == file) {
                    is_legal = 1;
                    break;
                }
            }
            
            // Choose background color
            if (is_cursor) {
                printf(CURSOR_BG);
            } else if (is_selected) {
                printf(HIGHLIGHT_BG);
            } else if (is_legal) {
                printf(LEGAL_MOVE_BG);
            } else if (is_light) {
                printf(WHITE_BG);
            } else {
                printf(BLACK_BG);
            }
            
            // Choose piece color
            int piece = game.board[rank][file];
            if (piece != EMPTY) {
                if (get_color(piece) == WHITE) {
                    printf(BLACK_FG);
                } else {
                    printf(WHITE_FG);
                }
                
                int index = (get_color(piece) == WHITE) ? 
                           get_type(piece) : get_type(piece) + 8;
                printf("%s ", piece_symbols[index]);
            } else {
                printf("  ");
            }
            
            printf(RESET);
            printf("│");
        }
        
        printf("%d\n", 8 - rank);
        
        if (rank < 7) {
            printf("  ├");
            for (int i = 0; i < 8; i++) printf("──%s", i < 7 ? "┼" : "┤\n");
        }
    }
    
    printf("  └");
    for (int i = 0; i < 8; i++) printf("──%s", i < 7 ? "┴" : "┘\n");
    printf("    a  b  c  d  e  f  g  h\n\n");
    
    // Display move history
    printf("Move History:\n");
    for (int i = 0; i < game.move_count; i++) {
        if (i % 2 == 0) {
            printf("%d. ", i / 2 + 1);
        }
        printf("%s ", game.move_history[i].notation);
        if (i % 2 == 1 || i == game.move_count - 1) {
            printf("\n");
        }
    }
    
    printf("\nControls: Arrow keys=Move cursor, Space=Select/Move, U=Undo, E=Engine move, Q=Quit\n");
    
    if (engine.active) {
        printf("Engine: %s\n", engine.name);
    }
}

// ===== UCI Engine Functions =====
int init_uci_engine(const char *engine_path) {
    int to_engine_pipe[2];
    int from_engine_pipe[2];
    
    if (pipe(to_engine_pipe) == -1 || pipe(from_engine_pipe) == -1) {
        return 0;
    }
    
    engine.pid = fork();
    
    if (engine.pid == -1) {
        return 0;
    }
    
    if (engine.pid == 0) {
        // Child process - run engine
        dup2(to_engine_pipe[0], STDIN_FILENO);
        dup2(from_engine_pipe[1], STDOUT_FILENO);
        
        close(to_engine_pipe[0]);
        close(to_engine_pipe[1]);
        close(from_engine_pipe[0]);
        close(from_engine_pipe[1]);
        
        execlp(engine_path, engine_path, NULL);
        exit(1);
    }
    
    // Parent process
    close(to_engine_pipe[0]);
    close(from_engine_pipe[1]);
    
    engine.to_engine = fdopen(to_engine_pipe[1], "w");
    engine.from_engine = fdopen(from_engine_pipe[0], "r");
    
    if (!engine.to_engine || !engine.from_engine) {
        return 0;
    }
    
    // Initialize UCI
    fprintf(engine.to_engine, "uci\n");
    fflush(engine.to_engine);
    
    char line[1024];
    engine.name[0] = '\0';
    
    while (fgets(line, sizeof(line), engine.from_engine)) {
        if (strncmp(line, "id name ", 8) == 0) {
            strncpy(engine.name, line + 8, sizeof(engine.name) - 1);
            engine.name[strcspn(engine.name, "\n")] = '\0';
        }
        if (strncmp(line, "uciok", 5) == 0) {
            break;
        }
    }
    
    fprintf(engine.to_engine, "isready\n");
    fflush(engine.to_engine);
    
    while (fgets(line, sizeof(line), engine.from_engine)) {
        if (strncmp(line, "readyok", 7) == 0) {
            break;
        }
    }
    
    engine.active = 1;
    return 1;
}

void get_engine_move(char *move_str) {
    if (!engine.active) return;
    
    // Send position
    fprintf(engine.to_engine, "position startpos");
    
    if (game.move_count > 0) {
        fprintf(engine.to_engine, " moves");
        for (int i = 0; i < game.move_count; i++) {
            Move *m = &game.move_history[i];
            fprintf(engine.to_engine, " %s%s",
                   square_to_algebraic(m->from_rank, m->from_file),
                   square_to_algebraic(m->to_rank, m->to_file));
            
            if (m->promotion != EMPTY) {
                char promo_char[6] = {'\0', '\0', 'n', 'b', 'r', 'q', 'k'};
                fprintf(engine.to_engine, "%c", promo_char[get_type(m->promotion)]);
            }
        }
    }
    
    fprintf(engine.to_engine, "\n");
    fflush(engine.to_engine);
    
    // Request move
    fprintf(engine.to_engine, "go movetime 1000\n");
    fflush(engine.to_engine);
    
    char line[1024];
    move_str[0] = '\0';
    
    while (fgets(line, sizeof(line), engine.from_engine)) {
        if (strncmp(line, "bestmove ", 9) == 0) {
            sscanf(line + 9, "%s", move_str);
            break;
        }
    }
}

void make_engine_move() {
    char move_str[10];
    get_engine_move(move_str);
    
    if (strlen(move_str) >= 4) {
        int from_rank, from_file, to_rank, to_file;
        char from_sq[3] = {move_str[0], move_str[1], '\0'};
        char to_sq[3] = {move_str[2], move_str[3], '\0'};
        
        algebraic_to_square(from_sq, &from_rank, &from_file);
        algebraic_to_square(to_sq, &to_rank, &to_file);
        
        int promotion = EMPTY;
        if (strlen(move_str) >= 5) {
            switch (move_str[4]) {
                case 'n': promotion = KNIGHT; break;
                case 'b': promotion = BISHOP; break;
                case 'r': promotion = ROOK; break;
                case 'q': promotion = QUEEN; break;
            }
        }
        
        // Validate and make move
        int piece = game.board[from_rank][from_file];
        if (piece != EMPTY && get_color(piece) == game.to_move) {
            generate_legal_moves(from_rank, from_file);
            
            int is_legal = 0;
            for (int i = 0; i < game.legal_move_count; i++) {
                if (game.legal_moves[i][0] == to_rank && 
                    game.legal_moves[i][1] == to_file) {
                    is_legal = 1;
                    break;
                }
            }
            
            if (is_legal) {
                make_move(from_rank, from_file, to_rank, to_file, promotion);
            }
        }
        
        game.legal_move_count = 0;
    }
}

// ===== Main Game Loop =====
int main(int argc, char *argv[]) {
    init_board();
    enable_raw_mode();
    
    engine.active = 0;
    
    // Check for engine argument
    if (argc > 1) {
        if (init_uci_engine(argv[1])) {
            // Engine initialized successfully
        }
    }
    
    display_board();
    
    int running = 1;
    while (running) {
        char c = read_key();
        
        if (c == 'q' || c == 'Q') {
            running = 0;
        } else if (c == 'u' || c == 'U') {
            undo_move();
            display_board();
        } else if (c == 'e' || c == 'E') {
            if (engine.active) {
                make_engine_move();
                display_board();
            }
        } else if (c == ' ') {
            if (!game.selected) {
                int piece = game.board[game.cursor_rank][game.cursor_file];
                if (piece != EMPTY && get_color(piece) == game.to_move) {
                    game.selected = 1;
                    game.selected_rank = game.cursor_rank;
                    game.selected_file = game.cursor_file;
                    generate_legal_moves(game.selected_rank, game.selected_file);
                    display_board();
                }
            } else {
                // Check if move is legal
                int is_legal = 0;
                for (int i = 0; i < game.legal_move_count; i++) {
                    if (game.legal_moves[i][0] == game.cursor_rank && 
                        game.legal_moves[i][1] == game.cursor_file) {
                        is_legal = 1;
                        break;
                    }
                }
                
                if (is_legal) {
                    make_move(game.selected_rank, game.selected_file,
                             game.cursor_rank, game.cursor_file, EMPTY);
                }
                
                game.selected = 0;
                game.legal_move_count = 0;
                display_board();
            }
        } else if (c == 27) { // Escape sequence
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 1 &&
                read(STDIN_FILENO, &seq[1], 1) == 1) {
                if (seq[0] == '[') {
                    switch (seq[1]) {
                        case 'A': // Up
                            if (game.cursor_rank > 0) game.cursor_rank--;
                            display_board();
                            break;
                        case 'B': // Down
                            if (game.cursor_rank < 7) game.cursor_rank++;
                            display_board();
                            break;
                        case 'C': // Right
                            if (game.cursor_file < 7) game.cursor_file++;
                            display_board();
                            break;
                        case 'D': // Left
                            if (game.cursor_file > 0) game.cursor_file--;
                            display_board();
                            break;
                    }
                }
            }
        }
    }
    
    clear_screen();
    printf("Thanks for playing!\n");
    
    return 0;
}
