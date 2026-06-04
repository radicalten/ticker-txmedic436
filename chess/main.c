#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>

/* ─────────────────────────────────────────────
   CONSTANTS & TYPES
───────────────────────────────────────────── */

#define BOARD_SIZE 8
#define MAX_MOVES  512
#define MAX_HISTORY 1024
#define PGN_BUF    65536
#define ENGINE_BUF 4096

typedef enum { EMPTY=0, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING } PieceType;
typedef enum { NONE=0, WHITE, BLACK } Color;

typedef struct {
    PieceType type;
    Color     color;
} Piece;

typedef struct {
    int from_r, from_c;
    int to_r,   to_c;
    PieceType promo;           /* promotion piece or EMPTY */
    Piece captured;
    int castling;              /* 1=kingside,2=queenside */
    int ep_capture;            /* en passant capture */
    int ep_col_before;         /* previous ep col (-1 if none) */
    int ep_col_after;
    int halfmove_before;
    /* castling rights before move */
    int cr_wk, cr_wq, cr_bk, cr_bq;
    char pgn[16];
} Move;

typedef struct {
    Piece board[8][8];
    Color turn;
    int ep_col;                /* en passant target column, -1 if none */
    int ep_row;
    int castle_wk, castle_wq; /* white castling rights */
    int castle_bk, castle_bq;
    int halfmove;
    int fullmove;
    Move history[MAX_HISTORY];
    int hist_count;
    int selected_r, selected_c;  /* cursor/selection */
    int piece_selected;
    Move legal[MAX_MOVES];
    int legal_count;
    /* engine */
    int engine_pid;
    int eng_in[2];   /* pipe: GUI writes, engine reads */
    int eng_out[2];  /* pipe: engine writes, GUI reads */
    int engine_active;
    Color engine_color;
    int engine_thinking;
    char engine_path[512];
    /* display */
    int flip;       /* board flip */
    char status_msg[256];
    char pgn_buf[PGN_BUF];
} GameState;

/* ─────────────────────────────────────────────
   TERMINAL HANDLING
───────────────────────────────────────────── */

static struct termios orig_termios;

static void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    /* show cursor, reset colors */
    printf("\033[?25h\033[0m\033[2J\033[H");
    fflush(stdout);
}

static void setup_terminal(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); /* hide cursor */
    fflush(stdout);
}

/* Read a key; returns -1 if nothing available.
   Returns special codes for arrows: 1001=UP,1002=DOWN,1003=LEFT,1004=RIGHT */
static int read_key(void) {
    unsigned char c;
    int n = (int)read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    if (c == 27) {
        unsigned char seq[4];
        int n2 = (int)read(STDIN_FILENO, seq, 3);
        if (n2 >= 2 && seq[0] == '[') {
            if (seq[1] == 'A') return 1001;
            if (seq[1] == 'B') return 1002;
            if (seq[1] == 'C') return 1004;
            if (seq[1] == 'D') return 1003;
        }
        return 27;
    }
    return (int)c;
}

/* ─────────────────────────────────────────────
   PIECE HELPERS
───────────────────────────────────────────── */

static Color opponent(Color c) { return c == WHITE ? BLACK : WHITE; }

static const char *piece_unicode(Piece p, int use_dark_bg) {
    /* White pieces */
    if (p.color == WHITE) {
        switch(p.type) {
            case KING:   return "\u2654";
            case QUEEN:  return "\u2655";
            case ROOK:   return "\u2656";
            case BISHOP: return "\u2657";
            case KNIGHT: return "\u2658";
            case PAWN:   return "\u2659";
            default: break;
        }
    }
    /* Black pieces */
    if (p.color == BLACK) {
        switch(p.type) {
            case KING:   return "\u265A";
            case QUEEN:  return "\u265B";
            case ROOK:   return "\u265C";
            case BISHOP: return "\u265D";
            case KNIGHT: return "\u265E";
            case PAWN:   return "\u265F";
            default: break;
        }
    }
    return " ";
}

static char piece_char(PieceType t) {
    switch(t) {
        case KING:   return 'K';
        case QUEEN:  return 'Q';
        case ROOK:   return 'R';
        case BISHOP: return 'B';
        case KNIGHT: return 'N';
        case PAWN:   return 'P';
        default:     return ' ';
    }
}

static PieceType char_to_piece(char c) {
    switch(toupper((unsigned char)c)) {
        case 'K': return KING;
        case 'Q': return QUEEN;
        case 'R': return ROOK;
        case 'B': return BISHOP;
        case 'N': return KNIGHT;
        case 'P': return PAWN;
    }
    return EMPTY;
}

/* ─────────────────────────────────────────────
   BOARD INIT & FEN
───────────────────────────────────────────── */

static void init_board(GameState *g) {
    memset(g, 0, sizeof(*g));
    g->ep_col = -1;
    g->ep_row = -1;
    g->engine_pid = -1;
    g->engine_color = NONE;
    g->selected_r = 0;
    g->selected_c = 0;

    /* Place pieces */
    PieceType back[] = {ROOK,KNIGHT,BISHOP,QUEEN,KING,BISHOP,KNIGHT,ROOK};
    for (int c = 0; c < 8; c++) {
        g->board[0][c] = (Piece){back[c], BLACK};
        g->board[1][c] = (Piece){PAWN, BLACK};
        g->board[6][c] = (Piece){PAWN, WHITE};
        g->board[7][c] = (Piece){back[c], WHITE};
    }
    g->turn = WHITE;
    g->castle_wk = g->castle_wq = g->castle_bk = g->castle_bq = 1;
    g->fullmove = 1;
    g->halfmove = 0;
    strcpy(g->status_msg, "Use arrow keys to move cursor, ENTER to select/move, 'u' to undo, 'f' to flip, 'q' to quit");
}

/* Generate FEN for engine */
static void board_to_fen(GameState *g, char *fen, int sz) {
    char buf[256] = {0};
    int pos = 0;
    for (int r = 0; r < 8; r++) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            Piece p = g->board[r][c];
            if (p.type == EMPTY) {
                empty++;
            } else {
                if (empty > 0) { buf[pos++] = '0' + empty; empty = 0; }
                char ch = piece_char(p.type);
                buf[pos++] = (p.color == WHITE) ? ch : (char)tolower((unsigned char)ch);
            }
        }
        if (empty > 0) buf[pos++] = '0' + empty;
        if (r < 7) buf[pos++] = '/';
    }
    buf[pos] = 0;

    char ep[4] = "-";
    if (g->ep_col >= 0 && g->ep_row >= 0) {
        ep[0] = 'a' + g->ep_col;
        ep[1] = '8' - g->ep_row;
        ep[2] = 0;
    }
    char castle[8] = "-";
    int ci = 0;
    if (g->castle_wk) castle[ci++] = 'K';
    if (g->castle_wq) castle[ci++] = 'Q';
    if (g->castle_bk) castle[ci++] = 'k';
    if (g->castle_bq) castle[ci++] = 'q';
    if (ci == 0) { castle[0] = '-'; castle[1] = 0; }
    else castle[ci] = 0;

    snprintf(fen, sz, "%s %c %s %s %d %d",
             buf,
             g->turn == WHITE ? 'w' : 'b',
             castle, ep,
             g->halfmove, g->fullmove);
}

