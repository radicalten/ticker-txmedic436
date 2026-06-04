/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 *
 * Compile: gcc -o chess_gui chess_gui.c -lncurses
 * Usage:   ./chess_gui [engine_path]
 *
 * Controls:
 *   Arrow keys     - Move cursor
 *   Enter/Space    - Select piece / confirm move
 *   U              - Undo last move (takeback)
 *   Q              - Quit
 *   F              - Flip board
 *   N              - New game
 *   E              - Toggle engine side (White/Black/Both/None)
 *
 * Requirements: ncurses library (brew install ncurses if needed)
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* ─────────────────────────────────────────────────────────────────────────────
   CONSTANTS & TYPES
───────────────────────────────────────────────────────────────────────────── */

#define MAX_MOVES       512
#define MAX_PGN_LEN     8192
#define MAX_HISTORY     256
#define ENGINE_BUF      4096
#define BOARD_X_OFFSET  4
#define BOARD_Y_OFFSET  2
#define CELL_W          5
#define CELL_H          3

/* Piece types */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

/* Colors */
#define WHITE_PIECE  1
#define BLACK_PIECE -1

/* Color pair IDs */
#define CP_LIGHT_SQ        1
#define CP_DARK_SQ         2
#define CP_LIGHT_SQ_SEL    3
#define CP_DARK_SQ_SEL     4
#define CP_LIGHT_SQ_MOVE   5
#define CP_DARK_SQ_MOVE    6
#define CP_LIGHT_SQ_HINT   7
#define CP_DARK_SQ_HINT    8
#define CP_WHITE_PIECE     9
#define CP_BLACK_PIECE    10
#define CP_STATUS_BAR     11
#define CP_PGN_TEXT       12
#define CP_BORDER         13
#define CP_CHECK_SQ       14
#define CP_LAST_MOVE_L    15
#define CP_LAST_MOVE_D    16

typedef struct {
    int piece;   /* piece type  */
    int color;   /* WHITE_PIECE or BLACK_PIECE */
} Square;

typedef struct {
    int from_file, from_rank;  /* 0-7 */
    int to_file,   to_rank;
    int piece;
    int color;
    int captured_piece;
    int captured_color;
    int promotion;             /* piece type if promotion */
    int is_castling;           /* 0=no, 1=kingside, -1=queenside */
    int is_en_passant;
    int gave_check;
    int gave_checkmate;
    /* castling rights before move */
    int prev_castle_wk, prev_castle_wq;
    int prev_castle_bk, prev_castle_bq;
    int prev_ep_file;          /* en passant file before move (-1 = none) */
    int prev_halfmove;
} Move;

typedef struct {
    Square board[8][8];       /* board[file][rank], rank 0 = rank 1 */
    int turn;                 /* WHITE_PIECE or BLACK_PIECE */
    int castle_wk, castle_wq;
    int castle_bk, castle_bq;
    int ep_file;              /* en passant target file, -1 if none */
    int ep_rank;
    int halfmove_clock;
    int fullmove_number;
    Move history[MAX_HISTORY];
    int history_count;
    int game_over;            /* 0=playing, 1=checkmate, 2=stalemate, 3=draw */
    int winner;               /* WHITE_PIECE or BLACK_PIECE or 0 */
} GameState;

typedef struct {
    pid_t pid;
    int   to_engine[2];       /* pipe: GUI writes, engine reads  */
    int   from_engine[2];     /* pipe: engine writes, GUI reads  */
    int   active;
    char  name[128];
    char  best_move[16];
    int   thinking;
    int   engine_white;
    int   engine_black;
} Engine;

/* ─────────────────────────────────────────────────────────────────────────────
   GLOBAL STATE
───────────────────────────────────────────────────────────────────────────── */

static GameState  g_state;
static Engine     g_engine;
static int        g_cursor_file = 4;
static int        g_cursor_rank = 0;
static int        g_sel_file    = -1;
static int        g_sel_rank    = -1;
static int        g_flipped     = 0;   /* 0 = white at bottom */
static char       g_pgn[MAX_PGN_LEN];
static char       g_status[256];
static int        g_legal_targets[64]; /* bitmask of legal target squares */
static int        g_legal_count  = 0;
static WINDOW    *g_win_board    = NULL;
static WINDOW    *g_win_pgn      = NULL;
static WINDOW    *g_win_status   = NULL;
static int        g_engine_side  = 0; /* 0=none,1=black,2=white,3=both */
static int        g_promotion_pending = 0;
static int        g_promo_file, g_promo_rank, g_promo_from_file, g_promo_from_rank;

/* ─────────────────────────────────────────────────────────────────────────────
   PIECE DISPLAY
───────────────────────────────────────────────────────────────────────────── */

/* Unicode chess pieces */
static const char *piece_unicode[7][2] = {
    /*         WHITE        BLACK  */
    /* EMPTY */  { " ",   " "   },
    /* PAWN  */  { "♙",  "♟"  },
    /* KNIGHT */ { "♘",  "♞"  },
    /* BISHOP */ { "♗",  "♝"  },
    /* ROOK  */  { "♖",  "♜"  },
    /* QUEEN */  { "♕",  "♛"  },
    /* KING  */  { "♔",  "♚"  },
};

/* ASCII fallback (for terminals without unicode) */
static const char piece_ascii[7][2] = {
    /* EMPTY */  {' ',' '},
    /* PAWN */   {'P','p'},
    /* KNIGHT */ {'N','n'},
    /* BISHOP */ {'B','b'},
    /* ROOK */   {'R','r'},
    /* QUEEN */  {'Q','q'},
    /* KING */   {'K','k'},
};

static int g_use_unicode = 1;

/* ─────────────────────────────────────────────────────────────────────────────
   FORWARD DECLARATIONS
───────────────────────────────────────────────────────────────────────────── */

static void       init_board(GameState *gs);
static void       draw_board(void);
static void       draw_pgn(void);
static void       draw_status(void);
static int        is_legal_move(GameState *gs, int ff, int fr, int tf, int tr, int promo);
static void       compute_legal_targets(GameState *gs, int file, int rank);
static int        apply_move(GameState *gs, int ff, int fr, int tf, int tr, int promo);
static int        is_in_check(GameState *gs, int color);
static void       undo_move(GameState *gs);
static void       update_pgn(void);
static void       engine_send(const char *msg);
static void       engine_go(void);
static void       engine_read_response(void);
static char      *move_to_uci(Move *m, char *buf);
static int        parse_uci_move(GameState *gs, const char *uci);
static void       game_state_to_fen(GameState *gs, char *fen);
static int        has_any_legal_moves(GameState *gs, int color);
static void       compute_pgn_move(GameState *before, Move *m, char *out);
static int        square_is_attacked(GameState *gs, int file, int rank, int by_color);

/* ─────────────────────────────────────────────────────────────────────────────
   BOARD INITIALIZATION
───────────────────────────────────────────────────────────────────────────── */

