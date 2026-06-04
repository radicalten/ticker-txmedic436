/*
 * chess_tui.c - Terminal Chess GUI with UCI Engine Support
 * Compile: gcc -o chess_tui chess_tui.c
 * Run: ./chess_tui [path_to_uci_engine]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>

/* ─── ANSI / Terminal ─────────────────────────────────────────────────── */
#define ESC "\x1b"
#define CSI "\x1b["
#define CLEAR_SCREEN    CSI "2J" CSI "H"
#define HIDE_CURSOR     CSI "?25l"
#define SHOW_CURSOR     CSI "?25h"
#define RESET_COLOR     CSI "0m"
#define BOLD            CSI "1m"
#define MOVE_TO(r,c)    printf(CSI "%d;%dH", (r), (c))
#define SAVE_POS        printf(CSI "s")
#define REST_POS        printf(CSI "u")

/* True-color macros */
#define FG(r,g,b)       printf(CSI "38;2;%d;%d;%dm", r, g, b)
#define BG(r,g,b)       printf(CSI "48;2;%d;%d;%dm", r, g, b)
#define FG_S(r,g,b)     CSI "38;2;" #r ";" #g ";" #b "m"

/* ─── Board / Chess constants ─────────────────────────────────────────── */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  0
#define BLACK  1

#define MAX_MOVES      512
#define MAX_HIST       1024
#define MAX_PGN_LEN    8192
#define ENGINE_BUF     4096

/* Castling flags */
#define CASTLE_WK  1
#define CASTLE_WQ  2
#define CASTLE_BK  4
#define CASTLE_BQ  8

/* ─── Structs ──────────────────────────────────────────────────────────── */
typedef struct {
    int piece;   /* PAWN..KING */
    int color;   /* WHITE/BLACK */
} Square;

typedef struct {
    int from_file, from_rank;
    int to_file,   to_rank;
    int promo;          /* promotion piece or 0 */
    int captured;       /* captured piece type */
    int captured_color;
    int ep_file;        /* en-passant file before move (-1 = none) */
    int ep_rank;
    int castling_before;
    int halfmove_before;
    int ep_capture;     /* was this an en-passant capture? */
    int castle_type;    /* 0=none,1=WK,2=WQ,3=BK,4=BQ */
    char san[16];       /* SAN notation */
} Move;

typedef struct {
    Square  sq[8][8];   /* [file][rank], file=0=a, rank=0=1 */
    int     turn;       /* WHITE / BLACK */
    int     castling;   /* bitmask */
    int     ep_file;    /* en-passant target (-1 = none) */
    int     ep_rank;
    int     halfmove;
    int     fullmove;
    Move    history[MAX_HIST];
    int     hist_count;
} Board;

typedef enum {
    TC_NODES, TC_DEPTH, TC_TIME
} TCMode;

typedef struct {
    TCMode  mode;
    long    nodes;      /* for TC_NODES */
    int     depth;      /* for TC_DEPTH */
    int     ms;         /* for TC_TIME  */
} TimeControl;

typedef struct {
    pid_t   pid;
    int     in_fd;   /* write to engine */
    int     out_fd;  /* read from engine */
    char    buf[ENGINE_BUF];
    int     buf_len;
    int     ready;
    int     active;
    char    name[128];
    char    best_move[16];
    int     thinking;
} Engine;

/* ─── Globals ──────────────────────────────────────────────────────────── */
static Board        g_board;
static Engine       g_engine;
static TimeControl  g_tc = { TC_TIME, 0, 0, 1000 };

static int  g_sel_file = 0, g_sel_rank = 0; /* cursor */
static int  g_from_file = -1, g_from_rank = -1; /* selected piece */
static int  g_running = 1;
static int  g_player_color = WHITE;   /* human plays white */
static int  g_engine_enabled = 0;
static int  g_in_menu = 0;
static int  g_in_promo = 0;
static int  g_promo_file, g_promo_rank, g_promo_from_file, g_promo_from_rank;

static char g_pgn[MAX_PGN_LEN];
static int  g_pgn_len = 0;
static char g_status_msg[256];
static int  g_status_timer = 0;

static struct termios g_orig_termios;

/* Legal move cache */
static Move g_legal[MAX_MOVES];
static int  g_legal_count = 0;
static int  g_legal_dirty = 1;

/* ─── Forward declarations ────────────────────────────────────────────── */
static void board_init(Board *b);
static void compute_legal_moves(Board *b, Move *out, int *count);
static int  is_in_check(Board *b, int color);
static void apply_move(Board *b, Move *m);
static void unapply_move(Board *b, Move *m);
static void render(void);
static void set_status(const char *fmt, ...);
static void engine_send(const char *msg);
static void engine_start_thinking(void);
static void engine_poll(void);
static int  move_from_uci(Board *b, const char *uci, Move *m);
static void move_to_uci(Move *m, char *out);
static void pgn_append_move(Move *m, int fullmove, int turn);
static void compute_san(Board *b, Move *m, Move *legal, int legal_count, char *out);

/* ─── Terminal setup ───────────────────────────────────────────────────── */
static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    t = g_orig_termios;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    printf(SHOW_CURSOR RESET_COLOR "\n");
    fflush(stdout);
}

static void cleanup(void) {
    term_restore();
    if (g_engine.active) {
        engine_send("quit\n");
        close(g_engine.in_fd);
        close(g_engine.out_fd);
    }
}

static void sig_handler(int s) {
    (void)s;
    cleanup();
    exit(0);
}

/* ─── Board Init ───────────────────────────────────────────────────────── */
static void board_init(Board *b) {
    memset(b, 0, sizeof(*b));
    b->ep_file = -1;
    b->ep_rank = -1;
    b->castling = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    b->fullmove = 1;
    b->turn = WHITE;

    /* Pawns */
    for (int f = 0; f < 8; f++) {
        b->sq[f][1].piece = PAWN; b->sq[f][1].color = WHITE;
        b->sq[f][6].piece = PAWN; b->sq[f][6].color = BLACK;
    }
    /* Rooks */
    b->sq[0][0].piece = ROOK; b->sq[0][0].color = WHITE;
    b->sq[7][0].piece = ROOK; b->sq[7][0].color = WHITE;
    b->sq[0][7].piece = ROOK; b->sq[0][7].color = BLACK;
    b->sq[7][7].piece = ROOK; b->sq[7][7].color = BLACK;
    /* Knights */
    b->sq[1][0].piece = KNIGHT; b->sq[1][0].color = WHITE;
    b->sq[6][0].piece = KNIGHT; b->sq[6][0].color = WHITE;
    b->sq[1][7].piece = KNIGHT; b->sq[1][7].color = BLACK;
    b->sq[6][7].piece = KNIGHT; b->sq[6][7].color = BLACK;
    /* Bishops */
    b->sq[2][0].piece = BISHOP; b->sq[2][0].color = WHITE;
    b->sq[5][0].piece = BISHOP; b->sq[5][0].color = WHITE;
    b->sq[2][7].piece = BISHOP; b->sq[2][7].color = BLACK;
    b->sq[5][7].piece = BISHOP; b->sq[5][7].color = BLACK;
    /* Queens */
    b->sq[3][0].piece = QUEEN; b->sq[3][0].color = WHITE;
    b->sq[3][7].piece = QUEEN; b->sq[3][7].color = BLACK;
    /* Kings */
    b->sq[4][0].piece = KING; b->sq[4][0].color = WHITE;
    b->sq[4][7].piece = KING; b->sq[4][7].color = BLACK;
}

