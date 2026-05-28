#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <time.h>

// Piece definitions
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define WHITE 0
#define BLACK 1

// Board representation
typedef struct {
    int board[64];
    int color[64];
    bool white_to_move;
    int castling_rights; // KQkq as bits 0-3
    int ep_square; // en passant square, -1 if none
    int halfmove_clock;
    int fullmove_number;
} Position;

// Move representation
typedef struct {
    int from;
    int to;
    int promotion; // 0 if none, otherwise piece type
    int score;
} Move;

// PeSTO piece-square tables (MiddleGame values)
static const int mg_pawn_table[64] = {
      0,   0,   0,   0,   0,   0,  0,   0,
     98, 134,  61,  95,  68, 126, 34, -11,
     -6,   7,  26,  31,  65,  56, 25, -20,
    -14,  13,   6,  21,  23,  12, 17, -23,
    -27,  -2,  -5,  12,  17,   6, 10, -25,
    -26,  -4,  -4, -10,   3,   3, 33, -12,
    -35,  -1, -20, -23, -15,  24, 38, -22,
      0,   0,   0,   0,   0,   0,  0,   0,
};

static const int mg_knight_table[64] = {
    -167, -89, -34, -49,  61, -97, -15, -107,
     -73, -41,  72,  36,  23,  62,   7,  -17,
     -47,  60,  37,  65,  84, 129,  73,   44,
      -9,  17,  19,  53,  37,  69,  18,   22,
     -13,   4,  16,  13,  28,  19,  21,   -8,
     -23,  -9,  12,  10,  19,  17,  25,  -16,
     -29, -53, -12,  -3,  -1,  18, -14,  -19,
    -105, -21, -58, -33, -17, -28, -19,  -23,
};

static const int mg_bishop_table[64] = {
    -29,   4, -82, -37, -25, -42,   7,  -8,
    -26,  16, -18, -13,  30,  59,  18, -47,
    -16,  37,  43,  40,  35,  50,  37,  -2,
     -4,   5,  19,  50,  37,  37,   7,  -2,
     -6,  13,  13,  26,  34,  12,  10,   4,
      0,  15,  15,  15,  14,  27,  18,  10,
      4,  15,  16,   0,   7,  21,  33,   1,
    -33,  -3, -14, -21, -13, -12, -39, -21,
};

static const int mg_rook_table[64] = {
     32,  42,  32,  51, 63,  9,  31,  43,
     27,  32,  58,  62, 80, 67,  26,  44,
     -5,  19,  26,  36, 17, 45,  61,  16,
    -24, -11,   7,  26, 24, 35,  -8, -20,
    -36, -26, -12,  -1,  9, -7,   6, -23,
    -45, -25, -16, -17,  3,  0,  -5, -33,
    -44, -16, -20,  -9, -1, 11,  -6, -71,
    -19, -13,   1,  17, 16,  7, -37, -26,
};

static const int mg_queen_table[64] = {
    -28,   0,  29,  12,  59,  44,  43,  45,
    -24, -39,  -5,   1, -16,  57,  28,  54,
    -13, -17,   7,   8,  29,  56,  47,  57,
    -27, -27, -16, -16,  -1,  17,  -2,   1,
     -9, -26,  -9, -10,  -2,  -4,   3,  -3,
    -14,   2, -11,  -2,  -5,   2,  14,   5,
    -35,  -8,  11,   2,   8,  15,  -3,   1,
     -1, -18,  -9,  10, -15, -25, -31, -50,
};

static const int mg_king_table[64] = {
    -65,  23,  16, -15, -56, -34,   2,  13,
     29,  -1, -20,  -7,  -8,  -4, -38, -29,
     -9,  24,   2, -16, -20,   6,  22, -22,
    -17, -20, -12, -27, -30, -25, -14, -36,
    -49,  -1, -27, -39, -46, -44, -33, -51,
    -14, -14, -22, -46, -44, -30, -15, -27,
      1,   7,  -8, -64, -43, -16,   9,   8,
    -15,  36,  12, -54,   8, -28,  24,  14,
};

