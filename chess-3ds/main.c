/*
 * Combined 3DS Chess Game
 * Merged Frontend GUI & Gissio's mcu-max Chess Engine
 * Optimized for Nintendo 3DS devkitARM / libctru
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

/* ==========================================================================
 * PART 1: THREAD-SAFE IPC SIMULATOR (Replaces Unix Pipes)
 * ========================================================================== */

#define QUEUE_SIZE 16384

typedef struct {
    char buf[QUEUE_SIZE];
    volatile int head;
    volatile int tail;
    LightLock lock;
} EngineQueue;

EngineQueue q_to_engine;
EngineQueue q_to_gui;

void queue_init(EngineQueue *q) {
    q->head = 0;
    q->tail = 0;
    LightLock_Init(&q->lock);
}

void queue_write(EngineQueue *q, const char *str) {
    LightLock_Lock(&q->lock);
    while (*str) {
        int next = (q->head + 1) % QUEUE_SIZE;
        if (next != q->tail) {
            q->buf[q->head] = *str;
            q->head = next;
        }
        str++;
    }
    LightLock_Unlock(&q->lock);
}

int queue_read(EngineQueue *q, char *out_buf, int max_len) {
    LightLock_Lock(&q->lock);
    int count = 0;
    while (q->tail != q->head && count < max_len - 1) {
        out_buf[count++] = q->buf[q->tail];
        q->tail = (q->tail + 1) % QUEUE_SIZE;
    }
    out_buf[count] = '\0';
    LightLock_Unlock(&q->lock);
    return count;
}

int queue_read_line(EngineQueue *q, char *out_buf, int max_len) {
    LightLock_Lock(&q->lock);
    int count = 0;
    int temp_tail = q->tail;
    int has_line = 0;
    
    while (temp_tail != q->head) {
        if (q->buf[temp_tail] == '\n') {
            has_line = 1;
            break;
        }
        temp_tail = (temp_tail + 1) % QUEUE_SIZE;
    }
    
    if (!has_line) {
        LightLock_Unlock(&q->lock);
        return 0; // Incomplete line, block
    }

    while (q->tail != q->head && count < max_len - 1) {
        char c = q->buf[q->tail];
        out_buf[count++] = c;
        q->tail = (q->tail + 1) % QUEUE_SIZE;
        if (c == '\n') break;
    }
    out_buf[count] = '\0';
    LightLock_Unlock(&q->lock);
    return count;
}

// Global engine definitions
#define MCUMAX_ID "mcu-max 1.0.6"
#define MCUMAX_AUTHOR "Gissio"
#define MCUMAX_SQUARE_INVALID 0x80

#define MCUMAX_MOVE_INVALID \
    (mcumax_move) { MCUMAX_SQUARE_INVALID, MCUMAX_SQUARE_INVALID }

typedef uint8_t mcumax_square;
typedef uint8_t mcumax_piece;

typedef struct {
    mcumax_square from;
    mcumax_square to;
} mcumax_move;

typedef void (*mcumax_callback)(void *);

enum {
    MCUMAX_EMPTY,
    MCUMAX_PAWN_UPSTREAM,
    MCUMAX_PAWN_DOWNSTREAM,
    MCUMAX_KNIGHT,
    MCUMAX_KING,
    MCUMAX_BISHOP,
    MCUMAX_ROOK,
    MCUMAX_QUEEN,
    MCUMAX_BLACK = 0x8,
};

/* ==========================================================================
 * PART 2: THE CHESS ENGINE PORT (mcu-max)
 * ========================================================================== */

// Override standard print and read for Engine thread redirection
int engine_printf(const char *format, ...);
char *engine_fgets(char *str, int n, FILE *stream);

#define printf(...) engine_printf(__VA_ARGS__)
#define fgets(...) engine_fgets(__VA_ARGS__)

#define MCUMAX_BOARD_MASK 0x88
#define MCUMAX_BOARD_WHITE 0x8
#define MCUMAX_BOARD_BLACK 0x10
#define MCUMAX_PIECE_MOVED 0x20
#define MCUMAX_SCORE_MAX 8000
#define MCUMAX_DEPTH_MAX 99

enum mcumax_mode {
    MCUMAX_INTERNAL_NODE,
    MCUMAX_SEARCH_VALID_MOVES,
    MCUMAX_SEARCH_BEST_MOVE,
    MCUMAX_PLAY_MOVE,
};

struct {
    uint8_t board[0x80 + 1];
    uint8_t current_side;
    int32_t score;
    uint8_t en_passant_square;
    int32_t non_pawn_material;
    uint8_t square_from;
    uint8_t square_to;
    uint32_t node_count;
    uint32_t node_max;
    uint32_t depth_max;
    bool stop_search;
    mcumax_callback user_callback;
    void *user_data;
    mcumax_move *valid_moves_buffer;
    uint32_t valid_moves_buffer_size;
    uint32_t valid_moves_num;
} mcumax;

static const int8_t mcumax_capture_values[] = { 0, 2, 2, 7, -1, 8, 12, 23 };
static const int8_t mcumax_step_vectors_indices[] = { 0, 7, -1, 11, 6, 8, 3, 6 };
static const int8_t mcumax_step_vectors[] = {
    -16, -15, -17, 0,
    1, 16, 0,
    1, 16, 15, 17, 0,
    14, 18, 31, 33, 0
};
static const int8_t mcumax_board_setup[] = {
    MCUMAX_ROOK, MCUMAX_KNIGHT, MCUMAX_BISHOP, MCUMAX_QUEEN,
    MCUMAX_KING, MCUMAX_BISHOP, MCUMAX_KNIGHT, MCUMAX_ROOK,
};

static u64 search_start_time;
static uint32_t search_time_limit_ms;
static bool time_limit_enabled;

static int32_t mcumax_search(int32_t alpha, int32_t beta, int32_t score,
                             uint8_t en_passant_square, uint8_t depth, enum mcumax_mode mode);

void mcumax_stop_search(void) {
    mcumax.stop_search = true;
}

void mcumax_set_callback(mcumax_callback callback, void *userdata) {
    mcumax.user_callback = callback;
    mcumax.user_data = userdata;
}

void dynamic_search_time_callback(void *userdata) {
    if (time_limit_enabled) {
        u64 current_time = osGetTime();
        u64 elapsed_ms = current_time - search_start_time;
        if (elapsed_ms >= (u64)search_time_limit_ms) {
            mcumax_stop_search();
        }
    }
}