/* ─── Move generation helpers ─────────────────────────────────────────── */
static int in_bounds(int f, int r) {
    return f >= 0 && f < 8 && r >= 0 && r < 8;
}

static void add_move(Move *list, int *cnt,
                     Board *b, int ff, int fr, int tf, int tr,
                     int promo) {
    if (!in_bounds(tf, tr)) return;
    Move m;
    memset(&m, 0, sizeof(m));
    m.from_file = ff; m.from_rank = fr;
    m.to_file   = tf; m.to_rank   = tr;
    m.promo     = promo;
    m.ep_file   = b->ep_file;
    m.ep_rank   = b->ep_rank;
    m.castling_before = b->castling;
    m.halfmove_before = b->halfmove;

    Square *tgt = &b->sq[tf][tr];
    if (tgt->piece) {
        m.captured       = tgt->piece;
        m.captured_color = tgt->color;
    }
    list[(*cnt)++] = m;
}

/* Generate pseudo-legal moves (may leave king in check) */
static void gen_pseudo(Board *b, int color, Move *out, int *cnt) {
    *cnt = 0;
    int dir = (color == WHITE) ? 1 : -1;

    for (int f = 0; f < 8; f++) {
        for (int r = 0; r < 8; r++) {
            Square *s = &b->sq[f][r];
            if (!s->piece || s->color != color) continue;

            switch (s->piece) {

            case PAWN: {
                int nr = r + dir;
                /* Single push */
                if (in_bounds(f, nr) && !b->sq[f][nr].piece) {
                    if (nr == 0 || nr == 7) {
                        add_move(out, cnt, b, f, r, f, nr, QUEEN);
                        add_move(out, cnt, b, f, r, f, nr, ROOK);
                        add_move(out, cnt, b, f, r, f, nr, BISHOP);
                        add_move(out, cnt, b, f, r, f, nr, KNIGHT);
                    } else {
                        add_move(out, cnt, b, f, r, f, nr, 0);
                    }
                    /* Double push */
                    int start_rank = (color == WHITE) ? 1 : 6;
                    if (r == start_rank && !b->sq[f][nr+dir].piece) {
                        add_move(out, cnt, b, f, r, f, nr+dir, 0);
                    }
                }
                /* Captures */
                for (int df = -1; df <= 1; df += 2) {
                    int nf = f + df;
                    if (!in_bounds(nf, nr)) continue;
                    Square *cap = &b->sq[nf][nr];
                    if (cap->piece && cap->color != color) {
                        if (nr == 0 || nr == 7) {
                            add_move(out, cnt, b, f, r, nf, nr, QUEEN);
                            add_move(out, cnt, b, f, r, nf, nr, ROOK);
                            add_move(out, cnt, b, f, r, nf, nr, BISHOP);
                            add_move(out, cnt, b, f, r, nf, nr, KNIGHT);
                        } else {
                            add_move(out, cnt, b, f, r, nf, nr, 0);
                        }
                    }
                    /* En passant */
                    if (b->ep_file == nf && b->ep_rank == nr) {
                        Move m;
                        memset(&m, 0, sizeof(m));
                        m.from_file = f; m.from_rank = r;
                        m.to_file = nf; m.to_rank = nr;
                        m.ep_capture = 1;
                        m.captured = PAWN;
                        m.captured_color = (color == WHITE) ? BLACK : WHITE;
                        m.ep_file = b->ep_file;
                        m.ep_rank = b->ep_rank;
                        m.castling_before = b->castling;
                        m.halfmove_before = b->halfmove;
                        out[(*cnt)++] = m;
                    }
                }
                break;
            }

            case KNIGHT: {
                int dfs[] = {-2,-2,-1,-1, 1, 1, 2, 2};
                int drs[] = {-1, 1,-2, 2,-2, 2,-1, 1};
                for (int i = 0; i < 8; i++) {
                    int nf = f+dfs[i], nr2 = r+drs[i];
                    if (!in_bounds(nf, nr2)) continue;
                    Square *t = &b->sq[nf][nr2];
                    if (t->piece && t->color == color) continue;
                    add_move(out, cnt, b, f, r, nf, nr2, 0);
                }
                break;
            }

            case BISHOP: {
                int dirs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
                for (int d = 0; d < 4; d++) {
                    for (int step = 1; step < 8; step++) {
                        int nf = f + dirs[d][0]*step;
                        int nr2 = r + dirs[d][1]*step;
                        if (!in_bounds(nf, nr2)) break;
                        Square *t = &b->sq[nf][nr2];
                        if (t->piece && t->color == color) break;
                        add_move(out, cnt, b, f, r, nf, nr2, 0);
                        if (t->piece) break;
                    }
                }
                break;
            }

            case ROOK: {
                int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                for (int d = 0; d < 4; d++) {
                    for (int step = 1; step < 8; step++) {
                        int nf = f + dirs[d][0]*step;
                        int nr2 = r + dirs[d][1]*step;
                        if (!in_bounds(nf, nr2)) break;
                        Square *t = &b->sq[nf][nr2];
                        if (t->piece && t->color == color) break;
                        add_move(out, cnt, b, f, r, nf, nr2, 0);
                        if (t->piece) break;
                    }
                }
                break;
            }

            case QUEEN: {
                int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},
                                  {1,1},{1,-1},{-1,1},{-1,-1}};
                for (int d = 0; d < 8; d++) {
                    for (int step = 1; step < 8; step++) {
                        int nf = f + dirs[d][0]*step;
                        int nr2 = r + dirs[d][1]*step;
                        if (!in_bounds(nf, nr2)) break;
                        Square *t = &b->sq[nf][nr2];
                        if (t->piece && t->color == color) break;
                        add_move(out, cnt, b, f, r, nf, nr2, 0);
                        if (t->piece) break;
                    }
                }
                break;
            }

            case KING: {
                int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},
                                  {1,1},{1,-1},{-1,1},{-1,-1}};
                for (int d = 0; d < 8; d++) {
                    int nf = f+dirs[d][0], nr2 = r+dirs[d][1];
                    if (!in_bounds(nf, nr2)) continue;
                    Square *t = &b->sq[nf][nr2];
                    if (t->piece && t->color == color) continue;
                    add_move(out, cnt, b, f, r, nf, nr2, 0);
                }
                /* Castling */
                int base_rank = (color == WHITE) ? 0 : 7;
                if (color == WHITE) {
                    if ((b->castling & CASTLE_WK) &&
                        !b->sq[5][base_rank].piece &&
                        !b->sq[6][base_rank].piece) {
                        Move m; memset(&m,0,sizeof(m));
                        m.from_file=4; m.from_rank=base_rank;
                        m.to_file=6;  m.to_rank=base_rank;
                        m.castle_type=1;
                        m.ep_file=b->ep_file; m.ep_rank=b->ep_rank;
                        m.castling_before=b->castling;
                        m.halfmove_before=b->halfmove;
                        out[(*cnt)++] = m;
                    }
                    if ((b->castling & CASTLE_WQ) &&
                        !b->sq[3][base_rank].piece &&
                        !b->sq[2][base_rank].piece &&
                        !b->sq[1][base_rank].piece) {
                        Move m; memset(&m,0,sizeof(m));
                        m.from_file=4; m.from_rank=base_rank;
                        m.to_file=2;  m.to_rank=base_rank;
                        m.castle_type=2;
                        m.ep_file=b->ep_file; m.ep_rank=b->ep_rank;
                        m.castling_before=b->castling;
                        m.halfmove_before=b->halfmove;
                        out[(*cnt)++] = m;
                    }
                } else {
                    if ((b->castling & CASTLE_BK) &&
                        !b->sq[5][base_rank].piece &&
                        !b->sq[6][base_rank].piece) {
                        Move m; memset(&m,0,sizeof(m));
                        m.from_file=4; m.from_rank=base_rank;
                        m.to_file=6;  m.to_rank=base_rank;
                        m.castle_type=3;
                        m.ep_file=b->ep_file; m.ep_rank=b->ep_rank;
                        m.castling_before=b->castling;
                        m.halfmove_before=b->halfmove;
                        out[(*cnt)++] = m;
                    }
                    if ((b->castling & CASTLE_BQ) &&
                        !b->sq[3][base_rank].piece &&
                        !b->sq[2][base_rank].piece &&
                        !b->sq[1][base_rank].piece) {
                        Move m; memset(&m,0,sizeof(m));
                        m.from_file=4; m.from_rank=base_rank;
                        m.to_file=2;  m.to_rank=base_rank;
                        m.castle_type=4;
                        m.ep_file=b->ep_file; m.ep_rank=b->ep_rank;
                        m.castling_before=b->castling;
                        m.halfmove_before=b->halfmove;
                        out[(*cnt)++] = m;
                    }
                }
                break;
            }

            } /* switch */
        }
    }
}

