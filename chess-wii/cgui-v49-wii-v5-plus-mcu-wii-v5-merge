#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <ogc/lwp_watchdog.h>

#define MAX_HISTORY 2048

/* ==========================================================================
 * PART 1: STRUCTURES & GLOBAL STATES
 * ========================================================================== */

// Board representation (GUI)
typedef struct {
    int board[64]; // P=1, N=2, B=3, R=4, Q=5, K=6 (Positive=White, Negative=Black)
    int turn;      // 1 = White, -1 = Black
    int castle;    // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;        // En-passant square (0-63), -1 if none
    int halfmoves; // For 50-move rule
    int fullmoves;
} BoardState;

typedef struct {
    int from;
    int to;
    int promo; // 0=None, 2=N, 3=B, 4=R, 5=Q
} Move;

// Engine representation (mcu-max)
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

// Global GUI State
BoardState current_state;
BoardState history[MAX_HISTORY];
Move move_history[MAX_HISTORY];
int history_count = 0;

int cursor_r = 6;  // Screen row (0-7)
int cursor_c = 4;  // Screen col (0-7)
int selected_sq = -1;

int board_orientation = 1; // 1 = White on bottom, -1 = Black on bottom
int user_side = 1;         // 1 = White, -1 = Black, 0 = Hotseat, 2 = Watch (AI vs AI)

int time_control_type = 0;   // 0 = Time (ms), 1 = Depth, 2 = Nodes
int time_control_val = 1000; // Default: 1000 ms

// Wii Console Globals
static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

// Global Engine State variables
static uint64_t search_start_time_ms;
static uint32_t search_time_limit_ms;
static bool time_limit_enabled;

// Forward declarations (GUI)
void init_board(BoardState *state);
int is_legal_move(const BoardState *state, Move m);
int has_legal_moves(const BoardState *state);
int is_square_attacked(const BoardState *state, int sq, int attacker);
void make_move(const BoardState *src, BoardState *dst, Move m);
void print_side_panel_line(int panel_row);
void print_recent_moves(int row);
int find_king(const BoardState *state, int color);
int count_repetitions(const BoardState *state);
int get_promo_choice();
void save_state();
int load_state();
void trigger_engine_move();

// Forward declarations (Engine API)
void mcumax_init(void);
void mcumax_set_fen_position(const char *value);
mcumax_piece mcumax_get_current_side(void);
mcumax_move mcumax_search_best_move(uint32_t node_max, uint32_t depth_max);
bool mcumax_play_move(mcumax_move move);
void mcumax_set_callback(mcumax_callback callback, void *userdata);
void mcumax_stop_search(void);

/* ==========================================================================
 * PART 2: ENGINE CORE IMPLEMENTATION
 * ========================================================================== */

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

/* ==========================================================================
 * PART 3: IN-MEMORY SEAMLESS ENGINE CONNECTOR & GUI
 * ========================================================================== */

void dynamic_search_time_callback(void *userdata)
{
    if (time_limit_enabled)
    {
        uint64_t elapsed_ms = ticks_to_millisecs(gettime()) - search_start_time_ms;
        if (elapsed_ms >= search_time_limit_ms)
        {
            mcumax_stop_search();
        }
    }
}

