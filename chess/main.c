#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Definitions ---
#define WHITE 8
#define BLACK 16

// Global State
int board[128];
int side;
int castling; // 1=WK, 2=WQ, 4=BK, 8=BQ
int ep_sq;
int nodes;

// --- PeSTO Evaluation Tables & Weights ---
const int mg_value[6] = { 82, 337, 365, 477, 1025, 0 };
const int eg_value[6] = { 94, 281, 297, 512,  936, 0 };
const int phase_inc[6] = { 0, 1, 1, 2, 4, 0 };

const int mg_pesto[6][64] = {
    { // Pawn
        0,  0,  0,  0,  0,  0,  0,  0,
       98,134, 61, 95, 68,126, 34,-11,
       -6,  7, 26, 31, 65, 56, 25,-20,
      -14, 13,  6, 21, 23, 12, 17,-23,
      -27, -2, -5, 12, 17,  6, 10,-25,
      -26, -4, -4,-10,  3,  3, 33,-12,
      -35, -1,-20,-23,-15, 24, 38,-22,
        0,  0,  0,  0,  0,  0,  0,  0
    },
    { // Knight
      -167, -89, -34, -49,  61, -97, -15, -107,
       -73, -41,  72,  36,  23,  62,   7,  -17,
       -47,  60,  37,  65,  84, 129,  73,   44,
        -9,  17,  19,  53,  37,  69,  18,   22,
       -13,   4,  16,  13,  28,  19,  21,   -8,
       -23,  -9,  12,  10,  19,  17,  25,  -16,
       -29, -53, -12,  -3,  -1,  18, -14,  -19,
      -105, -21, -58, -33, -17, -28, -19,  -23
    },
    { // Bishop
      -29,   4, -82, -37, -25, -42,   7,  -8,
      -26,  16, -18, -13,  30,  59,  18, -47,
      -16,  37,  43,  40,  35,  50,  37,  -2,
       -4,   5,  19,  50,  37,  37,   7,  -2,
       -6,  13,  13,  26,  34,  12,  10,   4,
        0,  15,  15,  15,  14,  27,  18,  10,
        4,  15,  16,   0,   7,  21,  33,   1,
      -33,  -3, -14, -21, -13, -12, -39, -21
    },
    { // Rook
       32,  42,  32,  51,  63,  9,  31,  43,
       27,  32,  58,  62,  80, 67,  26,  44,
       -5,  19,  26,  36,  17, 45,  61,  16,
      -24, -11,   7,  26,  24, 35,  -8, -20,
      -36, -26, -12,  -1,   9, -7,   6, -23,
      -45, -25, -16, -17,   3,  0,  -5, -33,
      -47, -59, -43, -36, -32,-32, -10, -18,
      -34, -19,  15,  -8,  -5, -4,  -9,  -4
    },
    { // Queen
      -28,   0,  29,  12,  59,  44,  43,  45,
      -24, -39,  -5,   1, -16,  57,  28,  54,
      -13, -17,   7,   8,  29,  56,  47,  57,
      -27, -27, -16, -16,  -1,  17,  -2,   1,
       -9, -26,  -9, -10,  -2,  -4,   3,  -3,
      -14,   2, -11,  -2,  -5,   2,  14,   5,
      -35,  -8,  11,   2,   8,  15,  -3,   1,
       -1, -18,  -9,  10, -15, -25, -31, -50
    },
    { // King
      -65,  23,  16, -15, -56, -34,   2,  13,
       29,  -1, -20,  -7,  -8,  -4, -38, -29,
       -9,  24,   2, -16, -20,   6,  22, -22,
      -17, -20, -12, -27, -30, -25, -14, -36,
      -49,  -1, -27, -39, -46, -44, -33, -51,
      -14, -14, -22, -46, -44, -30, -15, -27,
        1,   7,  -8, -64, -43, -16,   9,   8,
      -15,  36,  12, -54,   8, -28,  24,  14
    }
};