/* Check if square (tf,tr) is attacked by 'attacker' color */
static int is_attacked(Board *b, int tf, int tr, int attacker) {
    /* Knight */
    int ndfs[] = {-2,-2,-1,-1, 1, 1, 2, 2};
    int ndrs[] = {-1, 1,-2, 2,-2, 2,-1, 1};
    for (int i = 0; i < 8; i++) {
        int f = tf+ndfs[i], r = tr+ndrs[i];
        if (in_bounds(f,r) && b->sq[f][r].piece == KNIGHT &&
            b->sq[f][r].color == attacker) return 1;
    }
    /* Diagonals (bishop/queen) */
    int ddirs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int d = 0; d < 4; d++) {
        for (int step = 1; step < 8; step++) {
            int f = tf+ddirs[d][0]*step, r = tr+ddirs[d][1]*step;
            if (!in_bounds(f,r)) break;
            Square *s = &b->sq[f][r];
            if (s->piece) {
                if (s->color == attacker &&
                    (s->piece == BISHOP || s->piece == QUEEN)) return 1;
                break;
            }
        }
    }
    /* Orthogonals (rook/queen) */
    int rdirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    for (int d = 0; d < 4; d++) {
        for (int step = 1; step < 8; step++) {
            int f = tf+rdirs[d][0]*step, r = tr+rdirs[d][1]*step;
            if (!in_bounds(f,r)) break;
            Square *s = &b->sq[f][r];
            if (s->piece) {
                if (s->color == attacker &&
                    (s->piece == ROOK || s->piece == QUEEN)) return 1;
                break;
            }
        }
    }
    /* King */
    for (int df = -1; df <= 1; df++) for (int dr = -1; dr <= 1; dr++) {
        if (!df && !dr) continue;
        int f = tf+df, r = tr+dr;
        if (in_bounds(f,r) && b->sq[f][r].piece == KING &&
            b->sq[f][r].color == attacker) return 1;
    }
    /* Pawn */
    int pdir = (attacker == WHITE) ? -1 : 1; /* attacker pawn comes from */
    /* Actually pawn attacks: if attacker is WHITE, pawns attack upward (+1) */
    int pawn_dir = (attacker == WHITE) ? 1 : -1;
    for (int df = -1; df <= 1; df += 2) {
        int f = tf+df, r = tr - pawn_dir;
        if (in_bounds(f,r) && b->sq[f][r].piece == PAWN &&
            b->sq[f][r].color == attacker) return 1;
    }
    (void)pdir;
    return 0;
}

static int is_in_check(Board *b, int color) {
    /* Find king */
    for (int f = 0; f < 8; f++) {
        for (int r = 0; r < 8; r++) {
            if (b->sq[f][r].piece == KING && b->sq[f][r].color == color) {
                return is_attacked(b, f, r, 1-color);
            }
        }
    }
    return 0;
}

static void apply_move(Board *b, Move *m) {
    int ff = m->from_file, fr = m->from_rank;
    int tf = m->to_file,   tr = m->to_rank;
    Square *src = &b->sq[ff][fr];
    Square *dst = &b->sq[tf][tr];

    int piece = src->piece;
    int color = src->color;

    /* Update halfmove clock */
    if (piece == PAWN || m->captured) b->halfmove = 0;
    else b->halfmove++;

    /* En passant capture */
    if (m->ep_capture) {
        int cap_rank = (color == WHITE) ? tr - 1 : tr + 1;
        b->sq[tf][cap_rank].piece = 0;
        b->sq[tf][cap_rank].color = 0;
    }

    /* Move piece */
    dst->piece = (m->promo) ? m->promo : piece;
    dst->color = color;
    src->piece = 0;
    src->color = 0;

    /* Castling rook */
    if (m->castle_type) {
        int base = (color == WHITE) ? 0 : 7;
        if (m->castle_type == 1 || m->castle_type == 3) { /* kingside */
            b->sq[5][base].piece = ROOK; b->sq[5][base].color = color;
            b->sq[7][base].piece = 0;
        } else { /* queenside */
            b->sq[3][base].piece = ROOK; b->sq[3][base].color = color;
            b->sq[0][base].piece = 0;
        }
    }

    /* Update en passant target */
    b->ep_file = -1; b->ep_rank = -1;
    if (piece == PAWN && abs(tr - fr) == 2) {
        b->ep_file = ff;
        b->ep_rank = (fr + tr) / 2;
    }

    /* Update castling rights */
    if (piece == KING) {
        if (color == WHITE) b->castling &= ~(CASTLE_WK | CASTLE_WQ);
        else                b->castling &= ~(CASTLE_BK | CASTLE_BQ);
    }
    if (ff == 0 && fr == 0) b->castling &= ~CASTLE_WQ;
    if (ff == 7 && fr == 0) b->castling &= ~CASTLE_WK;
    if (ff == 0 && fr == 7) b->castling &= ~CASTLE_BQ;
    if (ff == 7 && fr == 7) b->castling &= ~CASTLE_BK;
    if (tf == 0 && tr == 0) b->castling &= ~CASTLE_WQ;
    if (tf == 7 && tr == 0) b->castling &= ~CASTLE_WK;
    if (tf == 0 && tr == 7) b->castling &= ~CASTLE_BQ;
    if (tf == 7 && tr == 7) b->castling &= ~CASTLE_BK;

    if (b->turn == BLACK) b->fullmove++;
    b->turn = 1 - b->turn;

    if (b->hist_count < MAX_HIST)
        b->history[b->hist_count++] = *m;
}

static void unapply_move(Board *b, Move *m) {
    if (b->hist_count > 0) b->hist_count--;

    int ff = m->from_file, fr = m->from_rank;
    int tf = m->to_file,   tr = m->to_rank;

    b->turn = 1 - b->turn;
    int color = b->turn;

    Square *src = &b->sq[ff][fr];
    Square *dst = &b->sq[tf][tr];

    int piece = m->promo ? PAWN : dst->piece;

    src->piece = piece;
    src->color = color;

    /* Restore captured */
    if (m->ep_capture) {
        dst->piece = 0; dst->color = 0;
        int cap_rank = (color == WHITE) ? tr - 1 : tr + 1;
        b->sq[tf][cap_rank].piece = PAWN;
        b->sq[tf][cap_rank].color = 1 - color;
    } else {
        dst->piece = m->captured;
        dst->color = m->captured ? m->captured_color : 0;
    }

    /* Undo castling rook */
    if (m->castle_type) {
        int base = (color == WHITE) ? 0 : 7;
        if (m->castle_type == 1 || m->castle_type == 3) {
            b->sq[7][base].piece = ROOK; b->sq[7][base].color = color;
            b->sq[5][base].piece = 0;
        } else {
            b->sq[0][base].piece = ROOK; b->sq[0][base].color = color;
            b->sq[3][base].piece = 0;
        }
    }

    b->ep_file       = m->ep_file;
    b->ep_rank       = m->ep_rank;
    b->castling      = m->castling_before;
    b->halfmove      = m->halfmove_before;
    if (color == BLACK) b->fullmove--;
}

