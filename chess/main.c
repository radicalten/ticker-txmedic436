/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 * 
 * Compile: gcc -o chess_gui chess_gui.c
 * Run:     ./chess_gui [engine_path]
 *
 * Controls:
 *   Arrow Keys  - Move cursor
 *   Enter/Space - Select piece / confirm move
 *   U           - Undo last move
 *   T           - Change time controls
 *   Q           - Quit
 *   R           - Restart game
 *   F           - Flip board
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

/* ─── ANSI / Terminal ───────────────────────────────────────────────────── */
#define ESC "\033"
#define CSI "\033["
#define CLEAR_SCREEN     CSI "2J" CSI "H"
#define CURSOR_HOME      CSI "H"
#define CURSOR_HIDE      CSI "?25l"
#define CURSOR_SHOW      CSI "?25h"
#define SAVE_CURSOR      CSI "s"
#define RESTORE_CURSOR   CSI "u"
#define CLEAR_LINE       CSI "2K"
#define BOLD             CSI "1m"
#define DIM              CSI "2m"
#define RESET            CSI "0m"
#define REVERSE          CSI "7m"

/* Colors */
#define FG_BLACK         CSI "30m"
#define FG_RED           CSI "31m"
#define FG_GREEN         CSI "32m"
#define FG_YELLOW        CSI "33m"
#define FG_BLUE          CSI "34m"
#define FG_MAGENTA       CSI "35m"
#define FG_CYAN          CSI "36m"
#define FG_WHITE         CSI "37m"
#define FG_BRIGHT_WHITE  CSI "97m"
#define FG_BRIGHT_YELLOW CSI "93m"
#define FG_BRIGHT_CYAN   CSI "96m"
#define FG_BRIGHT_GREEN  CSI "92m"
#define FG_BRIGHT_RED    CSI "91m"

/* 256-color backgrounds for board squares */
#define BG_LIGHT_SQ      CSI "48;5;222m"   /* warm tan */
#define BG_DARK_SQ       CSI "48;5;136m"   /* brown    */
#define BG_SEL_SQ        CSI "48;5;226m"   /* bright yellow */
#define BG_MOVE_SQ       CSI "48;5;118m"   /* green highlight */
#define BG_LAST_FROM     CSI "48;5;214m"   /* orange */
#define BG_LAST_TO       CSI "48;5;208m"   /* dark orange */
#define BG_CHECK_SQ      CSI "48;5;196m"   /* red */
#define BG_CURSOR_SQ     CSI "48;5;51m"    /* cyan */

/* ─── Board constants ───────────────────────────────────────────────────── */
#define EMPTY  0
#define WP 1
#define WN 2
#define WB 3
#define WR 4
#define WQ 5
#define WK 6
#define BP 7
#define BN 8
#define BB 9
#define BR 10
#define BQ 11
#define BK 12

#define WHITE 0
#define BLACK 1

#define MAX_MOVES      256
#define MAX_HISTORY    512
#define MAX_PGN_LEN   8192
#define MAX_MOVE_STR    16
#define MAX_ENGINE_LINE 4096

/* ─── Data structures ───────────────────────────────────────────────────── */
typedef struct {
    int from_sq;      /* 0-63 */
    int to_sq;
    int piece;        /* piece that moved */
    int captured;     /* piece captured (EMPTY if none) */
    int promotion;    /* promotion piece or EMPTY */
    int castle;       /* 0=none,1=K-side,2=Q-side */
    int ep_square;    /* en passant target before this move */
    int ep_capture;   /* ep capture square */
    /* castling rights before */
    int cr_wk, cr_wq, cr_bk, cr_bq;
    int halfmove;
    char san[16];
    char uci[8];
} Move;

typedef struct {
    int board[64];
    int side;          /* WHITE or BLACK to move */
    int ep_square;     /* en passant target square (-1 if none) */
    int castle_wk;
    int castle_wq;
    int castle_bk;
    int castle_bq;
    int halfmove;
    int fullmove;
    int in_check;

    Move history[MAX_HISTORY];
    int hist_count;

    /* Last move squares for highlighting */
    int last_from;
    int last_to;
} GameState;

typedef enum {
    TC_TIME,
    TC_DEPTH,
    TC_NODES
} TCMode;

typedef struct {
    TCMode mode;
    int time_ms;    /* milliseconds per move */
    int depth;
    long nodes;
} TimeControl;

typedef struct {
    pid_t pid;
    int to_engine[2];
    int from_engine[2];
    int running;
    int uci_ok;
    char name[128];
    char best_move[16];
    char ponder[16];
    int thinking;
} Engine;

/* ─── Globals ───────────────────────────────────────────────────────────── */
static GameState g_state;
static Engine    g_engine;
static TimeControl g_tc = { TC_TIME, 2000, 5, 100000 };
static struct termios g_old_termios;
static int g_cursor_file = 4; /* e */
static int g_cursor_rank = 1; /* rank 2 from white's perspective -> index 1 */
static int g_selected_sq = -1;
static int g_flipped = 0;
static int g_human_side = WHITE;
static int g_game_over = 0;
static char g_pgn[MAX_PGN_LEN];
static int g_pgn_len = 0;
static char g_status_msg[256];
static int g_engine_mode = 0; /* 1 if engine path provided */
static char g_engine_path[512];
static volatile int g_sigchld = 0;
static int g_move_count_display = 0; /* fullmove number for PGN display */

/* Legal moves for current position */
static Move g_legal_moves[MAX_MOVES];
static int  g_legal_count = 0;
/* Legal destinations from selected square */
static int  g_legal_targets[64];

/* ─── Piece characters ──────────────────────────────────────────────────── */
/* Unicode chess pieces */
static const char *piece_chars[] = {
    " ",   /* EMPTY */
    "♙",   /* WP */
    "♘",   /* WN */
    "♗",   /* WB */
    "♖",   /* WR */
    "♕",   /* WQ */
    "♔",   /* WK */
    "♟",   /* BP */
    "♞",   /* BN */
    "♝",   /* BB */
    "♜",   /* BR */
    "♛",   /* BQ */
    "♚",   /* BK */
};

static const char piece_letters[] = ".PNBRQKpnbrqk";

/* ─── Forward declarations ──────────────────────────────────────────────── */
static void init_board(void);
static void generate_moves(GameState *s, Move *moves, int *count);
static int  is_square_attacked(GameState *s, int sq, int by_side);
static void make_move(GameState *s, Move *m);
static void unmake_move(GameState *s);
static void compute_legal_moves(void);
static int  find_move(int from, int to, int promo);
static void apply_move_san(Move *m);

static void render_board(void);
static void render_sidebar(void);
static void render_all(void);

static void engine_start(void);
static void engine_send(const char *fmt, ...);
static void engine_go(void);
static void engine_process_output(void);
static void engine_stop(void);

static void tc_menu(void);
static void undo_move(void);
static void restart_game(void);
static char getch_raw(void);
static void set_raw_mode(void);
static void restore_terminal(void);
static void cleanup(void);

/* ─── Utility ───────────────────────────────────────────────────────────── */
static inline int rank_of(int sq) { return sq / 8; }
static inline int file_of(int sq) { return sq % 8; }
static inline int sq_of(int r, int f) { return r * 8 + f; }
static inline int is_white(int p) { return p >= WP && p <= WK; }
static inline int is_black(int p) { return p >= BP && p <= BK; }
static inline int piece_color(int p) { return is_black(p) ? BLACK : WHITE; }
static inline int same_color(int p1, int p2) {
    if (!p1 || !p2) return 0;
    return piece_color(p1) == piece_color(p2);
}

static void pos_to_str(int sq, char *out) {
    out[0] = 'a' + file_of(sq);
    out[1] = '1' + rank_of(sq);
    out[2] = '\0';
}

static int str_to_sq(const char *s) {
    if (!s || s[0] < 'a' || s[0] > 'h') return -1;
    if (s[1] < '1' || s[1] > '8') return -1;
    return sq_of(s[1] - '1', s[0] - 'a');
}

