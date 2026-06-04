/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 * 
 * Compile: gcc -o chess_gui chess_gui.c
 * Run:     ./chess_gui
 * 
 * Controls:
 *   Arrow Keys / HJKL - Move cursor
 *   Enter / Space      - Select/Move piece
 *   U                  - Undo last move
 *   N                  - New game
 *   E                  - Set engine path
 *   T                  - Set time controls
 *   Q                  - Quit
 *   F                  - Flip board
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

/* ─── Constants ─────────────────────────────────────────────────────────────*/
#define MAX_MOVES       1024
#define MAX_PGN_LEN     16384
#define MAX_PATH        512
#define MAX_LINE        4096
#define ENGINE_TIMEOUT  10
#define MAX_LEGAL_MOVES 256

/* Piece definitions */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6
#define WHITE  8
#define BLACK  16

/* Colors (ANSI) */
#define COL_RESET     "\033[0m"
#define COL_BOLD      "\033[1m"

/* Background colors */
#define BG_LIGHT      "\033[48;2;240;217;181m"   /* light square */
#define BG_DARK       "\033[48;2;181;136;99m"    /* dark square  */
#define BG_SELECT     "\033[48;2;100;180;100m"   /* selected     */
#define BG_CURSOR     "\033[48;2;80;130;200m"    /* cursor       */
#define BG_LEGAL      "\033[48;2;180;200;100m"   /* legal move   */
#define BG_LAST_FROM  "\033[48;2;200;190;80m"    /* last move from */
#define BG_LAST_TO    "\033[48;2;220;210;60m"    /* last move to   */
#define BG_CHECK      "\033[48;2;220;80;80m"     /* king in check  */

/* Foreground colors */
#define FG_WHITE_PC   "\033[38;2;255;255;255m"
#define FG_BLACK_PC   "\033[38;2;30;30;30m"
#define FG_UI         "\033[38;2;220;220;220m"
#define FG_GREEN      "\033[38;2;80;200;80m"
#define FG_YELLOW     "\033[38;2;220;200;60m"
#define FG_RED        "\033[38;2;220;80;80m"
#define FG_CYAN       "\033[38;2;80;200;220m"
#define FG_GRAY       "\033[38;2;160;160;160m"
#define FG_TITLE      "\033[38;2;255;200;50m"

/* ─── Types ──────────────────────────────────────────────────────────────────*/
typedef unsigned long long U64;

typedef struct {
    char from[3];   /* e.g. "e2" */
    char to[3];     /* e.g. "e4" */
    char promo;     /* promotion piece: q/r/b/n or 0 */
    char uci[6];    /* full UCI string */
    /* For PGN */
    int  piece;
    int  capture;
    int  check;     /* 1=check, 2=checkmate */
    char pgn[16];   /* SAN notation */
} Move;

typedef struct {
    int  board[64];       /* piece | color */
    int  turn;            /* WHITE or BLACK */
    int  castling;        /* bits: 0=WK,1=WQ,2=BK,3=BQ */
    int  ep_square;       /* en passant target, -1 if none */
    int  halfmove;        /* fifty-move counter */
    int  fullmove;
    Move history[MAX_MOVES];
    int  history_count;
    /* Saved state for undo */
    int  prev_board[64];
    int  prev_castling;
    int  prev_ep;
    int  prev_halfmove;
} GameState;

typedef struct {
    /* Search limits */
    int  depth;    /* 0 = not used */
    int  nodes;    /* 0 = not used */
    int  movetime; /* ms, 0 = not used */
    int  wtime;    /* white time ms */
    int  btime;    /* black time ms */
    int  winc;
    int  binc;
    int  mode;     /* 0=depth,1=nodes,2=movetime,3=timed */
} TimeControl;

typedef struct {
    char path[MAX_PATH];
    int  pid;
    int  in_fd;   /* write to engine */
    int  out_fd;  /* read from engine */
    int  active;
    int  thinking;
    char name[256];
    int  color;   /* WHITE or BLACK */
} Engine;

/* ─── Globals ────────────────────────────────────────────────────────────────*/
static GameState  g_game;
static Engine     g_engine;
static TimeControl g_tc;
static struct termios g_orig_termios;

static int  g_cursor_sq   = 36;  /* e4 */
static int  g_selected_sq = -1;
static int  g_flip        = 0;
static int  g_running     = 1;
static int  g_human_color = WHITE;
static int  g_engine_color = BLACK;

static int  g_legal_moves[MAX_LEGAL_MOVES];
static int  g_legal_count = 0;

static char g_pgn[MAX_PGN_LEN];
static char g_status[256];
static char g_engine_line[512];

static int  g_last_from = -1;
static int  g_last_to   = -1;

static int  g_game_over  = 0;
static char g_result[16] = "*";

/* PGN history for undo (full board snapshots) */
typedef struct {
    int  board[64];
    int  turn;
    int  castling;
    int  ep_square;
    int  halfmove;
    int  fullmove;
    int  last_from;
    int  last_to;
    Move move;
} Snapshot;

static Snapshot g_snapshots[MAX_MOVES];
static int      g_snap_count = 0;

/* ─── Terminal Handling ──────────────────────────────────────────────────────*/
static void term_raw(void)
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
}

static void term_clear(void)  { printf("\033[2J\033[H"); }
static void term_hide(void)   { printf("\033[?25l"); }
static void term_show(void)   { printf("\033[?25h"); }
static void goto_xy(int x, int y) { printf("\033[%d;%dH", y, x); }

/* ─── Piece Utilities ────────────────────────────────────────────────────────*/
static int piece_type(int p)  { return p & 7; }
static int piece_color(int p) { return p & (WHITE | BLACK); }

static const char *piece_unicode(int p)
{
    int c = piece_color(p), t = piece_type(p);
    if (c == WHITE) {
        switch(t) {
            case PAWN:   return "♙";
            case KNIGHT: return "♘";
            case BISHOP: return "♗";
            case ROOK:   return "♖";
            case QUEEN:  return "♕";
            case KING:   return "♔";
        }
    } else {
        switch(t) {
            case PAWN:   return "♟";
            case KNIGHT: return "♞";
            case BISHOP: return "♝";
            case ROOK:   return "♜";
            case QUEEN:  return "♛";
            case KING:   return "♚";
        }
    }
    return "  ";
}

static char piece_char(int p)
{
    switch(piece_type(p)) {
        case PAWN:   return 'P';
        case KNIGHT: return 'N';
        case BISHOP: return 'B';
        case ROOK:   return 'R';
        case QUEEN:  return 'Q';
        case KING:   return 'K';
    }
    return '?';
}

