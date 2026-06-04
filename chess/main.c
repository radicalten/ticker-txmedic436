/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 * Compile: gcc -o chess_gui chess_gui.c
 * Usage:   ./chess_gui [engine_path]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

/* ─────────────────────────────────────────────
   TERMINAL / ANSI
───────────────────────────────────────────── */
#define ANSI_RESET       "\x1b[0m"
#define ANSI_BOLD        "\x1b[1m"
#define ANSI_DIM         "\x1b[2m"

/* 256-colour backgrounds */
#define BG_LIGHT         "\x1b[48;5;180m"   /* light square  */
#define BG_DARK          "\x1b[48;5;94m"    /* dark square   */
#define BG_SELECTED      "\x1b[48;5;226m"   /* selected      */
#define BG_LEGAL         "\x1b[48;5;40m"    /* legal target  */
#define BG_LASTMOVE      "\x1b[48;5;33m"    /* last move     */
#define BG_CHECK         "\x1b[48;5;196m"   /* king in check */
#define BG_CURSOR        "\x1b[48;5;220m"   /* cursor        */

#define FG_WHITE_PIECE   "\x1b[38;5;255m"
#define FG_BLACK_PIECE   "\x1b[38;5;232m"
#define FG_LABEL         "\x1b[38;5;250m"

/* sidebar colours */
#define FG_CYAN          "\x1b[36m"
#define FG_YELLOW        "\x1b[33m"
#define FG_GREEN         "\x1b[32m"
#define FG_RED           "\x1b[31m"
#define FG_MAGENTA       "\x1b[35m"
#define FG_WHITE         "\x1b[37m"
#define FG_GRAY          "\x1b[38;5;245m"

/* ─────────────────────────────────────────────
   PIECE CONSTANTS
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

/* piece = type | (color << 3)  → 0-13 */
#define PIECE(type,color) ((type)|((color)<<3))
#define PTYPE(p)  ((p)&7)
#define PCOLOR(p) (((p)>>3)&1)

/* Unicode pieces (wide chars) */
static const char *PIECE_GLYPHS[2][7] = {
    /* WHITE */ {"  ","♙ ","♘ ","♗ ","♖ ","♕ ","♔ "},
    /* BLACK */ {"  ","♟ ","♞ ","♝ ","♜ ","♛ ","♚ "}
};

/* ─────────────────────────────────────────────
   BOARD & MOVE TYPES
───────────────────────────────────────────── */
typedef uint8_t  Piece;
typedef uint8_t  Square;   /* 0-63, rank-major (a1=0) */

#define SQ(file,rank) ((rank)*8+(file))
#define FILE_OF(sq)   ((sq)%8)
#define RANK_OF(sq)   ((sq)/8)

typedef struct {
    Square from, to;
    Piece  promo;          /* 0 = no promo */
    int    castle;         /* 0,1=K-side, 2=Q-side */
    int    ep;             /* en-passant capture */
    Piece  captured;       /* piece on target before move */
    Square ep_captured_sq; /* square of ep-captured pawn */
} Move;

#define MAX_MOVES 218

typedef struct {
    Piece  board[64];
    int    side;           /* WHITE / BLACK to move     */
    int    castle_rights;  /* bits: WK=1,WQ=2,BK=4,BQ=8 */
    int    ep_sq;          /* en-passant target (-1=none) */
    int    halfmove;
    int    fullmove;
    /* undo info */
    int    prev_castle;
    int    prev_ep;
    int    prev_halfmove;
} Position;

/* ─────────────────────────────────────────────
   MOVE HISTORY (for undo & PGN display)
───────────────────────────────────────────── */
#define MAX_HISTORY 512

typedef struct {
    Move     move;
    Position pos_before;   /* full position snapshot   */
    char     pgn[16];      /* PGN string for this move */
} HistEntry;

static HistEntry history[MAX_HISTORY];
static int       history_len = 0;

/* ─────────────────────────────────────────────
   UCI ENGINE STATE
───────────────────────────────────────────── */
typedef enum { TC_TIME, TC_DEPTH, TC_NODES } TCMode;

typedef struct {
    pid_t  pid;
    int    to_engine[2];   /* write end → engine stdin  */
    int    from_engine[2]; /* read end ← engine stdout  */
    int    alive;
    char   name[64];
    /* time control */
    TCMode tc_mode;
    int    tc_time_ms;     /* ms per move               */
    int    tc_depth;
    long   tc_nodes;
    /* last bestmove */
    char   bestmove[8];
    int    bestmove_ready;
} Engine;

static Engine engine = {0};

/* ─────────────────────────────────────────────
   GUI STATE
───────────────────────────────────────────── */
typedef enum {
    MODE_NAVIGATE,   /* moving cursor                */
    MODE_SELECTED,   /* piece selected, show legals  */
    MODE_PROMO,      /* awaiting promotion choice    */
    MODE_MENU        /* time-control menu            */
} GuiMode;

static Position  pos;
static GuiMode   gui_mode   = MODE_NAVIGATE;
static int       cursor_sq  = 0;     /* current cursor square */
static int       selected_sq= -1;    /* selected piece square */
static Move      legal_buf[MAX_MOVES];
static int       legal_count= 0;
static int       is_legal_target[64];/* quick lookup          */
static int       player_color= WHITE; /* human plays white    */
static int       engine_thinking = 0;
static char      status_msg[128]  = "";
static int       game_over        = 0;
static char      game_result[32]  = "";
static Move      promo_pending;      /* move awaiting promo   */
static int       flip_board        = 0;

/* ─────────────────────────────────────────────
   TERMINAL RAW MODE
───────────────────────────────────────────── */
static struct termios orig_termios;

