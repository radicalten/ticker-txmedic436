#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>

/* ─────────────────────────────────────────────
   ANSI / Terminal helpers
   ───────────────────────────────────────────── */
#define ESC "\033"
#define CLEAR_SCREEN()   printf(ESC "[2J" ESC "[H")
#define HIDE_CURSOR()    printf(ESC "[?25l")
#define SHOW_CURSOR()    printf(ESC "[?25h")
#define MOVE_CURSOR(r,c) printf(ESC "[%d;%dH", (r), (c))
#define RESET_COLOR()    printf(ESC "[0m")
#define BOLD()           printf(ESC "[1m")

/* Foreground colors */
#define FG_BLACK()   printf(ESC "[30m")
#define FG_WHITE()   printf(ESC "[97m")
#define FG_YELLOW()  printf(ESC "[93m")
#define FG_CYAN()    printf(ESC "[96m")
#define FG_GREEN()   printf(ESC "[92m")
#define FG_RED()     printf(ESC "[91m")
#define FG_MAGENTA() printf(ESC "[95m")
#define FG_BLUE()    printf(ESC "[94m")

/* Background colors */
#define BG_DARK_SQ()    printf(ESC "[48;5;94m")   /* dark square  (brown) */
#define BG_LIGHT_SQ()   printf(ESC "[48;5;229m")  /* light square (cream) */
#define BG_CURSOR()     printf(ESC "[48;5;226m")  /* cursor       (bright yellow) */
#define BG_SELECTED()   printf(ESC "[48;5;82m")   /* selected     (bright green) */
#define BG_LEGAL()      printf(ESC "[48;5;117m")  /* legal target (light blue) */
#define BG_LAST_MOVE()  printf(ESC "[48;5;214m")  /* last move    (orange) */
#define BG_CHECK()      printf(ESC "[48;5;196m")  /* in check     (red) */
#define BG_STATUS()     printf(ESC "[48;5;236m")  /* status bar   (dark grey) */

/* ─────────────────────────────────────────────
   Chess constants
   ───────────────────────────────────────────── */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  0
#define BLACK  1

#define MAX_MOVES     512
#define MAX_HIST      1024
#define MAX_PGN_LEN   65536
#define MAX_LEGAL      256

/* Castling flags */
#define CASTLE_WK  1
#define CASTLE_WQ  2
#define CASTLE_BK  4
#define CASTLE_BQ  8

/* ─────────────────────────────────────────────
   Data structures
   ───────────────────────────────────────────── */
typedef struct {
    int piece;     /* PAWN..KING */
    int color;     /* WHITE/BLACK */
} Square;

typedef struct {
    int from_r, from_c;
    int to_r,   to_c;
    int promo;          /* 0 or piece type */
    /* captured info for undo */
    int cap_piece;
    int cap_color;
    /* en-passant captured pawn position */
    int ep_cap_r, ep_cap_c;
    /* castling: did we castle? */
    int castled;        /* 0=no,1=king-side,2=queen-side */
    /* previous state for undo */
    int prev_castle_rights;
    int prev_ep_col;    /* -1 = none */
    int prev_halfmove;
    /* annotation */
    char san[16];       /* SAN notation */
} Move;

typedef struct {
    Square  sq[8][8];
    int     castle_rights;   /* bitmask CASTLE_WK|WQ|BK|BQ */
    int     ep_col;          /* en-passant target column, -1=none */
    int     turn;            /* WHITE or BLACK */
    int     halfmove;        /* 50-move rule counter */
    int     fullmove;
    Move    history[MAX_HIST];
    int     hist_len;
    int     in_check;        /* 1 if current player in check */
    int     game_over;       /* 0=playing,1=checkmate,2=stalemate,3=draw-50 */
    int     winner;          /* WHITE/BLACK or -1 */
} Board;

/* UCI engine */
typedef struct {
    pid_t   pid;
    int     fd_in;           /* we write to engine */
    int     fd_out;          /* we read from engine */
    char    path[512];
    int     ready;
    int     thinking;
    char    best_move[16];
    int     found_move;
    /* settings */
    int     depth;           /* 0 = unlimited */
    int     nodes;           /* 0 = unlimited */
    int     movetime;        /* ms, 0 = use time control */
    int     wtime, btime;
    int     winc,  binc;
} Engine;

/* UI state */
typedef struct {
    int     cur_r, cur_c;      /* cursor row/col (0-7) */
    int     sel_r, sel_c;      /* selected piece, -1 if none */
    int     selected;
    int     legal_targets[64]; /* 1 if square is a legal target */
    int     flip_board;        /* 0=white bottom, 1=black bottom */
    int     show_pgn;          /* toggle PGN panel */
    int     show_help;
    /* engine playing side: -1=none, 0=white, 1=black */
    int     engine_side;
    char    pgn[MAX_PGN_LEN];
    int     pgn_len;
    char    status_msg[256];
    int     status_color;      /* 0=normal,1=error,2=info */
    char    promo_choice;      /* 'q','r','b','n' */
} UI;

/* Settings menu */
typedef struct {
    int     max_depth;    /* 0 = no limit */
    int     max_nodes;    /* 0 = no limit */
    int     movetime_ms;  /* 0 = no limit / use time */
    int     wtime_ms;
    int     btime_ms;
    int     inc_ms;
    char    engine_path[512];
} Settings;

/* ─────────────────────────────────────────────
   Globals
   ───────────────────────────────────────────── */
static Board    g_board;
static UI       g_ui;
static Engine   g_engine;
static Settings g_settings;
static struct termios g_orig_termios;
static volatile int g_resize_flag = 0;
static int      g_term_rows = 40;
static int      g_term_cols = 120;

/* ─────────────────────────────────────────────
   Forward declarations
   ───────────────────────────────────────────── */
void board_init(Board *b);
int  is_in_bounds(int r, int c);
int  is_attacked(Board *b, int r, int c, int by_color);
int  is_in_check(Board *b, int color);
void generate_legal_moves(Board *b, int fr, int fc, Move *out, int *cnt);
int  make_move(Board *b, Move *m);
void undo_move(Board *b);
void move_to_san(Board *b, Move *m, char *out);
void update_pgn(void);
void draw_board(void);
void draw_status(void);
void draw_pgn_panel(void);
void draw_help(void);
void draw_settings(void);
void handle_input(void);
void engine_start(void);
void engine_stop(void);
void engine_send(const char *cmd);
char *engine_read_line(int timeout_ms);
void engine_go(void);
void engine_poll(void);
void engine_set_position(void);
void set_status(const char *msg, int color);
void compute_legal_targets(void);
void clear_selection(void);
int  try_move(int fr, int fc, int tr, int tc);
void detect_game_over(void);
void rebuild_pgn(void);
int  file_exists(const char *path);
void restore_terminal(void);
void setup_terminal(void);
static void sigwinch_handler(int sig);
void get_term_size(void);
int  is_promotion_move(Board *b, int fr, int fc, int tr, int tc);
int  piece_char_to_type(char c);
char piece_type_to_char(int t);
void san_from_move(Board *b_before, Move *m, char *san_out);
int  count_legal_moves(Board *b);

/* ═══════════════════════════════════════════════
   TERMINAL SETUP / RESTORE
   ═══════════════════════════════════════════════ */
void setup_terminal(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1; /* 100ms timeout */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    HIDE_CURSOR();
}

void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    SHOW_CURSOR();
    RESET_COLOR();
    CLEAR_SCREEN();
    MOVE_CURSOR(1,1);
}

static void sigwinch_handler(int sig) {
    (void)sig;
    g_resize_flag = 1;
}

void get_term_size(void) {
    /* Use escape sequence to query terminal size */
    printf(ESC "[999;999H" ESC "[6n");
    fflush(stdout);
    char buf[32];
    int i = 0;
    /* Read response ESC[rows;colsR */
    usleep(50000);
    while (i < 31) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;
        buf[i++] = c;
        if (c == 'R') break;
    }
    buf[i] = 0;
    int r = 40, c = 120;
    sscanf(buf, ESC "[%d;%dR", &r, &c);
    g_term_rows = r;
    g_term_cols = c;
}

/* ═══════════════════════════════════════════════
   BOARD LOGIC
   ═══════════════════════════════════════════════ */
void board_init(Board *b) {
    memset(b, 0, sizeof(*b));
    b->ep_col = -1;
    b->castle_rights = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    b->fullmove = 1;

    /* Place pieces - row 0 = rank 8 (black back rank) */
    int back[] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int c = 0; c < 8; c++) {
        b->sq[0][c].piece = back[c]; b->sq[0][c].color = BLACK;
        b->sq[1][c].piece = PAWN;   b->sq[1][c].color = BLACK;
        b->sq[6][c].piece = PAWN;   b->sq[6][c].color = WHITE;
        b->sq[7][c].piece = back[c]; b->sq[7][c].color = WHITE;
    }
}

