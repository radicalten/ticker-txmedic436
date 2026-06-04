/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 * Compile: gcc -o chess_gui chess_gui.c
 * Run: ./chess_gui [engine_path]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

/* ─── ANSI / Terminal ─────────────────────────────────────────────────────── */
#define ESC "\033["
#define CLEAR_SCREEN()   printf("\033[2J\033[H")
#define MOVE_CURSOR(r,c) printf("\033[%d;%dH",(r),(c))
#define HIDE_CURSOR()    printf("\033[?25l")
#define SHOW_CURSOR()    printf("\033[?25h")
#define RESET_COLOR()    printf("\033[0m")

/* Colors */
#define BG_LIGHT      "\033[48;5;223m"   /* light square   */
#define BG_DARK       "\033[48;5;136m"   /* dark square    */
#define BG_SELECT     "\033[48;5;226m"   /* selected piece */
#define BG_LEGAL      "\033[48;5;154m"   /* legal move dot */
#define BG_LAST_MOVE  "\033[48;5;214m"   /* last move highlight */
#define BG_CHECK      "\033[48;5;196m"   /* king in check  */
#define FG_WHITE_PC   "\033[38;5;255m"   /* white piece    */
#define FG_BLACK_PC   "\033[38;5;16m"    /* black piece    */
#define FG_BORDER     "\033[38;5;240m"   /* border text    */
#define BG_PANEL      "\033[48;5;235m"   /* side panel     */
#define FG_PANEL      "\033[38;5;250m"   /* side panel text*/
#define FG_TITLE      "\033[38;5;220m"   /* title          */
#define FG_STATUS     "\033[38;5;46m"    /* status green   */
#define FG_ERROR      "\033[38;5;196m"   /* error red      */
#define FG_HIGHLIGHT  "\033[38;5;226m"   /* highlight      */
#define BOLD          "\033[1m"

/* ─── Chess Constants ─────────────────────────────────────────────────────── */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  0
#define BLACK  1

#define MAX_MOVES      256
#define MAX_HISTORY    512
#define MAX_PGN_LEN    8192
#define MAX_LEGAL      128
#define ENGINE_BUF     4096

/* Castling flags */
#define CASTLE_WK  0x1
#define CASTLE_WQ  0x2
#define CASTLE_BK  0x4
#define CASTLE_BQ  0x8

/* ─── Data Structures ─────────────────────────────────────────────────────── */
typedef struct {
    int piece;   /* PAWN..KING or EMPTY */
    int color;   /* WHITE or BLACK      */
} Square;

typedef struct {
    int from_sq;          /* 0-63                    */
    int to_sq;            /* 0-63                    */
    int piece;            /* piece type moved        */
    int color;            /* color of moving piece   */
    int captured;         /* captured piece type     */
    int captured_color;
    int promoted;         /* promotion piece, or 0   */
    int castle;           /* 0=none,1=KS,2=QS        */
    int ep_capture;       /* en passant captured sq  */
    int prev_ep;          /* previous ep square      */
    int prev_castle;      /* previous castling rights*/
    int prev_halfmove;
    char pgn[16];         /* PGN notation            */
} Move;

typedef struct {
    Square board[64];
    int    side;           /* WHITE or BLACK to move  */
    int    castle_rights;  /* bitmask CASTLE_*        */
    int    ep_square;      /* -1 or target square     */
    int    halfmove;       /* 50-move rule counter    */
    int    fullmove;
    int    in_check;
} Position;

/* Time control modes */
typedef enum { TC_TIME, TC_DEPTH, TC_NODES } TCMode;

typedef struct {
    TCMode mode;
    int    time_ms;   /* ms per move  */
    int    depth;
    long   nodes;
} TimeControl;

/* Game state */
typedef struct {
    Position   pos;
    Move       history[MAX_HISTORY];
    int        hist_count;
    char       pgn_buf[MAX_PGN_LEN];
    int        pgn_len;
    int        last_from;     /* -1 or square */
    int        last_to;
    /* Selection */
    int        cursor;        /* 0-63 cursor square   */
    int        selected;      /* -1 or selected sq    */
    int        legal[MAX_LEGAL];
    int        legal_count;
    /* Engine */
    int        eng_in[2];
    int        eng_out[2];
    pid_t      eng_pid;
    int        engine_active;
    char       engine_path[256];
    int        engine_color;  /* which color engine plays, -1=none */
    /* Time control */
    TimeControl tc;
    /* UI state */
    int        flipped;
    char       status_msg[128];
    int        status_is_error;
    int        game_over;
    char       game_result[8];
    /* Terminal */
    int        term_rows;
    int        term_cols;
} GameState;

/* ─── Globals ─────────────────────────────────────────────────────────────── */
static GameState G;
static struct termios orig_termios;

/* ─── Forward Declarations ────────────────────────────────────────────────── */
static void init_position(Position *p);
static void generate_legal_moves(const Position *p, int sq, int *moves, int *count);
static int  is_in_check(const Position *p, int color);
static void make_move(Position *p, Move *m);
static void unmake_move(Position *p, const Move *m);
static void apply_move_to_game(GameState *g, Move *m);
static int  move_leaves_in_check(const Position *p, const Move *m);
static void generate_pgn(GameState *g, Move *m, const Position *before);
static void render(GameState *g);
static void process_key(GameState *g, int ch);
static void engine_send(GameState *g, const char *cmd);
static void engine_read_move(GameState *g);
static void set_status(GameState *g, const char *msg, int is_error);
static int  has_any_legal_moves(const Position *p);
static int  col_of(int sq);
static int  row_of(int sq);
static int  sq_of(int r, int c);

/* ─── Terminal Setup ──────────────────────────────────────────────────────── */
static void term_raw(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    SHOW_CURSOR();
    printf("\033[0m\n");
}

static void get_term_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 40;
        *cols = 120;
    }
}

static void handle_sigwinch(int sig) {
    (void)sig;
    get_term_size(&G.term_rows, &G.term_cols);
    render(&G);
}

static void handle_sigint(int sig) {
    (void)sig;
    term_restore();
    CLEAR_SCREEN();
    MOVE_CURSOR(1,1);
    exit(0);
}

/* ─── Helpers ─────────────────────────────────────────────────────────────── */
static int col_of(int sq) { return sq & 7; }
static int row_of(int sq) { return sq >> 3; }
static int sq_of(int r, int c) { return (r << 3) | c; }

static char piece_char(int piece, int color) {
    const char *w = ".PNBRQK";
    const char *b = ".pnbrqk";
    if (piece < 0 || piece > 6) return '?';
    return color == WHITE ? w[piece] : b[piece];
}

/* Unicode pieces */
static const char *piece_unicode(int piece, int color) {
    if (piece == EMPTY) return " ";
    static const char *wp[] = {"","♙","♘","♗","♖","♕","♔"};
    static const char *bp[] = {"","♟","♞","♝","♜","♛","♚"};
    if (piece < 1 || piece > 6) return "?";
    return color == WHITE ? wp[piece] : bp[piece];
}

static const char *piece_name(int piece) {
    static const char *n[] = {"","Pawn","Knight","Bishop","Rook","Queen","King"};
    if (piece < 0 || piece > 6) return "?";
    return n[piece];
}