static int32_t mcumax_search(int32_t alpha, int32_t beta, int32_t score,
                             uint8_t en_passant_square, uint8_t depth, enum mcumax_mode mode) {
    if (mcumax.user_callback)
        mcumax.user_callback(mcumax.user_data);

    uint8_t iter_depth;
    int32_t iter_score;
    uint8_t iter_square_from;
    uint8_t iter_square_to;
    uint8_t square_start;
    uint8_t square_from;
    uint8_t square_to;
    uint8_t replay_move;
    int32_t null_move_score;
    uint8_t scan_piece;
    uint8_t scan_piece_type;
    int8_t step_vector;
    int8_t step_vector_index;
    uint8_t castling_skip_square;
    uint8_t castling_rook_square;
    uint8_t capture_square;
    uint8_t capture_piece;
    int32_t capture_piece_value;
    uint8_t step_depth;
    int32_t step_alpha;
    int32_t step_score;
    int32_t step_score_new;

    alpha -= alpha < score;
    beta -= beta <= score;

    iter_depth = iter_score = iter_square_from = iter_square_to = 0;

    while ((iter_depth++ < depth) || (iter_depth < 3) ||
           ((mode != MCUMAX_INTERNAL_NODE) && (mcumax.square_from == MCUMAX_SQUARE_INVALID) &&
            (((mcumax.node_count < mcumax.node_max) && (iter_depth <= mcumax.depth_max)) ||
             (mcumax.square_from = iter_square_from,
              mcumax.square_to = iter_square_to & ~MCUMAX_BOARD_MASK,
              iter_depth = 3)))) {
        if (mcumax.stop_search)
            break;

        square_from = square_start = (mode != MCUMAX_SEARCH_VALID_MOVES) ? iter_square_from : 0;
        replay_move = iter_square_to & MCUMAX_SQUARE_INVALID;
        mcumax.current_side ^= 0x18;

        null_move_score = (iter_depth > 2) && (beta != -MCUMAX_SCORE_MAX)
                              ? mcumax_search(-beta, 1 - beta, -score, MCUMAX_SQUARE_INVALID, iter_depth - 3, MCUMAX_INTERNAL_NODE)
                              : MCUMAX_SCORE_MAX;

        mcumax.current_side ^= 0x18;

        iter_score = (-null_move_score < beta) || (mcumax.non_pawn_material > 35)
                         ? (iter_depth - 2) ? -MCUMAX_SCORE_MAX : score
                         : -null_move_score;

        mcumax.node_count++;

        do {
            scan_piece = mcumax.board[square_from];
            if (scan_piece & mcumax.current_side) {
                step_vector = scan_piece_type = (scan_piece & 0b111);
                step_vector_index = mcumax_step_vectors_indices[scan_piece_type];

                while ((step_vector = ((scan_piece_type > 2) && (step_vector < 0))
                                          ? -step_vector
                                          : -mcumax_step_vectors[++step_vector_index])) {
                replay:
                    square_to = square_from;
                    castling_skip_square = castling_rook_square = MCUMAX_SQUARE_INVALID;

                    do {
                        capture_square = square_to = replay_move ? (iter_square_to ^ replay_move) : (square_to + step_vector);
                        if (square_to & MCUMAX_BOARD_MASK)
                            break;

                        if ((en_passant_square != MCUMAX_SQUARE_INVALID) && mcumax.board[en_passant_square] &&
                            ((square_to - en_passant_square) < 2) && ((en_passant_square - square_to) < 2))
                            iter_score = MCUMAX_SCORE_MAX;

                        if ((scan_piece_type < 3) && (square_to == en_passant_square))
                            capture_square ^= 16;

                        capture_piece = mcumax.board[capture_square];
                        if ((capture_piece & mcumax.current_side) ||
                            ((scan_piece_type < 3) && !((square_to - square_from) & 0b111) - !capture_piece))
                            break;

                        capture_piece_value = 37 * mcumax_capture_values[capture_piece & 0b111] + (capture_piece & 0xc0);
                        if (capture_piece_value < 0) {
                            iter_score = MCUMAX_SCORE_MAX;
                            iter_depth = MCUMAX_DEPTH_MAX - 1;
                        }

                        if ((iter_score >= beta) && (iter_depth > 1))
                            goto cutoff;

                        step_score = (iter_depth != 1) ? score : capture_piece_value - scan_piece_type;

                        if ((iter_depth - !capture_piece) > 1) {
                            step_score = (scan_piece_type < 6)
                                             ? mcumax.board[square_from + 0x8] - mcumax.board[square_to + 0x8]
                                             : 0;

                            mcumax.board[castling_rook_square] = mcumax.board[capture_square] = mcumax.board[square_from] = 0;
                            mcumax.board[square_to] = scan_piece | MCUMAX_PIECE_MOVED;

                            if (!(castling_rook_square & MCUMAX_BOARD_MASK)) {
                                mcumax.board[castling_skip_square] = mcumax.current_side + 6;
                                step_score += 50;
                            }

                            step_score -= ((scan_piece_type != 4) || (mcumax.non_pawn_material > 30)) ? 0 : 20;

                            if (scan_piece_type < 3) {
                                step_score -= 9 * ((((square_from - 2) & MCUMAX_BOARD_MASK) || mcumax.board[square_from - 2] - scan_piece) +
                                         (((square_from + 2) & MCUMAX_BOARD_MASK) || mcumax.board[square_from + 2] - scan_piece) - 1 +
                                         (mcumax.board[square_from ^ 0x10] == (mcumax.current_side + 36))) - (mcumax.non_pawn_material >> 2);

                                capture_piece_value += step_alpha = (square_to + step_vector + 1) & MCUMAX_SQUARE_INVALID
                                            ? (647 - scan_piece_type)
                                            : 2 * (scan_piece & (square_to + 0x10) & 0x20);

                                mcumax.board[square_to] += step_alpha;
                            }

                            step_score += score + capture_piece_value;
                            step_alpha = iter_score > alpha ? iter_score : alpha;
                            step_depth = iter_depth - 1 - ((iter_depth > 5) && (scan_piece_type > 2) && !capture_piece && !replay_move);

                            if (!((mcumax.non_pawn_material > 30) || (null_move_score - MCUMAX_SCORE_MAX) ||
                                  (iter_depth < 3) || (capture_piece && (scan_piece_type != 4))))
                                step_depth = iter_depth;

                            do {
                                mcumax.current_side ^= 0x18;
                                step_score_new = ((mode == MCUMAX_SEARCH_VALID_MOVES) || (step_depth > 2) || (step_score > step_alpha))
                                                     ? -mcumax_search(-beta, -step_alpha, -step_score, castling_skip_square, step_depth, MCUMAX_INTERNAL_NODE)
                                                     : step_score;
                                mcumax.current_side ^= 0x18;
                            } while ((step_score_new > alpha) && (++step_depth < iter_depth));

                            step_score = step_score_new;

                            if ((mode == MCUMAX_PLAY_MOVE) && (step_score != -MCUMAX_SCORE_MAX) &&
                                (square_from == mcumax.square_from) && (square_to == mcumax.square_to)) {
                                mcumax.score = -score - capture_piece_value;
                                mcumax.en_passant_square = castling_skip_square;
                                mcumax.non_pawn_material += capture_piece_value >> 7;
                                mcumax.current_side ^= 0x18;
                                return beta;
                            }

                            mcumax.board[castling_rook_square] = mcumax.current_side + 6;
                            mcumax.board[castling_skip_square] = mcumax.board[square_to] = 0;
                            mcumax.board[square_from] = scan_piece;
                            mcumax.board[capture_square] = capture_piece;

                            if ((mode == MCUMAX_SEARCH_BEST_MOVE) && (step_score != -MCUMAX_SCORE_MAX) &&
                                (square_from == mcumax.square_from) && (square_to == mcumax.square_to))
                                return beta;

                            if ((mode == MCUMAX_SEARCH_VALID_MOVES) && (step_score != -MCUMAX_SCORE_MAX) &&
                                (mcumax.square_from == MCUMAX_SQUARE_INVALID) && (iter_depth == 3) && !replay_move) {
                                mcumax_move move = { square_from, square_to };
                                if (mcumax.valid_moves_num < mcumax.valid_moves_buffer_size)
                                    mcumax.valid_moves_buffer[mcumax.valid_moves_num] = move;
                                mcumax.valid_moves_num++;
                            }
                        }

                        if (step_score > iter_score) {
                            iter_score = step_score;
                            iter_square_from = square_from;
                            iter_square_to = square_to | (castling_skip_square & MCUMAX_SQUARE_INVALID);
                        }

                        if (replay_move) {
                            replay_move = 0;
                            goto replay;
                        }

                        if ((square_from + step_vector - square_to) || (scan_piece & MCUMAX_PIECE_MOVED) ||
                            ((scan_piece_type > 2) &&
                             (((scan_piece_type != 4) || (step_vector_index != 7) ||
                               (mcumax.board[castling_rook_square = ((square_from + 3) ^ ((step_vector >> 1) & 0b111))] - mcumax.current_side - 6) ||
                               mcumax.board[castling_rook_square ^ 1] || mcumax.board[castling_rook_square ^ 2]))))
                            capture_piece += (scan_piece_type < 5);
                        else
                            castling_skip_square = square_to;

                    } while (!capture_piece);
                }
            }
        } while ((square_from = ((square_from + 9) & ~MCUMAX_BOARD_MASK)) != square_start);

    cutoff:
        if ((iter_score == -MCUMAX_SCORE_MAX) && (null_move_score != MCUMAX_SCORE_MAX))
            iter_score = 0;

        if (mode == MCUMAX_SEARCH_BEST_MOVE) {
            u64 current_time = osGetTime();
            u64 elapsed_ms = current_time - search_start_time;
            if (elapsed_ms == 0) elapsed_ms = 1;

            char move_str[6];
            char f1 = 'a' + (iter_square_from & 0x7);
            char r1 = '8' - (iter_square_from >> 4);
            char f2 = 'a' + (iter_square_to & 0x7);
            char r2 = '8' - ((iter_square_to >> 4) & 0x7);

            bool is_promo = false;
            uint8_t piece = mcumax.board[iter_square_from] & 0x7;
            if ((piece == MCUMAX_PAWN_UPSTREAM || piece == MCUMAX_PAWN_DOWNSTREAM) && (r2 == '8' || r2 == '1')) {
                is_promo = true;
            }

            if (is_promo) sprintf(move_str, "%c%c%c%cq", f1, r1, f2, r2);
            else sprintf(move_str, "%c%c%c%c", f1, r1, f2, r2);

            int current_depth = (iter_depth - 2 > 0) ? (iter_depth - 2) : 1;

            if (iter_score > MCUMAX_SCORE_MAX - 100) {
                int mate_in_plies = MCUMAX_SCORE_MAX - iter_score;
                int mate_in_moves = (mate_in_plies + 1) / 2;
                printf("info depth %d score mate %d time %u nodes %u pv %s\n",
                       current_depth, mate_in_moves, (uint32_t)elapsed_ms, mcumax.node_count, move_str);
            } else if (iter_score < -MCUMAX_SCORE_MAX + 100) {
                int mate_in_plies = MCUMAX_SCORE_MAX + iter_score;
                int mate_in_moves = -(mate_in_plies + 1) / 2;
                printf("info depth %d score mate %d time %u nodes %u pv %s\n",
                       current_depth, mate_in_moves, (uint32_t)elapsed_ms, mcumax.node_count, move_str);
            } else {
                int score_cp = (iter_score * 100) / 74;
                printf("info depth %d score cp %d time %u nodes %u pv %s\n",
                       current_depth, score_cp, (uint32_t)elapsed_ms, mcumax.node_count, move_str);
            }
        }
    }

    return iter_score += iter_score < score;
}