static void init_board(GameState *gs) {
    memset(gs, 0, sizeof(GameState));
    gs->ep_file = -1;
    gs->turn    = WHITE_PIECE;
    gs->fullmove_number = 1;
    gs->castle_wk = gs->castle_wq = gs->castle_bk = gs->castle_bq = 1;

    /* Pawns */
    for (int f = 0; f < 8; f++) {
        gs->board[f][1] = (Square){PAWN, WHITE_PIECE};
        gs->board[f][6] = (Square){PAWN, BLACK_PIECE};
    }
    /* Rooks */
    gs->board[0][0] = (Square){ROOK,   WHITE_PIECE};
    gs->board[7][0] = (Square){ROOK,   WHITE_PIECE};
    gs->board[0][7] = (Square){ROOK,   BLACK_PIECE};
    gs->board[7][7] = (Square){ROOK,   BLACK_PIECE};
    /* Knights */
    gs->board[1][0] = (Square){KNIGHT, WHITE_PIECE};
    gs->board[6][0] = (Square){KNIGHT, WHITE_PIECE};
    gs->board[1][7] = (Square){KNIGHT, BLACK_PIECE};
    gs->board[6][7] = (Square){KNIGHT, BLACK_PIECE};
    /* Bishops */
    gs->board[2][0] = (Square){BISHOP, WHITE_PIECE};
    gs->board[5][0] = (Square){BISHOP, WHITE_PIECE};
    gs->board[2][7] = (Square){BISHOP, BLACK_PIECE};
    gs->board[5][7] = (Square){BISHOP, BLACK_PIECE};
    /* Queens */
    gs->board[3][0] = (Square){QUEEN,  WHITE_PIECE};
    gs->board[3][7] = (Square){QUEEN,  BLACK_PIECE};
    /* Kings */
    gs->board[4][0] = (Square){KING,   WHITE_PIECE};
    gs->board[4][7] = (Square){KING,   BLACK_PIECE};
}

/* ─────────────────────────────────────────────────────────────────────────────
   MOVE GENERATION / VALIDATION
───────────────────────────────────────────────────────────────────────────── */

static int square_is_attacked(GameState *gs, int file, int rank, int by_color) {
    /* Pawns */
    int pdir = (by_color == WHITE_PIECE) ? 1 : -1;
    for (int df = -1; df <= 1; df += 2) {
        int sf = file + df, sr = rank - pdir;
        if (sf >= 0 && sf < 8 && sr >= 0 && sr < 8) {
            Square s = gs->board[sf][sr];
            if (s.piece == PAWN && s.color == by_color) return 1;
        }
    }
    /* Knights */
    static const int kd[8][2] = {
        {1,2},{2,1},{-1,2},{-2,1},{1,-2},{2,-1},{-1,-2},{-2,-1}
    };
    for (int i = 0; i < 8; i++) {
        int sf = file + kd[i][0], sr = rank + kd[i][1];
        if (sf >= 0 && sf < 8 && sr >= 0 && sr < 8) {
            Square s = gs->board[sf][sr];
            if (s.piece == KNIGHT && s.color == by_color) return 1;
        }
    }
    /* Sliding pieces */
    static const int dirs[8][2] = {
        {0,1},{0,-1},{1,0},{-1,0},{1,1},{1,-1},{-1,1},{-1,-1}
    };
    for (int d = 0; d < 8; d++) {
        int sf = file + dirs[d][0], sr = rank + dirs[d][1];
        while (sf >= 0 && sf < 8 && sr >= 0 && sr < 8) {
            Square s = gs->board[sf][sr];
            if (s.piece != EMPTY) {
                if (s.color == by_color) {
                    int is_diag = (d >= 4);
                    if (is_diag && (s.piece == BISHOP || s.piece == QUEEN)) return 1;
                    if (!is_diag && (s.piece == ROOK   || s.piece == QUEEN)) return 1;
                }
                break;
            }
            sf += dirs[d][0]; sr += dirs[d][1];
        }
    }
    /* King */
    for (int df = -1; df <= 1; df++) {
        for (int dr = -1; dr <= 1; dr++) {
            if (!df && !dr) continue;
            int sf = file + df, sr = rank + dr;
            if (sf >= 0 && sf < 8 && sr >= 0 && sr < 8) {
                Square s = gs->board[sf][sr];
                if (s.piece == KING && s.color == by_color) return 1;
            }
        }
    }
    return 0;
}

static int is_in_check(GameState *gs, int color) {
    /* find king */
    int kf = -1, kr = -1;
    for (int f = 0; f < 8 && kf == -1; f++)
        for (int r = 0; r < 8 && kf == -1; r++)
            if (gs->board[f][r].piece == KING && gs->board[f][r].color == color)
                { kf = f; kr = r; }
    if (kf == -1) return 0;
    return square_is_attacked(gs, kf, kr, -color);
}

/* Returns 1 if the pseudo-legal move leaves own king safe */
static int move_is_safe(GameState *gs, int ff, int fr, int tf, int tr, int promo) {
    GameState tmp = *gs;
    Square moving = tmp.board[ff][fr];

    /* En passant capture */
    if (moving.piece == PAWN && tf == tmp.ep_file && tr == tmp.ep_rank) {
        tmp.board[tf][fr] = (Square){EMPTY, 0};
    }
    /* Castling – move rook */
    if (moving.piece == KING && abs(tf - ff) == 2) {
        int rook_from = (tf > ff) ? 7 : 0;
        int rook_to   = (tf > ff) ? 5 : 3;
        tmp.board[rook_to][fr]   = tmp.board[rook_from][fr];
        tmp.board[rook_from][fr] = (Square){EMPTY, 0};
    }
    tmp.board[tf][tr] = moving;
    tmp.board[ff][fr] = (Square){EMPTY, 0};
    if (moving.piece == PAWN && (tr == 7 || tr == 0) && promo)
        tmp.board[tf][tr] = (Square){promo, moving.color};

    return !is_in_check(&tmp, moving.color);
}