// Convert a GUI BoardState into standard FEN string to parse directly into engine 
void board_to_fen(const BoardState *state, char *fen) {
    int pos = 0;
    for (int r = 0; r < 8; r++) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            int p = state->board[r * 8 + c];
            if (p == 0) {
                empty++;
            } else {
                if (empty > 0) {
                    pos += sprintf(fen + pos, "%d", empty);
                    empty = 0;
                }
                char c_char = ' ';
                switch (abs(p)) {
                    case 1: c_char = 'p'; break;
                    case 2: c_char = 'n'; break;
                    case 3: c_char = 'b'; break;
                    case 4: c_char = 'r'; break;
                    case 5: c_char = 'q'; break;
                    case 6: c_char = 'k'; break;
                }
                if (p > 0) {
                    c_char = c_char - 32; // Uppercase for White
                }
                fen[pos++] = c_char;
            }
        }
        if (empty > 0) {
            pos += sprintf(fen + pos, "%d", empty);
        }
        if (r < 7) {
            fen[pos++] = '/';
        }
    }
    
    // Active side
    pos += sprintf(fen + pos, " %c", (state->turn == 1) ? 'w' : 'b');
    
    // Castling privileges
    pos += sprintf(fen + pos, " ");
    int castle = state->castle;
    int has_castle = 0;
    if (castle & 1) { fen[pos++] = 'K'; has_castle = 1; }
    if (castle & 2) { fen[pos++] = 'Q'; has_castle = 1; }
    if (castle & 4) { fen[pos++] = 'k'; has_castle = 1; }
    if (castle & 8) { fen[pos++] = 'q'; has_castle = 1; }
    if (!has_castle) {
        fen[pos++] = '-';
    }
    
    // En-passant coordinates
    if (state->ep != -1) {
        int ep_col = state->ep % 8;
        int ep_row = 8 - (state->ep / 8);
        pos += sprintf(fen + pos, " %c%d", 'a' + ep_col, ep_row);
    } else {
        pos += sprintf(fen + pos, " -");
    }
    
    // Clock markers
    pos += sprintf(fen + pos, " %d %d", state->halfmoves, state->fullmoves);
    fen[pos] = '\0';
}

void trigger_engine_move() {
    // 1. Generate FEN representation from GUI state
    char fen[256];
    board_to_fen(&current_state, fen);
    
    // 2. Hydrate board details into the in-memory mcu-max engine representation
    mcumax_set_fen_position(fen);
    
    // 3. Configure limitations based on user interface selections
    uint32_t depth_max = 30;
    uint32_t node_max = 100000000;
    
    search_time_limit_ms = 0;
    time_limit_enabled = false;

    if (time_control_type == 0) { // Time (ms)
        search_time_limit_ms = time_control_val;
        time_limit_enabled = true;
    } else if (time_control_type == 1) { // Depth
        depth_max = time_control_val;
    } else if (time_control_type == 2) { // Nodes
        node_max = time_control_val;
    }

    search_start_time_ms = ticks_to_millisecs(gettime());
    mcumax_set_callback(dynamic_search_time_callback, NULL);

    // 4. Start search calculation in thread (zero display latency/reloads)
    mcumax_move best = mcumax_search_best_move(node_max, depth_max);

    // 5. If valid move computed, convert structure maps back and play in GUI
    if ((best.from != MCUMAX_SQUARE_INVALID) && (best.to != MCUMAX_SQUARE_INVALID)) {
        int from_sq = ((best.from >> 4) & 7) * 8 + (best.from & 7);
        int to_sq = ((best.to >> 4) & 7) * 8 + (best.to & 7);
        
        int p = current_state.board[from_sq];
        int is_promo = (abs(p) == 1 && (to_sq / 8 == 0 || to_sq / 8 == 7));
        
        Move m = { from_sq, to_sq, is_promo ? 5 : 0 };
        
        if (is_legal_move(&current_state, m)) {
            push_state(&current_state, m);
            BoardState next;
            make_move(&current_state, &next, m);
            current_state = next;
            save_state();
        }
    }
}