/* ─── Position Init ───────────────────────────────────────────────────────── */
static void init_position(Position *p) {
    memset(p, 0, sizeof(*p));
    p->ep_square = -1;
    p->castle_rights = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    p->side = WHITE;
    p->fullmove = 1;

    /* Back ranks */
    int back_pieces[] = {ROOK,KNIGHT,BISHOP,QUEEN,KING,BISHOP,KNIGHT,ROOK};
    for (int c = 0; c < 8; c++) {
        p->board[sq_of(0,c)].piece = back_pieces[c];
        p->board[sq_of(0,c)].color = WHITE;
        p->board[sq_of(7,c)].piece = back_pieces[c];
        p->board[sq_of(7,c)].color = BLACK;
        p->board[sq_of(1,c)].piece = PAWN;
        p->board[sq_of(1,c)].color = WHITE;
        p->board[sq_of(6,c)].piece = PAWN;
        p->board[sq_of(6,c)].color = BLACK;
    }
}

/* ─── Move Generation ─────────────────────────────────────────────────────── */
/* Add a pseudo-legal move candidate to list */
static int add_pseudo(const Position *p, int from, int to, int *moves, int *count) {
    if (to < 0 || to > 63) return 0;
    int cap_color = p->board[to].color;
    int cap_piece = p->board[to].piece;
    if (cap_piece != EMPTY && cap_color == p->board[from].color) return 0;
    moves[(*count)++] = to;
    return cap_piece != EMPTY; /* 1 = blocked after */
}

static void gen_sliding(const Position *p, int from, int dr, int dc,
                        int *moves, int *count) {
    int r = row_of(from), c = col_of(from);
    for (int i = 1; i < 8; i++) {
        int nr = r + dr*i, nc = c + dc*i;
        if (nr < 0 || nr > 7 || nc < 0 || nc > 7) break;
        int to = sq_of(nr, nc);
        if (add_pseudo(p, from, to, moves, count)) break;
    }
}

