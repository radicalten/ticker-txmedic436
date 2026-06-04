/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 * Compile: gcc -o chess_gui chess_gui.c
 * Run:     ./chess_gui
 *
 * Controls:
 *   Arrow keys  - Move cursor
 *   Enter/Space - Select piece / confirm move
 *   U           - Undo last move
 *   Q           - Quit
 *   E           - Set engine path
 *   T           - Set time controls
 *   N           - New game
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

/* ─────────────────────────── ANSI / Terminal ─────────────────────────── */

#define ANSI_RESET       "\033[0m"
#define ANSI_BOLD        "\033[1m"
#define ANSI_FG_BLACK    "\033[30m"
#define ANSI_FG_WHITE    "\033[97m"
#define ANSI_FG_RED      "\033[91m"
#define ANSI_FG_GREEN    "\033[92m"
#define ANSI_FG_YELLOW   "\033[93m"
#define ANSI_FG_CYAN     "\033[96m"
#define ANSI_FG_MAGENTA  "\033[95m"
#define ANSI_BG_DARK     "\033[48;5;94m"   /* dark square  */
#define ANSI_BG_LIGHT    "\033[48;5;229m"  /* light square */
#define ANSI_BG_CURSOR   "\033[48;5;33m"   /* cursor       */
#define ANSI_BG_SELECTED "\033[48;5;208m"  /* selected     */
#define ANSI_BG_LEGAL    "\033[48;5;34m"   /* legal move   */
#define ANSI_BG_LASTMOVE "\033[48;5;130m"  /* last move    */
#define ANSI_BG_CHECK    "\033[48;5;196m"  /* king in check*/
#define ANSI_CLEAR       "\033[2J\033[H"
#define ANSI_HIDE_CURSOR "\033[?25l"
#define ANSI_SHOW_CURSOR "\033[?25h"

/* ─────────────────────────── Chess Constants ─────────────────────────── */

#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  0
#define BLACK  1

#define MAX_MOVES     256
#define MAX_HISTORY   512
#define MAX_PGN_LEN   16384
#define MAX_LEGAL      256

/* Castling rights flags */
#define CASTLE_WK  1
#define CASTLE_WQ  2
#define CASTLE_BK  4
#define CASTLE_BQ  8

/* ─────────────────────────── Data Structures ─────────────────────────── */

typedef struct {
    int piece;   /* piece type */
    int color;   /* WHITE or BLACK */
} Square;

typedef struct {
    int from_r, from_c;
    int to_r,   to_c;
    int promo;           /* promotion piece type, 0 if none */
    int captured_piece;
    int captured_color;
    int captured_r, captured_c; /* for en-passant */
    int ep_file;         /* en-passant file before this move (-1 = none) */
    int castling;        /* castling rights before this move */
    int is_castle;       /* 0=no, 1=kingside, -1=queenside */
    int is_ep;           /* en-passant capture */
    int half_move;       /* halfmove clock before move */
    char san[16];        /* SAN notation */
} Move;

typedef struct {
    Square board[8][8];
    int    turn;          /* WHITE or BLACK */
    int    ep_file;       /* en-passant target file, -1 if none */
    int    ep_rank;       /* en-passant target rank */
    int    castling;      /* castling rights bitmask */
    int    half_move;     /* halfmove clock */
    int    full_move;     /* fullmove number */
    Move   history[MAX_HISTORY];
    int    history_count;
    char   pgn[MAX_PGN_LEN];
    int    game_over;     /* 0=playing, 1=white wins, 2=black wins, 3=draw */
    char   result_str[64];
} GameState;

typedef struct {
    int r, c;
} LegalMove;

typedef struct {
    char   path[512];
    pid_t  pid;
    int    pipe_in[2];   /* parent writes -> engine reads  */
    int    pipe_out[2];  /* engine writes -> parent reads  */
    int    running;
    int    uci_ready;
    /* time controls */
    int    use_depth;    int depth;
    int    use_nodes;    long long nodes;
    int    use_time;     int movetime_ms;
    int    wtime, btime, winc, binc;
    int    use_clock;
    char   best_move[16];
    int    engine_color; /* which side engine plays: WHITE, BLACK, or -1=none */
} Engine;

/* ─────────────────────────── Globals ─────────────────────────── */

static GameState G;
static Engine    E;

static int cursor_r = 7, cursor_c = 0;
static int sel_r = -1,   sel_c = -1;   /* selected square */
static int selected = 0;

static LegalMove legal_targets[MAX_LEGAL];
static int       legal_count = 0;

static struct termios orig_term;
static char status_msg[256] = "Welcome! Press 'H' for help.";

/* ─────────────────────────── Terminal Setup ─────────────────────────── */

static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &orig_term);
    t = orig_term;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    printf(ANSI_HIDE_CURSOR);
    fflush(stdout);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    printf(ANSI_SHOW_CURSOR "\n");
    fflush(stdout);
}

/* Read a key; returns key code. Handles escape sequences. */
#define KEY_UP     1001
#define KEY_DOWN   1002
#define KEY_LEFT   1003
#define KEY_RIGHT  1004
#define KEY_ENTER  1005
#define KEY_ESC    27