void mcumax_init() {
    for (uint32_t x = 0; x < 8; x++) {
        mcumax.board[0x10 * 0 + x] = MCUMAX_BOARD_BLACK | mcumax_board_setup[x];
        mcumax.board[0x10 * 1 + x] = MCUMAX_BOARD_BLACK | MCUMAX_PAWN_DOWNSTREAM;
        for (uint32_t y = 2; y < 6; y++)
            mcumax.board[0x10 * y + x] = MCUMAX_EMPTY;
        mcumax.board[0x10 * 6 + x] = MCUMAX_BOARD_WHITE | MCUMAX_PAWN_UPSTREAM;
        mcumax.board[0x10 * 7 + x] = MCUMAX_BOARD_WHITE | mcumax_board_setup[x];

        for (uint32_t y = 0; y < 8; y++)
            mcumax.board[16 * y + x + 8] = (x - 4) * (x - 4) + (y - 4) * (y - 3);
    }
    mcumax.current_side = MCUMAX_BOARD_WHITE;
    mcumax.score = 0;
    mcumax.en_passant_square = MCUMAX_SQUARE_INVALID;
    mcumax.non_pawn_material = 0;
}

static mcumax_square mcumax_set_piece(mcumax_square square, mcumax_piece piece) {
    if (square & MCUMAX_BOARD_MASK) return square;
    mcumax.board[square] = piece ? (piece | MCUMAX_PIECE_MOVED) : piece;
    return square + 1;
}

mcumax_piece mcumax_get_piece(mcumax_square square) {
    if (square & MCUMAX_BOARD_MASK) return MCUMAX_EMPTY;
    return (mcumax.board[square] & 0xf) ^ MCUMAX_BLACK;
}

