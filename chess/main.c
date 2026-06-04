/*
 * chess_tui.c - Terminal Chess GUI with UCI Engine Support
 * Compile: gcc -o chess_tui chess_tui.c
 * Run: ./chess_tui
 *
 * Features:
 *  - Unicode chess pieces rendered in-place (no flicker)
 *  - UCI engine integration via pipes
 *  - Legal move enforcement
 *  - Move highlighting, check highlighting (red king)
 *  - Move takeback (undo)
 *  - PGN move list display
 *  - Configurable time controls (time/depth/nodes)
 *  - Keyboard navigation
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

/* ─────────────────────────────────────────────────────────────────────────── */
/*  CONSTANTS & MACROS                                                         */
/* ─────────────────────────────────────────────────────────────────────────── */

#define BOARD_SIZE      8
#define MAX_MOVES       512
#define MAX_PGN_LEN     8192
#define MAX_LEGAL       256
#define ENGINE_BUF      4096
#define MAX_HISTORY     512

/* Piece codes (positive = white, negative = black, 0 = empty) */
#define EMPTY   0
#define PAWN    1
#define KNIGHT  2
#define BISHOP  3
#define ROOK    4
#define QUEEN   5
#define KING    6

/* Colors */
#define WHITE_SIDE  1
#define BLACK_SIDE -1

/* Terminal color codes */
#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define DIM         "\033[2m"

/* 256-color backgrounds */
#define BG_LIGHT_SQ     "\033[48;5;229m"   /* cream            */
#define BG_DARK_SQ      "\033[48;5;94m"    /* brown            */
#define BG_SELECTED     "\033[48;5;226m"   /* bright yellow    */
#define BG_LEGAL        "\033[48;5;148m"   /* yellow-green     */
#define BG_LAST_MOVE    "\033[48;5;215m"   /* orange           */
#define BG_CHECK        "\033[48;5;196m"   /* red              */
#define BG_CURSOR       "\033[48;5;51m"    /* cyan             */
#define BG_CAPTURE      "\033[48;5;202m"   /* deep orange      */

/* Foreground piece colors */
#define FG_WHITE_PIECE  "\033[38;5;255m"   /* white            */
#define FG_BLACK_PIECE  "\033[38;5;232m"   /* near-black       */

/* UI panel colors */
#define FG_TITLE        "\033[38;5;220m"
#define FG_HEADER       "\033[38;5;75m"
#define FG_INFO         "\033[38;5;252m"
#define FG_MOVE_W       "\033[38;5;255m"
#define FG_MOVE_B       "\033[38;5;245m"
#define FG_HIGHLIGHT    "\033[38;5;226m"
#define FG_ERROR        "\033[38;5;196m"
#define FG_SUCCESS      "\033[38;5;82m"
#define FG_COORD        "\033[38;5;240m"

/* Unicode chess pieces */
static const char *PIECE_UNICODE[7][2] = {
    /*        WHITE         BLACK  */
    /* 0 */ { "  ",        "  "        },
    /* P */ { "\u265F ",   "\u265F "   },
    /* N */ { "\u265E ",   "\u265E "   },
    /* B */ { "\u265D ",   "\u265D "   },
    /* R */ { "\u265C ",   "\u265C "   },
    /* Q */ { "\u265B ",   "\u265B "   },
    /* K */ { "\u265A ",   "\u265A "   },
};

/* ─────────────────────────────────────────────────────────────────────────── */
/*  DATA STRUCTURES                                                            */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    int from_r, from_c;
    int to_r,   to_c;
    int piece;          /* piece that moved */
    int captured;       /* piece captured (0 if none) */
    int promotion;      /* promotion piece (0 if none) */
    int flags;          /* special flags */
    /* castling rights before this move */
    int prev_castle_wk, prev_castle_wq;
    int prev_castle_bk, prev_castle_bq;
    int prev_ep_col;    /* en passant column before move (-1 if none) */
    int prev_half_move; /* half-move clock before move */
    char pgn[16];       /* PGN notation for this move */
} Move;

#define FLAG_EP         0x01   /* en passant capture */
#define FLAG_CASTLE_K   0x02   /* kingside castle */
#define FLAG_CASTLE_Q   0x04   /* queenside castle */
#define FLAG_PROMO      0x08   /* promotion */

typedef struct {
    int board[8][8];    /* board[row][col]: positive=white, negative=black */
    int side;           /* whose turn: WHITE_SIDE or BLACK_SIDE */
    int ep_col;         /* en passant target column (-1 if none) */
    int ep_row;         /* en passant target row */
    int castle_wk;      /* white can castle kingside */
    int castle_wq;      /* white can castle queenside */
    int castle_bk;      /* black can castle kingside */
    int castle_bq;      /* black can castle queenside */
    int half_move;      /* half-move clock (50-move rule) */
    int full_move;      /* full move counter */

    /* UI state */
    int cursor_r, cursor_c;
    int selected_r, selected_c;
    int has_selection;
    int last_from_r, last_from_c;
    int last_to_r,   last_to_c;
    int has_last_move;

    /* History */
    Move history[MAX_HISTORY];
    int  hist_count;

    /* Legal moves cache for selected piece */
    int legal_to_r[MAX_LEGAL];
    int legal_to_c[MAX_LEGAL];
    int legal_count;

    /* PGN string */
    char pgn[MAX_PGN_LEN];
    int  pgn_len;

    /* Game state */
    int  in_check;
    int  king_r, king_c;   /* checked king position */
    int  game_over;
    char result[16];       /* "1-0", "0-1", "1/2-1/2" */
    char status_msg[128];
} GameState;

typedef enum {
    TC_TIME = 0,
    TC_DEPTH,
    TC_NODES
} TCMode;

typedef struct {
    TCMode mode;
    int    time_ms;    /* milliseconds per move */
    int    depth;      /* search depth */
    long   nodes;      /* node count */
    /* clock */
    int    wtime, btime;  /* ms remaining */
    int    winc,  binc;   /* ms increment */
} TimeControl;

typedef struct {
    pid_t  pid;
    int    in_fd;   /* write to engine */
    int    out_fd;  /* read from engine */
    char   buf[ENGINE_BUF];
    int    buf_len;
    int    ready;
    int    thinking;
    char   best_move[16];
    int    got_move;
    char   name[128];
} Engine;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  GLOBALS                                                                    */
/* ─────────────────────────────────────────────────────────────────────────── */

static struct termios g_orig_termios;
static GameState      g_state;
static Engine         g_engine;
static TimeControl    g_tc;
static int            g_engine_side = BLACK_SIDE;  /* engine plays black */
static int            g_use_engine  = 0;
static int            g_running     = 1;

/* Promotion choice for pawn promotion UI */
static int g_promo_pending = 0;
static int g_promo_from_r, g_promo_from_c;
static int g_promo_to_r,   g_promo_to_c;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  TERMINAL HANDLING                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

static void term_raw(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;  /* 100ms timeout */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
}

static void cursor_hide(void)  { printf("\033[?25l"); }
static void cursor_show(void)  { printf("\033[?25h"); }
static void cursor_goto(int r, int c) { printf("\033[%d;%Hf", r, c); }
static void screen_clear(void) { printf("\033[2J\033[H"); }
static void screen_alt(void)   { printf("\033[?1049h"); }
static void screen_main(void)  { printf("\033[?1049l"); }

static void cleanup(void) {
    cursor_show();
    screen_main();
    term_restore();
}