static int read_key(void) {
    unsigned char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;
    if (c == 27) {
        unsigned char seq[4] = {0};
        read(STDIN_FILENO, &seq[0], 1);
        if (seq[0] == '[') {
            read(STDIN_FILENO, &seq[1], 1);
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return KEY_ESC;
    }
    if (c == '\n' || c == '\r') return KEY_ENTER;
    return (int)c;
}

/* ─────────────────────────── Board Initialization ─────────────────────────── */

static void init_board(void) {
    memset(&G, 0, sizeof(G));
    G.ep_file      = -1;
    G.ep_rank      = -1;
    G.castling     = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    G.full_move    = 1;
    G.half_move    = 0;
    G.history_count = 0;
    G.game_over    = 0;
    memset(G.pgn, 0, sizeof(G.pgn));

    /* Place pieces */
    static const int back_row[] = {ROOK,KNIGHT,BISHOP,QUEEN,KING,BISHOP,KNIGHT,ROOK};
    for (int c = 0; c < 8; c++) {
        G.board[0][c].piece = back_row[c]; G.board[0][c].color = BLACK;
        G.board[1][c].piece = PAWN;        G.board[1][c].color = BLACK;
        G.board[6][c].piece = PAWN;        G.board[6][c].color = WHITE;
        G.board[7][c].piece = back_row[c]; G.board[7][c].color = WHITE;
    }

    snprintf(status_msg, sizeof(status_msg),
             "New game! White to move. Arrow keys to navigate, Enter to select.");
}

/* ─────────────────────────── Move Generation ─────────────────────────── */

static int in_bounds(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }

static int is_empty(int r, int c) { return G.board[r][c].piece == EMPTY; }

static int is_enemy(int r, int c, int color) {
    return !is_empty(r,c) && G.board[r][c].color != color;
}

/* Check if square (r,c) is attacked by 'attacker_color' */
static int is_attacked(int r, int c, int attacker_color, Square board[8][8]) {
    int ac = attacker_color;
    /* Pawns */
    int pdir = (ac == WHITE) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int pr = r + pdir, pc = c + dc;
        if (in_bounds(pr,pc) && board[pr][pc].piece == PAWN && board[pr][pc].color == ac)
            return 1;
    }
    /* Knights */
    int kd[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r+kd[i][0], nc = c+kd[i][1];
        if (in_bounds(nr,nc) && board[nr][nc].piece == KNIGHT && board[nr][nc].color == ac)
            return 1;
    }
    /* Bishops / Queens (diagonals) */
    int dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int d = 0; d < 4; d++) {
        for (int s = 1; s < 8; s++) {
            int nr = r+dirs[d][0]*s, nc = c+dirs[d][1]*s;
            if (!in_bounds(nr,nc)) break;
            if (board[nr][nc].piece != EMPTY) {
                if (board[nr][nc].color == ac &&
                    (board[nr][nc].piece == BISHOP || board[nr][nc].piece == QUEEN))
                    return 1;
                break;
            }
        }
    }
    /* Rooks / Queens (straights) */
    int rdirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int d = 0; d < 4; d++) {
        for (int s = 1; s < 8; s++) {
            int nr = r+rdirs[d][0]*s, nc = c+rdirs[d][1]*s;
            if (!in_bounds(nr,nc)) break;
            if (board[nr][nc].piece != EMPTY) {
                if (board[nr][nc].color == ac &&
                    (board[nr][nc].piece == ROOK || board[nr][nc].piece == QUEEN))
                    return 1;
                break;
            }
        }
    }
    /* King */
    for (int dr = -1; dr <= 1; dr++) for (int dc2 = -1; dc2 <= 1; dc2++) {
        if (!dr && !dc2) continue;
        int nr = r+dr, nc = c+dc2;
        if (in_bounds(nr,nc) && board[nr][nc].piece == KING && board[nr][nc].color == ac)
            return 1;
    }
    return 0;
}

/* Find king position for color on board */
static void find_king(Square board[8][8], int color, int *kr, int *kc) {
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (board[r][c].piece == KING && board[r][c].color == color) {
                *kr = r; *kc = c; return;
            }
    *kr = -1; *kc = -1;
}

static int in_check_board(Square board[8][8], int color) {
    int kr, kc;
    find_king(board, color, &kr, &kc);
    if (kr < 0) return 0;
    return is_attacked(kr, kc, 1-color, board);
}

/* Apply a pseudo-move to a temporary board and check legality */
static int is_legal_move(int fr, int fc, int tr, int tc, int promo) {
    Square tmp[8][8];
    memcpy(tmp, G.board, sizeof(tmp));

    int piece = tmp[fr][fc].piece;
    int color = tmp[fr][fc].color;

    /* En-passant */
    int is_ep = 0;
    if (piece == PAWN && tc == G.ep_file && tr == G.ep_rank) {
        is_ep = 1;
        int cap_r = (color == WHITE) ? tr+1 : tr-1;
        tmp[cap_r][tc].piece = EMPTY;
        tmp[cap_r][tc].color = EMPTY;
    }

    tmp[tr][tc] = tmp[fr][fc];
    tmp[fr][fc].piece = EMPTY;
    tmp[fr][fc].color = 0;
    if (promo) tmp[tr][tc].piece = promo;

    /* Castling: also move rook */
    if (piece == KING && abs(tc - fc) == 2) {
        if (tc > fc) { /* kingside */
            tmp[fr][5] = tmp[fr][7];
            tmp[fr][7].piece = EMPTY;
        } else { /* queenside */
            tmp[fr][3] = tmp[fr][0];
            tmp[fr][0].piece = EMPTY;
        }
    }

    return !in_check_board(tmp, color);
}

typedef struct { int r, c, promo; } PseudoMove;

/* Generate pseudo-legal moves for piece at (r,c) */
static int gen_pseudo(int r, int c, PseudoMove *out) {
    int n = 0;
    if (G.board[r][c].piece == EMPTY) return 0;
    int piece = G.board[r][c].piece;
    int color = G.board[r][c].color;

#define ADD(R,C) do { if(in_bounds(R,C)) { out[n].r=(R);out[n].c=(C);out[n].promo=0;n++; } } while(0)
#define ADD_P(R,C,P) do { out[n].r=(R);out[n].c=(C);out[n].promo=(P);n++; } while(0)

    if (piece == PAWN) {
        int dir = (color == WHITE) ? -1 : 1;
        int start_r = (color == WHITE) ? 6 : 1;
        int promo_r = (color == WHITE) ? 0 : 7;
        int nr = r + dir;
        if (in_bounds(nr,c) && is_empty(nr,c)) {
            if (nr == promo_r) {
                ADD_P(nr,c,QUEEN); ADD_P(nr,c,ROOK);
                ADD_P(nr,c,BISHOP); ADD_P(nr,c,KNIGHT);
            } else {
                ADD(nr,c);
                if (r == start_r && is_empty(r+2*dir,c)) ADD(r+2*dir,c);
            }
        }
        for (int dc = -1; dc <= 1; dc += 2) {
            int nc = c + dc;
            if (!in_bounds(nr,nc)) continue;
            if (is_enemy(nr,nc,color) || (nc == G.ep_file && nr == G.ep_rank)) {
                if (nr == promo_r) {
                    ADD_P(nr,nc,QUEEN); ADD_P(nr,nc,ROOK);
                    ADD_P(nr,nc,BISHOP); ADD_P(nr,nc,KNIGHT);
                } else ADD(nr,nc);
            }
        }
    } else if (piece == KNIGHT) {
        int kd[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r+kd[i][0], nc = c+kd[i][1];
            if (in_bounds(nr,nc) && !(!is_empty(nr,nc) && G.board[nr][nc].color == color))
                ADD(nr,nc);
        }
    } else if (piece == BISHOP || piece == QUEEN) {
        int dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
        for (int d = 0; d < 4; d++) {
            for (int s = 1; s < 8; s++) {
                int nr = r+dirs[d][0]*s, nc = c+dirs[d][1]*s;
                if (!in_bounds(nr,nc)) break;
                if (!is_empty(nr,nc)) {
                    if (G.board[nr][nc].color != color) ADD(nr,nc);
                    break;
                }
                ADD(nr,nc);
            }
        }
        if (piece == BISHOP) goto done;
    }
    if (piece == ROOK || piece == QUEEN) {
        int rdirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
        for (int d = 0; d < 4; d++) {
            for (int s = 1; s < 8; s++) {
                int nr = r+rdirs[d][0]*s, nc = c+rdirs[d][1]*s;
                if (!in_bounds(nr,nc)) break;
                if (!is_empty(nr,nc)) {
                    if (G.board[nr][nc].color != color) ADD(nr,nc);
                    break;
                }
                ADD(nr,nc);
            }
        }
        if (piece == ROOK) goto done;
    }
    if (piece == KING) {
        for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            int nr = r+dr, nc = c+dc;
            if (in_bounds(nr,nc) && !(G.board[nr][nc].piece && G.board[nr][nc].color == color))
                ADD(nr,nc);
        }
        /* Castling */
        if (color == WHITE && r == 7 && c == 4) {
            if ((G.castling & CASTLE_WK) && is_empty(7,5) && is_empty(7,6) &&
                !is_attacked(7,4,BLACK,G.board) && !is_attacked(7,5,BLACK,G.board) && !is_attacked(7,6,BLACK,G.board))
                ADD(7,6);
            if ((G.castling & CASTLE_WQ) && is_empty(7,3) && is_empty(7,2) && is_empty(7,1) &&
                !is_attacked(7,4,BLACK,G.board) && !is_attacked(7,3,BLACK,G.board) && !is_attacked(7,2,BLACK,G.board))
                ADD(7,2);
        }
        if (color == BLACK && r == 0 && c == 4) {
            if ((G.castling & CASTLE_BK) && is_empty(0,5) && is_empty(0,6) &&
                !is_attacked(0,4,WHITE,G.board) && !is_attacked(0,5,WHITE,G.board) && !is_attacked(0,6,WHITE,G.board))
                ADD(0,6);
            if ((G.castling & CASTLE_BQ) && is_empty(0,3) && is_empty(0,2) && is_empty(0,1) &&
                !is_attacked(0,4,WHITE,G.board) && !is_attacked(0,3,WHITE,G.board) && !is_attacked(0,2,WHITE,G.board))
                ADD(0,2);
        }
    }