static void generate_pseudo_moves(const Position *p, int sq,
                                  int *moves, int *count) {
    *count = 0;
    int piece = p->board[sq].piece;
    int color = p->board[sq].color;
    if (piece == EMPTY) return;
    int r = row_of(sq), c = col_of(sq);

    switch (piece) {
    case PAWN: {
        int dir = (color == WHITE) ? 1 : -1;
        int start_row = (color == WHITE) ? 1 : 6;
        /* Forward one */
        int nr = r + dir, to;
        if (nr >= 0 && nr <= 7) {
            to = sq_of(nr, c);
            if (p->board[to].piece == EMPTY) {
                moves[(*count)++] = to;
                /* Forward two from start */
                if (r == start_row) {
                    int to2 = sq_of(r + 2*dir, c);
                    if (p->board[to2].piece == EMPTY)
                        moves[(*count)++] = to2;
                }
            }
        }
        /* Captures */
        int cap_cols[] = {c-1, c+1};
        for (int i = 0; i < 2; i++) {
            int nc = cap_cols[i];
            if (nc < 0 || nc > 7 || nr < 0 || nr > 7) continue;
            to = sq_of(nr, nc);
            if ((p->board[to].piece != EMPTY && p->board[to].color != color) ||
                 to == p->ep_square)
                moves[(*count)++] = to;
        }
        break;
    }
    case KNIGHT: {
        int kd[][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr2 = r + kd[i][0], nc2 = c + kd[i][1];
            if (nr2 < 0 || nr2 > 7 || nc2 < 0 || nc2 > 7) continue;
            add_pseudo(p, sq, sq_of(nr2,nc2), moves, count);
        }
        break;
    }
    case BISHOP:
        gen_sliding(p, sq,  1,  1, moves, count);
        gen_sliding(p, sq,  1, -1, moves, count);
        gen_sliding(p, sq, -1,  1, moves, count);
        gen_sliding(p, sq, -1, -1, moves, count);
        break;
    case ROOK:
        gen_sliding(p, sq,  1,  0, moves, count);
        gen_sliding(p, sq, -1,  0, moves, count);
        gen_sliding(p, sq,  0,  1, moves, count);
        gen_sliding(p, sq,  0, -1, moves, count);
        break;
    case QUEEN:
        gen_sliding(p, sq,  1,  0, moves, count);
        gen_sliding(p, sq, -1,  0, moves, count);
        gen_sliding(p, sq,  0,  1, moves, count);
        gen_sliding(p, sq,  0, -1, moves, count);
        gen_sliding(p, sq,  1,  1, moves, count);
        gen_sliding(p, sq,  1, -1, moves, count);
        gen_sliding(p, sq, -1,  1, moves, count);
        gen_sliding(p, sq, -1, -1, moves, count);
        break;
    case KING: {
        int kd2[][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
        for (int i = 0; i < 8; i++) {
            int nr2 = r + kd2[i][0], nc2 = c + kd2[i][1];
            if (nr2 < 0 || nr2 > 7 || nc2 < 0 || nc2 > 7) continue;
            add_pseudo(p, sq, sq_of(nr2,nc2), moves, count);
        }
        /* Castling */
        if (color == WHITE && r == 0 && c == 4) {
            /* Kingside */
            if ((p->castle_rights & CASTLE_WK) &&
                p->board[sq_of(0,5)].piece == EMPTY &&
                p->board[sq_of(0,6)].piece == EMPTY)
                moves[(*count)++] = sq_of(0,6);
            /* Queenside */
            if ((p->castle_rights & CASTLE_WQ) &&
                p->board[sq_of(0,3)].piece == EMPTY &&
                p->board[sq_of(0,2)].piece == EMPTY &&
                p->board[sq_of(0,1)].piece == EMPTY)
                moves[(*count)++] = sq_of(0,2);
        }
        if (color == BLACK && r == 7 && c == 4) {
            if ((p->castle_rights & CASTLE_BK) &&
                p->board[sq_of(7,5)].piece == EMPTY &&
                p->board[sq_of(7,6)].piece == EMPTY)
                moves[(*count)++] = sq_of(7,6);
            if ((p->castle_rights & CASTLE_BQ) &&
                p->board[sq_of(7,3)].piece == EMPTY &&
                p->board[sq_of(7,2)].piece == EMPTY &&
                p->board[sq_of(7,1)].piece == EMPTY)
                moves[(*count)++] = sq_of(7,2);
        }
        break;
    }
    }
}

/* Check if a square is attacked by given color */
static int square_attacked(const Position *p, int sq, int by_color) {
    /* Try all opponent pieces */
    for (int from = 0; from < 64; from++) {
        if (p->board[from].piece == EMPTY) continue;
        if (p->board[from].color != by_color) continue;
        int moves[MAX_LEGAL], cnt;
        /* For attack detection, skip castling & ep edge cases */
        generate_pseudo_moves(p, from, moves, &cnt);
        for (int i = 0; i < cnt; i++)
            if (moves[i] == sq) return 1;
    }
    return 0;
}

static int is_in_check(const Position *p, int color) {
    /* Find king */
    for (int sq = 0; sq < 64; sq++) {
        if (p->board[sq].piece == KING && p->board[sq].color == color) {
            return square_attacked(p, sq, 1 - color);
        }
    }
    return 0;
}

/* Test if a move leaves the mover's king in check */
static int move_leaves_in_check(const Position *p, const Move *m) {
    Position tmp = *p;
    /* Quick apply without full bookkeeping */
    int from = m->from_sq, to = m->to_sq;
    int color = tmp.board[from].color;
    int piece = tmp.board[from].piece;

    tmp.board[to] = tmp.board[from];
    tmp.board[from].piece = EMPTY;

    /* Promotion */
    if (m->promoted) tmp.board[to].piece = m->promoted;

    /* En passant */
    if (piece == PAWN && to == p->ep_square && p->ep_square != -1) {
        int cap_sq = sq_of(row_of(from), col_of(to));
        tmp.board[cap_sq].piece = EMPTY;
    }

    /* Castling rook */
    if (piece == KING) {
        int dc = col_of(to) - col_of(from);
        if (abs(dc) == 2) {
            int row = row_of(from);
            if (dc == 2) { /* kingside */
                tmp.board[sq_of(row,5)] = tmp.board[sq_of(row,7)];
                tmp.board[sq_of(row,7)].piece = EMPTY;
            } else { /* queenside */
                tmp.board[sq_of(row,3)] = tmp.board[sq_of(row,0)];
                tmp.board[sq_of(row,0)].piece = EMPTY;
            }
        }
    }

    return is_in_check(&tmp, color);
}

/* Castling legality: king must not pass through attacked squares */
static int castle_path_attacked(const Position *p, int color, int kingside) {
    int row = (color == WHITE) ? 0 : 7;
    int opp = 1 - color;
    if (kingside) {
        return square_attacked(p, sq_of(row,4), opp) ||
               square_attacked(p, sq_of(row,5), opp) ||
               square_attacked(p, sq_of(row,6), opp);
    } else {
        return square_attacked(p, sq_of(row,4), opp) ||
               square_attacked(p, sq_of(row,3), opp) ||
               square_attacked(p, sq_of(row,2), opp);
    }
}

static void generate_legal_moves(const Position *p, int sq,
                                 int *moves, int *count) {
    *count = 0;
    if (p->board[sq].piece == EMPTY) return;
    if (p->board[sq].color != p->side) return;

    int pseudo[MAX_LEGAL], pcnt;
    generate_pseudo_moves(p, sq, pseudo, &pcnt);

    int piece = p->board[sq].piece;
    int color = p->board[sq].color;
    int r = row_of(sq), c = col_of(sq);

    for (int i = 0; i < pcnt; i++) {
        int to = pseudo[i];

        /* Castling extra checks */
        if (piece == KING && abs(col_of(to) - c) == 2) {
            /* Make sure the rook is actually there */
            int row2 = (color == WHITE) ? 0 : 7;
            int ks = (col_of(to) == 6);
            int rook_sq = sq_of(row2, ks ? 7 : 0);
            if (p->board[rook_sq].piece != ROOK ||
                p->board[rook_sq].color != color) continue;
            /* King can't castle out of or through check */
            if (castle_path_attacked(p, color, ks)) continue;
        }

        Move m = {0};
        m.from_sq = sq;
        m.to_sq   = to;
        m.piece   = piece;
        m.color   = color;
        m.captured = p->board[to].piece;
        m.captured_color = p->board[to].color;

        /* En passant */
        if (piece == PAWN && to == p->ep_square && p->ep_square != -1)
            m.ep_capture = sq_of(r, col_of(to));

        if (!move_leaves_in_check(p, &m))
            moves[(*count)++] = to;
    }
    (void)r;
}

static int has_any_legal_moves(const Position *p) {
    for (int sq = 0; sq < 64; sq++) {
        if (p->board[sq].piece == EMPTY) continue;
        if (p->board[sq].color != p->side) continue;
        int moves[MAX_LEGAL], cnt;
        generate_legal_moves(p, sq, moves, &cnt);
        if (cnt > 0) return 1;
    }
    return 0;
}

/* ─── Make / Unmake Move ──────────────────────────────────────────────────── */
static void make_move(Position *p, Move *m) {
    int from = m->from_sq, to = m->to_sq;
    int piece = p->board[from].piece;
    int color = p->board[from].color;

    m->prev_ep      = p->ep_square;
    m->prev_castle  = p->castle_rights;
    m->prev_halfmove= p->halfmove;
    m->piece        = piece;
    m->color        = color;
    m->captured     = p->board[to].piece;
    m->captured_color= p->board[to].color;

    /* Half-move clock */
    if (piece == PAWN || p->board[to].piece != EMPTY)
        p->halfmove = 0;
    else
        p->halfmove++;

    /* En passant clear */
    p->ep_square = -1;

    /* Move piece */
    p->board[to] = p->board[from];
    p->board[from].piece = EMPTY;

    /* Pawn special */
    if (piece == PAWN) {
        int dr = row_of(to) - row_of(from);
        /* Double push: set ep square */
        if (abs(dr) == 2)
            p->ep_square = sq_of(row_of(from) + dr/2, col_of(from));
        /* En passant capture */
        if (m->ep_capture) {
            m->captured = PAWN;
            m->captured_color = 1 - color;
            p->board[m->ep_capture].piece = EMPTY;
        }
        /* Promotion */
        if (row_of(to) == 7 || row_of(to) == 0) {
            if (!m->promoted) m->promoted = QUEEN;
            p->board[to].piece = m->promoted;
        }
    }

    /* Castling rook move */
    if (piece == KING) {
        int dc = col_of(to) - col_of(from);
        if (abs(dc) == 2) {
            int row = row_of(from);
            if (dc == 2) {
                m->castle = 1;
                p->board[sq_of(row,5)] = p->board[sq_of(row,7)];
                p->board[sq_of(row,7)].piece = EMPTY;
            } else {
                m->castle = 2;
                p->board[sq_of(row,3)] = p->board[sq_of(row,0)];
                p->board[sq_of(row,0)].piece = EMPTY;
            }
        }
        /* Remove castling rights */
        if (color == WHITE) p->castle_rights &= ~(CASTLE_WK|CASTLE_WQ);
        else                p->castle_rights &= ~(CASTLE_BK|CASTLE_BQ);
    }

    /* Update castling rights when rook moves */
    if (piece == ROOK) {
        if (from == sq_of(0,0)) p->castle_rights &= ~CASTLE_WQ;
        if (from == sq_of(0,7)) p->castle_rights &= ~CASTLE_WK;
        if (from == sq_of(7,0)) p->castle_rights &= ~CASTLE_BQ;
        if (from == sq_of(7,7)) p->castle_rights &= ~CASTLE_BK;
    }

    /* Update castling rights when rook is captured */
    if (m->captured == ROOK) {
        if (to == sq_of(0,0)) p->castle_rights &= ~CASTLE_WQ;
        if (to == sq_of(0,7)) p->castle_rights &= ~CASTLE_WK;
        if (to == sq_of(7,0)) p->castle_rights &= ~CASTLE_BQ;
        if (to == sq_of(7,7)) p->castle_rights &= ~CASTLE_BK;
    }

    if (color == BLACK) p->fullmove++;
    p->side = 1 - p->side;
    p->in_check = is_in_check(p, p->side);
}

static void unmake_move(Position *p, const Move *m) {
    int from = m->from_sq, to = m->to_sq;
    int color = m->color;

    p->side = color;
    if (color == BLACK) p->fullmove--;

    /* Move piece back */
    p->board[from] = p->board[to];
    p->board[from].piece = m->piece;  /* undo promotion */
    p->board[to].piece   = EMPTY;

    /* Restore captured piece */
    if (m->ep_capture) {
        p->board[m->ep_capture].piece = PAWN;
        p->board[m->ep_capture].color = m->captured_color;
    } else if (m->captured) {
        p->board[to].piece = m->captured;
        p->board[to].color = m->captured_color;
    }

    /* Undo castling rook */
    if (m->castle == 1) { /* kingside */
        int row = row_of(from);
        p->board[sq_of(row,7)] = p->board[sq_of(row,5)];
        p->board[sq_of(row,5)].piece = EMPTY;
    } else if (m->castle == 2) { /* queenside */
        int row = row_of(from);
        p->board[sq_of(row,0)] = p->board[sq_of(row,3)];
        p->board[sq_of(row,3)].piece = EMPTY;
    }

    p->ep_square     = m->prev_ep;
    p->castle_rights = m->prev_castle;
    p->halfmove      = m->prev_halfmove;
    p->in_check      = is_in_check(p, p->side);
}

/* ─── PGN Notation ────────────────────────────────────────────────────────── */
static void generate_pgn(GameState *g, Move *m, const Position *before) {
    (void)before;
    char buf[16];
    int  pos = 0;
    int  piece = m->piece;
    int  from  = m->from_sq;
    int  to    = m->to_sq;
    const char *files = "abcdefgh";

    if (m->castle == 1) { strcpy(m->pgn, "O-O");   return; }
    if (m->castle == 2) { strcpy(m->pgn, "O-O-O"); return; }

    if (piece != PAWN) {
        buf[pos++] = "?PNBRQK"[piece];
        /* Disambiguation: check if another same piece can go to same square */
        int need_file = 0, need_rank = 0;
        for (int sq = 0; sq < 64; sq++) {
            if (sq == from) continue;
            if (before->board[sq].piece != piece) continue;
            if (before->board[sq].color != m->color) continue;
            int lmoves[MAX_LEGAL], lcnt;
            generate_legal_moves(before, sq, lmoves, &lcnt);
            for (int i = 0; i < lcnt; i++) {
                if (lmoves[i] == to) {
                    if (col_of(sq) != col_of(from)) need_file = 1;
                    else need_rank = 1;
                }
            }
        }
        if (need_file) buf[pos++] = files[col_of(from)];
        if (need_rank) buf[pos++] = '1' + row_of(from);
    } else {
        /* Pawn capture: include file */
        if (m->captured || m->ep_capture)
            buf[pos++] = files[col_of(from)];
    }

    if (m->captured || m->ep_capture) buf[pos++] = 'x';
    buf[pos++] = files[col_of(to)];
    buf[pos++] = '1' + row_of(to);
    if (m->promoted) buf[pos++] = "?PNBRQK"[m->promoted];

    buf[pos] = '\0';

    /* Check / checkmate suffix */
    Position tmp = before ? *before : g->pos;
    /* We apply the move to test */
    Position after = *before;
    make_move(&after, m); /* side flips inside */
    if (after.in_check) {
        if (!has_any_legal_moves(&after)) buf[pos++] = '#';
        else                              buf[pos++] = '+';
        buf[pos] = '\0';
    }

    strncpy(m->pgn, buf, 15);
    m->pgn[15] = '\0';
}

/* Append move to PGN buffer */
static void pgn_append(GameState *g, const Move *m) {
    char tmp[32];
    if (m->color == WHITE) {
        snprintf(tmp, sizeof(tmp), "%d. %s ", (g->pos.fullmove - (m->color==BLACK?0:1)),
                 m->pgn);
        /* fullmove already incremented; use history count */
        int full = (g->hist_count) / 2 + 1;
        if (m->color == WHITE)
            snprintf(tmp, sizeof(tmp), "%d. %s ", full, m->pgn);
    } else {
        snprintf(tmp, sizeof(tmp), "%s ", m->pgn);
    }
    int len = strlen(tmp);
    if (g->pgn_len + len < MAX_PGN_LEN - 2) {
        memcpy(g->pgn_buf + g->pgn_len, tmp, len);
        g->pgn_len += len;
        g->pgn_buf[g->pgn_len] = '\0';
    }
}

/* Rebuild PGN from scratch using history */
static void rebuild_pgn(GameState *g) {
    g->pgn_len = 0;
    g->pgn_buf[0] = '\0';
    for (int i = 0; i < g->hist_count; i++) {
        const Move *m = &g->history[i];
        char tmp[32];
        if (m->color == WHITE) {
            int full = i/2 + 1;
            snprintf(tmp, sizeof(tmp), "%d. %s ", full, m->pgn);
        } else {
            snprintf(tmp, sizeof(tmp), "%s ", m->pgn);
        }
        int len = strlen(tmp);
        if (g->pgn_len + len < MAX_PGN_LEN - 2) {
            memcpy(g->pgn_buf + g->pgn_len, tmp, len);
            g->pgn_len += len;
            g->pgn_buf[g->pgn_len] = '\0';
        }
    }
}

/* ─── Apply Move to Game ──────────────────────────────────────────────────── */
static void apply_move_to_game(GameState *g, Move *m) {
    Position before = g->pos;
    generate_pgn(g, m, &before);
    make_move(&g->pos, m);

    if (g->hist_count < MAX_HISTORY)
        g->history[g->hist_count++] = *m;

    pgn_append(g, m);

    g->last_from = m->from_sq;
    g->last_to   = m->to_sq;
    g->selected  = -1;
    g->legal_count = 0;

    /* Check game over */
    if (!has_any_legal_moves(&g->pos)) {
        g->game_over = 1;
        if (g->pos.in_check) {
            strcpy(g->game_result, (g->pos.side == WHITE) ? "0-1" : "1-0");
            set_status(g, (g->pos.side == WHITE) ?
                "Black wins by checkmate!" : "White wins by checkmate!", 0);
        } else {
            strcpy(g->game_result, "1/2-1/2");
            set_status(g, "Draw by stalemate!", 0);
        }
    } else if (g->pos.halfmove >= 100) {
        g->game_over = 1;
        strcpy(g->game_result, "1/2-1/2");
        set_status(g, "Draw by 50-move rule!", 0);
    } else {
        char msg[64];
        if (g->pos.in_check)
            snprintf(msg, sizeof(msg), "%s is in check!",
                     g->pos.side == WHITE ? "White" : "Black");
        else
            snprintf(msg, sizeof(msg), "%s to move",
                     g->pos.side == WHITE ? "White" : "Black");
        set_status(g, msg, 0);
    }
}

/* ─── Undo Move ───────────────────────────────────────────────────────────── */
static void undo_move(GameState *g) {
    if (g->hist_count == 0) {
        set_status(g, "Nothing to undo!", 1);
        return;
    }
    /* If engine is playing, undo two moves (engine + player) */
    int undo_count = (g->engine_active && g->engine_color >= 0) ? 2 : 1;
    undo_count = (undo_count > g->hist_count) ? g->hist_count : undo_count;

    for (int i = 0; i < undo_count; i++) {
        Move *m = &g->history[--g->hist_count];
        unmake_move(&g->pos, m);
    }

    g->game_over = 0;
    g->game_result[0] = '\0';

    /* Update last move highlights */
    if (g->hist_count > 0) {
        g->last_from = g->history[g->hist_count-1].from_sq;
        g->last_to   = g->history[g->hist_count-1].to_sq;
    } else {
        g->last_from = -1;
        g->last_to   = -1;
    }

    g->selected = -1;
    g->legal_count = 0;
    rebuild_pgn(g);

    char msg[64];
    snprintf(msg, sizeof(msg), "Undone. %s to move",
             g->pos.side == WHITE ? "White" : "Black");
    set_status(g, msg, 0);
}

/* ─── Status ──────────────────────────────────────────────────────────────── */
static void set_status(GameState *g, const char *msg, int is_error) {
    strncpy(g->status_msg, msg, sizeof(g->status_msg)-1);
    g->status_is_error = is_error;
}

/* ─── FEN Generation ──────────────────────────────────────────────────────── */
static void pos_to_fen(const Position *p, char *fen, int size) {
    int pos = 0;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            int sq = sq_of(r, c);
            if (p->board[sq].piece == EMPTY) {
                empty++;
            } else {
                if (empty) { fen[pos++] = '0'+empty; empty = 0; }
                fen[pos++] = piece_char(p->board[sq].piece, p->board[sq].color);
            }
        }
        if (empty) fen[pos++] = '0'+empty;
        if (r > 0) fen[pos++] = '/';
    }
    fen[pos++] = ' ';
    fen[pos++] = (p->side == WHITE) ? 'w' : 'b';
    fen[pos++] = ' ';
    if (!p->castle_rights) fen[pos++] = '-';
    else {
        if (p->castle_rights & CASTLE_WK) fen[pos++] = 'K';
        if (p->castle_rights & CASTLE_WQ) fen[pos++] = 'Q';
        if (p->castle_rights & CASTLE_BK) fen[pos++] = 'k';
        if (p->castle_rights & CASTLE_BQ) fen[pos++] = 'q';
    }
    fen[pos++] = ' ';
    if (p->ep_square == -1) {
        fen[pos++] = '-';
    } else {
        fen[pos++] = 'a' + col_of(p->ep_square);
        fen[pos++] = '1' + row_of(p->ep_square);
    }
    pos += snprintf(fen+pos, size-pos, " %d %d",
                    p->halfmove, p->fullmove);
    fen[pos] = '\0';
}