static void compute_legal_moves(Board *b, Move *out, int *count) {
    Move pseudo[MAX_MOVES];
    int  pcnt = 0;
    gen_pseudo(b, b->turn, pseudo, &pcnt);
    *count = 0;

    for (int i = 0; i < pcnt; i++) {
        Move *m = &pseudo[i];
        /* Check castling doesn't pass through check */
        if (m->castle_type) {
            int base = (b->turn == WHITE) ? 0 : 7;
            int opp = 1 - b->turn;
            if (is_attacked(b, 4, base, opp)) continue;
            if (m->to_file == 6) { /* kingside */
                if (is_attacked(b, 5, base, opp) ||
                    is_attacked(b, 6, base, opp)) continue;
            } else { /* queenside */
                if (is_attacked(b, 3, base, opp) ||
                    is_attacked(b, 2, base, opp)) continue;
            }
        }
        apply_move(b, m);
        int in_chk = is_in_check(b, 1 - b->turn);
        unapply_move(b, m);
        if (!in_chk) out[(*count)++] = pseudo[i];
    }
}

/* ─── SAN notation ─────────────────────────────────────────────────────── */
static const char *piece_char = ".PNBRQK";
static const char *file_char  = "abcdefgh";

static void compute_san(Board *b, Move *m, Move *legal, int lcnt, char *out) {
    char buf[32];
    int  pos = 0;

    Square *src = &b->sq[m->from_file][m->from_rank];

    if (m->castle_type == 1 || m->castle_type == 3) {
        strcpy(out, "O-O"); return;
    }
    if (m->castle_type == 2 || m->castle_type == 4) {
        strcpy(out, "O-O-O"); return;
    }

    int piece = src->piece;
    if (piece != PAWN) buf[pos++] = piece_char[piece];

    /* Disambiguation */
    if (piece != PAWN) {
        int same_dest[8][8]; /* other same-piece attacks same square */
        int amb_file = 0, amb_rank = 0, ambiguous = 0;
        for (int i = 0; i < lcnt; i++) {
            if (&legal[i] == m) continue;
            if (legal[i].to_file == m->to_file &&
                legal[i].to_rank == m->to_rank) {
                Square *s2 = &b->sq[legal[i].from_file][legal[i].from_rank];
                if (s2->piece == piece && s2->color == b->turn) {
                    ambiguous = 1;
                    if (legal[i].from_file == m->from_file) amb_rank = 1;
                    else amb_file = 1;
                    (void)same_dest;
                }
            }
        }
        if (ambiguous) {
            if (!amb_file) buf[pos++] = file_char[m->from_file];
            else if (!amb_rank) buf[pos++] = '1' + m->from_rank;
            else {
                buf[pos++] = file_char[m->from_file];
                buf[pos++] = '1' + m->from_rank;
            }
        }
    } else {
        /* Pawn capture: include source file */
        if (m->captured || m->ep_capture)
            buf[pos++] = file_char[m->from_file];
    }

    if (m->captured || m->ep_capture) buf[pos++] = 'x';

    buf[pos++] = file_char[m->to_file];
    buf[pos++] = '1' + m->to_rank;

    if (m->promo) {
        buf[pos++] = '=';
        buf[pos++] = piece_char[m->promo];
    }

    buf[pos] = '\0';

    /* Check / checkmate */
    apply_move(b, m);
    int chk = is_in_check(b, b->turn);
    Move tmp[MAX_MOVES]; int tcnt=0;
    if (chk) {
        compute_legal_moves(b, tmp, &tcnt);
        if (tcnt == 0) buf[pos++] = '#';
        else           buf[pos++] = '+';
        buf[pos] = '\0';
    }
    unapply_move(b, m);

    strcpy(out, buf);
}

static void pgn_append_move(Move *m, int fullmove, int turn) {
    char tmp[64];
    int len = 0;
    if (turn == WHITE)
        len = snprintf(tmp, sizeof(tmp), "%d. %s ", fullmove, m->san);
    else
        len = snprintf(tmp, sizeof(tmp), "%s ", m->san);
    if (g_pgn_len + len < MAX_PGN_LEN - 1) {
        memcpy(g_pgn + g_pgn_len, tmp, len);
        g_pgn_len += len;
        g_pgn[g_pgn_len] = '\0';
    }
}

/* ─── UCI Engine ───────────────────────────────────────────────────────── */
static void engine_send(const char *msg) {
    if (!g_engine.active) return;
    write(g_engine.in_fd, msg, strlen(msg));
}

static int engine_launch(const char *path) {
    int to_engine[2], from_engine[2];
    if (pipe(to_engine)   < 0) return 0;
    if (pipe(from_engine) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        /* Child */
        dup2(to_engine[0],   STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[0]);  close(to_engine[1]);
        close(from_engine[0]); close(from_engine[1]);
        execlp(path, path, (char*)NULL);
        exit(1);
    }
    close(to_engine[0]);
    close(from_engine[1]);

    g_engine.pid    = pid;
    g_engine.in_fd  = to_engine[1];
    g_engine.out_fd = from_engine[0];
    g_engine.buf_len = 0;
    g_engine.active  = 1;
    g_engine.ready   = 0;
    g_engine.thinking= 0;
    g_engine.best_move[0] = '\0';

    /* Non-blocking read */
    int flags = fcntl(g_engine.out_fd, F_GETFL);
    fcntl(g_engine.out_fd, F_SETFL, flags | O_NONBLOCK);

    engine_send("uci\n");
    return 1;
}

/* Build position string for engine */
static void engine_send_position(void) {
    char buf[4096];
    int pos = 0;
    pos += snprintf(buf+pos, sizeof(buf)-pos, "position startpos moves");
    for (int i = 0; i < g_board.hist_count; i++) {
        char uci[8];
        move_to_uci(&g_board.history[i], uci);
        pos += snprintf(buf+pos, sizeof(buf)-pos, " %s", uci);
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos, "\n");
    engine_send(buf);
}

static void engine_start_thinking(void) {
    if (!g_engine.active || !g_engine.ready) return;
    engine_send("stop\n");
    engine_send_position();

    char go_cmd[128];
    switch (g_tc.mode) {
    case TC_NODES:
        snprintf(go_cmd, sizeof(go_cmd), "go nodes %ld\n", g_tc.nodes);
        break;
    case TC_DEPTH:
        snprintf(go_cmd, sizeof(go_cmd), "go depth %d\n", g_tc.depth);
        break;
    case TC_TIME:
        snprintf(go_cmd, sizeof(go_cmd), "go movetime %d\n", g_tc.ms);
        break;
    }
    engine_send(go_cmd);
    g_engine.thinking = 1;
}