/* ─────────────────────────────────────────────
   MOVE GENERATION HELPERS
───────────────────────────────────────────── */

static int in_bounds(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

/* Check if square (r,c) is attacked by 'attacker' color */
static int is_attacked(GameState *g, int r, int c, Color attacker) {
    /* Pawns */
    int pdir = (attacker == WHITE) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int ar = r + pdir;
        int ac = c + dc;
        if (in_bounds(ar, ac)) {
            Piece p = g->board[ar][ac];
            if (p.color == attacker && p.type == PAWN) return 1;
        }
    }
    /* Knights */
    int kd[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int ar = r + kd[i][0], ac = c + kd[i][1];
        if (in_bounds(ar,ac)) {
            Piece p = g->board[ar][ac];
            if (p.color == attacker && p.type == KNIGHT) return 1;
        }
    }
    /* Bishops / Queens (diagonals) */
    int bd[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 4; i++) {
        for (int dist = 1; dist < 8; dist++) {
            int ar = r + bd[i][0]*dist, ac = c + bd[i][1]*dist;
            if (!in_bounds(ar,ac)) break;
            Piece p = g->board[ar][ac];
            if (p.type != EMPTY) {
                if (p.color == attacker && (p.type == BISHOP || p.type == QUEEN)) return 1;
                break;
            }
        }
    }
    /* Rooks / Queens (straight) */
    int rd[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int i = 0; i < 4; i++) {
        for (int dist = 1; dist < 8; dist++) {
            int ar = r + rd[i][0]*dist, ac = c + rd[i][1]*dist;
            if (!in_bounds(ar,ac)) break;
            Piece p = g->board[ar][ac];
            if (p.type != EMPTY) {
                if (p.color == attacker && (p.type == ROOK || p.type == QUEEN)) return 1;
                break;
            }
        }
    }
    /* King */
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            int ar = r+dr, ac = c+dc;
            if (in_bounds(ar,ac)) {
                Piece p = g->board[ar][ac];
                if (p.color == attacker && p.type == KING) return 1;
            }
        }
    }
    return 0;
}

static int find_king(GameState *g, Color c, int *kr, int *kc) {
    for (int r = 0; r < 8; r++)
        for (int col = 0; col < 8; col++)
            if (g->board[r][col].color == c && g->board[r][col].type == KING) {
                *kr = r; *kc = col; return 1;
            }
    return 0;
}

static int in_check(GameState *g, Color c) {
    int kr, kc;
    if (!find_king(g, c, &kr, &kc)) return 0;
    return is_attacked(g, kr, kc, opponent(c));
}

/* Apply move on board without legality check (for move gen) */
static void apply_move_raw(GameState *g, Move *m) {
    Piece moving = g->board[m->from_r][m->from_c];

    /* En passant capture */
    if (m->ep_capture) {
        int cap_r = (moving.color == WHITE) ? m->to_r + 1 : m->to_r - 1;
        g->board[cap_r][m->to_c] = (Piece){EMPTY, NONE};
    }

    /* Castling */
    if (m->castling) {
        int rook_from_c = (m->castling == 1) ? 7 : 0;
        int rook_to_c   = (m->castling == 1) ? m->to_c - 1 : m->to_c + 1;
        g->board[m->from_r][rook_to_c] = g->board[m->from_r][rook_from_c];
        g->board[m->from_r][rook_from_c] = (Piece){EMPTY, NONE};
    }

    /* Move piece */
    g->board[m->to_r][m->to_c] = moving;
    g->board[m->from_r][m->from_c] = (Piece){EMPTY, NONE};

    /* Promotion */
    if (m->promo != EMPTY) {
        g->board[m->to_r][m->to_c].type = m->promo;
    }
}

static void undo_move_raw(GameState *g, Move *m) {
    Piece moved = g->board[m->to_r][m->to_c];
    if (m->promo != EMPTY) moved.type = PAWN;

    g->board[m->from_r][m->from_c] = moved;
    g->board[m->to_r][m->to_c] = m->captured;

    if (m->ep_capture) {
        int cap_r = (moved.color == WHITE) ? m->to_r + 1 : m->to_r - 1;
        g->board[cap_r][m->to_c] = (Piece){PAWN, opponent(moved.color)};
    }

    if (m->castling) {
        int rook_from_c = (m->castling == 1) ? 7 : 0;
        int rook_to_c   = (m->castling == 1) ? m->to_c - 1 : m->to_c + 1;
        g->board[m->from_r][rook_from_c] = g->board[m->from_r][rook_to_c];
        g->board[m->from_r][rook_to_c] = (Piece){EMPTY, NONE};
    }
}

/* Check if a pseudo-legal move leaves own king in check */
static int move_leaves_check(GameState *g, Move *m) {
    Color mover = g->board[m->from_r][m->from_c].color;
    apply_move_raw(g, m);
    int check = in_check(g, mover);
    undo_move_raw(g, m);
    return check;
}

/* ─────────────────────────────────────────────
   PGN NOTATION HELPERS
───────────────────────────────────────────── */