/* Pseudo-legal move check */
static int is_pseudo_legal(GameState *gs, int ff, int fr, int tf, int tr, int promo) {
    if (ff < 0 || ff > 7 || fr < 0 || fr > 7) return 0;
    if (tf < 0 || tf > 7 || tr < 0 || tr > 7) return 0;
    if (ff == tf && fr == tr) return 0;

    Square src = gs->board[ff][fr];
    Square dst = gs->board[tf][tr];

    if (src.piece == EMPTY) return 0;
    if (src.color != gs->turn) return 0;
    if (dst.piece != EMPTY && dst.color == src.color) return 0;

    int df = tf - ff, dr = tr - fr;

    switch (src.piece) {
    case PAWN: {
        int dir = src.color; /* WHITE=+1, BLACK=-1 */
        int start_rank = (src.color == WHITE_PIECE) ? 1 : 6;
        /* Forward */
        if (df == 0 && dr == dir && dst.piece == EMPTY) {
            /* Promotion check */
            if ((tr == 7 || tr == 0) && promo == EMPTY) return 0;
            return 1;
        }
        /* Double push */
        if (df == 0 && dr == 2*dir && fr == start_rank &&
            dst.piece == EMPTY &&
            gs->board[ff][fr + dir].piece == EMPTY) return 1;
        /* Capture */
        if (abs(df) == 1 && dr == dir) {
            if (dst.piece != EMPTY && dst.color != src.color) {
                if ((tr == 7 || tr == 0) && promo == EMPTY) return 0;
                return 1;
            }
            /* En passant */
            if (tf == gs->ep_file && tr == gs->ep_rank) return 1;
        }
        return 0;
    }
    case KNIGHT:
        return (abs(df) == 2 && abs(dr) == 1) || (abs(df) == 1 && abs(dr) == 2);
    case BISHOP:
        if (abs(df) != abs(dr)) return 0;
        goto sliding;
    case ROOK:
        if (df != 0 && dr != 0) return 0;
        goto sliding;
    case QUEEN:
        if (abs(df) != abs(dr) && df != 0 && dr != 0) return 0;
    sliding: {
        int sf = (df > 0) - (df < 0);
        int sr = (dr > 0) - (dr < 0);
        int cf = ff + sf, cr = fr + sr;
        while (cf != tf || cr != tr) {
            if (gs->board[cf][cr].piece != EMPTY) return 0;
            cf += sf; cr += sr;
        }
        return 1;
    }
    case KING:
        /* Normal king move */
        if (abs(df) <= 1 && abs(dr) <= 1) return 1;
        /* Castling */
        if (abs(df) == 2 && dr == 0) {
            if (is_in_check(gs, src.color)) return 0;
            if (df == 2) { /* kingside */
                if (src.color == WHITE_PIECE && !gs->castle_wk) return 0;
                if (src.color == BLACK_PIECE && !gs->castle_bk) return 0;
                /* Check squares between king and rook are empty */
                if (gs->board[5][fr].piece != EMPTY) return 0;
                if (gs->board[6][fr].piece != EMPTY) return 0;
                /* King not passing through check */
                if (square_is_attacked(gs, 5, fr, -src.color)) return 0;
                if (square_is_attacked(gs, 6, fr, -src.color)) return 0;
            } else { /* queenside */
                if (src.color == WHITE_PIECE && !gs->castle_wq) return 0;
                if (src.color == BLACK_PIECE && !gs->castle_bq) return 0;
                if (gs->board[3][fr].piece != EMPTY) return 0;
                if (gs->board[2][fr].piece != EMPTY) return 0;
                if (gs->board[1][fr].piece != EMPTY) return 0;
                if (square_is_attacked(gs, 3, fr, -src.color)) return 0;
                if (square_is_attacked(gs, 2, fr, -src.color)) return 0;
            }
            return 1;
        }
        return 0;
    }
    return 0;
}

static int is_legal_move(GameState *gs, int ff, int fr, int tf, int tr, int promo) {
    if (!is_pseudo_legal(gs, ff, fr, tf, tr, promo)) return 0;
    return move_is_safe(gs, ff, fr, tf, tr, promo);
}

static int has_any_legal_moves(GameState *gs, int color) {
    int saved_turn = gs->turn;
    gs->turn = color;
    for (int ff = 0; ff < 8; ff++)
        for (int fr = 0; fr < 8; fr++) {
            if (gs->board[ff][fr].piece == EMPTY || gs->board[ff][fr].color != color) continue;
            for (int tf = 0; tf < 8; tf++)
                for (int tr = 0; tr < 8; tr++) {
                    int promo = (gs->board[ff][fr].piece == PAWN && (tr == 0 || tr == 7)) ? QUEEN : EMPTY;
                    if (is_legal_move(gs, ff, fr, tf, tr, promo)) {
                        gs->turn = saved_turn;
                        return 1;
                    }
                }
        }
    gs->turn = saved_turn;
    return 0;
}

static void compute_legal_targets(GameState *gs, int file, int rank) {
    memset(g_legal_targets, 0, sizeof(g_legal_targets));
    g_legal_count = 0;
    if (gs->board[file][rank].piece == EMPTY) return;
    if (gs->board[file][rank].color != gs->turn) return;
    for (int tf = 0; tf < 8; tf++)
        for (int tr = 0; tr < 8; tr++) {
            int promo = (gs->board[file][rank].piece == PAWN && (tr == 0 || tr == 7)) ? QUEEN : EMPTY;
            if (is_legal_move(gs, file, rank, tf, tr, promo)) {
                g_legal_targets[tr * 8 + tf] = 1;
                g_legal_count++;
            }
        }
}

/* ─────────────────────────────────────────────────────────────────────────────
   APPLY / UNDO MOVE
───────────────────────────────────────────────────────────────────────────── */

/* Generate PGN notation for a move (before applying it) */
static void compute_pgn_move(GameState *before, Move *m, char *out) {
    char buf[32] = {0};
    int idx = 0;

    if (m->is_castling == 1)  { strcpy(out, "O-O");   goto check_suffix; }
    if (m->is_castling == -1) { strcpy(out, "O-O-O"); goto check_suffix; }

    /* Piece letter */
    if (m->piece != PAWN) {
        buf[idx++] = piece_ascii[m->piece][0]; /* upper case */
    }

    /* Disambiguation */
    if (m->piece != PAWN) {
        int ambig_file = 0, ambig_rank = 0, ambig_count = 0;
        for (int ff2 = 0; ff2 < 8; ff2++)
            for (int fr2 = 0; fr2 < 8; fr2++) {
                if (ff2 == m->from_file && fr2 == m->from_rank) continue;
                Square s = before->board[ff2][fr2];
                if (s.piece == m->piece && s.color == m->color) {
                    int promo = EMPTY;
                    if (is_legal_move((GameState*)before, ff2, fr2, m->to_file, m->to_rank, promo)) {
                        ambig_count++;
                        if (ff2 == m->from_file) ambig_rank = 1;
                        else                     ambig_file = 1;
                    }
                }
            }
        if (ambig_count > 0) {
            if (!ambig_file)       buf[idx++] = 'a' + m->from_file;
            else if (!ambig_rank)  buf[idx++] = '1' + m->from_rank;
            else { buf[idx++] = 'a' + m->from_file; buf[idx++] = '1' + m->from_rank; }
        }
    }

    /* Capture */
    if (m->captured_piece != EMPTY || m->is_en_passant) {
        if (m->piece == PAWN) buf[idx++] = 'a' + m->from_file;
        buf[idx++] = 'x';
    }

    /* Destination */
    buf[idx++] = 'a' + m->to_file;
    buf[idx++] = '1' + m->to_rank;

    /* Promotion */
    if (m->promotion) {
        buf[idx++] = '=';
        buf[idx++] = piece_ascii[m->promotion][0];
    }

    buf[idx] = '\0';
    strcpy(out, buf);

check_suffix:
    if (m->gave_checkmate) strcat(out, "#");
    else if (m->gave_check) strcat(out, "+");
}

