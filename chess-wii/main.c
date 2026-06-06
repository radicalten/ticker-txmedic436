/*
 * mcu-max
 * Chess game engine for low-resource MCUs
 *
 * (C) 2022-2024 Gissio
 *
 * License: MIT
 *
 * Based on micro-Max 4.8 by H.G. Muller.
 * Compliant with FIDE laws (except for underpromotion).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __wii__
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#endif

/* ==========================================================================
 * PART 1: ENGINE DEFINITIONS
 * ========================================================================== */

#define MCUMAX_ID "mcu-max 1.0.6"
#define MCUMAX_AUTHOR "Gissio"

#define MCUMAX_SQUARE_INVALID 0x80

#define MCUMAX_MOVE_INVALID \
    (mcumax_move) { MCUMAX_SQUARE_INVALID, MCUMAX_SQUARE_INVALID }

typedef uint8_t mcumax_square;
typedef uint8_t mcumax_piece;

typedef struct
{
    mcumax_square from;
    mcumax_square to;
} mcumax_move;

typedef void (*mcumax_callback)(void *);

enum
{
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

uint32_t mcumax_get_node_count(void);
void mcumax_init(void);
void mcumax_set_fen_position(const char *value);
mcumax_piece mcumax_get_piece(mcumax_square square);
mcumax_piece mcumax_get_current_side(void);
uint32_t mcumax_search_valid_moves(mcumax_move *buffer, uint32_t buffer_size);
mcumax_move mcumax_search_best_move(uint32_t node_max, uint32_t depth_max);
bool mcumax_play_move(mcumax_move move);
void mcumax_set_callback(mcumax_callback callback, void *userdata);
void mcumax_stop_search(void);

/* ==========================================================================
 * PART 2: ENGINE CORE IMPLEMENTATION
 * ========================================================================== */

#define MCUMAX_BOARD_MASK 0x88
#define MCUMAX_BOARD_WHITE 0x8
#define MCUMAX_BOARD_BLACK 0x10
#define MCUMAX_PIECE_MOVED 0x20
#define MCUMAX_SCORE_MAX 8000
#define MCUMAX_DEPTH_MAX 99

enum mcumax_mode
{
    MCUMAX_INTERNAL_NODE,
    MCUMAX_SEARCH_VALID_MOVES,
    MCUMAX_SEARCH_BEST_MOVE,
    MCUMAX_PLAY_MOVE,
};

struct
{
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

static const int8_t mcumax_capture_values[] = {
    0, 2, 2, 7, -1, 8, 12, 23};

static const int8_t mcumax_step_vectors_indices[] = {
    0, 7, -1, 11, 6, 8, 3, 6};

static const int8_t mcumax_step_vectors[] = {
    -16, -15, -17, 0,
    1, 16, 0,
    1, 16, 15, 17, 0,
    14, 18, 31, 33, 0};

static const int8_t mcumax_board_setup[] = {
    MCUMAX_ROOK,
    MCUMAX_KNIGHT,
    MCUMAX_BISHOP,
    MCUMAX_QUEEN,
    MCUMAX_KING,
    MCUMAX_BISHOP,
    MCUMAX_KNIGHT,
    MCUMAX_ROOK,
};

static clock_t search_start_time;
static uint32_t search_time_limit_ms;
static bool time_limit_enabled;

typedef bool (*mcumax_move_callback)(mcumax_move move);

static int32_t mcumax_search(int32_t alpha,
                             int32_t beta,
                             int32_t score,
                             uint8_t en_passant_square,
                             uint8_t depth,
                             enum mcumax_mode mode);

static int32_t mcumax_search(int32_t alpha,
                             int32_t beta,
                             int32_t score,
                             uint8_t en_passant_square,
                             uint8_t depth,
                             enum mcumax_mode mode)
{
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

    iter_depth =
        iter_score =
            iter_square_from =
                iter_square_to = 0;