/* ─── Engine I/O ──────────────────────────────────────────────────────────── */
static int start_engine(GameState *g) {
    if (g->engine_path[0] == '\0') return 0;
    if (pipe(g->eng_in) < 0 || pipe(g->eng_out) < 0) return 0;

    g->eng_pid = fork();
    if (g->eng_pid < 0) return 0;

    if (g->eng_pid == 0) {
        /* Child */
        dup2(g->eng_in[0],  STDIN_FILENO);
        dup2(g->eng_out[1], STDOUT_FILENO);
        close(g->eng_in[1]);
        close(g->eng_out[0]);
        /* Suppress engine stderr */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        execlp(g->engine_path, g->engine_path, NULL);
        exit(1);
    }

    /* Parent */
    close(g->eng_in[0]);
    close(g->eng_out[1]);

    /* Set non-blocking read */
    int flags = fcntl(g->eng_out[0], F_GETFL);
    fcntl(g->eng_out[0], F_SETFL, flags | O_NONBLOCK);

    /* Init UCI */
    engine_send(g, "uci\n");
    /* Drain uciok */
    char buf[ENGINE_BUF];
    time_t start = time(NULL);
    while (time(NULL) - start < 3) {
        fd_set fds; FD_ZERO(&fds); FD_SET(g->eng_out[0], &fds);
        struct timeval tv = {0, 50000};
        if (select(g->eng_out[0]+1, &fds, NULL, NULL, &tv) > 0) {
            int n = read(g->eng_out[0], buf, sizeof(buf)-1);
            if (n > 0) { buf[n]='\0'; if (strstr(buf,"uciok")) break; }
        }
    }
    engine_send(g, "isready\n");
    /* Wait readyok */
    start = time(NULL);
    while (time(NULL) - start < 3) {
        fd_set fds; FD_ZERO(&fds); FD_SET(g->eng_out[0], &fds);
        struct timeval tv = {0, 50000};
        if (select(g->eng_out[0]+1, &fds, NULL, NULL, &tv) > 0) {
            int n = read(g->eng_out[0], buf, sizeof(buf)-1);
            if (n > 0) { buf[n]='\0'; if (strstr(buf,"readyok")) break; }
        }
    }
    g->engine_active = 1;
    return 1;
}