static void generate_pgn_move(GameState *g, Move *m, char *out, int outsz) {
    /* Called before move is applied */
    Piece p = g->board[m->from_r][m->from_c];
    char buf[32] = {0};
    int pos = 0;

    if (m->castling == 1) { snprintf(out, outsz, "O-O"); return; }
    if (m->castling == 2) { snprintf(out, outsz, "O-O-O"); return; }

    char pc = piece_char(p.type);
    if (p.type != PAWN) buf[pos++] = pc;

    /* Disambiguation */
    if (p.type != PAWN) {
        int amb_r = 0, amb_c = 0, amb_count = 0;
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                if (r == m->from_r && c == m->from_c) continue;
                Piece q = g->board[r][c];
                if (q.color != p.color || q.type != p.type) continue;
                /* Check if this piece can also move to same square */
                Move tm;
                memset(&tm, 0, sizeof(tm));
                tm.from_r = r; tm.from_c = c;
                tm.to_r = m->to_r; tm.to_c = m->to_c;
                tm.captured = g->board[m->to_r][m->to_c];
                if (!move_leaves_check(g, &tm)) {
                    /* Check pseudo-legality for this piece type */
                    int dr = abs(m->to_r - r), dc = abs(m->to_c - c);
                    int legal_pseudo = 0;
                    if (q.type == ROOK) {
                        if (r == m->to_r || c == m->to_c) {
                            /* Check clear path */
                            int sr = (m->to_r - r) ? (m->to_r > r ? 1 : -1) : 0;
                            int sc = (m->to_c - c) ? (m->to_c > c ? 1 : -1) : 0;
                            int clear = 1;
                            for (int d = 1; ; d++) {
                                int ir = r + sr*d, ic = c + sc*d;
                                if (ir == m->to_r && ic == m->to_c) break;
                                if (g->board[ir][ic].type != EMPTY) { clear = 0; break; }
                            }
                            legal_pseudo = clear;
                        }
                    } else if (q.type == BISHOP) {
                        if (dr == dc) {
                            int sr = m->to_r > r ? 1 : -1;
                            int sc = m->to_c > c ? 1 : -1;
                            int clear = 1;
                            for (int d = 1; d < dr; d++) {
                                if (g->board[r+sr*d][c+sc*d].type != EMPTY) { clear = 0; break; }
                            }
                            legal_pseudo = clear;
                        }
                    } else if (q.type == QUEEN) {
                        if (r == m->to_r || c == m->to_c || dr == dc) {
                            int sr = m->to_r == r ? 0 : (m->to_r > r ? 1 : -1);
                            int sc = m->to_c == c ? 0 : (m->to_c > c ? 1 : -1);
                            int steps = (r == m->to_r) ? abs(m->to_c - c) : abs(m->to_r - r);
                            int clear = 1;
                            for (int d = 1; d < steps; d++) {
                                if (g->board[r+sr*d][c+sc*d].type != EMPTY) { clear = 0; break; }
                            }
                            legal_pseudo = clear;
                        }
                    } else if (q.type == KNIGHT) {
                        legal_pseudo = (dr == 2 && dc == 1) || (dr == 1 && dc == 2);
                    }
                    if (legal_pseudo) {
                        amb_count++;
                        if (c != m->from_c) amb_c = 1;
                        else amb_r = 1;
                    }
                }
            }
        }
        if (amb_count > 0) {
            if (amb_c && !amb_r) buf[pos++] = 'a' + m->from_c;
            else if (!amb_c && amb_r) buf[pos++] = '8' - m->from_r;
            else { buf[pos++] = 'a' + m->from_c; buf[pos++] = '8' - m->from_r; }
        }
    } else {
        /* Pawn capture disambiguation */
        if (m->captured.type != EMPTY || m->ep_capture) {
            buf[pos++] = 'a' + m->from_c;
        }
    }

    if (m->captured.type != EMPTY || m->ep_capture) buf[pos++] = 'x';
    buf[pos++] = 'a' + m->to_c;
    buf[pos++] = '8' - m->to_r;

    if (m->promo != EMPTY) {
        buf[pos++] = '=';
        buf[pos++] = piece_char(m->promo);
    }
    buf[pos] = 0;

    /* Check / checkmate after move */
    apply_move_raw(g, m);
    Color opp = opponent(p.color);
    int is_chk = in_check(g, opp);
    undo_move_raw(g, m);

    if (is_chk) {
        /* Quick mate check: generate legal moves for opponent after move */
        apply_move_raw(g, m);
        /* Count legal moves for opponent - simplified */
        int has_escape = 0;
        for (int r = 0; r < 8 && !has_escape; r++) {
            for (int c = 0; c < 8 && !has_escape; c++) {
                Piece q = g->board[r][c];
                if (q.color != opp) continue;
                for (int tr = 0; tr < 8 && !has_escape; tr++) {
                    for (int tc = 0; tc < 8 && !has_escape; tc++) {
                        if (tr == r && tc == c) continue;
                        Piece cap = g->board[tr][tc];
                        if (cap.color == opp) continue;
                        Move tm;
                        memset(&tm, 0, sizeof(tm));
                        tm.from_r = r; tm.from_c = c;
                        tm.to_r = tr; tm.to_c = tc;
                        tm.captured = cap;
                        if (!move_leaves_check(g, &tm)) has_escape = 1;
                    }
                }
            }
        }
        undo_move_raw(g, m);
        strcat(buf, has_escape ? "+" : "#");
    }

    snprintf(out, outsz, "%s", buf);
}

/* ─────────────────────────────────────────────
   LEGAL MOVE GENERATION
───────────────────────────────────────────── */

static void add_move_if_legal(GameState *g, Move *moves, int *cnt,
                               int fr, int fc, int tr, int tc,
                               PieceType promo, int castling, int ep_cap) {
    if (!in_bounds(tr, tc)) return;
    Piece target = g->board[tr][tc];
    Piece mover  = g->board[fr][fc];
    if (target.color == mover.color) return;

    Move m;
    memset(&m, 0, sizeof(m));
    m.from_r = fr; m.from_c = fc;
    m.to_r   = tr; m.to_c   = tc;
    m.captured = target;
    m.promo = promo;
    m.castling = castling;
    m.ep_capture = ep_cap;
    if (ep_cap) m.captured = (Piece){EMPTY, NONE};

    if (!move_leaves_check(g, &m)) {
        moves[(*cnt)++] = m;
    }
}