static void sig_handler(int sig) {
    (void)sig;
    cleanup();
    exit(0);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  CHESS LOGIC - MOVE GENERATION                                              */
/* ─────────────────────────────────────────────────────────────────────────── */

static int in_bounds(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

static int piece_color(int p) {
    if (p > 0) return WHITE_SIDE;
    if (p < 0) return BLACK_SIDE;
    return 0;
}

static int abs_piece(int p) {
    return p < 0 ? -p : p;
}

/* Find king position for given side */
static void find_king(const GameState *gs, int side, int *kr, int *kc) {
    int target = side * KING;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (gs->board[r][c] == target) { *kr = r; *kc = c; return; }
    *kr = -1; *kc = -1;
}

/* Check if square (r,c) is attacked by given side */
static int is_attacked(const GameState *gs, int r, int c, int by_side) {
    /* Pawns */
    int pdr = (by_side == WHITE_SIDE) ? 1 : -1;  /* direction pawns attack */
    int pr = r + pdr;
    if (in_bounds(pr, c-1) && gs->board[pr][c-1] == by_side * PAWN) return 1;
    if (in_bounds(pr, c+1) && gs->board[pr][c+1] == by_side * PAWN) return 1;

    /* Knights */
    static const int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn[i][0], nc = c + kn[i][1];
        if (in_bounds(nr,nc) && gs->board[nr][nc] == by_side * KNIGHT) return 1;
    }

    /* Bishops & Queens (diagonals) */
    static const int diag[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int d = 0; d < 4; d++) {
        for (int dist = 1; dist < 8; dist++) {
            int nr = r + diag[d][0]*dist, nc = c + diag[d][1]*dist;
            if (!in_bounds(nr,nc)) break;
            int p = gs->board[nr][nc];
            if (p != EMPTY) {
                int ap = abs_piece(p);
                if (piece_color(p) == by_side && (ap == BISHOP || ap == QUEEN)) return 1;
                break;
            }
        }
    }

    /* Rooks & Queens (straights) */
    static const int straight[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int d = 0; d < 4; d++) {
        for (int dist = 1; dist < 8; dist++) {
            int nr = r + straight[d][0]*dist, nc = c + straight[d][1]*dist;
            if (!in_bounds(nr,nc)) break;
            int p = gs->board[nr][nc];
            if (p != EMPTY) {
                int ap = abs_piece(p);
                if (piece_color(p) == by_side && (ap == ROOK || ap == QUEEN)) return 1;
                break;
            }
        }
    }

    /* King */
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            int nr = r+dr, nc = c+dc;
            if (in_bounds(nr,nc) && gs->board[nr][nc] == by_side * KING) return 1;
        }

    return 0;
}

static int in_check_side(const GameState *gs, int side) {
    int kr, kc;
    find_king(gs, side, &kr, &kc);
    if (kr < 0) return 0;
    return is_attacked(gs, kr, kc, -side);
}

/* Apply move to a temporary board state (no legality check) */
static void apply_move_raw(GameState *gs, int fr, int fc, int tr, int tc,
                            int promo, int flags) {
    int piece = gs->board[fr][fc];
    int side  = piece_color(piece);
    int ap    = abs_piece(piece);

    gs->board[fr][fc] = EMPTY;

    /* En passant capture */
    if (flags & FLAG_EP) {
        gs->board[fr][tc] = EMPTY;  /* captured pawn is on same row as from */
    }

    /* Promotion */
    if ((flags & FLAG_PROMO) && promo) {
        gs->board[tr][tc] = side * promo;
    } else {
        gs->board[tr][tc] = piece;
    }

    /* Castling: move rook */
    if (flags & FLAG_CASTLE_K) {
        int rook = gs->board[tr][7];
        gs->board[tr][7] = EMPTY;
        gs->board[tr][5] = rook;
    }
    if (flags & FLAG_CASTLE_Q) {
        int rook = gs->board[tr][0];
        gs->board[tr][0] = EMPTY;
        gs->board[tr][3] = rook;
    }

    /* Update castling rights */
    if (ap == KING) {
        if (side == WHITE_SIDE) { gs->castle_wk = 0; gs->castle_wq = 0; }
        else                    { gs->castle_bk = 0; gs->castle_bq = 0; }
    }
    if (ap == ROOK) {
        if (side == WHITE_SIDE) {
            if (fc == 7) gs->castle_wk = 0;
            if (fc == 0) gs->castle_wq = 0;
        } else {
            if (fc == 7) gs->castle_bk = 0;
            if (fc == 0) gs->castle_bq = 0;
        }
    }
    /* If rook on target square is captured */
    if (tr == 0 && tc == 0) gs->castle_bq = 0;
    if (tr == 0 && tc == 7) gs->castle_bk = 0;
    if (tr == 7 && tc == 0) gs->castle_wq = 0;
    if (tr == 7 && tc == 7) gs->castle_wk = 0;

    /* Update en passant */
    gs->ep_col = -1;
    if (ap == PAWN && abs(tr - fr) == 2) {
        gs->ep_col = fc;
        gs->ep_row = (fr + tr) / 2;
    }

    gs->side = -gs->side;
}

/* Generate pseudo-legal moves for a piece, then filter by legality */
typedef struct { int r, c, flags, promo; } RawMove;