/* ─── Board Initialization ──────────────────────────────────────────────── */
static void init_board(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.ep_square = -1;
    g_state.castle_wk = 1;
    g_state.castle_wq = 1;
    g_state.castle_bk = 1;
    g_state.castle_bq = 1;
    g_state.fullmove  = 1;
    g_state.last_from = -1;
    g_state.last_to   = -1;

    /* White pieces rank 1 (index 0) */
    g_state.board[0] = WR; g_state.board[1] = WN; g_state.board[2] = WB;
    g_state.board[3] = WQ; g_state.board[4] = WK; g_state.board[5] = WB;
    g_state.board[6] = WN; g_state.board[7] = WR;
    /* White pawns rank 2 */
    for (int f = 0; f < 8; f++) g_state.board[8 + f] = WP;
    /* Black pawns rank 7 */
    for (int f = 0; f < 8; f++) g_state.board[48 + f] = BP;
    /* Black pieces rank 8 */
    g_state.board[56] = BR; g_state.board[57] = BN; g_state.board[58] = BB;
    g_state.board[59] = BQ; g_state.board[60] = BK; g_state.board[61] = BB;
    g_state.board[62] = BN; g_state.board[63] = BR;

    g_pgn[0] = '\0';
    g_pgn_len = 0;
    g_game_over = 0;
    g_selected_sq = -1;
    g_cursor_file = 4;
    g_cursor_rank = 1;
    g_status_msg[0] = '\0';
    g_move_count_display = 1;
}

/* ─── Attack detection ──────────────────────────────────────────────────── */
static const int knight_delta[8][2] = {
    {-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}
};
static const int bishop_delta[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
static const int rook_delta[4][2]   = {{-1,0},{1,0},{0,-1},{0,1}};
static const int king_delta[8][2]   = {
    {-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}
};

static int is_square_attacked(GameState *s, int sq, int by_side) {
    int r = rank_of(sq), f = file_of(sq);

    /* Pawn attacks */
    if (by_side == WHITE) {
        /* White pawn attacks upward: pawn at (r-1, f±1) */
        if (r > 0) {
            if (f > 0 && s->board[sq_of(r-1,f-1)] == WP) return 1;
            if (f < 7 && s->board[sq_of(r-1,f+1)] == WP) return 1;
        }
    } else {
        /* Black pawn attacks downward */
        if (r < 7) {
            if (f > 0 && s->board[sq_of(r+1,f-1)] == BP) return 1;
            if (f < 7 && s->board[sq_of(r+1,f+1)] == BP) return 1;
        }
    }

    /* Knight */
    int atk_knight = (by_side == WHITE) ? WN : BN;
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_delta[i][0], nf = f + knight_delta[i][1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8)
            if (s->board[sq_of(nr,nf)] == atk_knight) return 1;
    }

    /* Bishop / Queen (diagonals) */
    int atk_b = (by_side == WHITE) ? WB : BB;
    int atk_q = (by_side == WHITE) ? WQ : BQ;
    for (int d = 0; d < 4; d++) {
        int nr = r + bishop_delta[d][0], nf = f + bishop_delta[d][1];
        while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int p = s->board[sq_of(nr,nf)];
            if (p) {
                if (p == atk_b || p == atk_q) return 1;
                break;
            }
            nr += bishop_delta[d][0]; nf += bishop_delta[d][1];
        }
    }

    /* Rook / Queen (straights) */
    int atk_r = (by_side == WHITE) ? WR : BR;
    for (int d = 0; d < 4; d++) {
        int nr = r + rook_delta[d][0], nf = f + rook_delta[d][1];
        while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int p = s->board[sq_of(nr,nf)];
            if (p) {
                if (p == atk_r || p == atk_q) return 1;
                break;
            }
            nr += rook_delta[d][0]; nf += rook_delta[d][1];
        }
    }

    /* King */
    int atk_k = (by_side == WHITE) ? WK : BK;
    for (int i = 0; i < 8; i++) {
        int nr = r + king_delta[i][0], nf = f + king_delta[i][1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8)
            if (s->board[sq_of(nr,nf)] == atk_k) return 1;
    }

    return 0;
}

static int find_king(GameState *s, int side) {
    int king = (side == WHITE) ? WK : BK;
    for (int i = 0; i < 64; i++)
        if (s->board[i] == king) return i;
    return -1;
}

static int in_check(GameState *s, int side) {
    int ksq = find_king(s, side);
    if (ksq < 0) return 0;
    return is_square_attacked(s, ksq, 1 - side);
}

/* ─── Move generation ───────────────────────────────────────────────────── */
static void add_move(Move *moves, int *count, GameState *s,
                     int from, int to, int promo) {
    if (*count >= MAX_MOVES) return;
    Move m;
    memset(&m, 0, sizeof(m));
    m.from_sq  = from;
    m.to_sq    = to;
    m.piece    = s->board[from];
    m.captured = s->board[to];
    m.promotion = promo;
    m.ep_square = s->ep_square;
    m.cr_wk = s->castle_wk; m.cr_wq = s->castle_wq;
    m.cr_bk = s->castle_bk; m.cr_bq = s->castle_bq;
    m.halfmove = s->halfmove;

    /* Check for en passant capture */
    if ((m.piece == WP || m.piece == BP) && to == s->ep_square && s->ep_square >= 0) {
        m.captured = (m.piece == WP) ? BP : WP;
        m.ep_capture = (m.piece == WP) ? to - 8 : to + 8;
    } else {
        m.ep_capture = -1;
    }

    /* Build UCI string */
    char fs[3], ts[3];
    pos_to_str(from, fs);
    pos_to_str(to, ts);
    if (promo) {
        char pc[] = {'.','p','n','b','r','q','k','p','n','b','r','q','k'};
        snprintf(m.uci, sizeof(m.uci), "%s%s%c", fs, ts, tolower(pc[promo]));
    } else {
        snprintf(m.uci, sizeof(m.uci), "%s%s", fs, ts);
    }

    moves[*count] = m;
    (*count)++;
}

static void gen_pawn_moves(GameState *s, Move *moves, int *count, int sq) {
    int piece = s->board[sq];
    int r = rank_of(sq), f = file_of(sq);

    if (piece == WP) {
        /* One step forward */
        if (r + 1 <= 7 && s->board[sq_of(r+1,f)] == EMPTY) {
            if (r + 1 == 7) {
                add_move(moves, count, s, sq, sq_of(7,f), WQ);
                add_move(moves, count, s, sq, sq_of(7,f), WR);
                add_move(moves, count, s, sq, sq_of(7,f), WB);
                add_move(moves, count, s, sq, sq_of(7,f), WN);
            } else {
                add_move(moves, count, s, sq, sq_of(r+1,f), EMPTY);
                /* Two steps from rank 2 */
                if (r == 1 && s->board[sq_of(r+2,f)] == EMPTY)
                    add_move(moves, count, s, sq, sq_of(r+2,f), EMPTY);
            }
        }
        /* Captures */
        for (int df = -1; df <= 1; df += 2) {
            int nf = f + df;
            if (nf < 0 || nf > 7) continue;
            int nsq = sq_of(r+1, nf);
            if (is_black(s->board[nsq]) ||
                (s->ep_square >= 0 && nsq == s->ep_square)) {
                if (r + 1 == 7) {
                    add_move(moves, count, s, sq, nsq, WQ);
                    add_move(moves, count, s, sq, nsq, WR);
                    add_move(moves, count, s, sq, nsq, WB);
                    add_move(moves, count, s, sq, nsq, WN);
                } else {
                    add_move(moves, count, s, sq, nsq, EMPTY);
                }
            }
        }
    } else if (piece == BP) {
        if (r - 1 >= 0 && s->board[sq_of(r-1,f)] == EMPTY) {
            if (r - 1 == 0) {
                add_move(moves, count, s, sq, sq_of(0,f), BQ);
                add_move(moves, count, s, sq, sq_of(0,f), BR);
                add_move(moves, count, s, sq, sq_of(0,f), BB);
                add_move(moves, count, s, sq, sq_of(0,f), BN);
            } else {
                add_move(moves, count, s, sq, sq_of(r-1,f), EMPTY);
                if (r == 6 && s->board[sq_of(r-2,f)] == EMPTY)
                    add_move(moves, count, s, sq, sq_of(r-2,f), EMPTY);
            }
        }
        for (int df = -1; df <= 1; df += 2) {
            int nf = f + df;
            if (nf < 0 || nf > 7) continue;
            int nsq = sq_of(r-1, nf);
            if (is_white(s->board[nsq]) ||
                (s->ep_square >= 0 && nsq == s->ep_square)) {
                if (r - 1 == 0) {
                    add_move(moves, count, s, sq, nsq, BQ);
                    add_move(moves, count, s, sq, nsq, BR);
                    add_move(moves, count, s, sq, nsq, BB);
                    add_move(moves, count, s, sq, nsq, BN);
                } else {
                    add_move(moves, count, s, sq, nsq, EMPTY);
                }
            }
        }
    }
}