/* Generate all legal moves for current position */
static int generate_legal_moves(GameState *g, Move *moves) {
    int cnt = 0;
    Color c = g->turn;

    for (int r = 0; r < 8; r++) {
        for (int col = 0; col < 8; col++) {
            Piece p = g->board[r][col];
            if (p.color != c) continue;

            if (p.type == PAWN) {
                int dir = (c == WHITE) ? -1 : 1;
                int start_r = (c == WHITE) ? 6 : 1;
                int promo_r = (c == WHITE) ? 0 : 7;

                /* Forward one */
                int nr = r + dir;
                if (in_bounds(nr, col) && g->board[nr][col].type == EMPTY) {
                    if (nr == promo_r) {
                        PieceType promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
                        for (int i = 0; i < 4; i++)
                            add_move_if_legal(g, moves, &cnt, r, col, nr, col, promos[i], 0, 0);
                    } else {
                        add_move_if_legal(g, moves, &cnt, r, col, nr, col, EMPTY, 0, 0);
                        /* Forward two */
                        if (r == start_r && g->board[r+2*dir][col].type == EMPTY)
                            add_move_if_legal(g, moves, &cnt, r, col, r+2*dir, col, EMPTY, 0, 0);
                    }
                }
                /* Captures */
                for (int dc = -1; dc <= 1; dc += 2) {
                    int nc = col + dc;
                    if (!in_bounds(nr, nc)) continue;
                    /* En passant */
                    if (g->ep_col == nc && g->ep_row == nr) {
                        add_move_if_legal(g, moves, &cnt, r, col, nr, nc, EMPTY, 0, 1);
                    } else if (g->board[nr][nc].type != EMPTY && g->board[nr][nc].color != c) {
                        if (nr == promo_r) {
                            PieceType promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
                            for (int i = 0; i < 4; i++)
                                add_move_if_legal(g, moves, &cnt, r, col, nr, nc, promos[i], 0, 0);
                        } else {
                            add_move_if_legal(g, moves, &cnt, r, col, nr, nc, EMPTY, 0, 0);
                        }
                    }
                }
            }
            else if (p.type == KNIGHT) {
                int kd[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
                for (int i = 0; i < 8; i++)
                    add_move_if_legal(g, moves, &cnt, r, col,
                                       r+kd[i][0], col+kd[i][1], EMPTY, 0, 0);
            }
            else if (p.type == BISHOP || p.type == ROOK || p.type == QUEEN) {
                int dirs[8][2] = {{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};
                int start = 0, end = 8;
                if (p.type == BISHOP) { start = 4; end = 8; }
                if (p.type == ROOK)   { start = 0; end = 4; }
                for (int i = start; i < end; i++) {
                    for (int dist = 1; dist < 8; dist++) {
                        int nr = r + dirs[i][0]*dist;
                        int nc = col + dirs[i][1]*dist;
                        if (!in_bounds(nr, nc)) break;
                        Piece t = g->board[nr][nc];
                        if (t.type != EMPTY) {
                            if (t.color != c)
                                add_move_if_legal(g, moves, &cnt, r, col, nr, nc, EMPTY, 0, 0);
                            break;
                        }
                        add_move_if_legal(g, moves, &cnt, r, col, nr, nc, EMPTY, 0, 0);
                    }
                }
            }
            else if (p.type == KING) {
                for (int dr = -1; dr <= 1; dr++)
                    for (int dc = -1; dc <= 1; dc++) {
                        if (!dr && !dc) continue;
                        add_move_if_legal(g, moves, &cnt, r, col, r+dr, col+dc, EMPTY, 0, 0);
                    }
                /* Castling */
                int king_r = (c == WHITE) ? 7 : 0;
                if (r == king_r && col == 4 && !in_check(g, c)) {
                    /* Kingside */
                    int wk = (c == WHITE) ? g->castle_wk : g->castle_bk;
                    if (wk &&
                        g->board[king_r][5].type == EMPTY &&
                        g->board[king_r][6].type == EMPTY &&
                        !is_attacked(g, king_r, 5, opponent(c)) &&
                        !is_attacked(g, king_r, 6, opponent(c))) {
                        Move m;
                        memset(&m, 0, sizeof(m));
                        m.from_r = king_r; m.from_c = 4;
                        m.to_r   = king_r; m.to_c   = 6;
                        m.castling = 1;
                        if (!move_leaves_check(g, &m)) moves[cnt++] = m;
                    }
                    /* Queenside */
                    int wq = (c == WHITE) ? g->castle_wq : g->castle_bq;
                    if (wq &&
                        g->board[king_r][3].type == EMPTY &&
                        g->board[king_r][2].type == EMPTY &&
                        g->board[king_r][1].type == EMPTY &&
                        !is_attacked(g, king_r, 3, opponent(c)) &&
                        !is_attacked(g, king_r, 2, opponent(c))) {
                        Move m;
                        memset(&m, 0, sizeof(m));
                        m.from_r = king_r; m.from_c = 4;
                        m.to_r   = king_r; m.to_c   = 2;
                        m.castling = 2;
                        if (!move_leaves_check(g, &m)) moves[cnt++] = m;
                    }
                }
            }
        }
    }
    return cnt;
}

/* ─────────────────────────────────────────────
   APPLY / UNDO FULL MOVE
───────────────────────────────────────────── */

static void update_pgn(GameState *g, Move *m) {
    char num[16] = {0};
    if (g->turn == WHITE) {
        snprintf(num, sizeof(num), "%d. ", g->fullmove);
        strncat(g->pgn_buf, num, PGN_BUF - strlen(g->pgn_buf) - 1);
    }
    strncat(g->pgn_buf, m->pgn, PGN_BUF - strlen(g->pgn_buf) - 1);
    strncat(g->pgn_buf, " ", PGN_BUF - strlen(g->pgn_buf) - 1);
}

static void apply_full_move(GameState *g, Move *m) {
    /* Save state into move for undo */
    m->ep_col_before = g->ep_col;
    m->halfmove_before = g->halfmove;
    m->cr_wk = g->castle_wk; m->cr_wq = g->castle_wq;
    m->cr_bk = g->castle_bk; m->cr_bq = g->castle_bq;

    /* Generate PGN before applying */
    generate_pgn_move(g, m, m->pgn, sizeof(m->pgn));
    update_pgn(g, m);

    apply_move_raw(g, m);

    Piece moved = g->board[m->to_r][m->to_c];

    /* Update castling rights */
    if (moved.type == KING) {
        if (moved.color == WHITE) { g->castle_wk = g->castle_wq = 0; }
        else                      { g->castle_bk = g->castle_bq = 0; }
    }
    if (moved.type == ROOK) {
        if (moved.color == WHITE) {
            if (m->from_c == 7) g->castle_wk = 0;
            if (m->from_c == 0) g->castle_wq = 0;
        } else {
            if (m->from_c == 7) g->castle_bk = 0;
            if (m->from_c == 0) g->castle_bq = 0;
        }
    }
    /* If rook was captured */
    if (m->captured.type == ROOK) {
        if (m->captured.color == WHITE) {
            if (m->to_c == 7 && m->to_r == 7) g->castle_wk = 0;
            if (m->to_c == 0 && m->to_r == 7) g->castle_wq = 0;
        } else {
            if (m->to_c == 7 && m->to_r == 0) g->castle_bk = 0;
            if (m->to_c == 0 && m->to_r == 0) g->castle_bq = 0;
        }
    }

    /* En passant target */
    g->ep_col = -1; g->ep_row = -1;
    if (moved.type == PAWN && abs(m->to_r - m->from_r) == 2) {
        g->ep_col = m->to_c;
        g->ep_row = (m->from_r + m->to_r) / 2;
    }

    /* Halfmove clock */
    if (moved.type == PAWN || m->captured.type != EMPTY)
        g->halfmove = 0;
    else
        g->halfmove++;

    if (g->turn == BLACK) g->fullmove++;
    g->turn = opponent(g->turn);

    /* Store in history */
    if (g->hist_count < MAX_HISTORY)
        g->history[g->hist_count++] = *m;
}

static void undo_full_move(GameState *g) {
    if (g->hist_count == 0) return;
    Move *m = &g->history[--g->hist_count];

    g->turn = opponent(g->turn);
    if (g->turn == BLACK) g->fullmove--;

    undo_move_raw(g, m);

    /* Restore state */
    g->ep_col = m->ep_col_before;
    g->ep_row = -1;
    if (g->ep_col >= 0) {
        g->ep_row = (g->turn == WHITE) ? 2 : 5;
    }
    g->halfmove = m->halfmove_before;
    g->castle_wk = m->cr_wk; g->castle_wq = m->cr_wq;
    g->castle_bk = m->cr_bk; g->castle_bq = m->cr_bq;

    /* Trim PGN - find last space and cut */
    int plen = (int)strlen(g->pgn_buf);
    if (plen > 0 && g->pgn_buf[plen-1] == ' ') plen--;
    /* Remove last token */
    while (plen > 0 && g->pgn_buf[plen-1] != ' ') plen--;
    /* If we removed a move number token too, check */
    if (plen > 0 && g->pgn_buf[plen-1] == ' ') plen--;
    /* If last char is a '.', remove move number */
    if (plen > 0 && g->pgn_buf[plen-1] == '.') {
        while (plen > 0 && g->pgn_buf[plen-1] != ' ') plen--;
    } else {
        plen++; /* keep the space */
    }
    if (plen < 0) plen = 0;
    g->pgn_buf[plen] = 0;
}

/* ─────────────────────────────────────────────
   ENGINE COMMUNICATION
───────────────────────────────────────────── */

static int start_engine(GameState *g, const char *path) {
    if (pipe(g->eng_in) < 0 || pipe(g->eng_out) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        /* Child: engine process */
        dup2(g->eng_in[0],  STDIN_FILENO);
        dup2(g->eng_out[1], STDOUT_FILENO);
        close(g->eng_in[0]); close(g->eng_in[1]);
        close(g->eng_out[0]); close(g->eng_out[1]);
        /* Redirect stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        execlp(path, path, NULL);
        exit(1);
    }
    /* Parent */
    close(g->eng_in[0]);
    close(g->eng_out[1]);
    g->engine_pid = pid;

    /* Set engine stdout non-blocking */
    int flags = fcntl(g->eng_out[0], F_GETFL, 0);
    fcntl(g->eng_out[0], F_SETFL, flags | O_NONBLOCK);

    /* Init UCI */
    const char *uci_init = "uci\nisready\n";
    write(g->eng_in[1], uci_init, strlen(uci_init));

    /* Wait for readyok */
    char rbuf[4096] = {0};
    int rpos = 0;
    time_t start = time(NULL);
    while (time(NULL) - start < 5) {
        char tmp[256];
        int n = (int)read(g->eng_out[0], tmp, sizeof(tmp)-1);
        if (n > 0) {
            tmp[n] = 0;
            strncat(rbuf, tmp, sizeof(rbuf)-rpos-1);
            rpos += n;
            if (strstr(rbuf, "readyok")) break;
        }
        usleep(10000);
    }
    g->engine_active = 1;
    return 1;
}

static void stop_engine(GameState *g) {
    if (!g->engine_active) return;
    write(g->eng_in[1], "quit\n", 5);
    usleep(100000);
    kill(g->engine_pid, SIGTERM);
    waitpid(g->engine_pid, NULL, WNOHANG);
    close(g->eng_in[1]);
    close(g->eng_out[0]);
    g->engine_active = 0;
    g->engine_pid = -1;
}

static void engine_send_position(GameState *g) {
    /* Build move list */
    char moves_str[MAX_HISTORY * 6 + 64] = {0};
    strcat(moves_str, "position startpos");
    if (g->hist_count > 0) {
        strcat(moves_str, " moves");
        for (int i = 0; i < g->hist_count; i++) {
            Move *m = &g->history[i];
            char ms[8];
            snprintf(ms, sizeof(ms), " %c%d%c%d",
                     'a' + m->from_c, 8 - m->from_r,
                     'a' + m->to_c,   8 - m->to_r);
            if (m->promo != EMPTY) {
                char pc = (char)tolower((unsigned char)piece_char(m->promo));
                ms[5] = pc; ms[6] = 0;
            }
            strncat(moves_str, ms, sizeof(moves_str)-strlen(moves_str)-1);
        }
    }
    strcat(moves_str, "\n");
    write(g->eng_in[1], moves_str, strlen(moves_str));
}

static void engine_go(GameState *g) {
    engine_send_position(g);
    const char *go_cmd = "go movetime 1000\n";
    write(g->eng_in[1], go_cmd, strlen(go_cmd));
    g->engine_thinking = 1;
}

/* Try to read bestmove from engine; returns 1 if got it */
static int engine_poll(GameState *g, char *best_move, int bmsz) {
    char buf[ENGINE_BUF];
    int n = (int)read(g->eng_out[0], buf, sizeof(buf)-1);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *bm = strstr(buf, "bestmove");
    if (!bm) return 0;
    bm += 9;
    while (*bm == ' ') bm++;
    int i = 0;
    while (i < bmsz-1 && *bm && *bm != ' ' && *bm != '\n' && *bm != '\r')
        best_move[i++] = *bm++;
    best_move[i] = 0;
    g->engine_thinking = 0;
    return 1;
}

/* Parse UCI move string like "e2e4" or "e7e8q" */
static int parse_uci_move(GameState *g, const char *mv, Move *out) {
    if (strlen(mv) < 4) return 0;
    int fc = mv[0] - 'a';
    int fr = '8' - mv[1];
    int tc = mv[2] - 'a';
    int tr = '8' - mv[3];
    PieceType promo = EMPTY;
    if (mv[4]) promo = char_to_piece(mv[4]);

    /* Find this move in legal moves */
    Move legal[MAX_MOVES];
    int lc = generate_legal_moves(g, legal);
    for (int i = 0; i < lc; i++) {
        Move *m = &legal[i];
        if (m->from_r == fr && m->from_c == fc &&
            m->to_r == tr && m->to_c == tc) {
            if (promo == EMPTY || m->promo == promo) {
                *out = *m;
                return 1;
            }
        }
    }
    return 0;
}

/* ─────────────────────────────────────────────
   DISPLAY
───────────────────────────────────────────── */

/* ANSI color codes */
#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"

/* Background colors */
#define BG_LIGHT        "\033[48;5;223m"   /* light square */
#define BG_DARK         "\033[48;5;137m"   /* dark square */
#define BG_CURSOR       "\033[48;5;227m"   /* cursor highlight */
#define BG_SELECTED     "\033[48;5;148m"   /* selected piece */
#define BG_LEGAL        "\033[48;5;107m"   /* legal move target */
#define BG_LAST_FROM    "\033[48;5;215m"   /* last move from */
#define BG_LAST_TO      "\033[48;5;208m"   /* last move to */
#define BG_CHECK        "\033[48;5;196m"   /* king in check */

/* Foreground */
#define FG_WHITE_PC     "\033[97m"
#define FG_BLACK_PC     "\033[30m"
#define FG_UI           "\033[38;5;255m"
#define FG_RANK         "\033[38;5;243m"

#define CELL_W 4  /* characters wide per cell (space + piece + space + border) */
#define CELL_H 2  /* lines tall per cell */

static void goto_xy(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

static void clear_screen(void) {
    printf("\033[2J\033[H");
}

/* Is the square a legal move target from the selected piece? */
static int is_legal_target(GameState *g, int r, int c) {
    for (int i = 0; i < g->legal_count; i++) {
        if (g->legal[i].to_r == r && g->legal[i].to_c == c) return 1;
    }
    return 0;
}

static void draw_board(GameState *g) {
    clear_screen();

    /* Determine last move squares */
    int last_fr = -1, last_fc = -1, last_tr = -1, last_tc = -1;
    if (g->hist_count > 0) {
        Move *lm = &g->history[g->hist_count-1];
        last_fr = lm->from_r; last_fc = lm->from_c;
        last_tr = lm->to_r;   last_tc = lm->to_c;
    }

    /* King in check */
    int check_r = -1, check_c = -1;
    if (in_check(g, g->turn)) {
        find_king(g, g->turn, &check_r, &check_c);
    }

    int board_top  = 2;
    int board_left = 5;

    /* File labels top */
    goto_xy(board_top - 1, board_left + 1);
    for (int ci = 0; ci < 8; ci++) {
        int display_c = g->flip ? (7 - ci) : ci;
        printf(FG_RANK "  %c " ANSI_RESET, 'a' + display_c);
    }

    for (int ri = 0; ri < 8; ri++) {
        int display_r = g->flip ? ri : (7 - ri);
        /* Each cell is 2 lines tall */
        for (int line = 0; line < CELL_H; line++) {
            int screen_row = board_top + ri * CELL_H + line;
            goto_xy(screen_row, 1);

            /* Rank label */
            if (line == 0)
                printf(FG_RANK " %d " ANSI_RESET, 8 - display_r);
            else
                printf("   ");

            for (int ci = 0; ci < 8; ci++) {
                int display_c = g->flip ? (7 - ci) : ci;
                int r = display_r, c = display_c;
                Piece p = g->board[r][c];

                /* Determine background */
                int is_light = (r + c) % 2 == 0;
                const char *bg;

                if (r == check_r && c == check_c) {
                    bg = BG_CHECK;
                } else if (g->piece_selected && r == g->selected_r && c == g->selected_c) {
                    bg = BG_SELECTED;
                } else if (g->piece_selected && is_legal_target(g, r, c)) {
                    bg = BG_LEGAL;
                } else if (!g->piece_selected && r == g->selected_r && c == g->selected_c) {
                    bg = BG_CURSOR;
                } else if (r == last_fr && c == last_fc) {
                    bg = BG_LAST_FROM;
                } else if (r == last_tr && c == last_tc) {
                    bg = BG_LAST_TO;
                } else {
                    bg = is_light ? BG_LIGHT : BG_DARK;
                }

                const char *fg = (p.color == WHITE) ? FG_WHITE_PC : FG_BLACK_PC;
                if (p.color == NONE) fg = "";

                printf("%s%s", bg, fg);

                if (line == 0) {
                    /* Top half of cell: piece */
                    if (p.type != EMPTY)
                        printf(" %s " ANSI_RESET, piece_unicode(p, !is_light));
                    else
                        printf("    " ANSI_RESET);
                } else {
                    /* Bottom half: dot for legal move */
                    if (g->piece_selected && is_legal_target(g, r, c) && p.type == EMPTY)
                        printf("  \u00b7 " ANSI_RESET);
                    else
                        printf("    " ANSI_RESET);
                }
            }
            /* Right rank label */
            if (line == 0)
                printf(FG_RANK " %d" ANSI_RESET, 8 - display_r);
        }
    }

    /* File labels bottom */
    goto_xy(board_top + 8 * CELL_H, board_left + 1);
    for (int ci = 0; ci < 8; ci++) {
        int display_c = g->flip ? (7 - ci) : ci;
        printf(FG_RANK "  %c " ANSI_RESET, 'a' + display_c);
    }

    /* ── Side panel ── */
    int panel_col = board_left + 8 * CELL_W + 6;
    int panel_row = board_top;

    /* Turn indicator */
    goto_xy(panel_row, panel_col);
    printf(ANSI_BOLD FG_UI "═══════════════════════" ANSI_RESET);
    panel_row++;
    goto_xy(panel_row++, panel_col);
    const char *turn_str = g->turn == WHITE ? "⬜ WHITE to move" : "⬛ BLACK to move";
    printf(ANSI_BOLD FG_UI "  %s" ANSI_RESET, turn_str);
    goto_xy(panel_row++, panel_col);
    printf(ANSI_BOLD FG_UI "═══════════════════════" ANSI_RESET);

    /* Check / Checkmate / Stalemate */
    Move legal_all[MAX_MOVES];
    int lc = generate_legal_moves(g, legal_all);
    goto_xy(panel_row++, panel_col);
    if (lc == 0) {
        if (in_check(g, g->turn))
            printf(ANSI_BOLD "\033[91m  ♛ CHECKMATE! %s wins!\033[0m",
                   g->turn == WHITE ? "BLACK" : "WHITE");
        else
            printf(ANSI_BOLD "\033[93m  ½ STALEMATE!\033[0m");
    } else if (in_check(g, g->turn)) {
        printf(ANSI_BOLD "\033[91m  ⚠ CHECK!\033[0m          ");
    } else {
        printf("                       ");
    }

    /* Engine status */
    goto_xy(panel_row++, panel_col);
    if (g->engine_active) {
        printf(FG_UI "  Engine: %s%s" ANSI_RESET,
               g->engine_color == WHITE ? "White" :
               g->engine_color == BLACK ? "Black" : "Both",
               g->engine_thinking ? " [thinking...]" : " [ready]");
    } else {
        printf(FG_UI "  No engine loaded        " ANSI_RESET);
    }

    /* Move history / PGN */
    goto_xy(panel_row++, panel_col);
    printf(ANSI_BOLD FG_UI "─── Move History ───────" ANSI_RESET);

    /* Display last ~14 moves worth of PGN tokens */
    char pgn_copy[PGN_BUF];
    strncpy(pgn_copy, g->pgn_buf, PGN_BUF-1);
    pgn_copy[PGN_BUF-1] = 0;

    /* Split into tokens and display last N lines */
    char *tokens[512];
    int tok_cnt = 0;
    char *tok = strtok(pgn_copy, " ");
    while (tok && tok_cnt < 512) {
        tokens[tok_cnt++] = tok;
        tok = strtok(NULL, " ");
    }

    /* Group into lines of ~3 tokens (num, white, black) */
    char lines[20][64];
    int line_cnt = 0;
    int ti = 0;
    while (ti < tok_cnt && line_cnt < 20) {
        char line[64] = {0};
        if (ti < tok_cnt && strchr(tokens[ti], '.')) {
            strncat(line, tokens[ti++], 63 - strlen(line));
            strncat(line, " ", 63 - strlen(line));
        }
        if (ti < tok_cnt && !strchr(tokens[ti], '.')) {
            strncat(line, tokens[ti++], 63 - strlen(line));
            strncat(line, " ", 63 - strlen(line));
        }
        if (ti < tok_cnt && !strchr(tokens[ti], '.')) {
            strncat(line, tokens[ti++], 63 - strlen(line));
        }
        strncpy(lines[line_cnt++], line, 63);
    }

    int display_lines = 12;
    int start_line = line_cnt > display_lines ? line_cnt - display_lines : 0;
    for (int i = start_line; i < line_cnt; i++) {
        goto_xy(panel_row++, panel_col);
        printf(FG_UI "  %-21s" ANSI_RESET, lines[i]);
    }
    /* Pad remaining lines */
    while (panel_row < board_top + 8 * CELL_H + 1) {
        goto_xy(panel_row++, panel_col);
        printf("                       ");
    }

    /* Status bar at bottom */
    int status_row = board_top + 8 * CELL_H + 2;
    goto_xy(status_row, 1);
    printf("\033[K"); /* clear line */
    printf(ANSI_BOLD FG_UI "Controls: " ANSI_RESET
           FG_UI "Arrows=cursor  Enter=select/move  u=undo  f=flip  e=engine  q=quit" ANSI_RESET);

    goto_xy(status_row + 1, 1);
    printf("\033[K");
    printf(FG_UI "%.78s" ANSI_RESET, g->status_msg);

    /* Cursor position indicator */
    goto_xy(status_row + 2, 1);
    int dr = g->flip ? (7 - g->selected_r) : g->selected_r;
    int dc = g->flip ? (7 - g->selected_c) : g->selected_c;
    printf(FG_UI "Cursor: %c%d" ANSI_RESET "  Move: %d  Half: %d",
           'a' + dc, 8 - dr, g->fullmove, g->halfmove);

    fflush(stdout);
}

/* ─────────────────────────────────────────────
   PROMOTION SELECTION
───────────────────────────────────────────── */

static PieceType prompt_promotion(GameState *g) {
    /* Simple terminal prompt for promotion choice */
    int row = 22;
    goto_xy(row, 1);
    printf(ANSI_BOLD FG_UI "Promote to: [Q]ueen  [R]ook  [B]ishop  [N]knight : " ANSI_RESET);
    fflush(stdout);

    /* Temporarily switch to blocking input */
    struct termios t = orig_termios;
    t.c_lflag &= ~(ECHO | ICANON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);

    PieceType promo = QUEEN;
    while (1) {
        char c;
        read(STDIN_FILENO, &c, 1);
        c = (char)toupper((unsigned char)c);
        if (c == 'Q') { promo = QUEEN;  break; }
        if (c == 'R') { promo = ROOK;   break; }
        if (c == 'B') { promo = BISHOP; break; }
        if (c == 'N') { promo = KNIGHT; break; }
    }

    /* Restore raw mode */
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    return promo;
}

/* ─────────────────────────────────────────────
   ENGINE SETUP PROMPT
───────────────────────────────────────────── */

static void prompt_engine_setup(GameState *g) {
    /* Temporarily restore canonical mode */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); /* show cursor */

    int row = 22;
    goto_xy(row, 1);
    printf("\033[K" ANSI_BOLD "Engine path (or Enter to cancel): " ANSI_RESET);
    fflush(stdout);

    char path[512] = {0};
    if (!fgets(path, sizeof(path), stdin)) {
        goto restore;
    }
    path[strcspn(path, "\n")] = 0;
    if (path[0] == 0) goto restore;

    if (g->engine_active) stop_engine(g);

    if (!start_engine(g, path)) {
        snprintf(g->status_msg, sizeof(g->status_msg),
                 "Failed to start engine: %s", path);
        goto restore;
    }

    strncpy(g->engine_path, path, sizeof(g->engine_path)-1);

    goto_xy(row+1, 1);
    printf("\033[K" ANSI_BOLD "Play as [w]hite, [b]lack, or [n]one (engine vs engine): " ANSI_RESET);
    fflush(stdout);

    char choice[8] = {0};
    if (!fgets(choice, sizeof(choice), stdin)) { goto restore; }
    choice[strcspn(choice, "\n")] = 0;

    if (choice[0] == 'w' || choice[0] == 'W') {
        g->engine_color = BLACK; /* engine plays black */
    } else if (choice[0] == 'b' || choice[0] == 'B') {
        g->engine_color = WHITE; /* engine plays white */
    } else {
        g->engine_color = NONE; /* no engine */
    }

    snprintf(g->status_msg, sizeof(g->status_msg),
             "Engine loaded: %s (plays %s)", path,
             g->engine_color == BLACK ? "Black" :
             g->engine_color == WHITE ? "White" : "None");

    /* If engine should move first, trigger it */
    if (g->engine_active && g->engine_color == g->turn) {
        engine_go(g);
    }

restore:
    /* Restore raw mode */
    {
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG);
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        printf("\033[?25l");
        fflush(stdout);
    }
}

/* ─────────────────────────────────────────────
   HANDLE USER INPUT
───────────────────────────────────────────── */

static void handle_enter(GameState *g) {
    int r = g->selected_r;
    int c = g->selected_c;

    /* Check game over */
    Move all_legal[MAX_MOVES];
    int lc = generate_legal_moves(g, all_legal);
    if (lc == 0) return;

    /* Engine's turn - don't allow human input */
    if (g->engine_active && g->engine_color == g->turn) return;

    if (!g->piece_selected) {
        /* Select a piece */
        Piece p = g->board[r][c];
        if (p.color == g->turn) {
            g->piece_selected = 1;
            /* Generate legal moves for this piece */
            g->legal_count = 0;
            for (int i = 0; i < lc; i++) {
                if (all_legal[i].from_r == r && all_legal[i].from_c == c) {
                    g->legal[g->legal_count++] = all_legal[i];
                }
            }
            if (g->legal_count == 0) {
                g->piece_selected = 0;
                snprintf(g->status_msg, sizeof(g->status_msg),
                         "No legal moves for that piece.");
            }
        }
    } else {
        /* Try to move to this square */
        int sel_r = -1, sel_c = -1;
        /* Find source from legal moves */
        if (g->legal_count > 0) {
            sel_r = g->legal[0].from_r;
            sel_c = g->legal[0].from_c;
        }

        /* Find matching legal move */
        Move *chosen = NULL;
        Move promo_moves[4];
        int promo_cnt = 0;

        for (int i = 0; i < g->legal_count; i++) {
            if (g->legal[i].to_r == r && g->legal[i].to_c == c) {
                if (g->legal[i].promo != EMPTY) {
                    promo_moves[promo_cnt++] = g->legal[i];
                } else {
                    chosen = &g->legal[i];
                    break;
                }
            }
        }

        if (promo_cnt > 0) {
            PieceType pt = prompt_promotion(g);
            for (int i = 0; i < promo_cnt; i++) {
                if (promo_moves[i].promo == pt) {
                    apply_full_move(g, &promo_moves[i]);
                    g->piece_selected = 0;
                    g->legal_count = 0;
                    snprintf(g->status_msg, sizeof(g->status_msg),
                             "Promoted to %s!", piece_char(pt) == 'Q' ? "Queen" :
                             piece_char(pt) == 'R' ? "Rook" :
                             piece_char(pt) == 'B' ? "Bishop" : "Knight");
                    if (g->engine_active && g->engine_color == g->turn)
                        engine_go(g);
                    return;
                }
            }
        } else if (chosen) {
            apply_full_move(g, chosen);
            g->piece_selected = 0;
            g->legal_count = 0;
            snprintf(g->status_msg, sizeof(g->status_msg), "Move played.");
            if (g->engine_active && g->engine_color == g->turn)
                engine_go(g);
        } else if (r == sel_r && c == sel_c) {
            /* Clicked same square: deselect */
            g->piece_selected = 0;
            g->legal_count = 0;
        } else {
            /* Try to select different piece of same color */
            Piece p = g->board[r][c];
            if (p.color == g->turn) {
                g->piece_selected = 1;
                g->legal_count = 0;
                for (int i = 0; i < lc; i++) {
                    if (all_legal[i].from_r == r && all_legal[i].from_c == c) {
                        g->legal[g->legal_count++] = all_legal[i];
                    }
                }
                if (g->legal_count == 0) {
                    g->piece_selected = 0;
                    snprintf(g->status_msg, sizeof(g->status_msg),
                             "No legal moves for that piece.");
                }
            } else {
                g->piece_selected = 0;
                g->legal_count = 0;
            }
        }
    }
}

/* ─────────────────────────────────────────────
   MAIN LOOP
───────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    GameState *g = calloc(1, sizeof(GameState));
    if (!g) { fprintf(stderr, "Out of memory\n"); return 1; }

    init_board(g);
    setup_terminal();

    /* If engine path given on command line */
    if (argc >= 2) {
        if (start_engine(g, argv[1])) {
            strncpy(g->engine_path, argv[1], sizeof(g->engine_path)-1);
            g->engine_color = BLACK; /* default: engine plays black */
            snprintf(g->status_msg, sizeof(g->status_msg),
                     "Engine loaded: %s (plays Black)", argv[1]);
        } else {
            snprintf(g->status_msg, sizeof(g->status_msg),
                     "Failed to load engine: %s", argv[1]);
        }
    }

    int running = 1;
    int need_redraw = 1;

    while (running) {
        /* Poll engine for best move */
        if (g->engine_active && g->engine_thinking) {
            char bm[16] = {0};
            if (engine_poll(g, bm, sizeof(bm))) {
                if (strlen(bm) >= 4 && strcmp(bm, "(none)") != 0) {
                    Move em;
                    if (parse_uci_move(g, bm, &em)) {
                        apply_full_move(g, &em);
                        g->piece_selected = 0;
                        g->legal_count = 0;
                        snprintf(g->status_msg, sizeof(g->status_msg),
                                 "Engine played: %s", em.pgn);
                        /* If engine plays both colors (engine vs engine) */
                        if (g->engine_color == NONE) {
                            engine_go(g);
                        }
                    } else {
                        snprintf(g->status_msg, sizeof(g->status_msg),
                                 "Engine illegal move: %s", bm);
                    }
                    need_redraw = 1;
                }
            }
        }

        if (need_redraw) {
            draw_board(g);
            need_redraw = 0;
        }

        int key = read_key();
        if (key < 0) {
            /* No key pressed, small sleep */
            usleep(16000); /* ~60fps */
            /* Still redraw if engine is thinking (to show status) */
            if (g->engine_active && g->engine_thinking) need_redraw = 1;
            continue;
        }

        need_redraw = 1;

        if (key == 'q' || key == 'Q') {
            running = 0;
        } else if (key == 'f' || key == 'F') {
            g->flip = !g->flip;
            /* Convert cursor position for flip */
        } else if (key == 'u' || key == 'U') {
            /* Undo - if engine is playing, undo twice */
            if (g->engine_thinking) {
                write(g->eng_in[1], "stop\n", 5);
                usleep(100000);
                g->engine_thinking = 0;
            }
            if (g->engine_active && g->engine_color != NONE && g->hist_count >= 2) {
                undo_full_move(g);
                undo_full_move(g);
            } else {
                undo_full_move(g);
            }
            g->piece_selected = 0;
            g->legal_count = 0;
            snprintf(g->status_msg, sizeof(g->status_msg), "Move undone.");
        } else if (key == 'e' || key == 'E') {
            prompt_engine_setup(g);
        } else if (key == 'n' || key == 'N') {
            /* New game */
            if (g->engine_active) {
                write(g->eng_in[1], "stop\n", 5);
                usleep(50000);
                g->engine_thinking = 0;
            }
            /* Keep engine settings */
            int ea = g->engine_active;
            int epid = g->engine_pid;
            int ein0 = g->eng_in[0], ein1 = g->eng_in[1];
            int eout0 = g->eng_out[0], eout1 = g->eng_out[1];
            Color ecol = g->engine_color;
            char epath[512];
            strncpy(epath, g->engine_path, sizeof(epath)-1);

            init_board(g);

            g->engine_active = ea;
            g->engine_pid = epid;
            g->eng_in[0] = ein0; g->eng_in[1] = ein1;
            g->eng_out[0] = eout0; g->eng_out[1] = eout1;
            g->engine_color = ecol;
            strncpy(g->engine_path, epath, sizeof(g->engine_path)-1);

            snprintf(g->status_msg, sizeof(g->status_msg), "New game started.");

            if (g->engine_active) {
                write(g->eng_in[1], "ucinewgame\n", 11);
                if (g->engine_color == g->turn) engine_go(g);
            }
        } else if (key == 1001) { /* UP */
            if (g->flip) {
                if (g->selected_r < 7) g->selected_r++;
            } else {
                if (g->selected_r > 0) g->selected_r--;
            }
        } else if (key == 1002) { /* DOWN */
            if (g->flip) {
                if (g->selected_r > 0) g->selected_r--;
            } else {
                if (g->selected_r < 7) g->selected_r++;
            }
        } else if (key == 1003) { /* LEFT */
            if (g->flip) {
                if (g->selected_c < 7) g->selected_c++;
            } else {
                if (g->selected_c > 0) g->selected_c--;
            }
        } else if (key == 1004) { /* RIGHT */
            if (g->flip) {
                if (g->selected_c > 0) g->selected_c--;
            } else {
                if (g->selected_c < 7) g->selected_c++;
            }
        } else if (key == '\r' || key == '\n' || key == ' ') {
            handle_enter(g);
        } else if (key == 27) { /* ESC - deselect */
            g->piece_selected = 0;
            g->legal_count = 0;
        }
    }

    if (g->engine_active) stop_engine(g);
    free(g);
    return 0;
}