static void engine_send(GameState *g, const char *cmd) {
    if (!g->engine_active) return;
    write(g->eng_in[1], cmd, strlen(cmd));
}

/* Build moves string from history */
static void build_moves_string(GameState *g, char *buf, int size) {
    int pos = 0;
    for (int i = 0; i < g->hist_count; i++) {
        const Move *m = &g->history[i];
        const char *files = "abcdefgh";
        if (pos + 8 >= size) break;
        buf[pos++] = files[col_of(m->from_sq)];
        buf[pos++] = '1' + row_of(m->from_sq);
        buf[pos++] = files[col_of(m->to_sq)];
        buf[pos++] = '1' + row_of(m->to_sq);
        if (m->promoted) buf[pos++] = tolower("?pnbrqk"[m->promoted]);
        buf[pos++] = ' ';
    }
    buf[pos] = '\0';
}

static void ask_engine_for_move(GameState *g) {
    if (!g->engine_active) return;

    char fen[128];
    pos_to_fen(&g->pos, fen, sizeof(fen));

    char moves_str[4096] = {0};
    build_moves_string(g, moves_str, sizeof(moves_str));

    char cmd[512];
    if (moves_str[0])
        snprintf(cmd, sizeof(cmd), "position fen %s moves %s\n", fen, moves_str);
    else
        snprintf(cmd, sizeof(cmd), "position fen %s\n", fen);
    engine_send(g, cmd);

    /* Build go command based on time control */
    switch (g->tc.mode) {
    case TC_TIME:
        snprintf(cmd, sizeof(cmd), "go movetime %d\n", g->tc.time_ms);
        break;
    case TC_DEPTH:
        snprintf(cmd, sizeof(cmd), "go depth %d\n", g->tc.depth);
        break;
    case TC_NODES:
        snprintf(cmd, sizeof(cmd), "go nodes %ld\n", g->tc.nodes);
        break;
    }
    engine_send(g, cmd);
    set_status(g, "Engine thinking...", 0);
    render(g);
}

/* Parse UCI move string like "e2e4" or "e7e8q" */
static int parse_uci_move(GameState *g, const char *mv_str, Move *m) {
    if (strlen(mv_str) < 4) return 0;
    const char *files = "abcdefgh";
    int fc = (int)(strchr(files, mv_str[0]) - files);
    int fr = mv_str[1] - '1';
    int tc2= (int)(strchr(files, mv_str[2]) - files);
    int tr = mv_str[3] - '1';

    if (fc<0||fc>7||fr<0||fr>7||tc2<0||tc2>7||tr<0||tr>7) return 0;

    int from = sq_of(fr, fc);
    int to   = sq_of(tr, tc2);

    memset(m, 0, sizeof(*m));
    m->from_sq = from;
    m->to_sq   = to;
    m->piece   = g->pos.board[from].piece;
    m->color   = g->pos.board[from].color;
    m->captured= g->pos.board[to].piece;
    m->captured_color = g->pos.board[to].color;

    /* En passant */
    if (m->piece == PAWN && to == g->pos.ep_square && g->pos.ep_square != -1)
        m->ep_capture = sq_of(fr, tc2);

    /* Castling */
    if (m->piece == KING && abs(tc2 - fc) == 2)
        m->castle = (tc2 > fc) ? 1 : 2;

    /* Promotion */
    if (strlen(mv_str) >= 5) {
        char pc = tolower(mv_str[4]);
        if      (pc == 'n') m->promoted = KNIGHT;
        else if (pc == 'b') m->promoted = BISHOP;
        else if (pc == 'r') m->promoted = ROOK;
        else                m->promoted = QUEEN;
    }

    return 1;
}

static void engine_read_move(GameState *g) {
    if (!g->engine_active) return;

    char buf[ENGINE_BUF] = {0};
    char total[ENGINE_BUF*2] = {0};
    int  total_len = 0;

    /* Wait for bestmove with timeout */
    time_t start = time(NULL);
    int timeout = (g->tc.mode == TC_TIME) ? (g->tc.time_ms/1000 + 5) :
                  (g->tc.mode == TC_DEPTH) ? 60 : 30;
    timeout = timeout < 5 ? 5 : timeout;

    while (time(NULL) - start < timeout) {
        fd_set fds; FD_ZERO(&fds); FD_SET(g->eng_out[0], &fds);
        struct timeval tv = {0, 100000};
        if (select(g->eng_out[0]+1, &fds, NULL, NULL, &tv) > 0) {
            int n = read(g->eng_out[0], buf, sizeof(buf)-1);
            if (n > 0) {
                buf[n] = '\0';
                if (total_len + n < (int)sizeof(total)-1) {
                    memcpy(total + total_len, buf, n);
                    total_len += n;
                    total[total_len] = '\0';
                }
                char *bm = strstr(total, "bestmove");
                if (bm) {
                    char mv[8] = {0};
                    if (sscanf(bm, "bestmove %7s", mv) == 1 &&
                        strcmp(mv, "(none)") != 0) {
                        Move m = {0};
                        if (parse_uci_move(g, mv, &m)) {
                            /* Validate move is legal */
                            int lmoves[MAX_LEGAL], lcnt;
                            generate_legal_moves(&g->pos, m.from_sq, lmoves, &lcnt);
                            int legal = 0;
                            for (int i = 0; i < lcnt; i++)
                                if (lmoves[i] == m.to_sq) { legal=1; break; }
                            if (legal) {
                                apply_move_to_game(g, &m);
                            } else {
                                set_status(g, "Engine returned illegal move!", 1);
                            }
                        }
                    }
                    return;
                }
            }
        }
    }
    set_status(g, "Engine timed out!", 1);
}