static void gen_slider(GameState *s, Move *moves, int *count, int sq,
                       const int delta[][2], int ndirs) {
    int piece = s->board[sq];
    int pcol = piece_color(piece);
    int r = rank_of(sq), f = file_of(sq);
    for (int d = 0; d < ndirs; d++) {
        int nr = r + delta[d][0], nf = f + delta[d][1];
        while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int nsq = sq_of(nr, nf);
            int np = s->board[nsq];
            if (np == EMPTY) {
                add_move(moves, count, s, sq, nsq, EMPTY);
            } else {
                if (piece_color(np) != pcol)
                    add_move(moves, count, s, sq, nsq, EMPTY);
                break;
            }
            nr += delta[d][0]; nf += delta[d][1];
        }
    }
}

static void gen_knight(GameState *s, Move *moves, int *count, int sq) {
    int pcol = piece_color(s->board[sq]);
    int r = rank_of(sq), f = file_of(sq);
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_delta[i][0], nf = f + knight_delta[i][1];
        if (nr < 0 || nr > 7 || nf < 0 || nf > 7) continue;
        int nsq = sq_of(nr, nf);
        if (s->board[nsq] == EMPTY || piece_color(s->board[nsq]) != pcol)
            add_move(moves, count, s, sq, nsq, EMPTY);
    }
}

static void gen_king(GameState *s, Move *moves, int *count, int sq) {
    int pcol = piece_color(s->board[sq]);
    int r = rank_of(sq), f = file_of(sq);
    for (int i = 0; i < 8; i++) {
        int nr = r + king_delta[i][0], nf = f + king_delta[i][1];
        if (nr < 0 || nr > 7 || nf < 0 || nf > 7) continue;
        int nsq = sq_of(nr, nf);
        if (s->board[nsq] == EMPTY || piece_color(s->board[nsq]) != pcol)
            add_move(moves, count, s, sq, nsq, EMPTY);
    }

    /* Castling */
    if (pcol == WHITE && sq == 4) {
        if (s->castle_wk &&
            s->board[5] == EMPTY && s->board[6] == EMPTY &&
            !is_square_attacked(s, 4, BLACK) &&
            !is_square_attacked(s, 5, BLACK) &&
            !is_square_attacked(s, 6, BLACK)) {
            Move m;
            memset(&m, 0, sizeof(m));
            m.from_sq = 4; m.to_sq = 6; m.piece = WK;
            m.castle = 1;
            m.ep_square = s->ep_square;
            m.cr_wk=s->castle_wk; m.cr_wq=s->castle_wq;
            m.cr_bk=s->castle_bk; m.cr_bq=s->castle_bq;
            m.halfmove = s->halfmove;
            m.ep_capture = -1;
            snprintf(m.uci, sizeof(m.uci), "e1g1");
            moves[(*count)++] = m;
        }
        if (s->castle_wq &&
            s->board[3] == EMPTY && s->board[2] == EMPTY && s->board[1] == EMPTY &&
            !is_square_attacked(s, 4, BLACK) &&
            !is_square_attacked(s, 3, BLACK) &&
            !is_square_attacked(s, 2, BLACK)) {
            Move m;
            memset(&m, 0, sizeof(m));
            m.from_sq = 4; m.to_sq = 2; m.piece = WK;
            m.castle = 2;
            m.ep_square = s->ep_square;
            m.cr_wk=s->castle_wk; m.cr_wq=s->castle_wq;
            m.cr_bk=s->castle_bk; m.cr_bq=s->castle_bq;
            m.halfmove = s->halfmove;
            m.ep_capture = -1;
            snprintf(m.uci, sizeof(m.uci), "e1c1");
            moves[(*count)++] = m;
        }
    } else if (pcol == BLACK && sq == 60) {
        if (s->castle_bk &&
            s->board[61] == EMPTY && s->board[62] == EMPTY &&
            !is_square_attacked(s, 60, WHITE) &&
            !is_square_attacked(s, 61, WHITE) &&
            !is_square_attacked(s, 62, WHITE)) {
            Move m;
            memset(&m, 0, sizeof(m));
            m.from_sq = 60; m.to_sq = 62; m.piece = BK;
            m.castle = 1;
            m.ep_square = s->ep_square;
            m.cr_wk=s->castle_wk; m.cr_wq=s->castle_wq;
            m.cr_bk=s->castle_bk; m.cr_bq=s->castle_bq;
            m.halfmove = s->halfmove;
            m.ep_capture = -1;
            snprintf(m.uci, sizeof(m.uci), "e8g8");
            moves[(*count)++] = m;
        }
        if (s->castle_bq &&
            s->board[59] == EMPTY && s->board[58] == EMPTY && s->board[57] == EMPTY &&
            !is_square_attacked(s, 60, WHITE) &&
            !is_square_attacked(s, 59, WHITE) &&
            !is_square_attacked(s, 58, WHITE)) {
            Move m;
            memset(&m, 0, sizeof(m));
            m.from_sq = 60; m.to_sq = 58; m.piece = BK;
            m.castle = 2;
            m.ep_square = s->ep_square;
            m.cr_wk=s->castle_wk; m.cr_wq=s->castle_wq;
            m.cr_bk=s->castle_bk; m.cr_bq=s->castle_bq;
            m.halfmove = s->halfmove;
            m.ep_capture = -1;
            snprintf(m.uci, sizeof(m.uci), "e8c8");
            moves[(*count)++] = m;
        }
    }
}

static void generate_pseudo_moves(GameState *s, Move *moves, int *count) {
    *count = 0;
    for (int sq = 0; sq < 64; sq++) {
        int p = s->board[sq];
        if (!p) continue;
        if (piece_color(p) != s->side) continue;
        switch (p) {
        case WP: case BP: gen_pawn_moves(s, moves, count, sq); break;
        case WN: case BN: gen_knight(s, moves, count, sq); break;
        case WB: case BB:
            gen_slider(s, moves, count, sq, bishop_delta, 4); break;
        case WR: case BR:
            gen_slider(s, moves, count, sq, rook_delta, 4); break;
        case WQ: case BQ:
            gen_slider(s, moves, count, sq, bishop_delta, 4);
            gen_slider(s, moves, count, sq, rook_delta, 4); break;
        case WK: case BK: gen_king(s, moves, count, sq); break;
        }
    }
}

/* Make/unmake for legality checking */
static void make_move_temp(GameState *s, Move *m) {
    s->board[m->to_sq] = m->piece;
    s->board[m->from_sq] = EMPTY;
    if (m->promotion) s->board[m->to_sq] = m->promotion;
    if (m->ep_capture >= 0 && m->ep_capture < 64)
        s->board[m->ep_capture] = EMPTY;
    if (m->castle == 1) {
        if (m->piece == WK) { s->board[5] = WR; s->board[7] = EMPTY; }
        else                { s->board[61] = BR; s->board[63] = EMPTY; }
    } else if (m->castle == 2) {
        if (m->piece == WK) { s->board[3] = WR; s->board[0] = EMPTY; }
        else                { s->board[59] = BR; s->board[56] = EMPTY; }
    }
}