static void engine_poll(void) {
    if (!g_engine.active) return;

    char tmp[1024];
    int n = read(g_engine.out_fd, tmp,
                 sizeof(tmp)-1 < (size_t)(ENGINE_BUF-g_engine.buf_len-1)
                 ? sizeof(tmp)-1 : ENGINE_BUF-g_engine.buf_len-1);
    if (n <= 0) return;

    /* Append to buffer */
    if (g_engine.buf_len + n < ENGINE_BUF) {
        memcpy(g_engine.buf + g_engine.buf_len, tmp, n);
        g_engine.buf_len += n;
        g_engine.buf[g_engine.buf_len] = '\0';
    }

    /* Process complete lines */
    char *line = g_engine.buf;
    char *nl;
    while ((nl = strchr(line, '\n')) != NULL) {
        *nl = '\0';

        if (strncmp(line, "uciok", 5) == 0) {
            engine_send("isready\n");
        } else if (strncmp(line, "readyok", 7) == 0) {
            g_engine.ready = 1;
            if (!g_engine.name[0])
                strcpy(g_engine.name, "UCI Engine");
            set_status("Engine ready: %s", g_engine.name);
        } else if (strncmp(line, "id name ", 8) == 0) {
            strncpy(g_engine.name, line+8, 127);
        } else if (strncmp(line, "bestmove", 8) == 0) {
            g_engine.thinking = 0;
            char mv[8] = {0};
            if (sscanf(line+9, "%7s", mv) == 1) {
                strcpy(g_engine.best_move, mv);
                /* Apply engine move */
                if (g_board.turn != g_player_color && g_engine_enabled) {
                    Move m;
                    if (move_from_uci(&g_board, mv, &m)) {
                        /* Compute legal and find match */
                        Move legal[MAX_MOVES]; int lc=0;
                        compute_legal_moves(&g_board, legal, &lc);
                        compute_san(&g_board, &m, legal, lc, m.san);
                        int full = g_board.fullmove;
                        int turn = g_board.turn;
                        apply_move(&g_board, &m);
                        pgn_append_move(&m, full, turn);
                        g_legal_dirty = 1;
                        render();
                    }
                }
            }
        }

        line = nl + 1;
    }

    /* Shift remaining */
    int remaining = g_engine.buf_len - (int)(line - g_engine.buf);
    if (remaining > 0) memmove(g_engine.buf, line, remaining);
    else remaining = 0;
    g_engine.buf_len = remaining;
    g_engine.buf[g_engine.buf_len] = '\0';
}

/* ─── Move UCI conversion ──────────────────────────────────────────────── */
static void move_to_uci(Move *m, char *out) {
    out[0] = 'a' + m->from_file;
    out[1] = '1' + m->from_rank;
    out[2] = 'a' + m->to_file;
    out[3] = '1' + m->to_rank;
    if (m->promo) {
        out[4] = tolower(piece_char[m->promo]);
        out[5] = '\0';
    } else {
        out[4] = '\0';
    }
}

static int move_from_uci(Board *b, const char *uci, Move *m) {
    if (strlen(uci) < 4) return 0;
    int ff = uci[0] - 'a';
    int fr = uci[1] - '1';
    int tf = uci[2] - 'a';
    int tr = uci[3] - '1';
    if (!in_bounds(ff,fr) || !in_bounds(tf,tr)) return 0;

    Move legal[MAX_MOVES]; int lc = 0;
    compute_legal_moves(b, legal, &lc);

    int promo = 0;
    if (uci[4]) {
        char p = uci[4];
        if (p=='q'||p=='Q') promo=QUEEN;
        else if(p=='r'||p=='R') promo=ROOK;
        else if(p=='b'||p=='B') promo=BISHOP;
        else if(p=='n'||p=='N') promo=KNIGHT;
    }

    for (int i = 0; i < lc; i++) {
        if (legal[i].from_file == ff && legal[i].from_rank == fr &&
            legal[i].to_file   == tf && legal[i].to_rank   == tr) {
            if (!promo || legal[i].promo == promo ||
                (promo == QUEEN && legal[i].promo)) {
                *m = legal[i];
                if (promo) m->promo = promo;
                return 1;
            }
        }
    }
    return 0;
}

/* ─── Status message ───────────────────────────────────────────────────── */
static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status_msg, sizeof(g_status_msg), fmt, ap);
    va_end(ap);
    g_status_timer = 80; /* ~4 seconds at ~50ms poll */
}

/* ─── Rendering ────────────────────────────────────────────────────────── */

/*
 * Layout (1-indexed terminal rows/cols):
 *  Row 1:    Title
 *  Row 2:    Engine info / status
 *  Row 3:    Separator
 *  Row 4-35: Board (4 rows per rank)
 *  Row 36:   File labels
 *  Row 37:   Separator
 *  Row 38:   Controls
 *  Row 39:   PGN header
 *  Row 40-50: PGN content
 *  Row 51:   Time control info
 */

#define BOARD_TOP    4
#define BOARD_LEFT   4
#define CELL_W       7
#define CELL_H       4

/* Color scheme */
#define CLR_LIGHT_SQ   210, 180, 140   /* tan */
#define CLR_DARK_SQ    101, 67,  33    /* brown */
#define CLR_HL_FROM    255, 215,  0    /* gold - selected from */
#define CLR_HL_TO      100, 200, 100   /* green - valid move target */
#define CLR_HL_CHECK   200,  50,  50   /* red - king in check */
#define CLR_HL_LAST    180, 120, 210   /* purple - last move */
#define CLR_CURSOR     255, 255, 100   /* yellow - cursor */
#define CLR_WHITE_PC   255, 255, 245   /* white piece text */
#define CLR_BLACK_PC    20,  20,  20   /* black piece text */
#define CLR_BORDER      80, 120, 160
#define CLR_TITLE      220, 200, 255

/* Unicode chess pieces */
static const char *piece_unicode[2][7] = {
    { " ", "♙", "♘", "♗", "♖", "♕", "♔" }, /* WHITE */
    { " ", "♟", "♞", "♝", "♜", "♛", "♚" }, /* BLACK */
};

/* Map squares to highlight states */
typedef enum {
    HL_NONE, HL_CURSOR, HL_FROM, HL_TO, HL_CHECK, HL_LAST
} HL;

static void set_sq_bg(int f, int r, HL hl, int is_light) {
    switch (hl) {
    case HL_CURSOR: BG(CLR_CURSOR); break;
    case HL_FROM:   BG(CLR_HL_FROM); break;
    case HL_TO:     BG(CLR_HL_TO); break;
    case HL_CHECK:  BG(CLR_HL_CHECK); break;
    case HL_LAST:   BG(CLR_HL_LAST); break;
    default:
        if (is_light) BG(CLR_LIGHT_SQ);
        else           BG(CLR_DARK_SQ);
        break;
    }
    (void)f; (void)r;
}

static int is_light(int f, int r) { return (f + r) % 2 == 1; }