const int eg_pesto[6][64] = {
    { // Pawn
        0,   0,   0,   0,   0,   0,   0,   0,
      178, 173, 158, 134, 147, 132, 165, 187,
       94, 100,  85,  67,  56,  53,  82,  84,
       32,  24,  13,   5,  -2,   4,  17,  17,
       13,   9,  -3,  -7,  -7,  -8,   3,  -1,
        4,   7,  -6,   1,   0,  -5,  -1,  -8,
       13,   8,   8,  10,  13,   0,   2,  -7,
        0,   0,   0,   0,   0,   0,   0,   0
    },
    { // Knight
      -58, -38, -13, -28, -31, -27, -63, -99,
      -25,   8,  25,  32,  21,  11, -20, -14,
      -24, -15,  32,  43,  31,  22,  -3, -25,
      -36,  -5,  25,  34,  32,  47,  14, -18,
      -41, -20,  13,  12,  12,  20, -13, -17,
      -43, -23,  -2,  11,  22,  22,  -7,  -3,
      -58, -47, -48, -11,  14,   9, -27, -44,
      -29, -51, -60, -56, -44, -34, -42, -71
    },
    { // Bishop
      -14, -21, -11,  -8,  -7,  -9, -17, -24,
       -8,  -4,   7, -12,  -3, -13,  -4, -14,
        2,  -8,   0,  -1,  -2,   6,   0,   4,
       -3,   9,  12,   9,  14,  10,   3,   2,
       -6,   3,  13,  19,   7,  10,  -3,  -9,
      -12,  -3,   8,  10,  13,   3,  -7, -15,
      -14, -18,  -7,  -1,   4,  -9, -15, -27,
      -23,  -9, -23,  -5,  -9, -16,  -5, -17
    },
    { // Rook
       13,  10,  18,  15,  12,  12,   8,   5,
       11,  13,  13,  11,  -3,   3,   8,   3,
        7,   7,   7,   5,   4,  -3,  -5,  -3,
        4,   3,  13,   1,   2,   1,  -1,   2,
        3,   5,   8,   4,  -5,  -6,  -8, -11,
       -4,   0,  -5,  -1,  -7, -12,  -8, -16,
       -6,  -6,   0,   2,  -9,  -9, -11,  -3,
       -9,   2,   3,  -1,  -5, -13,   4, -20
    },
    { // Queen
       -9,  22,  22,  27,  27,  19,  10,  20,
      -17,  20,  32,  41,  58,  25,  30,   0,
      -20,   6,   9,  49,  47,  35,  19,   9,
        3,  22,  24,  45,  57,  40,  57,  36,
      -18,  28,  19,  47,  31,  34,  12,  11,
       -16, -27,  15,   6,   9,  17,  10,   5,
       -22, -23, -30, -16, -16, -23, -36, -32,
       -33, -28, -22, -43,  -5, -32, -20, -41
    },
    { // King
      -74, -35, -18, -18, -11,  15,   4, -17,
      -12,  17,  14,  17,  17,  38,  23,  11,
       10,  17,  23,  15,  20,  45,  44,  13,
       -8,  22,  24,  27,  26,  33,  26,   3,
      -18,  -4,  21,  24,  27,  23,   9, -11,
      -19,  -3,  11,  21,  23,  16,   7,  -9,
      -27, -11,   4,  13,  14,   4,  -5, -17,
      -53, -34, -21, -11, -28, -14, -24, -43
    }
};

// --- Core Logic ---

void init_board() {
    memset(board, 0, sizeof(board));
    int start[64] = {
        10, 8, 9,11,12, 9, 8,10,
         7, 7, 7, 7, 7, 7, 7, 7,
         0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         1, 1, 1, 1, 1, 1, 1, 1,
         4, 2, 3, 5, 6, 3, 2, 4
    };
    for (int i = 0; i < 64; i++) {
        if (start[i]) {
            int sq = (i / 8) * 16 + (i % 8);
            board[sq] = ((start[i] > 6) ? BLACK : WHITE) | ((start[i] > 6) ? start[i]-6 : start[i]);
        }
    }
    side = WHITE;
    castling = 15;
    ep_sq = 0;
}