static void unmake_move_temp(GameState *s, Move *m) {
    s->board[m->from_sq] = m->piece;
    s->board[m->to_sq]   = m->captured;
    if (m->promotion) s->board[m->to_sq] = EMPTY;
    if (m->ep_capture >= 0 && m->ep_capture < 64) {
        int cap_piece = (m->piece == WP) ? BP : WP;
        s->board[m->ep_capture] = cap_piece;
        s->board[m->to_sq] = EMPTY;
    }
    if (m->castle == 1) {
        if (m->piece == WK) { s->board[7] = WR; s->board[5] = EMPTY; }
        else                { s->board[63] = BR; s->board[61] = EMPTY; }
    } else if (m->castle == 2) {
        if (m->piece == WK) { s->board[0] = WR; s->board[3] = EMPTY; }
        else                { s->board[56] = BR; s->board[59] = EMPTY; }
    }
}

static void generate_moves(GameState *s, Move *moves, int *count) {
    Move pseudo[MAX_MOVES];
    int  pcnt = 0;
    generate_pseudo_moves(s, pseudo, &pcnt);
    *count = 0;
    for (int i = 0; i < pcnt; i++) {
        make_move_temp(s, &pseudo[i]);
        int legal = !in_check(s, s->side);
        unmake_move_temp(s, &pseudo[i]);
        if (legal) {
            moves[(*count)++] = pseudo[i];
        }
    }
}

/* ─── Make / Unmake permanent ───────────────────────────────────────────── */
static void make_move(GameState *s, Move *m) {
    /* Save move in history */
    s->history[s->hist_count++] = *m;

    int p = m->piece;
    s->board[m->to_sq] = p;
    s->board[m->from_sq] = EMPTY;

    if (m->promotion) s->board[m->to_sq] = m->promotion;

    if (m->ep_capture >= 0 && m->ep_capture < 64)
        s->board[m->ep_capture] = EMPTY;

    if (m->castle == 1) {
        if (p == WK) { s->board[5] = WR; s->board[7] = EMPTY; }
        else         { s->board[61] = BR; s->board[63] = EMPTY; }
    } else if (m->castle == 2) {
        if (p == WK) { s->board[3] = WR; s->board[0] = EMPTY; }
        else         { s->board[59] = BR; s->board[56] = EMPTY; }
    }

    /* Update castling rights */
    if (p == WK) { s->castle_wk = 0; s->castle_wq = 0; }
    if (p == BK) { s->castle_bk = 0; s->castle_bq = 0; }
    if (p == WR) {
        if (m->from_sq == 0) s->castle_wq = 0;
        if (m->from_sq == 7) s->castle_wk = 0;
    }
    if (p == BR) {
        if (m->from_sq == 56) s->castle_bq = 0;
        if (m->from_sq == 63) s->castle_bk = 0;
    }
    /* Capture of rook eliminates castling */
    if (m->to_sq == 0) s->castle_wq = 0;
    if (m->to_sq == 7) s->castle_wk = 0;
    if (m->to_sq == 56) s->castle_bq = 0;
    if (m->to_sq == 63) s->castle_bk = 0;

    /* En passant target */
    s->ep_square = -1;
    if (p == WP && m->to_sq - m->from_sq == 16)
        s->ep_square = m->from_sq + 8;
    if (p == BP && m->from_sq - m->to_sq == 16)
        s->ep_square = m->from_sq - 8;

    /* Halfmove clock */
    if (p == WP || p == BP || m->captured)
        s->halfmove = 0;
    else
        s->halfmove++;

    if (s->side == BLACK) s->fullmove++;
    s->side = 1 - s->side;

    s->last_from = m->from_sq;
    s->last_to   = m->to_sq;
    s->in_check  = in_check(s, s->side);
}

static void unmake_move(GameState *s) {
    if (s->hist_count == 0) return;
    Move *m = &s->history[--s->hist_count];

    s->side = 1 - s->side;
    if (s->side == BLACK) s->fullmove--;

    s->board[m->from_sq] = m->piece;
    s->board[m->to_sq]   = m->promotion ? EMPTY : m->captured;
    if (m->promotion) s->board[m->to_sq] = EMPTY;
    if (m->ep_capture >= 0 && m->ep_capture < 64) {
        int cap_piece = (m->piece == WP) ? BP : WP;
        s->board[m->ep_capture] = cap_piece;
        s->board[m->to_sq] = EMPTY;
    }
    if (m->castle == 1) {
        if (m->piece == WK) { s->board[7] = WR; s->board[5] = EMPTY; }
        else                { s->board[63] = BR; s->board[61] = EMPTY; }
    } else if (m->castle == 2) {
        if (m->piece == WK) { s->board[0] = WR; s->board[3] = EMPTY; }
        else                { s->board[56] = BR; s->board[59] = EMPTY; }
    }

    s->ep_square  = m->ep_square;
    s->castle_wk  = m->cr_wk; s->castle_wq = m->cr_wq;
    s->castle_bk  = m->cr_bk; s->castle_bq = m->cr_bq;
    s->halfmove   = m->halfmove;
    s->in_check   = in_check(s, s->side);

    if (s->hist_count > 0) {
        s->last_from = s->history[s->hist_count-1].from_sq;
        s->last_to   = s->history[s->hist_count-1].to_sq;
    } else {
        s->last_from = -1;
        s->last_to   = -1;
    }
}

/* ─── SAN generation ────────────────────────────────────────────────────── */
static const char *piece_san = ".PNBRQK.pnbrqk";

static void build_san(GameState *s, Move *m) {
    /* s is the state BEFORE the move */
    char buf[32];
    int pos = 0;

    if (m->castle == 1) { strcpy(m->san, "O-O"); return; }
    if (m->castle == 2) { strcpy(m->san, "O-O-O"); return; }

    int p = m->piece;
    int ptype = (p > 6) ? p - 6 : p; /* normalize to white piece type */

    /* Piece letter */
    if (ptype != 1) { /* not pawn */
        buf[pos++] = "PNBRQK"[ptype-1];
    }

    /* Disambiguation */
    if (ptype != 1) {
        int ambig_file = 0, ambig_rank = 0, ambig = 0;
        Move others[MAX_MOVES]; int ocnt = 0;
        generate_moves(s, others, &ocnt);
        for (int i = 0; i < ocnt; i++) {
            if (others[i].from_sq == m->from_sq) continue;
            if (others[i].to_sq != m->to_sq) continue;
            int op = others[i].piece;
            int optype = (op > 6) ? op - 6 : op;
            if (optype != ptype) continue;
            ambig = 1;
            if (file_of(others[i].from_sq) == file_of(m->from_sq))
                ambig_rank = 1;
            else
                ambig_file = 1;
        }
        if (ambig) {
            if (ambig_file && !ambig_rank)
                buf[pos++] = '1' + rank_of(m->from_sq);
            else if (!ambig_file)
                buf[pos++] = 'a' + file_of(m->from_sq);
            else {
                buf[pos++] = 'a' + file_of(m->from_sq);
                buf[pos++] = '1' + rank_of(m->from_sq);
            }
        }
    } else {
        /* Pawn capture: add file */
        if (m->captured || m->ep_capture >= 0)
            buf[pos++] = 'a' + file_of(m->from_sq);
    }

    if (m->captured || (m->ep_capture >= 0))
        buf[pos++] = 'x';

    buf[pos++] = 'a' + file_of(m->to_sq);
    buf[pos++] = '1' + rank_of(m->to_sq);

    if (m->promotion) {
        buf[pos++] = '=';
        int pp = m->promotion;
        int pptype = (pp > 6) ? pp - 6 : pp;
        buf[pos++] = "PNBRQK"[pptype-1];
    }

    /* Check / checkmate detection */
    GameState tmp = *s;
    make_move_temp(&tmp, m);
    int opp = 1 - s->side;
    if (in_check(&tmp, opp)) {
        /* Check for checkmate */
        Move resp[MAX_MOVES]; int rcnt = 0;
        tmp.side = opp;
        generate_moves(&tmp, resp, &rcnt);
        buf[pos++] = (rcnt == 0) ? '#' : '+';
    }

    buf[pos] = '\0';
    strncpy(m->san, buf, sizeof(m->san)-1);
}