static void render_board(void) {
    /* Compute highlights */
    HL hl[8][8];
    memset(hl, 0, sizeof(hl));

    /* Last move highlight */
    if (g_board.hist_count > 0) {
        Move *lm = &g_board.history[g_board.hist_count-1];
        hl[lm->from_file][lm->from_rank] = HL_LAST;
        hl[lm->to_file][lm->to_rank]     = HL_LAST;
    }

    /* Legal move targets from selected */
    if (g_from_file >= 0) {
        hl[g_from_file][g_from_rank] = HL_FROM;
        if (g_legal_dirty) {
            compute_legal_moves(&g_board, g_legal, &g_legal_count);
            g_legal_dirty = 0;
        }
        for (int i = 0; i < g_legal_count; i++) {
            if (g_legal[i].from_file == g_from_file &&
                g_legal[i].from_rank == g_from_rank) {
                if (hl[g_legal[i].to_file][g_legal[i].to_rank] == HL_NONE ||
                    hl[g_legal[i].to_file][g_legal[i].to_rank] == HL_LAST)
                    hl[g_legal[i].to_file][g_legal[i].to_rank] = HL_TO;
            }
        }
    }

    /* Check highlight */
    for (int f = 0; f < 8; f++) {
        for (int r = 0; r < 8; r++) {
            if (g_board.sq[f][r].piece == KING) {
                if (is_in_check(&g_board, g_board.sq[f][r].color)) {
                    hl[f][r] = HL_CHECK;
                }
            }
        }
    }

    /* Cursor */
    if (hl[g_sel_file][g_sel_rank] == HL_NONE ||
        hl[g_sel_file][g_sel_rank] == HL_LAST)
        hl[g_sel_file][g_sel_rank] = HL_CURSOR;

    /* Draw board: rank 8 at top */
    for (int r = 7; r >= 0; r--) {
        int row = BOARD_TOP + (7 - r) * CELL_H;

        /* Rank label */
        MOVE_TO(row + CELL_H/2, 2);
        FG(CLR_BORDER); printf("%d", r+1);

        for (int f = 0; f < 8; f++) {
            int col = BOARD_LEFT + f * CELL_W;
            int lt = is_light(f, r);
            HL h = hl[f][r];

            Square *sq = &g_board.sq[f][r];

            for (int dy = 0; dy < CELL_H; dy++) {
                MOVE_TO(row + dy, col);
                set_sq_bg(f, r, h, lt);

                if (dy == CELL_H/2) {
                    /* Piece row */
                    if (sq->piece) {
                        if (sq->color == WHITE) FG(CLR_WHITE_PC);
                        else                    FG(CLR_BLACK_PC);
                        /* Center piece in cell */
                        int pad_l = (CELL_W - 3) / 2; /* 3 chars for piece */
                        for (int p = 0; p < pad_l; p++) printf(" ");
                        printf("%s", piece_unicode[sq->color][sq->piece]);
                        int pad_r = CELL_W - pad_l - 3;
                        for (int p = 0; p < pad_r; p++) printf(" ");
                    } else {
                        for (int p = 0; p < CELL_W; p++) printf(" ");
                    }
                } else {
                    for (int p = 0; p < CELL_W; p++) printf(" ");
                }
                printf(RESET_COLOR);
            }
        }
    }

    /* File labels */
    int label_row = BOARD_TOP + 8 * CELL_H;
    MOVE_TO(label_row, BOARD_LEFT);
    FG(CLR_BORDER);
    for (int f = 0; f < 8; f++) {
        int center = f * CELL_W + CELL_W/2;
        MOVE_TO(label_row, BOARD_LEFT + center);
        printf("%c", 'a' + f);
    }
    printf(RESET_COLOR);
}

static void render_sidebar(void) {
    int start_col = BOARD_LEFT + 8 * CELL_W + 3;
    int row = BOARD_TOP;

    /* Turn indicator */
    MOVE_TO(row++, start_col);
    printf(RESET_COLOR BOLD);
    FG(CLR_TITLE);
    printf("%-30s", "");
    MOVE_TO(row-1, start_col);
    if (g_board.turn == WHITE)
        printf("Turn: " CSI "38;2;255;255;245m" "● WHITE");
    else
        printf("Turn: " CSI "38;2;50;50;50m" "● BLACK");
    printf(RESET_COLOR);
    row++;

    /* Check/mate status */
    MOVE_TO(row++, start_col);
    printf(RESET_COLOR);
    if (g_legal_dirty) {
        compute_legal_moves(&g_board, g_legal, &g_legal_count);
        g_legal_dirty = 0;
    }
    int chk = is_in_check(&g_board, g_board.turn);
    if (g_legal_count == 0) {
        if (chk) { FG(200,50,50); printf("%-30s", "CHECKMATE!"); }
        else      { FG(200,200,50); printf("%-30s", "STALEMATE!"); }
    } else if (chk) {
        FG(200,50,50); printf("%-30s", "CHECK!");
    } else {
        printf("%-30s", "");
    }
    printf(RESET_COLOR);
    row++;

    /* Cursor position */
    MOVE_TO(row++, start_col);
    FG(150,200,150);
    printf("Cursor: %c%d   ", 'a'+g_sel_file, g_sel_rank+1);
    if (g_from_file >= 0) {
        printf("From: %c%d", 'a'+g_from_file, g_from_rank+1);
    } else {
        printf("          ");
    }
    printf(RESET_COLOR);
    row++;

    /* Engine info */
    MOVE_TO(row++, start_col);
    FG(CLR_BORDER);
    printf("─────────────────────────────");
    printf(RESET_COLOR);

    MOVE_TO(row++, start_col);
    FG(180,180,220);
    if (g_engine.active) {
        printf("Engine: %-20s", g_engine.name[0] ? g_engine.name : "loading...");
        if (g_engine.thinking) {
            MOVE_TO(row++, start_col);
            FG(100,220,100); printf("  [Thinking...]         ");
        } else {
            MOVE_TO(row++, start_col);
            printf("  Best: %-22s", g_engine.best_move[0] ? g_engine.best_move : "---");
        }
    } else {
        printf("No engine loaded          ");
        row++;
    }
    printf(RESET_COLOR);
    row++;

    /* Time control */
    MOVE_TO(row++, start_col);
    FG(CLR_BORDER);
    printf("─────────────────────────────");
    printf(RESET_COLOR);

    MOVE_TO(row++, start_col);
    FG(200,200,150);
    printf("Time Control:              ");
    MOVE_TO(row++, start_col);
    switch (g_tc.mode) {
    case TC_NODES: printf("  Nodes: %-19ld", g_tc.nodes); break;
    case TC_DEPTH: printf("  Depth: %-19d",  g_tc.depth); break;
    case TC_TIME:  printf("  Time:  %-16d ms",  g_tc.ms);   break;
    }
    printf(RESET_COLOR);
    row++;

    /* Controls */
    MOVE_TO(row++, start_col);
    FG(CLR_BORDER);
    printf("─────────────────────────────");
    printf(RESET_COLOR);

    const char *ctrl[] = {
        "HJKL/Arrows : Move cursor",
        "Space/Enter : Select/Move",
        "U           : Undo",
        "T           : Time control menu",
        "E           : Toggle engine",
        "F           : Flip board (TODO)",
        "R           : Reset game",
        "Q           : Quit",
        NULL
    };
    for (int i = 0; ctrl[i]; i++) {
        MOVE_TO(row++, start_col);
        FG(160,160,200);
        printf("%-31s", ctrl[i]);
        printf(RESET_COLOR);
    }
    row++;

    /* Status message */
    MOVE_TO(row++, start_col);
    FG(CLR_BORDER);
    printf("─────────────────────────────");
    printf(RESET_COLOR);

    MOVE_TO(row++, start_col);
    if (g_status_timer > 0) {
        FG(220,200,100);
        printf("%-31s", g_status_msg);
    } else {
        printf("%-31s", "");
    }
    printf(RESET_COLOR);
}