/* ─── Rendering ───────────────────────────────────────────────────────────── */

#define BOARD_ROW_START  3
#define BOARD_COL_START  3
#define CELL_W           5   /* chars per cell */
#define CELL_H           2   /* rows per cell  */
#define BOARD_W          (8 * CELL_W + 2)
#define BOARD_H          (8 * CELL_H + 2)
#define PANEL_COL        (BOARD_COL_START + BOARD_W + 2)
#define PANEL_W          42

static void draw_cell(GameState *g, int row_8, int col_8) {
    int sq;
    if (g->flipped)
        sq = sq_of(row_8, 7 - col_8);
    else
        sq = sq_of(7 - row_8, col_8);

    int is_light  = (col_of(sq) + row_of(sq)) % 2 == 1;
    int is_select = (sq == g->selected);
    int is_last   = (sq == g->last_from || sq == g->last_to);
    int is_check  = (g->pos.board[sq].piece == KING &&
                     g->pos.board[sq].color == g->pos.side &&
                     g->pos.in_check);
    /* Is this a legal move target? */
    int is_legal  = 0;
    for (int i = 0; i < g->legal_count; i++)
        if (g->legal[i] == sq) { is_legal = 1; break; }

    int is_cursor = (sq == g->cursor);

    /* Choose background */
    const char *bg;
    if (is_check)       bg = BG_CHECK;
    else if (is_select) bg = BG_SELECT;
    else if (is_legal)  bg = BG_LEGAL;
    else if (is_last)   bg = BG_LAST_MOVE;
    else if (is_light)  bg = BG_LIGHT;
    else                bg = BG_DARK;

    int piece = g->pos.board[sq].piece;
    int color = g->pos.board[sq].color;
    const char *fg = (color == WHITE) ? FG_WHITE_PC : FG_BLACK_PC;

    /* Screen position */
    int sr = BOARD_ROW_START + 1 + row_8 * CELL_H;
    int sc = BOARD_COL_START + 1 + col_8 * CELL_W;

    /* Top half of cell */
    MOVE_CURSOR(sr, sc);
    if (is_cursor) printf("\033[4m"); /* underline for cursor */
    printf("%s%s     " RESET_COLOR(), bg, "");

    /* Bottom half: piece */
    MOVE_CURSOR(sr+1, sc);
    printf("%s%s", bg, fg);
    if (is_cursor) printf(BOLD);

    /* Dot for legal move if empty */
    if (is_legal && piece == EMPTY)
        printf("  \u2022  "); /* centered dot */
    else if (piece != EMPTY) {
        const char *sym = piece_unicode(piece, color);
        printf("  %s  ", sym);
    } else {
        printf("     ");
    }
    printf(RESET_COLOR());
}

static void render(GameState *g) {
    /* Flush everything at once */
    HIDE_CURSOR();
    MOVE_CURSOR(1,1);

    /* Title bar */
    printf(BOLD FG_TITLE "  ♟  TERMINAL CHESS  ♟  " RESET_COLOR());
    printf(FG_PANEL "  [↑↓←→/hjkl] Move  [Enter] Select  [u] Undo  [f] Flip  [q] Quit" RESET_COLOR());

    /* Board border top */
    MOVE_CURSOR(BOARD_ROW_START, BOARD_COL_START);
    printf(FG_BORDER "  ");
    for (int c = 0; c < 8; c++) {
        char file_ch = g->flipped ? ('h'-c) : ('a'+c);
        printf("  %c  ", file_ch);
    }
    printf(RESET_COLOR());

    /* Board top line */
    MOVE_CURSOR(BOARD_ROW_START+1, BOARD_COL_START);
    printf(FG_BORDER "  ");
    for (int c = 0; c < 8*CELL_W; c++) printf("─");
    printf(RESET_COLOR());

    /* Cells */
    for (int r = 0; r < 8; r++) {
        /* Rank label */
        int rank_num = g->flipped ? (r+1) : (8-r);
        MOVE_CURSOR(BOARD_ROW_START + 1 + r*CELL_H, BOARD_COL_START);
        printf(FG_BORDER "%d " RESET_COLOR(), rank_num);
        MOVE_CURSOR(BOARD_ROW_START + 2 + r*CELL_H, BOARD_COL_START);
        printf(FG_BORDER "  " RESET_COLOR());

        for (int c = 0; c < 8; c++)
            draw_cell(g, r, c);
    }

    /* Board bottom line */
    MOVE_CURSOR(BOARD_ROW_START + 1 + 8*CELL_H, BOARD_COL_START);
    printf(FG_BORDER "  ");
    for (int c = 0; c < 8*CELL_W; c++) printf("─");
    printf(RESET_COLOR());

    /* ─── Side Panel ─────────────────────────────────────────── */
    int pr = BOARD_ROW_START;
    int pc = PANEL_COL;

    /* Panel: Status */
    MOVE_CURSOR(pr++, pc);
    printf(BG_PANEL FG_TITLE BOLD " %-*s " RESET_COLOR(), PANEL_W-2,
           "STATUS");
    MOVE_CURSOR(pr++, pc);
    if (g->status_is_error)
        printf(BG_PANEL FG_ERROR " %-*s " RESET_COLOR(), PANEL_W-2,
               g->status_msg);
    else
        printf(BG_PANEL FG_STATUS " %-*s " RESET_COLOR(), PANEL_W-2,
               g->status_msg);

    /* Turn / check indicator */
    MOVE_CURSOR(pr++, pc);
    char turn_str[64];
    if (g->game_over) {
        snprintf(turn_str, sizeof(turn_str), "Game Over: %s", g->game_result);
    } else {
        snprintf(turn_str, sizeof(turn_str), "%s to move  Move #%d",
                 g->pos.side == WHITE ? "♔ White" : "♚ Black",
                 g->pos.fullmove);
    }
    printf(BG_PANEL FG_PANEL " %-*s " RESET_COLOR(), PANEL_W-2, turn_str);

    /* Panel: Time Control */
    pr++;
    MOVE_CURSOR(pr++, pc);
    printf(BG_PANEL FG_TITLE BOLD " %-*s " RESET_COLOR(), PANEL_W-2,
           "TIME CONTROL  [t]=mode [+/-]=value");
    MOVE_CURSOR(pr++, pc);
    char tc_str[64];
    switch (g->tc.mode) {
    case TC_TIME:
        snprintf(tc_str, sizeof(tc_str), "Mode: Time  %dms per move", g->tc.time_ms);
        break;
    case TC_DEPTH:
        snprintf(tc_str, sizeof(tc_str), "Mode: Depth  %d plies", g->tc.depth);
        break;
    case TC_NODES:
        snprintf(tc_str, sizeof(tc_str), "Mode: Nodes  %ld nodes", g->tc.nodes);
        break;
    }
    printf(BG_PANEL FG_PANEL " %-*s " RESET_COLOR(), PANEL_W-2, tc_str);

    /* Panel: Engine info */
    pr++;
    MOVE_CURSOR(pr++, pc);
    printf(BG_PANEL FG_TITLE BOLD " %-*s " RESET_COLOR(), PANEL_W-2,
           "ENGINE  [e]=toggle [s]=swap sides");
    MOVE_CURSOR(pr++, pc);
    char eng_str[64];
    if (!g->engine_active || g->engine_path[0]=='\0') {
        snprintf(eng_str, sizeof(eng_str), "No engine loaded");
    } else {
        const char *side_str = "None";
        if (g->engine_color == WHITE) side_str = "White";
        else if (g->engine_color == BLACK) side_str = "Black";
        else side_str = "Disabled";
        snprintf(eng_str, sizeof(eng_str), "Playing: %s", side_str);
    }
    printf(BG_PANEL FG_PANEL " %-*s " RESET_COLOR(), PANEL_W-2, eng_str);

    /* Panel: Move History (PGN) */
    pr++;
    MOVE_CURSOR(pr++, pc);
    printf(BG_PANEL FG_TITLE BOLD " %-*s " RESET_COLOR(), PANEL_W-2,
           "MOVES (PGN)");

    /* Word-wrap PGN into panel */
    int pgn_rows = 12;
    char pgn_copy[MAX_PGN_LEN];
    strncpy(pgn_copy, g->pgn_buf, sizeof(pgn_copy)-1);
    pgn_copy[sizeof(pgn_copy)-1] = '\0';

    /* Display last pgn_rows lines worth */
    int pgn_width = PANEL_W - 2;
    int pgn_len   = strlen(pgn_copy);

    /* Simple wrapping: split into lines */
    char lines[32][64];
    int  line_count = 0;
    int  i = 0, lp = 0;
    int  max_lines = 30;

    memset(lines, 0, sizeof(lines));
    while (i < pgn_len && line_count < max_lines) {
        /* Find wrap point */
        int end = i + pgn_width;
        if (end >= pgn_len) end = pgn_len;
        else {
            /* Back up to space */
            int back = end;
            while (back > i && pgn_copy[back] != ' ') back--;
            if (back > i) end = back + 1;
        }
        int ll = end - i; if (ll > 63) ll = 63;
        memcpy(lines[line_count], pgn_copy+i, ll);
        lines[line_count][ll] = '\0';
        line_count++;
        i = end;
        (void)lp;
    }

    /* Show last pgn_rows lines */
    int start_line = (line_count > pgn_rows) ? (line_count - pgn_rows) : 0;
    for (int li = 0; li < pgn_rows; li++) {
        MOVE_CURSOR(pr++, pc);
        if (start_line + li < line_count)
            printf(BG_PANEL FG_PANEL " %-*.*s " RESET_COLOR(),
                   pgn_width, pgn_width, lines[start_line+li]);
        else
            printf(BG_PANEL " %-*s " RESET_COLOR(), pgn_width, "");
    }

    /* Panel: Keys help */
    pr++;
    MOVE_CURSOR(pr++, pc);
    printf(BG_PANEL FG_TITLE BOLD " %-*s " RESET_COLOR(), PANEL_W-2, "KEYS");
    const char *help[] = {
        "Arrow/hjkl : Move cursor",
        "Enter/Space: Select / Move piece",
        "u          : Undo last move",
        "f          : Flip board",
        "t          : Cycle time control mode",
        "+ / -      : Increase/decrease TC value",
        "e          : Toggle engine play",
        "s          : Swap engine color",
        "n          : New game",
        "q / Esc    : Quit",
        NULL
    };
    for (int hi = 0; help[hi]; hi++) {
        MOVE_CURSOR(pr++, pc);
        printf(BG_PANEL FG_PANEL " %-*s " RESET_COLOR(), PANEL_W-2, help[hi]);
    }

    /* Clear area below panel */
    for (int ci = 0; ci < 3; ci++) {
        MOVE_CURSOR(pr++, pc);
        printf("%-*s", PANEL_W, "");
    }

    fflush(stdout);
}