// EndGame tables
static const int eg_pawn_table[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
    178, 173, 158, 134, 147, 132, 165, 187,
     94, 100,  85,  67,  56,  53,  82,  84,
     32,  24,  13,   5,  -2,   4,  17,  17,
     13,   9,  -3,  -7,  -7,  -8,   3,  -1,
      4,   7,  -6,   1,   0,  -5,  -1,  -8,
     13,   8,   8,  10,  13,   0,   2,  -7,
      0,   0,   0,   0,   0,   0,   0,   0,
};

static const int eg_knight_table[64] = {
    -58, -38, -13, -28, -31, -27, -63, -99,
    -25,  -8, -25,  -2,  -9, -25, -24, -52,
    -24, -20,  10,   9,  -1,  -9, -19, -41,
    -17,   3,  22,  22,  22,  11,   8, -18,
    -18,  -6,  16,  25,  16,  17,   4, -18,
    -23,  -3,  -1,  15,  10,  -3, -20, -22,
    -42, -20, -10,  -5,  -2, -20, -23, -44,
    -29, -51, -23, -15, -22, -18, -50, -64,
};

static const int eg_bishop_table[64] = {
    -14, -21, -11,  -8, -7,  -9, -17, -24,
     -8,  -4,   7, -12, -3, -13,  -4, -14,
      2,  -8,   0,  -1, -2,   6,   0,   4,
     -3,   9,  12,   9, 14,  10,   3,   2,
     -6,   3,  13,  19,  7,  10,  -3,  -9,
    -12,  -3,   8,  10, 13,   3,  -7, -15,
    -14, -18,  -7,  -1,  4,  -9, -15, -27,
    -23,  -9, -23,  -5, -9, -16,  -5, -17,
};

static const int eg_rook_table[64] = {
    13, 10, 18, 15, 12,  12,   8,   5,
    11, 13, 13, 11, -3,   3,   8,   3,
     7,  7,  7,  5,  4,  -3,  -5,  -3,
     4,  3, 13,  1,  2,   1,  -1,   2,
     3,  5,  8,  4, -5,  -6,  -8, -11,
    -4,  0, -5, -1, -7, -12,  -8, -16,
    -6, -6,  0,  2, -9,  -9, -11,  -3,
    -9,  2,  3, -1, -5, -13,   4, -20,
};

static const int eg_queen_table[64] = {
     -9,  22,  22,  27,  27,  19,  10,  20,
    -17,  20,  32,  41,  58,  25,  30,   0,
    -20,   6,   9,  49,  47,  35,  19,   9,
      3,  22,  24,  45,  57,  40,  57,  36,
    -18,  28,  19,  47,  31,  34,  39,  23,
    -16, -27,  15,   6,   9,  17,  10,   5,
    -22, -23, -30, -16, -16, -23, -36, -32,
    -33, -28, -22, -43,  -5, -32, -20, -41,
};

static const int eg_king_table[64] = {
    -74, -35, -18, -18, -11,  15,   4, -17,
    -12,  17,  14,  17,  17,  38,  23,  11,
     10,  17,  23,  15,  20,  45,  44,  13,
     -8,  22,  24,  27,  26,  33,  26,   3,
    -18,  -4,  21,  24,  27,  23,   9, -11,
    -19,  -3,  11,  21,  23,  16,   7,  -9,
    -27, -11,   4,  13,  14,   4,  -5, -17,
    -53, -34, -21, -11, -28, -14, -24, -43
};

// Piece values for move ordering
static const int piece_values[7] = {0, 100, 320, 330, 500, 900, 20000};

Position root_position;

// Utility functions
int square(int rank, int file) {
    return rank * 8 + file;
}

int rank_of(int sq) {
    return sq / 8;
}

int file_of(int sq) {
    return sq % 8;
}

void print_board(Position *pos) {
    const char *piece_chars = ".PNBRQK";
    for (int rank = 7; rank >= 0; rank--) {
        printf("%d ", rank + 1);
        for (int file = 0; file < 8; file++) {
            int sq = square(rank, file);
            char c = piece_chars[pos->board[sq]];
            if (c != '.' && pos->color[sq] == BLACK) c = tolower(c);
            printf("%c ", c);
        }
        printf("\n");
    }
    printf("  a b c d e f g h\n");
}