int is_in_bounds(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

/* Check if square (r,c) is attacked by `by_color` */
int is_attacked(Board *b, int r, int c, int by_color) {
    /* Pawns */
    int pawn_dir = (by_color == WHITE) ? 1 : -1;
    int pr = r + pawn_dir;
    for (int dc = -1; dc <= 1; dc += 2) {
        int pc = c + dc;
        if (is_in_bounds(pr, pc) &&
            b->sq[pr][pc].piece == PAWN &&
            b->sq[pr][pc].color == by_color) return 1;
    }
    /* Knights */
    int kmoves[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + kmoves[i][0], nc = c + kmoves[i][1];
        if (is_in_bounds(nr,nc) &&
            b->sq[nr][nc].piece == KNIGHT &&
            b->sq[nr][nc].color == by_color) return 1;
    }
    /* Bishops / Queens (diagonals) */
    int diags[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 4; i++) {
        for (int d = 1; d < 8; d++) {
            int nr = r + d*diags[i][0], nc = c + d*diags[i][1];
            if (!is_in_bounds(nr,nc)) break;
            if (b->sq[nr][nc].piece != EMPTY) {
                if (b->sq[nr][nc].color == by_color &&
                    (b->sq[nr][nc].piece == BISHOP || b->sq[nr][nc].piece == QUEEN))
                    return 1;
                break;
            }
        }
    }
    /* Rooks / Queens (ranks/files) */
    int straights[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int i = 0; i < 4; i++) {
        for (int d = 1; d < 8; d++) {
            int nr = r + d*straights[i][0], nc = c + d*straights[i][1];
            if (!is_in_bounds(nr,nc)) break;
            if (b->sq[nr][nc].piece != EMPTY) {
                if (b->sq[nr][nc].color == by_color &&
                    (b->sq[nr][nc].piece == ROOK || b->sq[nr][nc].piece == QUEEN))
                    return 1;
                break;
            }
        }
    }
    /* King */
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc2 = -1; dc2 <= 1; dc2++) {
            if (!dr && !dc2) continue;
            int nr = r+dr, nc = c+dc2;
            if (is_in_bounds(nr,nc) &&
                b->sq[nr][nc].piece == KING &&
                b->sq[nr][nc].color == by_color) return 1;
        }
    }
    return 0;
}

int is_in_check(Board *b, int color) {
    /* Find king */
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (b->sq[r][c].piece == KING && b->sq[r][c].color == color)
                return is_attacked(b, r, c, 1-color);
    return 0;
}

/* Make move on board WITHOUT legality check – returns 0 if leaves king in check */
static int apply_move(Board *b, Move *m) {
    Square from_sq = b->sq[m->from_r][m->from_c];
    
    /* Save capture info */
    m->cap_piece = b->sq[m->to_r][m->to_c].piece;
    m->cap_color = b->sq[m->to_r][m->to_c].color;
    m->ep_cap_r  = -1;
    m->castled   = 0;
    m->prev_castle_rights = b->castle_rights;
    m->prev_ep_col        = b->ep_col;
    m->prev_halfmove      = b->halfmove;

    /* En passant capture */
    if (from_sq.piece == PAWN && m->to_c == b->ep_col &&
        m->from_r != m->to_r &&
        b->sq[m->to_r][m->to_c].piece == EMPTY) {
        int ep_r = m->from_r; /* captured pawn is on same rank as moving pawn */
        m->ep_cap_r = ep_r;
        m->cap_piece = PAWN;
        m->cap_color = 1 - from_sq.color;
        b->sq[ep_r][m->to_c].piece = EMPTY;
        b->sq[ep_r][m->to_c].color = 0;
    }

    /* Move piece */
    b->sq[m->to_r][m->to_c] = from_sq;
    b->sq[m->from_r][m->from_c].piece = EMPTY;
    b->sq[m->from_r][m->from_c].color = 0;

    /* Promotion */
    if (m->promo) {
        b->sq[m->to_r][m->to_c].piece = m->promo;
    }

    /* Castling */
    if (from_sq.piece == KING) {
        int dc = m->to_c - m->from_c;
        if (dc == 2) { /* king-side */
            b->sq[m->to_r][5] = b->sq[m->to_r][7];
            b->sq[m->to_r][7].piece = EMPTY;
            m->castled = 1;
        } else if (dc == -2) { /* queen-side */
            b->sq[m->to_r][3] = b->sq[m->to_r][0];
            b->sq[m->to_r][0].piece = EMPTY;
            m->castled = 2;
        }
        /* Remove castling rights for this side */
        if (from_sq.color == WHITE) b->castle_rights &= ~(CASTLE_WK | CASTLE_WQ);
        else                         b->castle_rights &= ~(CASTLE_BK | CASTLE_BQ);
    }
    /* Rook moves affect castling rights */
    if (from_sq.piece == ROOK) {
        if (from_sq.color == WHITE) {
            if (m->from_r == 7 && m->from_c == 7) b->castle_rights &= ~CASTLE_WK;
            if (m->from_r == 7 && m->from_c == 0) b->castle_rights &= ~CASTLE_WQ;
        } else {
            if (m->from_r == 0 && m->from_c == 7) b->castle_rights &= ~CASTLE_BK;
            if (m->from_r == 0 && m->from_c == 0) b->castle_rights &= ~CASTLE_BQ;
        }
    }
    /* If a rook is captured on its starting square */
    if (m->cap_piece == ROOK) {
        if (m->to_r == 7 && m->to_c == 7) b->castle_rights &= ~CASTLE_WK;
        if (m->to_r == 7 && m->to_c == 0) b->castle_rights &= ~CASTLE_WQ;
        if (m->to_r == 0 && m->to_c == 7) b->castle_rights &= ~CASTLE_BK;
        if (m->to_r == 0 && m->to_c == 0) b->castle_rights &= ~CASTLE_BQ;
    }

    /* Update en passant */
    b->ep_col = -1;
    if (from_sq.piece == PAWN && abs(m->to_r - m->from_r) == 2) {
        b->ep_col = m->from_c;
    }

    /* Halfmove clock */
    if (from_sq.piece == PAWN || m->cap_piece != EMPTY)
        b->halfmove = 0;
    else
        b->halfmove++;

    return 1;
}

int make_move(Board *b, Move *m) {
    /* Check legality by applying and checking if king left in check */
    Board tmp = *b;
    apply_move(&tmp, m);
    if (is_in_check(&tmp, b->turn)) return 0;
    
    /* Generate SAN before applying */
    san_from_move(b, m, m->san);

    /* Apply for real */
    apply_move(b, m);
    
    /* Switch turn */
    if (b->turn == BLACK) b->fullmove++;
    b->turn = 1 - b->turn;
    b->in_check = is_in_check(b, b->turn);

    /* Store in history */
    b->history[b->hist_len++] = *m;
    return 1;
}

void undo_move(Board *b) {
    if (b->hist_len == 0) return;
    Move *m = &b->history[--b->hist_len];

    /* Switch turn back */
    b->turn = 1 - b->turn;
    if (b->turn == BLACK) b->fullmove--;

    Square moved_sq = b->sq[m->to_r][m->to_c];

    /* Undo promotion */
    if (m->promo) moved_sq.piece = PAWN;

    /* Restore moved piece */
    b->sq[m->from_r][m->from_c] = moved_sq;
    b->sq[m->to_r][m->to_c].piece = EMPTY;
    b->sq[m->to_r][m->to_c].color = 0;

    /* Restore capture */
    if (m->ep_cap_r >= 0) {
        /* en passant: restore captured pawn */
        b->sq[m->ep_cap_r][m->to_c].piece = PAWN;
        b->sq[m->ep_cap_r][m->to_c].color = m->cap_color;
    } else if (m->cap_piece != EMPTY) {
        b->sq[m->to_r][m->to_c].piece = m->cap_piece;
        b->sq[m->to_r][m->to_c].color = m->cap_color;
    }

    /* Undo castling rook */
    if (m->castled == 1) { /* king-side */
        b->sq[m->to_r][7] = b->sq[m->to_r][5];
        b->sq[m->to_r][5].piece = EMPTY;
    } else if (m->castled == 2) { /* queen-side */
        b->sq[m->to_r][0] = b->sq[m->to_r][3];
        b->sq[m->to_r][3].piece = EMPTY;
    }

    /* Restore board state */
    b->castle_rights = m->prev_castle_rights;
    b->ep_col        = m->prev_ep_col;
    b->halfmove      = m->prev_halfmove;
    b->in_check      = is_in_check(b, b->turn);
    b->game_over     = 0;
    b->winner        = -1;
}