done:
    return n;
}

/* Get fully legal moves for piece at (r,c) */
static int get_legal_moves(int r, int c, LegalMove *out) {
    PseudoMove pm[MAX_MOVES];
    int pn = gen_pseudo(r, c, pm);
    int n = 0;
    for (int i = 0; i < pn; i++) {
        if (is_legal_move(r, c, pm[i].r, pm[i].c, pm[i].promo)) {
            out[n].r = pm[i].r;
            out[n].c = pm[i].c;
            n++;
            /* If promotion, don't duplicate */
            while (i+1 < pn && pm[i+1].r == pm[i].r && pm[i+1].c == pm[i].c) i++;
        }
    }
    return n;
}

/* Check if the current player has any legal moves */
static int has_any_legal_move(int color) {
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            if (G.board[r][c].piece == EMPTY) continue;
            if (G.board[r][c].color != color) continue;
            PseudoMove pm[MAX_MOVES];
            int pn = gen_pseudo(r, c, pm);
            for (int i = 0; i < pn; i++)
                if (is_legal_move(r, c, pm[i].r, pm[i].c, pm[i].promo))
                    return 1;
        }
    return 0;
}

/* ─────────────────────────── SAN Notation ─────────────────────────── */

static char piece_char(int p) {
    static const char pc[] = " PNBRQK";
    return pc[p];
}

static void move_to_san(int fr, int fc, int tr, int tc, int promo, char *out) {
    int piece  = G.board[fr][fc].piece;
    int color  = G.board[fr][fc].color;
    char buf[32];
    int  idx = 0;

    /* Castling */
    if (piece == KING && abs(tc - fc) == 2) {
        strcpy(out, tc > fc ? "O-O" : "O-O-O");
        return;
    }

    if (piece != PAWN) buf[idx++] = piece_char(piece);

    /* Disambiguation */
    if (piece != PAWN) {
        int amb_file = 0, amb_rank = 0, amb_count = 0;
        for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
            if (r == fr && c == fc) continue;
            if (G.board[r][c].piece != piece) continue;
            if (G.board[r][c].color != color) continue;
            if (!is_legal_move(r, c, tr, tc, promo)) continue;
            PseudoMove pm[MAX_MOVES];
            int pn = gen_pseudo(r, c, pm);
            for (int i = 0; i < pn; i++) {
                if (pm[i].r == tr && pm[i].c == tc) {
                    amb_count++;
                    if (c == fc) amb_rank = 1;
                    if (r == fr) amb_file = 1;
                }
            }
        }
        if (amb_count > 0) {
            if (!amb_file)       buf[idx++] = 'a' + fc;
            else if (!amb_rank)  buf[idx++] = '8' - fr;
            else { buf[idx++] = 'a' + fc; buf[idx++] = '8' - fr; }
        }
    }

    /* Capture / pawn file */
    int is_capture = !is_empty(tr, tc) ||
                     (piece == PAWN && tc == G.ep_file && tr == G.ep_rank);
    if (piece == PAWN && is_capture) buf[idx++] = 'a' + fc;
    if (is_capture) buf[idx++] = 'x';

    buf[idx++] = 'a' + tc;
    buf[idx++] = '8' - tr;

    if (promo) { buf[idx++] = '='; buf[idx++] = piece_char(promo); }
    buf[idx] = 0;

    /* Simulate move to check check/checkmate */
    Square tmp[8][8];
    memcpy(tmp, G.board, sizeof(tmp));
    tmp[tr][tc] = tmp[fr][fc];
    tmp[fr][fc].piece = EMPTY;
    if (promo) tmp[tr][tc].piece = promo;
    if (piece == PAWN && tc == G.ep_file && tr == G.ep_rank) {
        int cap_r = (color == WHITE) ? tr+1 : tr-1;
        tmp[cap_r][tc].piece = EMPTY;
    }
    if (piece == KING && abs(tc - fc) == 2) {
        if (tc > fc) { tmp[fr][5] = tmp[fr][7]; tmp[fr][7].piece = EMPTY; }
        else         { tmp[fr][3] = tmp[fr][0]; tmp[fr][0].piece = EMPTY; }
    }

    int opp = 1 - color;
    if (in_check_board(tmp, opp)) buf[idx++] = '+';
    buf[idx] = 0;

    strcpy(out, buf);
}

/* ─────────────────────────── PGN Update ─────────────────────────── */

static void pgn_append(Move *m) {
    char tmp[64];
    if (G.history_count % 2 == 1) {
        /* White just moved (history_count was odd before increment... 
         * Actually count is already incremented. Let's check: 
         * We store the move then call pgn_append. Let's use full_move. */
    }
    /* White move: add move number */
    if (m->captured_color == WHITE || /* not relevant */
        G.board[m->to_r][m->to_c].color == WHITE) {
        /* Check turn from full_move */
    }
    /* Simple approach: rebuild PGN from history */
    G.pgn[0] = 0;
    int pos = 0;
    for (int i = 0; i < G.history_count; i++) {
        if (i % 2 == 0) {
            int fmn = 1 + i / 2;
            int len = snprintf(tmp, sizeof(tmp), "%d. ", fmn);
            memcpy(G.pgn + pos, tmp, len);
            pos += len;
        }
        int slen = strlen(G.history[i].san);
        memcpy(G.pgn + pos, G.history[i].san, slen);
        pos += slen;
        G.pgn[pos++] = ' ';
        if (pos >= MAX_PGN_LEN - 64) break;
    }
    /* Result */
    if (G.game_over) {
        const char *res = (G.game_over == 1) ? "1-0" :
                          (G.game_over == 2) ? "0-1" : "1/2-1/2";
        int rlen = strlen(res);
        memcpy(G.pgn + pos, res, rlen);
        pos += rlen;
    }
    G.pgn[pos] = 0;
}