/* ─── Keyboard Input ──────────────────────────────────────────────────────── */
/* Read a keypress, returning char or special code */
#define KEY_UP    256
#define KEY_DOWN  257
#define KEY_LEFT  258
#define KEY_RIGHT 259
#define KEY_ENTER 260
#define KEY_ESC   261

static int read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c == '\033') {
        unsigned char seq[3] = {0};
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 50000};
        if (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) > 0) {
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
                if (read(STDIN_FILENO, &seq[1], 1) == 1) {
                    switch (seq[1]) {
                    case 'A': return KEY_UP;
                    case 'B': return KEY_DOWN;
                    case 'C': return KEY_RIGHT;
                    case 'D': return KEY_LEFT;
                    }
                }
            }
        }
        return KEY_ESC;
    }
    if (c == '\r' || c == '\n') return KEY_ENTER;
    return (int)c;
}

/* Convert cursor sq to display row/col and back */
static void cursor_move(GameState *g, int dr, int dc) {
    int r = row_of(g->cursor);
    int c = col_of(g->cursor);
    /* dr/dc are in board coordinates; flip if needed */
    if (g->flipped) { dr = -dr; dc = -dc; }
    r = (r + dr + 8) % 8;
    c = (c + dc + 8) % 8;
    g->cursor = sq_of(r, c);
}

static void promotion_menu(GameState *g, Move *m) {
    /* Simple inline promotion selection */
    int pieces[] = {QUEEN, ROOK, BISHOP, KNIGHT};
    const char *names[] = {"Queen","Rook","Bishop","Knight"};
    int sel = 0;

    while (1) {
        /* Draw menu */
        int mr = BOARD_ROW_START + 5;
        int mc = PANEL_COL;
        MOVE_CURSOR(mr, mc);
        printf(BG_PANEL FG_TITLE BOLD " PROMOTE TO:                              " RESET_COLOR());
        for (int i = 0; i < 4; i++) {
            MOVE_CURSOR(mr+1+i, mc);
            if (i == sel)
                printf(BG_SELECT FG_BLACK_PC BOLD " %-40s " RESET_COLOR(), names[i]);
            else
                printf(BG_PANEL FG_PANEL " %-40s " RESET_COLOR(), names[i]);
        }
        fflush(stdout);

        int k = read_key();
        if (k == KEY_UP)   sel = (sel+3)%4;
        if (k == KEY_DOWN) sel = (sel+1)%4;
        if (k == KEY_ENTER || k == ' ') { m->promoted = pieces[sel]; return; }
        if (k == 'q' || k == KEY_ESC)   { m->promoted = QUEEN; return; }
    }
}