/* Generate pseudo-legal moves for piece at (fr,fc), then filter for legality */
void generate_legal_moves(Board *b, int fr, int fc, Move *out, int *cnt) {
    *cnt = 0;
    Square sq = b->sq[fr][fc];
    if (sq.piece == EMPTY) return;
    if (sq.color != b->turn) return;

    int color = sq.color;
    int opp   = 1 - color;

    /* Helper lambda-ish macro to try adding a move */
#define TRY_MOVE(TR, TC, PRO) do { \
    if (is_in_bounds(TR,TC)) { \
        Move _m; memset(&_m,0,sizeof(_m)); \
        _m.from_r=fr; _m.from_c=fc; _m.to_r=(TR); _m.to_c=(TC); _m.promo=(PRO); \
        Board _tmp = *b; \
        apply_move(&_tmp, &_m); \
        if (!is_in_check(&_tmp, color)) { \
            out[(*cnt)++] = _m; \
        } \
    } \
} while(0)

#define TRY_PROMO(TR, TC) do { \
    TRY_MOVE(TR,TC,QUEEN); TRY_MOVE(TR,TC,ROOK); \
    TRY_MOVE(TR,TC,BISHOP); TRY_MOVE(TR,TC,KNIGHT); \
} while(0)

    switch (sq.piece) {
    case PAWN: {
        int dir = (color == WHITE) ? -1 : 1;
        int start_row = (color == WHITE) ? 6 : 1;
        int promo_row = (color == WHITE) ? 0 : 7;
        /* Forward one */
        int nr = fr + dir, nc = fc;
        if (is_in_bounds(nr,nc) && b->sq[nr][nc].piece == EMPTY) {
            if (nr == promo_row) TRY_PROMO(nr,nc);
            else TRY_MOVE(nr,nc,0);
            /* Forward two */
            if (fr == start_row && b->sq[nr+dir][nc].piece == EMPTY) {
                TRY_MOVE(nr+dir, nc, 0);
            }
        }
        /* Captures */
        for (int dc = -1; dc <= 1; dc += 2) {
            int tr = fr+dir, tc = fc+dc;
            if (!is_in_bounds(tr,tc)) continue;
            int is_ep = (tc == b->ep_col && b->sq[tr][tc].piece == EMPTY &&
                         b->sq[fr][tc].piece == PAWN && b->sq[fr][tc].color == opp);
            if ((b->sq[tr][tc].piece != EMPTY && b->sq[tr][tc].color == opp) || is_ep) {
                if (tr == promo_row) TRY_PROMO(tr,tc);
                else TRY_MOVE(tr,tc,0);
            }
        }
        break;
    }
    case KNIGHT: {
        int km[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int tr = fr+km[i][0], tc = fc+km[i][1];
            if (!is_in_bounds(tr,tc)) continue;
            if (b->sq[tr][tc].piece == EMPTY || b->sq[tr][tc].color == opp)
                TRY_MOVE(tr,tc,0);
        }
        break;
    }
    case BISHOP: {
        int dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
        for (int i = 0; i < 4; i++) {
            for (int d = 1; d < 8; d++) {
                int tr = fr+d*dirs[i][0], tc = fc+d*dirs[i][1];
                if (!is_in_bounds(tr,tc)) break;
                if (b->sq[tr][tc].piece == EMPTY) { TRY_MOVE(tr,tc,0); continue; }
                if (b->sq[tr][tc].color == opp) { TRY_MOVE(tr,tc,0); }
                break;
            }
        }
        break;
    }
    case ROOK: {
        int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
        for (int i = 0; i < 4; i++) {
            for (int d = 1; d < 8; d++) {
                int tr = fr+d*dirs[i][0], tc = fc+d*dirs[i][1];
                if (!is_in_bounds(tr,tc)) break;
                if (b->sq[tr][tc].piece == EMPTY) { TRY_MOVE(tr,tc,0); continue; }
                if (b->sq[tr][tc].color == opp) { TRY_MOVE(tr,tc,0); }
                break;
            }
        }
        break;
    }
    case QUEEN: {
        int dirs[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
        for (int i = 0; i < 8; i++) {
            for (int d = 1; d < 8; d++) {
                int tr = fr+d*dirs[i][0], tc = fc+d*dirs[i][1];
                if (!is_in_bounds(tr,tc)) break;
                if (b->sq[tr][tc].piece == EMPTY) { TRY_MOVE(tr,tc,0); continue; }
                if (b->sq[tr][tc].color == opp) { TRY_MOVE(tr,tc,0); }
                break;
            }
        }
        break;
    }
    case KING: {
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc2 = -1; dc2 <= 1; dc2++) {
                if (!dr && !dc2) continue;
                int tr = fr+dr, tc = fc+dc2;
                if (!is_in_bounds(tr,tc)) continue;
                if (b->sq[tr][tc].piece == EMPTY || b->sq[tr][tc].color == opp)
                    TRY_MOVE(tr,tc,0);
            }
        }
        /* Castling */
        if (!is_in_check(b, color)) {
            /* King-side */
            int rank = (color == WHITE) ? 7 : 0;
            if (fr == rank && fc == 4) {
                int wk = (color==WHITE) ? CASTLE_WK : CASTLE_BK;
                int wq = (color==WHITE) ? CASTLE_WQ : CASTLE_BQ;
                if ((b->castle_rights & wk) &&
                    b->sq[rank][5].piece == EMPTY &&
                    b->sq[rank][6].piece == EMPTY &&
                    !is_attacked(b, rank, 5, opp) &&
                    !is_attacked(b, rank, 6, opp)) {
                    TRY_MOVE(rank, 6, 0);
                }
                /* Queen-side */
                if ((b->castle_rights & wq) &&
                    b->sq[rank][3].piece == EMPTY &&
                    b->sq[rank][2].piece == EMPTY &&
                    b->sq[rank][1].piece == EMPTY &&
                    !is_attacked(b, rank, 3, opp) &&
                    !is_attacked(b, rank, 2, opp)) {
                    TRY_MOVE(rank, 2, 0);
                }
            }
        }
        break;
    }
    }
#undef TRY_MOVE
#undef TRY_PROMO
}

int count_legal_moves(Board *b) {
    int total = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (b->sq[r][c].piece != EMPTY && b->sq[r][c].color == b->turn) {
                Move moves[MAX_LEGAL];
                int cnt;
                generate_legal_moves(b, r, c, moves, &cnt);
                total += cnt;
            }
    return total;
}

void detect_game_over(void) {
    Board *b = &g_board;
    if (b->game_over) return;
    int legal = count_legal_moves(b);
    if (legal == 0) {
        if (b->in_check) {
            b->game_over = 1; /* checkmate */
            b->winner    = 1 - b->turn;
        } else {
            b->game_over = 2; /* stalemate */
            b->winner    = -1;
        }
    } else if (b->halfmove >= 100) {
        b->game_over = 3; /* 50-move rule */
        b->winner    = -1;
    }
}

/* ═══════════════════════════════════════════════
   SAN NOTATION
   ═══════════════════════════════════════════════ */
char piece_type_to_char(int t) {
    switch(t) {
        case KNIGHT: return 'N';
        case BISHOP: return 'B';
        case ROOK:   return 'R';
        case QUEEN:  return 'Q';
        case KING:   return 'K';
        default:     return 'P';
    }
}

int piece_char_to_type(char c) {
    switch(toupper(c)) {
        case 'N': return KNIGHT;
        case 'B': return BISHOP;
        case 'R': return ROOK;
        case 'Q': return QUEEN;
        case 'K': return KING;
        default:  return PAWN;
    }
}