// Initialize position from FEN
void init_position(Position *pos, const char *fen) {
    memset(pos, 0, sizeof(Position));
    for (int i = 0; i < 64; i++) {
        pos->board[i] = EMPTY;
        pos->ep_square = -1;
    }
    
    int sq = 56; // Start at a8
    int i = 0;
    
    // Parse piece placement
    while (fen[i] != ' ') {
        char c = fen[i];
        if (c == '/') {
            sq -= 16;
        } else if (isdigit(c)) {
            sq += (c - '0');
        } else {
            int piece = 0, color = WHITE;
            switch (tolower(c)) {
                case 'p': piece = PAWN; break;
                case 'n': piece = KNIGHT; break;
                case 'b': piece = BISHOP; break;
                case 'r': piece = ROOK; break;
                case 'q': piece = QUEEN; break;
                case 'k': piece = KING; break;
            }
            if (islower(c)) color = BLACK;
            pos->board[sq] = piece;
            pos->color[sq] = color;
            sq++;
        }
        i++;
    }
    
    i++; // Skip space
    
    // Parse side to move
    pos->white_to_move = (fen[i] == 'w');
    i += 2;
    
    // Parse castling rights
    pos->castling_rights = 0;
    while (fen[i] != ' ') {
        switch (fen[i]) {
            case 'K': pos->castling_rights |= 1; break;
            case 'Q': pos->castling_rights |= 2; break;
            case 'k': pos->castling_rights |= 4; break;
            case 'q': pos->castling_rights |= 8; break;
        }
        i++;
    }
    i++;
    
    // Parse en passant square
    if (fen[i] != '-') {
        int file = fen[i] - 'a';
        int rank = fen[i+1] - '1';
        pos->ep_square = square(rank, file);
        i += 2;
    } else {
        i++;
    }
}

// Get PeSTO score for a piece on a square
int get_piece_value(int piece, int sq, int color, bool endgame) {
    if (piece == EMPTY) return 0;
    
    // Flip square for black
    int table_sq = (color == WHITE) ? sq : (sq ^ 56);
    
    int mg_value = 0, eg_value = 0;
    
    switch (piece) {
        case PAWN:
            mg_value = mg_pawn_table[table_sq];
            eg_value = eg_pawn_table[table_sq];
            break;
        case KNIGHT:
            mg_value = mg_knight_table[table_sq];
            eg_value = eg_knight_table[table_sq];
            break;
        case BISHOP:
            mg_value = mg_bishop_table[table_sq];
            eg_value = eg_bishop_table[table_sq];
            break;
        case ROOK:
            mg_value = mg_rook_table[table_sq];
            eg_value = eg_rook_table[table_sq];
            break;
        case QUEEN:
            mg_value = mg_queen_table[table_sq];
            eg_value = eg_queen_table[table_sq];
            break;
        case KING:
            mg_value = mg_king_table[table_sq];
            eg_value = eg_king_table[table_sq];
            break;
    }
    
    // For simplicity, we'll use a 50/50 blend
    // In a more sophisticated engine, you'd calculate game phase
    int value = (mg_value + eg_value) / 2 + piece_values[piece];
    
    return value;
}

// Evaluate position using PeSTO
int evaluate(Position *pos) {
    int score = 0;
    
    for (int sq = 0; sq < 64; sq++) {
        if (pos->board[sq] != EMPTY) {
            int value = get_piece_value(pos->board[sq], sq, pos->color[sq], false);
            if (pos->color[sq] == WHITE) {
                score += value;
            } else {
                score -= value;
            }
        }
    }
    
    return pos->white_to_move ? score : -score;
}