static void term_raw(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void clear_screen(void) {
    printf("\x1b[2J\x1b[H");
}

static void hide_cursor(void) { printf("\x1b[?25l"); }
static void show_cursor(void) { printf("\x1b[?25h"); }

/* ─────────────────────────────────────────────
   POSITION INIT
───────────────────────────────────────────── */
static void pos_init(Position *p) {
    memset(p, 0, sizeof(*p));
    p->ep_sq = -1;
    p->fullmove = 1;
    p->castle_rights = 0xF;

    /* pawns */
    for (int f = 0; f < 8; f++) {
        p->board[SQ(f,1)] = PIECE(PAWN, WHITE);
        p->board[SQ(f,6)] = PIECE(PAWN, BLACK);
    }
    /* back ranks */
    static const Piece back[] = {ROOK,KNIGHT,BISHOP,QUEEN,KING,BISHOP,KNIGHT,ROOK};
    for (int f = 0; f < 8; f++) {
        p->board[SQ(f,0)] = PIECE(back[f], WHITE);
        p->board[SQ(f,7)] = PIECE(back[f], BLACK);
    }
}

/* ─────────────────────────────────────────────
   MOVE GENERATION
───────────────────────────────────────────── */
static int sq_attacked(const Position *p, int sq, int by_color);

static void add_move(Move *buf, int *cnt, int from, int to,
                     Piece promo, int castle, int ep,
                     const Position *p) {
    Move m;
    m.from = from; m.to = to; m.promo = promo;
    m.castle = castle; m.ep = ep;
    m.captured = p->board[to];
    m.ep_captured_sq = 0;
    if (ep) {
        int dir = (p->side == WHITE) ? -8 : 8;
        m.ep_captured_sq = to + dir;
        m.captured = p->board[m.ep_captured_sq];
    }
    buf[(*cnt)++] = m;
}

/* Sliding piece helpers */
static const int ROOK_DIR[]   = {8,-8,1,-1};
static const int BISHOP_DIR[] = {9,7,-9,-7};
static const int KNIGHT_OFF[] = {17,15,10,6,-6,-10,-15,-17};

static void gen_slider(const Position *p, int sq, Move *buf, int *cnt,
                       const int *dirs, int ndirs) {
    int color = PCOLOR(p->board[sq]);
    for (int d = 0; d < ndirs; d++) {
        int dir = dirs[d];
        int cur = sq;
        for (;;) {
            int nf = FILE_OF(cur) + (dir%8 == 0 ? 0 : (dir > 0 ? dir%8 : -((-dir)%8)));
            /* simpler: just track file changes */
            int next = cur + dir;
            if (next < 0 || next >= 64) break;
            /* wrap check */
            int cf = FILE_OF(cur), nfr = FILE_OF(next);
            if (abs(cf - nfr) > 2) break;
            Piece target = p->board[next];
            if (target == EMPTY) {
                add_move(buf, cnt, sq, next, 0, 0, 0, p);
            } else {
                if (PCOLOR(target) != color)
                    add_move(buf, cnt, sq, next, 0, 0, 0, p);
                break;
            }
            cur = next;
        }
    }
}

static void gen_piece_moves(const Position *p, int sq, Move *buf, int *cnt) {
    Piece piece = p->board[sq];
    if (piece == EMPTY) return;
    int color = PCOLOR(piece);
    int type  = PTYPE(piece);

    if (type == PAWN) {
        int dir = (color == WHITE) ? 8 : -8;
        int start_rank = (color == WHITE) ? 1 : 6;
        int promo_rank = (color == WHITE) ? 7 : 0;
        int next = sq + dir;

        /* forward */
        if (next >= 0 && next < 64 && p->board[next] == EMPTY) {
            if (RANK_OF(next) == promo_rank) {
                for (Piece pp = KNIGHT; pp <= QUEEN; pp++)
                    add_move(buf, cnt, sq, next, PIECE(pp,color), 0, 0, p);
            } else {
                add_move(buf, cnt, sq, next, 0, 0, 0, p);
                /* double push */
                if (RANK_OF(sq) == start_rank) {
                    int next2 = sq + 2*dir;
                    if (p->board[next2] == EMPTY)
                        add_move(buf, cnt, sq, next2, 0, 0, 0, p);
                }
            }
        }
        /* captures */
        int cap_files[2] = {FILE_OF(sq)-1, FILE_OF(sq)+1};
        for (int i = 0; i < 2; i++) {
            int cf = cap_files[i];
            if (cf < 0 || cf > 7) continue;
            int csq = sq + dir + (cf - FILE_OF(sq));
            if (csq < 0 || csq >= 64) continue;
            Piece t = p->board[csq];
            if (t != EMPTY && PCOLOR(t) != color) {
                if (RANK_OF(csq) == promo_rank) {
                    for (Piece pp = KNIGHT; pp <= QUEEN; pp++)
                        add_move(buf, cnt, sq, csq, PIECE(pp,color), 0, 0, p);
                } else {
                    add_move(buf, cnt, sq, csq, 0, 0, 0, p);
                }
            }
            /* en passant */
            if (p->ep_sq >= 0 && csq == p->ep_sq)
                add_move(buf, cnt, sq, csq, 0, 0, 1, p);
        }
    } else if (type == KNIGHT) {
        for (int i = 0; i < 8; i++) {
            int next = sq + KNIGHT_OFF[i];
            if (next < 0 || next >= 64) continue;
            /* file wrap */
            if (abs(FILE_OF(sq) - FILE_OF(next)) > 2) continue;
            Piece t = p->board[next];
            if (t == EMPTY || PCOLOR(t) != color)
                add_move(buf, cnt, sq, next, 0, 0, 0, p);
        }
    } else if (type == BISHOP) {
        gen_slider(p, sq, buf, cnt, BISHOP_DIR, 4);
    } else if (type == ROOK) {
        gen_slider(p, sq, buf, cnt, ROOK_DIR, 4);
    } else if (type == QUEEN) {
        gen_slider(p, sq, buf, cnt, ROOK_DIR, 4);
        gen_slider(p, sq, buf, cnt, BISHOP_DIR, 4);
    } else if (type == KING) {
        int offsets[] = {1,-1,8,-8,9,7,-9,-7};
        for (int i = 0; i < 8; i++) {
            int next = sq + offsets[i];
            if (next < 0 || next >= 64) continue;
            if (abs(FILE_OF(sq)-FILE_OF(next)) > 1) continue;
            Piece t = p->board[next];
            if (t == EMPTY || PCOLOR(t) != color)
                add_move(buf, cnt, sq, next, 0, 0, 0, p);
        }
        /* castling */
        if (color == WHITE && sq == SQ(4,0)) {
            /* kingside */
            if ((p->castle_rights & 1) &&
                p->board[SQ(5,0)]==EMPTY && p->board[SQ(6,0)]==EMPTY &&
                !sq_attacked(p,SQ(4,0),BLACK) &&
                !sq_attacked(p,SQ(5,0),BLACK) &&
                !sq_attacked(p,SQ(6,0),BLACK))
                add_move(buf, cnt, sq, SQ(6,0), 0, 1, 0, p);
            /* queenside */
            if ((p->castle_rights & 2) &&
                p->board[SQ(3,0)]==EMPTY && p->board[SQ(2,0)]==EMPTY &&
                p->board[SQ(1,0)]==EMPTY &&
                !sq_attacked(p,SQ(4,0),BLACK) &&
                !sq_attacked(p,SQ(3,0),BLACK) &&
                !sq_attacked(p,SQ(2,0),BLACK))
                add_move(buf, cnt, sq, SQ(2,0), 0, 2, 0, p);
        }
        if (color == BLACK && sq == SQ(4,7)) {
            if ((p->castle_rights & 4) &&
                p->board[SQ(5,7)]==EMPTY && p->board[SQ(6,7)]==EMPTY &&
                !sq_attacked(p,SQ(4,7),WHITE) &&
                !sq_attacked(p,SQ(5,7),WHITE) &&
                !sq_attacked(p,SQ(6,7),WHITE))
                add_move(buf, cnt, sq, SQ(6,7), 0, 1, 0, p);
            if ((p->castle_rights & 8) &&
                p->board[SQ(3,7)]==EMPTY && p->board[SQ(2,7)]==EMPTY &&
                p->board[SQ(1,7)]==EMPTY &&
                !sq_attacked(p,SQ(4,7),WHITE) &&
                !sq_attacked(p,SQ(3,7),WHITE) &&
                !sq_attacked(p,SQ(2,7),WHITE))
                add_move(buf, cnt, sq, SQ(2,7), 0, 2, 0, p);
        }
    }
}

/* ─── sq_attacked ─── */
static int sq_attacked(const Position *p, int sq, int by_color) {
    /* pawns */
    int pdir = (by_color == WHITE) ? -8 : 8;
    int pawn = PIECE(PAWN, by_color);
    for (int df = -1; df <= 1; df += 2) {
        int attf = FILE_OF(sq) + df;
        if (attf < 0 || attf > 7) continue;
        int att = sq + pdir + df;
        if (att < 0 || att >= 64) continue;
        if (p->board[att] == pawn) return 1;
    }
    /* knights */
    Piece kn = PIECE(KNIGHT, by_color);
    for (int i = 0; i < 8; i++) {
        int att = sq + KNIGHT_OFF[i];
        if (att < 0 || att >= 64) continue;
        if (abs(FILE_OF(sq)-FILE_OF(att)) > 2) continue;
        if (p->board[att] == kn) return 1;
    }
    /* sliders */
    Piece br = PIECE(BISHOP,by_color), rk = PIECE(ROOK,by_color);
    Piece qn = PIECE(QUEEN, by_color), ki = PIECE(KING, by_color);

    /* rook/queen rays */
    for (int d = 0; d < 4; d++) {
        int dir = ROOK_DIR[d];
        int cur = sq;
        for (;;) {
            int next = cur + dir;
            if (next < 0 || next >= 64) break;
            if (dir == 1 || dir == -1) {
                if (RANK_OF(next) != RANK_OF(cur)) break;
            }
            Piece t = p->board[next];
            if (t != EMPTY) {
                if (t == rk || t == qn) return 1;
                if (t == ki && next == cur + dir &&
                    abs(FILE_OF(next)-FILE_OF(cur))<=1) return 1;
                break;
            }
            cur = next;
        }
    }
    /* bishop/queen rays */
    for (int d = 0; d < 4; d++) {
        int dir = BISHOP_DIR[d];
        int cur = sq;
        for (;;) {
            int next = cur + dir;
            if (next < 0 || next >= 64) break;
            if (abs(FILE_OF(next)-FILE_OF(cur)) != 1) break;
            Piece t = p->board[next];
            if (t != EMPTY) {
                if (t == br || t == qn) return 1;
                if (t == ki && next == cur+dir) return 1;
                break;
            }
            cur = next;
        }
    }
    return 0;
}

/* ─── Apply/Undo move (for legality check) ─── */
static void apply_move_internal(Position *p, const Move *m) {
    p->prev_castle   = p->castle_rights;
    p->prev_ep       = p->ep_sq;
    p->prev_halfmove = p->halfmove;

    Piece piece = p->board[m->from];
    int   type  = PTYPE(piece);
    int   color = PCOLOR(piece);

    p->ep_sq = -1;

    if (m->ep) {
        p->board[m->ep_captured_sq] = EMPTY;
    }

    p->board[m->to]   = m->promo ? m->promo : piece;
    p->board[m->from] = EMPTY;

    /* castling rook */
    if (m->castle) {
        int rank = (color == WHITE) ? 0 : 7;
        if (m->castle == 1) { /* kingside */
            p->board[SQ(5,rank)] = PIECE(ROOK, color);
            p->board[SQ(7,rank)] = EMPTY;
        } else {              /* queenside */
            p->board[SQ(3,rank)] = PIECE(ROOK, color);
            p->board[SQ(0,rank)] = EMPTY;
        }
    }

    /* double pawn push → set ep */
    if (type == PAWN && abs((int)m->to - (int)m->from) == 16) {
        p->ep_sq = (m->from + m->to) / 2;
    }

    /* update castle rights */
    if (type == KING) {
        if (color == WHITE) p->castle_rights &= ~3;
        else                p->castle_rights &= ~12;
    }
    if (type == ROOK) {
        if (m->from == SQ(0,0)) p->castle_rights &= ~2;
        if (m->from == SQ(7,0)) p->castle_rights &= ~1;
        if (m->from == SQ(0,7)) p->castle_rights &= ~8;
        if (m->from == SQ(7,7)) p->castle_rights &= ~4;
    }
    /* rook captured on corner → lose rights */
    if (m->to == SQ(0,0)) p->castle_rights &= ~2;
    if (m->to == SQ(7,0)) p->castle_rights &= ~1;
    if (m->to == SQ(0,7)) p->castle_rights &= ~8;
    if (m->to == SQ(7,7)) p->castle_rights &= ~4;

    if (type == PAWN || m->captured) p->halfmove = 0;
    else                              p->halfmove++;

    if (color == BLACK) p->fullmove++;
    p->side ^= 1;
}

static int king_sq(const Position *p, int color) {
    Piece k = PIECE(KING, color);
    for (int i = 0; i < 64; i++)
        if (p->board[i] == k) return i;
    return -1;
}

static int in_check(const Position *p, int color) {
    int ksq = king_sq(p, color);
    if (ksq < 0) return 0;
    return sq_attacked(p, ksq, color^1);
}

/* Generate fully legal moves */
static int gen_legal(const Position *p, int sq_filter, Move *buf) {
    Move pseudo[MAX_MOVES];
    int  pcnt = 0;

    if (sq_filter >= 0) {
        gen_piece_moves(p, sq_filter, pseudo, &pcnt);
    } else {
        for (int s = 0; s < 64; s++) {
            if (p->board[s] && PCOLOR(p->board[s]) == p->side)
                gen_piece_moves(p, s, pseudo, &pcnt);
        }
    }

    int lcnt = 0;
    for (int i = 0; i < pcnt; i++) {
        Position tmp = *p;
        apply_move_internal(&tmp, &pseudo[i]);
        if (!in_check(&tmp, p->side))
            buf[lcnt++] = pseudo[i];
    }
    return lcnt;
}

/* ─────────────────────────────────────────────
   PGN NOTATION
───────────────────────────────────────────── */
static char piece_char(int type) {
    switch(type) {
        case KNIGHT: return 'N';
        case BISHOP: return 'B';
        case ROOK:   return 'R';
        case QUEEN:  return 'Q';
        case KING:   return 'K';
        default:     return 0;
    }
}

static void sq_to_alg(int sq, char *out) {
    out[0] = 'a' + FILE_OF(sq);
    out[1] = '1' + RANK_OF(sq);
    out[2] = 0;
}

static void move_to_pgn(const Position *before, const Move *m, char *out) {
    Piece piece = before->board[m->from];
    int   type  = PTYPE(piece);
    int   color = PCOLOR(piece);
    char  buf[32] = "";
    int   bi = 0;

    /* castling */
    if (m->castle == 1) { strcpy(out, "O-O");   goto suffix; }
    if (m->castle == 2) { strcpy(out, "O-O-O"); goto suffix; }

    /* piece letter */
    if (type != PAWN) buf[bi++] = piece_char(type);

    /* disambiguation */
    if (type != PAWN) {
        Move all[MAX_MOVES];
        int ac = gen_legal(before, -1, all);
        int amb_file = 0, amb_rank = 0, amb = 0;
        for (int i = 0; i < ac; i++) {
            if (all[i].to == m->to &&
                all[i].from != m->from &&
                PTYPE(before->board[all[i].from]) == type &&
                PCOLOR(before->board[all[i].from]) == color) {
                amb = 1;
                if (FILE_OF(all[i].from) == FILE_OF(m->from)) amb_rank = 1;
                if (RANK_OF(all[i].from) == RANK_OF(m->from)) amb_file = 1;
            }
        }
        if (amb) {
            if (!amb_file)      buf[bi++] = 'a' + FILE_OF(m->from);
            else if (!amb_rank) buf[bi++] = '1' + RANK_OF(m->from);
            else {
                buf[bi++] = 'a' + FILE_OF(m->from);
                buf[bi++] = '1' + RANK_OF(m->from);
            }
        }
    } else if (m->captured || m->ep) {
        buf[bi++] = 'a' + FILE_OF(m->from);
    }

    /* capture */
    if (m->captured || m->ep) buf[bi++] = 'x';

    /* destination */
    buf[bi++] = 'a' + FILE_OF(m->to);
    buf[bi++] = '1' + RANK_OF(m->to);
    buf[bi]   = 0;

    /* promotion */
    if (m->promo) {
        buf[bi++] = '=';
        buf[bi++] = piece_char(PTYPE(m->promo));
        buf[bi]   = 0;
    }

    strcpy(out, buf);

suffix:;
    /* check / checkmate */
    Position after = *before;
    apply_move_internal(&after, m);
    Move test[MAX_MOVES];
    int tc = gen_legal(&after, -1, test);
    if (in_check(&after, after.side)) {
        if (tc == 0) strcat(out, "#");
        else         strcat(out, "+");
    }
}

/* UCI coordinate notation */
static void move_to_uci(const Move *m, char *out) {
    out[0] = 'a' + FILE_OF(m->from);
    out[1] = '1' + RANK_OF(m->from);
    out[2] = 'a' + FILE_OF(m->to);
    out[3] = '1' + RANK_OF(m->to);
    if (m->promo) {
        char pc = tolower(piece_char(PTYPE(m->promo)));
        out[4] = pc;
        out[5] = 0;
    } else {
        out[4] = 0;
    }
}

/* Parse UCI move string into a Move, given current position */
static int parse_uci_move(const Position *p, const char *uci, Move *out) {
    if (strlen(uci) < 4) return 0;
    int ff = uci[0]-'a', fr = uci[1]-'1';
    int tf = uci[2]-'a', tr = uci[3]-'1';
    if (ff<0||ff>7||fr<0||fr>7||tf<0||tf>7||tr<0||tr>7) return 0;

    Move legal[MAX_MOVES];
    int lc = gen_legal(p, -1, legal);
    int promo_type = 0;
    if (uci[4]) {
        switch(tolower(uci[4])) {
            case 'n': promo_type = KNIGHT; break;
            case 'b': promo_type = BISHOP; break;
            case 'r': promo_type = ROOK;   break;
            case 'q': promo_type = QUEEN;  break;
        }
    }
    for (int i = 0; i < lc; i++) {
        Move *m = &legal[i];
        if (FILE_OF(m->from)==ff && RANK_OF(m->from)==fr &&
            FILE_OF(m->to)==tf   && RANK_OF(m->to)==tr) {
            if (promo_type == 0 || PTYPE(m->promo) == promo_type) {
                *out = *m;
                return 1;
            }
        }
    }
    return 0;
}

/* ─────────────────────────────────────────────
   FEN BUILDER (for sending to engine)
───────────────────────────────────────────── */
static void pos_to_fen(const Position *p, char *fen) {
    static const char piece_chars[2][7] =
        {{'.',  'P','N','B','R','Q','K'},
         {'.', 'p','n','b','r','q','k'}};
    int bi = 0;
    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            Piece pc = p->board[SQ(file,rank)];
            if (pc == EMPTY) { empty++; continue; }
            if (empty) { fen[bi++] = '0'+empty; empty=0; }
            fen[bi++] = piece_chars[PCOLOR(pc)][PTYPE(pc)];
        }
        if (empty) fen[bi++] = '0'+empty;
        if (rank > 0) fen[bi++] = '/';
    }
    fen[bi++] = ' ';
    fen[bi++] = (p->side==WHITE) ? 'w' : 'b';
    fen[bi++] = ' ';

    if (p->castle_rights == 0) { fen[bi++] = '-'; }
    else {
        if (p->castle_rights & 1) fen[bi++] = 'K';
        if (p->castle_rights & 2) fen[bi++] = 'Q';
        if (p->castle_rights & 4) fen[bi++] = 'k';
        if (p->castle_rights & 8) fen[bi++] = 'q';
    }
    fen[bi++] = ' ';
    if (p->ep_sq < 0) { fen[bi++] = '-'; }
    else {
        fen[bi++] = 'a' + FILE_OF(p->ep_sq);
        fen[bi++] = '1' + RANK_OF(p->ep_sq);
    }
    bi += sprintf(fen+bi, " %d %d", p->halfmove, p->fullmove);
    fen[bi] = 0;
}