void init_wii_console() {
    VIDEO_Init();
    WPAD_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
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
    if (strlen(str) >= 5) {
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

void save_state() {
    FILE *f = fopen("sd:/apps/wiichess/state.bin", "wb");
    if (f) {
        fwrite(&current_state, sizeof(BoardState), 1, f);
        fwrite(&history_count, sizeof(int), 1, f);
        if (history_count > 0) {
            fwrite(history, sizeof(BoardState), history_count, f);
            fwrite(move_history, sizeof(Move), history_count, f);
        }
        fwrite(&board_orientation, sizeof(int), 1, f);
        fwrite(&user_side, sizeof(int), 1, f);
        fwrite(&time_control_type, sizeof(int), 1, f);
        fwrite(&time_control_val, sizeof(int), 1, f);
        fclose(f);
    }
}

int load_state() {
    FILE *f = fopen("sd:/apps/wiichess/state.bin", "rb");
    if (f) {
        fread(&current_state, sizeof(BoardState), 1, f);
        fread(&history_count, sizeof(int), 1, f);
        if (history_count > 0) {
            fread(history, sizeof(BoardState), history_count, f);
            fread(move_history, sizeof(Move), history_count, f);
        }
        fread(&board_orientation, sizeof(int), 1, f);
        fread(&user_side, sizeof(int), 1, f);
        fread(&time_control_type, sizeof(int), 1, f);
        fread(&time_control_val, sizeof(int), 1, f);
        fclose(f);
        return 1;
    }
    return 0;
}

// Rules engine validation systems
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

void draw_ui() {
    printf("\033[H\r\n"); 

    const char *turn_str = (current_state.turn == 1) ? "\033[1;33mWhite\033[0m" : "\033[1;35mBlack\033[0m";
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

    const char *types[] = {"Time-Limit", "Depth-Limit", "Node-Limit"};
    printf(" | %s", types[time_control_type]);
    if (time_control_type == 0) {
        printf(" (%d ms)", time_control_val);
    } else if (time_control_type == 1) {
        printf(" (depth %d)", time_control_val);
    } else {
        printf(" (nodes %d)", time_control_val);
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

            if (is_cursor) {
                bg_color = "\033[48;5;208m"; 
            } else if (is_selected) {
                bg_color = "\033[48;5;34m";  
            } else if (sq == king_in_check) {
                bg_color = "\033[48;5;196m"; 
            } else if (is_prev_move) {
                bg_color = is_light ? "\033[48;5;75m" : "\033[48;5;68m"; 
            } else if (is_legal_dest) {
                bg_color = is_light ? "\033[48;5;151m" : "\033[48;5;108m"; 
            } else {
                bg_color = is_light ? "\033[48;5;180m" : "\033[48;5;94m"; 
            }

            const char *piece_str = " ";
            const char *fg_color = "\033[38;5;232m"; 
            if (p != 0) {
                if (p > 0) fg_color = "\033[38;5;255m\033[1m"; 
                switch (abs(p)) {
                    case 1: piece_str = "P"; break;
                    case 2: piece_str = "N"; break;
                    case 3: piece_str = "B"; break;
                    case 4: piece_str = "R"; break;
                    case 5: piece_str = "Q"; break;
                    case 6: piece_str = "K"; break;
                }
            }

            printf("%s%s %s \033[0m", bg_color, fg_color, piece_str);
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

    printf(" \033[38;5;245m[D-Pad] Navigate | [A] Select | [B] Undo | [Home] Exit\033[0m\033[K\r\n");
    printf(" \033[38;5;245m[-] Flip Board   | [+] Switch Sides | [1] Limit Mode\033[0m\033[K\r\n");
    printf(" \033[38;5;245m[2] Adjust Val\033[0m\033[K\r\n\r\n");
    
    printf(" \033[38;5;248mExecution Mode:\033[0m Seamless In-Memory AI Solver Active");
    printf("\033[K\r\n\033[J");
    fflush(stdout);
}

void print_side_panel_line(int panel_row) {
    printf("   ");
    print_recent_moves(panel_row);
}

void print_recent_moves(int row) {
    int total_full_moves = (history_count + 1) / 2;
    if (total_full_moves == 0) {
        return; 
    }
    int start_move = 1;
    if (total_full_moves > 10) {
        start_move = total_full_moves - 9;
    }
    int display = start_move + row;
    if (display > total_full_moves) {
        return; 
    }

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

int get_promo_choice() {
    printf("\r\n \033[1;33mPromote: [A] Queen, [B] Rook, [-] Bishop, [+] Knight\033[0m");
    fflush(stdout);
    int choice = 5;
    while (1) {
        WPAD_ScanPads();
        u32 pressed = WPAD_ButtonsDown(0);
        if (pressed & WPAD_BUTTON_A) { choice = 5; break; }
        if (pressed & WPAD_BUTTON_B) { choice = 4; break; }
        if (pressed & WPAD_BUTTON_MINUS) { choice = 3; break; }
        if (pressed & WPAD_BUTTON_PLUS) { choice = 2; break; }
        VIDEO_WaitVSync();
    }
    printf("\r\033[K\033[A\033[K");
    fflush(stdout);
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
            save_state(); 
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
    int step_back = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step_back > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
        step_back--;
    }
    selected_sq = -1;
    save_state();
}

void handle_reset_board() {
    init_board(&current_state);
    history_count = 0;
    selected_sq = -1;
    cursor_r = 6;
    cursor_c = 4;
    save_state();
}

void handle_switch_sides() {
    if (user_side == 1) user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0) user_side = 2;
    else user_side = 1;
    save_state();
}

void adjust_time_control() {
    if (time_control_type == 0) { 
        int time_list[] = {50, 100, 250, 500, 1000, 1500, 2000, 3000, 5000, 10000};
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
        time_control_val = (time_control_val % 15) + 1; // Bound search depth between 1 and 15
    } else { 
        int nodes_list[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288};
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
    save_state();
}

void handle_input() {
    WPAD_ScanPads();
    u32 pressed = WPAD_ButtonsDown(0);

    if (pressed & WPAD_BUTTON_UP) {
        if (cursor_r > 0) cursor_r--;
    }
    if (pressed & WPAD_BUTTON_DOWN) {
        if (cursor_r < 7) cursor_r++;
    }
    if (pressed & WPAD_BUTTON_LEFT) {
        if (cursor_c > 0) cursor_c--;
    }
    if (pressed & WPAD_BUTTON_RIGHT) {
        if (cursor_c < 7) cursor_c++;
    }
    if (pressed & WPAD_BUTTON_A) {
        handle_select();
    }
    if (pressed & WPAD_BUTTON_B) {
        handle_undo();
    }
    if (pressed & WPAD_BUTTON_MINUS) {
        board_orientation = -board_orientation;
        save_state();
    }
    if (pressed & WPAD_BUTTON_PLUS) {
        handle_switch_sides();
    }
    if (pressed & WPAD_BUTTON_1) {
        time_control_type = (time_control_type + 1) % 3;
        time_control_val = (time_control_type == 0) ? 1000 : (time_control_type == 1 ? 5 : 4096);
        save_state();
    }
    if (pressed & WPAD_BUTTON_2) {
        adjust_time_control();
    }
    if (pressed & WPAD_BUTTON_HOME) {
        remove("sd:/apps/wiichess/state.bin");
        remove("sd:/apps/wiichess/position.uci");
        remove("sd:/apps/wiichess/move.txt");
        printf("\033[?25h\033[2J\033[H"); 
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
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

int main(int argc, char **argv) {
    init_wii_console();
    
    if (!fatInitDefault()) {
        printf("FAT Init Failed! Check SD Card access.\n");
        VIDEO_WaitVSync();
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    }

    mcumax_init();

    int has_previous_state = load_state();
    if (!has_previous_state) {
        init_board(&current_state);
        save_state();
    }

    // Process leftover move files if resuming from an older installation
    FILE *f_move = fopen("sd:/apps/wiichess/move.txt", "r");
    if (f_move) {
        char move_str[16];
        if (fscanf(f_move, "%15s", move_str) == 1) {
            Move m = uci_to_move(move_str);
            if (is_legal_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_move(&current_state, &next, m);
                current_state = next;
                save_state();
            }
        }
        fclose(f_move);
        remove("sd:/apps/wiichess/move.txt"); 
    }

    printf("\033[2J\033[H\033[?25l"); 

    while (1) {
        int engine_active = 0;
        if (has_legal_moves(&current_state) && current_state.halfmoves < 100 && count_repetitions(&current_state) < 3) {
            if (user_side == 2) engine_active = 1;
            else if (user_side == 1 && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn == 1) engine_active = 1;
        }

        if (engine_active) {
            draw_ui();
            printf("\n  \033[1;32mEngine Calculating...\033[0m\n");
            fflush(stdout);
            VIDEO_WaitVSync();
            trigger_engine_move(); 
        }

        draw_ui();
        handle_input();
        
        VIDEO_WaitVSync(); 
    }
    return 0;
}