static void render_pgn(void) {
    int pgn_row = BOARD_TOP + 8 * CELL_H + 3;
    int pgn_col = 1;
    int pgn_width = BOARD_LEFT + 8 * CELL_W + 55;

    MOVE_TO(pgn_row++, pgn_col);
    FG(CLR_BORDER);
    for (int i = 0; i < pgn_width && i < 100; i++) printf("─");
    printf(RESET_COLOR);

    MOVE_TO(pgn_row++, pgn_col);
    FG(CLR_TITLE); BOLD;
    printf("PGN: ");
    printf(RESET_COLOR);

    /* Word-wrap PGN into rows */
    const int max_pgn_rows = 6;
    const int line_width = pgn_width - 2;

    char pgn_copy[MAX_PGN_LEN];
    strncpy(pgn_copy, g_pgn, MAX_PGN_LEN-1);

    int len = strlen(pgn_copy);
    int start = 0;
    int lines_drawn = 0;

    /* Show last portion if long */
    if (len > max_pgn_rows * line_width) {
        start = len - max_pgn_rows * line_width;
        /* Align to word boundary */
        while (start < len && pgn_copy[start] != ' ') start++;
        if (start < len) start++;
    }

    char line_buf[256];
    int cur = start;
    for (int row = 0; row < max_pgn_rows && cur < len; row++) {
        int end = cur + line_width;
        if (end >= len) end = len;
        else {
            /* Back up to space */
            int e2 = end;
            while (e2 > cur && pgn_copy[e2] != ' ') e2--;
            if (e2 > cur) end = e2;
        }
        int ll = end - cur;
        if (ll <= 0) break;
        memcpy(line_buf, pgn_copy + cur, ll);
        line_buf[ll] = '\0';
        MOVE_TO(pgn_row++, pgn_col);
        FG(200,200,200);
        printf("%-*s", line_width, line_buf);
        printf(RESET_COLOR);
        cur = end;
        while (cur < len && pgn_copy[cur] == ' ') cur++;
        lines_drawn++;
    }
    /* Clear remaining rows */
    for (int row = lines_drawn; row < max_pgn_rows; row++) {
        MOVE_TO(pgn_row++, pgn_col);
        printf("%-*s", line_width, "");
    }
}

static void render_title(void) {
    MOVE_TO(1, 1);
    FG(CLR_TITLE); BOLD;
    printf(" ♔ CHESS TUI  ─  UCI Engine Interface  ♚ ");
    printf(RESET_COLOR);
    MOVE_TO(2, 1);
    FG(CLR_BORDER);
    printf("────────────────────────────────────────────────────────────────────────────────");
    printf(RESET_COLOR);
}

/* Promotion menu */
static void render_promo_menu(void) {
    int row = 12, col = 25;
    MOVE_TO(row++, col);
    BG(50,50,80); FG(CLR_TITLE); BOLD;
    printf("  Promote pawn to:  ");
    printf(RESET_COLOR);
    const char *opts[] = { "Q Queen", "R Rook", "B Bishop", "N Knight" };
    for (int i = 0; i < 4; i++) {
        MOVE_TO(row++, col);
        BG(40,40,70); FG(220,220,255);
        printf("  [%s]  ", opts[i]);
        printf(RESET_COLOR);
    }
}

/* Time control menu */
static void render_tc_menu(void) {
    int row = 10, col = 20;
    MOVE_TO(row++, col);
    BG(50,50,80); FG(CLR_TITLE); BOLD;
    printf("  Time Control Settings  ");
    printf(RESET_COLOR);

    const char *modes[] = { "N  Nodes", "D  Depth", "T  Time(ms)" };
    for (int i = 0; i < 3; i++) {
        MOVE_TO(row++, col);
        BG(40,40,70); FG(g_tc.mode == i ? 255 : 180,
                         g_tc.mode == i ? 255 : 180,
                         g_tc.mode == i ? 100 : 220);
        printf("  [%s]  ", modes[i]);
        printf(RESET_COLOR);
    }
    row++;
    MOVE_TO(row++, col);
    BG(30,30,60); FG(200,200,200);
    switch (g_tc.mode) {
    case TC_NODES: printf("  Value: %ld nodes  (+/-)", g_tc.nodes); break;
    case TC_DEPTH: printf("  Value: %d depth  (+/-)", g_tc.depth); break;
    case TC_TIME:  printf("  Value: %d ms     (+/-)", g_tc.ms); break;
    }
    printf(RESET_COLOR);
    MOVE_TO(row++, col);
    BG(30,30,60); FG(160,160,200);
    printf("  Esc to close             ");
    printf(RESET_COLOR);
}

static void render(void) {
    /* Don't clear screen, just repaint everything in place */
    printf(HIDE_CURSOR);
    render_title();
    render_board();
    render_sidebar();
    render_pgn();
    if (g_in_menu)  render_tc_menu();
    if (g_in_promo) render_promo_menu();
    fflush(stdout);
}

/* ─── Input handling ───────────────────────────────────────────────────── */
static int read_key(void) {
    unsigned char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    if (c == 27) { /* ESC sequence */
        unsigned char seq[4];
        int sn = read(STDIN_FILENO, seq, 3);
        if (sn > 0 && seq[0] == '[') {
            switch (seq[1]) {
            case 'A': return 1000; /* Up    */
            case 'B': return 1001; /* Down  */
            case 'C': return 1002; /* Right */
            case 'D': return 1003; /* Left  */
            }
        }
        return 27; /* plain ESC */
    }
    return c;
}

/* Try to make a move from g_from to (tf,tr) */
static int try_move(int tf, int tr, int promo) {
    if (g_legal_dirty) {
        compute_legal_moves(&g_board, g_legal, &g_legal_count);
        g_legal_dirty = 0;
    }
    for (int i = 0; i < g_legal_count; i++) {
        Move *m = &g_legal[i];
        if (m->from_file == g_from_file && m->from_rank == g_from_rank &&
            m->to_file == tf && m->to_rank == tr) {
            /* Promotion? */
            if (m->promo && !promo) {
                /* Multiple promotion choices possible – show menu */
                g_in_promo = 1;
                g_promo_file = tf; g_promo_rank = tr;
                g_promo_from_file = g_from_file;
                g_promo_from_rank = g_from_rank;
                return 2; /* pending */
            }
            if (m->promo && promo && m->promo != promo) continue;

            /* Compute SAN before applying */
            compute_san(&g_board, m, g_legal, g_legal_count, m->san);
            int full = g_board.fullmove;
            int turn = g_board.turn;
            apply_move(&g_board, m);
            pgn_append_move(m, full, turn);
            g_legal_dirty = 1;
            g_from_file = -1; g_from_rank = -1;
            set_status("Moved: %s", m->san);

            /* Let engine think if it's engine's turn */
            if (g_engine_enabled && g_engine.active && g_engine.ready &&
                g_board.turn != g_player_color) {
                engine_start_thinking();
            }
            return 1;
        }
    }
    return 0;
}

static void handle_promo_key(int key) {
    int promo = 0;
    if (key == 'q' || key == 'Q') promo = QUEEN;
    else if (key == 'r' || key == 'R') promo = ROOK;
    else if (key == 'b' || key == 'B') promo = BISHOP;
    else if (key == 'n' || key == 'N') promo = KNIGHT;
    else if (key == 27) { g_in_promo = 0; return; }
    else return;

    g_in_promo = 0;
    g_from_file = g_promo_from_file;
    g_from_rank = g_promo_from_rank;
    if (!try_move(g_promo_file, g_promo_rank, promo)) {
        set_status("Invalid promotion move");
        g_from_file = -1;
    }
}