void print_board() {
    char pieces[] = ".PNBRQK.pnbrqk";
    printf("\n");
    for (int row = 0; row < 8; row++) {
        printf("%d ", 8 - row);
        for (int col = 0; col < 8; col++) {
            int p = board[row * 16 + col];
            printf("%c ", pieces[p ? (p & 7) + ((p & 16) ? 7 : 0) : 0]);
        }
        printf("\n");
    }
    printf("  a b c d e f g h\n\n");
}

int is_attacked(int sq, int enemy) {
    int pawn_step = (enemy == WHITE) ? 16 : -16;
    for (int d = -1; d <= 1; d += 2) {
        int p_sq = sq + pawn_step + d;
        if (!(p_sq & 0x88) && board[p_sq] == (enemy | 1)) return 1;
    }
    int n_dirs[] = {-33, -31, -18, -14, 14, 18, 31, 33};
    for (int i=0; i<8; i++) {
        int n_sq = sq + n_dirs[i];
        if (!(n_sq & 0x88) && board[n_sq] == (enemy | 2)) return 1;
    }
    int k_dirs[] = {-17, -16, -15, -1, 1, 15, 16, 17};
    for (int i=0; i<8; i++) {
        int k_sq = sq + k_dirs[i];
        if (!(k_sq & 0x88) && board[k_sq] == (enemy | 6)) return 1;
    }
    int b_dirs[] = {-17, -15, 15, 17};
    for (int i=0; i<4; i++) {
        int cur = sq + b_dirs[i];
        while (!(cur & 0x88)) {
            if (board[cur]) {
                if (board[cur] == (enemy | 3) || board[cur] == (enemy | 5)) return 1;
                break;
            }
            cur += b_dirs[i];
        }
    }
    int r_dirs[] = {-16, -1, 1, 16};
    for (int i=0; i<4; i++) {
        int cur = sq + r_dirs[i];
        while (!(cur & 0x88)) {
            if (board[cur]) {
                if (board[cur] == (enemy | 4) || board[cur] == (enemy | 5)) return 1;
                break;
            }
            cur += r_dirs[i];
        }
    }
    return 0;
}