void mcumax_set_fen_position(const char *fen_string) {
    mcumax_init();
    uint32_t field_index = 0;
    uint32_t board_index = 0;
    char c;
    while ((c = *fen_string++)) {
        if (c == ' ') {
            if (field_index < 4) field_index++;
            continue;
        }
        switch (field_index) {
        case 0:
            if (board_index < 0x80) {
                switch (c) {
                case '8': case '7': case '6': case '5':
                case '4': case '3': case '2': case '1': {
                    for (int32_t i = 0; i < (c - '0'); i++)
                        board_index = mcumax_set_piece(board_index, MCUMAX_EMPTY);
                    break;
                }
                case 'P': board_index = mcumax_set_piece(board_index, MCUMAX_PAWN_UPSTREAM | MCUMAX_BOARD_WHITE); break;
                case 'N': board_index = mcumax_set_piece(board_index, MCUMAX_KNIGHT | MCUMAX_BOARD_WHITE); break;
                case 'B': board_index = mcumax_set_piece(board_index, MCUMAX_BISHOP | MCUMAX_BOARD_WHITE); break;
                case 'R': board_index = mcumax_set_piece(board_index, MCUMAX_ROOK | MCUMAX_BOARD_WHITE); break;
                case 'Q': board_index = mcumax_set_piece(board_index, MCUMAX_QUEEN | MCUMAX_BOARD_WHITE); break;
                case 'K': board_index = mcumax_set_piece(board_index, MCUMAX_KING | MCUMAX_BOARD_WHITE); break;
                case 'p': board_index = mcumax_set_piece(board_index, MCUMAX_PAWN_DOWNSTREAM | MCUMAX_BOARD_BLACK); break;
                case 'n': board_index = mcumax_set_piece(board_index, MCUMAX_KNIGHT | MCUMAX_BOARD_BLACK); break;
                case 'b': board_index = mcumax_set_piece(board_index, MCUMAX_BISHOP | MCUMAX_BOARD_BLACK); break;
                case 'r': board_index = mcumax_set_piece(board_index, MCUMAX_ROOK | MCUMAX_BOARD_BLACK); break;
                case 'q': board_index = mcumax_set_piece(board_index, MCUMAX_QUEEN | MCUMAX_BOARD_BLACK); break;
                case 'k': board_index = mcumax_set_piece(board_index, MCUMAX_KING | MCUMAX_BOARD_BLACK); break;
                case '/': board_index = (board_index < 0x80) ? (board_index & 0xf0) + 0x10 : board_index; break;
                }
            }
            break;
        case 1:
            if (c == 'w') mcumax.current_side = MCUMAX_BOARD_WHITE;
            else if (c == 'b') mcumax.current_side = MCUMAX_BOARD_BLACK;
            break;
        case 2:
            switch (c) {
            case 'K': mcumax.board[0x74] &= ~MCUMAX_PIECE_MOVED; mcumax.board[0x77] &= ~MCUMAX_PIECE_MOVED; break;
            case 'Q': mcumax.board[0x74] &= ~MCUMAX_PIECE_MOVED; mcumax.board[0x70] &= ~MCUMAX_PIECE_MOVED; break;
            case 'k': mcumax.board[0x04] &= ~MCUMAX_PIECE_MOVED; mcumax.board[0x07] &= ~MCUMAX_PIECE_MOVED; break;
            case 'q': mcumax.board[0x04] &= ~MCUMAX_PIECE_MOVED; mcumax.board[0x00] &= ~MCUMAX_PIECE_MOVED; break;
            }
            break;
        case 3:
            if (c >= 'a' && c <= 'h') {
                mcumax.en_passant_square &= 0x7f;
                mcumax.en_passant_square |= (c - 'a');
            } else if (c >= '1' && c <= '8') {
                mcumax.en_passant_square &= 0x7f;
                mcumax.en_passant_square |= 16 * ('8' - c);
            }
            break;
        }
    }
}

mcumax_piece mcumax_get_current_side(void) {
    return mcumax.current_side;
}

static int32_t mcumax_start_search(enum mcumax_mode mode, mcumax_move move, uint32_t depth_max, uint32_t node_max) {
    mcumax.square_from = move.from;
    mcumax.square_to = move.to;
    mcumax.node_max = node_max;
    mcumax.node_count = 0;
    mcumax.depth_max = depth_max;
    mcumax.stop_search = false;

    return mcumax_search(-MCUMAX_SCORE_MAX, MCUMAX_SCORE_MAX, mcumax.score, mcumax.en_passant_square, 3, mode);
}

uint32_t mcumax_search_valid_moves(mcumax_move *valid_moves_buffer, uint32_t valid_moves_buffer_size) {
    mcumax.valid_moves_num = 0;
    mcumax.valid_moves_buffer = valid_moves_buffer;
    mcumax.valid_moves_buffer_size = valid_moves_buffer_size;
    mcumax_start_search(MCUMAX_SEARCH_VALID_MOVES, MCUMAX_MOVE_INVALID, 0, 0);
    return mcumax.valid_moves_num;
}

mcumax_move mcumax_search_best_move(uint32_t node_max, uint32_t depth_max) {
    int32_t score = mcumax_start_search(MCUMAX_SEARCH_BEST_MOVE, MCUMAX_MOVE_INVALID, depth_max + 3, node_max);
    if (score == MCUMAX_SCORE_MAX)
        return (mcumax_move){ mcumax.square_from, mcumax.square_to };
    else
        return MCUMAX_MOVE_INVALID;
}

bool mcumax_play_move(mcumax_move move) {
    return mcumax_start_search(MCUMAX_PLAY_MOVE, move, 0, 0) == MCUMAX_SCORE_MAX;
}

#define MAIN_VALID_MOVES_NUM 512

mcumax_square get_square(char *s) {
    mcumax_square rank = s[0] - 'a';
    if (rank > 7) return MCUMAX_SQUARE_INVALID;
    mcumax_square file = '8' - s[1];
    if (file > 7) return MCUMAX_SQUARE_INVALID;
    return 0x10 * file + rank;
}

bool is_square_valid(char *s) {
    return (get_square(s) != MCUMAX_SQUARE_INVALID);
}

bool is_move_valid(char *s) {
    return is_square_valid(s) && is_square_valid(s + 2);
}

void print_square(mcumax_square square) {
    printf("%c%c", 'a' + ((square & 0x07) >> 0), '1' + 7 - ((square & 0x70) >> 4));
}

void print_move(mcumax_move move) {
    if ((move.from == MCUMAX_SQUARE_INVALID) || (move.to == MCUMAX_SQUARE_INVALID))
        printf("(none)");
    else {
        print_square(move.from);
        print_square(move.to);
    }
}

bool send_uci_command(char *line) {
    char *token = strtok(line, " \n");
    if (!token) return false;

    if (!strcmp(token, "uci")) {
        printf("id name " MCUMAX_ID "\n");
        printf("id author " MCUMAX_AUTHOR "\n");
        printf("uciok\n");
    } else if (!strcmp(token, "ucinewgame")) {
        mcumax_init();
    } else if (!strcmp(token, "isready")) {
        printf("readyok\n");
    } else if (!strcmp(token, "position")) {
        int fen_index = 0;
        char fen_string[256];
        while ((token = strtok(NULL, " \n"))) {
            if (fen_index) {
                strcat(fen_string, token);
                strcat(fen_string, " ");
                fen_index++;
                if (fen_index > 6) {
                    mcumax_set_fen_position(fen_string);
                    fen_index = 0;
                }
            } else {
                if (!strcmp(token, "startpos"))
                    mcumax_init();
                else if (!strcmp(token, "fen")) {
                    fen_index = 1;
                    strcpy(fen_string, "");
                } else if (is_move_valid(token)) {
                    mcumax_play_move((mcumax_move){ get_square(token + 0), get_square(token + 2) });
                }
            }
        }
    } else if (!strcmp(token, "go")) {
        uint32_t depth_max = 30;
        uint32_t node_max = 100000000;
        search_time_limit_ms = 0;
        time_limit_enabled = false;
        uint32_t wtime = 0, btime = 0, winc = 0, binc = 0;
        bool has_wtime = false, has_btime = false;

        while ((token = strtok(NULL, " \n"))) {
            if (!strcmp(token, "infinite")) {
                depth_max = MCUMAX_DEPTH_MAX;
                node_max = 0xFFFFFFFF;
                time_limit_enabled = false;
            } else if (!strcmp(token, "depth")) {
                char *val = strtok(NULL, " \n");
                if (val) depth_max = atoi(val);
            } else if (!strcmp(token, "nodes")) {
                char *val = strtok(NULL, " \n");
                if (val) node_max = strtoul(val, NULL, 10);
            } else if (!strcmp(token, "movetime")) {
                char *val = strtok(NULL, " \n");
                if (val) {
                    search_time_limit_ms = atoi(val);
                    time_limit_enabled = true;
                }
            } else if (!strcmp(token, "wtime")) {
                char *val = strtok(NULL, " \n");
                if (val) { wtime = atoi(val); has_wtime = true; }
            } else if (!strcmp(token, "btime")) {
                char *val = strtok(NULL, " \n");
                if (val) { btime = atoi(val); has_btime = true; }
            } else if (!strcmp(token, "winc")) {
                char *val = strtok(NULL, " \n");
                if (val) winc = atoi(val);
            } else if (!strcmp(token, "binc")) {
                char *val = strtok(NULL, " \n");
                if (val) binc = atoi(val);
            }
        }

        if (!time_limit_enabled) {
            uint8_t active_side = mcumax_get_current_side();
            if (active_side == MCUMAX_BOARD_WHITE && has_wtime) {
                search_time_limit_ms = (wtime / 25) + (winc / 2);
                time_limit_enabled = true;
            } else if (active_side == MCUMAX_BOARD_BLACK && has_btime) {
                search_time_limit_ms = (btime / 25) + (binc / 2);
                time_limit_enabled = true;
            }
        }

        if (time_limit_enabled && (search_time_limit_ms < 20)) {
            search_time_limit_ms = 20;
        }

        search_start_time = osGetTime();
        mcumax_set_callback(dynamic_search_time_callback, NULL);

        mcumax_move move = mcumax_search_best_move(node_max, depth_max);

        u64 end_time = osGetTime();
        double elapsed_seconds = (double)(end_time - search_start_time) / 1000.0;
        if (elapsed_seconds < 0.001) elapsed_seconds = 0.001;

        uint32_t nodes_searched = mcumax.node_count;
        uint32_t time_ms = (uint32_t)(elapsed_seconds * 1000.0);
        uint32_t nps = (uint32_t)((double)nodes_searched / elapsed_seconds);

        printf("info time %u nodes %u nps %u\n", time_ms, nodes_searched, nps);

        mcumax_play_move(move);

        printf("bestmove ");
        print_move(move);
        printf("\n");
    } else if (!strcmp(token, "quit")) {
        return true;
    }
    return false;
}