    while ((iter_depth++ < depth) ||
           (iter_depth < 3) ||
           ((mode != MCUMAX_INTERNAL_NODE) &&
            (mcumax.square_from == MCUMAX_SQUARE_INVALID) &&
            (((mcumax.node_count < mcumax.node_max) &&
              (iter_depth <= mcumax.depth_max)) ||
             (mcumax.square_from = iter_square_from,
              mcumax.square_to = iter_square_to & ~MCUMAX_BOARD_MASK,
              iter_depth = 3))))
    {
        if (mcumax.stop_search)
            break;

        square_from =
            square_start = (mode != MCUMAX_SEARCH_VALID_MOVES)
                               ? iter_square_from
                               : 0;

        replay_move = iter_square_to & MCUMAX_SQUARE_INVALID;
        mcumax.current_side ^= 0x18;

        null_move_score = (iter_depth > 2) && (beta != -MCUMAX_SCORE_MAX)
                              ? mcumax_search(-beta,
                                              1 - beta,
                                              -score,
                                              MCUMAX_SQUARE_INVALID,
                                              iter_depth - 3,
                                              MCUMAX_INTERNAL_NODE)
                              : MCUMAX_SCORE_MAX;

        mcumax.current_side ^= 0x18;

        iter_score = (-null_move_score < beta) ||
                             (mcumax.non_pawn_material > 35)
                         ? (iter_depth - 2)
                               ? -MCUMAX_SCORE_MAX
                               : score
                         : -null_move_score;

        mcumax.node_count++;

        do
        {
            scan_piece = mcumax.board[square_from];

            if (scan_piece & mcumax.current_side)
            {
                step_vector = scan_piece_type = (scan_piece & 0b111);
                step_vector_index = mcumax_step_vectors_indices[scan_piece_type];

                while ((step_vector = ((scan_piece_type > 2) &&
                                       (step_vector < 0))
                                          ? -step_vector
                                          : -mcumax_step_vectors[++step_vector_index]))
                {
                replay:
                    square_to = square_from;
                    castling_skip_square =
                        castling_rook_square = MCUMAX_SQUARE_INVALID;

                    do
                    {
                        capture_square =
                            square_to =
                                replay_move
                                    ? (iter_square_to ^ replay_move)
                                    : (square_to + step_vector);

                        if (square_to & MCUMAX_BOARD_MASK)
                            break;

                        if ((en_passant_square != MCUMAX_SQUARE_INVALID) &&
                            mcumax.board[en_passant_square] &&
                            ((square_to - en_passant_square) < 2) &&
                            ((en_passant_square - square_to) < 2))
                            iter_score = MCUMAX_SCORE_MAX;

                        if ((scan_piece_type < 3) &&
                            (square_to == en_passant_square))
                            capture_square ^= 16;

                        capture_piece = mcumax.board[capture_square];

                        if ((capture_piece & mcumax.current_side) ||
                            ((scan_piece_type < 3) &&
                             !((square_to - square_from) & 0b111) - !capture_piece))
                            break;

                        capture_piece_value = 37 * mcumax_capture_values[capture_piece & 0b111] +
                                              (capture_piece & 0xc0);

                        if (capture_piece_value < 0)
                        {
                            iter_score = MCUMAX_SCORE_MAX;
                            iter_depth = MCUMAX_DEPTH_MAX - 1;
                        }

                        if ((iter_score >= beta) &&
                            (iter_depth > 1))
                            goto cutoff;

                        step_score = (iter_depth != 1)
                                         ? score
                                         : capture_piece_value - scan_piece_type;

                        if ((iter_depth - !capture_piece) > 1)
                        {
                            step_score = (scan_piece_type < 6)
                                             ? mcumax.board[square_from + 0x8] -
                                                   mcumax.board[square_to + 0x8]
                                             : 0;

                            mcumax.board[castling_rook_square] =
                                mcumax.board[capture_square] =
                                    mcumax.board[square_from] = 0;

                            mcumax.board[square_to] = scan_piece | MCUMAX_PIECE_MOVED;

                            if (!(castling_rook_square & MCUMAX_BOARD_MASK))
                            {
                                mcumax.board[castling_skip_square] = mcumax.current_side + 6;
                                step_score += 50;
                            }

                            step_score -= ((scan_piece_type != 4) ||
                                           (mcumax.non_pawn_material > 30))
                                              ? 0
                                              : 20;

                            if (scan_piece_type < 3)
                            {
                                step_score -=
                                    9 * ((((square_from - 2) & MCUMAX_BOARD_MASK) ||
                                          mcumax.board[square_from - 2] - scan_piece) +
                                         (((square_from + 2) & MCUMAX_BOARD_MASK) ||
                                          mcumax.board[square_from + 2] - scan_piece) -
                                         1 +
                                         (mcumax.board[square_from ^ 0x10] ==
                                          (mcumax.current_side + 36))) 
                                    - (mcumax.non_pawn_material >> 2); 

                                capture_piece_value +=
                                    step_alpha =
                                        (square_to + step_vector + 1) & MCUMAX_SQUARE_INVALID
                                            ? (647 - scan_piece_type)
                                            : 2 * (scan_piece & (square_to + 0x10) & 0x20);

                                mcumax.board[square_to] += step_alpha;
                            }

                            step_score += score + capture_piece_value;
                            step_alpha = iter_score > alpha
                                             ? iter_score
                                             : alpha;

                            step_depth = iter_depth - 1 -
                                         ((iter_depth > 5) &&
                                          (scan_piece_type > 2) &&
                                          !capture_piece &&
                                          !replay_move);

                            if (!((mcumax.non_pawn_material > 30) ||
                                  (null_move_score - MCUMAX_SCORE_MAX) ||
                                  (iter_depth < 3) ||
                                  (capture_piece &&
                                   (scan_piece_type != 4))))
                                step_depth = iter_depth;

                            do
                            {
                                mcumax.current_side ^= 0x18;

                                step_score_new = ((mode == MCUMAX_SEARCH_VALID_MOVES) ||
                                                  (step_depth > 2) ||
                                                  (step_score > step_alpha))
                                                     ? -mcumax_search(-beta,
                                                                      -step_alpha,
                                                                      -step_score,
                                                                      castling_skip_square,
                                                                      step_depth,
                                                                      MCUMAX_INTERNAL_NODE)
                                                     : step_score;

                                mcumax.current_side ^= 0x18;
                            } while ((step_score_new > alpha) &&
                                     (++step_depth < iter_depth));

                            step_score = step_score_new;

                            if ((mode == MCUMAX_PLAY_MOVE) &&
                                (step_score != -MCUMAX_SCORE_MAX) &&
                                (square_from == mcumax.square_from) &&
                                (square_to == mcumax.square_to))
                            {
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

                            if ((mode == MCUMAX_SEARCH_BEST_MOVE) &&
                                (step_score != -MCUMAX_SCORE_MAX) &&
                                (square_from == mcumax.square_from) &&
                                (square_to == mcumax.square_to))
                                return beta;

                            if ((mode == MCUMAX_SEARCH_VALID_MOVES) &&
                                (step_score != -MCUMAX_SCORE_MAX) &&
                                (mcumax.square_from == MCUMAX_SQUARE_INVALID) &&
                                (iter_depth == 3) &&
                                !replay_move)
                            {
                                mcumax_move move = {square_from, square_to};

                                if (mcumax.valid_moves_num < mcumax.valid_moves_buffer_size)
                                    mcumax.valid_moves_buffer[mcumax.valid_moves_num] = move;

                                mcumax.valid_moves_num++;
                            }
                        }

                        if (step_score > iter_score)
                        {
                            iter_score = step_score;
                            iter_square_from = square_from;
                            iter_square_to = square_to |
                                             (castling_skip_square & MCUMAX_SQUARE_INVALID);
                        }

                        if (replay_move)
                        {
                            replay_move = 0;
                            goto replay;
                        }

                        if ((square_from + step_vector - square_to) ||
                            (scan_piece & MCUMAX_PIECE_MOVED) ||
                            ((scan_piece_type > 2) &&
                             (((scan_piece_type != 4) ||
                               (step_vector_index != 7) ||
                               (mcumax.board[castling_rook_square =
                                                 ((square_from + 3) ^
                                                  ((step_vector >> 1) & 0b111))] -
                                mcumax.current_side - 6) ||
                               mcumax.board[castling_rook_square ^ 1] ||
                               mcumax.board[castling_rook_square ^ 2]))))
                            capture_piece += (scan_piece_type < 5);
                        else
                            castling_skip_square = square_to;

                    } while (!capture_piece);
                }
            }

        } while ((square_from = ((square_from + 9) &
                                 ~MCUMAX_BOARD_MASK)) != square_start);

    cutoff:
        if ((iter_score == -MCUMAX_SCORE_MAX) &&
            (null_move_score != MCUMAX_SCORE_MAX))
            iter_score = 0;

        if (mode == MCUMAX_SEARCH_BEST_MOVE)
        {
            clock_t current_clock = clock();
            double elapsed_sec = (double)(current_clock - search_start_time) / CLOCKS_PER_SEC;
            uint32_t elapsed_ms = (uint32_t)(elapsed_sec * 1000.0);
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

            if (is_promo) {
                sprintf(move_str, "%c%c%c%cq", f1, r1, f2, r2);
            } else {
                sprintf(move_str, "%c%c%c%c", f1, r1, f2, r2);
            }

            int current_depth = (iter_depth - 2 > 0) ? (iter_depth - 2) : 1;

            if (iter_score > MCUMAX_SCORE_MAX - 100) {
                int mate_in_plies = MCUMAX_SCORE_MAX - iter_score;
                int mate_in_moves = (mate_in_plies + 1) / 2;
                printf("info depth %d score mate %d time %u nodes %u pv %s\n",
                       current_depth, mate_in_moves, elapsed_ms, mcumax.node_count, move_str);
            } else if (iter_score < -MCUMAX_SCORE_MAX + 100) {
                int mate_in_plies = MCUMAX_SCORE_MAX + iter_score;
                int mate_in_moves = -(mate_in_plies + 1) / 2;
                printf("info depth %d score mate %d time %u nodes %u pv %s\n",
                       current_depth, mate_in_moves, elapsed_ms, mcumax.node_count, move_str);
            } else {
                int score_cp = (iter_score * 100) / 74;
                printf("info depth %d score cp %d time %u nodes %u pv %s\n",
                       current_depth, score_cp, elapsed_ms, mcumax.node_count, move_str);
            }
            fflush(stdout);
        }
    }

    return iter_score += iter_score < score;
}

void mcumax_init()
{
    for (uint32_t x = 0; x < 8; x++)
    {
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

static mcumax_square mcumax_set_piece(mcumax_square square, mcumax_piece piece)
{
    if (square & MCUMAX_BOARD_MASK)
        return square;

    mcumax.board[square] = piece ? (piece | MCUMAX_PIECE_MOVED) : piece;
    return square + 1;
}

mcumax_piece mcumax_get_piece(mcumax_square square)
{
    if (square & MCUMAX_BOARD_MASK)
        return MCUMAX_EMPTY;

    return (mcumax.board[square] & 0xf) ^ MCUMAX_BLACK;
}

void mcumax_set_fen_position(const char *fen_string)
{
    mcumax_init();

    uint32_t field_index = 0;
    uint32_t board_index = 0;

    char c;
    while ((c = *fen_string++))
    {
        if (c == ' ')
        {
            if (field_index < 4)
                field_index++;
            continue;
        }

        switch (field_index)
        {
        case 0:
            if (board_index < 0x80)
            {
                switch (c)
                {
                case '8':
                case '7':
                case '6':
                case '5':
                case '4':
                case '3':
                case '2':
                case '1':
                    for (int32_t i = 0; i < (c - '0'); i++)
                        board_index = mcumax_set_piece(board_index, MCUMAX_EMPTY);
                    break;

                case 'P':
                    board_index = mcumax_set_piece(board_index, MCUMAX_PAWN_UPSTREAM | MCUMAX_BOARD_WHITE);
                    break;
                case 'N':
                    board_index = mcumax_set_piece(board_index, MCUMAX_KNIGHT | MCUMAX_BOARD_WHITE);
                    break;
                case 'B':
                    board_index = mcumax_set_piece(board_index, MCUMAX_BISHOP | MCUMAX_BOARD_WHITE);
                    break;
                case 'R':
                    board_index = mcumax_set_piece(board_index, MCUMAX_ROOK | MCUMAX_BOARD_WHITE);
                    break;
                case 'Q':
                    board_index = mcumax_set_piece(board_index, MCUMAX_QUEEN | MCUMAX_BOARD_WHITE);
                    break;
                case 'K':
                    board_index = mcumax_set_piece(board_index, MCUMAX_KING | MCUMAX_BOARD_WHITE);
                    break;
                case 'p':
                    board_index = mcumax_set_piece(board_index, MCUMAX_PAWN_DOWNSTREAM | MCUMAX_BOARD_BLACK);
                    break;
                case 'n':
                    board_index = mcumax_set_piece(board_index, MCUMAX_KNIGHT | MCUMAX_BOARD_BLACK);
                    break;
                case 'b':
                    board_index = mcumax_set_piece(board_index, MCUMAX_BISHOP | MCUMAX_BOARD_BLACK);
                    break;
                case 'r':
                    board_index = mcumax_set_piece(board_index, MCUMAX_ROOK | MCUMAX_BOARD_BLACK);
                    break;
                case 'q':
                    board_index = mcumax_set_piece(board_index, MCUMAX_QUEEN | MCUMAX_BOARD_BLACK);
                    break;
                case 'k':
                    board_index = mcumax_set_piece(board_index, MCUMAX_KING | MCUMAX_BOARD_BLACK);
                    break;
                case '/':
                    board_index = (board_index < 0x80) ? (board_index & 0xf0) + 0x10 : board_index;
                    break;
                }
            }
            break;

        case 1:
            switch (c)
            {
            case 'w':
                mcumax.current_side = MCUMAX_BOARD_WHITE;
                break;
            case 'b':
                mcumax.current_side = MCUMAX_BOARD_BLACK;
                break;
            }
            break;

        case 2:
            switch (c)
            {
            case 'K':
                mcumax.board[0x74] &= ~MCUMAX_PIECE_MOVED;
                mcumax.board[0x77] &= ~MCUMAX_PIECE_MOVED;
                break;
            case 'Q':
                mcumax.board[0x74] &= ~MCUMAX_PIECE_MOVED;
                mcumax.board[0x70] &= ~MCUMAX_PIECE_MOVED;
                break;
            case 'k':
                mcumax.board[0x04] &= ~MCUMAX_PIECE_MOVED;
                mcumax.board[0x07] &= ~MCUMAX_PIECE_MOVED;
                break;
            case 'q':
                mcumax.board[0x04] &= ~MCUMAX_PIECE_MOVED;
                mcumax.board[0x00] &= ~MCUMAX_PIECE_MOVED;
                break;
            }
            break;

        case 3:
            switch (c)
            {
            case 'a': case 'b': case 'c': case 'd':
            case 'e': case 'f': case 'g': case 'h':
                mcumax.en_passant_square &= 0x7f;
                mcumax.en_passant_square |= (c - 'a');
                break;

            case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8':
                mcumax.en_passant_square &= 0x7f;
                mcumax.en_passant_square |= 16 * ('8' - c);
                break;
            }
            break;
        }
    }
}

mcumax_piece mcumax_get_current_side(void)
{
    return mcumax.current_side;
}

static int32_t mcumax_start_search(enum mcumax_mode mode,
                                   mcumax_move move,
                                   uint32_t depth_max,
                                   uint32_t node_max)
{
    mcumax.square_from = move.from;
    mcumax.square_to = move.to;

    mcumax.node_max = node_max;
    mcumax.node_count = 0;
    mcumax.depth_max = depth_max;
    mcumax.stop_search = false;

    return mcumax_search(-MCUMAX_SCORE_MAX,
                         MCUMAX_SCORE_MAX,
                         mcumax.score,
                         mcumax.en_passant_square,
                         3,
                         mode);
}

uint32_t mcumax_search_valid_moves(mcumax_move *valid_moves_buffer, uint32_t valid_moves_buffer_size)
{
    mcumax.valid_moves_num = 0;
    mcumax.valid_moves_buffer = valid_moves_buffer;
    mcumax.valid_moves_buffer_size = valid_moves_buffer_size;

    mcumax_start_search(MCUMAX_SEARCH_VALID_MOVES, MCUMAX_MOVE_INVALID, 0, 0);
    return mcumax.valid_moves_num;
}

mcumax_move mcumax_search_best_move(uint32_t node_max, uint32_t depth_max)
{
    int32_t score = mcumax_start_search(MCUMAX_SEARCH_BEST_MOVE,
                                        MCUMAX_MOVE_INVALID, depth_max + 3, node_max);

    if (score == MCUMAX_SCORE_MAX)
        return (mcumax_move){mcumax.square_from, mcumax.square_to};
    else
        return MCUMAX_MOVE_INVALID;
}

bool mcumax_play_move(mcumax_move move)
{
    return mcumax_start_search(MCUMAX_PLAY_MOVE, move, 0, 0) == MCUMAX_SCORE_MAX;
}

void mcumax_set_callback(mcumax_callback callback, void *userdata)
{
    mcumax.user_callback = callback;
    mcumax.user_data = userdata;
}

void mcumax_stop_search(void)
{
    mcumax.stop_search = true;
}

uint32_t mcumax_get_node_count(void)
{
    return mcumax.node_count;
}

/* ==========================================================================
 * PART 3: UCI CHESS INTERFACE EXAMPLE
 * ========================================================================== */

#define MAIN_VALID_MOVES_NUM 512

void dynamic_search_time_callback(void *userdata)
{
    if (time_limit_enabled)
    {
        clock_t current_clock = clock();
        double elapsed_ms = ((double)(current_clock - search_start_time) / CLOCKS_PER_SEC) * 1000.0;
        
        if (elapsed_ms >= (double)search_time_limit_ms)
        {
            mcumax_stop_search();
        }
    }
}

void print_board()
{
    const char *symbols = ".PPNKBRQ.ppnkbrq";
    printf("  +-----------------+\n");
    for (uint32_t y = 0; y < 8; y++)
    {
        printf("%d | ", 8 - y);
        for (uint32_t x = 0; x < 8; x++)
            printf("%c ", symbols[mcumax_get_piece(0x10 * y + x)]);
        printf("|\n");
    }
    printf("  +-----------------+\n");
    printf("    a b c d e f g h\n\n");
}

mcumax_square get_square(char *s)
{
    mcumax_square rank = s[0] - 'a';
    if (rank > 7)
        return MCUMAX_SQUARE_INVALID;

    mcumax_square file = '8' - s[1];
    if (file > 7)
        return MCUMAX_SQUARE_INVALID;

    return 0x10 * file + rank;
}

bool is_square_valid(char *s)
{
    return (get_square(s) != MCUMAX_SQUARE_INVALID);
}

bool is_move_valid(char *s)
{
    return is_square_valid(s) && is_square_valid(s + 2);
}

void print_square(mcumax_square square)
{
    printf("%c%c",
           'a' + ((square & 0x07) >> 0),
           '1' + 7 - ((square & 0x70) >> 4));
}

void print_move(mcumax_move move)
{
    if ((move.from == MCUMAX_SQUARE_INVALID) ||
        (move.to == MCUMAX_SQUARE_INVALID))
        printf("(none)");
    else
    {
        print_square(move.from);
        print_square(move.to);
    }
}

#ifdef __wii__
void write_move_to_file(mcumax_move move) {
    FILE *f = fopen("sd:/apps/wiichess/move.txt", "w");
    if (f) {
        if ((move.from != MCUMAX_SQUARE_INVALID) && (move.to != MCUMAX_SQUARE_INVALID)) {
            bool is_promo = false;
            uint8_t piece = mcumax.board[move.from] & 0x7;
            char r2 = '8' - ((move.to >> 4) & 0x7);
            if ((piece == MCUMAX_PAWN_UPSTREAM || piece == MCUMAX_PAWN_DOWNSTREAM) && (r2 == '8' || r2 == '1')) {
                is_promo = true;
            }
            
            fprintf(f, "%c%c%c%c",
                   'a' + ((move.from & 0x07) >> 0),
                   '1' + 7 - ((move.from & 0x70) >> 4),
                   'a' + ((move.to & 0x07) >> 0),
                   '1' + 7 - ((move.to & 0x70) >> 4));
            if (is_promo) {
                fprintf(f, "q");
            }
            fprintf(f, "\n");
        } else {
            fprintf(f, "0000\n");
        }
        fclose(f);
    }
}

// Relocatable high-RAM DOL Loader definitions inside File 2
typedef struct {
    uint32_t text_pos[7];
    uint32_t data_pos[11];
    uint32_t text_start[7];
    uint32_t data_start[11];
    uint32_t text_size[7];
    uint32_t data_size[11];
    uint32_t bss_start;
    uint32_t bss_size;
    uint32_t entry_point;
} dolheader;

typedef struct {
    uint32_t dest;
    void *src;
    uint32_t size;
} StagedBlock;

typedef struct {
    StagedBlock staged_text[7];
    StagedBlock staged_data[11];
    uint32_t bss_start;
    uint32_t bss_size;
    uint32_t entry_point;
} LoaderParams;

static void loader_entry(LoaderParams *params) {
    for (int i = 0; i < 7; i++) {
        if (params->staged_text[i].size > 0) {
            uint8_t *dst = (uint8_t *)params->staged_text[i].dest;
            uint8_t *src = (uint8_t *)params->staged_text[i].src;
            for (uint32_t j = 0; j < params->staged_text[i].size; j++) {
                dst[j] = src[j];
            }
        }
    }
    for (int i = 0; i < 11; i++) {
        if (params->staged_data[i].size > 0) {
            uint8_t *dst = (uint8_t *)params->staged_data[i].dest;
            uint8_t *src = (uint8_t *)params->staged_data[i].src;
            for (uint32_t j = 0; j < params->staged_data[i].size; j++) {
                dst[j] = src[j];
            }
        }
    }
    if (params->bss_size > 0) {
        uint8_t *dst = (uint8_t *)params->bss_start;
        for (uint32_t j = 0; j < params->bss_size; j++) {
            dst[j] = 0;
        }
    }

    for (int i = 0; i < 7; i++) {
        if (params->staged_text[i].size > 0) {
            uint32_t start = params->staged_text[i].dest & ~31;
            uint32_t end = (params->staged_text[i].dest + params->staged_text[i].size + 31) & ~31;
            for (uint32_t addr = start; addr < end; addr += 32) {
                __asm__ volatile("dcbst 0,%0 ; sync ; icbi 0,%0" : : "r"(addr));
            }
        }
    }
    for (int i = 0; i < 11; i++) {
        if (params->staged_data[i].size > 0) {
            uint32_t start = params->staged_data[i].dest & ~31;
            uint32_t end = (params->staged_data[i].dest + params->staged_data[i].size + 31) & ~31;
            for (uint32_t addr = start; addr < end; addr += 32) {
                __asm__ volatile("dcbst 0,%0 ; sync" : : "r"(addr));
            }
        }
    }
    __asm__ volatile("sync ; isync");

    typedef void (*entry_point)(void);
    entry_point start = (entry_point)params->entry_point;
    start();
}

static void loader_entry_end(void) {}

void run_dol(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    }

    dolheader header;
    fread(&header, 1, sizeof(dolheader), f);

    LoaderParams *params = malloc(sizeof(LoaderParams));
    memset(params, 0, sizeof(LoaderParams));
    params->bss_start = header.bss_start;
    params->bss_size = header.bss_size;
    params->entry_point = header.entry_point;

    for (int i = 0; i < 7; i++) {
        params->staged_text[i].size = header.text_size[i];
        if (params->staged_text[i].size > 0) {
            params->staged_text[i].dest = header.text_start[i];
            params->staged_text[i].src = malloc(params->staged_text[i].size);
            fseek(f, header.text_pos[i], SEEK_SET);
            fread(params->staged_text[i].src, 1, params->staged_text[i].size, f);
        }
    }

    for (int i = 0; i < 11; i++) {
        params->staged_data[i].size = header.data_size[i];
        if (params->staged_data[i].size > 0) {
            params->staged_data[i].dest = header.data_start[i];
            params->staged_data[i].src = malloc(params->staged_data[i].size);
            fseek(f, header.data_pos[i], SEEK_SET);
            fread(params->staged_data[i].src, 1, params->staged_data[i].size, f);
        }
    }
    fclose(f);

    WPAD_Shutdown();

    u32 loader_size = (uint32_t)loader_entry_end - (uint32_t)loader_entry;
    void *loader_target = (void *)0x81700000;
    memcpy(loader_target, (void *)loader_entry, loader_size);

    DCFlushRange(loader_target, loader_size);
    ICInvalidateRange(loader_target, loader_size);

    typedef void (*loader_fn)(LoaderParams *);
    loader_fn run_relocated = (loader_fn)loader_target;
    run_relocated(params);
}
#endif

bool send_uci_command(char *line)
{
    char *token = strtok(line, " \n");

    if (!token)
        return false;

    if (!strcmp(token, "uci"))
    {
        printf("id name " MCUMAX_ID "\n");
        printf("id author " MCUMAX_AUTHOR "\n");
        printf("uciok\n");
    }
    else if (!strcmp(token, "ucinewgame"))
        mcumax_init();
    else if (!strcmp(token, "isready"))
        printf("readyok\n");
    else if (!strcmp(token, "d"))
        print_board();
    else if (!strcmp(token, "l"))
    {
        mcumax_move valid_moves[MAIN_VALID_MOVES_NUM];
        uint32_t valid_moves_num = mcumax_search_valid_moves(valid_moves, MAIN_VALID_MOVES_NUM);

        for (uint32_t i = 0; i < valid_moves_num; i++)
        {
            print_move(valid_moves[i]);
            printf(" ");
        }
        printf("\n");
    }
    else if (!strcmp(token, "position"))
    {
        int fen_index = 0;
        char fen_string[256];

        while ((token = strtok(NULL, " \n")))
        {
            if (fen_index)
            {
                strcat(fen_string, token);
                strcat(fen_string, " ");

                fen_index++;
                if (fen_index > 6)
                {
                    mcumax_set_fen_position(fen_string);
                    fen_index = 0;
                }
            }
            else
            {
                if (!strcmp(token, "startpos"))
                    mcumax_init();
                else if (!strcmp(token, "fen"))
                {
                    fen_index = 1;
                    strcpy(fen_string, "");
                }
                else if (is_move_valid(token))
                {
                    mcumax_play_move((mcumax_move){
                        get_square(token + 0),
                        get_square(token + 2),
                    });
                }
            }
        }
    }
    else if (!strcmp(token, "go"))
    {
        uint32_t depth_max = 30;
        uint32_t node_max = 100000000;
        
        search_time_limit_ms = 0;
        time_limit_enabled = false;

        uint32_t wtime = 0, btime = 0, winc = 0, binc = 0;
        bool has_wtime = false, has_btime = false;

        while ((token = strtok(NULL, " \n")))
        {
            if (!strcmp(token, "infinite"))
            {
                depth_max = MCUMAX_DEPTH_MAX;
                node_max = 0xFFFFFFFF;
                time_limit_enabled = false;
            }
            else if (!strcmp(token, "depth"))
            {
                char *val = strtok(NULL, " \n");
                if (val) depth_max = atoi(val);
            }
            else if (!strcmp(token, "nodes"))
            {
                char *val = strtok(NULL, " \n");
                if (val) node_max = strtoul(val, NULL, 10);
            }
            else if (!strcmp(token, "movetime"))
            {
                char *val = strtok(NULL, " \n");
                if (val) {
                    search_time_limit_ms = atoi(val);
                    time_limit_enabled = true;
                }
            }
            else if (!strcmp(token, "wtime"))
            {
                char *val = strtok(NULL, " \n");
                if (val) {
                    wtime = atoi(val);
                    has_wtime = true;
                }
            }
            else if (!strcmp(token, "btime"))
            {
                char *val = strtok(NULL, " \n");
                if (val) {
                    btime = atoi(val);
                    has_btime = true;
                }
            }
            else if (!strcmp(token, "winc"))
            {
                char *val = strtok(NULL, " \n");
                if (val) winc = atoi(val);
            }
            else if (!strcmp(token, "binc"))
            {
                char *val = strtok(NULL, " \n");
                if (val) binc = atoi(val);
            }
        }

        if (!time_limit_enabled)
        {
            uint8_t active_side = mcumax_get_current_side();
            
            if (active_side == MCUMAX_BOARD_WHITE && has_wtime)
            {
                search_time_limit_ms = (wtime / 25) + (winc / 2);
                time_limit_enabled = true;
            }
            else if (active_side == MCUMAX_BOARD_BLACK && has_btime)
            {
                search_time_limit_ms = (btime / 25) + (binc / 2);
                time_limit_enabled = true;
            }
        }

        if (time_limit_enabled && (search_time_limit_ms < 20))
        {
            search_time_limit_ms = 20;
        }

        search_start_time = clock();
        mcumax_set_callback(dynamic_search_time_callback, NULL);

        mcumax_move move = mcumax_search_best_move(node_max, depth_max);

        clock_t end_time = clock();
        double elapsed_seconds = (double)(end_time - search_start_time) / CLOCKS_PER_SEC;

        if (elapsed_seconds < 0.001) {
            elapsed_seconds = 0.001; 
        }

        uint32_t nodes_searched = mcumax_get_node_count();
        uint32_t time_ms = (uint32_t)(elapsed_seconds * 1000.0);
        uint32_t nps = (uint32_t)((double)nodes_searched / elapsed_seconds);

        printf("info time %u nodes %u nps %u\n", time_ms, nodes_searched, nps);

#ifdef __wii__
        write_move_to_file(move);
#endif

        mcumax_play_move(move);

        printf("bestmove ");
        print_move(move);
        printf("\n");
    }
    else if (!strcmp(token, "quit"))
        return true;
    else
        printf("Unknown command: %s\n", token);

    return false;
}

int main()
{
#ifdef __wii__
    // Setup video outputs, FAT buffers and standard Wii resources
    VIDEO_Init();
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    if (!fatInitDefault()) {
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    }
#endif

    mcumax_init();

#ifdef __wii__
    FILE *f_uci = fopen("sd:/apps/wiichess/position.uci", "r");
    if (f_uci) {
        char line[1024];
        while (fgets(line, sizeof(line), f_uci)) {
            send_uci_command(line);
        }
        fclose(f_uci);
        remove("sd:/apps/wiichess/position.uci");

        printf("\nCalculation complete. Returning to GUI...\n");
        VIDEO_WaitVSync();
        
        run_dol("sd:/apps/wiichess/boot.dol");
    }
#endif

    // Interactive fallback loop for testing via terminal/PC
    while (true)
    {
        fflush(stdout);

        char line[65536];
        if (!fgets(line, sizeof(line), stdin))
            break;

        if (send_uci_command(line))
            break;
    }
    return 0;
}