/* ─── PGN accumulation ──────────────────────────────────────────────────── */
static void pgn_append_move(Move *m, int fullmove, int side) {
    char tmp[32];
    if (side == WHITE)
        snprintf(tmp, sizeof(tmp), "%d. %s ", fullmove, m->san);
    else
        snprintf(tmp, sizeof(tmp), "%s ", m->san);
    int len = (int)strlen(tmp);
    if (g_pgn_len + len < MAX_PGN_LEN - 1) {
        strcat(g_pgn, tmp);
        g_pgn_len += len;
    }
}

/* ─── Legal move helpers ────────────────────────────────────────────────── */
static void compute_legal_moves(void) {
    generate_moves(&g_state, g_legal_moves, &g_legal_count);
    memset(g_legal_targets, 0, sizeof(g_legal_targets));
    if (g_selected_sq >= 0) {
        for (int i = 0; i < g_legal_count; i++) {
            if (g_legal_moves[i].from_sq == g_selected_sq)
                g_legal_targets[g_legal_moves[i].to_sq] = 1;
        }
    }
}

static int find_move(int from, int to, int promo) {
    for (int i = 0; i < g_legal_count; i++) {
        if (g_legal_moves[i].from_sq == from &&
            g_legal_moves[i].to_sq   == to) {
            if (promo == 0) return i;
            if (g_legal_moves[i].promotion == promo) return i;
        }
    }
    return -1;
}

/* ─── Rendering ─────────────────────────────────────────────────────────── */
#define BOARD_ROW    3
#define BOARD_COL    2
#define CELL_W       4  /* chars wide per cell (includes space for unicode) */
#define CELL_H       2  /* rows per cell */

static void move_to(int row, int col) {
    printf(CSI "%d;%dH", row, col);
}

/* Draw a single board cell */
static void draw_cell(int display_r, int display_f, int sq) {
    /* display_r, display_f: visual row/file (0=top-left from viewer) */
    int scr_row = BOARD_ROW + display_r * CELL_H;
    int scr_col = BOARD_COL + display_f * CELL_W;

    /* Determine base color of square */
    int actual_r = rank_of(sq);
    int actual_f = file_of(sq);
    int light = (actual_r + actual_f) % 2 == 1;

    /* Square background */
    const char *bg;
    int cursor_sq = sq_of(g_cursor_rank, g_cursor_file);

    if (sq == cursor_sq)
        bg = BG_CURSOR_SQ;
    else if (sq == g_selected_sq)
        bg = BG_SEL_SQ;
    else if (g_selected_sq >= 0 && g_legal_targets[sq])
        bg = BG_MOVE_SQ;
    else if (sq == g_state.last_from)
        bg = BG_LAST_FROM;
    else if (sq == g_state.last_to)
        bg = BG_LAST_TO;
    else if (g_state.in_check) {
        int ksq = find_king(&g_state, g_state.side);
        if (sq == ksq) bg = BG_CHECK_SQ;
        else bg = light ? BG_LIGHT_SQ : BG_DARK_SQ;
    } else
        bg = light ? BG_LIGHT_SQ : BG_DARK_SQ;

    int piece = g_state.board[sq];

    /* Top half of cell */
    move_to(scr_row, scr_col);
    printf("%s", bg);
    /* Piece color */
    if (piece) {
        if (is_white(piece))
            printf(BOLD FG_BRIGHT_WHITE);
        else
            printf(BOLD FG_BLACK);
    }
    /* Draw piece on top row, spaces on bottom row */
    if (piece)
        printf(" %s  " RESET, piece_chars[piece]);
    else
        printf("    " RESET);

    /* Bottom half */
    move_to(scr_row + 1, scr_col);
    printf("%s    " RESET, bg);
}

static void render_board(void) {
    /* Draw border + coordinates */
    /* Top border */
    move_to(BOARD_ROW - 1, BOARD_COL);
    printf(BOLD FG_BRIGHT_WHITE);
    if (!g_flipped)
        printf("  a   b   c   d   e   f   g   h" RESET);
    else
        printf("  h   g   f   e   d   c   b   a" RESET);

    for (int dr = 0; dr < 8; dr++) {
        /* Actual rank on board */
        int actual_r = g_flipped ? dr : 7 - dr;
        /* Rank label */
        move_to(BOARD_ROW + dr * CELL_H,     BOARD_COL - 2);
        printf(BOLD FG_BRIGHT_WHITE "%d " RESET, actual_r + 1);
        move_to(BOARD_ROW + dr * CELL_H + 1, BOARD_COL - 2);
        printf("  ");

        for (int df = 0; df < 8; df++) {
            int actual_f = g_flipped ? 7 - df : df;
            int sq = sq_of(actual_r, actual_f);
            draw_cell(dr, df, sq);
        }

        /* Right rank label */
        move_to(BOARD_ROW + dr * CELL_H,     BOARD_COL + 32 + 1);
        printf(BOLD FG_BRIGHT_WHITE " %d" RESET, actual_r + 1);
    }

    /* Bottom file labels */
    move_to(BOARD_ROW + 16, BOARD_COL);
    printf(BOLD FG_BRIGHT_WHITE);
    if (!g_flipped)
        printf("  a   b   c   d   e   f   g   h" RESET);
    else
        printf("  h   g   f   e   d   c   b   a" RESET);
}

/* PGN display: word-wrap to 30 chars */
#define SIDE_COL   40
#define SIDE_ROW    2
#define PGN_ROWS   20