int generate_moves(int *moves) {
    int count = 0;
    int enemy = side ^ 24;
    int dirs[6][8] = {
        {0}, {-33, -31, -18, -14, 14, 18, 31, 33}, {-17, -15, 15, 17}, 
        {-16, -1, 1, 16}, {-17, -16, -15, -1, 1, 15, 16, 17}, {-17, -16, -15, -1, 1, 15, 16, 17}
    };
    int num_dirs[6] = {0, 8, 4, 4, 8, 8};

    for (int sq = 0; sq < 128; sq++) {
        if (!(sq & 0x88) && (board[sq] & side)) {
            int p = board[sq] & 7;
            if (p == 1) { // Pawn
                int step = (side == WHITE) ? -16 : 16;
                int prom_row = (side == WHITE) ? 0 : 7;
                if (!( (sq + step) & 0x88) && board[sq + step] == 0) {
                    if ((sq + step) >> 4 == prom_row) {
                        for(int pr = 2; pr <= 5; pr++) moves[count++] = sq | ((sq+step)<<8) | (pr<<16);
                    } else {
                        moves[count++] = sq | ((sq+step)<<8);
                        int start_row = (side == WHITE) ? 6 : 1;
                        if ((sq >> 4) == start_row && board[sq + step * 2] == 0)
                            moves[count++] = sq | ((sq+step*2)<<8);
                    }
                }
                for (int d = -1; d <= 1; d += 2) {
                    int cap_sq = sq + step + d;
                    if (!(cap_sq & 0x88)) {
                        if ((board[cap_sq] & enemy) || cap_sq == ep_sq) {
                            int ep_flag = (cap_sq == ep_sq) ? (1<<20) : 0;
                            if (cap_sq >> 4 == prom_row) {
                                for(int pr = 2; pr <= 5; pr++) moves[count++] = sq | (cap_sq<<8) | (pr<<16) | ep_flag;
                            } else {
                                moves[count++] = sq | (cap_sq<<8) | ep_flag;
                            }
                        }
                    }
                }
            } else { // Officers
                for (int i = 0; i < num_dirs[p - 1]; i++) {
                    int cur = sq + dirs[p - 1][i];
                    while (!(cur & 0x88)) {
                        if (board[cur]) {
                            if (board[cur] & enemy) moves[count++] = sq | (cur<<8);
                            break;
                        }
                        moves[count++] = sq | (cur<<8);
                        if (p == 2 || p == 6) break;
                        cur += dirs[p - 1][i];
                    }
                }
                // Castling
                if (p == 6) {
                    if (side == WHITE) {
                        if ((castling & 1) && !board[117] && !board[118] && !is_attacked(116, BLACK) && !is_attacked(117, BLACK) && !is_attacked(118, BLACK))
                            moves[count++] = 116 | (118<<8) | (1<<24);
                        if ((castling & 2) && !board[115] && !board[114] && !board[113] && !is_attacked(116, BLACK) && !is_attacked(115, BLACK) && !is_attacked(114, BLACK))
                            moves[count++] = 116 | (114<<8) | (1<<24);
                    } else {
                        if ((castling & 4) && !board[5] && !board[6] && !is_attacked(4, WHITE) && !is_attacked(5, WHITE) && !is_attacked(6, WHITE))
                            moves[count++] = 4 | (6<<8) | (1<<24);
                        if ((castling & 8) && !board[3] && !board[2] && !board[1] && !is_attacked(4, WHITE) && !is_attacked(3, WHITE) && !is_attacked(2, WHITE))
                            moves[count++] = 4 | (2<<8) | (1<<24);
                    }
                }
            }
        }
    }
    return count;
}

void make_move(int move) {
    int src = move & 0xFF, dst = (move >> 8) & 0xFF, promo = (move >> 16) & 7;
    int ep = (move >> 20) & 1, castle = (move >> 24) & 1;
    int p = board[src];
    
    board[dst] = p; board[src] = 0;
    if (promo) board[dst] = side | promo;
    if (ep) board[dst + ((side == WHITE) ? 16 : -16)] = 0;
    if (castle) {
        if (dst == 118) { board[117] = board[119]; board[119] = 0; } // WK
        if (dst == 114) { board[115] = board[112]; board[112] = 0; } // WQ
        if (dst == 6)   { board[5] = board[7]; board[7] = 0; }     // BK
        if (dst == 2)   { board[3] = board[0]; board[0] = 0; }     // BQ
    }

    if (src == 116 || dst == 116) castling &= ~3;
    if (src == 112 || dst == 112) castling &= ~2;
    if (src == 119 || dst == 119) castling &= ~1;
    if (src == 4 || dst == 4) castling &= ~12;
    if (src == 0 || dst == 0) castling &= ~8;
    if (src == 7 || dst == 7) castling &= ~4;

    ep_sq = 0;
    if ((p & 7) == 1 && abs(src - dst) == 32) ep_sq = src + ((side == WHITE) ? -16 : 16);
    side ^= 24;
}

// --- Evaluation & Search ---

int evaluate() {
    int mg[2] = {0, 0}, eg[2] = {0, 0}, gamePhase = 0;
    for (int sq = 0; sq < 128; sq++) {
        if (!(sq & 0x88) && board[sq]) {
            int p = (board[sq] & 7) - 1;
            int c = (board[sq] & WHITE) ? 0 : 1; // 0 for White, 1 for Black
            int idx64 = (sq >> 4) * 8 + (sq & 7);
            int mapped_sq = (c == 0) ? idx64 : (idx64 ^ 56);
            
            mg[c] += mg_value[p] + mg_pesto[p][mapped_sq];
            eg[c] += eg_value[p] + eg_pesto[p][mapped_sq];
            gamePhase += phase_inc[p];
        }
    }
    int mgPhase = (gamePhase > 24) ? 24 : gamePhase;
    int egPhase = 24 - mgPhase;
    int score = ((mg[0] - mg[1]) * mgPhase + (eg[0] - eg[1]) * egPhase) / 24;
    return (side == WHITE) ? score : -score;
}