static int apply_move(GameState *gs, int ff, int fr, int tf, int tr, int promo) {
    if (!is_legal_move(gs, ff, fr, tf, tr, promo)) return 0;

    Move *m = &gs->history[gs->history_count];
    memset(m, 0, sizeof(Move));
    m->from_file = ff; m->from_rank = fr;
    m->to_file   = tf; m->to_rank   = tr;
    m->piece     = gs->board[ff][fr].piece;
    m->color     = gs->board[ff][fr].color;
    m->promotion = promo;
    m->captured_piece = gs->board[tf][tr].piece;
    m->captured_color = gs->board[tf][tr].color;
    m->prev_castle_wk = gs->castle_wk;
    m->prev_castle_wq = gs->castle_wq;
    m->prev_castle_bk = gs->castle_bk;
    m->prev_castle_bq = gs->castle_bq;
    m->prev_ep_file   = gs->ep_file;
    m->prev_halfmove  = gs->halfmove_clock;

    /* Castling */
    if (m->piece == KING && abs(tf - ff) == 2) {
        m->is_castling = (tf > ff) ? 1 : -1;
        int rook_from = (tf > ff) ? 7 : 0;
        int rook_to   = (tf > ff) ? 5 : 3;
        gs->board[rook_to][fr]   = gs->board[rook_from][fr];
        gs->board[rook_from][fr] = (Square){EMPTY, 0};
    }

    /* En passant capture */
    if (m->piece == PAWN && tf == gs->ep_file && tr == gs->ep_rank) {
        m->is_en_passant = 1;
        m->captured_piece = PAWN;
        m->captured_color = -m->color;
        gs->board[tf][fr] = (Square){EMPTY, 0};
    }

    /* Apply move */
    gs->board[tf][tr] = gs->board[ff][fr];
    gs->board[ff][fr] = (Square){EMPTY, 0};

    /* Promotion */
    if (m->piece == PAWN && (tr == 7 || tr == 0) && promo) {
        gs->board[tf][tr] = (Square){promo, m->color};
        m->promotion = promo;
    }

    /* Update castling rights */
    if (m->piece == KING) {
        if (m->color == WHITE_PIECE) { gs->castle_wk = gs->castle_wq = 0; }
        else                         { gs->castle_bk = gs->castle_bq = 0; }
    }
    if (m->piece == ROOK || m->captured_piece == ROOK) {
        if (ff == 0 && fr == 0) gs->castle_wq = 0;
        if (ff == 7 && fr == 0) gs->castle_wk = 0;
        if (ff == 0 && fr == 7) gs->castle_bq = 0;
        if (ff == 7 && fr == 7) gs->castle_bk = 0;
        if (tf == 0 && tr == 0) gs->castle_wq = 0;
        if (tf == 7 && tr == 0) gs->castle_wk = 0;
        if (tf == 0 && tr == 7) gs->castle_bq = 0;
        if (tf == 7 && tr == 7) gs->castle_bk = 0;
    }

    /* En passant target */
    gs->ep_file = -1; gs->ep_rank = -1;
    if (m->piece == PAWN && abs(tr - fr) == 2) {
        gs->ep_file = ff;
        gs->ep_rank = (fr + tr) / 2;
    }

    /* Half-move clock */
    if (m->piece == PAWN || m->captured_piece != EMPTY)
        gs->halfmove_clock = 0;
    else
        gs->halfmove_clock++;

    /* Switch turn */
    gs->turn = -gs->turn;
    if (gs->turn == WHITE_PIECE) gs->fullmove_number++;

    /* Check / checkmate detection */
    int opponent = -m->color;
    m->gave_check = is_in_check(gs, opponent);
    int opp_has_moves = has_any_legal_moves(gs, opponent);
    if (!opp_has_moves) {
        if (m->gave_check) {
            m->gave_checkmate = 1;
            gs->game_over = 1;
            gs->winner    = m->color;
        } else {
            gs->game_over = 2; /* stalemate */
        }
    }
    /* 50-move rule */
    if (gs->halfmove_clock >= 100) gs->game_over = 3;

    gs->history_count++;
    return 1;
}

static void undo_move(GameState *gs) {
    if (gs->history_count == 0) return;
    gs->game_over = 0;
    gs->winner    = 0;

    Move *m = &gs->history[gs->history_count - 1];
    gs->history_count--;

    /* Restore turn */
    gs->turn = m->color;
    if (m->color == BLACK_PIECE) gs->fullmove_number--;

    /* Move piece back */
    gs->board[m->from_file][m->from_rank] = (Square){m->piece, m->color};
    gs->board[m->to_file][m->to_rank]     = (Square){EMPTY, 0};

    /* Restore captured piece */
    if (m->captured_piece != EMPTY && !m->is_en_passant)
        gs->board[m->to_file][m->to_rank] = (Square){m->captured_piece, m->captured_color};

    /* En passant restore */
    if (m->is_en_passant)
        gs->board[m->to_file][m->from_rank] = (Square){PAWN, m->captured_color};

    /* Castling: restore rook */
    if (m->is_castling == 1) {
        gs->board[7][m->from_rank] = (Square){ROOK, m->color};
        gs->board[5][m->from_rank] = (Square){EMPTY, 0};
    } else if (m->is_castling == -1) {
        gs->board[0][m->from_rank] = (Square){ROOK, m->color};
        gs->board[3][m->from_rank] = (Square){EMPTY, 0};
    }

    /* Restore state */
    gs->castle_wk    = m->prev_castle_wk;
    gs->castle_wq    = m->prev_castle_wq;
    gs->castle_bk    = m->prev_castle_bk;
    gs->castle_bq    = m->prev_castle_bq;
    gs->ep_file      = m->prev_ep_file;
    gs->ep_rank      = (m->prev_ep_file >= 0) ?
                       (m->color == WHITE_PIECE ? 2 : 5) : -1;
    gs->halfmove_clock = m->prev_halfmove;
}

/* ─────────────────────────────────────────────────────────────────────────────
   FEN GENERATION
───────────────────────────────────────────────────────────────────────────── */

static void game_state_to_fen(GameState *gs, char *fen) {
    int idx = 0;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            Square s = gs->board[f][r];
            if (s.piece == EMPTY) {
                empty++;
            } else {
                if (empty) { fen[idx++] = '0' + empty; empty = 0; }
                char c = piece_ascii[s.piece][0];
                if (s.color == BLACK_PIECE) c = tolower(c);
                fen[idx++] = c;
            }
        }
        if (empty) fen[idx++] = '0' + empty;
        if (r > 0) fen[idx++] = '/';
    }
    fen[idx++] = ' ';
    fen[idx++] = (gs->turn == WHITE_PIECE) ? 'w' : 'b';
    fen[idx++] = ' ';

    int any_castle = 0;
    if (gs->castle_wk) { fen[idx++] = 'K'; any_castle = 1; }
    if (gs->castle_wq) { fen[idx++] = 'Q'; any_castle = 1; }
    if (gs->castle_bk) { fen[idx++] = 'k'; any_castle = 1; }
    if (gs->castle_bq) { fen[idx++] = 'q'; any_castle = 1; }
    if (!any_castle)   { fen[idx++] = '-'; }
    fen[idx++] = ' ';

    if (gs->ep_file >= 0 && gs->ep_rank >= 0) {
        fen[idx++] = 'a' + gs->ep_file;
        fen[idx++] = '1' + gs->ep_rank;
    } else {
        fen[idx++] = '-';
    }

    idx += sprintf(fen + idx, " %d %d",
                   gs->halfmove_clock, gs->fullmove_number);
    fen[idx] = '\0';
}

/* ─────────────────────────────────────────────────────────────────────────────
   PGN BUILDING
───────────────────────────────────────────────────────────────────────────── */