static void render_sidebar(void) {
    int row = SIDE_ROW;
    int col = SIDE_COL;

    /* Title */
    move_to(row++, col);
    printf(BOLD FG_BRIGHT_CYAN "╔══════════════════════════════╗" RESET);
    move_to(row++, col);
    printf(BOLD FG_BRIGHT_CYAN "║  " FG_BRIGHT_YELLOW "♔  Terminal Chess GUI  ♚  "
           FG_BRIGHT_CYAN "║" RESET);
    move_to(row++, col);
    printf(BOLD FG_BRIGHT_CYAN "╚══════════════════════════════╝" RESET);
    row++;

    /* Engine info */
    move_to(row++, col);
    printf(BOLD FG_BRIGHT_GREEN "Engine: " RESET);
    if (g_engine_mode && g_engine.running)
        printf(FG_BRIGHT_WHITE "%s" RESET, g_engine.name[0] ? g_engine.name : "Connected");
    else if (g_engine_mode)
        printf(FG_RED "Not running" RESET);
    else
        printf(FG_YELLOW "Human vs Human" RESET);

    /* Time control */
    move_to(row++, col);
    printf(BOLD FG_BRIGHT_GREEN "Time Control: " RESET);
    switch (g_tc.mode) {
    case TC_TIME:  printf(FG_WHITE "Time %dms/move" RESET, g_tc.time_ms); break;
    case TC_DEPTH: printf(FG_WHITE "Depth %d" RESET, g_tc.depth); break;
    case TC_NODES: printf(FG_WHITE "Nodes %ld" RESET, g_tc.nodes); break;
    }

    /* Side to move */
    move_to(row++, col);
    printf(BOLD FG_BRIGHT_GREEN "To move: " RESET);
    if (g_game_over)
        printf(FG_RED "Game Over" RESET);
    else
        printf(FG_BRIGHT_WHITE "%s%s" RESET,
               g_state.side == WHITE ? "White" : "Black",
               g_state.in_check ? " [CHECK]" : "");

    /* Fullmove */
    move_to(row++, col);
    printf(BOLD FG_BRIGHT_GREEN "Move: " RESET FG_WHITE "%d" RESET,
           g_state.fullmove);

    row++;

    /* PGN header */
    move_to(row++, col);
    printf(BOLD FG_BRIGHT_CYAN "── Move History ──────────────" RESET);

    /* PGN display with word wrap */
    char pgn_copy[MAX_PGN_LEN];
    strncpy(pgn_copy, g_pgn, MAX_PGN_LEN-1);
    pgn_copy[MAX_PGN_LEN-1] = '\0';

    /* Display last PGN_ROWS lines */
    char lines[PGN_ROWS][64];
    int  lcount = 0;
    memset(lines, 0, sizeof(lines));

    char *token = strtok(pgn_copy, " ");
    char cur_line[64] = "";
    while (token) {
        if (strlen(cur_line) + strlen(token) + 1 > 30) {
            if (lcount < PGN_ROWS)
                strncpy(lines[lcount++], cur_line, 63);
            strcpy(cur_line, token);
            strcat(cur_line, " ");
        } else {
            strcat(cur_line, token);
            strcat(cur_line, " ");
        }
        token = strtok(NULL, " ");
    }
    if (strlen(cur_line) > 0 && lcount < PGN_ROWS)
        strncpy(lines[lcount++], cur_line, 63);

    /* Print up to PGN_ROWS lines */
    int start = (lcount > PGN_ROWS) ? lcount - PGN_ROWS : 0;
    for (int i = start; i < lcount && i < PGN_ROWS; i++) {
        move_to(row++, col);
        printf(FG_WHITE "%-30s" RESET, lines[i]);
    }
    /* Clear remaining lines */
    while (row < SIDE_ROW + 32) {
        move_to(row++, col);
        printf("%-32s", "");
    }

    /* Status message */
    move_to(SIDE_ROW + 32, col);
    printf(BOLD FG_BRIGHT_RED "%-32s" RESET, g_status_msg);

    /* Controls */
    int crow = SIDE_ROW + 34;
    move_to(crow++, col);
    printf(BOLD FG_BRIGHT_CYAN "── Controls ──────────────────" RESET);
    move_to(crow++, col);
    printf(FG_WHITE "Arrows" FG_YELLOW ":Move cursor  "
           FG_WHITE "Enter" FG_YELLOW ":Select" RESET);
    move_to(crow++, col);
    printf(FG_WHITE "U" FG_YELLOW ":Undo  "
           FG_WHITE "T" FG_YELLOW ":Time ctrl  "
           FG_WHITE "F" FG_YELLOW ":Flip" RESET);
    move_to(crow++, col);
    printf(FG_WHITE "R" FG_YELLOW ":Restart  "
           FG_WHITE "Q" FG_YELLOW ":Quit" RESET);
}

static void render_all(void) {
    /* Don't clear screen - redraw in place */
    render_board();
    render_sidebar();
    fflush(stdout);
}

/* ─── Terminal input ────────────────────────────────────────────────────── */
static void set_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &g_old_termios);
    struct termios raw = g_old_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1; /* 100ms timeout */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old_termios);
}

static void cleanup(void) {
    restore_terminal();
    engine_stop();
    move_to(45, 1);
    printf(CURSOR_SHOW "\n");
    fflush(stdout);
}

static char getch_raw(void) {
    char c = 0;
    read(STDIN_FILENO, &c, 1);
    return c;
}

/* ─── Engine I/O ────────────────────────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    g_sigchld = 1;
}

static void engine_start(void) {
    if (!g_engine_mode) return;
    if (pipe(g_engine.to_engine) < 0 ||
        pipe(g_engine.from_engine) < 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), "Pipe error: %s", strerror(errno));
        return;
    }
    g_engine.pid = fork();
    if (g_engine.pid < 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), "Fork error: %s", strerror(errno));
        return;
    }
    if (g_engine.pid == 0) {
        /* Child */
        dup2(g_engine.to_engine[0], STDIN_FILENO);
        dup2(g_engine.from_engine[1], STDOUT_FILENO);
        close(g_engine.to_engine[1]);
        close(g_engine.from_engine[0]);
        execl(g_engine_path, g_engine_path, NULL);
        exit(1);
    }
    close(g_engine.to_engine[0]);
    close(g_engine.from_engine[1]);
    /* Non-blocking read */
    fcntl(g_engine.from_engine[0], F_SETFL, O_NONBLOCK);
    g_engine.running = 1;
    g_engine.uci_ok  = 0;
    g_engine.thinking = 0;

    signal(SIGCHLD, sigchld_handler);

    /* Send UCI */
    engine_send("uci\n");
}

static void engine_send(const char *fmt, ...) {
    if (!g_engine.running) return;
    char buf[MAX_ENGINE_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(g_engine.to_engine[1], buf, strlen(buf));
}

/* Build position string from history */
static void engine_send_position(void) {
    char cmd[4096];
    strcpy(cmd, "position startpos");
    if (g_state.hist_count > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < g_state.hist_count; i++) {
            strcat(cmd, " ");
            strcat(cmd, g_state.history[i].uci);
        }
    }
    strcat(cmd, "\n");
    engine_send("%s", cmd);
}

static void engine_go(void) {
    if (!g_engine.running || !g_engine.uci_ok) return;
    engine_send_position();
    switch (g_tc.mode) {
    case TC_TIME:
        engine_send("go movetime %d\n", g_tc.time_ms);
        break;
    case TC_DEPTH:
        engine_send("go depth %d\n", g_tc.depth);
        break;
    case TC_NODES:
        engine_send("go nodes %ld\n", g_tc.nodes);
        break;
    }
    g_engine.thinking = 1;
    g_engine.best_move[0] = '\0';
}

static void engine_process_output(void) {
    if (!g_engine.running) return;
    char line[MAX_ENGINE_LINE];
    int  pos = 0;
    char c;
    /* Read all available data */
    while (read(g_engine.from_engine[0], &c, 1) == 1) {
        if (c == '\n') {
            line[pos] = '\0';
            pos = 0;
            /* Parse line */
            if (strncmp(line, "uciok", 5) == 0) {
                g_engine.uci_ok = 1;
                engine_send("isready\n");
            } else if (strncmp(line, "id name ", 8) == 0) {
                strncpy(g_engine.name, line + 8, sizeof(g_engine.name) - 1);
            } else if (strncmp(line, "readyok", 7) == 0) {
                engine_send("ucinewgame\n");
            } else if (strncmp(line, "bestmove ", 9) == 0) {
                g_engine.thinking = 0;
                char *bm = line + 9;
                char *sp = strchr(bm, ' ');
                if (sp) *sp = '\0';
                strncpy(g_engine.best_move, bm, sizeof(g_engine.best_move) - 1);
            } else if (strncmp(line, "info ", 5) == 0) {
                /* Extract depth/score for status */
                char *dp = strstr(line, "depth ");
                char *sc = strstr(line, "score cp ");
                if (dp && sc) {
                    int depth, score;
                    sscanf(dp + 6, "%d", &depth);
                    sscanf(sc + 9, "%d", &score);
                    snprintf(g_status_msg, sizeof(g_status_msg),
                             "Engine: d%d score %+.2f", depth, score / 100.0);
                }
            }
        } else {
            if (pos < MAX_ENGINE_LINE - 1)
                line[pos++] = c;
        }
    }
}

static void engine_stop(void) {
    if (!g_engine.running) return;
    engine_send("quit\n");
    usleep(50000);
    kill(g_engine.pid, SIGTERM);
    waitpid(g_engine.pid, NULL, WNOHANG);
    close(g_engine.to_engine[1]);
    close(g_engine.from_engine[0]);
    g_engine.running = 0;
}