static void handle_tc_key(int key) {
    switch(key) {
    case 'n': case 'N': g_tc.mode = TC_NODES; break;
    case 'd': case 'D': g_tc.mode = TC_DEPTH; break;
    case 't': case 'T': g_tc.mode = TC_TIME;  break;
    case '+': case '=':
        if (g_tc.mode == TC_NODES) g_tc.nodes = g_tc.nodes ? g_tc.nodes*2 : 1000;
        else if (g_tc.mode == TC_DEPTH) g_tc.depth++;
        else g_tc.ms = g_tc.ms < 60000 ? g_tc.ms + 500 : g_tc.ms;
        break;
    case '-':
        if (g_tc.mode == TC_NODES) { if(g_tc.nodes>1000) g_tc.nodes/=2; }
        else if (g_tc.mode == TC_DEPTH) { if(g_tc.depth>1) g_tc.depth--; }
        else { if(g_tc.ms>100) g_tc.ms-=500; }
        break;
    case 27: case 'q': g_in_menu = 0; break;
    }
}

static void handle_key(int key) {
    if (g_in_promo) { handle_promo_key(key); return; }
    if (g_in_menu)  { handle_tc_key(key); return; }

    /* Only allow human input on their turn */
    int human_turn = (g_board.turn == g_player_color) ||
                     (!g_engine_enabled);

    switch (key) {
    /* Movement */
    case 'h': case 1003: /* left  */ g_sel_file = (g_sel_file+7)%8; break;
    case 'l': case 1002: /* right */ g_sel_file = (g_sel_file+1)%8; break;
    case 'k': case 1000: /* up    */ g_sel_rank = (g_sel_rank+1)%8; break;
    case 'j': case 1001: /* down  */ g_sel_rank = (g_sel_rank+7)%8; break;

    case ' ': case '\r': case '\n':
        if (!human_turn) { set_status("Not your turn!"); break; }
        if (g_from_file < 0) {
            /* Select piece */
            Square *sq = &g_board.sq[g_sel_file][g_sel_rank];
            if (sq->piece && sq->color == g_board.turn) {
                g_from_file = g_sel_file;
                g_from_rank = g_sel_rank;
                if (g_legal_dirty) {
                    compute_legal_moves(&g_board, g_legal, &g_legal_count);
                    g_legal_dirty = 0;
                }
            } else {
                set_status("No piece to select here");
            }
        } else {
            /* Try to move */
            if (g_sel_file == g_from_file && g_sel_rank == g_from_rank) {
                /* Deselect */
                g_from_file = -1; g_from_rank = -1;
            } else {
                int r = try_move(g_sel_file, g_sel_rank, 0);
                if (r == 0) {
                    /* Maybe selecting another piece */
                    Square *sq = &g_board.sq[g_sel_file][g_sel_rank];
                    if (sq->piece && sq->color == g_board.turn) {
                        g_from_file = g_sel_file;
                        g_from_rank = g_sel_rank;
                    } else {
                        set_status("Illegal move!");
                        g_from_file = -1;
                    }
                } else if (r == 2) {
                    /* Promo pending */
                }
            }
        }
        break;

    case 27: /* ESC – deselect */
        g_from_file = -1; g_from_rank = -1;
        break;

    case 'u': case 'U': /* Undo */
        if (g_board.hist_count > 0) {
            if (g_engine_enabled && g_board.hist_count >= 2) {
                /* Undo two moves */
                unapply_move(&g_board, &g_board.history[g_board.hist_count-1]);
                /* Trim PGN */
                if (g_pgn_len > 0) {
                    g_pgn_len = 0; g_pgn[0]='\0';
                    for (int i = 0; i < g_board.hist_count; i++)
                        pgn_append_move(&g_board.history[i],
                            (i/2)+1, i%2==0 ? WHITE : BLACK);
                }
                unapply_move(&g_board, &g_board.history[g_board.hist_count-1]);
                if (g_pgn_len > 0) {
                    g_pgn_len = 0; g_pgn[0]='\0';
                    for (int i = 0; i < g_board.hist_count; i++)
                        pgn_append_move(&g_board.history[i],
                            (i/2)+1, i%2==0 ? WHITE : BLACK);
                }
            } else {
                unapply_move(&g_board, &g_board.history[g_board.hist_count-1]);
                /* Rebuild PGN */
                g_pgn_len = 0; g_pgn[0]='\0';
                for (int i = 0; i < g_board.hist_count; i++)
                    pgn_append_move(&g_board.history[i],
                        (i/2)+1, i%2==0 ? WHITE : BLACK);
            }
            g_from_file = -1; g_from_rank = -1;
            g_legal_dirty = 1;
            set_status("Move undone");
        } else {
            set_status("Nothing to undo");
        }
        break;

    case 'e': case 'E':
        if (g_engine.active) {
            g_engine_enabled = !g_engine_enabled;
            set_status("Engine %s", g_engine_enabled ? "enabled" : "disabled");
            if (g_engine_enabled && g_board.turn != g_player_color) {
                engine_start_thinking();
            }
        } else {
            set_status("No engine loaded (pass path as argument)");
        }
        break;

    case 't': case 'T':
        g_in_menu = 1;
        break;

    case 'r': case 'R':
        board_init(&g_board);
        g_from_file = -1; g_from_rank = -1;
        g_sel_file = 0; g_sel_rank = 0;
        g_pgn_len = 0; g_pgn[0] = '\0';
        g_legal_dirty = 1;
        engine_send("ucinewgame\n");
        set_status("Game reset");
        break;

    case 'q': case 'Q':
        g_running = 0;
        break;
    }
}

/* ─── Main ─────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Check terminal size */
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    if (ws.ws_col < 100 || ws.ws_row < 40) {
        fprintf(stderr,
            "Terminal too small! Need at least 100x40 (got %dx%d)\n"
            "Resize your terminal and try again.\n",
            ws.ws_col, ws.ws_row);
        return 1;
    }

    board_init(&g_board);
    memset(&g_engine, 0, sizeof(g_engine));
    g_engine.active = 0;

    /* Default TC */
    g_tc.mode  = TC_TIME;
    g_tc.ms    = 1000;
    g_tc.depth = 5;
    g_tc.nodes = 100000;

    /* Launch engine if provided */
    if (argc > 1) {
        if (engine_launch(argv[1])) {
            g_engine_enabled = 1;
            set_status("Launching engine: %s", argv[1]);
        } else {
            set_status("Failed to launch engine");
        }
    }

    term_raw();
    printf(CLEAR_SCREEN HIDE_CURSOR);

    /* Initial render */
    render();

    /* Main loop */
    struct timespec ts = { 0, 20000000 }; /* 20ms */
    while (g_running) {
        /* Poll engine output */
        engine_poll();

        /* Check for engine move when it's engine's turn */
        if (g_engine_enabled && g_engine.active && g_engine.ready &&
            !g_engine.thinking && g_board.turn != g_player_color) {
            /* Check if legal moves exist (not game over) */
            if (g_legal_dirty) {
                compute_legal_moves(&g_board, g_legal, &g_legal_count);
                g_legal_dirty = 0;
            }
            if (g_legal_count > 0) {
                engine_start_thinking();
            }
        }

        /* Read input (non-blocking) */
        int key = read_key();
        if (key >= 0) {
            handle_key(key);
            render();
        }

        /* Status timer */
        if (g_status_timer > 0) {
            g_status_timer--;
            if (g_status_timer == 0) render();
        }

        /* Periodic re-render for engine status */
        static int frame = 0;
        if (++frame >= 5) { /* every 100ms */
            frame = 0;
            render();
        }

        nanosleep(&ts, NULL);
    }

    cleanup();
    printf(CLEAR_SCREEN);
    MOVE_TO(1,1);
    printf("Thanks for playing!\n");
    return 0;
}