// Check if a square is attacked by the given color
bool is_square_attacked(Position *pos, int sq, int by_color) {
    int rank = rank_of(sq);
    int file = file_of(sq);
    
    // Pawn attacks
    int pawn_dir = (by_color == WHITE) ? 1 : -1;
    if (rank + pawn_dir >= 0 && rank + pawn_dir < 8) {
        if (file > 0) {
            int from = square(rank + pawn_dir, file - 1);
            if (pos->board[from] == PAWN && pos->color[from] == by_color)
                return true;
        }
        if (file < 7) {
            int from = square(rank + pawn_dir, file + 1);
            if (pos->board[from] == PAWN && pos->color[from] == by_color)
                return true;
        }
    }
    
    // Knight attacks
    int knight_moves[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int r = rank + knight_moves[i][0];
        int f = file + knight_moves[i][1];
        if (r >= 0 && r < 8 && f >= 0 && f < 8) {
            int from = square(r, f);
            if (pos->board[from] == KNIGHT && pos->color[from] == by_color)
                return true;
        }
    }
    
    // Sliding pieces (bishop, rook, queen)
    int directions[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    for (int i = 0; i < 8; i++) {
        int r = rank, f = file;
        while (1) {
            r += directions[i][0];
            f += directions[i][1];
            if (r < 0 || r >= 8 || f < 0 || f >= 8) break;
            
            int from = square(r, f);
            if (pos->board[from] != EMPTY) {
                if (pos->color[from] == by_color) {
                    int piece = pos->board[from];
                    bool is_diagonal = (i == 0 || i == 2 || i == 5 || i == 7);
                    bool is_orthogonal = !is_diagonal;
                    
                    if (piece == QUEEN) return true;
                    if (piece == BISHOP && is_diagonal) return true;
                    if (piece == ROOK && is_orthogonal) return true;
                }
                break;
            }
        }
    }
    
    // King attacks
    for (int dr = -1; dr <= 1; dr++) {
        for (int df = -1; df <= 1; df++) {
            if (dr == 0 && df == 0) continue;
            int r = rank + dr;
            int f = file + df;
            if (r >= 0 && r < 8 && f >= 0 && f < 8) {
                int from = square(r, f);
                if (pos->board[from] == KING && pos->color[from] == by_color)
                    return true;
            }
        }
    }
    
    return false;
}

// Find king square
int find_king(Position *pos, int color) {
    for (int sq = 0; sq < 64; sq++) {
        if (pos->board[sq] == KING && pos->color[sq] == color)
            return sq;
    }
    return -1;
}

// Check if current side is in check
bool is_in_check(Position *pos) {
    int color = pos->white_to_move ? WHITE : BLACK;
    int king_sq = find_king(pos, color);
    if (king_sq == -1) return false;
    return is_square_attacked(pos, king_sq, 1 - color);
}

// Make a move
void make_move(Position *pos, Move *move) {
    int moving_piece = pos->board[move->from];
    int moving_color = pos->color[move->from];
    
    // Move piece
    pos->board[move->to] = moving_piece;
    pos->color[move->to] = moving_color;
    pos->board[move->from] = EMPTY;
    
    // Handle promotion
    if (move->promotion != 0) {
        pos->board[move->to] = move->promotion;
    }
    
    // Handle en passant capture
    if (moving_piece == PAWN && move->to == pos->ep_square && pos->ep_square != -1) {
        int captured_pawn_sq = move->to + (moving_color == WHITE ? -8 : 8);
        pos->board[captured_pawn_sq] = EMPTY;
    }
    
    // Update en passant square
    int old_ep = pos->ep_square;
    pos->ep_square = -1;
    if (moving_piece == PAWN && abs(move->to - move->from) == 16) {
        pos->ep_square = (move->from + move->to) / 2;
    }
    
    // Handle castling
    if (moving_piece == KING && abs(move->to - move->from) == 2) {
        int rook_from, rook_to;
        if (move->to > move->from) { // Kingside
            rook_from = move->from + 3;
            rook_to = move->from + 1;
        } else { // Queenside
            rook_from = move->from - 4;
            rook_to = move->from - 1;
        }
        pos->board[rook_to] = ROOK;
        pos->color[rook_to] = moving_color;
        pos->board[rook_from] = EMPTY;
    }
    
    // Update castling rights
    if (moving_piece == KING) {
        if (moving_color == WHITE) {
            pos->castling_rights &= ~3;
        } else {
            pos->castling_rights &= ~12;
        }
    }
    if (moving_piece == ROOK) {
        if (move->from == 0) pos->castling_rights &= ~2;
        else if (move->from == 7) pos->castling_rights &= ~1;
        else if (move->from == 56) pos->castling_rights &= ~8;
        else if (move->from == 63) pos->castling_rights &= ~4;
    }
    
    pos->white_to_move = !pos->white_to_move;
}

// Generate pseudo-legal moves
int generate_moves(Position *pos, Move *moves) {
    int count = 0;
    int color = pos->white_to_move ? WHITE : BLACK;
    int opponent = 1 - color;
    
    for (int from = 0; from < 64; from++) {
        if (pos->board[from] == EMPTY || pos->color[from] != color)
            continue;
        
        int piece = pos->board[from];
        int rank = rank_of(from);
        int file = file_of(from);
        
        if (piece == PAWN) {
            int dir = (color == WHITE) ? 8 : -8;
            int start_rank = (color == WHITE) ? 1 : 6;
            int promo_rank = (color == WHITE) ? 7 : 0;
            
            // Single push
            int to = from + dir;
            if (to >= 0 && to < 64 && pos->board[to] == EMPTY) {
                if (rank_of(to) == promo_rank) {
                    for (int promo = KNIGHT; promo <= QUEEN; promo++) {
                        moves[count].from = from;
                        moves[count].to = to;
                        moves[count].promotion = promo;
                        count++;
                    }
                } else {
                    moves[count].from = from;
                    moves[count].to = to;
                    moves[count].promotion = 0;
                    count++;
                    
                    // Double push
                    if (rank == start_rank) {
                        to = from + 2 * dir;
                        if (pos->board[to] == EMPTY) {
                            moves[count].from = from;
                            moves[count].to = to;
                            moves[count].promotion = 0;
                            count++;
                        }
                    }
                }
            }
            
            // Captures
            for (int df = -1; df <= 1; df += 2) {
                if (file + df < 0 || file + df >= 8) continue;
                to = from + dir + df;
                if (to >= 0 && to < 64) {
                    if (pos->board[to] != EMPTY && pos->color[to] == opponent) {
                        if (rank_of(to) == promo_rank) {
                            for (int promo = KNIGHT; promo <= QUEEN; promo++) {
                                moves[count].from = from;
                                moves[count].to = to;
                                moves[count].promotion = promo;
                                count++;
                            }
                        } else {
                            moves[count].from = from;
                            moves[count].to = to;
                            moves[count].promotion = 0;
                            count++;
                        }
                    }
                    // En passant
                    if (to == pos->ep_square) {
                        moves[count].from = from;
                        moves[count].to = to;
                        moves[count].promotion = 0;
                        count++;
                    }
                }
            }
        } else if (piece == KNIGHT) {
            int knight_moves_offsets[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
            for (int i = 0; i < 8; i++) {
                int r = rank + knight_moves_offsets[i][0];
                int f = file + knight_moves_offsets[i][1];
                if (r >= 0 && r < 8 && f >= 0 && f < 8) {
                    int to = square(r, f);
                    if (pos->board[to] == EMPTY || pos->color[to] == opponent) {
                        moves[count].from = from;
                        moves[count].to = to;
                        moves[count].promotion = 0;
                        count++;
                    }
                }
            }
        } else if (piece == KING) {
            // Normal king moves
            for (int dr = -1; dr <= 1; dr++) {
                for (int df = -1; df <= 1; df++) {
                    if (dr == 0 && df == 0) continue;
                    int r = rank + dr;
                    int f = file + df;
                    if (r >= 0 && r < 8 && f >= 0 && f < 8) {
                        int to = square(r, f);
                        if (pos->board[to] == EMPTY || pos->color[to] == opponent) {
                            moves[count].from = from;
                            moves[count].to = to;
                            moves[count].promotion = 0;
                            count++;
                        }
                    }
                }
            }
            
            // Castling
            if (color == WHITE) {
                // Kingside
                if ((pos->castling_rights & 1) && 
                    pos->board[5] == EMPTY && pos->board[6] == EMPTY &&
                    !is_square_attacked(pos, 4, BLACK) &&
                    !is_square_attacked(pos, 5, BLACK) &&
                    !is_square_attacked(pos, 6, BLACK)) {
                    moves[count].from = 4;
                    moves[count].to = 6;
                    moves[count].promotion = 0;
                    count++;
                }
                // Queenside
                if ((pos->castling_rights & 2) &&
                    pos->board[1] == EMPTY && pos->board[2] == EMPTY && pos->board[3] == EMPTY &&
                    !is_square_attacked(pos, 4, BLACK) &&
                    !is_square_attacked(pos, 3, BLACK) &&
                    !is_square_attacked(pos, 2, BLACK)) {
                    moves[count].from = 4;
                    moves[count].to = 2;
                    moves[count].promotion = 0;
                    count++;
                }
            } else {
                // Kingside
                if ((pos->castling_rights & 4) &&
                    pos->board[61] == EMPTY && pos->board[62] == EMPTY &&
                    !is_square_attacked(pos, 60, WHITE) &&
                    !is_square_attacked(pos, 61, WHITE) &&
                    !is_square_attacked(pos, 62, WHITE)) {
                    moves[count].from = 60;
                    moves[count].to = 62;
                    moves[count].promotion = 0;
                    count++;
                }
                // Queenside
                if ((pos->castling_rights & 8) &&
                    pos->board[57] == EMPTY && pos->board[58] == EMPTY && pos->board[59] == EMPTY &&
                    !is_square_attacked(pos, 60, WHITE) &&
                    !is_square_attacked(pos, 59, WHITE) &&
                    !is_square_attacked(pos, 58, WHITE)) {
                    moves[count].from = 60;
                    moves[count].to = 58;
                    moves[count].promotion = 0;
                    count++;
                }
            }
        } else { // Sliding pieces (BISHOP, ROOK, QUEEN)
            int directions[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
            int start = 0, end = 8;
            
            if (piece == BISHOP) {
                // Only diagonals: 0,2,5,7
            } else if (piece == ROOK) {
                // Only orthogonals: 1,3,4,6
            }
            
            for (int i = 0; i < 8; i++) {
                if (piece == BISHOP && (i == 1 || i == 3 || i == 4 || i == 6)) continue;
                if (piece == ROOK && (i == 0 || i == 2 || i == 5 || i == 7)) continue;
                
                int r = rank, f = file;
                while (1) {
                    r += directions[i][0];
                    f += directions[i][1];
                    if (r < 0 || r >= 8 || f < 0 || f >= 8) break;
                    
                    int to = square(r, f);
                    if (pos->board[to] == EMPTY) {
                        moves[count].from = from;
                        moves[count].to = to;
                        moves[count].promotion = 0;
                        count++;
                    } else {
                        if (pos->color[to] == opponent) {
                            moves[count].from = from;
                            moves[count].to = to;
                            moves[count].promotion = 0;
                            count++;
                        }
                        break;
                    }
                }
            }
        }
    }
    
    return count;
}

// Check if a move is legal
bool is_legal(Position *pos, Move *move) {
    Position temp = *pos;
    make_move(&temp, move);
    temp.white_to_move = !temp.white_to_move; // Flip back to check our king
    bool legal = !is_in_check(&temp);
    return legal;
}

// Move ordering for alpha-beta
void order_moves(Position *pos, Move *moves, int count) {
    for (int i = 0; i < count; i++) {
        moves[i].score = 0;
        
        // MVV-LVA (Most Valuable Victim - Least Valuable Attacker)
        if (pos->board[moves[i].to] != EMPTY) {
            moves[i].score = 10 * piece_values[pos->board[moves[i].to]] - 
                           piece_values[pos->board[moves[i].from]];
        }
        
        // Promotions
        if (moves[i].promotion != 0) {
            moves[i].score += piece_values[moves[i].promotion];
        }
    }
    
    // Simple bubble sort (good enough for small move lists)
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (moves[j].score > moves[i].score) {
                Move temp = moves[i];
                moves[i] = moves[j];
                moves[j] = temp;
            }
        }
    }
}

// Alpha-beta search
int search(Position *pos, int depth, int alpha, int beta) {
    if (depth == 0) {
        return evaluate(pos);
    }
    
    Move moves[256];
    int move_count = generate_moves(pos, moves);
    order_moves(pos, moves, move_count);
    
    int legal_moves = 0;
    
    for (int i = 0; i < move_count; i++) {
        if (!is_legal(pos, &moves[i])) continue;
        legal_moves++;
        
        Position new_pos = *pos;
        make_move(&new_pos, &moves[i]);
        
        int score = -search(&new_pos, depth - 1, -beta, -alpha);
        
        if (score >= beta) {
            return beta;
        }
        if (score > alpha) {
            alpha = score;
        }
    }
    
    // Checkmate or stalemate
    if (legal_moves == 0) {
        if (is_in_check(pos)) {
            return -30000; // Checkmate
        } else {
            return 0; // Stalemate
        }
    }
    
    return alpha;
}

// Find best move
Move find_best_move(Position *pos, int depth) {
    Move moves[256];
    int move_count = generate_moves(pos, moves);
    order_moves(pos, moves, move_count);
    
    Move best_move = {0};
    int best_score = -999999;
    
    for (int i = 0; i < move_count; i++) {
        if (!is_legal(pos, &moves[i])) continue;
        
        Position new_pos = *pos;
        make_move(&new_pos, &moves[i]);
        
        int score = -search(&new_pos, depth - 1, -999999, 999999);
        
        if (score > best_score) {
            best_score = score;
            best_move = moves[i];
        }
    }
    
    return best_move;
}

// Convert square to algebraic notation
void square_to_str(int sq, char *str) {
    str[0] = 'a' + file_of(sq);
    str[1] = '1' + rank_of(sq);
    str[2] = '\0';
}

// Parse algebraic notation to square
int str_to_square(const char *str) {
    int file = str[0] - 'a';
    int rank = str[1] - '1';
    return square(rank, file);
}

// UCI move to internal move
Move uci_to_move(Position *pos, const char *uci) {
    Move move;
    move.from = str_to_square(uci);
    move.to = str_to_square(uci + 2);
    move.promotion = 0;
    
    if (strlen(uci) > 4) {
        switch (uci[4]) {
            case 'q': move.promotion = QUEEN; break;
            case 'r': move.promotion = ROOK; break;
            case 'b': move.promotion = BISHOP; break;
            case 'n': move.promotion = KNIGHT; break;
        }
    }
    
    return move;
}

// Internal move to UCI
void move_to_uci(Move *move, char *uci) {
    square_to_str(move->from, uci);
    square_to_str(move->to, uci + 2);
    uci[4] = '\0';
    
    if (move->promotion != 0) {
        const char *promo = "..nbrq";
        uci[4] = promo[move->promotion];
        uci[5] = '\0';
    }
}

// UCI loop
void uci_loop() {
    char line[8192];
    Position pos;
    init_position(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    
    while (fgets(line, sizeof(line), stdin)) {
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        
        if (strcmp(line, "uci") == 0) {
            printf("id name PeSTO-Chess\n");
            printf("id author Chess Engine\n");
            printf("uciok\n");
            fflush(stdout);
        } else if (strcmp(line, "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);
        } else if (strcmp(line, "ucinewgame") == 0) {
            init_position(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        } else if (strncmp(line, "position", 8) == 0) {
            char *fen_start = strstr(line, "fen ");
            if (fen_start) {
                fen_start += 4;
                char *moves_start = strstr(fen_start, " moves");
                if (moves_start) {
                    *moves_start = '\0';
                    init_position(&pos, fen_start);
                    *moves_start = ' ';
                    moves_start += 7;
                    
                    // Apply moves
                    char *token = strtok(moves_start, " ");
                    while (token) {
                        Move move = uci_to_move(&pos, token);
                        make_move(&pos, &move);
                        token = strtok(NULL, " ");
                    }
                } else {
                    init_position(&pos, fen_start);
                }
            } else if (strstr(line, "startpos")) {
                init_position(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
                
                char *moves_start = strstr(line, "moves");
                if (moves_start) {
                    moves_start += 6;
                    char *token = strtok(moves_start, " ");
                    while (token) {
                        Move move = uci_to_move(&pos, token);
                        make_move(&pos, &move);
                        token = strtok(NULL, " ");
                    }
                }
            }
        } else if (strncmp(line, "go", 2) == 0) {
            int depth = 5;
            
            // Parse depth if provided
            char *depth_str = strstr(line, "depth");
            if (depth_str) {
                sscanf(depth_str, "depth %d", &depth);
            }
            
            Move best = find_best_move(&pos, depth);
            char uci[6];
            move_to_uci(&best, uci);
            printf("bestmove %s\n", uci);
            fflush(stdout);
        } else if (strcmp(line, "quit") == 0) {
            break;
        }
    }
}

int main() {
    uci_loop();
    return 0;
}