static void update_pgn(void) {
    memset(g_pgn, 0, sizeof(g_pgn));
    /* Replay all moves from scratch to get correct annotations */
    GameState tmp;
    init_board(&tmp);
    int idx = 0;

    for (int i = 0; i < g_state.history_count; i++) {
        Move *m = &g_state.history[i];

        if (tmp.turn == WHITE_PIECE) {
            idx += snprintf(g_pgn + idx, MAX_PGN_LEN - idx,
                            "%d. ", tmp.fullmove_number);
        }

        char san[32];
        compute_pgn_move(&tmp, m, san);
        idx += snprintf(g_pgn + idx, MAX_PGN_LEN - idx, "%s ", san);

        /* Replay the move on tmp */
        apply_move(&tmp, m->from_file, m->from_rank,
                        m->to_file,   m->to_rank, m->promotion);
    }

    /* Result */
    if (g_state.game_over == 1) {
        if (g_state.winner == WHITE_PIECE) strcat(g_pgn, "1-0");
        else                               strcat(g_pgn, "0-1");
    } else if (g_state.game_over == 2 || g_state.game_over == 3) {
        strcat(g_pgn, "1/2-1/2");
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
   ENGINE COMMUNICATION
───────────────────────────────────────────────────────────────────────────── */

static void engine_send(const char *msg) {
    if (!g_engine.active) return;
    write(g_engine.to_engine[1], msg, strlen(msg));
    write(g_engine.to_engine[1], "\n", 1);
}

static char *move_to_uci(Move *m, char *buf) {
    buf[0] = 'a' + m->from_file;
    buf[1] = '1' + m->from_rank;
    buf[2] = 'a' + m->to_file;
    buf[3] = '1' + m->to_rank;
    buf[4] = '\0';
    if (m->promotion) {
        buf[4] = tolower(piece_ascii[m->promotion][0]);
        buf[5] = '\0';
    }
    return buf;
}

static int parse_uci_move(GameState *gs, const char *uci) {
    if (strlen(uci) < 4) return 0;
    int ff = uci[0] - 'a';
    int fr = uci[1] - '1';
    int tf = uci[2] - 'a';
    int tr = uci[3] - '1';
    int promo = EMPTY;
    if (strlen(uci) >= 5) {
        switch (tolower(uci[4])) {
            case 'q': promo = QUEEN;  break;
            case 'r': promo = ROOK;   break;
            case 'b': promo = BISHOP; break;
            case 'n': promo = KNIGHT; break;
        }
    }
    return apply_move(gs, ff, fr, tf, tr, promo);
}

static void engine_go(void) {
    if (!g_engine.active) return;
    char fen[256];
    game_state_to_fen(&g_state, fen);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "position fen %s", fen);
    engine_send(cmd);
    engine_send("go movetime 1000");
    g_engine.thinking = 1;
    g_engine.best_move[0] = '\0';
}

static void engine_read_response(void) {
    if (!g_engine.active || !g_engine.thinking) return;

    char buf[ENGINE_BUF];
    ssize_t n = read(g_engine.from_engine[0], buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Parse lines */
    char *line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "bestmove", 8) == 0) {
            char *tok = strtok(line + 9, " \r\n");
            if (tok && strcmp(tok, "(none)") != 0) {
                strncpy(g_engine.best_move, tok, 15);
                g_engine.thinking = 0;
            }
        }
        line = strtok(NULL, "\n");
    }
}

static int start_engine(const char *path) {
    if (pipe(g_engine.to_engine)   < 0) return 0;
    if (pipe(g_engine.from_engine) < 0) return 0;

    g_engine.pid = fork();
    if (g_engine.pid < 0) return 0;

    if (g_engine.pid == 0) {
        /* Child: engine process */
        dup2(g_engine.to_engine[0],   STDIN_FILENO);
        dup2(g_engine.from_engine[1], STDOUT_FILENO);
        close(g_engine.to_engine[1]);
        close(g_engine.from_engine[0]);
        /* Suppress stderr */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        execlp(path, path, NULL);
        exit(1);
    }
    /* Parent */
    close(g_engine.to_engine[0]);
    close(g_engine.from_engine[1]);

    /* Set non-blocking reads */
    fcntl(g_engine.from_engine[0], F_SETFL, O_NONBLOCK);

    /* Send UCI init */
    sleep(0); /* give engine a moment */
    engine_send("uci");
    engine_send("isready");

    /* Read until readyok */
    char buf[ENGINE_BUF];
    int ready = 0;
    time_t start = time(NULL);
    while (!ready && time(NULL) - start < 5) {
        ssize_t n = read(g_engine.from_engine[0], buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "readyok")) ready = 1;
            if (strstr(buf, "uciok")) {
                /* also grab name */
                char *p = strstr(buf, "id name ");
                if (p) {
                    p += 8;
                    char *end = strpbrk(p, "\r\n");
                    if (end) *end = '\0';
                    strncpy(g_engine.name, p, 127);
                }
            }
        }
        usleep(50000);
    }

    g_engine.active = ready;
    return ready;
}