// Thread-safe IO Overrides
int engine_printf(const char *format, ...) {
    char temp[1024];
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(temp, sizeof(temp), format, args);
    va_end(args);
    queue_write(&q_to_gui, temp);
    return ret;
}

char *engine_fgets(char *str, int n, FILE *stream) {
    (void)stream;
    while (1) {
        int read_chars = queue_read_line(&q_to_engine, str, n);
        if (read_chars > 0) {
            return str;
        }
        svcSleepThread(5000000LL); // Sleep 5ms to preserve battery life
    }
}

// Engine execution loop running on Thread 2
void engine_thread_func(void *arg) {
    (void)arg;
    mcumax_init();
    while (true) {
        char line[4096];
        if (!fgets(line, sizeof(line), NULL))
            break;
        if (send_uci_command(line))
            break;
    }
}

/* ==========================================================================
 * PART 3: THE INTERACTIVE GRAPHICAL FRONTEND (GUI)
 * ========================================================================== */

#undef printf
#undef fgets

#define MAX_HISTORY 2048

typedef struct {
    int board[64];
    int turn;      
    int castle;    
    int ep;        
    int halfmoves; 
    int fullmoves;
} BoardState;

typedef struct {
    int from;
    int to;
    int promo; 
} Move;

BoardState current_state;
BoardState history[MAX_HISTORY];
Move move_history[MAX_HISTORY];
int history_count = 0;

int cursor_r = 6;  
int cursor_c = 4;  
int selected_sq = -1;

int board_orientation = 1; 
int user_side = 1;         

int time_control_type = 0;   
int time_control_val = 500; // default 500 ms for 3DS response

int engine_thinking = 0;
long long engine_nps = 0;    
char engine_buffer[8192];
int engine_buf_len = 0;
int game_running = 1;

void init_board(BoardState *state);
int is_legal_move(const BoardState *state, Move m);
int has_legal_moves(const BoardState *state);
int is_square_attacked(const BoardState *state, int sq, int attacker);
void make_move(const BoardState *src, BoardState *dst, Move m);
void print_side_panel_line(int panel_row);
void print_recent_moves(int row);
void send_to_engine(const char *cmd);
int find_king(const BoardState *state, int color);
int count_repetitions(const BoardState *state);
int get_promo_choice();

void send_to_engine(const char *cmd) {
    queue_write(&q_to_engine, cmd);
}

int screen_to_board_sq(int r, int c) {
    if (board_orientation == 1) {
        return r * 8 + c;
    } else {
        return (7 - r) * 8 + (7 - c);
    }
}

void move_to_uci(Move m, char *buf) {
    int f_col = m.from % 8;
    int f_row = 8 - (m.from / 8);
    int t_col = m.to % 8;
    int t_row = 8 - (m.to / 8);
    sprintf(buf, "%c%d%c%d", 'a' + f_col, f_row, 'a' + t_col, t_row);
    if (m.promo != 0) {
        char p = 'q';
        if (m.promo == 2) p = 'n';
        if (m.promo == 3) p = 'b';
        if (m.promo == 4) p = 'r';
        sprintf(buf + 4, "%c", p);
    }
}

Move uci_to_move(const char *str) {
    Move m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f_col = str[0] - 'a';
    int f_row = 8 - (str[1] - '0');
    int t_col = str[2] - 'a';
    int t_row = 8 - (str[3] - '0');
    m.from = f_row * 8 + f_col;
    m.to = t_row * 8 + t_col;
    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'n') m.promo = 2;
        else if (p == 'b') m.promo = 3;
        else if (p == 'r') m.promo = 4;
        else m.promo = 5;
    }
    return m;
}

void push_state(const BoardState *state, Move m) {
    if (history_count < MAX_HISTORY - 1) {
        history[history_count] = *state;
        move_history[history_count] = m;
        history_count++;
    }
}

void trigger_engine_move() {
    engine_nps = 0; 
    static char cmd[8192];
    cmd[0] = '\0';
    strcpy(cmd, "position startpos moves");
    
    int len = strlen(cmd);
    for (int i = 0; i < history_count; i++) {
        char uci_m[10];
        move_to_uci(move_history[i], uci_m);
        int move_len = strlen(uci_m);
        if (len + 1 + move_len + 2 >= (int)sizeof(cmd)) {
            break; 
        }
        cmd[len++] = ' ';
        strcpy(cmd + len, uci_m);
        len += move_len;
    }
    cmd[len++] = '\n';
    cmd[len] = '\0';
    
    send_to_engine(cmd);

    char go_cmd[256];
    if (time_control_type == 0) {
        sprintf(go_cmd, "go movetime %d\n", time_control_val);
    } else if (time_control_type == 1) {
        sprintf(go_cmd, "go depth %d\n", time_control_val);
    } else {
        sprintf(go_cmd, "go nodes %d\n", time_control_val);
    }
    send_to_engine(go_cmd);
}