void san_from_move(Board *b_before, Move *m, char *san_out) {
    /* Build SAN notation for a move given the board BEFORE the move */
    char buf[32];
    int idx = 0;
    Square mover = b_before->sq[m->from_r][m->from_c];
    int is_cap = (b_before->sq[m->to_r][m->to_c].piece != EMPTY) ||
                 (mover.piece == PAWN && m->to_c != m->from_c &&
                  b_before->sq[m->to_r][m->to_c].piece == EMPTY); /* en passant */

    /* Castling */
    if (mover.piece == KING) {
        int dc = m->to_c - m->from_c;
        if (dc == 2) { strcpy(san_out, "O-O"); goto suffix; }
        if (dc == -2) { strcpy(san_out, "O-O-O"); goto suffix; }
    }

    /* Piece letter */
    if (mover.piece != PAWN) {
        buf[idx++] = piece_type_to_char(mover.piece);
    }

    /* Disambiguation */
    if (mover.piece != PAWN && mover.piece != KING) {
        int ambig_file = 0, ambig_rank = 0, ambig = 0;
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                if (r == m->from_r && c == m->from_c) continue;
                if (b_before->sq[r][c].piece != mover.piece) continue;
                if (b_before->sq[r][c].color != mover.color) continue;
                /* Check if this piece can also move to the same square */
                Move tmp_moves[MAX_LEGAL]; int tmp_cnt;
                generate_legal_moves((Board*)b_before, r, c, tmp_moves, &tmp_cnt);
                for (int i = 0; i < tmp_cnt; i++) {
                    if (tmp_moves[i].to_r == m->to_r && tmp_moves[i].to_c == m->to_c) {
                        ambig = 1;
                        if (c == m->from_c) ambig_rank = 1;
                        else ambig_file = 1;
                    }
                }
            }
        }
        if (ambig) {
            if (!ambig_file) buf[idx++] = 'a' + m->from_c;
            else if (!ambig_rank) buf[idx++] = '8' - m->from_r;
            else { buf[idx++] = 'a' + m->from_c; buf[idx++] = '8' - m->from_r; }
        }
    }

    /* Pawn capture: add file */
    if (mover.piece == PAWN && is_cap) {
        buf[idx++] = 'a' + m->from_c;
    }

    /* Capture marker */
    if (is_cap) buf[idx++] = 'x';

    /* Destination */
    buf[idx++] = 'a' + m->to_c;
    buf[idx++] = '8' - m->to_r;

    /* Promotion */
    if (m->promo) {
        buf[idx++] = '=';
        buf[idx++] = piece_type_to_char(m->promo);
    }
    buf[idx] = 0;
    strcpy(san_out, buf);

suffix:;
    /* Apply move to temp board and check for check/mate */
    Board tmp = *b_before;
    apply_move(&tmp, m);
    tmp.turn = 1 - b_before->turn;
    int chk = is_in_check(&tmp, tmp.turn);
    if (chk) {
        tmp.in_check = chk;
        int legal = count_legal_moves(&tmp);
        if (legal == 0) strcat(san_out, "#");
        else            strcat(san_out, "+");
    }
}

/* ═══════════════════════════════════════════════
   PGN
   ═══════════════════════════════════════════════ */
void rebuild_pgn(void) {
    Board *b = &g_board;
    g_ui.pgn_len = 0;
    g_ui.pgn[0]  = 0;

    /* Header */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char date_buf[20];
    strftime(date_buf, sizeof(date_buf), "%Y.%m.%d", tm_info);

    g_ui.pgn_len += snprintf(g_ui.pgn + g_ui.pgn_len,
        MAX_PGN_LEN - g_ui.pgn_len,
        "[Event \"Terminal Chess\"]\n"
        "[Site \"Mac Terminal\"]\n"
        "[Date \"%s\"]\n"
        "[Round \"?\"]\n"
        "[White \"Player\"]\n"
        "[Black \"%s\"]\n"
        "[Result \"*\"]\n\n",
        date_buf,
        (g_engine.pid > 0 && g_ui.engine_side == BLACK) ? g_engine.path : "Player");

    /* Moves */
    int col = 0;
    for (int i = 0; i < b->hist_len; i++) {
        char tok[32] = "";
        if (i % 2 == 0) {
            snprintf(tok, sizeof(tok), "%d. ", i/2 + 1);
            if (col + (int)strlen(tok) > 76) {
                g_ui.pgn[g_ui.pgn_len++] = '\n';
                col = 0;
            }
            memcpy(g_ui.pgn + g_ui.pgn_len, tok, strlen(tok));
            g_ui.pgn_len += strlen(tok);
            col += strlen(tok);
        }
        snprintf(tok, sizeof(tok), "%s ", b->history[i].san);
        if (col + (int)strlen(tok) > 76) {
            g_ui.pgn[g_ui.pgn_len++] = '\n';
            col = 0;
        }
        memcpy(g_ui.pgn + g_ui.pgn_len, tok, strlen(tok));
        g_ui.pgn_len += strlen(tok);
        col += strlen(tok);
    }

    /* Result */
    const char *result = "*";
    if (g_board.game_over == 1)
        result = (g_board.winner == WHITE) ? "1-0" : "0-1";
    else if (g_board.game_over == 2 || g_board.game_over == 3)
        result = "1/2-1/2";
    if (col + (int)strlen(result) > 76) {
        g_ui.pgn[g_ui.pgn_len++] = '\n';
    }
    g_ui.pgn_len += snprintf(g_ui.pgn + g_ui.pgn_len,
        MAX_PGN_LEN - g_ui.pgn_len, "%s\n", result);
    g_ui.pgn[g_ui.pgn_len] = 0;
}

/* ═══════════════════════════════════════════════
   ENGINE (UCI)
   ═══════════════════════════════════════════════ */
int file_exists(const char *path) {
    return access(path, X_OK) == 0;
}

void engine_start(void) {
    if (!file_exists(g_settings.engine_path)) {
        set_status("Engine not found. Press 'e' to set path.", 1);
        return;
    }

    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) {
        set_status("Failed to create pipes.", 1);
        return;
    }

    g_engine.pid = fork();
    if (g_engine.pid < 0) {
        set_status("Fork failed.", 1);
        return;
    }
    if (g_engine.pid == 0) {
        /* Child: engine process */
        dup2(pipe_in[0],  STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_in[0]);
        close(pipe_out[1]);
        execl(g_settings.engine_path, g_settings.engine_path, (char*)NULL);
        exit(1);
    }
    /* Parent */
    close(pipe_in[0]);
    close(pipe_out[1]);
    g_engine.fd_in  = pipe_in[1];
    g_engine.fd_out = pipe_out[0];
    /* Non-blocking read */
    fcntl(g_engine.fd_out, F_SETFL, O_NONBLOCK);
    
    strncpy(g_engine.path, g_settings.engine_path, 511);
    g_engine.ready    = 0;
    g_engine.thinking = 0;
    g_engine.found_move = 0;

    /* Handshake */
    engine_send("uci\n");
    /* Wait for uciok */
    int waited = 0;
    while (waited < 3000) {
        char *line = engine_read_line(100);
        if (line && strstr(line, "uciok")) { g_engine.ready = 1; break; }
        waited += 100;
    }
    if (!g_engine.ready) {
        set_status("Engine did not respond to UCI. Check path.", 1);
        engine_stop();
        return;
    }
    engine_send("isready\n");
    waited = 0;
    while (waited < 3000) {
        char *line = engine_read_line(100);
        if (line && strstr(line, "readyok")) break;
        waited += 100;
    }

    /* Set options */
    char opt_buf[128];
    if (g_settings.max_depth > 0) {
        snprintf(opt_buf, sizeof(opt_buf),
            "setoption name MultiPV value 1\n");
        engine_send(opt_buf);
    }
    engine_send("ucinewgame\n");
    set_status("Engine ready!", 2);
}

void engine_stop(void) {
    if (g_engine.pid <= 0) return;
    engine_send("quit\n");
    usleep(200000);
    kill(g_engine.pid, SIGTERM);
    waitpid(g_engine.pid, NULL, WNOHANG);
    close(g_engine.fd_in);
    close(g_engine.fd_out);
    g_engine.pid      = 0;
    g_engine.ready    = 0;
    g_engine.thinking = 0;
}

void engine_send(const char *cmd) {
    if (g_engine.fd_in <= 0) return;
    write(g_engine.fd_in, cmd, strlen(cmd));
}

char *engine_read_line(int timeout_ms) {
    static char buf[4096];
    static int  buf_len = 0;
    static char line[4096];

    struct pollfd pfd;
    pfd.fd     = g_engine.fd_out;
    pfd.events = POLLIN;

    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int ret = poll(&pfd, 1, 10);
        if (ret > 0) {
            char tmp[512];
            int n = read(g_engine.fd_out, tmp, sizeof(tmp)-1);
            if (n > 0) {
                if (buf_len + n < 4095) {
                    memcpy(buf + buf_len, tmp, n);
                    buf_len += n;
                    buf[buf_len] = 0;
                }
            }
        }
        /* Check for complete line */
        char *nl = strchr(buf, '\n');
        if (nl) {
            int len = nl - buf;
            memcpy(line, buf, len);
            line[len] = 0;
            /* Remove line from buffer */
            buf_len -= (len + 1);
            memmove(buf, nl+1, buf_len);
            buf[buf_len] = 0;
            return line;
        }
        elapsed += 10;
    }
    return NULL;
}