static void stop_engine(void) {
    if (!g_engine.active) return;
    engine_send("quit");
    usleep(200000);
    kill(g_engine.pid, SIGTERM);
    waitpid(g_engine.pid, NULL, WNOHANG);
    close(g_engine.to_engine[1]);
    close(g_engine.from_engine[0]);
    g_engine.active = 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
   NCURSES DRAWING
───────────────────────────────────────────────────────────────────────────── */

static void init_colors(void) {
    start_color();
    use_default_colors();

    /* Try to use 256-color palette if available */
    int has256 = (COLORS >= 256);

    /* Light square */
    int light_bg   = has256 ? 229 : COLOR_WHITE;
    int dark_bg    = has256 ? 94  : COLOR_RED;
    int sel_bg     = has256 ? 226 : COLOR_YELLOW;
    int move_bg    = has256 ? 118 : COLOR_GREEN;
    int hint_bg    = has256 ? 33  : COLOR_CYAN;
    int last_l_bg  = has256 ? 192 : COLOR_GREEN;
    int last_d_bg  = has256 ? 64  : COLOR_GREEN;
    int check_bg   = has256 ? 196 : COLOR_RED;
    int wp_fg      = has256 ? 15  : COLOR_WHITE;
    int bp_fg      = has256 ? 232 : COLOR_BLACK;

    init_pair(CP_LIGHT_SQ,     COLOR_BLACK, light_bg);
    init_pair(CP_DARK_SQ,      wp_fg,       dark_bg);
    init_pair(CP_LIGHT_SQ_SEL, COLOR_BLACK, sel_bg);
    init_pair(CP_DARK_SQ_SEL,  COLOR_BLACK, sel_bg);
    init_pair(CP_LIGHT_SQ_MOVE,COLOR_BLACK, move_bg);
    init_pair(CP_DARK_SQ_MOVE, COLOR_BLACK, move_bg);
    init_pair(CP_LIGHT_SQ_HINT,COLOR_BLACK, hint_bg);
    init_pair(CP_DARK_SQ_HINT, COLOR_BLACK, hint_bg);
    init_pair(CP_WHITE_PIECE,  wp_fg,       light_bg);
    init_pair(CP_BLACK_PIECE,  bp_fg,       dark_bg);
    init_pair(CP_STATUS_BAR,   COLOR_BLACK, COLOR_CYAN);
    init_pair(CP_PGN_TEXT,     COLOR_WHITE, -1);
    init_pair(CP_BORDER,       COLOR_CYAN,  -1);
    init_pair(CP_CHECK_SQ,     COLOR_WHITE, check_bg);
    init_pair(CP_LAST_MOVE_L,  COLOR_BLACK, last_l_bg);
    init_pair(CP_LAST_MOVE_D,  COLOR_BLACK, last_d_bg);
}

static int screen_to_board_file(int scr_col) {
    /* Returns -1 if outside board */
    int rel = scr_col - BOARD_X_OFFSET;
    if (rel < 0 || rel >= 8 * CELL_W) return -1;
    int f = rel / CELL_W;
    return g_flipped ? 7 - f : f;
}

static int screen_to_board_rank(int scr_row) {
    int rel = scr_row - BOARD_Y_OFFSET;
    if (rel < 0 || rel >= 8 * CELL_H) return -1;
    int r = rel / CELL_H;
    return g_flipped ? r : 7 - r;
}

static void draw_board(void) {
    if (!g_win_board) return;
    werase(g_win_board);

    /* Find king in check */
    int check_kf = -1, check_kr = -1;
    if (is_in_check(&g_state, g_state.turn)) {
        for (int f = 0; f < 8; f++)
            for (int r = 0; r < 8; r++)
                if (g_state.board[f][r].piece == KING &&
                    g_state.board[f][r].color == g_state.turn)
                    { check_kf = f; check_kr = r; }
    }

    /* Last move squares */
    int lm_ff = -1, lm_fr = -1, lm_tf = -1, lm_tr = -1;
    if (g_state.history_count > 0) {
        Move *lm = &g_state.history[g_state.history_count - 1];
        lm_ff = lm->from_file; lm_fr = lm->from_rank;
        lm_tf = lm->to_file;   lm_tr = lm->to_rank;
    }

    /* Draw border with coordinates */
    wattron(g_win_board, COLOR_PAIR(CP_BORDER) | A_BOLD);
    /* Rank labels (left) */
    for (int r = 0; r < 8; r++) {
        int disp_r = g_flipped ? r : 7 - r;
        int y = BOARD_Y_OFFSET + r * CELL_H + CELL_H / 2;
        mvwprintw(g_win_board, y, 1, "%d", disp_r + 1);
    }
    /* File labels (bottom) */
    for (int f = 0; f < 8; f++) {
        int disp_f = g_flipped ? 7 - f : f;
        int x = BOARD_X_OFFSET + f * CELL_W + CELL_W / 2;
        mvwprintw(g_win_board, BOARD_Y_OFFSET + 8 * CELL_H, x,
                  "%c", 'a' + disp_f);
    }
    wattroff(g_win_board, COLOR_PAIR(CP_BORDER) | A_BOLD);

    /* Draw squares */
    for (int sr = 0; sr < 8; sr++) {        /* screen row */
        for (int sf = 0; sf < 8; sf++) {    /* screen file */
            int file = g_flipped ? 7 - sf : sf;
            int rank = g_flipped ? sr : 7 - sr;

            int light = ((file + rank) % 2 == 0) ? 0 : 1;
            /* 0 = dark square, 1 = light square (a1 is dark) */
            /* Actually a1 (file=0,rank=0): (0+0)%2=0 → dark */

            int cp;
            int is_selected   = (g_sel_file == file && g_sel_rank == rank);
            int is_hint       = (g_sel_file >= 0 && g_legal_targets[rank*8+file]);
            int is_cursor     = (g_cursor_file == file && g_cursor_rank == rank);
            int is_check      = (file == check_kf && rank == check_kr);
            int is_last_move  = (file == lm_ff && rank == lm_fr) ||
                                (file == lm_tf && rank == lm_tr);

            if (is_check)          cp = CP_CHECK_SQ;
            else if (is_selected)  cp = light ? CP_LIGHT_SQ_SEL  : CP_DARK_SQ_SEL;
            else if (is_hint)      cp = light ? CP_LIGHT_SQ_HINT : CP_DARK_SQ_HINT;
            else if (is_last_move) cp = light ? CP_LAST_MOVE_L   : CP_LAST_MOVE_D;
            else                   cp = light ? CP_LIGHT_SQ      : CP_DARK_SQ;

            int sy = BOARD_Y_OFFSET + sr * CELL_H;
            int sx = BOARD_X_OFFSET + sf * CELL_W;

            /* Fill the cell */
            wattron(g_win_board, COLOR_PAIR(cp));
            for (int cy = 0; cy < CELL_H; cy++) {
                wmove(g_win_board, sy + cy, sx);
                for (int cx = 0; cx < CELL_W; cx++)
                    waddch(g_win_board, ' ');
            }

            /* Draw piece in center */
            Square sq = g_state.board[file][rank];
            if (sq.piece != EMPTY) {
                wmove(g_win_board, sy + CELL_H / 2, sx + CELL_W / 2);
                wattron(g_win_board, A_BOLD);
                if (g_use_unicode) {
                    waddstr(g_win_board,
                            piece_unicode[sq.piece][sq.color == BLACK_PIECE ? 1 : 0]);
                } else {
                    waddch(g_win_board,
                           piece_ascii[sq.piece][sq.color == BLACK_PIECE ? 1 : 0]);
                }
                wattroff(g_win_board, A_BOLD);
            }

            wattroff(g_win_board, COLOR_PAIR(cp));

            /* Cursor outline */
            if (is_cursor) {
                wattron(g_win_board, A_REVERSE | A_BOLD);
                wmove(g_win_board, sy, sx);
                waddch(g_win_board, '+');
                wmove(g_win_board, sy, sx + CELL_W - 1);
                waddch(g_win_board, '+');
                wmove(g_win_board, sy + CELL_H - 1, sx);
                waddch(g_win_board, '+');
                wmove(g_win_board, sy + CELL_H - 1, sx + CELL_W - 1);
                waddch(g_win_board, '+');
                wattroff(g_win_board, A_REVERSE | A_BOLD);
            }
        }
    }

    /* Material count */
    int white_mat = 0, black_mat = 0;
    static const int mat_val[7] = {0, 1, 3, 3, 5, 9, 0};
    for (int f = 0; f < 8; f++)
        for (int r = 0; r < 8; r++) {
            Square s = g_state.board[f][r];
            if (s.piece) {
                if (s.color == WHITE_PIECE) white_mat += mat_val[s.piece];
                else                        black_mat  += mat_val[s.piece];
            }
        }

    int adv = white_mat - black_mat;
    int bx = BOARD_X_OFFSET + 8 * CELL_W + 2;
    wattron(g_win_board, COLOR_PAIR(CP_BORDER) | A_BOLD);
    mvwprintw(g_win_board, BOARD_Y_OFFSET,
              bx, "Material");
    wattroff(g_win_board, A_BOLD);
    mvwprintw(g_win_board, BOARD_Y_OFFSET + 1, bx, "W: %+d", adv);
    wattroff(g_win_board, COLOR_PAIR(CP_BORDER));

    /* Captured pieces display */
    /* (simple: just show material balance) */

    wrefresh(g_win_board);
}

static void draw_pgn(void) {
    if (!g_win_pgn) return;
    werase(g_win_pgn);
    box(g_win_pgn, 0, 0);

    wattron(g_win_pgn, COLOR_PAIR(CP_BORDER) | A_BOLD);
    mvwprintw(g_win_pgn, 0, 2, " PGN Moves ");
    wattroff(g_win_pgn, COLOR_PAIR(CP_BORDER) | A_BOLD);

    update_pgn();

    int max_y, max_x;
    getmaxyx(g_win_pgn, max_y, max_x);

    wattron(g_win_pgn, COLOR_PAIR(CP_PGN_TEXT));

    /* Word-wrap PGN text */
    int y = 1, x = 1;
    int max_w = max_x - 2;
    char tmp[MAX_PGN_LEN];
    strncpy(tmp, g_pgn, MAX_PGN_LEN - 1);

    char *tok = strtok(tmp, " ");
    while (tok && y < max_y - 1) {
        int len = strlen(tok);
        if (x + len + 1 > max_w) {
            y++; x = 1;
            if (y >= max_y - 1) break;
        }
        mvwprintw(g_win_pgn, y, x, "%s ", tok);
        x += len + 1;
        tok = strtok(NULL, " ");
    }

    wattroff(g_win_pgn, COLOR_PAIR(CP_PGN_TEXT));
    wrefresh(g_win_pgn);
}

static void draw_status(void) {
    if (!g_win_status) return;
    werase(g_win_status);

    wattron(g_win_status, COLOR_PAIR(CP_STATUS_BAR));
    int max_y, max_x;
    getmaxyx(g_win_status, max_y, max_x);
    for (int i = 0; i < max_x; i++)
        mvwaddch(g_win_status, 0, i, ' ');

    /* Status message */
    char turn_str[64];
    if (g_state.game_over == 1)
        snprintf(turn_str, sizeof(turn_str), "CHECKMATE! %s wins",
                 g_state.winner == WHITE_PIECE ? "White" : "Black");
    else if (g_state.game_over == 2)
        snprintf(turn_str, sizeof(turn_str), "STALEMATE - Draw");
    else if (g_state.game_over == 3)
        snprintf(turn_str, sizeof(turn_str), "DRAW (50-move rule)");
    else if (is_in_check(&g_state, g_state.turn))
        snprintf(turn_str, sizeof(turn_str), "%s to move - CHECK!",
                 g_state.turn == WHITE_PIECE ? "White" : "Black");
    else
        snprintf(turn_str, sizeof(turn_str), "%s to move",
                 g_state.turn == WHITE_PIECE ? "White" : "Black");

    mvwprintw(g_win_status, 0, 1, "%s", turn_str);

    /* Engine info */
    if (g_engine.active) {
        const char *sides[] = {"None", "Black", "White", "Both"};
        mvwprintw(g_win_status, 0, max_x - 30,
                  "Engine[%s]:%s",
                  g_engine.name[0] ? g_engine.name : "UCI",
                  g_engine.thinking ? "thinking..." :
                  sides[g_engine_side & 3]);
    }

    wattroff(g_win_status, COLOR_PAIR(CP_STATUS_BAR));

    /* Key hints */
    wattron(g_win_status, A_BOLD);
    if (max_y > 1) {
        mvwprintw(g_win_status, 1, 0,
            " [Arrows]Move  [Space/Enter]Select  [U]Undo  [F]Flip  "
            "[N]New  [E]Engine  [Q]Quit");
    }
    wattroff(g_win_status, A_BOLD);

    wrefresh(g_win_status);
}

static void redraw_all(void) {
    draw_board();
    draw_pgn();
    draw_status();
}

/* ─────────────────────────────────────────────────────────────────────────────
   PROMOTION DIALOG
───────────────────────────────────────────────────────────────────────────── */

static int promotion_dialog(int color) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int dh = 7, dw = 30;
    int dy = (max_y - dh) / 2;
    int dx = (max_x - dw) / 2;

    WINDOW *dwin = newwin(dh, dw, dy, dx);
    box(dwin, 0, 0);
    wattron(dwin, A_BOLD | COLOR_PAIR(CP_BORDER));
    mvwprintw(dwin, 0, 2, " Promote Pawn ");
    wattroff(dwin, A_BOLD | COLOR_PAIR(CP_BORDER));

    const char *opts[] = {"Queen  (Q)", "Rook   (R)", "Bishop (B)", "Knight (N)"};
    int pieces[]       = {QUEEN, ROOK, BISHOP, KNIGHT};
    int sel = 0;
    int running = 1;

    while (running) {
        for (int i = 0; i < 4; i++) {
            if (i == sel) wattron(dwin, A_REVERSE);
            mvwprintw(dwin, i + 2, 5, "%s", opts[i]);
            if (i == sel) wattroff(dwin, A_REVERSE);
        }
        mvwprintw(dwin, dh - 2, 2, "Use arrows + Enter");
        wrefresh(dwin);

        int ch = wgetch(dwin);
        switch (ch) {
            case KEY_UP:   sel = (sel + 3) % 4; break;
            case KEY_DOWN: sel = (sel + 1) % 4; break;
            case '\n': case ' ': running = 0; break;
            case 'q': case 'Q': running = 0; sel = 0; break;
            case 'r': case 'R': sel = 1; running = 0; break;
            case 'b': case 'B': sel = 2; running = 0; break;
            case 'n': case 'N': sel = 3; running = 0; break;
        }
    }
    delwin(dwin);
    redraw_all();
    return pieces[sel];
}