void sort_moves(int *moves, int count) {
    for (int i = 0; i < count - 1; i++) {
        int best_i = i, best_score = -1;
        for (int j = i; j < count; j++) {
            int d = (moves[j] >> 8) & 0xFF;
            int score = board[d] ? (board[d] & 7) : 0;
            if (moves[j] & (1<<20)) score = 1; // EP
            if (score > best_score) { best_score = score; best_i = j; }
        }
        int temp = moves[i]; moves[i] = moves[best_i]; moves[best_i] = temp;
    }
}

int quiesce(int alpha, int beta) {
    nodes++;
    int stand_pat = evaluate();
    if (stand_pat >= beta) return beta;
    if (alpha < stand_pat) alpha = stand_pat;

    int moves[256];
    int count = generate_moves(moves);
    sort_moves(moves, count);

    for (int i = 0; i < count; i++) {
        int dst = (moves[i] >> 8) & 0xFF;
        if (board[dst] == 0 && !(moves[i] & (1<<20))) continue; // Captures only
        
        int saved_board[128], saved_side = side, saved_castling = castling, saved_ep = ep_sq;
        memcpy(saved_board, board, sizeof(board));
        make_move(moves[i]);
        
        int king_sq = -1;
        for(int sq=0; sq<128; sq++) if(!(sq&0x88) && board[sq] == (saved_side|6)) king_sq = sq;
        if (is_attacked(king_sq, side)) {
            memcpy(board, saved_board, sizeof(board)); side = saved_side; castling = saved_castling; ep_sq = saved_ep;
            continue;
        }

        int score = -quiesce(-beta, -alpha);
        memcpy(board, saved_board, sizeof(board)); side = saved_side; castling = saved_castling; ep_sq = saved_ep;

        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

int search(int depth, int alpha, int beta) {
    nodes++;
    if (depth == 0) return quiesce(alpha, beta);

    int moves[256], legal = 0;
    int count = generate_moves(moves);
    sort_moves(moves, count);
    int best_score = -100000;

    for (int i = 0; i < count; i++) {
        int saved_board[128], saved_side = side, saved_castling = castling, saved_ep = ep_sq;
        memcpy(saved_board, board, sizeof(board));
        make_move(moves[i]);

        int king_sq = -1;
        for(int sq=0; sq<128; sq++) if(!(sq&0x88) && board[sq] == (saved_side|6)) king_sq = sq;
        if (is_attacked(king_sq, side)) {
            memcpy(board, saved_board, sizeof(board)); side = saved_side; castling = saved_castling; ep_sq = saved_ep;
            continue;
        }
        legal++;
        int score = -search(depth - 1, -beta, -alpha);
        memcpy(board, saved_board, sizeof(board)); side = saved_side; castling = saved_castling; ep_sq = saved_ep;

        if (score > best_score) best_score = score;
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

    if (legal == 0) {
        int king_sq = -1;
        for(int sq=0; sq<128; sq++) if(!(sq&0x88) && board[sq] == (side|6)) king_sq = sq;
        return is_attacked(king_sq, side^24) ? -99999 : 0;
    }
    return best_score;
}

int best_move = 0;
int think(int depth) {
    int moves[256];
    int count = generate_moves(moves);
    sort_moves(moves, count);
    int alpha = -100000, beta = 100000, max_score = -100000;
    best_move = 0;

    for (int i = 0; i < count; i++) {
        int saved_board[128], saved_side = side, saved_castling = castling, saved_ep = ep_sq;
        memcpy(saved_board, board, sizeof(board));
        make_move(moves[i]);

        int king_sq = -1;
        for(int sq=0; sq<128; sq++) if(!(sq&0x88) && board[sq] == (saved_side|6)) king_sq = sq;
        if (is_attacked(king_sq, side)) {
            memcpy(board, saved_board, sizeof(board)); side = saved_side; castling = saved_castling; ep_sq = saved_ep;
            continue;
        }

        int score = -search(depth - 1, -beta, -alpha);
        memcpy(board, saved_board, sizeof(board)); side = saved_side; castling = saved_castling; ep_sq = saved_ep;

        if (score > max_score) { max_score = score; best_move = moves[i]; }
        if (score > alpha) alpha = score;
    }
    return max_score;
}

// --- CLI ---

int parse_move(char *str, int *parsed_move) {
    if (strlen(str) < 4) return 0;
    int src = (7 - (str[1] - '1')) * 16 + (str[0] - 'a');
    int dst = (7 - (str[3] - '1')) * 16 + (str[2] - 'a');
    int promo = (str[4] == 'q') ? 5 : (str[4] == 'r') ? 4 : (str[4] == 'b') ? 3 : (str[4] == 'n') ? 2 : 0;

    int moves[256];
    int count = generate_moves(moves);
    for (int i = 0; i < count; i++) {
        if ((moves[i] & 0xFF) == src && ((moves[i] >> 8) & 0xFF) == dst && ((moves[i] >> 16) & 7) == promo) {
            int saved_board[128], saved_side = side, saved_castling = castling, saved_ep = ep_sq;
            memcpy(saved_board, board, sizeof(board));
            make_move(moves[i]);
            int king_sq = -1;
            for(int sq=0; sq<128; sq++) if(!(sq&0x88) && board[sq] == (saved_side|6)) king_sq = sq;
            int illegal = is_attacked(king_sq, side);
            memcpy(board, saved_board, sizeof(board)); side = saved_side; castling = saved_castling; ep_sq = saved_ep;
            
            if (!illegal) { *parsed_move = moves[i]; return 1; }
        }
    }
    return 0;
}

void print_move(int move) {
    int src = move & 0xFF, dst = (move >> 8) & 0xFF, promo = (move >> 16) & 7;
    char p = (promo==5) ? 'q' : (promo==4) ? 'r' : (promo==3) ? 'b' : (promo==2) ? 'n' : ' ';
    printf("%c%d%c%d%c\n", 'a' + (src & 7), 8 - (src >> 4), 'a' + (dst & 7), 8 - (dst >> 4), p);
}

int main() {
    init_board();
    char line[256];
    printf("Micro-PeSTO C Chess Engine\n");
    printf("Commands: e2e4 (your move), go (engine move), d (display), quit\n");
    print_board();

    while (1) {
        printf("> ");
        if (!fgets(line, sizeof(line), stdin) || !strncmp(line, "quit", 4)) break;
        if (!strncmp(line, "d", 1)) { print_board(); continue; }

        int m;
        int is_engine_turn = 0;

        if (!strncmp(line, "go", 2)) {
            is_engine_turn = 1;
        } else if (parse_move(line, &m)) {
            make_move(m);
            print_board();
            is_engine_turn = 1; // Auto-respond
        } else {
            printf("Unknown command or illegal move.\n");
        }

        if (is_engine_turn) {
            nodes = 0;
            printf("Thinking...\n");
            int score = think(4); // Search Depth
            
            if (best_move) {
                printf("PeSTO info: depth 4, nodes %d, score %d\n", nodes, score);
                printf("Engine plays: ");
                print_move(best_move);
                make_move(best_move);
                print_board();
            } else {
                int king_sq = -1;
                for(int sq=0; sq<128; sq++) if(!(sq&0x88) && board[sq] == (side|6)) king_sq = sq;
                if(is_attacked(king_sq, side^24)) printf("Checkmate!\n");
                else printf("Stalemate!\n");
            }
        }
    }
    return 0;
}