static void process_key(GameState *g, int ch) {
    switch (ch) {
    /* Cursor movement */
    case KEY_UP:    case 'k': cursor_move(g,  1,  0); break;
    case KEY_DOWN:  case 'j': cursor_move(g, -1,  0); break;
    case KEY_LEFT:  case 'h': cursor_move(g,  0, -1); break;
    case KEY_RIGHT: case 'l': cursor_move(g,  0,  1); break;

    /* Select / Move */
    case KEY_ENTER: case ' ': {
        if (g->game_over) { set_status(g,"Game over. Press n for new game.",1); break; }
        /* If engine should move now, ignore */
        if (g->engine_active && g->engine_color == g->pos.side) break;

        int sq = g->cursor;

        if (g->selected == -1) {
            /* Select a piece */
            if (g->pos.board[sq].piece != EMPTY &&
                g->pos.board[sq].color == g->pos.side) {
                g->selected = sq;
                generate_legal_moves(&g->pos, sq, g->legal, &g->legal_count);
                if (g->legal_count == 0) {
                    g->selected = -1;
                    set_status(g, "No legal moves for that piece!", 1);
                }
            } else {
                set_status(g, "No friendly piece there!", 1);
            }
        } else {
            /* Attempt move */
            int found = 0;
            for (int i = 0; i < g->legal_count; i++) {
                if (g->legal[i] == sq) { found = 1; break; }
            }
            if (found) {
                Move m = {0};
                m.from_sq = g->selected;
                m.to_sq   = sq;
                m.piece   = g->pos.board[g->selected].piece;
                m.color   = g->pos.board[g->selected].color;
                m.captured= g->pos.board[sq].piece;
                m.captured_color = g->pos.board[sq].color;

                /* En passant */
                if (m.piece == PAWN && sq == g->pos.ep_square && g->pos.ep_square != -1)
                    m.ep_capture = sq_of(row_of(g->selected), col_of(sq));

                /* Castling */
                if (m.piece == KING && abs(col_of(sq)-col_of(g->selected)) == 2)
                    m.castle = (col_of(sq) > col_of(g->selected)) ? 1 : 2;

                /* Promotion */
                int is_promo = (m.piece == PAWN &&
                                (row_of(sq)==7 || row_of(sq)==0));
                if (is_promo) {
                    m.promoted = QUEEN; /* default */
                    promotion_menu(g, &m);
                }

                apply_move_to_game(g, &m);

                /* If engine should respond */
                if (!g->game_over && g->engine_active &&
                    g->engine_color == g->pos.side) {
                    render(g);
                    ask_engine_for_move(g);
                    engine_read_move(g);
                }
            } else if (sq == g->selected) {
                /* Deselect */
                g->selected = -1;
                g->legal_count = 0;
            } else if (g->pos.board[sq].piece != EMPTY &&
                       g->pos.board[sq].color == g->pos.side) {
                /* Switch selection */
                g->selected = sq;
                generate_legal_moves(&g->pos, sq, g->legal, &g->legal_count);
            } else {
                g->selected = -1;
                g->legal_count = 0;
                set_status(g, "Illegal move!", 1);
            }
        }
        break;
    }

    /* Undo */
    case 'u': undo_move(g); break;

    /* Flip board */
    case 'f':
        g->flipped = !g->flipped;
        set_status(g, g->flipped ? "Board flipped (Black's view)" :
                                   "Board flipped (White's view)", 0);
        break;

    /* New game */
    case 'n':
        init_position(&g->pos);
        g->hist_count  = 0;
        g->pgn_len     = 0;
        g->pgn_buf[0]  = '\0';
        g->last_from   = -1;
        g->last_to     = -1;
        g->selected    = -1;
        g->legal_count = 0;
        g->game_over   = 0;
        g->game_result[0] = '\0';
        set_status(g, "New game started! White to move.", 0);
        if (g->engine_active && g->engine_color == WHITE) {
            engine_send(g, "ucinewgame\n");
            ask_engine_for_move(g);
            engine_read_move(g);
        } else if (g->engine_active) {
            engine_send(g, "ucinewgame\n");
        }
        break;

    /* Time control mode cycle */
    case 't':
        g->tc.mode = (TCMode)((g->tc.mode + 1) % 3);
        set_status(g, "Time control mode changed.", 0);
        break;

    /* Increase TC value */
    case '+': case '=':
        switch (g->tc.mode) {
        case TC_TIME:  g->tc.time_ms  += 500;  break;
        case TC_DEPTH: g->tc.depth    += 1;    break;
        case TC_NODES: g->tc.nodes    += 100000; break;
        }
        set_status(g, "TC value increased.", 0);
        break;

    /* Decrease TC value */
    case '-':
        switch (g->tc.mode) {
        case TC_TIME:  g->tc.time_ms  = (g->tc.time_ms > 500) ? g->tc.time_ms-500 : 100; break;
        case TC_DEPTH: g->tc.depth    = (g->tc.depth > 1)     ? g->tc.depth-1 : 1;       break;
        case TC_NODES: g->tc.nodes    = (g->tc.nodes > 100000)? g->tc.nodes-100000 : 1000; break;
        }
        set_status(g, "TC value decreased.", 0);
        break;

    /* Engine toggle */
    case 'e':
        if (!g->engine_active && g->engine_path[0] != '\0') {
            start_engine(g);
            set_status(g, "Engine enabled.", 0);
        } else if (g->engine_active) {
            g->engine_color = (g->engine_color < 0) ? BLACK : -1;
            set_status(g, g->engine_color>=0 ? "Engine enabled." : "Engine disabled.", 0);
        } else {
            set_status(g, "No engine path specified!", 1);
        }
        break;

    /* Swap engine color */
    case 's':
        if (g->engine_active && g->engine_color >= 0) {
            g->engine_color = 1 - g->engine_color;
            char msg[64];
            snprintf(msg, sizeof(msg), "Engine now plays %s.",
                     g->engine_color == WHITE ? "White" : "Black");
            set_status(g, msg, 0);
            /* If it's now engine's turn, trigger */
            if (!g->game_over && g->engine_color == g->pos.side) {
                render(g);
                ask_engine_for_move(g);
                engine_read_move(g);
            }
        }
        break;

    /* Quit */
    case 'q': case KEY_ESC:
        term_restore();
        CLEAR_SCREEN();
        MOVE_CURSOR(1,1);
        if (g->engine_active) {
            engine_send(g, "quit\n");
            close(g->eng_in[1]);
            close(g->eng_out[0]);
        }
        printf("Thanks for playing!\n");
        if (g->pgn_len > 0) {
            printf("\nGame PGN:\n%s\n", g->pgn_buf);
            if (g->game_result[0])
                printf(" %s\n", g->game_result);
        }
        exit(0);
    }
}

/* ─── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    /* Init game */
    memset(&G, 0, sizeof(G));
    init_position(&G.pos);
    G.last_from    = -1;
    G.last_to      = -1;
    G.selected     = -1;
    G.cursor       = sq_of(1, 4); /* e2 */
    G.engine_color = -1;

    /* Default time control */
    G.tc.mode    = TC_TIME;
    G.tc.time_ms = 1000;
    G.tc.depth   = 5;
    G.tc.nodes   = 500000;

    set_status(&G, "White to move. Use arrows + Enter to play.", 0);

    /* Engine path from arg */
    if (argc > 1) {
        strncpy(G.engine_path, argv[1], sizeof(G.engine_path)-1);
        if (start_engine(&G)) {
            G.engine_color = BLACK;
            set_status(&G, "Engine loaded! Playing Black. White to move.", 0);
        } else {
            set_status(&G, "Failed to start engine!", 1);
            G.engine_path[0] = '\0';
        }
    }

    /* Terminal setup */
    get_term_size(&G.term_rows, &G.term_cols);
    term_raw();
    HIDE_CURSOR();
    CLEAR_SCREEN();

    signal(SIGWINCH, handle_sigwinch);
    signal(SIGINT,   handle_sigint);

    /* Check terminal size */
    if (G.term_cols < 90 || G.term_rows < 25) {
        term_restore();
        fprintf(stderr, "Terminal too small! Need at least 90 cols x 25 rows.\n");
        fprintf(stderr, "Current: %d cols x %d rows\n", G.term_cols, G.term_rows);
        return 1;
    }

    render(&G);

    /* Main loop */
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 100000}; /* 100ms timeout */

        int r = select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);
        if (r > 0) {
            int ch = read_key();
            if (ch >= 0) {
                process_key(&G, ch);
                render(&G);
            }
        } else if (r == 0) {
            /* Timeout: just re-render in case of resize */
            /* (SIGWINCH handles actual resize) */
        }
    }

    term_restore();
    return 0;
}