static int gen_pseudo(const GameState *gs, int fr, int fc,
                       RawMove *out, int max_out) {
    int n = 0;
    int piece = gs->board[fr][fc];
    if (!piece) return 0;
    int side  = piece_color(piece);
    int ap    = abs_piece(piece);

#define ADD(R,C,FL,PR) do { if(n<max_out){ out[n].r=(R); out[n].c=(C); out[n].flags=(FL); out[n].promo=(PR); n++;} } while(0)

    switch (ap) {
    case PAWN: {
        int dir = side;  /* WHITE_SIDE=1 moves toward row 0, BLACK=-1 toward row 7 */
        /* White pawns start at row 6, move toward row 0 (decreasing row) */
        /* Black pawns start at row 1, move toward row 7 (increasing row) */
        int fwd = fr - dir;  /* forward for white: row decreases; black: increases */
        int start_row = (side == WHITE_SIDE) ? 6 : 1;
        int promo_row = (side == WHITE_SIDE) ? 0 : 7;

        /* Single push */
        if (in_bounds(fwd, fc) && gs->board[fwd][fc] == EMPTY) {
            if (fwd == promo_row) {
                ADD(fwd, fc, FLAG_PROMO, QUEEN);
                ADD(fwd, fc, FLAG_PROMO, ROOK);
                ADD(fwd, fc, FLAG_PROMO, BISHOP);
                ADD(fwd, fc, FLAG_PROMO, KNIGHT);
            } else {
                ADD(fwd, fc, 0, 0);
                /* Double push */
                if (fr == start_row && gs->board[fwd-dir][fc] == EMPTY)
                    ADD(fwd-dir, fc, 0, 0);
            }
        }

        /* Captures */
        for (int dc = -1; dc <= 1; dc += 2) {
            int tc = fc + dc;
            if (!in_bounds(fwd, tc)) continue;
            int target = gs->board[fwd][tc];
            if (target != EMPTY && piece_color(target) == -side) {
                if (fwd == promo_row) {
                    ADD(fwd, tc, FLAG_PROMO, QUEEN);
                    ADD(fwd, tc, FLAG_PROMO, ROOK);
                    ADD(fwd, tc, FLAG_PROMO, BISHOP);
                    ADD(fwd, tc, FLAG_PROMO, KNIGHT);
                } else {
                    ADD(fwd, tc, 0, 0);
                }
            }
            /* En passant */
            if (gs->ep_col == tc && gs->ep_row == fwd) {
                ADD(fwd, tc, FLAG_EP, 0);
            }
        }
        break;
    }
    case KNIGHT: {
        static const int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = fr+kn[i][0], nc = fc+kn[i][1];
            if (in_bounds(nr,nc) && piece_color(gs->board[nr][nc]) != side)
                ADD(nr,nc,0,0);
        }
        break;
    }
    case BISHOP: {
        static const int d[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
        for (int i = 0; i < 4; i++)
            for (int dist = 1; dist < 8; dist++) {
                int nr = fr+d[i][0]*dist, nc = fc+d[i][1]*dist;
                if (!in_bounds(nr,nc)) break;
                int t = gs->board[nr][nc];
                if (t == EMPTY) ADD(nr,nc,0,0);
                else { if (piece_color(t) != side) ADD(nr,nc,0,0); break; }
            }
        break;
    }
    case ROOK: {
        static const int d[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
        for (int i = 0; i < 4; i++)
            for (int dist = 1; dist < 8; dist++) {
                int nr = fr+d[i][0]*dist, nc = fc+d[i][1]*dist;
                if (!in_bounds(nr,nc)) break;
                int t = gs->board[nr][nc];
                if (t == EMPTY) ADD(nr,nc,0,0);
                else { if (piece_color(t) != side) ADD(nr,nc,0,0); break; }
            }
        break;
    }
    case QUEEN: {
        static const int d[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
        for (int i = 0; i < 8; i++)
            for (int dist = 1; dist < 8; dist++) {
                int nr = fr+d[i][0]*dist, nc = fc+d[i][1]*dist;
                if (!in_bounds(nr,nc)) break;
                int t = gs->board[nr][nc];
                if (t == EMPTY) ADD(nr,nc,0,0);
                else { if (piece_color(t) != side) ADD(nr,nc,0,0); break; }
            }
        break;
    }
    case KING: {
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++) {
                if (!dr && !dc) continue;
                int nr = fr+dr, nc = fc+dc;
                if (in_bounds(nr,nc) && piece_color(gs->board[nr][nc]) != side)
                    ADD(nr,nc,0,0);
            }
        /* Castling */
        if (side == WHITE_SIDE && fr == 7 && fc == 4) {
            if (gs->castle_wk && gs->board[7][5]==EMPTY && gs->board[7][6]==EMPTY
                && !is_attacked(gs,7,4,BLACK_SIDE)
                && !is_attacked(gs,7,5,BLACK_SIDE)
                && !is_attacked(gs,7,6,BLACK_SIDE))
                ADD(7,6,FLAG_CASTLE_K,0);
            if (gs->castle_wq && gs->board[7][3]==EMPTY && gs->board[7][2]==EMPTY
                && gs->board[7][1]==EMPTY
                && !is_attacked(gs,7,4,BLACK_SIDE)
                && !is_attacked(gs,7,3,BLACK_SIDE)
                && !is_attacked(gs,7,2,BLACK_SIDE))
                ADD(7,2,FLAG_CASTLE_Q,0);
        }
        if (side == BLACK_SIDE && fr == 0 && fc == 4) {
            if (gs->castle_bk && gs->board[0][5]==EMPTY && gs->board[0][6]==EMPTY
                && !is_attacked(gs,0,4,WHITE_SIDE)
                && !is_attacked(gs,0,5,WHITE_SIDE)
                && !is_attacked(gs,0,6,WHITE_SIDE))
                ADD(0,6,FLAG_CASTLE_K,0);
            if (gs->castle_bq && gs->board[0][3]==EMPTY && gs->board[0][2]==EMPTY
                && gs->board[0][1]==EMPTY
                && !is_attacked(gs,0,4,WHITE_SIDE)
                && !is_attacked(gs,0,3,WHITE_SIDE)
                && !is_attacked(gs,0,2,WHITE_SIDE))
                ADD(0,2,FLAG_CASTLE_Q,0);
        }
        break;
    }
    }
#undef ADD
    return n;
}

/* Test if move is legal (doesn't leave own king in check) */
static int is_legal(const GameState *gs, int fr, int fc, int tr, int tc,
                    int flags, int promo) {
    GameState tmp = *gs;
    apply_move_raw(&tmp, fr, fc, tr, tc, promo, flags);
    return !in_check_side(&tmp, gs->side);
}

/* Generate all legal moves for piece at (fr,fc) */
static int legal_moves_for(const GameState *gs, int fr, int fc,
                             int *out_r, int *out_c, int *out_flags,
                             int max_out) {
    RawMove pseudo[128];
    int np = gen_pseudo(gs, fr, fc, pseudo, 128);
    int n = 0;
    for (int i = 0; i < np && n < max_out; i++) {
        if (is_legal(gs, fr, fc, pseudo[i].r, pseudo[i].c,
                     pseudo[i].flags, pseudo[i].promo)) {
            /* Avoid duplicate destination squares (promotions) */
            int dup = 0;
            for (int j = 0; j < n; j++)
                if (out_r[j]==pseudo[i].r && out_c[j]==pseudo[i].c) { dup=1; break; }
            if (!dup) {
                out_r[n] = pseudo[i].r;
                out_c[n] = pseudo[i].c;
                if (out_flags) out_flags[n] = pseudo[i].flags;
                n++;
            }
        }
    }
    return n;
}

/* Check if any legal move exists for side */
static int has_any_legal_move(const GameState *gs, int side) {
    for (int fr = 0; fr < 8; fr++)
        for (int fc = 0; fc < 8; fc++) {
            if (piece_color(gs->board[fr][fc]) != side) continue;
            RawMove pseudo[128];
            int np = gen_pseudo(gs, fr, fc, pseudo, 128);
            for (int i = 0; i < np; i++)
                if (is_legal(gs, fr, fc, pseudo[i].r, pseudo[i].c,
                             pseudo[i].flags, pseudo[i].promo))
                    return 1;
        }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  PGN NOTATION                                                               */
/* ─────────────────────────────────────────────────────────────────────────── */

static const char *PIECE_LETTER = " PNBRQK";
static const char *FILE_LETTER  = "abcdefgh";

/* Build PGN for a move (before applying it) */
static void build_pgn(const GameState *gs, int fr, int fc, int tr, int tc,
                       int flags, int promo, char *out, int out_size) {
    int piece = gs->board[fr][fc];
    int ap    = abs_piece(piece);
    int side  = piece_color(piece);
    char buf[32];
    int  pos = 0;

    /* Castling */
    if (flags & FLAG_CASTLE_K) { snprintf(out, out_size, "O-O"); goto check_suffix; }
    if (flags & FLAG_CASTLE_Q) { snprintf(out, out_size, "O-O-O"); goto check_suffix; }

    /* Piece letter */
    if (ap != PAWN) buf[pos++] = PIECE_LETTER[ap];

    /* Disambiguation */
    if (ap != PAWN) {
        int ambig_file = 0, ambig_rank = 0, ambig = 0;
        for (int r2 = 0; r2 < 8; r2++)
            for (int c2 = 0; c2 < 8; c2++) {
                if (r2==fr && c2==fc) continue;
                if (gs->board[r2][c2] != gs->board[fr][fc]) continue;
                RawMove pm[128];
                int np = gen_pseudo(gs, r2, c2, pm, 128);
                for (int i = 0; i < np; i++) {
                    if (pm[i].r==tr && pm[i].c==tc &&
                        is_legal(gs, r2, c2, pm[i].r, pm[i].c, pm[i].flags, pm[i].promo)) {
                        ambig = 1;
                        if (c2==fc) ambig_rank = 1;
                        else        ambig_file  = 1;
                    }
                }
            }
        if (ambig) {
            if (!ambig_rank || ambig_file) buf[pos++] = FILE_LETTER[fc];
            if (ambig_rank)                buf[pos++] = '1' + (7-fr);
        }
    } else {
        /* Pawn capture: include source file */
        if (fc != tc) buf[pos++] = FILE_LETTER[fc];
    }

    /* Capture */
    if (gs->board[tr][tc] != EMPTY || (flags & FLAG_EP))
        buf[pos++] = 'x';

    /* Destination */
    buf[pos++] = FILE_LETTER[tc];
    buf[pos++] = '1' + (7-tr);

    /* Promotion */
    if (flags & FLAG_PROMO) {
        buf[pos++] = '=';
        buf[pos++] = PIECE_LETTER[promo];
    }

    buf[pos] = '\0';
    snprintf(out, out_size, "%s", buf);

check_suffix:;
    /* Check / checkmate suffix — apply the move and test */
    GameState tmp = *gs;
    /* Find actual flags/promo for this move */
    RawMove pseudo[128];
    int np = gen_pseudo(&tmp, fr, fc, pseudo, 128);
    int actual_flags = flags, actual_promo = promo;
    for (int i = 0; i < np; i++)
        if (pseudo[i].r==tr && pseudo[i].c==tc) {
            actual_flags = pseudo[i].flags;
            if (actual_promo == 0) actual_promo = pseudo[i].promo;
            break;
        }
    apply_move_raw(&tmp, fr, fc, tr, tc, actual_promo, actual_flags);
    int new_side = tmp.side;
    if (in_check_side(&tmp, new_side)) {
        if (!has_any_legal_move(&tmp, new_side)) {
            strncat(out, "#", out_size - strlen(out) - 1);
        } else {
            strncat(out, "+", out_size - strlen(out) - 1);
        }
    }
}

static void append_pgn(GameState *gs, const char *move_pgn) {
    char tmp[64];
    if (gs->hist_count % 2 == 0) {
        /* White's move — add move number */
        snprintf(tmp, sizeof(tmp), "%d. %s ", gs->hist_count/2 + 1, move_pgn);
    } else {
        snprintf(tmp, sizeof(tmp), "%s ", move_pgn);
    }
    int len = strlen(tmp);
    if (gs->pgn_len + len < MAX_PGN_LEN - 1) {
        memcpy(gs->pgn + gs->pgn_len, tmp, len);
        gs->pgn_len += len;
        gs->pgn[gs->pgn_len] = '\0';
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  GAME LOGIC - APPLY MOVE (FULL)                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Get flags and promo for a specific move destination */
static int get_move_flags(const GameState *gs, int fr, int fc, int tr, int tc,
                           int *flags_out, int *promo_out) {
    RawMove pseudo[128];
    int np = gen_pseudo(gs, fr, fc, pseudo, 128);
    for (int i = 0; i < np; i++) {
        if (pseudo[i].r == tr && pseudo[i].c == tc) {
            if (is_legal(gs, fr, fc, tr, tc, pseudo[i].flags, pseudo[i].promo)) {
                *flags_out = pseudo[i].flags;
                *promo_out = pseudo[i].promo;
                return 1;
            }
        }
    }
    return 0;
}

static int do_move(GameState *gs, int fr, int fc, int tr, int tc, int promo) {
    int flags = 0, default_promo = 0;
    /* Find flags */
    RawMove pseudo[128];
    int np = gen_pseudo(gs, fr, fc, pseudo, 128);
    int found = 0;
    for (int i = 0; i < np; i++) {
        if (pseudo[i].r == tr && pseudo[i].c == tc) {
            if (promo && (pseudo[i].flags & FLAG_PROMO) && pseudo[i].promo != promo)
                continue;
            if (is_legal(gs, fr, fc, tr, tc, pseudo[i].flags, pseudo[i].promo)) {
                flags = pseudo[i].flags;
                default_promo = pseudo[i].promo;
                found = 1;
                break;
            }
        }
    }
    if (!found) return 0;
    if (!promo && (flags & FLAG_PROMO)) promo = default_promo;

    /* Build PGN before applying */
    char pgn_str[16];
    build_pgn(gs, fr, fc, tr, tc, flags, promo, pgn_str, sizeof(pgn_str));

    /* Save history */
    Move *m = &gs->history[gs->hist_count];
    m->from_r = fr; m->from_c = fc;
    m->to_r   = tr; m->to_c   = tc;
    m->piece  = gs->board[fr][fc];
    m->captured = (flags & FLAG_EP) ? -gs->side * PAWN : gs->board[tr][tc];
    m->promotion = promo;
    m->flags  = flags;
    m->prev_castle_wk = gs->castle_wk;
    m->prev_castle_wq = gs->castle_wq;
    m->prev_castle_bk = gs->castle_bk;
    m->prev_castle_bq = gs->castle_bq;
    m->prev_ep_col    = gs->ep_col;
    m->prev_half_move = gs->half_move;
    snprintf(m->pgn, sizeof(m->pgn), "%s", pgn_str);
    gs->hist_count++;

    /* Append to PGN string */
    append_pgn(gs, pgn_str);

    /* Half-move clock */
    int ap = abs_piece(gs->board[fr][fc]);
    if (ap == PAWN || gs->board[tr][tc] != EMPTY)
        gs->half_move = 0;
    else
        gs->half_move++;

    if (gs->side == BLACK_SIDE) gs->full_move++;

    /* Apply */
    apply_move_raw(gs, fr, fc, tr, tc, promo, flags);

    /* Update last move */
    gs->last_from_r = fr; gs->last_from_c = fc;
    gs->last_to_r   = tr; gs->last_to_c   = tc;
    gs->has_last_move = 1;

    /* Clear selection */
    gs->has_selection = 0;
    gs->legal_count   = 0;

    /* Check game end */
    int cur_side = gs->side;
    gs->in_check = in_check_side(gs, cur_side);
    if (gs->in_check) {
        find_king(gs, cur_side, &gs->king_r, &gs->king_c);
    }

    if (!has_any_legal_move(gs, cur_side)) {
        gs->game_over = 1;
        if (gs->in_check) {
            snprintf(gs->result, sizeof(gs->result), "%s", (cur_side == WHITE_SIDE) ? "0-1" : "1-0");
            snprintf(gs->status_msg, sizeof(gs->status_msg), "Checkmate! %s wins.",
                     (cur_side == WHITE_SIDE) ? "Black" : "White");
            /* Append result to PGN */
            if (gs->pgn_len + 8 < MAX_PGN_LEN)
                gs->pgn_len += snprintf(gs->pgn + gs->pgn_len, MAX_PGN_LEN - gs->pgn_len,
                                        "%s", gs->result);
        } else {
            snprintf(gs->result, sizeof(gs->result), "1/2-1/2");
            snprintf(gs->status_msg, sizeof(gs->status_msg), "Stalemate! Draw.");
            if (gs->pgn_len + 12 < MAX_PGN_LEN)
                gs->pgn_len += snprintf(gs->pgn + gs->pgn_len, MAX_PGN_LEN - gs->pgn_len,
                                        "1/2-1/2");
        }
    } else if (gs->half_move >= 100) {
        gs->game_over = 1;
        snprintf(gs->result, sizeof(gs->result), "1/2-1/2");
        snprintf(gs->status_msg, sizeof(gs->status_msg), "Draw by 50-move rule.");
    } else {
        snprintf(gs->status_msg, sizeof(gs->status_msg), "%s to move.",
                 gs->side == WHITE_SIDE ? "White" : "Black");
        if (gs->in_check) {
            strncat(gs->status_msg, " Check!", sizeof(gs->status_msg)-strlen(gs->status_msg)-1);
        }
    }

    return 1;
}

/* Undo last move */
static int undo_move(GameState *gs) {
    if (gs->hist_count == 0) return 0;

    gs->hist_count--;
    Move *m = &gs->history[gs->hist_count];

    /* Restore turn (reverse) */
    gs->side = -gs->side;

    /* Restore castling/ep */
    gs->castle_wk = m->prev_castle_wk;
    gs->castle_wq = m->prev_castle_wq;
    gs->castle_bk = m->prev_castle_bk;
    gs->castle_bq = m->prev_castle_bq;
    gs->ep_col    = m->prev_ep_col;
    gs->half_move = m->prev_half_move;

    if (gs->side == BLACK_SIDE) gs->full_move--;

    /* Restore piece */
    int piece = m->piece;
    gs->board[m->from_r][m->from_c] = piece;
    gs->board[m->to_r][m->to_c]     = EMPTY;

    /* Restore captured */
    if (m->flags & FLAG_EP) {
        /* Captured pawn was on from_r, to_c */
        gs->board[m->from_r][m->to_c] = m->captured;
    } else {
        gs->board[m->to_r][m->to_c] = m->captured;
    }

    /* Restore castled rook */
    if (m->flags & FLAG_CASTLE_K) {
        int rook = gs->board[m->to_r][5];
        gs->board[m->to_r][5] = EMPTY;
        gs->board[m->to_r][7] = rook;
    }
    if (m->flags & FLAG_CASTLE_Q) {
        int rook = gs->board[m->to_r][3];
        gs->board[m->to_r][3] = EMPTY;
        gs->board[m->to_r][0] = rook;
    }

    /* Restore promotion: piece at from is already original pawn */

    /* Rebuild PGN from scratch */
    gs->pgn_len = 0;
    gs->pgn[0]  = '\0';
    gs->game_over = 0;
    gs->result[0] = '\0';

    for (int i = 0; i < gs->hist_count; i++) {
        append_pgn(gs, gs->history[i].pgn);
    }

    /* Update last move highlight */
    if (gs->hist_count > 0) {
        Move *prev = &gs->history[gs->hist_count - 1];
        gs->last_from_r = prev->from_r; gs->last_from_c = prev->from_c;
        gs->last_to_r   = prev->to_r;   gs->last_to_c   = prev->to_c;
        gs->has_last_move = 1;
    } else {
        gs->has_last_move = 0;
    }

    gs->has_selection = 0;
    gs->legal_count   = 0;

    /* Update check status */
    gs->in_check = in_check_side(gs, gs->side);
    if (gs->in_check) find_king(gs, gs->side, &gs->king_r, &gs->king_c);

    snprintf(gs->status_msg, sizeof(gs->status_msg), "%s to move. (Takeback)",
             gs->side == WHITE_SIDE ? "White" : "Black");

    return 1;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  BOARD INITIALIZATION                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static void init_board(GameState *gs) {
    memset(gs, 0, sizeof(*gs));

    /* Black pieces (row 0-1) */
    gs->board[0][0] = -ROOK;   gs->board[0][1] = -KNIGHT;
    gs->board[0][2] = -BISHOP; gs->board[0][3] = -QUEEN;
    gs->board[0][4] = -KING;   gs->board[0][5] = -BISHOP;
    gs->board[0][6] = -KNIGHT; gs->board[0][7] = -ROOK;
    for (int c = 0; c < 8; c++) gs->board[1][c] = -PAWN;

    /* White pieces (row 6-7) */
    for (int c = 0; c < 8; c++) gs->board[6][c] = PAWN;
    gs->board[7][0] = ROOK;   gs->board[7][1] = KNIGHT;
    gs->board[7][2] = BISHOP; gs->board[7][3] = QUEEN;
    gs->board[7][4] = KING;   gs->board[7][5] = BISHOP;
    gs->board[7][6] = KNIGHT; gs->board[7][7] = ROOK;

    gs->side = WHITE_SIDE;
    gs->ep_col = -1;
    gs->castle_wk = gs->castle_wq = gs->castle_bk = gs->castle_bq = 1;
    gs->full_move = 1;
    gs->cursor_r = 4; gs->cursor_c = 4;
    gs->has_last_move = 0;
    gs->selected_r = -1; gs->selected_c = -1;
    gs->has_selection = 0;
    gs->in_check = 0;
    gs->game_over = 0;

    snprintf(gs->status_msg, sizeof(gs->status_msg), "White to move. Use arrows+Enter or hjkl.");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  ENGINE INTERFACE (UCI)                                                     */
/* ─────────────────────────────────────────────────────────────────────────── */

static int engine_start(Engine *e, const char *path) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe)) return 0;

    e->pid = fork();
    if (e->pid < 0) return 0;

    if (e->pid == 0) {
        /* Child: engine process */
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[1]); close(out_pipe[0]);
        close(in_pipe[0]); close(out_pipe[1]);
        execlp(path, path, NULL);
        /* If path fails, try full path */
        execl(path, path, NULL);
        _exit(1);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    e->in_fd  = in_pipe[1];
    e->out_fd = out_pipe[0];

    /* Make output non-blocking */
    fcntl(e->out_fd, F_SETFL, O_NONBLOCK);

    e->buf_len  = 0;
    e->ready    = 0;
    e->thinking = 0;
    e->got_move = 0;
    snprintf(e->name, sizeof(e->name), "Engine");

    /* Send UCI */
    const char *uci_init = "uci\n";
    write(e->in_fd, uci_init, strlen(uci_init));

    return 1;
}

static void engine_write(Engine *e, const char *msg) {
    if (e->pid <= 0) return;
    write(e->in_fd, msg, strlen(msg));
}

static void engine_stop(Engine *e) {
    if (e->pid <= 0) return;
    engine_write(e, "quit\n");
    usleep(100000);
    kill(e->pid, SIGTERM);
    waitpid(e->pid, NULL, WNOHANG);
    close(e->in_fd);
    close(e->out_fd);
    e->pid = 0;
}

/* Read available engine output, parse lines */
static void engine_read(Engine *e) {
    if (e->pid <= 0) return;
    char tmp[1024];
    ssize_t n;
    while ((n = read(e->out_fd, tmp, sizeof(tmp)-1)) > 0) {
        if (e->buf_len + n >= ENGINE_BUF) {
            /* Shift buffer */
            memmove(e->buf, e->buf + n, e->buf_len - n);
            e->buf_len -= n;
        }
        memcpy(e->buf + e->buf_len, tmp, n);
        e->buf_len += n;
        e->buf[e->buf_len] = '\0';
    }

    /* Process complete lines */
    char *line = e->buf;
    char *nl;
    while ((nl = memchr(line, '\n', e->buf + e->buf_len - line)) != NULL) {
        *nl = '\0';
        /* Parse line */
        if (strncmp(line, "uciok", 5) == 0) {
            e->ready = 1;
            engine_write(e, "isready\n");
        } else if (strncmp(line, "readyok", 7) == 0) {
            e->ready = 1;
        } else if (strncmp(line, "id name ", 8) == 0) {
            snprintf(e->name, sizeof(e->name), "%s", line + 8);
        } else if (strncmp(line, "bestmove", 8) == 0) {
            /* Parse: bestmove e2e4 [ponder ...] */
            char *mv = line + 9;
            /* Skip spaces */
            while (*mv == ' ') mv++;
            int i = 0;
            while (mv[i] && mv[i] != ' ' && i < 15) i++;
            memcpy(e->best_move, mv, i);
            e->best_move[i] = '\0';
            e->got_move  = 1;
            e->thinking  = 0;
        }
        line = nl + 1;
    }
    /* Shift buffer to remove processed lines */
    int remaining = e->buf + e->buf_len - line;
    if (remaining > 0 && line != e->buf)
        memmove(e->buf, line, remaining);
    e->buf_len = remaining;
    if (e->buf_len < 0) e->buf_len = 0;
}

/* Build position string from history */
static void build_position_str(const GameState *gs, char *out, int size) {
    int pos = snprintf(out, size, "position startpos");
    if (gs->hist_count > 0) {
        pos += snprintf(out+pos, size-pos, " moves");
        for (int i = 0; i < gs->hist_count; i++) {
            const Move *m = &gs->history[i];
            char promo_ch = '\0';
            if (m->flags & FLAG_PROMO) {
                int p = abs_piece(m->promotion);
                promo_ch = tolower(PIECE_LETTER[p]);
            }
            if (promo_ch)
                pos += snprintf(out+pos, size-pos, " %c%d%c%d%c",
                    FILE_LETTER[m->from_c], 8-m->from_r,
                    FILE_LETTER[m->to_c],   8-m->to_r, promo_ch);
            else
                pos += snprintf(out+pos, size-pos, " %c%d%c%d",
                    FILE_LETTER[m->from_c], 8-m->from_r,
                    FILE_LETTER[m->to_c],   8-m->to_r);
        }
    }
    snprintf(out+pos, size-pos, "\n");
}

static void engine_go(Engine *e, const GameState *gs, const TimeControl *tc) {
    char pos[4096];
    build_position_str(gs, pos, sizeof(pos));
    engine_write(e, pos);

    char go_cmd[256];
    if (tc->mode == TC_DEPTH) {
        snprintf(go_cmd, sizeof(go_cmd), "go depth %d\n", tc->depth);
    } else if (tc->mode == TC_NODES) {
        snprintf(go_cmd, sizeof(go_cmd), "go nodes %ld\n", tc->nodes);
    } else {
        /* Time-based */
        snprintf(go_cmd, sizeof(go_cmd),
                 "go wtime %d btime %d winc %d binc %d\n",
                 tc->wtime, tc->btime, tc->winc, tc->binc);
    }
    engine_write(e, go_cmd);
    e->thinking = 1;
    e->got_move = 0;
}

/* Convert UCI move string to board coordinates */
static int parse_uci_move(const char *mv, int *fr, int *fc, int *tr, int *tc, int *promo) {
    if (strlen(mv) < 4) return 0;
    *fc = mv[0] - 'a';
    *fr = 8 - (mv[1] - '0');
    *tc = mv[2] - 'a';
    *tr = 8 - (mv[3] - '0');
    *promo = 0;
    if (mv[4]) {
        switch (tolower(mv[4])) {
            case 'q': *promo = QUEEN;  break;
            case 'r': *promo = ROOK;   break;
            case 'b': *promo = BISHOP; break;
            case 'n': *promo = KNIGHT; break;
        }
    }
    return in_bounds(*fr,*fc) && in_bounds(*tr,*tc);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  RENDERING                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Layout constants (terminal row/col, 1-based) */
#define BOARD_ORIGIN_R   3     /* top row of board */
#define BOARD_ORIGIN_C   5     /* left col of board */
#define CELL_W           4     /* characters per cell */
#define CELL_H           2     /* rows per cell */
#define PANEL_COL        42    /* info panel start column */

static void set_cell_bg(int r, int c) {
    const GameState *gs = &g_state;
    int light = (r + c) % 2 == 0;

    /* Priority: check > selected > last_move > legal > cursor > normal */
    if (gs->in_check && gs->king_r == r && gs->king_c == c) {
        printf("%s", BG_CHECK); return;
    }
    if (gs->has_selection && gs->selected_r == r && gs->selected_c == c) {
        printf("%s", BG_SELECTED); return;
    }
    if (gs->has_last_move &&
        ((r == gs->last_from_r && c == gs->last_from_c) ||
         (r == gs->last_to_r   && c == gs->last_to_c))) {
        printf("%s", BG_LAST_MOVE); return;
    }
    /* Legal move destination */
    for (int i = 0; i < gs->legal_count; i++) {
        if (gs->legal_to_r[i] == r && gs->legal_to_c[i] == c) {
            if (gs->board[r][c] != EMPTY)
                printf("%s", BG_CAPTURE);
            else
                printf("%s", BG_LEGAL);
            return;
        }
    }
    if (gs->cursor_r == r && gs->cursor_c == c) {
        printf("%s", BG_CURSOR); return;
    }
    printf("%s", light ? BG_LIGHT_SQ : BG_DARK_SQ);
}

static void render_board(void) {
    const GameState *gs = &g_state;

    for (int r = 0; r < 8; r++) {
        for (int row_line = 0; row_line < CELL_H; row_line++) {
            int term_r = BOARD_ORIGIN_R + r * CELL_H + row_line;
            cursor_goto(term_r, BOARD_ORIGIN_C);

            /* Rank label */
            if (row_line == 0) {
                printf("%s%c%s", FG_COORD, '8'-r, RESET);
            } else {
                printf(" ");
            }
            printf(" ");

            for (int c = 0; c < 8; c++) {
                set_cell_bg(r, c);

                if (row_line == 0) {
                    /* Piece row */
                    int piece = gs->board[r][c];
                    if (piece != EMPTY) {
                        int ap   = abs_piece(piece);
                        int side = piece_color(piece);
                        printf("%s", side == WHITE_SIDE ? FG_WHITE_PIECE : FG_BLACK_PIECE);
                        printf("%s", PIECE_UNICODE[ap][side == WHITE_SIDE ? 0 : 1]);
                    } else {
                        printf("  ");
                    }
                    printf(" %s", RESET);
                } else {
                    printf("   %s", RESET);
                }
            }
            printf("\r\n");
        }
    }

    /* File labels */
    cursor_goto(BOARD_ORIGIN_R + 8 * CELL_H, BOARD_ORIGIN_C + 2);
    printf("%s", FG_COORD);
    for (int c = 0; c < 8; c++) printf(" %c  ", 'a'+c);
    printf("%s\r\n", RESET);
}

static void render_panel(void) {
    const GameState *gs = &g_state;
    int r = BOARD_ORIGIN_R;
    int c = PANEL_COL;

    /* Title */
    cursor_goto(r, c);
    printf("%s%s╔══════════════════════════╗%s", BOLD, FG_TITLE, RESET);
    r++;
    cursor_goto(r, c);
    printf("%s%s║    TERMINAL CHESS  v1.0  ║%s", BOLD, FG_TITLE, RESET);
    r++;
    cursor_goto(r, c);
    printf("%s%s╚══════════════════════════╝%s", BOLD, FG_TITLE, RESET);
    r+=2;

    /* Status */
    cursor_goto(r, c);
    if (gs->game_over)
        printf("%s%-28s%s", FG_ERROR, gs->status_msg, RESET);
    else if (gs->in_check)
        printf("%s%-28s%s", FG_ERROR, gs->status_msg, RESET);
    else
        printf("%s%-28s%s", FG_SUCCESS, gs->status_msg, RESET);
    r++;

    /* Engine info */
    cursor_goto(r, c);
    if (g_use_engine)
        printf("%sEngine: %-20s%s", FG_HEADER, g_engine.name, RESET);
    else
        printf("%sEngine: %-20s%s", FG_HEADER, "None (2-player)", RESET);
    r++;

    /* Time control */
    cursor_goto(r, c);
    const char *tc_names[] = {"Time", "Depth", "Nodes"};
    if (g_tc.mode == TC_TIME)
        printf("%sTC: %s %dms             %s", FG_INFO, tc_names[g_tc.mode], g_tc.time_ms, RESET);
    else if (g_tc.mode == TC_DEPTH)
        printf("%sTC: %s %d              %s", FG_INFO, tc_names[g_tc.mode], g_tc.depth, RESET);
    else
        printf("%sTC: %s %ld           %s", FG_INFO, tc_names[g_tc.mode], g_tc.nodes, RESET);
    r++;

    /* Move counter */
    cursor_goto(r, c);
    printf("%sMoves: %-3d  Half: %-3d      %s", FG_INFO,
           gs->full_move, gs->half_move, RESET);
    r+=2;

    /* PGN display */
    cursor_goto(r, c);
    printf("%s%s── PGN ─────────────────────%s", BOLD, FG_HEADER, RESET);
    r++;

    /* Wrap and display last ~14 moves worth */
    int pgn_rows = 14;
    char pgn_copy[MAX_PGN_LEN];
    strncpy(pgn_copy, gs->pgn, MAX_PGN_LEN-1);
    pgn_copy[MAX_PGN_LEN-1] = '\0';

    /* Split into lines of max 28 chars */
    char lines[30][32];
    int  nlines = 0;
    char *tok = strtok(pgn_copy, " ");
    int  line_pos = 0;
    char cur_line[32] = "";
    while (tok && nlines < 30) {
        int tlen = strlen(tok);
        if (line_pos + tlen + 1 > 28) {
            if (line_pos > 0) {
                strncpy(lines[nlines++], cur_line, 31);
                cur_line[0] = '\0';
                line_pos    = 0;
            }
        }
        if (line_pos > 0) { strcat(cur_line, " "); line_pos++; }
        strncat(cur_line, tok, 31 - line_pos);
        line_pos += tlen;
        tok = strtok(NULL, " ");
    }
    if (line_pos > 0 && nlines < 30)
        strncpy(lines[nlines++], cur_line, 31);

    /* Show last pgn_rows lines */
    int start = (nlines > pgn_rows) ? nlines - pgn_rows : 0;
    for (int i = 0; i < pgn_rows; i++) {
        cursor_goto(r + i, c);
        int li = start + i;
        if (li < nlines)
            printf("%s%-28s%s", FG_INFO, lines[li], RESET);
        else
            printf("%-28s", "");
    }
    r += pgn_rows + 1;

    /* Controls */
    cursor_goto(r, c);
    printf("%s%s── Controls ────────────────%s", BOLD, FG_HEADER, RESET);
    r++;
    const char *controls[] = {
        "Arrow keys/hjkl : Move cursor",
        "Enter/Space     : Select/Move",
        "u               : Undo move",
        "e               : Launch engine",
        "t               : Time controls",
        "n               : New game",
        "q               : Quit",
        NULL
    };
    for (int i = 0; controls[i]; i++) {
        cursor_goto(r+i, c);
        printf("%s%-28s%s", FG_INFO, controls[i], RESET);
    }
}

static void render_promo_dialog(void) {
    int term_r = 12, term_c = 20;
    cursor_goto(term_r, term_c);
    printf("%s%s╔═══════════════════════╗%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+1, term_c);
    printf("%s%s║   PROMOTE PAWN TO:    ║%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+2, term_c);
    printf("%s%s║  Q=Queen  R=Rook      ║%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+3, term_c);
    printf("%s%s║  B=Bishop N=Knight    ║%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+4, term_c);
    printf("%s%s╚═══════════════════════╝%s", BOLD, FG_TITLE, RESET);
    fflush(stdout);
}

static void render_tc_dialog(void) {
    int term_r = 10, term_c = 18;
    cursor_goto(term_r,   term_c);
    printf("%s%s╔═══════════════════════════╗%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+1, term_c);
    printf("%s%s║    TIME CONTROL SETTINGS  ║%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+2, term_c);
    printf("%s%s╠═══════════════════════════╣%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+3, term_c);
    printf("%s%s║ 1: Time   (ms per move)   ║%s", BOLD, FG_INFO, RESET);
    cursor_goto(term_r+4, term_c);
    printf("%s%s║ 2: Depth  (fixed depth)   ║%s", BOLD, FG_INFO, RESET);
    cursor_goto(term_r+5, term_c);
    printf("%s%s║ 3: Nodes  (node count)    ║%s", BOLD, FG_INFO, RESET);
    cursor_goto(term_r+6, term_c);
    printf("%s%s║ ESC: Cancel               ║%s", BOLD, FG_INFO, RESET);
    cursor_goto(term_r+7, term_c);
    printf("%s%s╚═══════════════════════════╝%s", BOLD, FG_TITLE, RESET);
    fflush(stdout);
}

static void render_engine_dialog(void) {
    int term_r = 10, term_c = 15;
    cursor_goto(term_r,   term_c);
    printf("%s%s╔═════════════════════════════════╗%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+1, term_c);
    printf("%s%s║       LAUNCH UCI ENGINE         ║%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+2, term_c);
    printf("%s%s╠═════════════════════════════════╣%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+3, term_c);
    printf("%s%s║ Common engines:                 ║%s", BOLD, FG_INFO, RESET);
    cursor_goto(term_r+4, term_c);
    printf("%s%s║  stockfish                      ║%s", BOLD, FG_INFO, RESET);
    cursor_goto(term_r+5, term_c);
    printf("%s%s║  /usr/local/bin/stockfish       ║%s", BOLD, FG_INFO, RESET);
    cursor_goto(term_r+6, term_c);
    printf("%s%s║  lc0                            ║%s", BOLD, FG_INFO, RESET);
    cursor_goto(term_r+7, term_c);
    printf("%s%s╠═════════════════════════════════╣%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+8, term_c);
    printf("%s%s║ Enter path (or Enter to cancel):║%s", BOLD, FG_INFO, RESET);
    cursor_goto(term_r+9, term_c);
    printf("%s%s║ > %-30s║%s", BOLD, FG_SUCCESS, "", RESET);
    cursor_goto(term_r+10,term_c);
    printf("%s%s╚═════════════════════════════════╝%s", BOLD, FG_TITLE, RESET);
    cursor_goto(term_r+9, term_c+3);
    fflush(stdout);
}

static void full_render(void) {
    /* Move cursor to top-left, don't clear (reduces flicker) */
    cursor_goto(1,1);
    /* Render title bar */
    printf("%s%s  ♔ TERMINAL CHESS — UCI ENGINE INTERFACE ♚  %s\r\n",
           BOLD, FG_TITLE, RESET);
    printf("%s  ─────────────────────────────────────────────%s\r\n",
           FG_COORD, RESET);
    render_board();
    render_panel();
    /* Move cursor away from board */
    cursor_goto(BOARD_ORIGIN_R + 8*CELL_H + 3, 1);
    fflush(stdout);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  INPUT HANDLING                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Read a line from terminal in raw mode (for engine path input) */
static int read_line_term(char *buf, int size) {
    term_restore();
    /* Enable echo temporarily */
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= (ECHO | ICANON);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    cursor_show();

    char *ret = fgets(buf, size, stdin);

    cursor_hide();
    term_raw();

    if (!ret) return 0;
    int len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    return len;
}

/* Read a number from terminal */
static long read_number_term(void) {
    char buf[64];
    int len = read_line_term(buf, sizeof(buf));
    if (!len) return -1;
    return atol(buf);
}

static void handle_tc_dialog(void) {
    render_tc_dialog();
    char ch = 0;
    read(STDIN_FILENO, &ch, 1);
    if (ch == '1') {
        g_tc.mode = TC_TIME;
        /* Ask for value */
        cursor_goto(20, 18);
        printf("%sTime per move (ms, e.g. 1000): %s", FG_INFO, RESET);
        fflush(stdout);
        long v = read_number_term();
        if (v > 0) {
            g_tc.time_ms = (int)v;
            g_tc.wtime   = (int)v * 40;
            g_tc.btime   = (int)v * 40;
        }
    } else if (ch == '2') {
        g_tc.mode = TC_DEPTH;
        cursor_goto(20, 18);
        printf("%sSearch depth (e.g. 8): %s", FG_INFO, RESET);
        fflush(stdout);
        long v = read_number_term();
        if (v > 0) g_tc.depth = (int)v;
    } else if (ch == '3') {
        g_tc.mode = TC_NODES;
        cursor_goto(20, 18);
        printf("%sNode count (e.g. 1000000): %s", FG_INFO, RESET);
        fflush(stdout);
        long v = read_number_term();
        if (v > 0) g_tc.nodes = v;
    }
    full_render();
}

static void handle_engine_dialog(void) {
    render_engine_dialog();
    char path[256] = {0};
    int len = read_line_term(path, sizeof(path));
    if (len == 0) { full_render(); return; }

    if (g_use_engine) {
        engine_stop(&g_engine);
        g_use_engine = 0;
    }

    memset(&g_engine, 0, sizeof(g_engine));
    if (engine_start(&g_engine, path)) {
        g_use_engine = 1;
        snprintf(g_state.status_msg, sizeof(g_state.status_msg),
                 "Engine started. Waiting for readyok...");
        /* Wait briefly for engine to init */
        usleep(500000);
        engine_read(&g_engine);
    } else {
        snprintf(g_state.status_msg, sizeof(g_state.status_msg),
                 "Failed to start engine: %s", path);
    }
    full_render();
}

static void handle_promo_input(char ch) {
    int promo = 0;
    switch (tolower(ch)) {
        case 'q': promo = QUEEN;  break;
        case 'r': promo = ROOK;   break;
        case 'b': promo = BISHOP; break;
        case 'n': promo = KNIGHT; break;
        default: return;
    }
    g_promo_pending = 0;
    do_move(&g_state, g_promo_from_r, g_promo_from_c,
            g_promo_to_r,   g_promo_to_c, promo);
    full_render();
}

/* Check if a pawn move to destination requires promotion */
static int needs_promotion(const GameState *gs, int fr, int fc, int tr) {
    int piece = gs->board[fr][fc];
    if (abs_piece(piece) != PAWN) return 0;
    if (piece_color(piece) == WHITE_SIDE && tr == 0) return 1;
    if (piece_color(piece) == BLACK_SIDE && tr == 7) return 1;
    return 0;
}

static void try_select_or_move(int r, int c) {
    GameState *gs = &g_state;
    if (gs->game_over) return;

    int piece = gs->board[r][c];

    if (!gs->has_selection) {
        /* Select piece */
        if (piece != EMPTY && piece_color(piece) == gs->side) {
            gs->selected_r = r;
            gs->selected_c = c;
            gs->has_selection = 1;
            int flags_tmp[MAX_LEGAL];
            gs->legal_count = legal_moves_for(gs, r, c,
                                               gs->legal_to_r, gs->legal_to_c,
                                               flags_tmp, MAX_LEGAL);
        }
    } else {
        /* Check if clicking own piece (re-select) */
        if (piece != EMPTY && piece_color(piece) == gs->side &&
            !(r == gs->selected_r && c == gs->selected_c)) {
            gs->selected_r = r;
            gs->selected_c = c;
            int flags_tmp[MAX_LEGAL];
            gs->legal_count = legal_moves_for(gs, r, c,
                                               gs->legal_to_r, gs->legal_to_c,
                                               flags_tmp, MAX_LEGAL);
            return;
        }

        /* Attempt move */
        int is_legal_dest = 0;
        for (int i = 0; i < gs->legal_count; i++) {
            if (gs->legal_to_r[i] == r && gs->legal_to_c[i] == c) {
                is_legal_dest = 1;
                break;
            }
        }

        if (is_legal_dest) {
            if (needs_promotion(gs, gs->selected_r, gs->selected_c, r)) {
                /* Show promotion dialog */
                g_promo_pending = 1;
                g_promo_from_r = gs->selected_r;
                g_promo_from_c = gs->selected_c;
                g_promo_to_r   = r;
                g_promo_to_c   = c;
                render_promo_dialog();
                return;
            } else {
                do_move(gs, gs->selected_r, gs->selected_c, r, c, 0);
            }
        } else {
            /* Deselect */
            gs->has_selection = 0;
            gs->legal_count   = 0;
            /* If clicked own piece, select it */
            if (piece != EMPTY && piece_color(piece) == gs->side) {
                gs->selected_r = r;
                gs->selected_c = c;
                gs->has_selection = 1;
                int flags_tmp[MAX_LEGAL];
                gs->legal_count = legal_moves_for(gs, r, c,
                                                   gs->legal_to_r, gs->legal_to_c,
                                                   flags_tmp, MAX_LEGAL);
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  MAIN LOOP                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(void) {
    /* Terminal setup */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    screen_alt();
    term_raw();
    cursor_hide();
    screen_clear();

    /* Init game */
    init_board(&g_state);

    /* Default time control */
    g_tc.mode    = TC_DEPTH;
    g_tc.depth   = 8;
    g_tc.time_ms = 2000;
    g_tc.nodes   = 1000000;
    g_tc.wtime   = 300000;
    g_tc.btime   = 300000;
    g_tc.winc    = 0;
    g_tc.binc    = 0;

    full_render();

    while (g_running) {
        /* Poll engine if it's thinking */
        if (g_use_engine && g_engine.thinking) {
            engine_read(&g_engine);
            if (g_engine.got_move && !g_state.game_over) {
                int fr, fc, tr, tc, promo;
                if (parse_uci_move(g_engine.best_move, &fr, &fc, &tr, &tc, &promo)) {
                    if (!do_move(&g_state, fr, fc, tr, tc, promo)) {
                        snprintf(g_state.status_msg, sizeof(g_state.status_msg),
                                 "Engine illegal move: %s", g_engine.best_move);
                    }
                }
                g_engine.got_move = 0;
                g_engine.thinking = 0;
                /* Update time control */
                if (g_tc.mode == TC_TIME) {
                    if (g_state.side == WHITE_SIDE)
                        g_tc.btime = (g_tc.btime > g_tc.time_ms) ? g_tc.btime - g_tc.time_ms : 100;
                    else
                        g_tc.wtime = (g_tc.wtime > g_tc.time_ms) ? g_tc.wtime - g_tc.time_ms : 100;
                }
                full_render();
            }
        }

        /* Trigger engine move if it's engine's turn */
        if (g_use_engine && !g_engine.thinking && g_engine.ready &&
            !g_state.game_over && g_state.side == g_engine_side) {
            engine_go(&g_engine, &g_state, &g_tc);
        }

        /* Read input */
        unsigned char seq[8] = {0};
        int nread = read(STDIN_FILENO, seq, sizeof(seq));
        if (nread <= 0) {
            /* No input, small sleep to avoid spinning */
            usleep(20000);
            continue;
        }

        /* Promotion dialog active */
        if (g_promo_pending) {
            handle_promo_input(seq[0]);
            continue;
        }

        GameState *gs = &g_state;

        if (seq[0] == '\033' && nread >= 3 && seq[1] == '[') {
            /* Escape sequence — arrow keys */
            switch (seq[2]) {
                case 'A': /* Up */
                    if (gs->cursor_r > 0) gs->cursor_r--;
                    break;
                case 'B': /* Down */
                    if (gs->cursor_r < 7) gs->cursor_r++;
                    break;
                case 'C': /* Right */
                    if (gs->cursor_c < 7) gs->cursor_c++;
                    break;
                case 'D': /* Left */
                    if (gs->cursor_c > 0) gs->cursor_c--;
                    break;
            }
            full_render();
        } else {
            switch (seq[0]) {
                /* vi-style movement */
                case 'h': case 'H':
                    if (gs->cursor_c > 0) { gs->cursor_c--; full_render(); }
                    break;
                case 'l': case 'L':
                    if (gs->cursor_c < 7) { gs->cursor_c++; full_render(); }
                    break;
                case 'k': case 'K':
                    if (gs->cursor_r > 0) { gs->cursor_r--; full_render(); }
                    break;
                case 'j': case 'J':
                    if (gs->cursor_r < 7) { gs->cursor_r++; full_render(); }
                    break;

                /* Select / move */
                case '\r': case '\n': case ' ':
                    try_select_or_move(gs->cursor_r, gs->cursor_c);
                    full_render();
                    break;

                /* Deselect */
                case '\033':
                    gs->has_selection = 0;
                    gs->legal_count   = 0;
                    full_render();
                    break;

                /* Undo */
                case 'u': case 'U':
                    if (g_use_engine && g_engine.thinking) {
                        engine_write(&g_engine, "stop\n");
                        usleep(100000);
                        engine_read(&g_engine);
                        g_engine.thinking = 0;
                    }
                    /* Undo twice if playing engine (undo engine + player move) */
                    undo_move(gs);
                    if (g_use_engine && gs->hist_count > 0)
                        undo_move(gs);
                    full_render();
                    break;

                /* Engine dialog */
                case 'e': case 'E':
                    handle_engine_dialog();
                    break;

                /* Time control dialog */
                case 't': case 'T':
                    handle_tc_dialog();
                    break;

                /* New game */
                case 'n': case 'N': {
                    if (g_use_engine && g_engine.thinking) {
                        engine_write(&g_engine, "stop\n");
                        usleep(100000);
                        engine_read(&g_engine);
                        g_engine.thinking = 0;
                    }
                    if (g_use_engine)
                        engine_write(&g_engine, "ucinewgame\n");
                    init_board(gs);
                    g_tc.wtime = 300000;
                    g_tc.btime = 300000;
                    full_render();
                    break;
                }

                /* Quit */
                case 'q': case 'Q':
                    g_running = 0;
                    break;

                /* Flip engine side (play as black/white) */
                case 'f': case 'F':
                    g_engine_side = -g_engine_side;
                    snprintf(gs->status_msg, sizeof(gs->status_msg),
                             "Engine now plays %s.",
                             g_engine_side == WHITE_SIDE ? "White" : "Black");
                    full_render();
                    break;

                default:
                    break;
            }
        }
    }

    /* Cleanup */
    if (g_use_engine) engine_stop(&g_engine);
    cleanup();

    /* Print PGN to stdout */
    printf("\nGame PGN:\n%s\n", g_state.pgn);
    if (g_state.result[0])
        printf("Result: %s\n", g_state.result);
    printf("\n");

    return 0;
}