/* ─────────────────────────── Apply / Undo Move ─────────────────────────── */

static void apply_move(int fr, int fc, int tr, int tc, int promo) {
    if (G.history_count >= MAX_HISTORY) return;

    Move *m = &G.history[G.history_count];
    memset(m, 0, sizeof(*m));
    m->from_r = fr; m->from_c = fc;
    m->to_r   = tr; m->to_c   = tc;
    m->promo  = promo;
    m->ep_file = G.ep_file;
    m->ep_rank = (G.ep_rank >= 0) ? G.ep_rank : -1;
    m->castling = G.castling;
    m->half_move = G.half_move;

    int piece = G.board[fr][fc].piece;
    int color = G.board[fr][fc].color;

    /* Capture info */
    m->captured_piece = G.board[tr][tc].piece;
    m->captured_color = G.board[tr][tc].color;
    m->captured_r     = tr;
    m->captured_c     = tc;

    /* En-passant capture */
    if (piece == PAWN && tc == G.ep_file && tr == G.ep_rank) {
        m->is_ep = 1;
        int cap_r = (color == WHITE) ? tr+1 : tr-1;
        m->captured_piece = G.board[cap_r][tc].piece;
        m->captured_color = G.board[cap_r][tc].color;
        m->captured_r     = cap_r;
        m->captured_c     = tc;
        G.board[cap_r][tc].piece = EMPTY;
        G.board[cap_r][tc].color = 0;
    }

    /* Castling */
    if (piece == KING && abs(tc - fc) == 2) {
        m->is_castle = (tc > fc) ? 1 : -1;
        if (tc > fc) {
            G.board[fr][5] = G.board[fr][7];
            G.board[fr][7].piece = EMPTY;
            G.board[fr][7].color = 0;
        } else {
            G.board[fr][3] = G.board[fr][0];
            G.board[fr][0].piece = EMPTY;
            G.board[fr][0].color = 0;
        }
    }

    /* Move piece */
    G.board[tr][tc] = G.board[fr][fc];
    G.board[fr][fc].piece = EMPTY;
    G.board[fr][fc].color = 0;

    if (promo) G.board[tr][tc].piece = promo;

    /* SAN (before state update) — we need to compute before updating ep/castling */
    move_to_san(fr, fc, tr, tc, promo, m->san);

    /* Update castling rights */
    if (piece == KING) {
        if (color == WHITE) G.castling &= ~(CASTLE_WK | CASTLE_WQ);
        else                G.castling &= ~(CASTLE_BK | CASTLE_BQ);
    }
    if (piece == ROOK) {
        if (fr == 7 && fc == 7) G.castling &= ~CASTLE_WK;
        if (fr == 7 && fc == 0) G.castling &= ~CASTLE_WQ;
        if (fr == 0 && fc == 7) G.castling &= ~CASTLE_BK;
        if (fr == 0 && fc == 0) G.castling &= ~CASTLE_BQ;
    }
    /* Rook captured */
    if (tr == 7 && tc == 7) G.castling &= ~CASTLE_WK;
    if (tr == 7 && tc == 0) G.castling &= ~CASTLE_WQ;
    if (tr == 0 && tc == 7) G.castling &= ~CASTLE_BK;
    if (tr == 0 && tc == 0) G.castling &= ~CASTLE_BQ;

    /* En-passant */
    G.ep_file = -1; G.ep_rank = -1;
    if (piece == PAWN && abs(tr - fr) == 2) {
        G.ep_file = fc;
        G.ep_rank = (fr + tr) / 2;
    }

    /* Halfmove clock */
    if (piece == PAWN || m->captured_piece) G.half_move = 0;
    else G.half_move++;

    /* Fullmove */
    if (color == BLACK) G.full_move++;

    G.turn = 1 - G.turn;
    G.history_count++;

    /* Append SAN to PGN check (update check symbol) */
    /* Check if the move gives check - update '+' or '#' in SAN */
    if (in_check_board(G.board, G.turn)) {
        int san_len = strlen(m->san);
        if (m->san[san_len-1] == '+') {
            /* check if checkmate */
            if (!has_any_legal_move(G.turn)) {
                m->san[san_len-1] = '#';
                G.game_over = (G.turn == BLACK) ? 1 : 2;
                snprintf(G.result_str, sizeof(G.result_str),
                         "%s wins by checkmate!", G.turn == BLACK ? "White" : "Black");
            }
        } else {
            m->san[san_len] = '+';
            m->san[san_len+1] = 0;
            if (!has_any_legal_move(G.turn)) {
                m->san[san_len] = '#';
                G.game_over = (G.turn == BLACK) ? 1 : 2;
                snprintf(G.result_str, sizeof(G.result_str),
                         "%s wins by checkmate!", G.turn == BLACK ? "White" : "Black");
            }
        }
    } else {
        if (!has_any_legal_move(G.turn)) {
            G.game_over = 3;
            snprintf(G.result_str, sizeof(G.result_str), "Stalemate! Draw.");
        }
    }

    /* Fifty-move rule */
    if (G.half_move >= 100 && !G.game_over) {
        G.game_over = 3;
        snprintf(G.result_str, sizeof(G.result_str), "Draw by fifty-move rule.");
    }

    pgn_append(m);
}

static void undo_move(void) {
    if (G.history_count == 0) {
        snprintf(status_msg, sizeof(status_msg), "Nothing to undo.");
        return;
    }

    Move *m = &G.history[G.history_count - 1];

    /* Restore turn */
    G.turn     = 1 - G.turn;
    G.castling = m->castling;
    G.ep_file  = m->ep_file;
    G.ep_rank  = (m->ep_file >= 0) ? m->ep_rank : -1;
    G.half_move = m->half_move;
    if (G.turn == BLACK) G.full_move--;

    int fr = m->from_r, fc = m->from_c;
    int tr = m->to_r,   tc = m->to_c;

    /* Move piece back */
    G.board[fr][fc] = G.board[tr][tc];
    if (m->promo) G.board[fr][fc].piece = PAWN;
    G.board[tr][tc].piece = EMPTY;
    G.board[tr][tc].color = 0;

    /* Restore capture */
    if (m->captured_piece) {
        G.board[m->captured_r][m->captured_c].piece = m->captured_piece;
        G.board[m->captured_r][m->captured_c].color = m->captured_color;
    }

    /* Undo castling rook */
    if (m->is_castle == 1) {
        G.board[fr][7] = G.board[fr][5];
        G.board[fr][5].piece = EMPTY;
        G.board[fr][5].color = 0;
    } else if (m->is_castle == -1) {
        G.board[fr][0] = G.board[fr][3];
        G.board[fr][3].piece = EMPTY;
        G.board[fr][3].color = 0;
    }

    G.game_over = 0;
    G.history_count--;
    pgn_append(m);

    selected = 0; sel_r = -1; sel_c = -1; legal_count = 0;
    snprintf(status_msg, sizeof(status_msg), "Move undone. %s to move.",
             G.turn == WHITE ? "White" : "Black");
}