/* ─── Apply engine best move ────────────────────────────────────────────── */
static void apply_engine_move(void) {
    if (!g_engine.best_move[0]) return;
    char *bm = g_engine.best_move;
    int from = str_to_sq(bm);
    int to   = str_to_sq(bm + 2);
    if (from < 0 || to < 0) return;
    int promo = EMPTY;
    if (bm[4]) {
        switch (tolower(bm[4])) {
        case 'q': promo = (g_state.side == WHITE) ? WQ : BQ; break;
        case 'r': promo = (g_state.side == WHITE) ? WR : BR; break;
        case 'b': promo = (g_state.side == WHITE) ? WB : BB; break;
        case 'n': promo = (g_state.side == WHITE) ? WN : BN; break;
        }
    }
    int idx = find_move(from, to, promo);
    if (idx >= 0) {
        build_san(&g_state, &g_legal_moves[idx]);
        int fm = g_state.fullmove;
        int sd = g_state.side;
        make_move(&g_state, &g_legal_moves[idx]);
        pgn_append_move(&g_state.history[g_state.hist_count - 1], fm, sd);
        compute_legal_moves();
        if (g_legal_count == 0) {
            if (g_state.in_check)
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "Checkmate! %s wins!", g_state.side == WHITE ? "Black" : "White");
            else
                snprintf(g_status_msg, sizeof(g_status_msg), "Stalemate! Draw.");
            g_game_over = 1;
        }
        g_engine.best_move[0] = '\0';
    }
}

/* ─── Promotion dialog ──────────────────────────────────────────────────── */
static int promotion_dialog(int side) {
    /* Simple: draw a small menu */
    int row = 20, col = SIDE_COL;
    move_to(row++, col);
    printf(BOLD FG_BRIGHT_YELLOW "Promote pawn to:" RESET);
    move_to(row++, col);
    printf(FG_WHITE "1" FG_YELLOW "=Queen  "
           FG_WHITE "2" FG_YELLOW "=Rook" RESET);
    move_to(row++, col);
    printf(FG_WHITE "3" FG_YELLOW "=Bishop  "
           FG_WHITE "4" FG_YELLOW "=Knight" RESET);
    fflush(stdout);
    char c;
    while (1) {
        c = getch_raw();
        if (c == '1') return (side == WHITE) ? WQ : BQ;
        if (c == '2') return (side == WHITE) ? WR : BR;
        if (c == '3') return (side == WHITE) ? WB : BB;
        if (c == '4') return (side == WHITE) ? WN : BN;
    }
}

/* ─── User move handling ────────────────────────────────────────────────── */
static void handle_select(void) {
    int sq = sq_of(g_cursor_rank, g_cursor_file);
    int piece = g_state.board[sq];

    if (g_selected_sq < 0) {
        /* Select a piece */
        if (piece && piece_color(piece) == g_state.side) {
            /* Check if piece has any legal moves */
            int has_move = 0;
            for (int i = 0; i < g_legal_count; i++)
                if (g_legal_moves[i].from_sq == sq) { has_move = 1; break; }
            if (has_move) {
                g_selected_sq = sq;
                compute_legal_moves();
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "Selected %s at %c%d",
                         piece_chars[piece],
                         'a' + file_of(sq), rank_of(sq) + 1);
            } else {
                snprintf(g_status_msg, sizeof(g_status_msg), "No legal moves for that piece.");
            }
        } else if (piece) {
            snprintf(g_status_msg, sizeof(g_status_msg), "Not your piece.");
        }
    } else {
        /* Try to move */
        if (sq == g_selected_sq) {
            /* Deselect */
            g_selected_sq = -1;
            compute_legal_moves();
            g_status_msg[0] = '\0';
            return;
        }
        /* Check if target is a legal destination */
        if (g_legal_targets[sq]) {
            int promo = EMPTY;
            int p = g_state.board[g_selected_sq];
            if ((p == WP && rank_of(sq) == 7) ||
                (p == BP && rank_of(sq) == 0)) {
                promo = promotion_dialog(g_state.side);
            }
            int idx = find_move(g_selected_sq, sq, promo);
            if (idx >= 0) {
                int fm = g_state.fullmove;
                int sd = g_state.side;
                build_san(&g_state, &g_legal_moves[idx]);
                make_move(&g_state, &g_legal_moves[idx]);
                pgn_append_move(&g_state.history[g_state.hist_count - 1], fm, sd);
                g_selected_sq = -1;
                compute_legal_moves();

                if (g_legal_count == 0) {
                    if (g_state.in_check)
                        snprintf(g_status_msg, sizeof(g_status_msg),
                                 "Checkmate! %s wins!",
                                 g_state.side == WHITE ? "Black" : "White");
                    else
                        snprintf(g_status_msg, sizeof(g_status_msg), "Stalemate! Draw.");
                    g_game_over = 1;
                } else {
                    snprintf(g_status_msg, sizeof(g_status_msg),
                             "Played: %s", g_state.history[g_state.hist_count-1].san);
                    if (g_state.in_check)
                        strncat(g_status_msg, " Check!",
                                sizeof(g_status_msg) - strlen(g_status_msg) - 1);
                    /* Trigger engine if engine mode and it's engine's turn */
                    if (g_engine_mode && !g_game_over &&
                        g_state.side != g_human_side) {
                        engine_go();
                    }
                }
            }
        } else if (piece && piece_color(piece) == g_state.side) {
            /* Re-select different piece */
            g_selected_sq = sq;
            compute_legal_moves();
            snprintf(g_status_msg, sizeof(g_status_msg), "Selected %s", piece_chars[piece]);
        } else {
            snprintf(g_status_msg, sizeof(g_status_msg), "Illegal move.");
            g_selected_sq = -1;
            compute_legal_moves();
        }
    }
}

/* ─── Undo ──────────────────────────────────────────────────────────────── */
static void undo_move(void) {
    if (g_state.hist_count == 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), "Nothing to undo.");
        return;
    }
    /* If engine is thinking, stop it */
    if (g_engine.thinking) {
        engine_send("stop\n");
        g_engine.thinking = 0;
    }
    unmake_move(&g_state);
    /* If engine mode, undo engine's move too */
    if (g_engine_mode && g_state.hist_count > 0 &&
        g_state.side != g_human_side) {
        unmake_move(&g_state);
    }
    /* Rebuild PGN from history */
    g_pgn[0] = '\0'; g_pgn_len = 0;
    /* We store moves in history; rebuild PGN */
    int save_hist = g_state.hist_count;
    Move save_moves[MAX_HISTORY];
    memcpy(save_moves, g_state.history, save_hist * sizeof(Move));
    /* We need to replay from start to rebuild PGN accurately */
    /* Simple approach: iterate saved history */
    int cur_side = WHITE;
    int cur_fm = 1;
    for (int i = 0; i < save_hist; i++) {
        if (cur_side == WHITE)
            snprintf(g_pgn + g_pgn_len, MAX_PGN_LEN - g_pgn_len,
                     "%d. %s ", cur_fm, save_moves[i].san);
        else {
            snprintf(g_pgn + g_pgn_len, MAX_PGN_LEN - g_pgn_len,
                     "%s ", save_moves[i].san);
            cur_fm++;
        }
        g_pgn_len = (int)strlen(g_pgn);
        cur_side = 1 - cur_side;
    }

    g_selected_sq = -1;
    g_game_over   = 0;
    compute_legal_moves();
    snprintf(g_status_msg, sizeof(g_status_msg), "Move undone.");
}

/* ─── Restart ───────────────────────────────────────────────────────────── */
static void restart_game(void) {
    if (g_engine.thinking) {
        engine_send("stop\n");
        g_engine.thinking = 0;
    }
    init_board();
    compute_legal_moves();
    if (g_engine_mode && g_engine.uci_ok)
        engine_send("ucinewgame\n");
    snprintf(g_status_msg, sizeof(g_status_msg), "New game started.");
    if (g_engine_mode && g_state.side != g_human_side)
        engine_go();
}