void process_engine_output(char *line) {
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[len - 1] = '\0';
        len--;
    }

    if (strncmp(line, "info", 4) == 0) {
        char *nps_ptr = strstr(line, " nps ");
        if (nps_ptr) {
            long long val;
            if (sscanf(nps_ptr, " nps %lld", &val) == 1) {
                engine_nps = val;
            }
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16];
        if (sscanf(line, "bestmove %15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 || strcmp(move_str, "NULL") == 0) {
                engine_thinking = 0;
                return;
            }
            Move m = uci_to_move(move_str);
            if (is_legal_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_move(&current_state, &next, m);
                current_state = next;
            }
            engine_thinking = 0;
        }
    }
}

void read_from_engine() {
    char tmp[1024];
    int n;
    while ((n = queue_read(&q_to_gui, tmp, sizeof(tmp))) > 0) {
        for (int i = 0; i < n; i++) {
            if (tmp[i] == '\n') {
                if (engine_buf_len > 0) {
                    engine_buffer[engine_buf_len] = '\0';
                    process_engine_output(engine_buffer);
                    engine_buf_len = 0;
                }
            } else if (tmp[i] != '\r') {
                if (engine_buf_len < (int)sizeof(engine_buffer) - 1) {
                    engine_buffer[engine_buf_len++] = tmp[i];
                } else {
                    engine_buffer[engine_buf_len] = '\0';
                    process_engine_output(engine_buffer);
                    engine_buf_len = 0;
                    engine_buffer[engine_buf_len++] = tmp[i];
                }
            }
        }
    }
}

int find_king(const BoardState *state, int color) {
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == color * 6) return i;
    }
    return -1;
}

int is_square_attacked(const BoardState *state, int sq, int attacker) {
    int r = sq / 8, c = sq % 8;

    int k_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int k_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 2) return 1;
        }
    }

    int kg_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int kg_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kg_r[i], nc = c + kg_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 6) return 1;
        }
    }

    int p_offset = (attacker == 1) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int nr = r + p_offset, nc = c + dc;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 1) return 1;
        }
    }

    int d_r[] = {-1, -1, 1, 1};
    int d_c[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + d_r[i], nc = c + d_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 3 || target == attacker * 5) return 1;
                break;
            }
            nr += d_r[i]; nc += d_c[i];
        }
    }

    int s_r[] = {-1, 1, 0, 0};
    int s_c[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + s_r[i], nc = c + s_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 4 || target == attacker * 5) return 1;
                break;
            }
            nr += s_r[i]; nc += s_c[i];
        }
    }
    return 0;
}

int is_pseudo_legal_move(const BoardState *state, Move m) {
    int p = state->board[m.from];
    int target = state->board[m.to];
    int turn = state->turn;

    if (p == 0) return 0;
    if ((turn == 1 && p < 0) || (turn == -1 && p > 0)) return 0;
    if (m.from == m.to) return 0;
    if (target != 0 && ((turn == 1 && target > 0) || (turn == -1 && target < 0))) return 0;

    int fr = m.from / 8, fc = m.from % 8;
    int tr = m.to / 8, tc = m.to % 8;
    int dr = tr - fr, dc = tc - fc;
    int abs_dr = abs(dr), abs_dc = abs(dc);

    switch (abs(p)) {
        case 1: { 
            int dir = (turn == 1) ? -1 : 1;
            if (dc == 0 && dr == dir && target == 0) return 1;
            int start_r = (turn == 1) ? 6 : 1;
            if (dc == 0 && fr == start_r && dr == 2 * dir) {
                if (state->board[(fr + dir) * 8 + fc] == 0 && target == 0) return 1;
            }
            if (abs_dc == 1 && dr == dir) {
                if (target != 0) return 1;
                if (m.to == state->ep) return 1;
            }
            return 0;
        }
        case 2: 
            return ((abs_dr == 2 && abs_dc == 1) || (abs_dr == 1 && abs_dc == 2));
        case 3: { 
            if (abs_dr != abs_dc) return 0;
            int sr = (dr > 0) ? 1 : -1, sc = (dc > 0) ? 1 : -1;
            int r = fr + sr, c = fc + sc;
            while (r != tr && c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 4: { 
            if (dr != 0 && dc != 0) return 0;
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 5: { 
            if (abs_dr != abs_dc && dr != 0 && dc != 0) return 0;
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            if (abs_dr == abs_dc) {
                sr = (dr > 0) ? 1 : -1;
                sc = (dc > 0) ? 1 : -1;
            }
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 6: { 
            if (abs_dr <= 1 && abs_dc <= 1) return 1;
            if (dr == 0 && abs_dc == 2) {
                if (turn == 1 && fr == 7 && fc == 4) {
                    if (m.to == 62 && (state->castle & 1)) {
                        if (state->board[61] == 0 && state->board[62] == 0) {
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 61, -1) &&
                                !is_square_attacked(state, 62, -1)) return 1;
                        }
                    }
                    if (m.to == 58 && (state->castle & 2)) {
                        if (state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0) {
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 59, -1) &&
                                !is_square_attacked(state, 58, -1)) return 1;
                        }
                    }
                }
                if (turn == -1 && fr == 0 && fc == 4) {
                    if (m.to == 6 && (state->castle & 4)) {
                        if (state->board[5] == 0 && state->board[6] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 5, 1) &&
                                !is_square_attacked(state, 6, 1)) return 1;
                        }
                    }
                    if (m.to == 2 && (state->castle & 8)) {
                        if (state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 3, 1) &&
                                !is_square_attacked(state, 2, 1)) return 1;
                        }
                    }
                }
            }
            return 0;
        }
    }
    return 0;
}

int is_legal_move(const BoardState *state, Move m) {
    if (!is_pseudo_legal_move(state, m)) return 0;
    BoardState next;
    make_move(state, &next, m);
    int king = find_king(&next, state->turn);
    if (king == -1) return 0;
    return !is_square_attacked(&next, king, -state->turn);
}

int has_legal_moves(const BoardState *state) {
    for (int f = 0; f < 64; f++) {
        if (state->board[f] == 0) continue;
        if ((state->turn == 1 && state->board[f] < 0) || (state->turn == -1 && state->board[f] > 0)) continue;
        for (int t = 0; t < 64; t++) {
            Move m = {f, t, 0};
            if (abs(state->board[f]) == 1 && (t / 8 == 0 || t / 8 == 7)) {
                m.promo = 5; 
            }
            if (is_legal_move(state, m)) return 1;
        }
    }
    return 0;
}

int count_repetitions(const BoardState *state) {
    int count = 1; 
    for (int i = 0; i < history_count; i++) {
        if (state->turn == history[i].turn &&
            state->castle == history[i].castle &&
            state->ep == history[i].ep &&
            memcmp(state->board, history[i].board, sizeof(state->board)) == 0) {
            count++;
        }
    }
    return count;
}