/* ─────────────────────────── UCI Engine ─────────────────────────── */

static void engine_send(const char *cmd) {
    if (!E.running) return;
    write(E.pipe_in[1], cmd, strlen(cmd));
    write(E.pipe_in[1], "\n", 1);
}

static int engine_read_line(char *buf, int maxlen, int timeout_ms) {
    if (!E.running) return 0;
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(E.pipe_out[0], &fds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(E.pipe_out[0]+1, &fds, NULL, NULL, &tv);
    if (r <= 0) return 0;
    int pos = 0;
    while (pos < maxlen-1) {
        char c;
        int n = read(E.pipe_out[0], &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        buf[pos++] = c;
    }
    buf[pos] = 0;
    return pos > 0;
}

static void engine_stop(void) {
    if (!E.running) return;
    engine_send("quit");
    waitpid(E.pid, NULL, WNOHANG);
    close(E.pipe_in[1]);
    close(E.pipe_out[0]);
    E.running = 0;
    E.uci_ready = 0;
}

static int engine_start(const char *path) {
    engine_stop();
    if (pipe(E.pipe_in) || pipe(E.pipe_out)) return 0;
    E.pid = fork();
    if (E.pid < 0) return 0;
    if (E.pid == 0) {
        /* Child */
        dup2(E.pipe_in[0],  STDIN_FILENO);
        dup2(E.pipe_out[1], STDOUT_FILENO);
        dup2(E.pipe_out[1], STDERR_FILENO);
        close(E.pipe_in[1]); close(E.pipe_out[0]);
        execl(path, path, NULL);
        _exit(1);
    }
    close(E.pipe_in[0]);
    close(E.pipe_out[1]);
    E.running = 1;

    /* Set non-blocking */
    int flags = fcntl(E.pipe_out[0], F_GETFL, 0);
    fcntl(E.pipe_out[0], F_SETFL, flags | O_NONBLOCK);

    strncpy(E.path, path, sizeof(E.path)-1);

    /* UCI handshake */
    engine_send("uci");
    char buf[512];
    int tries = 0;
    while (tries++ < 100) {
        if (engine_read_line(buf, sizeof(buf), 50)) {
            if (strncmp(buf, "uciok", 5) == 0) { E.uci_ready = 1; break; }
        }
    }
    if (!E.uci_ready) { engine_stop(); return 0; }
    engine_send("isready");
    tries = 0;
    while (tries++ < 100) {
        if (engine_read_line(buf, sizeof(buf), 50)) {
            if (strncmp(buf, "readyok", 7) == 0) break;
        }
    }
    engine_send("ucinewgame");
    return 1;
}

/* Build position string from history */
static void build_position_cmd(char *out, int maxlen) {
    int pos = 0;
    int n = snprintf(out, maxlen, "position startpos");
    pos += n;
    if (G.history_count > 0) {
        n = snprintf(out+pos, maxlen-pos, " moves");
        pos += n;
        for (int i = 0; i < G.history_count; i++) {
            Move *m = &G.history[i];
            char mv[8];
            snprintf(mv, sizeof(mv), " %c%d%c%d",
                     'a'+m->from_c, 8-m->from_r,
                     'a'+m->to_c,   8-m->to_r);
            n = snprintf(out+pos, maxlen-pos, "%s", mv);
            pos += n;
            if (m->promo) {
                char pc = tolower(piece_char(m->promo));
                out[pos++] = pc;
            }
        }
    }
    out[pos] = 0;
}

static void engine_go(void) {
    if (!E.running || !E.uci_ready) return;
    char pos[4096];
    build_position_cmd(pos, sizeof(pos));
    engine_send(pos);

    char go_cmd[256];
    if (E.use_depth)
        snprintf(go_cmd, sizeof(go_cmd), "go depth %d", E.depth);
    else if (E.use_nodes)
        snprintf(go_cmd, sizeof(go_cmd), "go nodes %lld", E.nodes);
    else if (E.use_time)
        snprintf(go_cmd, sizeof(go_cmd), "go movetime %d", E.movetime_ms);
    else if (E.use_clock)
        snprintf(go_cmd, sizeof(go_cmd), "go wtime %d btime %d winc %d binc %d",
                 E.wtime, E.btime, E.winc, E.binc);
    else
        snprintf(go_cmd, sizeof(go_cmd), "go movetime 1000");

    engine_send(go_cmd);
}

/* Parse UCI move string like "e2e4", "e7e8q" */
static void parse_uci_move(const char *mv, int *fr, int *fc, int *tr, int *tc, int *promo) {
    if (strlen(mv) < 4) return;
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
}

/* Poll engine for best move (non-blocking) */
static int engine_poll_bestmove(void) {
    if (!E.running) return 0;
    char buf[512];
    while (engine_read_line(buf, sizeof(buf), 0)) {
        if (strncmp(buf, "bestmove", 8) == 0) {
            char *mv = buf + 9;
            /* trim spaces */
            while (*mv == ' ') mv++;
            char *sp = strchr(mv, ' ');
            if (sp) *sp = 0;
            strncpy(E.best_move, mv, sizeof(E.best_move)-1);
            return 1;
        }
    }
    return 0;
}

/* ─────────────────────────── Promotion Dialog ─────────────────────────── */

static int promotion_dialog(int color) {
    /* Simple inline prompt */
    printf("\033[s"); /* save cursor */
    printf("\n" ANSI_BOLD ANSI_FG_CYAN
           "Promote to: [Q]ueen  [R]ook  [B]ishop  [N]knight > "
           ANSI_RESET);
    fflush(stdout);

    /* Temporarily restore canonical mode for input */
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    struct termios raw = t;
    raw.c_lflag |= (ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char c = 0;
    scanf(" %c", &c);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    printf("\033[u"); /* restore cursor */

    switch (toupper(c)) {
        case 'R': return ROOK;
        case 'B': return BISHOP;
        case 'N': return KNIGHT;
        default:  return QUEEN;
    }
}

/* ─────────────────────────── Rendering ─────────────────────────── */

static const char *piece_unicode(int piece, int color) {
    static const char *wp[] = {"  ", "♙ ", "♘ ", "♗ ", "♖ ", "♕ ", "♔ "};
    static const char *bp[] = {"  ", "♟ ", "♞ ", "♝ ", "♜ ", "♛ ", "♚ "};
    if (piece < 0 || piece > 6) return "  ";
    return (color == WHITE) ? wp[piece] : bp[piece];
}

static int is_legal_target(int r, int c) {
    for (int i = 0; i < legal_count; i++)
        if (legal_targets[i].r == r && legal_targets[i].c == c) return 1;
    return 0;
}

static void render(void) {
    printf(ANSI_CLEAR);

    /* Last move squares */
    int lm_fr = -1, lm_fc = -1, lm_tr = -1, lm_tc = -1;
    if (G.history_count > 0) {
        Move *lm = &G.history[G.history_count-1];
        lm_fr = lm->from_r; lm_fc = lm->from_c;
        lm_tr = lm->to_r;   lm_tc = lm->to_c;
    }

    /* King in check */
    int ck_r = -1, ck_c = -1;
    if (in_check_board(G.board, G.turn))
        find_king(G.board, G.turn, &ck_r, &ck_c);

    printf(ANSI_BOLD "  ╔════════════════════════════════╗\n" ANSI_RESET);
    for (int r = 0; r < 8; r++) {
        printf(ANSI_BOLD "%d " ANSI_RESET, 8 - r);
        printf(ANSI_BOLD "║" ANSI_RESET);
        for (int c = 0; c < 8; c++) {
            /* Determine background */
            const char *bg;
            int is_light = (r + c) % 2 == 0;

            if (r == cursor_r && c == cursor_c)
                bg = ANSI_BG_CURSOR;
            else if (selected && r == sel_r && c == sel_c)
                bg = ANSI_BG_SELECTED;
            else if (is_legal_target(r, c))
                bg = ANSI_BG_LEGAL;
            else if (r == ck_r && c == ck_c)
                bg = ANSI_BG_CHECK;
            else if ((r == lm_fr && c == lm_fc) || (r == lm_tr && c == lm_tc))
                bg = ANSI_BG_LASTMOVE;
            else
                bg = is_light ? ANSI_BG_LIGHT : ANSI_BG_DARK;

            /* Piece color */
            const char *fg;
            if (G.board[r][c].piece != EMPTY)
                fg = (G.board[r][c].color == WHITE) ? ANSI_FG_WHITE : ANSI_FG_BLACK;
            else
                fg = "";

            printf("%s%s%s%s", bg, ANSI_BOLD, fg,
                   piece_unicode(G.board[r][c].piece, G.board[r][c].color));
            printf(ANSI_RESET);
        }
        printf(ANSI_BOLD "║" ANSI_RESET);

        /* Side panel */
        if (r == 0) {
            printf("  " ANSI_BOLD ANSI_FG_CYAN "♟  TERMINAL CHESS" ANSI_RESET);
        } else if (r == 1) {
            printf("  " ANSI_FG_YELLOW "Engine: " ANSI_RESET);
            if (E.running)
                printf(ANSI_FG_GREEN "%s" ANSI_RESET, E.path);
            else
                printf(ANSI_FG_RED "Not loaded" ANSI_RESET);
        } else if (r == 2) {
            printf("  " ANSI_FG_YELLOW "Playing: " ANSI_RESET);
            if (E.running) {
                if (E.engine_color == WHITE)      printf("White (Engine) vs Black (Human)");
                else if (E.engine_color == BLACK)  printf("White (Human) vs Black (Engine)");
                else                               printf("Human vs Human");
            } else {
                printf("Human vs Human");
            }
        } else if (r == 3) {
            printf("  " ANSI_FG_YELLOW "Turn: " ANSI_RESET ANSI_BOLD);
            printf("%s  Move %d" ANSI_RESET,
                   G.turn == WHITE ? "White" : "Black", G.full_move);
        } else if (r == 4) {
            /* Time control info */
            printf("  " ANSI_FG_YELLOW "Time ctrl: " ANSI_RESET);
            if (E.use_depth)       printf("Depth %d", E.depth);
            else if (E.use_nodes)  printf("Nodes %lld", E.nodes);
            else if (E.use_time)   printf("%d ms/move", E.movetime_ms);
            else if (E.use_clock)  printf("Clock wt=%d bt=%d", E.wtime/1000, E.btime/1000);
            else                   printf("1000 ms/move");
        } else if (r == 5) {
            if (G.game_over) {
                printf("  " ANSI_BOLD ANSI_FG_RED "GAME OVER: %s" ANSI_RESET, G.result_str);
            }
        }
        printf("\n");
    }
    printf(ANSI_BOLD "  ╚════════════════════════════════╝\n" ANSI_RESET);
    printf(ANSI_BOLD "    a b c d e f g h\n\n" ANSI_RESET);

    /* Status bar */
    printf(ANSI_BG_CURSOR ANSI_BOLD " %-72s " ANSI_RESET "\n\n", status_msg);

    /* PGN panel */
    printf(ANSI_BOLD ANSI_FG_CYAN "── Moves (PGN) ─────────────────────────────────────────────\n" ANSI_RESET);
    if (G.pgn[0]) {
        /* Word-wrap at 66 chars */
        int plen = strlen(G.pgn);
        int col = 0;
        printf("  ");
        for (int i = 0; i < plen && i < 1000; i++) {
            putchar(G.pgn[i]);
            col++;
            if (col >= 66 && G.pgn[i] == ' ') { printf("\n  "); col = 0; }
        }
        printf("\n");
    } else {
        printf("  (no moves yet)\n");
    }

    /* Controls */
    printf("\n" ANSI_BOLD ANSI_FG_CYAN "── Controls ────────────────────────────────────────────────\n" ANSI_RESET);
    printf("  " ANSI_FG_YELLOW "Arrows" ANSI_RESET "=Navigate  "
           ANSI_FG_YELLOW "Enter/Space" ANSI_RESET "=Select/Move  "
           ANSI_FG_YELLOW "U" ANSI_RESET "=Undo  "
           ANSI_FG_YELLOW "N" ANSI_RESET "=New  "
           ANSI_FG_YELLOW "E" ANSI_RESET "=Engine\n");
    printf("  " ANSI_FG_YELLOW "T" ANSI_RESET "=TimeCtrl  "
           ANSI_FG_YELLOW "S" ANSI_RESET "=Side(engine)  "
           ANSI_FG_YELLOW "P" ANSI_RESET "=SavePGN  "
           ANSI_FG_YELLOW "Q" ANSI_RESET "=Quit\n");

    fflush(stdout);
}

/* ─────────────────────────── Input Handlers ─────────────────────────── */

static void save_pgn(void) {
    /* Restore terminal temporarily */
    term_restore();
    printf(ANSI_FG_YELLOW "Save PGN to file (Enter filename): " ANSI_RESET);
    fflush(stdout);

    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    struct termios raw = t;
    raw.c_lflag |= (ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char fname[256] = {0};
    fgets(fname, sizeof(fname), stdin);
    fname[strcspn(fname, "\n")] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);

    if (fname[0]) {
        FILE *f = fopen(fname, "w");
        if (f) {
            fprintf(f, "[Event \"Terminal Chess\"]\n");
            fprintf(f, "[Site \"Terminal\"]\n");
            fprintf(f, "[Date \"????.??.??\"]\n");
            fprintf(f, "[Round \"1\"]\n");
            fprintf(f, "[White \"?\"]\n");
            fprintf(f, "[Black \"?\"]\n");
            fprintf(f, "[Result \"*\"]\n\n");
            fprintf(f, "%s\n", G.pgn);
            fclose(f);
            snprintf(status_msg, sizeof(status_msg), "PGN saved to '%s'.", fname);
        } else {
            snprintf(status_msg, sizeof(status_msg), "Error saving to '%s'.", fname);
        }
    }
    term_raw();
}

static void set_engine_path(void) {
    term_restore();
    printf(ANSI_FG_YELLOW "Enter engine path (e.g. /usr/local/bin/stockfish): " ANSI_RESET);
    fflush(stdout);

    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    struct termios raw = t;
    raw.c_lflag |= (ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char path[512] = {0};
    fgets(path, sizeof(path), stdin);
    path[strcspn(path, "\n")] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);

    term_raw();

    if (path[0]) {
        if (engine_start(path)) {
            snprintf(status_msg, sizeof(status_msg),
                     "Engine '%s' loaded! Press S to set which side it plays.", path);
        } else {
            snprintf(status_msg, sizeof(status_msg),
                     "Failed to start engine '%s'. Check the path.", path);
        }
    }
}

static void set_time_controls(void) {
    term_restore();
    printf(ANSI_FG_CYAN "\n── Set Time Controls ──────────────────────────────\n" ANSI_RESET);
    printf("  1) Fixed depth\n");
    printf("  2) Fixed nodes\n");
    printf("  3) Fixed time per move (ms)\n");
    printf("  4) Clock (wtime/btime/winc/binc in ms)\n");
    printf("  5) Default (1000 ms/move)\n");
    printf("  Choice: ");
    fflush(stdout);

    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    struct termios raw = t;
    raw.c_lflag |= (ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    E.use_depth = E.use_nodes = E.use_time = E.use_clock = 0;
    char line[128];
    fgets(line, sizeof(line), stdin);
    int choice = atoi(line);

    switch (choice) {
        case 1:
            printf("  Depth: "); fflush(stdout);
            fgets(line, sizeof(line), stdin);
            E.depth = atoi(line);
            E.use_depth = 1;
            snprintf(status_msg, sizeof(status_msg), "Time control: depth %d", E.depth);
            break;
        case 2:
            printf("  Nodes: "); fflush(stdout);
            fgets(line, sizeof(line), stdin);
            E.nodes = atoll(line);
            E.use_nodes = 1;
            snprintf(status_msg, sizeof(status_msg), "Time control: nodes %lld", E.nodes);
            break;
        case 3:
            printf("  Milliseconds per move: "); fflush(stdout);
            fgets(line, sizeof(line), stdin);
            E.movetime_ms = atoi(line);
            E.use_time = 1;
            snprintf(status_msg, sizeof(status_msg), "Time control: %d ms/move", E.movetime_ms);
            break;
        case 4:
            printf("  wtime (ms): "); fflush(stdout); fgets(line,sizeof(line),stdin); E.wtime = atoi(line);
            printf("  btime (ms): "); fflush(stdout); fgets(line,sizeof(line),stdin); E.btime = atoi(line);
            printf("  winc  (ms): "); fflush(stdout); fgets(line,sizeof(line),stdin); E.winc  = atoi(line);
            printf("  binc  (ms): "); fflush(stdout); fgets(line,sizeof(line),stdin); E.binc  = atoi(line);
            E.use_clock = 1;
            snprintf(status_msg, sizeof(status_msg), "Clock: w=%ds b=%ds inc w=%d b=%d",
                     E.wtime/1000, E.btime/1000, E.winc, E.binc);
            break;
        default:
            snprintf(status_msg, sizeof(status_msg), "Default: 1000 ms/move");
            break;
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    term_raw();
}

static void set_engine_side(void) {
    term_restore();
    printf(ANSI_FG_CYAN "\n── Engine Side ─────────────────────────────────────\n" ANSI_RESET);
    printf("  1) Engine plays White\n");
    printf("  2) Engine plays Black\n");
    printf("  3) Engine off (Human vs Human)\n");
    printf("  Choice: ");
    fflush(stdout);

    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    struct termios raw = t;
    raw.c_lflag |= (ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char line[16];
    fgets(line, sizeof(line), stdin);
    int ch = atoi(line);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    term_raw();

    if      (ch == 1) { E.engine_color = WHITE; snprintf(status_msg,sizeof(status_msg),"Engine plays White."); }
    else if (ch == 2) { E.engine_color = BLACK; snprintf(status_msg,sizeof(status_msg),"Engine plays Black."); }
    else              { E.engine_color = -1;    snprintf(status_msg,sizeof(status_msg),"Human vs Human."); }
}

/* ─────────────────────────── Move Handling ─────────────────────────── */

static int is_promo_move(int fr, int fc, int tr, int tc) {
    return G.board[fr][fc].piece == PAWN && (tr == 0 || tr == 7);
}

static void try_human_move(int fr, int fc, int tr, int tc) {
    if (G.game_over) {
        snprintf(status_msg, sizeof(status_msg), "Game over! Press N for a new game.");
        return;
    }

    /* Check it's the human's turn */
    if (E.running && E.engine_color == G.turn) {
        snprintf(status_msg, sizeof(status_msg), "It's the engine's turn! Please wait.");
        return;
    }

    /* Check piece belongs to current player */
    if (G.board[fr][fc].piece == EMPTY || G.board[fr][fc].color != G.turn) {
        snprintf(status_msg, sizeof(status_msg), "That's not your piece!");
        return;
    }

    /* Check it's a legal target */
    int found = 0;
    for (int i = 0; i < legal_count; i++)
        if (legal_targets[i].r == tr && legal_targets[i].c == tc) { found = 1; break; }

    if (!found) {
        snprintf(status_msg, sizeof(status_msg), "Illegal move! Select a highlighted square.");
        return;
    }

    int promo = 0;
    if (is_promo_move(fr, fc, tr, tc)) {
        promo = promotion_dialog(G.turn);
    }

    /* Actually apply */
    char san_buf[16];
    move_to_san(fr, fc, tr, tc, promo, san_buf);
    apply_move(fr, fc, tr, tc, promo);

    selected = 0; sel_r = -1; sel_c = -1; legal_count = 0;

    if (G.game_over) {
        snprintf(status_msg, sizeof(status_msg), "Game Over: %s", G.result_str);
    } else {
        snprintf(status_msg, sizeof(status_msg),
                 "Played %s. %s to move.",
                 san_buf, G.turn == WHITE ? "White" : "Black");

        /* Trigger engine if applicable */
        if (E.running && E.engine_color == G.turn) {
            engine_go();
            snprintf(status_msg, sizeof(status_msg),
                     "Played %s. Engine thinking...", san_buf);
        }
    }
}

/* ─────────────────────────── Main Loop ─────────────────────────── */

static int engine_thinking = 0;

static void handle_key(int k) {
    if (k == 0) return;

    switch (k) {
        case KEY_UP:    cursor_r = (cursor_r - 1 + 8) % 8; break;
        case KEY_DOWN:  cursor_r = (cursor_r + 1) % 8;     break;
        case KEY_LEFT:  cursor_c = (cursor_c - 1 + 8) % 8; break;
        case KEY_RIGHT: cursor_c = (cursor_c + 1) % 8;     break;

        case KEY_ENTER:
        case ' ':
            if (G.game_over) {
                snprintf(status_msg, sizeof(status_msg),
                         "Game over: %s  Press N for new game.", G.result_str);
                break;
            }
            if (engine_thinking && E.running && E.engine_color == G.turn) {
                snprintf(status_msg, sizeof(status_msg), "Engine is thinking, please wait...");
                break;
            }
            if (!selected) {
                /* Try to select */
                if (G.board[cursor_r][cursor_c].piece == EMPTY) {
                    snprintf(status_msg, sizeof(status_msg), "Empty square. Choose a piece.");
                    break;
                }
                if (G.board[cursor_r][cursor_c].color != G.turn) {
                    snprintf(status_msg, sizeof(status_msg), "That's the opponent's piece!");
                    break;
                }
                if (E.running && E.engine_color == G.turn) {
                    snprintf(status_msg, sizeof(status_msg), "Engine's turn! Please wait.");
                    break;
                }
                sel_r = cursor_r; sel_c = cursor_c;
                legal_count = get_legal_moves(sel_r, sel_c, legal_targets);
                if (legal_count == 0) {
                    snprintf(status_msg, sizeof(status_msg), "This piece has no legal moves.");
                } else {
                    selected = 1;
                    snprintf(status_msg, sizeof(status_msg),
                             "Selected %c%d. %d legal moves. Navigate and press Enter.",
                             'a'+sel_c, 8-sel_r, legal_count);
                }
            } else {
                /* Try to move, or re-select */
                if (cursor_r == sel_r && cursor_c == sel_c) {
                    selected = 0; sel_r = -1; sel_c = -1; legal_count = 0;
                    snprintf(status_msg, sizeof(status_msg), "Deselected.");
                } else if (G.board[cursor_r][cursor_c].piece != EMPTY &&
                           G.board[cursor_r][cursor_c].color == G.turn &&
                           !is_legal_target(cursor_r, cursor_c)) {
                    /* Re-select another piece */
                    sel_r = cursor_r; sel_c = cursor_c;
                    legal_count = get_legal_moves(sel_r, sel_c, legal_targets);
                    selected = (legal_count > 0);
                    snprintf(status_msg, sizeof(status_msg),
                             "Selected %c%d. %d legal moves.",
                             'a'+sel_c, 8-sel_r, legal_count);
                } else {
                    try_human_move(sel_r, sel_c, cursor_r, cursor_c);
                    if (!G.game_over && E.running && E.engine_color == G.turn)
                        engine_thinking = 1;
                }
            }
            break;

        case 'u': case 'U':
            if (engine_thinking) {
                engine_send("stop");
                engine_thinking = 0;
            }
            undo_move();
            /* If engine was playing, undo one more (the engine's move) */
            if (E.running && E.engine_color != -1 && G.history_count > 0)
                undo_move();
            break;

        case 'n': case 'N':
            if (engine_thinking) { engine_send("stop"); engine_thinking = 0; }
            init_board();
            selected = 0; sel_r = -1; sel_c = -1; legal_count = 0;
            if (E.running) {
                engine_send("ucinewgame");
                /* If engine plays white, trigger it */
                if (E.engine_color == WHITE) {
                    engine_go();
                    engine_thinking = 1;
                    snprintf(status_msg, sizeof(status_msg), "New game! Engine thinking (White)...");
                } else {
                    snprintf(status_msg, sizeof(status_msg), "New game! White to move.");
                }
            }
            break;

        case 'e': case 'E':
            if (engine_thinking) { engine_send("stop"); engine_thinking = 0; }
            set_engine_path();
            if (E.running && E.engine_color == WHITE && G.history_count == 0) {
                engine_go(); engine_thinking = 1;
                snprintf(status_msg, sizeof(status_msg), "Engine loaded and thinking (White)...");
            }
            break;

        case 't': case 'T':
            if (engine_thinking) { engine_send("stop"); engine_thinking = 0; }
            set_time_controls();
            break;

        case 's': case 'S':
            if (engine_thinking) { engine_send("stop"); engine_thinking = 0; }
            set_engine_side();
            if (E.running && E.engine_color == G.turn) {
                engine_go(); engine_thinking = 1;
                snprintf(status_msg, sizeof(status_msg), "Engine now plays %s. Thinking...",
                         E.engine_color == WHITE ? "White" : "Black");
            }
            break;

        case 'p': case 'P':
            save_pgn();
            break;

        case 'h': case 'H':
            snprintf(status_msg, sizeof(status_msg),
                     "Arrows=move cursor | Enter=select/move | U=undo | N=new | E=engine | T=time | S=side | P=savePGN | Q=quit");
            break;

        case 'q': case 'Q': case KEY_ESC:
            engine_stop();
            term_restore();
            printf("Thanks for playing Terminal Chess!\n");
            exit(0);
    }
}

/* ─────────────────────────── Main ─────────────────────────── */

int main(void) {
    /* Check terminal supports UTF-8 */
    const char *lang = getenv("LANG");
    if (!lang) lang = "";

    init_board();
    memset(&E, 0, sizeof(E));
    E.engine_color = -1;  /* no engine by default */
    E.movetime_ms  = 1000;

    term_raw();
    atexit(term_restore);

    /* Initial render */
    render();

    while (1) {
        /* Poll engine for best move */
        if (engine_thinking && E.running) {
            if (engine_poll_bestmove()) {
                engine_thinking = 0;
                if (!G.game_over && strlen(E.best_move) >= 4) {
                    int fr, fc, tr, tc, promo;
                    parse_uci_move(E.best_move, &fr, &fc, &tr, &tc, &promo);
                    char san_buf[16];
                    move_to_san(fr, fc, tr, tc, promo, san_buf);
                    apply_move(fr, fc, tr, tc, promo);
                    if (G.game_over) {
                        snprintf(status_msg, sizeof(status_msg),
                                 "Engine played %s. Game Over: %s",
                                 san_buf, G.result_str);
                    } else {
                        snprintf(status_msg, sizeof(status_msg),
                                 "Engine played %s. Your turn (%s).",
                                 san_buf, G.turn == WHITE ? "White" : "Black");
                    }
                    render();
                }
            }
        }

        /* Read input */
        int k = read_key();
        if (k) {
            handle_key(k);
            render();
        }

        /* Small sleep to avoid busy-waiting */
        usleep(10000); /* 10ms */
    }

    return 0;
}