/* ─── Time control menu ─────────────────────────────────────────────────── */
static void tc_menu(void) {
    /* Draw over sidebar area */
    restore_terminal();
    printf(CURSOR_SHOW);
    int row = 20, col = SIDE_COL;
    move_to(row++, col); printf(BOLD FG_BRIGHT_YELLOW "Time Control Settings:" RESET "           ");
    move_to(row++, col); printf(FG_WHITE "1" FG_YELLOW ") Time (ms per move): " FG_WHITE "%d" RESET "  ", g_tc.time_ms);
    move_to(row++, col); printf(FG_WHITE "2" FG_YELLOW ") Depth: " FG_WHITE "%d" RESET "            ", g_tc.depth);
    move_to(row++, col); printf(FG_WHITE "3" FG_YELLOW ") Nodes: " FG_WHITE "%ld" RESET "           ", g_tc.nodes);
    move_to(row++, col); printf(FG_WHITE "4" FG_YELLOW ") Cancel" RESET "                      ");
    move_to(row++, col); printf(FG_BRIGHT_GREEN "Choice: " RESET);
    fflush(stdout);

    /* Temporary: read a line */
    char input[64];
    int orig_flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, orig_flags & ~O_NONBLOCK);
    struct termios cooked = g_old_termios;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &cooked);

    if (fgets(input, sizeof(input), stdin)) {
        int choice = atoi(input);
        if (choice == 1) {
            move_to(row, col);
            printf(FG_BRIGHT_GREEN "Time (ms): " RESET);
            fflush(stdout);
            if (fgets(input, sizeof(input), stdin))
                g_tc.time_ms = atoi(input);
            if (g_tc.time_ms < 100) g_tc.time_ms = 100;
            g_tc.mode = TC_TIME;
        } else if (choice == 2) {
            move_to(row, col);
            printf(FG_BRIGHT_GREEN "Depth: " RESET);
            fflush(stdout);
            if (fgets(input, sizeof(input), stdin))
                g_tc.depth = atoi(input);
            if (g_tc.depth < 1) g_tc.depth = 1;
            if (g_tc.depth > 30) g_tc.depth = 30;
            g_tc.mode = TC_DEPTH;
        } else if (choice == 3) {
            move_to(row, col);
            printf(FG_BRIGHT_GREEN "Nodes: " RESET);
            fflush(stdout);
            if (fgets(input, sizeof(input), stdin))
                g_tc.nodes = atol(input);
            if (g_tc.nodes < 1000) g_tc.nodes = 1000;
            g_tc.mode = TC_NODES;
        }
    }

    set_raw_mode();
    printf(CURSOR_HIDE);
    snprintf(g_status_msg, sizeof(g_status_msg), "Time control updated.");
}

/* ─── Main loop ─────────────────────────────────────────────────────────── */
static int cursor_to_sq(void) {
    return sq_of(g_cursor_rank, g_cursor_file);
}

int main(int argc, char *argv[]) {
    /* Parse arguments */
    if (argc >= 2) {
        g_engine_mode = 1;
        strncpy(g_engine_path, argv[1], sizeof(g_engine_path) - 1);
    }
    if (argc >= 3) {
        /* Optional: --black to play as black */
        if (strcmp(argv[2], "--black") == 0)
            g_human_side = BLACK;
    }

    init_board();
    compute_legal_moves();

    set_raw_mode();
    printf(CLEAR_SCREEN CURSOR_HIDE);
    fflush(stdout);

    /* Start engine */
    if (g_engine_mode) {
        engine_start();
        /* Wait a moment for uciok */
        struct timespec ts = {0, 200000000L};
        nanosleep(&ts, NULL);
        engine_process_output();
        if (g_engine.uci_ok)
            engine_send("isready\n");
        nanosleep(&ts, NULL);
        engine_process_output();
        /* If engine plays white */
        if (g_engine_mode && g_state.side != g_human_side && g_engine.uci_ok)
            engine_go();
    }

    /* Initial render */
    render_all();

    while (1) {
        /* Process engine output */
        if (g_engine_mode && g_engine.running) {
            engine_process_output();
            if (!g_game_over && g_engine.best_move[0] &&
                g_state.side != g_human_side) {
                apply_engine_move();
                render_all();
                if (!g_game_over && g_engine_mode &&
                    g_state.side == g_human_side) {
                    /* Human to move */
                }
            }
        }

        /* Check sigchld */
        if (g_sigchld) {
            g_sigchld = 0;
            int status;
            if (waitpid(g_engine.pid, &status, WNOHANG) > 0) {
                g_engine.running = 0;
                snprintf(g_status_msg, sizeof(g_status_msg), "Engine process ended.");
                render_all();
            }
        }

        /* Read keyboard input (non-blocking) */
        char c = getch_raw();
        if (c == 0) continue; /* timeout, loop */

        if (c == 'q' || c == 'Q') {
            break;
        }

        if (c == 'r' || c == 'R') {
            restart_game();
            render_all();
            continue;
        }

        if (c == 'f' || c == 'F') {
            g_flipped = !g_flipped;
            render_all();
            continue;
        }

        if (c == 'u' || c == 'U') {
            undo_move();
            render_all();
            continue;
        }

        if (c == 't' || c == 'T') {
            tc_menu();
            render_all();
            continue;
        }

        /* Arrow keys: ESC [ A/B/C/D */
        if (c == '\033') {
            char c2 = getch_raw();
            if (c2 == '[') {
                char c3 = getch_raw();
                int moved = 0;
                if (c3 == 'A') { /* Up */
                    if (!g_flipped) {
                        if (g_cursor_rank < 7) { g_cursor_rank++; moved = 1; }
                    } else {
                        if (g_cursor_rank > 0) { g_cursor_rank--; moved = 1; }
                    }
                } else if (c3 == 'B') { /* Down */
                    if (!g_flipped) {
                        if (g_cursor_rank > 0) { g_cursor_rank--; moved = 1; }
                    } else {
                        if (g_cursor_rank < 7) { g_cursor_rank++; moved = 1; }
                    }
                } else if (c3 == 'C') { /* Right */
                    if (!g_flipped) {
                        if (g_cursor_file < 7) { g_cursor_file++; moved = 1; }
                    } else {
                        if (g_cursor_file > 0) { g_cursor_file--; moved = 1; }
                    }
                } else if (c3 == 'D') { /* Left */
                    if (!g_flipped) {
                        if (g_cursor_file > 0) { g_cursor_file--; moved = 1; }
                    } else {
                        if (g_cursor_file < 7) { g_cursor_file++; moved = 1; }
                    }
                }
                if (moved) render_all();
            }
            continue;
        }

        /* Enter or Space: select/move */
        if (c == '\r' || c == '\n' || c == ' ') {
            if (!g_game_over) {
                if (g_engine_mode && g_state.side != g_human_side) {
                    snprintf(g_status_msg, sizeof(g_status_msg),
                             "Engine is thinking...");
                } else {
                    handle_select();
                }
                render_all();
            }
            continue;
        }

        /* HJKL vim keys */
        if (c == 'h' || c == 'H') {
            if (!g_flipped) { if (g_cursor_file > 0) g_cursor_file--; }
            else { if (g_cursor_file < 7) g_cursor_file++; }
            render_all(); continue;
        }
        if (c == 'l' || c == 'L') {
            if (!g_flipped) { if (g_cursor_file < 7) g_cursor_file++; }
            else { if (g_cursor_file > 0) g_cursor_file--; }
            render_all(); continue;
        }
        if (c == 'k' || c == 'K') {
            if (!g_flipped) { if (g_cursor_rank < 7) g_cursor_rank++; }
            else { if (g_cursor_rank > 0) g_cursor_rank--; }
            render_all(); continue;
        }
        if (c == 'j' || c == 'J') {
            if (!g_flipped) { if (g_cursor_rank > 0) g_cursor_rank--; }
            else { if (g_cursor_rank < 7) g_cursor_rank++; }
            render_all(); continue;
        }

        /* Square jump by typing file letter */
        if (c >= 'a' && c <= 'h') {
            g_cursor_file = c - 'a';
            render_all(); continue;
        }
        if (c >= '1' && c <= '8') {
            g_cursor_rank = c - '1';
            render_all(); continue;
        }
    }

    /* Cleanup */
    printf(CLEAR_SCREEN CURSOR_HOME CURSOR_SHOW RESET);
    cleanup();
    printf("Thanks for playing!\n");
    return 0;
}