void engine_set_position(void) {
    /* Build moves string from history */
    char cmd[8192];
    int  pos = 0;
    pos += snprintf(cmd+pos, sizeof(cmd)-pos, "position startpos");
    if (g_board.hist_len > 0) {
        pos += snprintf(cmd+pos, sizeof(cmd)-pos, " moves");
        for (int i = 0; i < g_board.hist_len; i++) {
            Move *m = &g_board.history[i];
            pos += snprintf(cmd+pos, sizeof(cmd)-pos,
                " %c%d%c%d",
                'a' + m->from_c, 8 - m->from_r,
                'a' + m->to_c,   8 - m->to_r);
            if (m->promo) {
                char pc = tolower(piece_type_to_char(m->promo));
                cmd[pos++] = pc;
            }
        }
    }
    cmd[pos++] = '\n';
    cmd[pos]   = 0;
    engine_send(cmd);
}

void engine_go(void) {
    if (!g_engine.ready || g_engine.thinking) return;
    engine_set_position();

    char go_cmd[256];
    int  pos = 0;
    pos += snprintf(go_cmd+pos, sizeof(go_cmd)-pos, "go");
    if (g_settings.max_depth > 0)
        pos += snprintf(go_cmd+pos, sizeof(go_cmd)-pos, " depth %d", g_settings.max_depth);
    if (g_settings.max_nodes > 0)
        pos += snprintf(go_cmd+pos, sizeof(go_cmd)-pos, " nodes %d", g_settings.max_nodes);
    if (g_settings.movetime_ms > 0)
        pos += snprintf(go_cmd+pos, sizeof(go_cmd)-pos, " movetime %d", g_settings.movetime_ms);
    if (g_settings.max_depth == 0 && g_settings.max_nodes == 0 && g_settings.movetime_ms == 0) {
        /* Default: 2 seconds */
        pos += snprintf(go_cmd+pos, sizeof(go_cmd)-pos, " movetime 2000");
    }
    go_cmd[pos++] = '\n';
    go_cmd[pos]   = 0;
    engine_send(go_cmd);
    g_engine.thinking   = 1;
    g_engine.found_move = 0;
    set_status("Engine thinking...", 2);
}

void engine_poll(void) {
    if (!g_engine.ready || !g_engine.thinking) return;
    char *line;
    while ((line = engine_read_line(0)) != NULL) {
        if (strncmp(line, "bestmove", 8) == 0) {
            char mv[10] = "";
            sscanf(line, "bestmove %9s", mv);
            strncpy(g_engine.best_move, mv, 15);
            g_engine.found_move = 1;
            g_engine.thinking   = 0;
            break;
        }
    }

    if (g_engine.found_move && strlen(g_engine.best_move) >= 4) {
        /* Parse UCI move e.g. "e2e4" or "e7e8q" */
        char *mv = g_engine.best_move;
        int fc = mv[0] - 'a';
        int fr = '8' - mv[1];
        int tc = mv[2] - 'a';
        int tr = '8' - mv[3];
        int promo = 0;
        if (mv[4]) promo = piece_char_to_type(mv[4]);

        Move m;
        memset(&m, 0, sizeof(m));
        m.from_r = fr; m.from_c = fc;
        m.to_r   = tr; m.to_c   = tc;
        m.promo  = promo;

        if (make_move(&g_board, &m)) {
            detect_game_over();
            rebuild_pgn();
            if (g_board.game_over == 1) {
                set_status(g_board.winner == WHITE ? "Checkmate! White wins!" :
                           "Checkmate! Black wins!", 1);
            } else if (g_board.game_over == 2) {
                set_status("Stalemate! Draw.", 2);
            } else if (g_board.game_over == 3) {
                set_status("Draw by 50-move rule.", 2);
            } else {
                char smsg[64];
                snprintf(smsg, sizeof(smsg), "Engine played: %s", m.san);
                set_status(smsg, 2);
            }
        } else {
            set_status("Engine illegal move (bug).", 1);
        }
        g_engine.found_move = 0;
        g_engine.best_move[0] = 0;
    }
}

/* ═══════════════════════════════════════════════
   UI HELPERS
   ═══════════════════════════════════════════════ */
void set_status(const char *msg, int color) {
    strncpy(g_ui.status_msg, msg, 255);
    g_ui.status_color = color;
}

void compute_legal_targets(void) {
    memset(g_ui.legal_targets, 0, sizeof(g_ui.legal_targets));
    if (!g_ui.selected) return;
    Move moves[MAX_LEGAL];
    int cnt;
    generate_legal_moves(&g_board, g_ui.sel_r, g_ui.sel_c, moves, &cnt);
    for (int i = 0; i < cnt; i++) {
        g_ui.legal_targets[moves[i].to_r * 8 + moves[i].to_c] = 1;
    }
}

void clear_selection(void) {
    g_ui.selected = 0;
    g_ui.sel_r    = -1;
    g_ui.sel_c    = -1;
    memset(g_ui.legal_targets, 0, sizeof(g_ui.legal_targets));
}

int is_promotion_move(Board *b, int fr, int fc, int tr, int tc) {
    Square sq = b->sq[fr][fc];
    if (sq.piece != PAWN) return 0;
    int promo_row = (sq.color == WHITE) ? 0 : 7;
    return (tr == promo_row);
}

int try_move(int fr, int fc, int tr, int tc) {
    if (g_board.game_over) {
        set_status("Game over. Press 'n' for new game.", 1);
        return 0;
    }
    /* Check it's player's turn */
    if (g_board.sq[fr][fc].color != g_board.turn) {
        set_status("Not your piece.", 1);
        return 0;
    }
    if (g_ui.engine_side == g_board.turn) {
        set_status("Wait for engine's move.", 1);
        return 0;
    }

    int promo = 0;
    if (is_promotion_move(&g_board, fr, fc, tr, tc)) {
        /* Use preset promo choice */
        switch (g_ui.promo_choice) {
            case 'q': promo = QUEEN;  break;
            case 'r': promo = ROOK;   break;
            case 'b': promo = BISHOP; break;
            case 'n': promo = KNIGHT; break;
            default:  promo = QUEEN;  break;
        }
    }

    Move m;
    memset(&m, 0, sizeof(m));
    m.from_r = fr; m.from_c = fc;
    m.to_r   = tr; m.to_c   = tc;
    m.promo  = promo;

    if (!make_move(&g_board, &m)) {
        set_status("Illegal move.", 1);
        return 0;
    }

    detect_game_over();
    rebuild_pgn();

    if (g_board.game_over == 1) {
        set_status(g_board.winner == WHITE ? "Checkmate! White wins!" :
                   "Checkmate! Black wins!", 1);
    } else if (g_board.game_over == 2) {
        set_status("Stalemate! Draw.", 2);
    } else if (g_board.game_over == 3) {
        set_status("Draw by 50-move rule.", 2);
    } else if (g_board.in_check) {
        char smsg[64];
        snprintf(smsg, sizeof(smsg), "Played %s — Check!", m.san);
        set_status(smsg, 2);
    } else {
        char smsg[64];
        snprintf(smsg, sizeof(smsg), "Played: %s", m.san);
        set_status(smsg, 0);
    }

    /* Trigger engine if it's engine's turn */
    if (!g_board.game_over && g_engine.ready &&
        g_ui.engine_side == g_board.turn) {
        engine_go();
    }

    return 1;
}

/* ═══════════════════════════════════════════════
   DRAWING
   ═══════════════════════════════════════════════ */

/* Unicode chess pieces */
static const char *piece_chars[2][7] = {
    /* WHITE */
    {"·", "♙", "♘", "♗", "♖", "♕", "♔"},
    /* BLACK */
    {"·", "♟", "♞", "♝", "♜", "♛", "♚"},
};

/* Board layout: top-left at row=BOARD_TOP, col=BOARD_LEFT */
#define BOARD_TOP   3
#define BOARD_LEFT  5
/* Each square: 4 cols wide, 2 rows tall */
#define SQ_W  4
#define SQ_H  2