static int sq_from_str(const char *s)
{
    if (!s || s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8')
        return -1;
    return (s[1] - '1') * 8 + (s[0] - 'a');
}

static void sq_to_str(int sq, char *buf)
{
    buf[0] = 'a' + (sq % 8);
    buf[1] = '1' + (sq / 8);
    buf[2] = '\0';
}

/* ─── Board Initialization ───────────────────────────────────────────────────*/
static void init_board(void)
{
    static const int back_rank[] = {
        ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK
    };
    memset(g_game.board, 0, sizeof(g_game.board));
    for (int f = 0; f < 8; f++) {
        g_game.board[f]      = WHITE | back_rank[f];
        g_game.board[8 + f]  = WHITE | PAWN;
        g_game.board[48 + f] = BLACK | PAWN;
        g_game.board[56 + f] = BLACK | back_rank[f];
    }
    g_game.turn      = WHITE;
    g_game.castling  = 0xF;
    g_game.ep_square = -1;
    g_game.halfmove  = 0;
    g_game.fullmove  = 1;
    g_game.history_count = 0;
    g_last_from = -1;
    g_last_to   = -1;
    g_snap_count = 0;
    g_game_over  = 0;
    strcpy(g_result, "*");
    strcpy(g_status, "White to move");
    g_pgn[0] = '\0';
    g_selected_sq = -1;
    g_legal_count = 0;
    g_cursor_sq = 4; /* e1 */
}

/* ─── Move Generation ────────────────────────────────────────────────────────*/
static int on_board(int sq) { return sq >= 0 && sq < 64; }

static const int knight_offsets[] = { 17, 15, 10, 6, -6, -10, -15, -17 };
static const int king_offsets[]   = { 9, 8, 7, 1, -1, -7, -8, -9 };

/* Check if moving from sq1 to sq2 wraps (file distance > 2 = wrapped) */
static int no_wrap(int from, int to)
{
    if (!on_board(from) || !on_board(to)) return 0;
    int fc = from % 8, tc2 = to % 8;
    return abs(fc - tc2) <= 2;
}

/* Generate pseudo-legal moves for a piece */
static int gen_piece_moves(int sq, int *out, int check_wrap)
{
    int p = g_game.board[sq];
    int color = piece_color(p), type = piece_type(p);
    int count = 0;
    int opp = (color == WHITE) ? BLACK : WHITE;

    if (type == PAWN) {
        int dir = (color == WHITE) ? 1 : -1;
        int start_rank = (color == WHITE) ? 1 : 6;
        int promo_rank = (color == WHITE) ? 7 : 0;
        int fwd = sq + dir * 8;
        (void)promo_rank;

        /* Forward one */
        if (on_board(fwd) && g_game.board[fwd] == EMPTY) {
            out[count++] = fwd;
            /* Forward two */
            int fwd2 = sq + dir * 16;
            if ((sq / 8) == start_rank && on_board(fwd2) &&
                g_game.board[fwd2] == EMPTY)
                out[count++] = fwd2;
        }
        /* Captures */
        int caps[] = { sq + dir * 8 - 1, sq + dir * 8 + 1 };
        for (int i = 0; i < 2; i++) {
            int csq = caps[i];
            if (!on_board(csq)) continue;
            if (abs((csq % 8) - (sq % 8)) != 1) continue;
            if ((piece_color(g_game.board[csq]) == opp) ||
                csq == g_game.ep_square)
                out[count++] = csq;
        }
    } else if (type == KNIGHT) {
        for (int i = 0; i < 8; i++) {
            int dest = sq + knight_offsets[i];
            if (!on_board(dest)) continue;
            if (check_wrap && !no_wrap(sq, dest)) continue;
            int dp = g_game.board[dest];
            if (piece_color(dp) != color)
                out[count++] = dest;
        }
    } else if (type == KING) {
        for (int i = 0; i < 8; i++) {
            int dest = sq + king_offsets[i];
            if (!on_board(dest)) continue;
            if (check_wrap && !no_wrap(sq, dest)) continue;
            int dp = g_game.board[dest];
            if (piece_color(dp) != color)
                out[count++] = dest;
        }
        /* Castling */
        if (color == WHITE && sq == 4) {
            /* Kingside */
            if ((g_game.castling & 1) &&
                g_game.board[5] == EMPTY && g_game.board[6] == EMPTY)
                out[count++] = 6;
            /* Queenside */
            if ((g_game.castling & 2) &&
                g_game.board[3] == EMPTY && g_game.board[2] == EMPTY &&
                g_game.board[1] == EMPTY)
                out[count++] = 2;
        }
        if (color == BLACK && sq == 60) {
            if ((g_game.castling & 4) &&
                g_game.board[61] == EMPTY && g_game.board[62] == EMPTY)
                out[count++] = 62;
            if ((g_game.castling & 8) &&
                g_game.board[59] == EMPTY && g_game.board[58] == EMPTY &&
                g_game.board[57] == EMPTY)
                out[count++] = 58;
        }
    } else {
        /* Sliding pieces */
        int dirs[8], ndirs = 0;
        if (type == ROOK || type == QUEEN) {
            dirs[ndirs++] = 8; dirs[ndirs++] = -8;
            dirs[ndirs++] = 1; dirs[ndirs++] = -1;
        }
        if (type == BISHOP || type == QUEEN) {
            dirs[ndirs++] = 9; dirs[ndirs++] = -9;
            dirs[ndirs++] = 7; dirs[ndirs++] = -7;
        }
        for (int d = 0; d < ndirs; d++) {
            int cur = sq;
            while (1) {
                int prev = cur;
                cur += dirs[d];
                if (!on_board(cur)) break;
                /* Prevent rank wrapping for horizontal moves */
                if (dirs[d] == 1 || dirs[d] == -1) {
                    if ((cur / 8) != (prev / 8)) break;
                }
                /* Prevent diagonal wrapping */
                if (dirs[d] == 9 || dirs[d] == -9 ||
                    dirs[d] == 7 || dirs[d] == -7) {
                    if (abs((cur % 8) - (prev % 8)) != 1) break;
                }
                int dp = g_game.board[cur];
                if (piece_color(dp) == color) break;
                out[count++] = cur;
                if (dp != EMPTY) break; /* blocked by opponent */
            }
        }
    }
    return count;
}

/* Find king square */
static int find_king(int color)
{
    for (int i = 0; i < 64; i++)
        if (g_game.board[i] == (color | KING))
            return i;
    return -1;
}

/* Check if a square is attacked by 'color' */
static int sq_attacked(int sq, int by_color)
{
    /* Save and check */
    for (int i = 0; i < 64; i++) {
        int p = g_game.board[i];
        if (piece_color(p) != by_color || p == EMPTY) continue;
        int moves[64], cnt = gen_piece_moves(i, moves, 1);
        for (int j = 0; j < cnt; j++)
            if (moves[j] == sq) return 1;
    }
    return 0;
}

/* Make move on board (no legality check) */
typedef struct {
    int from, to;
    int moved, captured;
    int castling, ep;
    int halfmove;
    char promo;
    int ep_captured_sq; /* for en passant */
    int rook_from, rook_to; /* for castling */
} MoveUndo;

static MoveUndo do_move(int from, int to, char promo)
{
    MoveUndo u;
    u.from = from; u.to = to;
    u.moved = g_game.board[from];
    u.captured = g_game.board[to];
    u.castling = g_game.castling;
    u.ep = g_game.ep_square;
    u.halfmove = g_game.halfmove;
    u.promo = promo;
    u.ep_captured_sq = -1;
    u.rook_from = -1; u.rook_to = -1;

    int type = piece_type(u.moved);
    int color = piece_color(u.moved);

    /* En passant capture */
    if (type == PAWN && to == g_game.ep_square && g_game.ep_square != -1) {
        int cap_sq = to + (color == WHITE ? -8 : 8);
        u.ep_captured_sq = cap_sq;
        u.captured = g_game.board[cap_sq];
        g_game.board[cap_sq] = EMPTY;
    }

    /* Move piece */
    g_game.board[to]   = u.moved;
    g_game.board[from] = EMPTY;

    /* Promotion */
    if (type == PAWN && (to / 8 == 7 || to / 8 == 0)) {
        int pp = QUEEN;
        if (promo == 'r') pp = ROOK;
        else if (promo == 'b') pp = BISHOP;
        else if (promo == 'n') pp = KNIGHT;
        g_game.board[to] = color | pp;
    }

    /* Castling move */
    if (type == KING) {
        if (color == WHITE) {
            g_game.castling &= ~3;
            if (from == 4 && to == 6) {
                g_game.board[5] = WHITE | ROOK;
                g_game.board[7] = EMPTY;
                u.rook_from = 7; u.rook_to = 5;
            }
            if (from == 4 && to == 2) {
                g_game.board[3] = WHITE | ROOK;
                g_game.board[0] = EMPTY;
                u.rook_from = 0; u.rook_to = 3;
            }
        } else {
            g_game.castling &= ~12;
            if (from == 60 && to == 62) {
                g_game.board[61] = BLACK | ROOK;
                g_game.board[63] = EMPTY;
                u.rook_from = 63; u.rook_to = 61;
            }
            if (from == 60 && to == 58) {
                g_game.board[59] = BLACK | ROOK;
                g_game.board[56] = EMPTY;
                u.rook_from = 56; u.rook_to = 59;
            }
        }
    }

    /* Rook moves affect castling */
    if (type == ROOK) {
        if (from == 0)  g_game.castling &= ~2;
        if (from == 7)  g_game.castling &= ~1;
        if (from == 56) g_game.castling &= ~8;
        if (from == 63) g_game.castling &= ~4;
    }
    /* Captures of rooks */
    if (to == 0)  g_game.castling &= ~2;
    if (to == 7)  g_game.castling &= ~1;
    if (to == 56) g_game.castling &= ~8;
    if (to == 63) g_game.castling &= ~4;

    /* En passant square */
    g_game.ep_square = -1;
    if (type == PAWN && abs(to - from) == 16)
        g_game.ep_square = (from + to) / 2;

    /* Half-move clock */
    if (type == PAWN || u.captured != EMPTY)
        g_game.halfmove = 0;
    else
        g_game.halfmove++;

    if (color == BLACK) g_game.fullmove++;
    g_game.turn = (color == WHITE) ? BLACK : WHITE;

    return u;
}

static void undo_move_internal(MoveUndo *u)
{
    g_game.board[u->from] = u->moved;
    g_game.board[u->to]   = u->captured;

    /* En passant */
    if (u->ep_captured_sq != -1) {
        int opp_pawn = (piece_color(u->moved) == WHITE) ?
                       (BLACK | PAWN) : (WHITE | PAWN);
        g_game.board[u->ep_captured_sq] = opp_pawn;
        g_game.board[u->to] = EMPTY;
    }

    /* Castling rook */
    if (u->rook_from != -1) {
        g_game.board[u->rook_from] = g_game.board[u->rook_to];
        g_game.board[u->rook_to]   = EMPTY;
    }

    g_game.castling  = u->castling;
    g_game.ep_square = u->ep;
    g_game.halfmove  = u->halfmove;

    int color = piece_color(u->moved);
    if (color == BLACK) g_game.fullmove--;
    g_game.turn = color;
}

/* Check if move leaves own king in check */
static int move_legal(int from, int to, char promo)
{
    MoveUndo u = do_move(from, to, promo);
    int color = piece_color(u.moved);
    int ksq   = find_king(color);
    int opp   = (color == WHITE) ? BLACK : WHITE;
    int in_check = (ksq >= 0) ? sq_attacked(ksq, opp) : 0;
    undo_move_internal(&u);
    return !in_check;
}

/* Castling legality (king not in check through squares) */
static int castling_legal(int from, int to)
{
    int color = piece_color(g_game.board[from]);
    int opp   = (color == WHITE) ? BLACK : WHITE;
    int ksq   = find_king(color);

    /* King must not be in check */
    if (sq_attacked(ksq, opp)) return 0;

    /* King path must not be attacked */
    int mid = (from + to) / 2;
    MoveUndo u1 = do_move(from, mid, 0);
    int attacked = sq_attacked(mid, piece_color(u1.moved) == WHITE ? BLACK : WHITE);
    undo_move_internal(&u1);
    if (attacked) return 0;

    return move_legal(from, to, 0);
}

/* Generate all legal moves from 'sq' */
static int gen_legal_moves(int sq, int *out)
{
    int p = g_game.board[sq];
    if (p == EMPTY || piece_color(p) != g_game.turn) return 0;

    int pseudo[64], cnt = gen_piece_moves(sq, pseudo, 1);
    int count = 0;

    for (int i = 0; i < cnt; i++) {
        int to = pseudo[i];
        int type = piece_type(p);

        /* Castling special check */
        if (type == KING && abs(to - sq) == 2) {
            if (castling_legal(sq, to))
                out[count++] = to;
        } else {
            if (move_legal(sq, to, 'q'))
                out[count++] = to;
        }
    }
    return count;
}

/* Check if current player is in check */
static int in_check(int color)
{
    int ksq = find_king(color);
    int opp = (color == WHITE) ? BLACK : WHITE;
    return ksq >= 0 && sq_attacked(ksq, opp);
}

/* Check if current player has any legal moves */
static int has_legal_moves(int color)
{
    int saved_turn = g_game.turn;
    g_game.turn = color;
    for (int sq = 0; sq < 64; sq++) {
        int p = g_game.board[sq];
        if (piece_color(p) != color) continue;
        int moves[64], cnt = gen_legal_moves(sq, moves);
        if (cnt > 0) { g_game.turn = saved_turn; return 1; }
    }
    g_game.turn = saved_turn;
    return 0;
}

/* ─── PGN / SAN Generation ───────────────────────────────────────────────────*/
static void gen_san(int from, int to, char promo, int color, char *out)
{
    int p   = g_game.board[from];
    int cap = g_game.board[to];
    int type = piece_type(p);
    char buf[32];
    int pos = 0;

    /* Castling */
    if (type == KING) {
        if ((color == WHITE && from == 4 && to == 6) ||
            (color == BLACK && from == 60 && to == 62)) {
            strcpy(out, "O-O");
            return;
        }
        if ((color == WHITE && from == 4 && to == 2) ||
            (color == BLACK && from == 60 && to == 58)) {
            strcpy(out, "O-O-O");
            return;
        }
    }

    /* Piece letter */
    if (type != PAWN) buf[pos++] = piece_char(p);

    /* Disambiguation */
    if (type != PAWN) {
        int ambig_file = 0, ambig_rank = 0, ambig = 0;
        for (int sq = 0; sq < 64; sq++) {
            if (sq == from) continue;
            if (g_game.board[sq] != p) continue;
            /* Can this piece also move to 'to'? */
            int moves[64], cnt = gen_legal_moves(sq, moves);
            for (int i = 0; i < cnt; i++) {
                if (moves[i] == to) {
                    ambig++;
                    if ((sq % 8) == (from % 8)) ambig_rank = 1;
                    else ambig_file = 1;
                }
            }
        }
        if (ambig > 0) {
            if (!ambig_file) buf[pos++] = 'a' + (from % 8);
            else if (!ambig_rank) buf[pos++] = '1' + (from / 8);
            else {
                buf[pos++] = 'a' + (from % 8);
                buf[pos++] = '1' + (from / 8);
            }
        }
    } else if (cap != EMPTY || to == g_game.ep_square) {
        buf[pos++] = 'a' + (from % 8);
    }

    /* Capture */
    if (cap != EMPTY || to == g_game.ep_square) buf[pos++] = 'x';

    /* Destination */
    buf[pos++] = 'a' + (to % 8);
    buf[pos++] = '1' + (to / 8);

    /* Promotion */
    if (type == PAWN && (to / 8 == 7 || to / 8 == 0)) {
        buf[pos++] = '=';
        if (promo == 'r') buf[pos++] = 'R';
        else if (promo == 'b') buf[pos++] = 'B';
        else if (promo == 'n') buf[pos++] = 'N';
        else buf[pos++] = 'Q';
    }

    buf[pos] = '\0';

    /* Make the move to check for check/mate */
    MoveUndo u = do_move(from, to, promo ? promo : 'q');
    int opp_in_check = in_check(g_game.turn);
    int opp_has_moves = has_legal_moves(g_game.turn);
    undo_move_internal(&u);

    if (opp_in_check) {
        if (!opp_has_moves) buf[pos++] = '#';
        else                buf[pos++] = '+';
        buf[pos] = '\0';
    }

    strcpy(out, buf);
}

static void update_pgn(void)
{
    g_pgn[0] = '\0';
    char tmp[64];
    for (int i = 0; i < g_game.history_count; i++) {
        Move *m = &g_game.history[i];
        if (m->piece == WHITE || i == 0) {
            /* Move number */
            int fmv = (i / 2) + 1;
            if (i % 2 == 0) {
                snprintf(tmp, sizeof(tmp), "%d. ", fmv);
                strcat(g_pgn, tmp);
            }
        }
        strcat(g_pgn, m->pgn);
        strcat(g_pgn, " ");
        /* Wrap lines */
        int len = strlen(g_pgn);
        if (len > 2 && g_pgn[len-1] == ' ') {
            /* Count chars on current line */
        }
    }
    if (strcmp(g_result, "*") != 0) {
        strcat(g_pgn, g_result);
    }
}

/* ─── Engine Communication ───────────────────────────────────────────────────*/
static int engine_write(const char *cmd)
{
    if (!g_engine.active) return -1;
    size_t len = strlen(cmd);
    ssize_t r = write(g_engine.in_fd, cmd, len);
    return (r == (ssize_t)len) ? 0 : -1;
}

static int engine_read_line(char *buf, int maxlen, int timeout_ms)
{
    if (!g_engine.active) return -1;
    fd_set rfds;
    struct timeval tv;
    int pos = 0;
    long long deadline_us = timeout_ms * 1000LL;
    
    while (pos < maxlen - 1) {
        FD_ZERO(&rfds);
        FD_SET(g_engine.out_fd, &rfds);
        tv.tv_sec  = deadline_us / 1000000;
        tv.tv_usec = deadline_us % 1000000;
        
        int ret = select(g_engine.out_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) break;
        
        char c;
        ssize_t n = read(g_engine.out_fd, &c, 1);
        if (n <= 0) break;
        if (c == '\n') { buf[pos] = '\0'; return pos; }
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static int engine_start(const char *path)
{
    if (g_engine.active) {
        engine_write("quit\n");
        close(g_engine.in_fd);
        close(g_engine.out_fd);
        waitpid(g_engine.pid, NULL, WNOHANG);
        g_engine.active = 0;
    }

    int to_engine[2], from_engine[2];
    if (pipe(to_engine) || pipe(from_engine)) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child: engine process */
        dup2(to_engine[0],   STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        /* Close stderr to avoid noise */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(to_engine[0]); close(to_engine[1]);
        close(from_engine[0]); close(from_engine[1]);
        execlp(path, path, NULL);
        exit(1);
    }

    close(to_engine[0]);
    close(from_engine[1]);

    g_engine.pid    = pid;
    g_engine.in_fd  = to_engine[1];
    g_engine.out_fd = from_engine[0];
    g_engine.active = 1;
    g_engine.thinking = 0;
    strncpy(g_engine.path, path, MAX_PATH - 1);

    /* Make out_fd non-blocking */
    int flags = fcntl(g_engine.out_fd, F_GETFL, 0);
    fcntl(g_engine.out_fd, F_SETFL, flags | O_NONBLOCK);

    /* UCI handshake */
    engine_write("uci\n");
    char line[MAX_LINE];
    g_engine.name[0] = '\0';
    for (int i = 0; i < 50; i++) {
        int n = engine_read_line(line, MAX_LINE, 200);
        if (n > 0) {
            if (strncmp(line, "id name ", 8) == 0)
                strncpy(g_engine.name, line + 8, 255);
            if (strcmp(line, "uciok") == 0) break;
        }
    }
    engine_write("isready\n");
    for (int i = 0; i < 50; i++) {
        int n = engine_read_line(line, MAX_LINE, 200);
        if (n > 0 && strcmp(line, "readyok") == 0) break;
    }
    return 0;
}

/* Build UCI position string */
static void build_position_cmd(char *buf, int maxlen)
{
    snprintf(buf, maxlen, "position startpos moves");
    for (int i = 0; i < g_game.history_count; i++) {
        strcat(buf, " ");
        strcat(buf, g_game.history[i].uci);
    }
    strcat(buf, "\n");
}

/* Build go command from time controls */
static void build_go_cmd(char *buf, int maxlen)
{
    switch (g_tc.mode) {
        case 0: /* depth */
            snprintf(buf, maxlen, "go depth %d\n",
                     g_tc.depth > 0 ? g_tc.depth : 6);
            break;
        case 1: /* nodes */
            snprintf(buf, maxlen, "go nodes %d\n",
                     g_tc.nodes > 0 ? g_tc.nodes : 100000);
            break;
        case 2: /* movetime */
            snprintf(buf, maxlen, "go movetime %d\n",
                     g_tc.movetime > 0 ? g_tc.movetime : 1000);
            break;
        case 3: /* timed game */
            snprintf(buf, maxlen,
                     "go wtime %d btime %d winc %d binc %d\n",
                     g_tc.wtime, g_tc.btime, g_tc.winc, g_tc.binc);
            break;
        default:
            snprintf(buf, maxlen, "go depth 6\n");
    }
}

static void engine_go(void)
{
    if (!g_engine.active) return;
    char pos[MAX_LINE], go[256];
    build_position_cmd(pos, MAX_LINE);
    build_go_cmd(go, 256);
    engine_write(pos);
    engine_write(go);
    g_engine.thinking = 1;
    g_engine_line[0] = '\0';
}

static void engine_stop(void)
{
    if (g_engine.active && g_engine.thinking)
        engine_write("stop\n");
}

/* Try to read engine output (non-blocking) */
/* Returns 1 if bestmove received */
static int engine_poll(char *best_move)
{
    if (!g_engine.active || !g_engine.thinking) return 0;
    
    char line[MAX_LINE];
    int n = engine_read_line(line, MAX_LINE, 0);
    if (n > 0) {
        if (strncmp(line, "info", 4) == 0) {
            /* Extract depth/score info for display */
            strncpy(g_engine_line, line, 511);
            return 0;
        }
        if (strncmp(line, "bestmove", 8) == 0) {
            /* Extract move */
            char *sp = line + 9;
            int i = 0;
            while (sp[i] && sp[i] != ' ' && i < 5) {
                best_move[i] = sp[i]; i++;
            }
            best_move[i] = '\0';
            g_engine.thinking = 0;
            return 1;
        }
    }
    return 0;
}

/* ─── Apply Move ─────────────────────────────────────────────────────────────*/
static int apply_move(int from, int to, char promo)
{
    if (!move_legal(from, to, promo ? promo : 'q')) return 0;

    /* Save snapshot for undo */
    if (g_snap_count < MAX_MOVES) {
        Snapshot *s = &g_snapshots[g_snap_count++];
        memcpy(s->board,    g_game.board,   sizeof(g_game.board));
        s->turn      = g_game.turn;
        s->castling  = g_game.castling;
        s->ep_square = g_game.ep_square;
        s->halfmove  = g_game.halfmove;
        s->fullmove  = g_game.fullmove;
        s->last_from = g_last_from;
        s->last_to   = g_last_to;
    }

    /* Build move record */
    Move mv;
    memset(&mv, 0, sizeof(mv));
    sq_to_str(from, mv.from);
    sq_to_str(to,   mv.to);
    mv.promo = promo;
    mv.piece = g_game.turn;
    mv.capture = (g_game.board[to] != EMPTY);
    snprintf(mv.uci, sizeof(mv.uci), "%s%s%c",
             mv.from, mv.to, promo ? promo : '\0');
    /* Trim null terminator from uci if no promo */
    if (!promo) mv.uci[4] = '\0';

    /* Generate SAN before making move */
    gen_san(from, to, promo ? promo : 'q', g_game.turn, mv.pgn);

    /* Make move */
    do_move(from, to, promo ? promo : 'q');

    /* Store in history */
    if (g_game.history_count < MAX_MOVES)
        g_game.history[g_game.history_count++] = mv;

    g_last_from = from;
    g_last_to   = to;

    /* Check for game over */
    int cur_turn = g_game.turn;
    int chk = in_check(cur_turn);
    int moves = has_legal_moves(cur_turn);

    if (!moves) {
        if (chk) {
            g_game_over = 1;
            if (cur_turn == WHITE) {
                strcpy(g_status, "Checkmate! Black wins");
                strcpy(g_result, "0-1");
            } else {
                strcpy(g_status, "Checkmate! White wins");
                strcpy(g_result, "1-0");
            }
        } else {
            g_game_over = 1;
            strcpy(g_status, "Stalemate! Draw");
            strcpy(g_result, "1/2-1/2");
        }
    } else if (g_game.halfmove >= 100) {
        g_game_over = 1;
        strcpy(g_status, "Draw by 50-move rule");
        strcpy(g_result, "1/2-1/2");
    } else {
        if (chk) {
            snprintf(g_status, sizeof(g_status), "%s is in check!",
                     cur_turn == WHITE ? "White" : "Black");
        } else {
            snprintf(g_status, sizeof(g_status), "%s to move",
                     cur_turn == WHITE ? "White" : "Black");
        }
    }

    update_pgn();
    return 1;
}

static int apply_uci_move(const char *uci)
{
    if (strlen(uci) < 4) return 0;
    int from = sq_from_str(uci);
    int to   = sq_from_str(uci + 2);
    char promo = (strlen(uci) >= 5) ? uci[4] : 0;
    return apply_move(from, to, promo);
}

/* ─── Undo ───────────────────────────────────────────────────────────────────*/
static void undo_move(void)
{
    if (g_snap_count == 0) return;
    if (g_engine.thinking) engine_stop();

    /* How many to undo: if engine plays, undo 2 */
    int undo_count = (g_engine.active) ? 2 : 1;
    if (undo_count > g_snap_count) undo_count = g_snap_count;

    g_snap_count -= undo_count;
    Snapshot *s = &g_snapshots[g_snap_count];

    memcpy(g_game.board,   s->board,   sizeof(g_game.board));
    g_game.turn      = s->turn;
    g_game.castling  = s->castling;
    g_game.ep_square = s->ep_square;
    g_game.halfmove  = s->halfmove;
    g_game.fullmove  = s->fullmove;
    g_last_from = s->last_from;
    g_last_to   = s->last_to;

    g_game.history_count -= undo_count;
    if (g_game.history_count < 0) g_game.history_count = 0;

    g_game_over = 0;
    strcpy(g_result, "*");
    snprintf(g_status, sizeof(g_status), "%s to move (after undo)",
             g_game.turn == WHITE ? "White" : "Black");

    g_selected_sq = -1;
    g_legal_count = 0;
    update_pgn();
}

/* ─── Drawing ────────────────────────────────────────────────────────────────*/
#define BOARD_X  3
#define BOARD_Y  2
#define CELL_W   4  /* chars per cell */
#define CELL_H   2  /* lines per cell */

static int display_sq(int sq)
{
    /* Convert board square to display square based on flip */
    if (!g_flip) return sq;
    int file = sq % 8, rank = sq / 8;
    return (7 - rank) * 8 + (7 - file);
}

static int screen_to_sq(int dfile, int drank)
{
    if (!g_flip) return drank * 8 + dfile;
    return (7 - drank) * 8 + (7 - dfile);
}

static void draw_board(void)
{
    /* Board occupies rows BOARD_Y to BOARD_Y+15, cols BOARD_X to BOARD_X+33 */
    for (int drank = 7; drank >= 0; drank--) {
        int screen_row = BOARD_Y + (7 - drank) * CELL_H;

        for (int line = 0; line < CELL_H; line++) {
            goto_xy(BOARD_X, screen_row + line);

            /* Rank label on left */
            if (line == 1) {
                int actual_rank = g_flip ? (7 - drank) : drank;
                printf(FG_GRAY "%d " COL_RESET, actual_rank + 1);
            } else {
                printf("  ");
            }

            for (int dfile = 0; dfile < 8; dfile++) {
                int sq = screen_to_sq(dfile, drank);
                int p  = g_game.board[sq];

                /* Determine background */
                int is_light = ((dfile + drank) % 2 == 1);
                const char *bg;

                if (sq == g_cursor_sq && sq == g_selected_sq)
                    bg = BG_SELECT;
                else if (sq == g_cursor_sq)
                    bg = BG_CURSOR;
                else if (sq == g_selected_sq)
                    bg = BG_SELECT;
                else {
                    /* Check if legal move target */
                    int is_legal = 0;
                    for (int i = 0; i < g_legal_count; i++) {
                        if (g_legal_moves[i] == sq) { is_legal = 1; break; }
                    }
                    if (is_legal)
                        bg = BG_LEGAL;
                    else if (sq == g_last_from)
                        bg = BG_LAST_FROM;
                    else if (sq == g_last_to)
                        bg = BG_LAST_TO;
                    else {
                        /* Check if king is in check */
                        if (p == (g_game.turn | KING) && in_check(g_game.turn))
                            bg = BG_CHECK;
                        else
                            bg = is_light ? BG_LIGHT : BG_DARK;
                    }
                }

                /* Foreground */
                const char *fg = (piece_color(p) == WHITE) ?
                                  FG_WHITE_PC : FG_BLACK_PC;
                if (p == EMPTY) fg = "";

                printf("%s", bg);

                if (line == 1) {
                    /* Piece row */
                    if (p != EMPTY) {
                        printf("%s %s " COL_RESET, fg, piece_unicode(p));
                    } else {
                        /* Legal move dot */
                        int is_legal = 0;
                        for (int i = 0; i < g_legal_count; i++) {
                            if (g_legal_moves[i] == sq) { is_legal = 1; break; }
                        }
                        if (is_legal)
                            printf(FG_GRAY " · " COL_RESET);
                        else
                            printf("    " COL_RESET);
                    }
                } else {
                    printf("    " COL_RESET);
                }
            }
        }
    }

    /* File labels */
    goto_xy(BOARD_X + 2, BOARD_Y + 16);
    printf(FG_GRAY);
    for (int f = 0; f < 8; f++) {
        int actual_file = g_flip ? (7 - f) : f;
        printf("  %c ", 'a' + actual_file);
    }
    printf(COL_RESET);
}

/* Draw engine info bar */
static void draw_engine_info(void)
{
    goto_xy(BOARD_X, BOARD_Y + 18);
    printf(FG_CYAN COL_BOLD "Engine: " COL_RESET);
    if (g_engine.active) {
        printf(FG_GREEN "%s" COL_RESET, g_engine.name[0] ?
               g_engine.name : g_engine.path);
        if (g_engine.thinking)
            printf(FG_YELLOW " [thinking...]" COL_RESET);
    } else {
        printf(FG_GRAY "None loaded (press E to load)" COL_RESET);
    }

    goto_xy(BOARD_X, BOARD_Y + 19);
    printf(FG_CYAN "TC: " COL_RESET);
    switch (g_tc.mode) {
        case 0: printf("Depth %d   ", g_tc.depth > 0 ? g_tc.depth : 6); break;
        case 1: printf("Nodes %d   ", g_tc.nodes > 0 ? g_tc.nodes : 100000); break;
        case 2: printf("Time %dms   ", g_tc.movetime > 0 ? g_tc.movetime : 1000); break;
        case 3: printf("W:%ds B:%ds (+%d/%d)   ",
                       g_tc.wtime/1000, g_tc.btime/1000,
                       g_tc.winc/1000, g_tc.binc/1000); break;
    }
}

/* Draw PGN panel to the right of the board */
static void draw_pgn(void)
{
    int px = BOARD_X + 38;
    int py = BOARD_Y;
    int pw = 42; /* panel width */

    goto_xy(px, py - 1);
    printf(FG_TITLE COL_BOLD "── Moves (PGN) ─────────────────────" COL_RESET);

    /* Word-wrap PGN into panel */
    char copy[MAX_PGN_LEN];
    strncpy(copy, g_pgn, MAX_PGN_LEN - 1);

    char lines[24][64];
    int  nlines = 0;
    char *tok = strtok(copy, " ");
    int  cur_line = 0, cur_len = 0;
    memset(lines, 0, sizeof(lines));

    while (tok && nlines < 24) {
        int tlen = strlen(tok);
        if (cur_len + tlen + 1 > pw - 2) {
            cur_line++;
            cur_len = 0;
            if (cur_line >= 24) break;
        }
        if (cur_len > 0) { strcat(lines[cur_line], " "); cur_len++; }
        strcat(lines[cur_line], tok);
        cur_len += tlen;
        tok = strtok(NULL, " ");
        nlines = cur_line + 1;
    }

    /* Show last 20 lines */
    int start = nlines > 20 ? nlines - 20 : 0;
    for (int i = 0; i < 20; i++) {
        goto_xy(px, py + i);
        int li = start + i;
        if (li < nlines)
            printf(FG_UI "%-*s" COL_RESET, pw - 2, lines[li]);
        else
            printf("%-*s", pw - 2, "");
    }

    /* Result */
    goto_xy(px, py + 20);
    if (g_game_over)
        printf(FG_YELLOW COL_BOLD "Result: %-10s" COL_RESET, g_result);
    else
        printf("                    ");
}

/* Draw sidebar info */
static void draw_sidebar(void)
{
    int px = BOARD_X + 38;
    int py = BOARD_Y + 22;

    goto_xy(px, py);
    printf(FG_TITLE COL_BOLD "── Controls ────────────────────────" COL_RESET);
    goto_xy(px, py + 1);
    printf(FG_GRAY "Arrows/HJKL  Move cursor" COL_RESET);
    goto_xy(px, py + 2);
    printf(FG_GRAY "Enter/Space  Select / Move" COL_RESET);
    goto_xy(px, py + 3);
    printf(FG_GRAY "U            Undo" COL_RESET);
    goto_xy(px, py + 4);
    printf(FG_GRAY "N            New game" COL_RESET);
    goto_xy(px, py + 5);
    printf(FG_GRAY "E            Load engine" COL_RESET);
    goto_xy(px, py + 6);
    printf(FG_GRAY "T            Time controls" COL_RESET);
    goto_xy(px, py + 7);
    printf(FG_GRAY "F            Flip board" COL_RESET);
    goto_xy(px, py + 8);
    printf(FG_GRAY "Q            Quit" COL_RESET);
}

static void draw_status(void)
{
    goto_xy(BOARD_X, BOARD_Y + 20);
    printf(FG_YELLOW COL_BOLD "Status: " COL_RESET);
    printf(FG_UI "%-50s" COL_RESET, g_status);

    /* Cursor info */
    goto_xy(BOARD_X, BOARD_Y + 21);
    char csq_str[4];
    sq_to_str(g_cursor_sq, csq_str);
    printf(FG_GRAY "Cursor: %s  Move %d   " COL_RESET,
           csq_str, g_game.fullmove);
}

static void draw_title(void)
{
    goto_xy(1, 1);
    printf(FG_TITLE COL_BOLD
           "  ♔ Terminal Chess GUI — UCI Engine Interface ♚"
           COL_RESET);
}

static void redraw(void)
{
    term_hide();
    draw_title();
    draw_board();
    draw_status();
    draw_engine_info();
    draw_pgn();
    draw_sidebar();
    /* Move cursor off board */
    goto_xy(1, 30);
    fflush(stdout);
    term_show();
}

/* ─── Input Handling ─────────────────────────────────────────────────────────*/
static void term_show_cursor_input(void)
{
    /* Temporarily restore terminal for text input */
    term_restore();
    term_show();
}

static void term_hide_cursor_input(void)
{
    term_raw();
    term_hide();
}

static void get_string_input(const char *prompt, char *buf, int maxlen)
{
    term_show_cursor_input();
    goto_xy(BOARD_X, BOARD_Y + 23);
    printf(FG_CYAN "%s" COL_RESET, prompt);
    fflush(stdout);
    if (fgets(buf, maxlen, stdin)) {
        int l = strlen(buf);
        if (l > 0 && buf[l-1] == '\n') buf[l-1] = '\0';
    }
    term_hide_cursor_input();
}

static void handle_load_engine(void)
{
    char path[MAX_PATH] = "";
    term_restore();
    term_show();
    printf("\033[%d;%dH", BOARD_Y + 23, BOARD_X);
    printf(FG_CYAN "Engine path (e.g. /usr/local/bin/stockfish): " COL_RESET);
    fflush(stdout);
    if (fgets(path, MAX_PATH, stdin)) {
        int l = strlen(path);
        if (l > 0 && path[l-1] == '\n') path[l-1] = '\0';
    }
    term_raw();
    term_hide();

    if (strlen(path) == 0) {
        snprintf(g_status, sizeof(g_status), "Engine load cancelled");
        return;
    }

    /* Check file exists */
    if (access(path, X_OK) != 0) {
        snprintf(g_status, sizeof(g_status),
                 "Cannot execute: %s", path);
        return;
    }

    if (engine_start(path) == 0) {
        snprintf(g_status, sizeof(g_status),
                 "Engine loaded: %s",
                 g_engine.name[0] ? g_engine.name : path);
        /* Ask which color engine plays */
        term_restore();
        term_show();
        printf("\033[%d;%dH", BOARD_Y + 24, BOARD_X);
        printf(FG_CYAN "Engine plays [w]hite or [b]lack (default b): " COL_RESET);
        fflush(stdout);
        char choice[8] = "b";
        if (fgets(choice, 8, stdin)) {
            int l = strlen(choice);
            if (l > 0 && choice[l-1] == '\n') choice[l-1] = '\0';
        }
        term_raw();
        term_hide();
        if (choice[0] == 'w' || choice[0] == 'W') {
            g_engine_color = WHITE;
            g_human_color  = BLACK;
        } else {
            g_engine_color = BLACK;
            g_human_color  = WHITE;
        }
        g_engine.color = g_engine_color;
    } else {
        snprintf(g_status, sizeof(g_status), "Failed to start engine");
    }
}

static void handle_time_controls(void)
{
    term_restore();
    term_show();
    printf("\033[%d;%dH", BOARD_Y + 23, BOARD_X);
    printf(FG_CYAN "Time control mode:\n" COL_RESET);
    printf("  [0] Depth (default 6)\n");
    printf("  [1] Nodes\n");
    printf("  [2] Move time (ms)\n");
    printf("  [3] Game time (w/b time + increment)\n");
    printf(FG_CYAN "Choice [0-3]: " COL_RESET);
    fflush(stdout);

    char line[64];
    if (!fgets(line, 64, stdin)) { term_raw(); term_hide(); return; }
    int mode = atoi(line);
    if (mode < 0 || mode > 3) mode = 0;
    g_tc.mode = mode;

    switch (mode) {
        case 0:
            printf(FG_CYAN "Depth (e.g. 10): " COL_RESET);
            fflush(stdout);
            if (fgets(line, 64, stdin)) g_tc.depth = atoi(line);
            if (g_tc.depth < 1) g_tc.depth = 6;
            break;
        case 1:
            printf(FG_CYAN "Nodes (e.g. 500000): " COL_RESET);
            fflush(stdout);
            if (fgets(line, 64, stdin)) g_tc.nodes = atoi(line);
            if (g_tc.nodes < 1) g_tc.nodes = 100000;
            break;
        case 2:
            printf(FG_CYAN "Move time in ms (e.g. 2000): " COL_RESET);
            fflush(stdout);
            if (fgets(line, 64, stdin)) g_tc.movetime = atoi(line);
            if (g_tc.movetime < 100) g_tc.movetime = 1000;
            break;
        case 3:
            printf(FG_CYAN "White time in seconds: " COL_RESET);
            fflush(stdout);
            if (fgets(line, 64, stdin)) g_tc.wtime = atoi(line) * 1000;
            printf(FG_CYAN "Black time in seconds: " COL_RESET);
            fflush(stdout);
            if (fgets(line, 64, stdin)) g_tc.btime = atoi(line) * 1000;
            printf(FG_CYAN "Increment in seconds (0 for none): " COL_RESET);
            fflush(stdout);
            if (fgets(line, 64, stdin)) {
                int inc = atoi(line) * 1000;
                g_tc.winc = inc; g_tc.binc = inc;
            }
            break;
    }

    term_raw();
    term_hide();
    snprintf(g_status, sizeof(g_status), "Time control updated");
}

/* Promotion prompt */
static char prompt_promotion(void)
{
    term_restore();
    term_show();
    printf("\033[%d;%dH", BOARD_Y + 23, BOARD_X);
    printf(FG_CYAN "Promote to: [Q]ueen [R]ook [B]ishop [N]ight: " COL_RESET);
    fflush(stdout);
    char line[8];
    char promo = 'q';
    if (fgets(line, 8, stdin)) {
        switch(tolower(line[0])) {
            case 'r': promo = 'r'; break;
            case 'b': promo = 'b'; break;
            case 'n': promo = 'n'; break;
            default:  promo = 'q'; break;
        }
    }
    term_raw();
    term_hide();
    return promo;
}

/* Try to make a move from selected to cursor */
static void try_move(int from, int to)
{
    if (from == to) {
        g_selected_sq = -1;
        g_legal_count = 0;
        return;
    }

    int p = g_game.board[from];
    int type = piece_type(p);
    char promo = 0;

    /* Check if move is in legal list */
    int found = 0;
    for (int i = 0; i < g_legal_count; i++) {
        if (g_legal_moves[i] == to) { found = 1; break; }
    }
    if (!found) {
        /* Maybe selecting a different own piece */
        if (piece_color(g_game.board[to]) == g_game.turn) {
            g_selected_sq = to;
            g_legal_count = gen_legal_moves(to, g_legal_moves);
        } else {
            snprintf(g_status, sizeof(g_status), "Illegal move");
        }
        return;
    }

    /* Pawn promotion */
    if (type == PAWN && (to / 8 == 7 || to / 8 == 0))
        promo = prompt_promotion();

    if (apply_move(from, to, promo)) {
        g_selected_sq = -1;
        g_legal_count = 0;

        /* If engine should respond */
        if (g_engine.active && !g_game_over &&
            g_game.turn == g_engine_color) {
            engine_go();
        }
    }
}

static void handle_select(void)
{
    int sq = g_cursor_sq;

    if (g_game_over) {
        snprintf(g_status, sizeof(g_status), "Game over. Press N for new game.");
        return;
    }

    /* If it's the engine's turn, don't allow human moves */
    if (g_engine.active && g_game.turn == g_engine_color) {
        snprintf(g_status, sizeof(g_status), "Engine is thinking...");
        return;
    }

    if (g_selected_sq == -1) {
        /* Select a piece */
        int p = g_game.board[sq];
        if (p == EMPTY || piece_color(p) != g_game.turn) {
            snprintf(g_status, sizeof(g_status), "No %s piece on %c%d",
                     g_game.turn == WHITE ? "white" : "black",
                     'a' + (sq % 8), (sq / 8) + 1);
            return;
        }
        g_selected_sq = sq;
        g_legal_count = gen_legal_moves(sq, g_legal_moves);
        char ssq[4];
        sq_to_str(sq, ssq);
        snprintf(g_status, sizeof(g_status),
                 "Selected %s on %s (%d legal moves)",
                 piece_unicode(p), ssq, g_legal_count);
    } else {
        /* Move or re-select */
        try_move(g_selected_sq, sq);
    }
}

static void move_cursor(int df, int dr)
{
    int file = g_cursor_sq % 8;
    int rank = g_cursor_sq / 8;
    file += df;
    rank += dr;
    if (file < 0) file = 0;
    if (file > 7) file = 7;
    if (rank < 0) rank = 0;
    if (rank > 7) rank = 7;
    g_cursor_sq = rank * 8 + file;
}

/* Read input with non-blocking check for engine */
static int read_key(void)
{
    unsigned char buf[4] = {0};
    ssize_t n = read(STDIN_FILENO, buf, 1);
    if (n <= 0) return 0;

    if (buf[0] == 27) {
        /* Escape sequence */
        n = read(STDIN_FILENO, buf + 1, 1);
        if (n <= 0) return 27; /* plain ESC */
        if (buf[1] == '[') {
            n = read(STDIN_FILENO, buf + 2, 1);
            if (n <= 0) return 0;
            switch (buf[2]) {
                case 'A': return 1001; /* Up    */
                case 'B': return 1002; /* Down  */
                case 'C': return 1003; /* Right */
                case 'D': return 1004; /* Left  */
            }
        }
        return 0;
    }
    return buf[0];
}

/* ─── Main Loop ──────────────────────────────────────────────────────────────*/
static void cleanup(void)
{
    if (g_engine.active) {
        engine_write("quit\n");
        close(g_engine.in_fd);
        close(g_engine.out_fd);
        sleep(1);
        waitpid(g_engine.pid, NULL, WNOHANG);
    }
    term_restore();
    term_show();
    term_clear();
    printf("Thanks for playing!\n");
}

int main(void)
{
    /* Default time control */
    g_tc.mode   = 0;
    g_tc.depth  = 6;
    g_tc.nodes  = 100000;
    g_tc.movetime = 1000;
    g_tc.wtime  = 300000;
    g_tc.btime  = 300000;
    g_tc.winc   = 0;
    g_tc.binc   = 0;

    init_board();

    term_raw();
    term_hide();
    term_clear();

    snprintf(g_status, sizeof(g_status), "White to move. Load engine with E.");

    redraw();

    while (g_running) {
        /* Poll engine if thinking */
        if (g_engine.active && g_engine.thinking) {
            char best[8] = "";
            if (engine_poll(best) && strlen(best) >= 4) {
                /* Got best move from engine */
                if (strcmp(best, "0000") != 0) {
                    apply_uci_move(best);
                }
                g_engine.thinking = 0;
                redraw();
            } else {
                /* Partial redraw for engine status */
                draw_engine_info();
                fflush(stdout);
            }
        }

        int key = read_key();
        if (key == 0) continue;

        int need_redraw = 1;

        switch (key) {
            case 'q': case 'Q':
                g_running = 0;
                break;

            /* Movement */
            case 1001: case 'k': case 'K': /* Up */
                if (g_flip) move_cursor(0, -1);
                else        move_cursor(0,  1);
                break;
            case 1002: case 'j': case 'J': /* Down */
                if (g_flip) move_cursor(0,  1);
                else        move_cursor(0, -1);
                break;
            case 1003: case 'l': case 'L': /* Right */
                if (g_flip) move_cursor(-1, 0);
                else        move_cursor( 1, 0);
                break;
            case 1004: case 'h': case 'H': /* Left */
                if (g_flip) move_cursor( 1, 0);
                else        move_cursor(-1, 0);
                break;

            /* Select / move */
            case '\r': case '\n': case ' ':
                handle_select();
                break;

            /* Escape = deselect */
            case 27:
                g_selected_sq = -1;
                g_legal_count = 0;
                break;

            /* Undo */
            case 'u': case 'U':
                if (g_snap_count > 0) {
                    undo_move();
                } else {
                    snprintf(g_status, sizeof(g_status), "Nothing to undo");
                }
                break;

            /* New game */
            case 'n': case 'N':
                if (g_engine.thinking) engine_stop();
                init_board();
                g_cursor_sq = 4;
                snprintf(g_status, sizeof(g_status),
                         "New game. %s to move.",
                         g_game.turn == WHITE ? "White" : "Black");
                /* If engine plays white, let it go */
                if (g_engine.active && g_game.turn == g_engine_color)
                    engine_go();
                break;

            /* Flip board */
            case 'f': case 'F':
                g_flip ^= 1;
                break;

            /* Load engine */
            case 'e': case 'E':
                handle_load_engine();
                /* If engine plays current color, start thinking */
                if (g_engine.active && !g_game_over &&
                    g_game.turn == g_engine_color)
                    engine_go();
                break;

            /* Time controls */
            case 't': case 'T':
                handle_time_controls();
                break;

            default:
                need_redraw = 0;
                break;
        }

        if (need_redraw)
            redraw();
    }

    cleanup();
    return 0;
}