void make_move(const BoardState *src, BoardState *dst, Move m) {
    *dst = *src;
    int p = dst->board[m.from];
    int is_capture = (src->board[m.to] != 0) || (abs(p) == 1 && m.to == src->ep);

    dst->board[m.from] = 0;
    if (m.promo != 0) {
        dst->board[m.to] = dst->turn * m.promo;
    } else {
        dst->board[m.to] = p;
    }

    if (abs(p) == 1 && m.to == dst->ep) {
        int p_dir = (dst->turn == 1 ? 8 : -8);
        int mt_cap = m.to + p_dir;
        dst->board[mt_cap] = 0;
    }

    dst->ep = -1;
    if (abs(p) == 1 && abs(m.from - m.to) == 16) {
        dst->ep = m.from + (dst->turn == 1 ? -8 : 8);
    }

    if (abs(p) == 6) {
        if (m.from == 60 && m.to == 62) { dst->board[61] = dst->board[63]; dst->board[63] = 0; }
        else if (m.from == 60 && m.to == 58) { dst->board[59] = dst->board[56]; dst->board[56] = 0; }
        else if (m.from == 4 && m.to == 6) { dst->board[5] = dst->board[7]; dst->board[7] = 0; }
        else if (m.from == 4 && m.to == 2) { dst->board[3] = dst->board[0]; dst->board[0] = 0; }
    }

    if (m.from == 60) dst->castle &= ~1 & ~2;
    if (m.from == 4)  dst->castle &= ~4 & ~8;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from == 7  || m.to == 7)  dst->castle &= ~4;
    if (m.from == 0  || m.to == 0)  dst->castle &= ~8;

    dst->turn = -dst->turn;
    if (abs(p) == 1 || is_capture) dst->halfmoves = 0;
    else dst->halfmoves++;
    if (dst->turn == 1) dst->fullmoves++;
}

// GUI Drawing using 3DS-supported 16-color ANSI sequence and high-legibility ASCII layout
void draw_ui() {
    printf("\033[H\r\n"); 

    const char *turn_str = (current_state.turn == 1) ? "\033[1;31mWhite\033[0m" : "\033[1;30mBlack\033[0m";
    int king = find_king(&current_state, current_state.turn);
    int is_ch = is_square_attacked(&current_state, king, -current_state.turn);
    int has_mov = has_legal_moves(&current_state);
    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";
    int repetitions = count_repetitions(&current_state);

    printf("  ");
    if (current_state.halfmoves >= 100) {
        printf("\033[1;36mDRAW (50-move rule)\033[0m");
    } else if (repetitions >= 3) {
        printf("\033[1;36mDRAW (threefold repetition)\033[0m");
    } else if (!has_mov) {
        if (is_ch) printf("\033[1;31mCHECKMATE!\033[0m");
        else printf("\033[1;36mSTALEMATE!\033[0m");
    } else if (is_ch) {
        printf("%s (\033[1;31mCHECK!\033[0m)", turn_str);
    } else {
        printf("%s's Turn", turn_str);
    }
    printf(" | W:%s B:%s", w_play, b_play);

    const char *types[] = {"Time", "Depth", "Nodes"};
    printf(" | %s", types[time_control_type]);
    if (time_control_type == 0) {
        printf(" (%d ms)", time_control_val);
    } else if (time_control_type == 1) {
        printf(" (depth %d)", time_control_val);
    } else {
        printf(" (%d nodes)", time_control_val);
    }
    printf("\033[K\r\n\r\n");

    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(0);
    printf("\033[K\r\n");

    int king_in_check = -1;
    int w_king = find_king(&current_state, 1);
    int b_king = find_king(&current_state, -1);
    if (w_king != -1 && is_square_attacked(&current_state, w_king, -1)) {
        king_in_check = w_king;
    } else if (b_king != -1 && is_square_attacked(&current_state, b_king, 1)) {
        king_in_check = b_king;
    }

    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        printf("  %d ", rank_lbl);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p = current_state.board[sq];

            int is_light = ((sq / 8) + (sq % 8)) % 2 == 0;
            const char *bg_color;

            int is_selected = (sq == selected_sq);
            int is_cursor = (r == cursor_r && c == cursor_c);

            int is_prev_move = 0;
            if (history_count > 0) {
                Move last_move = move_history[history_count - 1];
                if (sq == last_move.from || sq == last_move.to) {
                    is_prev_move = 1;
                }
            }

            int is_legal_dest = 0;
            if (selected_sq != -1) {
                Move test_m = {selected_sq, sq, 0};
                if (abs(current_state.board[selected_sq]) == 1 && (sq / 8 == 0 || sq / 8 == 7)) {
                    test_m.promo = 5;
                }
                if (is_legal_move(&current_state, test_m)) {
                    is_legal_dest = 1;
                }
            }

            // 16-Color ANSI Mapping for 3DS compatibility
            if (is_cursor) {
                bg_color = "\033[45m"; // Magenta (Active cursor)
            } else if (is_selected) {
                bg_color = "\033[42m"; // Green (Selected piece)
            } else if (sq == king_in_check) {
                bg_color = "\033[41m"; // Red (King in check)
            } else if (is_prev_move) {
                bg_color = "\033[44m"; // Blue (Previous move highlight)
            } else if (is_legal_dest) {
                bg_color = "\033[102m"; // Bright Green (Legal moves)
            } else {
                bg_color = is_light ? "\033[107m" : "\033[43m"; // Bright White vs Yellow/Brown board cells
            }

            const char *piece_str = " ";
            const char *fg_color = "\033[1;30m"; // Black Pieces default
            if (p != 0) {
                if (p > 0) fg_color = "\033[1;31m"; // Bold Red for White Pieces
                switch (abs(p)) {
                    case 1: piece_str = "P"; break;
                    case 2: piece_str = "N"; break;
                    case 3: piece_str = "B"; break;
                    case 4: piece_str = "R"; break;
                    case 5: piece_str = "Q"; break;
                    case 6: piece_str = "K"; break;
                }
            }

            // Print with clean character brackets to elevate visibility on standard console LCD
            if (p != 0) {
                if (p > 0) printf("%s%s[%s]\033[0m", bg_color, fg_color, piece_str);
                else printf("%s%s(%s)\033[0m", bg_color, fg_color, piece_str);
            } else {
                printf("%s   \033[0m", bg_color);
            }
        }

        printf(" %d ", rank_lbl);
        print_side_panel_line(r + 1);
        printf("\033[K\r\n"); 
    }

    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(9);
    printf("\033[K\r\n\r\n");

    printf(" \033[1;37m[D-Pad/C-Pad] Navigate | [A] Select | [B] Undo\033[0m\033[K\r\n");
    printf(" \033[1;37m[X] Reset Board | [Y] Flip Board | [L] Switch Sides\033[0m\033[K\r\n");
    printf(" \033[1;37m[R] Adjust Value/Limit | [START] Quit\033[0m\033[K\r\n\r\n");
    
    printf(" \033[1;32mEngine Thread Status: ACTIVE\033[0m");
    if (engine_nps > 0) {
        printf(" | NPS: %lld", engine_nps);
    }
    printf("\033[K\r\n\033[J");
}

void print_side_panel_line(int panel_row) {
    printf("   ");
    print_recent_moves(panel_row);
}

void print_recent_moves(int row) {
    int total_full_moves = (history_count + 1) / 2;
    if (total_full_moves == 0) return;
    
    int start_move = 1;
    if (total_full_moves > 10) {
        start_move = total_full_moves - 9;
    }
    int display = start_move + row;
    if (display > total_full_moves) return;

    int w_idx = (display - 1) * 2;
    int b_idx = w_idx + 1;

    printf("   %2d. ", display);
    if (w_idx < history_count) {
        char w_str[10];
        move_to_uci(move_history[w_idx], w_str);
        printf("%-6s", w_str);
    } else {
        printf("------");
    }
    printf(" ");

    if (b_idx < history_count) {
        char b_str[10];
        move_to_uci(move_history[b_idx], b_str);
        printf("%-6s", b_str);
    } else {
        if (w_idx < history_count) printf("...");
        else printf("------");
    }
}