static void set_sq_bg(int r, int c, int cur_r, int cur_c,
                      int sel_r, int sel_c, int selected,
                      int last_from_r, int last_from_c,
                      int last_to_r, int last_to_c,
                      int *legal_targets, int in_check) {
    int is_cursor   = (r == cur_r && c == cur_c);
    int is_selected = (selected && r == sel_r && c == sel_c);
    int is_legal    = (legal_targets[r*8+c]);
    int is_last     = ((r==last_from_r && c==last_from_c) ||
                       (r==last_to_r   && c==last_to_c));
    int is_check    = (in_check && g_board.sq[r][c].piece == KING &&
                       g_board.sq[r][c].color == g_board.turn);

    if (is_cursor)        { BG_CURSOR();    return; }
    if (is_selected)      { BG_SELECTED();  return; }
    if (is_legal)         { BG_LEGAL();     return; }
    if (is_check)         { BG_CHECK();     return; }
    if (is_last)          { BG_LAST_MOVE(); return; }
    if ((r + c) % 2 == 0) BG_LIGHT_SQ();
    else                   BG_DARK_SQ();
}

void draw_board(void) {
    Board *b = &g_board;
    int last_from_r=-1, last_from_c=-1, last_to_r=-1, last_to_c=-1;
    if (b->hist_len > 0) {
        Move *lm = &b->history[b->hist_len-1];
        last_from_r = lm->from_r; last_from_c = lm->from_c;
        last_to_r   = lm->to_r;  last_to_c   = lm->to_c;
    }

    /* Draw ranks */
    for (int row = 0; row < 8; row++) {
        int disp_r = g_ui.flip_board ? (7 - row) : row;
        for (int sq_line = 0; sq_line < SQ_H; sq_line++) {
            MOVE_CURSOR(BOARD_TOP + row*SQ_H + sq_line, BOARD_LEFT);

            /* Rank label on left */
            if (sq_line == SQ_H/2) {
                RESET_COLOR();
                BOLD();
                FG_WHITE();
                printf(" %d ", 8 - disp_r);
            } else {
                printf("   ");
            }

            for (int col = 0; col < 8; col++) {
                int disp_c = g_ui.flip_board ? (7 - col) : col;
                set_sq_bg(disp_r, disp_c,
                    g_ui.cur_r, g_ui.cur_c,
                    g_ui.sel_r, g_ui.sel_c,
                    g_ui.selected,
                    last_from_r, last_from_c,
                    last_to_r,   last_to_c,
                    g_ui.legal_targets,
                    b->in_check);

                if (sq_line == SQ_H/2) {
                    /* Middle line: show piece */
                    Square *sq = &b->sq[disp_r][disp_c];
                    if (sq->piece != EMPTY) {
                        /* Piece color */
                        if (sq->color == WHITE) FG_WHITE();
                        else FG_BLACK();
                        BOLD();
                        printf(" %s ", piece_chars[sq->color][sq->piece]);
                    } else {
                        printf("    ");
                    }
                } else {
                    printf("    ");
                }
            }
            RESET_COLOR();
            printf("\n");
        }
    }

    /* File labels */
    MOVE_CURSOR(BOARD_TOP + 8*SQ_H, BOARD_LEFT + 3);
    RESET_COLOR();
    BOLD();
    FG_YELLOW();
    for (int col = 0; col < 8; col++) {
        int disp_c = g_ui.flip_board ? (7-col) : col;
        printf(" %c  ", 'a' + disp_c);
    }
    printf("\n");
    RESET_COLOR();

    /* Right side: game info */
    int info_col = BOARD_LEFT + 3 + 8*SQ_W + 4;
    int info_row = BOARD_TOP;

    MOVE_CURSOR(info_row++, info_col);
    BOLD(); FG_CYAN();
    printf("╔═══════════════════════╗");

    MOVE_CURSOR(info_row++, info_col);
    printf("║  ♟  TERMINAL CHESS   ║");

    MOVE_CURSOR(info_row++, info_col);
    printf("╚═══════════════════════╝");
    RESET_COLOR();

    /* Turn indicator */
    MOVE_CURSOR(info_row++, info_col);
    BOLD();
    printf("Turn: ");
    if (b->turn == WHITE) { FG_WHITE(); printf("White ♙"); }
    else                  { FG_YELLOW(); printf("Black ♟"); }
    RESET_COLOR();

    /* Check indicator */
    if (b->in_check) {
        MOVE_CURSOR(info_row++, info_col);
        BOLD(); FG_RED();
        printf("  ★ CHECK! ★");
        RESET_COLOR();
    } else info_row++;

    /* Game over */
    if (b->game_over) {
        MOVE_CURSOR(info_row++, info_col);
        BOLD(); FG_MAGENTA();
        if (b->game_over == 1)
            printf(b->winner==WHITE ? "★ CHECKMATE White wins!" : "★ CHECKMATE Black wins!");
        else if (b->game_over == 2)
            printf("★ STALEMATE (Draw)");
        else
            printf("★ DRAW (50-move rule)");
        RESET_COLOR();
    } else info_row++;

    info_row++;

    /* Engine status */
    MOVE_CURSOR(info_row++, info_col);
    FG_CYAN();
    if (g_engine.pid > 0) {
        printf("Engine: ");
        FG_GREEN();
        /* basename */
        char *base = strrchr(g_engine.path, '/');
        printf("%.20s", base ? base+1 : g_engine.path);
        if (g_engine.thinking) {
            FG_YELLOW();
            printf(" [thinking]");
        }
    } else {
        printf("Engine: ");
        FG_RED();
        printf("not loaded");
    }
    RESET_COLOR();

    /* Engine side */
    MOVE_CURSOR(info_row++, info_col);
    FG_CYAN();
    printf("Plays as: ");
    FG_WHITE();
    if (g_ui.engine_side == -1)  printf("none");
    else if (g_ui.engine_side == WHITE) printf("White");
    else printf("Black");
    RESET_COLOR();

    /* Settings */
    MOVE_CURSOR(info_row++, info_col);
    FG_CYAN();
    printf("Depth: ");
    FG_WHITE();
    if (g_settings.max_depth > 0) printf("%d", g_settings.max_depth);
    else printf("∞");

    printf("  Nodes: ");
    if (g_settings.max_nodes > 0) printf("%d", g_settings.max_nodes);
    else printf("∞");
    RESET_COLOR();

    MOVE_CURSOR(info_row++, info_col);
    FG_CYAN();
    printf("MoveTime: ");
    FG_WHITE();
    if (g_settings.movetime_ms > 0) printf("%dms", g_settings.movetime_ms);
    else printf("2000ms");
    RESET_COLOR();

    info_row++;

    /* Move history (last 12 moves) */
    MOVE_CURSOR(info_row++, info_col);
    BOLD(); FG_YELLOW();
    printf("Move History:");
    RESET_COLOR();

    int start_i = (b->hist_len > 12) ? b->hist_len - 12 : 0;
    /* Make sure start_i is even for proper move numbering */
    if (start_i % 2 != 0) start_i++;

    for (int i = start_i; i < b->hist_len; i += 2) {
        MOVE_CURSOR(info_row++, info_col);
        FG_WHITE();
        printf("%3d. %-10s", i/2+1, b->history[i].san);
        if (i+1 < b->hist_len) {
            FG_YELLOW();
            printf(" %-10s", b->history[i+1].san);
        }
        RESET_COLOR();
    }

    info_row++;

    /* Promo choice */
    MOVE_CURSOR(info_row++, info_col);
    FG_CYAN();
    printf("Promo: ");
    FG_WHITE();
    switch(g_ui.promo_choice) {
        case 'q': printf("Queen"); break;
        case 'r': printf("Rook");  break;
        case 'b': printf("Bishop");break;
        case 'n': printf("Knight");break;
        default:  printf("Queen"); break;
    }
    RESET_COLOR();

    /* Cursor position */
    MOVE_CURSOR(info_row++, info_col);
    FG_CYAN();
    int disp_cur_r = g_ui.flip_board ? (7-g_ui.cur_r) : g_ui.cur_r;
    int disp_cur_c = g_ui.flip_board ? (7-g_ui.cur_c) : g_ui.cur_c;
    printf("Cursor: %c%d", 'a'+g_ui.cur_c, 8-g_ui.cur_r);
    (void)disp_cur_r; (void)disp_cur_c;
    RESET_COLOR();
}

void draw_status(void) {
    int row = BOARD_TOP + 8*SQ_H + 2;
    MOVE_CURSOR(row, 1);
    BG_STATUS();
    BOLD();
    switch (g_ui.status_color) {
        case 1: FG_RED();     break;
        case 2: FG_GREEN();   break;
        default: FG_WHITE();  break;
    }
    /* Pad to terminal width */
    int len = strlen(g_ui.status_msg);
    printf(" %s", g_ui.status_msg);
    for (int i = len; i < g_term_cols - 2; i++) printf(" ");
    RESET_COLOR();
}