/* ─────────────────────────────────────────────────────────────────────────────
   HANDLE USER MOVE
───────────────────────────────────────────────────────────────────────────── */

static void handle_select(void) {
    if (g_state.game_over) return;

    /* Engine is thinking – don't accept input */
    if (g_engine.thinking) return;

    /* If it's engine's turn, don't accept human input */
    if ((g_state.turn == WHITE_PIECE && g_engine_side == 2) ||
        (g_state.turn == BLACK_PIECE && g_engine_side == 1) ||
        g_engine_side == 3) return;

    int cf = g_cursor_file, cr = g_cursor_rank;

    if (g_sel_file < 0) {
        /* First click: select piece */
        if (g_state.board[cf][cr].piece != EMPTY &&
            g_state.board[cf][cr].color == g_state.turn) {
            g_sel_file = cf; g_sel_rank = cr;
            compute_legal_targets(&g_state, cf, cr);
        }
    } else {
        /* Second click: attempt move */
        int ff = g_sel_file, fr = g_sel_rank;

        if (cf == ff && cr == fr) {
            /* Deselect */
            g_sel_file = -1; g_sel_rank = -1;
            g_legal_count = 0;
            memset(g_legal_targets, 0, sizeof(g_legal_targets));
            return;
        }

        /* Click another own piece → reselect */
        if (g_state.board[cf][cr].piece != EMPTY &&
            g_state.board[cf][cr].color == g_state.turn) {
            g_sel_file = cf; g_sel_rank = cr;
            compute_legal_targets(&g_state, cf, cr);
            return;
        }

        /* Promotion check */
        int promo = EMPTY;
        if (g_state.board[ff][fr].piece == PAWN && (cr == 7 || cr == 0)) {
            if (is_legal_move(&g_state, ff, fr, cf, cr, QUEEN)) {
                promo = promotion_dialog(g_state.turn);
            }
        }

        if (!is_legal_move(&g_state, ff, fr, cf, cr, promo)) {
            /* Illegal – deselect */
            g_sel_file = -1; g_sel_rank = -1;
            g_legal_count = 0;
            memset(g_legal_targets, 0, sizeof(g_legal_targets));
            snprintf(g_status, sizeof(g_status), "Illegal move!");
            return;
        }

        apply_move(&g_state, ff, fr, cf, cr, promo);
        g_sel_file = -1; g_sel_rank = -1;
        g_legal_count = 0;
        memset(g_legal_targets, 0, sizeof(g_legal_targets));

        /* Tell engine if applicable */
        if (g_engine.active) {
            if ((g_state.turn == BLACK_PIECE && g_engine_side == 1) ||
                (g_state.turn == WHITE_PIECE && g_engine_side == 2) ||
                g_engine_side == 3) {
                engine_go();
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
   ENGINE SIDE TOGGLE
───────────────────────────────────────────────────────────────────────────── */

static void toggle_engine_side(void) {
    if (!g_engine.active) return;
    g_engine_side = (g_engine_side + 1) % 4;
    snprintf(g_status, sizeof(g_status), "Engine plays: %s",
             (const char*[]){"None","Black","White","Both"}[g_engine_side]);

    /* If engine's turn right now, fire it */
    if (!g_state.game_over) {
        if ((g_state.turn == BLACK_PIECE && g_engine_side == 1) ||
            (g_state.turn == WHITE_PIECE && g_engine_side == 2) ||
            g_engine_side == 3) {
            engine_go();
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
   LAYOUT / RESIZE
───────────────────────────────────────────────────────────────────────────── */

static void setup_windows(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (g_win_board)  { delwin(g_win_board);  g_win_board  = NULL; }
    if (g_win_pgn)    { delwin(g_win_pgn);    g_win_pgn    = NULL; }
    if (g_win_status) { delwin(g_win_status); g_win_status = NULL; }

    int board_h = BOARD_Y_OFFSET + 8 * CELL_H + 2;
    int board_w = BOARD_X_OFFSET + 8 * CELL_W + 14;

    int pgn_x = board_w + 1;
    int pgn_w = cols - pgn_x - 1;
    if (pgn_w < 20) pgn_w = 20;

    int status_h = 2;
    int status_y = rows - status_h;

    g_win_board  = newwin(board_h,    board_w, 0,        0);
    g_win_pgn    = newwin(status_y,   pgn_w,   0,        pgn_x);
    g_win_status = newwin(status_h,   cols,    status_y, 0);

    keypad(g_win_board, TRUE);
    nodelay(g_win_board, TRUE);
}

/* ─────────────────────────────────────────────────────────────────────────────
   MAIN LOOP
───────────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* Locale for unicode */
    setenv("LANG", "en_US.UTF-8", 0);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (!has_colors()) {
        endwin();
        fprintf(stderr, "Terminal does not support colors.\n");
        return 1;
    }
    init_colors();

    /* Try unicode – fall back to ASCII if terminal can't handle it */
    {
        int r, c;
        getmaxyx(stdscr, r, c);
        (void)r; (void)c;
        /* Attempt to print a unicode char; if columns consumed > 1, ASCII mode */
        /* Simple heuristic: just check TERM */
        const char *term = getenv("TERM");
        if (term && (strstr(term, "xterm") || strstr(term, "256") ||
                     strstr(term, "utf") || strstr(term, "linux"))) {
            g_use_unicode = 1;
        } else {
            g_use_unicode = 0;
        }
    }

    /* Start engine if path given */
    memset(&g_engine, 0, sizeof(g_engine));
    g_engine_side = 0;
    if (argc >= 2) {
        if (start_engine(argv[1])) {
            snprintf(g_status, sizeof(g_status),
                     "Engine '%s' loaded.", g_engine.name);
            g_engine_side = 1; /* engine plays black by default */
        } else {
            snprintf(g_status, sizeof(g_status),
                     "Failed to start engine: %s", argv[1]);
        }
    } else {
        snprintf(g_status, sizeof(g_status),
                 "No engine loaded. Two-player mode.");
    }

    init_board(&g_state);
    g_cursor_file = 4; g_cursor_rank = 0;
    g_sel_file    = -1; g_sel_rank = -1;

    setup_windows();
    redraw_all();

    int running = 1;
    while (running) {
        /* Poll engine */
        if (g_engine.active && g_engine.thinking) {
            engine_read_response();
            if (!g_engine.thinking && g_engine.best_move[0]) {
                /* Apply engine move */
                if (!g_state.game_over)
                    parse_uci_move(&g_state, g_engine.best_move);

                /* Check if engine should move again (both sides) */
                if (!g_state.game_over && g_engine_side == 3) {
                    engine_go();
                }
                redraw_all();
            }
        }

        /* Input */
        int ch = wgetch(g_win_board);
        if (ch == ERR) {
            usleep(50000);
            continue;
        }

        switch (ch) {
        case KEY_UP:
            g_cursor_rank = (g_cursor_rank < 7) ? g_cursor_rank + 1 : 7;
            break;
        case KEY_DOWN:
            g_cursor_rank = (g_cursor_rank > 0) ? g_cursor_rank - 1 : 0;
            break;
        case KEY_LEFT:
            g_cursor_file = (g_cursor_file > 0) ? g_cursor_file - 1 : 0;
            break;
        case KEY_RIGHT:
            g_cursor_file = (g_cursor_file < 7) ? g_cursor_file + 1 : 7;
            break;

        case '\n': case ' ':
            handle_select();
            break;

        case 'u': case 'U':
            if (g_state.history_count > 0 && !g_engine.thinking) {
                undo_move(&g_state);
                /* If engine is playing, undo twice to get back to human's turn */
                if (g_engine.active && g_engine_side != 0 &&
                    g_engine_side != 3 && g_state.history_count > 0) {
                    /* check if it's now engine's turn */
                    int eng_turn = ((g_state.turn == BLACK_PIECE && g_engine_side == 1) ||
                                    (g_state.turn == WHITE_PIECE && g_engine_side == 2));
                    if (eng_turn) undo_move(&g_state);
                }
                g_sel_file = -1; g_sel_rank = -1;
                g_legal_count = 0;
                memset(g_legal_targets, 0, sizeof(g_legal_targets));
            }
            break;

        case 'f': case 'F':
            g_flipped = !g_flipped;
            break;

        case 'n': case 'N': {
            /* Confirm new game */
            int my, mx;
            getmaxyx(stdscr, my, mx);
            WINDOW *cw = newwin(5, 36, my/2-2, mx/2-18);
            box(cw, 0, 0);
            mvwprintw(cw, 1, 2, "Start new game?");
            mvwprintw(cw, 2, 2, "[Y] Yes    [N] No");
            wrefresh(cw);
            nodelay(cw, FALSE);
            int c2 = wgetch(cw);
            delwin(cw);
            if (c2 == 'y' || c2 == 'Y') {
                if (g_engine.thinking) engine_send("stop");
                init_board(&g_state);
                g_sel_file = g_sel_rank = -1;
                g_legal_count = 0;
                memset(g_legal_targets, 0, sizeof(g_legal_targets));
                memset(g_pgn, 0, sizeof(g_pgn));
                g_engine.thinking = 0;

                /* If engine plays white, kick it off */
                if (g_engine.active && (g_engine_side == 2 || g_engine_side == 3))
                    engine_go();
            }
            nodelay(g_win_board, TRUE);
            break;
        }

        case 'e': case 'E':
            toggle_engine_side();
            break;

        case 'q': case 'Q':
            running = 0;
            break;

        case KEY_RESIZE:
            setup_windows();
            break;

        default:
            break;
        }

        redraw_all();
    }

    /* Cleanup */
    stop_engine();
    if (g_win_board)  delwin(g_win_board);
    if (g_win_pgn)    delwin(g_win_pgn);
    if (g_win_status) delwin(g_win_status);
    endwin();

    /* Print final PGN to stdout */
    update_pgn();
    printf("\nGame PGN:\n%s\n", g_pgn);
    return 0;
}