/* ─────────────────────────────────────────────
   UCI ENGINE I/O
───────────────────────────────────────────── */
static void engine_write(const char *msg) {
    if (!engine.alive) return;
    write(engine.to_engine[1], msg, strlen(msg));
}

/* Non-blocking read of one line from engine */
static int engine_read_line(char *buf, int maxlen) {
    if (!engine.alive) return 0;
    fd_set fds; struct timeval tv = {0,0};
    FD_ZERO(&fds); FD_SET(engine.from_engine[0], &fds);
    if (select(engine.from_engine[0]+1, &fds, NULL, NULL, &tv) <= 0)
        return 0;
    int i = 0;
    char c;
    while (i < maxlen-1) {
        int n = read(engine.from_engine[0], &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = 0;
    return i > 0;
}

/* Poll engine for bestmove */
static void engine_poll(void) {
    if (!engine.alive || !engine_thinking) return;
    char line[512];
    while (engine_read_line(line, sizeof(line))) {
        if (strncmp(line, "bestmove", 8) == 0) {
            char *tok = strtok(line+9, " \t\r\n");
            if (tok && strcmp(tok,"(none)") != 0) {
                strncpy(engine.bestmove, tok, 7);
                engine.bestmove[7] = 0;
                engine.bestmove_ready = 1;
            }
            engine_thinking = 0;
        }
    }
}

static void engine_send_position(void) {
    if (!engine.alive) return;
    char cmd[4096];
    /* Build move list from history */
    if (history_len == 0) {
        engine_write("position startpos\n");
        return;
    }
    strcpy(cmd, "position startpos moves");
    for (int i = 0; i < history_len; i++) {
        char uci[8];
        move_to_uci(&history[i].move, uci);
        strcat(cmd, " ");
        strcat(cmd, uci);
    }
    strcat(cmd, "\n");
    engine_write(cmd);
}

static void engine_go(void) {
    if (!engine.alive) return;
    char cmd[128];
    engine_thinking = 1;
    engine.bestmove_ready = 0;
    if (engine.tc_mode == TC_TIME) {
        snprintf(cmd, sizeof(cmd), "go movetime %d\n", engine.tc_time_ms);
    } else if (engine.tc_mode == TC_DEPTH) {
        snprintf(cmd, sizeof(cmd), "go depth %d\n", engine.tc_depth);
    } else {
        snprintf(cmd, sizeof(cmd), "go nodes %ld\n", engine.tc_nodes);
    }
    engine_write(cmd);
}

static int engine_start(const char *path) {
    if (pipe(engine.to_engine) < 0)   return 0;
    if (pipe(engine.from_engine) < 0) return 0;

    engine.pid = fork();
    if (engine.pid < 0) return 0;

    if (engine.pid == 0) {
        /* child: engine process */
        dup2(engine.to_engine[0],   STDIN_FILENO);
        dup2(engine.from_engine[1], STDOUT_FILENO);
        close(engine.to_engine[1]);
        close(engine.from_engine[0]);
        execlp(path, path, NULL);
        exit(1);
    }
    close(engine.to_engine[0]);
    close(engine.from_engine[1]);

    /* set non-blocking */
    int flags = fcntl(engine.from_engine[0], F_GETFL);
    fcntl(engine.from_engine[0], F_SETFL, flags | O_NONBLOCK);

    engine.alive = 1;
    engine.tc_mode    = TC_TIME;
    engine.tc_time_ms = 1000;
    engine.tc_depth   = 6;
    engine.tc_nodes   = 100000;

    engine_write("uci\n");
    usleep(200000);

    /* Read engine name */
    char line[512];
    int  tries = 20;
    while (tries-- > 0) {
        usleep(50000);
        while (engine_read_line(line, sizeof(line))) {
            if (strncmp(line,"id name",7)==0) {
                strncpy(engine.name, line+8, 63);
            }
            if (strcmp(line,"uciok")==0) goto done;
        }
    }
done:
    engine_write("isready\n");
    tries = 20;
    while (tries-- > 0) {
        usleep(50000);
        while (engine_read_line(line, sizeof(line)))
            if (strcmp(line,"readyok")==0) goto ready;
    }
ready:
    return 1;
}

static void engine_stop(void) {
    if (!engine.alive) return;
    engine_write("quit\n");
    usleep(100000);
    kill(engine.pid, SIGTERM);
    waitpid(engine.pid, NULL, WNOHANG);
    engine.alive = 0;
}

/* ─────────────────────────────────────────────
   APPLY MOVE TO GAME
───────────────────────────────────────────── */
static void do_move(const Move *m) {
    if (history_len >= MAX_HISTORY) return;

    HistEntry *he = &history[history_len];
    he->pos_before = pos;
    he->move       = *m;
    move_to_pgn(&pos, m, he->pgn);
    history_len++;

    apply_move_internal(&pos, m);

    /* check for game over */
    Move legal[MAX_MOVES];
    int lc = gen_legal(&pos, -1, legal);
    if (lc == 0) {
        game_over = 1;
        if (in_check(&pos, pos.side)) {
            snprintf(game_result, sizeof(game_result),
                     "%s wins by checkmate",
                     pos.side == WHITE ? "Black" : "White");
        } else {
            strcpy(game_result, "Draw by stalemate");
        }
    }
    if (pos.halfmove >= 100) {
        game_over = 1;
        strcpy(game_result, "Draw by 50-move rule");
    }
}

static void undo_move(void) {
    if (history_len == 0) {
        snprintf(status_msg, sizeof(status_msg), "Nothing to undo.");
        return;
    }
    if (engine_thinking) {
        engine_write("stop\n");
        usleep(100000);
        engine_thinking = 0;
    }
    history_len--;
    pos = history[history_len].pos_before;
    game_over = 0;
    strcpy(game_result, "");
    snprintf(status_msg, sizeof(status_msg), "Move undone.");
    selected_sq = -1;
    gui_mode = MODE_NAVIGATE;
    memset(is_legal_target, 0, sizeof(is_legal_target));
}

/* ─────────────────────────────────────────────
   DRAWING
───────────────────────────────────────────── */
static void sq_bg(int sq) {
    int light = (FILE_OF(sq) + RANK_OF(sq)) % 2 == 1;

    /* check  */
    if (!game_over && PTYPE(pos.board[sq]) == KING &&
        PCOLOR(pos.board[sq]) == pos.side &&
        in_check(&pos, pos.side)) {
        printf(BG_CHECK);
        return;
    }
    /* last move highlight */
    if (history_len > 0) {
        int lf = history[history_len-1].move.from;
        int lt = history[history_len-1].move.to;
        if (sq == lf || sq == lt) {
            printf(BG_LASTMOVE);
            return;
        }
    }
    /* selected */
    if (sq == selected_sq) { printf(BG_SELECTED); return; }
    /* legal target */
    if (is_legal_target[sq]) { printf(BG_LEGAL); return; }
    /* cursor */
    if (sq == cursor_sq) { printf(BG_CURSOR); return; }

    printf("%s", light ? BG_LIGHT : BG_DARK);
}

static void draw_board(void) {
    printf("\n");
    for (int display_rank = 7; display_rank >= 0; display_rank--) {
        int rank = flip_board ? (7 - display_rank) : display_rank;
        printf("  %s%c%s  ", FG_LABEL, '1'+rank, ANSI_RESET);

        for (int display_file = 0; display_file < 8; display_file++) {
            int file = flip_board ? (7 - display_file) : display_file;
            int sq   = SQ(file, rank);
            Piece pc = pos.board[sq];

            sq_bg(sq);

            if (pc != EMPTY) {
                int color = PCOLOR(pc);
                int type  = PTYPE(pc);
                printf("%s%s%s",
                       color==WHITE ? FG_WHITE_PIECE : FG_BLACK_PIECE,
                       PIECE_GLYPHS[color][type],
                       ANSI_RESET);
            } else {
                /* dot for legal targets */
                if (is_legal_target[sq]) {
                    printf(BG_LEGAL);
                    printf(FG_WHITE_PIECE "· " ANSI_RESET);
                } else {
                    printf("%s  %s", sq_bg(sq), ANSI_RESET);
                    /* call sq_bg again just for the spaces */
                    sq_bg(sq);
                    printf("  " ANSI_RESET);
                }
            }
        }
        printf("\n");
    }
    printf("\n     ");
    for (int f = 0; f < 8; f++) {
        int file = flip_board ? (7-f) : f;
        printf("%s%c%s ", FG_LABEL, 'a'+file, ANSI_RESET);
    }
    printf("\n");
}

static void draw_sidebar(void) {
    /* Move to column 28 for sidebar (approximate) */
    char sq_str[4];
    sq_to_alg(cursor_sq, sq_str);

    printf("\n");
    printf(ANSI_BOLD FG_CYAN "  ╔══════════════════════╗\n" ANSI_RESET);
    printf(ANSI_BOLD FG_CYAN "  ║  " FG_WHITE "Terminal Chess GUI" FG_CYAN "   ║\n" ANSI_RESET);
    printf(ANSI_BOLD FG_CYAN "  ╚══════════════════════╝\n" ANSI_RESET);

    /* Engine info */
    if (engine.alive) {
        printf("  " FG_GREEN "Engine: " FG_WHITE "%s\n" ANSI_RESET,
               engine.name[0] ? engine.name : "UCI Engine");
        const char *tcstr =
            engine.tc_mode == TC_TIME  ? "Time" :
            engine.tc_mode == TC_DEPTH ? "Depth" : "Nodes";
        if (engine.tc_mode == TC_TIME)
            printf("  " FG_YELLOW "TC: " FG_WHITE "%s %dms\n" ANSI_RESET,
                   tcstr, engine.tc_time_ms);
        else if (engine.tc_mode == TC_DEPTH)
            printf("  " FG_YELLOW "TC: " FG_WHITE "%s %d\n" ANSI_RESET,
                   tcstr, engine.tc_depth);
        else
            printf("  " FG_YELLOW "TC: " FG_WHITE "%s %ldN\n" ANSI_RESET,
                   tcstr, engine.tc_nodes);
        if (engine_thinking)
            printf("  " FG_MAGENTA "Engine thinking...\n" ANSI_RESET);
    } else {
        printf("  " FG_GRAY "No engine loaded\n" ANSI_RESET);
        printf("  " FG_GRAY "Run: ./chess_gui <path>\n" ANSI_RESET);
    }

    /* Turn */
    printf("\n");
    if (!game_over) {
        printf("  " FG_CYAN "Turn: " ANSI_BOLD "%s\n" ANSI_RESET,
               pos.side == WHITE ? "White ♔" : "Black ♚");
        printf("  " FG_GRAY "Move #%d\n" ANSI_RESET, pos.fullmove);
        printf("  " FG_GRAY "Cursor: %s\n" ANSI_RESET, sq_str);
        if (in_check(&pos, pos.side))
            printf("  " FG_RED ANSI_BOLD "CHECK!\n" ANSI_RESET);
    } else {
        printf("  " FG_RED ANSI_BOLD "GAME OVER\n" ANSI_RESET);
        printf("  " FG_YELLOW "%s\n" ANSI_RESET, game_result);
    }

    /* Last 5 moves in PGN */
    printf("\n  " FG_CYAN ANSI_BOLD "Recent Moves:\n" ANSI_RESET);
    int start = history_len - 5;
    if (start < 0) start = 0;
    for (int i = start; i < history_len; i++) {
        HistEntry *he = &history[i];
        int mn = he->pos_before.fullmove;
        int side = he->pos_before.side;
        if (side == WHITE)
            printf("  " FG_WHITE "%d. " FG_YELLOW "%s\n" ANSI_RESET,
                   mn, he->pgn);
        else
            printf("       " FG_GRAY "%s\n" ANSI_RESET, he->pgn);
    }
    if (history_len == 0)
        printf("  " FG_GRAY "(none)\n" ANSI_RESET);

    /* Controls */
    printf("\n");
    printf("  " FG_CYAN ANSI_BOLD "Controls:\n" ANSI_RESET);
    printf("  " FG_GRAY "Arrow/WASD : move cursor\n" ANSI_RESET);
    printf("  " FG_GRAY "Enter/Space: select/move\n" ANSI_RESET);
    printf("  " FG_GRAY "U          : undo move\n" ANSI_RESET);
    printf("  " FG_GRAY "F          : flip board\n" ANSI_RESET);
    printf("  " FG_GRAY "T          : time controls\n" ANSI_RESET);
    printf("  " FG_GRAY "ESC        : deselect/back\n" ANSI_RESET);
    printf("  " FG_GRAY "Q          : quit\n" ANSI_RESET);

    /* Status */
    if (status_msg[0]) {
        printf("\n  " FG_YELLOW "» %s\n" ANSI_RESET, status_msg);
    }
}

/* Promotion menu */
static void draw_promo_menu(void) {
    int color = pos.side ^ 1; /* side that just moved */
    printf("\n");
    printf("  " FG_CYAN ANSI_BOLD "Promote pawn to:\n" ANSI_RESET);
    printf("  " FG_WHITE "  Q) Queen   %s\n" ANSI_RESET,
           PIECE_GLYPHS[color][QUEEN]);
    printf("  " FG_WHITE "  R) Rook    %s\n" ANSI_RESET,
           PIECE_GLYPHS[color][ROOK]);
    printf("  " FG_WHITE "  B) Bishop  %s\n" ANSI_RESET,
           PIECE_GLYPHS[color][BISHOP]);
    printf("  " FG_WHITE "  N) Knight  %s\n" ANSI_RESET,
           PIECE_GLYPHS[color][KNIGHT]);
}

/* Time control menu */
static void draw_tc_menu(void) {
    printf("\n");
    printf("  " FG_CYAN ANSI_BOLD "╔════ Time Controls ═════╗\n" ANSI_RESET);
    printf("  " FG_CYAN          "║                        ║\n" ANSI_RESET);
    printf("  " FG_CYAN          "║  " FG_WHITE "1) Time per move"
           FG_CYAN "      ║\n" ANSI_RESET);
    printf("  " FG_CYAN          "║  " FG_WHITE "2) Fixed depth "
           FG_CYAN "       ║\n" ANSI_RESET);
    printf("  " FG_CYAN          "║  " FG_WHITE "3) Node count  "
           FG_CYAN "       ║\n" ANSI_RESET);
    printf("  " FG_CYAN          "║  " FG_WHITE "ESC) Cancel    "
           FG_CYAN "       ║\n" ANSI_RESET);
    printf("  " FG_CYAN          "╚════════════════════════╝\n" ANSI_RESET);

    printf("\n  Current: ");
    if (engine.tc_mode == TC_TIME)
        printf(FG_GREEN "%d ms/move\n" ANSI_RESET, engine.tc_time_ms);
    else if (engine.tc_mode == TC_DEPTH)
        printf(FG_GREEN "Depth %d\n" ANSI_RESET, engine.tc_depth);
    else
        printf(FG_GREEN "%ld nodes\n" ANSI_RESET, engine.tc_nodes);
}

static void redraw(void) {
    clear_screen();
    hide_cursor();

    /* Title bar */
    printf(ANSI_BOLD FG_CYAN
           "\n  ♔ Terminal Chess  ♚\n"
           ANSI_RESET);

    draw_board();
    draw_sidebar();

    if (gui_mode == MODE_PROMO)  draw_promo_menu();
    if (gui_mode == MODE_MENU)   draw_tc_menu();

    fflush(stdout);
}

/* ─────────────────────────────────────────────
   INPUT HANDLING
───────────────────────────────────────────── */
static void cursor_move(int df, int dr) {
    int f = FILE_OF(cursor_sq) + df;
    int r = RANK_OF(cursor_sq) + dr;
    if (f < 0) f = 0; if (f > 7) f = 7;
    if (r < 0) r = 0; if (r > 7) r = 7;
    cursor_sq = SQ(f, r);
}

/* Read TC value from terminal (raw → cooked temporarily) */
static int read_tc_value(const char *prompt, long *val) {
    term_restore();
    show_cursor();
    printf("\n  %s: ", prompt);
    fflush(stdout);
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) {
        term_raw(); hide_cursor();
        return 0;
    }
    *val = atol(buf);
    term_raw(); hide_cursor();
    return (*val > 0);
}

static void handle_tc_menu(int ch) {
    long val;
    switch(ch) {
        case '1':
            if (read_tc_value("Enter time in ms (e.g. 1000)", &val)) {
                engine.tc_mode    = TC_TIME;
                engine.tc_time_ms = (int)val;
                snprintf(status_msg, sizeof(status_msg),
                         "TC: %dms per move", engine.tc_time_ms);
            }
            gui_mode = MODE_NAVIGATE;
            break;
        case '2':
            if (read_tc_value("Enter search depth (e.g. 8)", &val)) {
                engine.tc_mode  = TC_DEPTH;
                engine.tc_depth = (int)val;
                snprintf(status_msg, sizeof(status_msg),
                         "TC: depth %d", engine.tc_depth);
            }
            gui_mode = MODE_NAVIGATE;
            break;
        case '3':
            if (read_tc_value("Enter node count (e.g. 500000)", &val)) {
                engine.tc_mode  = TC_NODES;
                engine.tc_nodes = val;
                snprintf(status_msg, sizeof(status_msg),
                         "TC: %ld nodes", engine.tc_nodes);
            }
            gui_mode = MODE_NAVIGATE;
            break;
        case 27: /* ESC */
            gui_mode = MODE_NAVIGATE;
            break;
    }
}

static void handle_promo(int ch) {
    int color = pos.side; /* side to move (before move applied) */
    int ptype = 0;
    switch(tolower(ch)) {
        case 'q': ptype = QUEEN;  break;
        case 'r': ptype = ROOK;   break;
        case 'b': ptype = BISHOP; break;
        case 'n': ptype = KNIGHT; break;
        default: return;
    }
    /* find the matching legal move */
    Move legal[MAX_MOVES];
    int lc = gen_legal(&history[history_len > 0 ? history_len-1 : 0].pos_before,
                       -1, legal);
    /* We need legal moves for pos (before promo applied) */
    Position *pbefore = (history_len > 0) ?
                        &history[history_len-1].pos_before : &pos;
    /* Actually we stored pos before in promo_pending already, just apply */
    promo_pending.promo = PIECE(ptype, color);

    do_move(&promo_pending);
    gui_mode = MODE_NAVIGATE;
    selected_sq = -1;
    memset(is_legal_target, 0, sizeof(is_legal_target));
    status_msg[0] = 0;

    /* Trigger engine if it's engine's turn */
    if (engine.alive && !game_over && pos.side != player_color) {
        engine_send_position();
        engine_go();
    }
}

static void try_select_or_move(void) {
    if (game_over) return;

    /* not player's turn (engine game) */
    if (engine.alive && pos.side != player_color && !engine_thinking) {
        snprintf(status_msg, sizeof(status_msg), "Wait for engine move.");
        return;
    }
    if (engine_thinking) {
        snprintf(status_msg, sizeof(status_msg), "Engine is thinking...");
        return;
    }

    int sq = cursor_sq;

    if (gui_mode == MODE_NAVIGATE) {
        /* Select a piece */
        Piece pc = pos.board[sq];
        if (pc == EMPTY || PCOLOR(pc) != pos.side) {
            snprintf(status_msg, sizeof(status_msg),
                     "No %s piece there.",
                     pos.side == WHITE ? "white" : "black");
            return;
        }
        selected_sq = sq;
        gui_mode = MODE_SELECTED;

        /* Generate legal moves for this piece */
        legal_count = gen_legal(&pos, sq, legal_buf);
        memset(is_legal_target, 0, sizeof(is_legal_target));
        for (int i = 0; i < legal_count; i++)
            is_legal_target[legal_buf[i].to] = 1;

        if (legal_count == 0) {
            snprintf(status_msg, sizeof(status_msg), "No legal moves for that piece.");
            selected_sq = -1;
            gui_mode = MODE_NAVIGATE;
        } else {
            status_msg[0] = 0;
        }

    } else if (gui_mode == MODE_SELECTED) {
        if (sq == selected_sq) {
            /* Deselect */
            selected_sq = -1;
            gui_mode = MODE_NAVIGATE;
            memset(is_legal_target, 0, sizeof(is_legal_target));
            return;
        }
        /* Check if target is legal */
        if (!is_legal_target[sq]) {
            /* Maybe re-select another piece */
            Piece pc = pos.board[sq];
            if (pc != EMPTY && PCOLOR(pc) == pos.side) {
                selected_sq = sq;
                legal_count = gen_legal(&pos, sq, legal_buf);
                memset(is_legal_target, 0, sizeof(is_legal_target));
                for (int i = 0; i < legal_count; i++)
                    is_legal_target[legal_buf[i].to] = 1;
                return;
            }
            snprintf(status_msg, sizeof(status_msg), "Illegal move.");
            return;
        }

        /* Find the move */
        Move chosen = {0};
        int found = 0;
        /* If multiple moves to target (promo), use first; handle promo */
        int promo_needed = 0;
        for (int i = 0; i < legal_count; i++) {
            if (legal_buf[i].to == sq) {
                if (legal_buf[i].promo) { promo_needed = 1; }
                else { chosen = legal_buf[i]; found = 1; break; }
            }
        }

        if (promo_needed) {
            /* Save move shell, ask for promo choice */
            for (int i = 0; i < legal_count; i++)
                if (legal_buf[i].to == sq) { promo_pending = legal_buf[i]; break; }
            gui_mode = MODE_PROMO;
            selected_sq = -1;
            memset(is_legal_target, 0, sizeof(is_legal_target));
            status_msg[0] = 0;
            return;
        }

        if (!found) {
            snprintf(status_msg, sizeof(status_msg), "Move not found (bug).");
            return;
        }

        do_move(&chosen);
        selected_sq = -1;
        gui_mode = MODE_NAVIGATE;
        memset(is_legal_target, 0, sizeof(is_legal_target));
        status_msg[0] = 0;

        /* Trigger engine */
        if (engine.alive && !game_over && pos.side != player_color) {
            engine_send_position();
            engine_go();
        }
    }
}

/* ─────────────────────────────────────────────
   MAIN INPUT LOOP
───────────────────────────────────────────── */
static void handle_key(unsigned char *buf, int len) {
    if (len == 0) return;

    /* Time control menu */
    if (gui_mode == MODE_MENU) {
        handle_tc_menu(buf[0]);
        return;
    }

    /* Promotion menu */
    if (gui_mode == MODE_PROMO) {
        handle_promo(buf[0]);
        return;
    }

    /* ESC sequences (arrow keys) */
    if (len >= 3 && buf[0] == 27 && buf[1] == '[') {
        if (flip_board) {
            switch(buf[2]) {
                case 'A': cursor_move(0, -1); return; /* Up    → rank-- */
                case 'B': cursor_move(0,  1); return; /* Down  → rank++ */
                case 'C': cursor_move(-1, 0); return; /* Right → file-- */
                case 'D': cursor_move( 1, 0); return; /* Left  → file++ */
            }
        } else {
            switch(buf[2]) {
                case 'A': cursor_move(0,  1); return; /* Up    */
                case 'B': cursor_move(0, -1); return; /* Down  */
                case 'C': cursor_move( 1, 0); return; /* Right */
                case 'D': cursor_move(-1, 0); return; /* Left  */
            }
        }
    }

    if (buf[0] == 27) { /* lone ESC */
        if (gui_mode == MODE_SELECTED) {
            selected_sq = -1;
            gui_mode = MODE_NAVIGATE;
            memset(is_legal_target, 0, sizeof(is_legal_target));
            status_msg[0] = 0;
        }
        return;
    }

    switch(buf[0]) {
        /* WASD / hjkl navigation */
        case 'w': case 'W':
            cursor_move(0, flip_board ? -1 :  1); break;
        case 's': case 'S':
            cursor_move(0, flip_board ?  1 : -1); break;
        case 'd': case 'D':
            cursor_move(flip_board ? -1 :  1, 0); break;
        case 'a': case 'A':
            cursor_move(flip_board ?  1 : -1, 0); break;

        case 'h': cursor_move(-1, 0); break;
        case 'l': cursor_move( 1, 0); break;
        case 'k': cursor_move(0,  1); break;
        case 'j': cursor_move(0, -1); break;

        /* Select / move */
        case '\r': case '\n': case ' ':
            try_select_or_move();
            break;

        /* Undo */
        case 'u': case 'U':
            undo_move();
            /* if engine had moved last, undo twice so human plays */
            if (engine.alive && history_len > 0 &&
                history[history_len-1].pos_before.side == player_color) {
                undo_move();
            }
            break;

        /* Flip board */
        case 'f': case 'F':
            flip_board ^= 1;
            break;

        /* Time control menu */
        case 't': case 'T':
            if (engine.alive) gui_mode = MODE_MENU;
            else snprintf(status_msg, sizeof(status_msg),
                          "No engine loaded.");
            break;

        /* Toggle player color (human side) */
        case 'c': case 'C':
            player_color ^= 1;
            snprintf(status_msg, sizeof(status_msg),
                     "You play %s", player_color==WHITE ? "White" : "Black");
            break;

        /* New game */
        case 'n': case 'N':
            if (engine_thinking) {
                engine_write("stop\n");
                usleep(100000);
                engine_thinking = 0;
            }
            pos_init(&pos);
            history_len = 0;
            game_over = 0;
            game_result[0] = 0;
            selected_sq = -1;
            gui_mode = MODE_NAVIGATE;
            memset(is_legal_target, 0, sizeof(is_legal_target));
            snprintf(status_msg, sizeof(status_msg), "New game started.");
            /* If engine plays white */
            if (engine.alive && pos.side != player_color) {
                engine_send_position();
                engine_go();
            }
            break;

        /* Quit */
        case 'q': case 'Q':
            engine_stop();
            term_restore();
            show_cursor();
            clear_screen();
            printf("Thanks for playing!\n");
            exit(0);
    }
}

/* ─────────────────────────────────────────────
   MAIN
───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    /* Init */
    pos_init(&pos);
    cursor_sq = SQ(4, 0); /* e1 */

    /* Start engine if path given */
    if (argc >= 2) {
        printf("Loading engine: %s\n", argv[1]);
        fflush(stdout);
        if (!engine_start(argv[1])) {
            fprintf(stderr, "Failed to start engine.\n");
        } else {
            snprintf(status_msg, sizeof(status_msg),
                     "Engine ready: %s",
                     engine.name[0] ? engine.name : argv[1]);
            /* Engine plays black by default; if black to move first wouldn't
               make sense, user can press C to swap.
               If engine plays black, human (white) goes first — fine. */
        }
    }

    term_raw();
    hide_cursor();

    /* If engine plays white */
    if (engine.alive && pos.side != player_color) {
        engine_send_position();
        engine_go();
    }

    /* Main loop */
    while (1) {
        /* Poll engine for bestmove */
        engine_poll();

        if (engine.bestmove_ready) {
            engine.bestmove_ready = 0;
            if (!game_over) {
                Move m;
                if (parse_uci_move(&pos, engine.bestmove, &m)) {
                    do_move(&m);
                    snprintf(status_msg, sizeof(status_msg),
                             "Engine played: %s", engine.bestmove);
                } else {
                    snprintf(status_msg, sizeof(status_msg),
                             "Engine illegal move: %s", engine.bestmove);
                }
            }
        }

        redraw();

        /* Non-blocking read of keyboard */
        unsigned char buf[8];
        int len = read(STDIN_FILENO, buf, sizeof(buf));
        if (len > 0) {
            /* Clear status on new input */
            status_msg[0] = 0;
            handle_key(buf, len);
        }

        usleep(50000); /* 50ms tick */
    }

    return 0;
}