void draw_pgn_panel(void) {
    if (!g_ui.show_pgn) return;
    /* Show PGN in a box below the board */
    int row   = BOARD_TOP + 8*SQ_H + 4;
    int col   = BOARD_LEFT;
    int width = 72;
    int max_rows = g_term_rows - row - 2;
    if (max_rows < 3) return;

    MOVE_CURSOR(row, col);
    BOLD(); FG_CYAN();
    printf("╔");
    for (int i = 0; i < width+2; i++) printf("═");
    printf("╗");

    MOVE_CURSOR(row+1, col);
    printf("║ PGN");
    for (int i = 4; i < width+2; i++) printf(" ");
    printf("║");

    MOVE_CURSOR(row+2, col);
    printf("╠");
    for (int i = 0; i < width+2; i++) printf("═");
    printf("╣");
    RESET_COLOR();

    /* Print PGN lines */
    char pgn_copy[MAX_PGN_LEN];
    strncpy(pgn_copy, g_ui.pgn, MAX_PGN_LEN-1);
    char *saveptr = NULL;
    char *line = strtok_r(pgn_copy, "\n", &saveptr);
    int r = row+3;
    while (line && r < row + max_rows) {
        MOVE_CURSOR(r, col);
        BOLD(); FG_CYAN();
        printf("║ ");
        RESET_COLOR();
        FG_WHITE();
        int llen = strlen(line);
        if (llen > width) llen = width;
        printf("%-*.*s", width, llen, line);
        BOLD(); FG_CYAN();
        printf(" ║");
        RESET_COLOR();
        r++;
        line = strtok_r(NULL, "\n", &saveptr);
    }
    /* Fill remaining rows */
    while (r < row + max_rows) {
        MOVE_CURSOR(r, col);
        BOLD(); FG_CYAN();
        printf("║");
        for (int i = 0; i < width+2; i++) printf(" ");
        printf("║");
        RESET_COLOR();
        r++;
    }
    MOVE_CURSOR(r, col);
    BOLD(); FG_CYAN();
    printf("╚");
    for (int i = 0; i < width+2; i++) printf("═");
    printf("╝");
    RESET_COLOR();
}

void draw_help(void) {
    if (!g_ui.show_help) return;
    /* Overlay help panel */
    int row = BOARD_TOP;
    int col = BOARD_LEFT + 3 + 8*SQ_W + 30;
    
    MOVE_CURSOR(row++, col);
    BOLD(); FG_CYAN();
    printf("╔══════════════════════╗");
    MOVE_CURSOR(row++, col);
    printf("║     KEY BINDINGS     ║");
    MOVE_CURSOR(row++, col);
    printf("╠══════════════════════╣");
    RESET_COLOR();

    const char *keys[] = {
        "║ Arrows  : Move cursor  ║",
        "║ Enter   : Select/Move  ║",
        "║ Esc     : Deselect     ║",
        "║ u       : Undo move    ║",
        "║ f       : Flip board   ║",
        "║ p       : Toggle PGN   ║",
        "║ h       : Toggle help  ║",
        "║ e       : Engine menu  ║",
        "║ s       : Settings     ║",
        "║ n       : New game     ║",
        "║ w/b     : Engine plays ║",
        "║         White/Black    ║",
        "║ x       : Engine off   ║",
        "║ q/Q     : Quit promo   ║",
        "║ r/B/N   : Rook/Bish/Kn ║",
        "║ Ctrl+C  : Exit         ║",
        "╚══════════════════════╝",
        NULL
    };
    for (int i = 0; keys[i]; i++) {
        MOVE_CURSOR(row++, col);
        BOLD(); FG_CYAN();
        printf("%s", keys[i]);
        RESET_COLOR();
    }
}

void draw_all(void) {
    CLEAR_SCREEN();
    /* Title */
    MOVE_CURSOR(1, 1);
    BOLD(); FG_CYAN();
    printf("  ♔ Terminal Chess ♔");
    RESET_COLOR();
    FG_WHITE();
    printf("  [h]=help  [p]=PGN  [u]=undo  [n]=new  [e]=engine  [s]=settings  [q]=quit");
    RESET_COLOR();

    MOVE_CURSOR(2, 1);
    FG_CYAN();
    for (int i = 0; i < g_term_cols && i < 100; i++) printf("─");
    RESET_COLOR();

    draw_board();
    draw_status();
    draw_pgn_panel();
    draw_help();
    fflush(stdout);
}

/* ═══════════════════════════════════════════════
   SETTINGS / ENGINE MENU
   ═══════════════════════════════════════════════ */
void show_engine_menu(void) {
    CLEAR_SCREEN();
    MOVE_CURSOR(1,1);
    BOLD(); FG_CYAN();
    printf("╔═══════════════════════════════════╗\n");
    printf("║         ENGINE SETTINGS            ║\n");
    printf("╠═══════════════════════════════════╣\n");
    RESET_COLOR();

    FG_WHITE();
    printf("║ Current engine: %-18s ║\n",
        strlen(g_settings.engine_path) > 0 ? g_settings.engine_path : "(none)");
    printf("║                                   ║\n");
    printf("║ Options:                          ║\n");
    printf("║  [1] Set engine path              ║\n");
    printf("║  [2] Start engine                 ║\n");
    printf("║  [3] Stop engine                  ║\n");
    printf("║  [4] Set max depth  (0=unlimited) ║\n");
    printf("║  [5] Set max nodes  (0=unlimited) ║\n");
    printf("║  [6] Set move time  (ms)          ║\n");
    printf("║  [w] Engine plays White           ║\n");
    printf("║  [b] Engine plays Black           ║\n");
    printf("║  [x] Engine plays neither         ║\n");
    printf("║  [Esc/q] Back                     ║\n");
    BOLD(); FG_CYAN();
    printf("╚═══════════════════════════════════╝\n");
    RESET_COLOR();

    printf("\nDepth: ");
    FG_GREEN();
    if (g_settings.max_depth > 0) printf("%d", g_settings.max_depth);
    else printf("unlimited");
    RESET_COLOR();

    printf("  Nodes: ");
    FG_GREEN();
    if (g_settings.max_nodes > 0) printf("%d", g_settings.max_nodes);
    else printf("unlimited");
    RESET_COLOR();

    printf("  MoveTime: ");
    FG_GREEN();
    if (g_settings.movetime_ms > 0) printf("%dms", g_settings.movetime_ms);
    else printf("2000ms default");
    RESET_COLOR();

    printf("\nEngine status: ");
    if (g_engine.pid > 0) { FG_GREEN(); printf("Running\n"); }
    else { FG_RED(); printf("Stopped\n"); }
    RESET_COLOR();

    printf("\nChoice: ");
    fflush(stdout);

    /* Temporarily restore canonical mode for input */
    struct termios cooked = g_orig_termios;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &cooked);
    SHOW_CURSOR();

    char choice[4];
    if (fgets(choice, sizeof(choice), stdin) == NULL) choice[0] = 'q';

    setup_terminal();

    char buf[512];
    switch (choice[0]) {
        case '1':
            printf("Enter engine path: ");
            fflush(stdout);
            struct termios t2 = g_orig_termios;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &t2);
            SHOW_CURSOR();
            if (fgets(buf, sizeof(buf), stdin)) {
                buf[strcspn(buf, "\n")] = 0;
                strncpy(g_settings.engine_path, buf, 511);
            }
            setup_terminal();
            break;
        case '2':
            engine_stop();
            engine_start();
            break;
        case '3':
            engine_stop();
            set_status("Engine stopped.", 2);
            break;
        case '4': {
            printf("Max depth (0=unlimited): ");
            fflush(stdout);
            struct termios t3 = g_orig_termios;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &t3);
            SHOW_CURSOR();
            if (fgets(buf, sizeof(buf), stdin))
                g_settings.max_depth = atoi(buf);
            setup_terminal();
            break;
        }
        case '5': {
            printf("Max nodes (0=unlimited): ");
            fflush(stdout);
            struct termios t4 = g_orig_termios;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &t4);
            SHOW_CURSOR();
            if (fgets(buf, sizeof(buf), stdin))
                g_settings.max_nodes = atoi(buf);
            setup_terminal();
            break;
        }
        case '6': {
            printf("Move time in ms (0=2000ms default): ");
            fflush(stdout);
            struct termios t5 = g_orig_termios;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &t5);
            SHOW_CURSOR();
            if (fgets(buf, sizeof(buf), stdin))
                g_settings.movetime_ms = atoi(buf);
            setup_terminal();
            break;
        }
        case 'w':
            g_ui.engine_side = WHITE;
            set_status("Engine plays White.", 2);
            if (!g_board.game_over && g_engine.ready && g_board.turn == WHITE)
                engine_go();
            break;
        case 'b':
            g_ui.engine_side = BLACK;
            set_status("Engine plays Black.", 2);
            if (!g_board.game_over && g_engine.ready && g_board.turn == BLACK)
                engine_go();
            break;
        case 'x':
            g_ui.engine_side = -1;
            set_status("Engine disabled.", 2);
            break;
        default:
            break;
    }
}