// 3DS Hardware Key Prompter for Promotion options
int get_promo_choice() {
    printf("\n \033[1;33mPromote pawn! Press:\033[0m\n");
    printf(" [A] Queen | [B] Rook | [X] Bishop | [Y] Knight\n");
    
    int choice = 5;
    while (1) {
        hidScanInput();
        uint32_t kDown = hidKeysDown();
        if (kDown & KEY_A) { choice = 5; break; }
        if (kDown & KEY_B) { choice = 4; break; }
        if (kDown & KEY_X) { choice = 3; break; }
        if (kDown & KEY_Y) { choice = 2; break; }
        svcSleepThread(10000000ULL); // 10ms
    }
    return choice;
}

void handle_select() {
    if (!has_legal_moves(&current_state) || current_state.halfmoves >= 100 || count_repetitions(&current_state) >= 3) {
        return;
    }

    int sq = screen_to_board_sq(cursor_r, cursor_c);
    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p != 0 && ((current_state.turn == 1 && p > 0) || (current_state.turn == -1 && p < 0))) {
            selected_sq = sq;
        }
    } else {
        Move m = {selected_sq, sq, 0};
        int p = current_state.board[selected_sq];
        int is_promo = (abs(p) == 1 && (sq / 8 == 0 || sq / 8 == 7));
        if (is_promo) {
            m.promo = 5; 
        }

        if (is_legal_move(&current_state, m)) {
            if (is_promo) {
                m.promo = get_promo_choice();
            }
            push_state(&current_state, m);
            BoardState next;
            make_move(&current_state, &next, m);
            current_state = next;
            selected_sq = -1;
        } else {
            int target = current_state.board[sq];
            if (target != 0 && ((current_state.turn == 1 && target > 0) || (current_state.turn == -1 && target < 0))) {
                selected_sq = sq; 
            } else {
                selected_sq = -1; 
            }
        }
    }
}

void handle_undo() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    engine_nps = 0;
    int step_back = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step_back > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
        step_back--;
    }
    selected_sq = -1;
}

void handle_reset_board() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    engine_nps = 0;
    init_board(&current_state);
    history_count = 0;
    selected_sq = -1;
    cursor_r = 6;
    cursor_c = 4;
    send_to_engine("ucinewgame\nisready\n");
}

void handle_switch_sides() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    if (user_side == 1) user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0) user_side = 2;
    else user_side = 1;
}

void adjust_time_control() {
    if (time_control_type == 0) { 
        int time_list[] = {100, 200, 500, 1000, 2000, 5000, 10000};
        int time_count = sizeof(time_list) / sizeof(time_list[0]);
        int found_idx = -1;
        for (int i = 0; i < time_count; i++) {
            if (time_control_val == time_list[i]) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == -1 || found_idx == time_count - 1) {
            time_control_val = time_list[0];
        } else {
            time_control_val = time_list[found_idx + 1];
        }
    } else if (time_control_type == 1) { 
        time_control_val = (time_control_val % 10) + 1; // max depth 10 for performance bounds
    } else { 
        int nodes_list[] = {512, 1024, 2048, 4096, 8192, 16384, 32768};
        int nodes_count = sizeof(nodes_list) / sizeof(nodes_list[0]);
        int found_idx = -1;
        for (int i = 0; i < nodes_count; i++) {
            if (time_control_val == nodes_list[i]) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == -1 || found_idx == nodes_count - 1) {
            time_control_val = nodes_list[0];
        } else {
            time_control_val = nodes_list[found_idx + 1];
        }
    }
}

// Map 3DS D-Pad / Circle-Pad physical inputs to game controls
void handle_input_3ds() {
    hidScanInput();
    uint32_t kDown = hidKeysDown();

    if (kDown & (KEY_UP | KEY_CPAD_UP)) {
        if (cursor_r > 0) cursor_r--;
    }
    if (kDown & (KEY_DOWN | KEY_CPAD_DOWN)) {
        if (cursor_r < 7) cursor_r++;
    }
    if (kDown & (KEY_LEFT | KEY_CPAD_LEFT)) {
        if (cursor_c > 0) cursor_c--;
    }
    if (kDown & (KEY_RIGHT | KEY_CPAD_RIGHT)) {
        if (cursor_c < 7) cursor_c++;
    }
    if (kDown & KEY_A) {
        handle_select();
    }
    if (kDown & KEY_B) {
        handle_undo();
    }
    if (kDown & KEY_X) {
        handle_reset_board();
    }
    if (kDown & KEY_Y) {
        board_orientation = -board_orientation;
    }
    if (kDown & KEY_L) {
        handle_switch_sides();
    }
    if (kDown & KEY_R) {
        static int press_count = 0;
        press_count++;
        if (press_count % 2 == 0) {
            time_control_type = (time_control_type + 1) % 3;
            time_control_val = (time_control_type == 0) ? 500 : (time_control_type == 1 ? 4 : 2048);
        } else {
            adjust_time_control();
        }
    }
    if (kDown & KEY_START) {
        game_running = 0; 
    }
}

void init_board(BoardState *state) {
    int start[64] = {
        -4, -2, -3, -5, -6, -3, -2, -4,
        -1, -1, -1, -1, -1, -1, -1, -1,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         1,  1,  1,  1,  1,  1,  1,  1,
         4,  2,  3,  5,  6,  3,  2,  4
    };
    memcpy(state->board, start, sizeof(start));
    state->turn = 1;
    state->castle = 15;
    state->ep = -1;
    state->halfmoves = 0;
    state->fullmoves = 1;
}

/* ==========================================================================
 * PART 4: 3DS SYSTEM INITIALIZATION & MAIN LOOP
 * ========================================================================== */

int main() {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL); 

    printf("Starting Chess Game on 3DS...\n");
    gfxFlushBuffers();

    queue_init(&q_to_engine);
    queue_init(&q_to_gui);

    init_board(&current_state);

    // Spawn Engine on Core 0 as a background thread (Standard 3DS CPU Priority)
    Thread engine_thread = threadCreate(engine_thread_func, NULL, 64 * 1024, 0x3F, -1, false);

    // Send initialization sequence
    send_to_engine("uci\nisready\n");

    printf("\033[2J\033[H"); 

    while (aptMainLoop() && game_running) {
        handle_input_3ds();
        read_from_engine();

        int engine_active = 0;
        if (has_legal_moves(&current_state) && current_state.halfmoves < 100 && count_repetitions(&current_state) < 3) {
            if (user_side == 2) engine_active = 1;
            else if (user_side == 1 && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn == 1) engine_active = 1;
        }

        if (engine_active && !engine_thinking) {
            engine_thinking = 1;
            trigger_engine_move();
        }

        draw_ui();

        gfxFlushBuffers();
        gspWaitForVBlank(); // Synchronize to 3DS Top LCD vertical retrace (60 fps cap)
    }

    // Clean Exit
    queue_write(&q_to_engine, "quit\n");
    threadJoin(engine_thread, 1000000000ULL); // Wait up to 1 second
    threadFree(engine_thread);

    gfxExit();
    return 0;
}