/* ═══════════════════════════════════════════════
   INPUT HANDLING
   ═══════════════════════════════════════════════ */
void handle_input(void) {
    unsigned char buf[8];
    int n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return;

    unsigned char ch = buf[0];

    /* Arrow keys: ESC [ A/B/C/D */
    if (ch == 27 && n >= 3 && buf[1] == '[') {
        /* Move cursor in visual space, then map to board space */
        int delta_r = 0, delta_c = 0;
        switch(buf[2]) {
            case 'A': delta_r = -1; break; /* up */
            case 'B': delta_r =  1; break; /* down */
            case 'C': delta_c =  1; break; /* right */
            case 'D': delta_c = -1; break; /* left */
        }
        if (g_ui.flip_board) { delta_r = -delta_r; delta_c = -delta_c; }

        g_ui.cur_r = (g_ui.cur_r + delta_r + 8) % 8;
        g_ui.cur_c = (g_ui.cur_c + delta_c + 8) % 8;
        return;
    }

    /* ESC alone: deselect */
    if (ch == 27 && n == 1) {
        clear_selection();
        set_status("Selection cleared.", 0);
        return;
    }

    /* Enter / Space: select or move */
    if (ch == '\r' || ch == '\n' || ch == ' ') {
        int r = g_ui.cur_r, c = g_ui.cur_c;
        if (!g_ui.selected) {
            /* Select piece */
            Square *sq = &g_board.sq[r][c];
            if (sq->piece != EMPTY && sq->color == g_board.turn) {
                if (g_ui.engine_side == g_board.turn) {
                    set_status("Wait for engine.", 1);
                    return;
                }
                g_ui.selected = 1;
                g_ui.sel_r    = r;
                g_ui.sel_c    = c;
                compute_legal_targets();
                set_status("Piece selected. Move cursor and press Enter.", 0);
            } else if (sq->piece != EMPTY) {
                set_status("That's not your piece.", 1);
            } else {
                set_status("Empty square.", 1);
            }
        } else {
            /* Move or re-select */
            if (r == g_ui.sel_r && c == g_ui.sel_c) {
                clear_selection();
                set_status("Deselected.", 0);
            } else if (g_ui.legal_targets[r*8+c]) {
                int ok = try_move(g_ui.sel_r, g_ui.sel_c, r, c);
                clear_selection();
                if (!ok) set_status("Illegal move.", 1);
            } else {
                /* Maybe reselect a friendly piece */
                Square *sq = &g_board.sq[r][c];
                if (sq->piece != EMPTY && sq->color == g_board.turn &&
                    g_ui.engine_side != g_board.turn) {
                    g_ui.sel_r = r; g_ui.sel_c = c;
                    compute_legal_targets();
                    set_status("Reselected.", 0);
                } else {
                    clear_selection();
                    set_status("Not a legal target.", 1);
                }
            }
        }
        return;
    }

    /* Letter commands */
    switch(tolower(ch)) {
        case 'u': /* undo */
            if (g_engine.thinking) { set_status("Can't undo while engine thinks.", 1); break; }
            if (g_board.hist_len == 0) { set_status("Nothing to undo.", 1); break; }
            /* If engine plays, undo twice (engine move + player move) */
            undo_move(&g_board);
            if (g_ui.engine_side != -1 && g_board.hist_len > 0 &&
                g_board.turn == g_ui.engine_side) {
                undo_move(&g_board);
            }
            clear_selection();
            rebuild_pgn();
            set_status("Move undone.", 2);
            break;

        case 'f': /* flip board */
            g_ui.flip_board = !g_ui.flip_board;
            set_status(g_ui.flip_board ? "Board flipped (Black's perspective)." :
                       "Board normal (White's perspective).", 0);
            break;

        case 'p': /* toggle PGN */
            g_ui.show_pgn = !g_ui.show_pgn;
            break;

        case 'h': /* toggle help */
            g_ui.show_help = !g_ui.show_help;
            break;

        case 'e': /* engine menu */
            show_engine_menu();
            break;

        case 'n': /* new game */
            if (g_engine.thinking) { engine_send("stop\n"); usleep(100000); }
            g_engine.thinking = 0;
            board_init(&g_board);
            clear_selection();
            rebuild_pgn();
            set_status("New game started.", 2);
            if (!g_board.game_over && g_engine.ready &&
                g_ui.engine_side == g_board.turn) {
                engine_go();
            }
            break;

        case 'w': /* engine plays white */
            g_ui.engine_side = WHITE;
            set_status("Engine plays White.", 2);
            if (!g_board.game_over && g_engine.ready && g_board.turn == WHITE)
                engine_go();
            break;

        case 'b': /* engine plays black */
            if (!g_ui.selected) { /* Don't conflict with bishop promo */
                g_ui.engine_side = BLACK;
                set_status("Engine plays Black.", 2);
                if (!g_board.game_over && g_engine.ready && g_board.turn == BLACK)
                    engine_go();
            }
            break;

        case 'x': /* engine off */
            g_ui.engine_side = -1;
            if (g_engine.thinking) { engine_send("stop\n"); g_engine.thinking=0; }
            set_status("Engine disabled.", 2);
            break;

        /* Promotion piece selection */
        case 'q':
            g_ui.promo_choice = 'q';
            set_status("Promotion: Queen", 0);
            break;
        case 'r':
            g_ui.promo_choice = 'r';
            set_status("Promotion: Rook", 0);
            break;
        case 'v': /* v for bishop to avoid conflict */
            g_ui.promo_choice = 'b';
            set_status("Promotion: Bishop", 0);
            break;
        case 'k':
            g_ui.promo_choice = 'n';
            set_status("Promotion: Knight", 0);
            break;

        case 's': /* settings quick display */
            set_status("Settings: e=engine menu, w/b=engine side, u=undo, f=flip", 2);
            break;

        case 3: /* Ctrl+C */
            restore_terminal();
            exit(0);
            break;

        default:
            break;
    }
}

/* ═══════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════ */
int main(void) {
    /* Init settings */
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.max_depth    = 0;
    g_settings.max_nodes    = 0;
    g_settings.movetime_ms  = 2000;
    /* Try common engine locations */
    const char *common_paths[] = {
        "/usr/local/bin/stockfish",
        "/opt/homebrew/bin/stockfish",
        "/usr/bin/stockfish",
        "./stockfish",
        NULL
    };
    for (int i = 0; common_paths[i]; i++) {
        if (file_exists(common_paths[i])) {
            strncpy(g_settings.engine_path, common_paths[i], 511);
            break;
        }
    }

    /* Init game */
    memset(&g_engine, 0, sizeof(g_engine));
    memset(&g_ui,     0, sizeof(g_ui));
    g_ui.cur_r        = 7; /* start at a1 area */
    g_ui.cur_c        = 4;
    g_ui.sel_r        = -1;
    g_ui.sel_c        = -1;
    g_ui.engine_side  = -1;
    g_ui.promo_choice = 'q';
    g_ui.show_pgn     = 1;
    g_ui.show_help    = 0;

    board_init(&g_board);
    rebuild_pgn();
    set_status("Welcome to Terminal Chess! Press 'h' for help, 'e' to load engine.", 2);

    /* Setup terminal */
    setup_terminal();
    signal(SIGWINCH, sigwinch_handler);
    get_term_size();

    /* Auto-start engine if found */
    if (strlen(g_settings.engine_path) > 0) {
        engine_start();
    }

    /* Main loop */
    while (1) {
        /* Handle terminal resize */
        if (g_resize_flag) {
            g_resize_flag = 0;
            get_term_size();
        }

        /* Poll engine output */
        if (g_engine.ready && g_engine.thinking) {
            engine_poll();
        }

        /* Draw */
        draw_all();

        /* Input (non-blocking, 80ms) */
        struct pollfd pfd;
        pfd.fd     = STDIN_FILENO;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 80);
        if (ret > 0) {
            handle_input();
        }
    }

    restore_terminal();
    engine_stop();
    return 0;
}
