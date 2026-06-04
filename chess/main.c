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
#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

/* ─── ANSI Colors & Box Drawing ─────────────────────────────────────────── */
#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define DIM         "\033[2m"

/* True-color background */
#define BG_LIGHT    "\033[48;2;240;217;181m"   /* light square  */
#define BG_DARK     "\033[48;2;181;136;99m"    /* dark square   */
#define BG_SELECT   "\033[48;2;106;168;79m"    /* selected      */
#define BG_LEGAL    "\033[48;2;100;180;100m"   /* legal target  */
#define BG_LAST     "\033[48;2;205;210;106m"   /* last move     */
#define BG_CHECK    "\033[48;2;220;60;60m"     /* king in check */
#define BG_CURSOR   "\033[48;2;80;130;220m"    /* cursor        */

/* Foreground */
#define FG_WHITE_P  "\033[38;2;255;255;255m"
#define FG_BLACK_P  "\033[38;2;20;20;20m"
#define FG_YELLOW   "\033[38;2;255;220;50m"
#define FG_CYAN     "\033[38;2;80;220;220m"
#define FG_GREEN    "\033[38;2;80;220;80m"
#define FG_RED      "\033[38;2;220;80;80m"
#define FG_MAGENTA  "\033[38;2;220;80;220m"
#define FG_GRAY     "\033[38;2;160;160;160m"
#define FG_WHITE    "\033[38;2;230;230;230m"

/* Box drawing */
#define TL "╔" 
#define TR "╗"
#define BL "╚"
#define BR "╝"
#define HZ "═"
#define VT "║"
#define TM "╦"
#define BM "╩"
#define LM "╠"
#define RM "╣"
#define CR "╬"
#define SH "─"
#define SV "│"

/* ─── Chess Constants ────────────────────────────────────────────────────── */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  8
#define BLACK  16

#define W_PAWN   (WHITE|PAWN)
#define W_KNIGHT (WHITE|KNIGHT)
#define W_BISHOP (WHITE|BISHOP)
#define W_ROOK   (WHITE|ROOK)
#define W_QUEEN  (WHITE|QUEEN)
#define W_KING   (WHITE|KING)
#define B_PAWN   (BLACK|PAWN)
#define B_KNIGHT (BLACK|KNIGHT)
#define B_BISHOP (BLACK|BISHOP)
#define B_ROOK   (BLACK|ROOK)
#define B_QUEEN  (BLACK|QUEEN)
#define B_KING   (BLACK|KING)

#define PIECE_TYPE(p) ((p)&7)
#define PIECE_COLOR(p) ((p)&24)
#define IS_WHITE(p) ((p)&WHITE)
#define IS_BLACK(p) ((p)&BLACK)

/* Castling flags */
#define CASTLE_WK 1
#define CASTLE_WQ 2
#define CASTLE_BK 4
#define CASTLE_BQ 8

#define MAX_MOVES    512
#define MAX_HISTORY  512
#define MAX_PGN_LEN  16384
#define ENGINE_BUF   4096

/* ─── Structures ─────────────────────────────────────────────────────────── */
typedef struct {
    int from, to;
    int piece, captured;
    int promotion;
    int castle;          /* 0=none,1=WK,2=WQ,3=BK,4=BQ */
    int ep_square;       /* en passant square before move */
    int ep_capture;      /* was it an ep capture? */
    int castle_rights;   /* saved castle rights */
    int halfmove;        /* saved halfmove clock */
} Move;

typedef struct {
    int board[64];
    int side;            /* WHITE or BLACK to move */
    int castle_rights;
    int ep_square;       /* -1 if none */
    int halfmove;
    int fullmove;
    Move history[MAX_HISTORY];
    int hist_count;
} GameState;

typedef struct {
    int from, to;
    int promotion;
} UCIMove;

/* ─── Engine Process ─────────────────────────────────────────────────────── */
typedef struct {
    pid_t pid;
    int   in_fd;   /* we write here  */
    int   out_fd;  /* we read here   */
    char  name[128];
    int   ready;
    int   active;
} Engine;

/* ─── UI State ───────────────────────────────────────────────────────────── */
typedef struct {
    int cursor_sq;      /* 0-63 */
    int selected_sq;    /* -1 if none */
    int legal[64];      /* legal target squares from selected */
    int legal_count;
    char status[256];
    char engine_status[256];
    int  human_color;   /* WHITE or BLACK (or 0 = both sides human) */
    int  game_over;
    char result[16];
    /* Engine settings */
    int  depth_limit;
    int  nodes_limit;
    int  time_limit_ms;
    /* PGN */
    char pgn[MAX_PGN_LEN];
    int  pgn_move_num;
    /* Last move highlight */
    int  last_from, last_to;
    /* Screen buffer */
    char screen_buf[65536];
    int  sbuf_pos;
} UIState;

/* ─── Globals ────────────────────────────────────────────────────────────── */
static GameState G;
static UIState   UI;
static Engine    Eng;
static struct termios orig_termios;

/* Unicode chess pieces (double-width safe with space) */
static const char *PIECE_GLYPH[2][7] = {
    /* WHITE */ { " ", "♙", "♘", "♗", "♖", "♕", "♔" },
    /* BLACK */ { " ", "♟", "♞", "♝", "♜", "♛", "♚" }
};

/* ─── Terminal Raw Mode ──────────────────────────────────────────────────── */
static void term_raw(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\033[0m\n");
    fflush(stdout);
}

static void cleanup(void) {
    term_restore();
    if (Eng.active) {
        write(Eng.in_fd, "quit\n", 5);
        close(Eng.in_fd);
        close(Eng.out_fd);
    }
}

static void sig_handler(int s) {
    (void)s;
    cleanup();
    exit(0);
}

/* ─── Screen Buffer ──────────────────────────────────────────────────────── */
static void sb_reset(void) { UI.sbuf_pos = 0; UI.screen_buf[0] = 0; }
static void sb_print(const char *s) {
    int l = strlen(s);
    if (UI.sbuf_pos + l < (int)sizeof(UI.screen_buf)-1) {
        memcpy(UI.screen_buf + UI.sbuf_pos, s, l);
        UI.sbuf_pos += l;
        UI.screen_buf[UI.sbuf_pos] = 0;
    }
}
static void sb_printf(const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_print(tmp);
}
static void sb_flush(void) {
    write(STDOUT_FILENO, UI.screen_buf, UI.sbuf_pos);
    sb_reset();
}

/* ─── Board Helpers ──────────────────────────────────────────────────────── */
static int sq(int r, int c) { return r*8+c; }
static int sq_rank(int s)   { return s/8; }
static int sq_file(int s)   { return s%8; }

static int sq_from_name(const char *n) {
    if (!n || !n[0] || !n[1]) return -1;
    int f = n[0]-'a', r = n[1]-'1';
    if (f<0||f>7||r<0||r>7) return -1;
    return sq(r,f);
}

static void sq_name(int s, char *buf) {
    buf[0] = 'a'+sq_file(s);
    buf[1] = '1'+sq_rank(s);
    buf[2] = 0;
}

/* ─── Move Generation ────────────────────────────────────────────────────── */
static int opp(int color) { return color==WHITE?BLACK:WHITE; }

/* Check if square is attacked by 'attacker_color' */
static int is_attacked(const GameState *g, int s, int attacker_color) {
    int r = sq_rank(s), f = sq_file(s);
    int acolor = attacker_color;

    /* Pawns */
    int pdr = (acolor==WHITE)?-1:1;  /* direction attacker's pawn attacks FROM */
    for (int df=-1; df<=1; df+=2) {
        int ar = r+pdr, af = f+df;
        if (ar>=0&&ar<8&&af>=0&&af<8) {
            int p = g->board[sq(ar,af)];
            if (PIECE_COLOR(p)==acolor && PIECE_TYPE(p)==PAWN) return 1;
        }
    }

    /* Knights */
    int knd[][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i=0;i<8;i++) {
        int ar=r+knd[i][0], af=f+knd[i][1];
        if (ar>=0&&ar<8&&af>=0&&af<8) {
            int p=g->board[sq(ar,af)];
            if (PIECE_COLOR(p)==acolor&&PIECE_TYPE(p)==KNIGHT) return 1;
        }
    }

    /* Rook/Queen (straight) */
    int dirs[][2] = {{0,1},{0,-1},{1,0},{-1,0}};
    for (int d=0;d<4;d++) {
        int ar=r+dirs[d][0], af=f+dirs[d][1];
        while (ar>=0&&ar<8&&af>=0&&af<8) {
            int p=g->board[sq(ar,af)];
            if (p!=EMPTY) {
                if (PIECE_COLOR(p)==acolor &&
                    (PIECE_TYPE(p)==ROOK||PIECE_TYPE(p)==QUEEN)) return 1;
                break;
            }
            ar+=dirs[d][0]; af+=dirs[d][1];
        }
    }

    /* Bishop/Queen (diagonal) */
    int bdirs[][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int d=0;d<4;d++) {
        int ar=r+bdirs[d][0], af=f+bdirs[d][1];
        while (ar>=0&&ar<8&&af>=0&&af<8) {
            int p=g->board[sq(ar,af)];
            if (p!=EMPTY) {
                if (PIECE_COLOR(p)==acolor &&
                    (PIECE_TYPE(p)==BISHOP||PIECE_TYPE(p)==QUEEN)) return 1;
                break;
            }
            ar+=bdirs[d][0]; af+=bdirs[d][1];
        }
    }

    /* King */
    for (int dr=-1;dr<=1;dr++) for (int df=-1;df<=1;df++) {
        if (!dr&&!df) continue;
        int ar=r+dr, af=f+df;
        if (ar>=0&&ar<8&&af>=0&&af<8) {
            int p=g->board[sq(ar,af)];
            if (PIECE_COLOR(p)==acolor&&PIECE_TYPE(p)==KING) return 1;
        }
    }
    return 0;
}

static int find_king(const GameState *g, int color) {
    for (int i=0;i<64;i++)
        if (g->board[i]==(color|KING)) return i;
    return -1;
}

static int in_check(const GameState *g, int color) {
    int ks = find_king(g, color);
    if (ks<0) return 0;
    return is_attacked(g, ks, opp(color));
}

/* Apply move to a copy of state (no legality check here) */
static void apply_move_raw(GameState *g, Move *m) {
    g->board[m->to]   = m->piece;
    g->board[m->from] = EMPTY;
    if (m->promotion) g->board[m->to] = PIECE_COLOR(m->piece)|m->promotion;
    if (m->ep_capture) {
        int ep_r = sq_rank(m->to) + (IS_WHITE(m->piece)?-1:1);
        g->board[sq(ep_r, sq_file(m->to))] = EMPTY;
    }
    switch(m->castle) {
        case 1: g->board[sq(0,5)]=W_ROOK; g->board[sq(0,7)]=EMPTY; break; /* WK */
        case 2: g->board[sq(0,3)]=W_ROOK; g->board[sq(0,0)]=EMPTY; break; /* WQ */
        case 3: g->board[sq(7,5)]=B_ROOK; g->board[sq(7,7)]=EMPTY; break; /* BK */
        case 4: g->board[sq(7,3)]=B_ROOK; g->board[sq(7,0)]=EMPTY; break; /* BQ */
    }
}

/* Generate pseudo-legal moves for a piece at 'from' */
static int gen_piece_moves(const GameState *g, int from, Move *moves, int count) {
    int piece = g->board[from];
    int ptype = PIECE_TYPE(piece);
    int pcolor = PIECE_COLOR(piece);
    int r = sq_rank(from), f = sq_file(from);

    if (!piece || !pcolor) return count;

    #define ADD_MOVE(T, CAP, PROM, CAST, EPC) do { \
        moves[count].from=(from); moves[count].to=(T); \
        moves[count].piece=piece; moves[count].captured=(CAP); \
        moves[count].promotion=(PROM); moves[count].castle=(CAST); \
        moves[count].ep_capture=(EPC); \
        moves[count].ep_square=g->ep_square; \
        moves[count].castle_rights=g->castle_rights; \
        moves[count].halfmove=g->halfmove; \
        count++; \
    } while(0)

    switch(ptype) {
    case PAWN: {
        int dir = (pcolor==WHITE)?1:-1;
        int start_r = (pcolor==WHITE)?1:6;
        int promo_r = (pcolor==WHITE)?7:0;
        /* Forward */
        int nr = r+dir;
        if (nr>=0&&nr<8&&g->board[sq(nr,f)]==EMPTY) {
            if (nr==promo_r) {
                for (int p=KNIGHT;p<=QUEEN;p++) ADD_MOVE(sq(nr,f),EMPTY,p,0,0);
            } else {
                ADD_MOVE(sq(nr,f),EMPTY,0,0,0);
                /* Double push */
                if (r==start_r && g->board[sq(nr+dir,f)]==EMPTY)
                    ADD_MOVE(sq(nr+dir,f),EMPTY,0,0,0);
            }
        }
        /* Captures */
        for (int df=-1;df<=1;df+=2) {
            int nf=f+df;
            if (nf<0||nf>7) continue;
            int ts=sq(nr,nf);
            if (nr>=0&&nr<8) {
                int cap=g->board[ts];
                if (cap!=EMPTY && PIECE_COLOR(cap)==opp(pcolor)) {
                    if (nr==promo_r) {
                        for (int p=KNIGHT;p<=QUEEN;p++) ADD_MOVE(ts,cap,p,0,0);
                    } else {
                        ADD_MOVE(ts,cap,0,0,0);
                    }
                }
                /* En passant */
                if (ts==g->ep_square && g->ep_square>=0) {
                    ADD_MOVE(ts,opp(pcolor)|PAWN,0,0,1);
                }
            }
        }
        break;
    }
    case KNIGHT: {
        int kd[][2]={{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i=0;i<8;i++) {
            int nr2=r+kd[i][0], nf2=f+kd[i][1];
            if (nr2<0||nr2>7||nf2<0||nf2>7) continue;
            int ts=sq(nr2,nf2);
            int cap=g->board[ts];
            if (cap==EMPTY||PIECE_COLOR(cap)==opp(pcolor))
                ADD_MOVE(ts,cap,0,0,0);
        }
        break;
    }
    case BISHOP:
    case ROOK:
    case QUEEN: {
        int sd[4][2]={{0,1},{0,-1},{1,0},{-1,0}};
        int dd[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
        int start=0, end=4;
        if (ptype==BISHOP) { start=0; end=0; /* only diag below */ }
        /* straight */
        if (ptype==ROOK||ptype==QUEEN) {
            for (int d=0;d<4;d++) {
                int cr2=r+sd[d][0], cf2=f+sd[d][1];
                while(cr2>=0&&cr2<8&&cf2>=0&&cf2<8) {
                    int ts=sq(cr2,cf2);
                    int cap=g->board[ts];
                    if (cap==EMPTY) { ADD_MOVE(ts,EMPTY,0,0,0); }
                    else { if(PIECE_COLOR(cap)==opp(pcolor)) ADD_MOVE(ts,cap,0,0,0); break; }
                    cr2+=sd[d][0]; cf2+=sd[d][1];
                }
            }
        }
        /* diagonal */
        if (ptype==BISHOP||ptype==QUEEN) {
            for (int d=0;d<4;d++) {
                int cr2=r+dd[d][0], cf2=f+dd[d][1];
                while(cr2>=0&&cr2<8&&cf2>=0&&cf2<8) {
                    int ts=sq(cr2,cf2);
                    int cap=g->board[ts];
                    if (cap==EMPTY) { ADD_MOVE(ts,EMPTY,0,0,0); }
                    else { if(PIECE_COLOR(cap)==opp(pcolor)) ADD_MOVE(ts,cap,0,0,0); break; }
                    cr2+=dd[d][0]; cf2+=dd[d][1];
                }
            }
        }
        (void)start; (void)end;
        break;
    }
    case KING: {
        for (int dr=-1;dr<=1;dr++) for (int df2=-1;df2<=1;df2++) {
            if (!dr&&!df2) continue;
            int nr2=r+dr, nf2=f+df2;
            if (nr2<0||nr2>7||nf2<0||nf2>7) continue;
            int ts=sq(nr2,nf2);
            int cap=g->board[ts];
            if (cap==EMPTY||PIECE_COLOR(cap)==opp(pcolor))
                ADD_MOVE(ts,cap,0,0,0);
        }
        /* Castling */
        if (pcolor==WHITE && from==sq(0,4)) {
            /* King-side */
            if ((g->castle_rights&CASTLE_WK) &&
                g->board[sq(0,5)]==EMPTY && g->board[sq(0,6)]==EMPTY &&
                !is_attacked(g,sq(0,4),BLACK) &&
                !is_attacked(g,sq(0,5),BLACK) &&
                !is_attacked(g,sq(0,6),BLACK))
                ADD_MOVE(sq(0,6),EMPTY,0,1,0);
            /* Queen-side */
            if ((g->castle_rights&CASTLE_WQ) &&
                g->board[sq(0,3)]==EMPTY && g->board[sq(0,2)]==EMPTY &&
                g->board[sq(0,1)]==EMPTY &&
                !is_attacked(g,sq(0,4),BLACK) &&
                !is_attacked(g,sq(0,3),BLACK) &&
                !is_attacked(g,sq(0,2),BLACK))
                ADD_MOVE(sq(0,2),EMPTY,0,2,0);
        }
        if (pcolor==BLACK && from==sq(7,4)) {
            if ((g->castle_rights&CASTLE_BK) &&
                g->board[sq(7,5)]==EMPTY && g->board[sq(7,6)]==EMPTY &&
                !is_attacked(g,sq(7,4),WHITE) &&
                !is_attacked(g,sq(7,5),WHITE) &&
                !is_attacked(g,sq(7,6),WHITE))
                ADD_MOVE(sq(7,6),EMPTY,0,3,0);
            if ((g->castle_rights&CASTLE_BQ) &&
                g->board[sq(7,3)]==EMPTY && g->board[sq(7,2)]==EMPTY &&
                g->board[sq(7,1)]==EMPTY &&
                !is_attacked(g,sq(7,4),WHITE) &&
                !is_attacked(g,sq(7,3),WHITE) &&
                !is_attacked(g,sq(7,2),WHITE))
                ADD_MOVE(sq(7,2),EMPTY,0,4,0);
        }
        break;
    }
    }
    #undef ADD_MOVE
    return count;
}

/* Get all legal moves for a color */
static int gen_legal_moves(const GameState *g, int color, Move *moves) {
    Move pseudo[256];
    int count = 0;
    for (int i=0;i<64;i++) {
        if (PIECE_COLOR(g->board[i])==color)
            count = gen_piece_moves(g, i, pseudo, count);
    }
    int legal_count = 0;
    for (int i=0;i<count;i++) {
        GameState tmp = *g;
        apply_move_raw(&tmp, &pseudo[i]);
        if (!in_check(&tmp, color))
            moves[legal_count++] = pseudo[i];
    }
    return legal_count;
}

/* Get legal moves FROM a specific square */
static int gen_legal_from(const GameState *g, int from, Move *moves) {
    Move pseudo[128];
    int count = gen_piece_moves(g, from, pseudo, 0);
    int legal_count = 0;
    int color = PIECE_COLOR(g->board[from]);
    for (int i=0;i<count;i++) {
        GameState tmp = *g;
        apply_move_raw(&tmp, &pseudo[i]);
        if (!in_check(&tmp, color))
            moves[legal_count++] = pseudo[i];
    }
    return legal_count;
}

/* ─── Apply/Undo Moves ───────────────────────────────────────────────────── */
static void update_castle_rights(GameState *g, Move *m) {
    /* Remove rights based on piece moved / captured */
    if (m->piece==W_KING)  g->castle_rights &= ~(CASTLE_WK|CASTLE_WQ);
    if (m->piece==B_KING)  g->castle_rights &= ~(CASTLE_BK|CASTLE_BQ);
    if (m->piece==W_ROOK) {
        if (m->from==sq(0,0)) g->castle_rights &= ~CASTLE_WQ;
        if (m->from==sq(0,7)) g->castle_rights &= ~CASTLE_WK;
    }
    if (m->piece==B_ROOK) {
        if (m->from==sq(7,0)) g->castle_rights &= ~CASTLE_BQ;
        if (m->from==sq(7,7)) g->castle_rights &= ~CASTLE_BK;
    }
    /* Rook captured */
    if (m->to==sq(0,0)) g->castle_rights &= ~CASTLE_WQ;
    if (m->to==sq(0,7)) g->castle_rights &= ~CASTLE_WK;
    if (m->to==sq(7,0)) g->castle_rights &= ~CASTLE_BQ;
    if (m->to==sq(7,7)) g->castle_rights &= ~CASTLE_BK;
}

static void do_move(GameState *g, Move *m) {
    /* Save state in move */
    m->ep_square     = g->ep_square;
    m->castle_rights = g->castle_rights;
    m->halfmove      = g->halfmove;

    apply_move_raw(g, m);
    update_castle_rights(g, m);

    /* Update en passant square */
    g->ep_square = -1;
    if (PIECE_TYPE(m->piece)==PAWN) {
        int dr = sq_rank(m->to)-sq_rank(m->from);
        if (dr==2) g->ep_square = sq(sq_rank(m->from)+1, sq_file(m->from));
        if (dr==-2) g->ep_square = sq(sq_rank(m->from)-1, sq_file(m->from));
    }

    /* Halfmove clock */
    if (PIECE_TYPE(m->piece)==PAWN || m->captured)
        g->halfmove = 0;
    else
        g->halfmove++;

    if (g->side==BLACK) g->fullmove++;
    g->side = opp(g->side);

    /* Save to history */
    if (g->hist_count < MAX_HISTORY)
        g->history[g->hist_count++] = *m;
}

static void undo_move(GameState *g) {
    if (g->hist_count==0) return;
    Move *m = &g->history[--g->hist_count];

    g->side = opp(g->side);
    if (g->side==BLACK) g->fullmove--;

    /* Restore board */
    g->board[m->from] = m->piece;
    g->board[m->to]   = m->captured;
    /* If promotion, restore pawn */
    if (m->promotion) g->board[m->from] = m->piece; /* piece is original pawn */
    /* Actually piece in promotion move is pawn color|PAWN at 'from' */

    /* Restore en passant capture */
    if (m->ep_capture) {
        int ep_r = sq_rank(m->to) + (IS_WHITE(m->piece)?-1:1);
        g->board[sq(ep_r, sq_file(m->to))] = opp(PIECE_COLOR(m->piece))|PAWN;
        g->board[m->to] = EMPTY; /* ep target was empty */
    }

    /* Restore castling rooks */
    switch(m->castle) {
        case 1: g->board[sq(0,7)]=W_ROOK; g->board[sq(0,5)]=EMPTY; break;
        case 2: g->board[sq(0,0)]=W_ROOK; g->board[sq(0,3)]=EMPTY; break;
        case 3: g->board[sq(7,7)]=B_ROOK; g->board[sq(7,5)]=EMPTY; break;
        case 4: g->board[sq(7,0)]=B_ROOK; g->board[sq(7,3)]=EMPTY; break;
    }

    /* Restore state */
    g->ep_square     = m->ep_square;
    g->castle_rights = m->castle_rights;
    g->halfmove      = m->halfmove;
}

/* ─── Game Initialization ────────────────────────────────────────────────── */
static void init_board(GameState *g) {
    memset(g, 0, sizeof(*g));
    /* White pieces */
    g->board[sq(0,0)]=W_ROOK;   g->board[sq(0,1)]=W_KNIGHT;
    g->board[sq(0,2)]=W_BISHOP; g->board[sq(0,3)]=W_QUEEN;
    g->board[sq(0,4)]=W_KING;   g->board[sq(0,5)]=W_BISHOP;
    g->board[sq(0,6)]=W_KNIGHT; g->board[sq(0,7)]=W_ROOK;
    for (int f=0;f<8;f++) g->board[sq(1,f)]=W_PAWN;
    /* Black pieces */
    g->board[sq(7,0)]=B_ROOK;   g->board[sq(7,1)]=B_KNIGHT;
    g->board[sq(7,2)]=B_BISHOP; g->board[sq(7,3)]=B_QUEEN;
    g->board[sq(7,4)]=B_KING;   g->board[sq(7,5)]=B_BISHOP;
    g->board[sq(7,6)]=B_KNIGHT; g->board[sq(7,7)]=B_ROOK;
    for (int f=0;f<8;f++) g->board[sq(6,f)]=B_PAWN;
    g->side = WHITE;
    g->castle_rights = CASTLE_WK|CASTLE_WQ|CASTLE_BK|CASTLE_BQ;
    g->ep_square = -1;
    g->halfmove  = 0;
    g->fullmove  = 1;
    g->hist_count = 0;
}

/* ─── FEN export ─────────────────────────────────────────────────────────── */
static void get_fen(const GameState *g, char *fen) {
    static const char pchars[] = "  pnbrqk  PNBRQK";
    int pos = 0;
    for (int r=7;r>=0;r--) {
        int empty=0;
        for (int f=0;f<8;f++) {
            int p=g->board[sq(r,f)];
            if (!p) { empty++; }
            else {
                if (empty) { fen[pos++]='0'+empty; empty=0; }
                int idx = IS_WHITE(p) ? (PIECE_TYPE(p)+8) : PIECE_TYPE(p);
                fen[pos++] = pchars[idx];
            }
        }
        if (empty) fen[pos++]='0'+empty;
        if (r>0) fen[pos++]='/';
    }
    fen[pos++]=' ';
    fen[pos++] = (g->side==WHITE)?'w':'b';
    fen[pos++]=' ';
    int cr_start = pos;
    if (g->castle_rights&CASTLE_WK) fen[pos++]='K';
    if (g->castle_rights&CASTLE_WQ) fen[pos++]='Q';
    if (g->castle_rights&CASTLE_BK) fen[pos++]='k';
    if (g->castle_rights&CASTLE_BQ) fen[pos++]='q';
    if (pos==cr_start) fen[pos++]='-';
    fen[pos++]=' ';
    if (g->ep_square>=0) {
        fen[pos++]='a'+sq_file(g->ep_square);
        fen[pos++]='1'+sq_rank(g->ep_square);
    } else { fen[pos++]='-'; }
    fen[pos]=0;
    sprintf(fen+pos," %d %d",g->halfmove,g->fullmove);
}

/* ─── PGN Move Notation ──────────────────────────────────────────────────── */
static void move_to_san(const GameState *g_before, const Move *m, char *san) {
    /* We generate SAN notation for the move */
    int pos = 0;
    int ptype = PIECE_TYPE(m->piece);
    int pcolor = PIECE_COLOR(m->piece);
    char to_name[4]; sq_name(m->to, to_name);

    /* Castling */
    if (m->castle==1||m->castle==3) { strcpy(san,"O-O"); pos=3; goto check_part; }
    if (m->castle==2||m->castle==4) { strcpy(san,"O-O-O"); pos=5; goto check_part; }

    /* Piece letter */
    if (ptype!=PAWN) {
        const char *pl = " PNBRQK";
        san[pos++] = pl[ptype];
    }

    /* Disambiguation */
    if (ptype!=PAWN) {
        Move all[256]; int cnt = gen_legal_moves(g_before, pcolor, all);
        int ambig_file=0, ambig_rank=0, ambig=0;
        for (int i=0;i<cnt;i++) {
            if (all[i].from!=m->from && all[i].to==m->to &&
                PIECE_TYPE(all[i].piece)==ptype) {
                ambig=1;
                if (sq_file(all[i].from)==sq_file(m->from)) ambig_rank=1;
                else ambig_file=1;
            }
        }
        if (ambig) {
            if (!ambig_rank||ambig_file) san[pos++]='a'+sq_file(m->from);
            if (ambig_rank)              san[pos++]='1'+sq_rank(m->from);
        }
    }

    /* Pawn file on capture */
    if (ptype==PAWN && (m->captured||m->ep_capture))
        san[pos++]='a'+sq_file(m->from);

    /* Capture */
    if (m->captured||m->ep_capture) san[pos++]='x';

    /* Destination */
    san[pos++]=to_name[0];
    san[pos++]=to_name[1];

    /* Promotion */
    if (m->promotion) {
        san[pos++]='=';
        san[pos++]=" PNBRQK"[m->promotion];
    }

check_part:
    san[pos]=0;

    /* Check / Checkmate */
    GameState tmp = *g_before;
    apply_move_raw(&tmp, (Move*)m);
    /* Update castle rights etc for check detection */
    tmp.side = opp(pcolor);
    if (in_check(&tmp, tmp.side)) {
        Move legal[256];
        int lc = gen_legal_moves(&tmp, tmp.side, legal);
        san[pos++] = (lc==0)?'#':'+';
        san[pos]=0;
    }
}

static void append_pgn_move(const GameState *g_before, const Move *m) {
    char san[32];
    move_to_san(g_before, m, san);
    char tmp[64];
    if (g_before->side==WHITE) {
        snprintf(tmp, sizeof(tmp), "%d. %s ", g_before->fullmove, san);
    } else {
        snprintf(tmp, sizeof(tmp), "%s ", san);
    }
    if (strlen(UI.pgn)+strlen(tmp) < MAX_PGN_LEN-2)
        strcat(UI.pgn, tmp);
}

static void rebuild_pgn(void) {
    UI.pgn[0]=0;
    GameState tmp;
    init_board(&tmp);
    for (int i=0;i<G.hist_count;i++) {
        Move m = G.history[i];
        /* Reconstruct move's saved fields from state */
        append_pgn_move(&tmp, &m);
        apply_move_raw(&tmp, &m);
        tmp.side = opp(tmp.side);
        /* Minimal state update */
        tmp.ep_square = -1;
        if (PIECE_TYPE(m.piece)==PAWN) {
            int dr=sq_rank(m.to)-sq_rank(m.from);
            if (dr==2) tmp.ep_square=sq(sq_rank(m.from)+1,sq_file(m.from));
            if (dr==-2)tmp.ep_square=sq(sq_rank(m.from)-1,sq_file(m.from));
        }
        if (tmp.side==BLACK) tmp.fullmove++;
        /* Restore fullmove direction */
        if (tmp.side==WHITE) {} /* already incremented */
    }
}

/* ─── UCI Engine Communication ───────────────────────────────────────────── */
static int engine_write(const char *msg) {
    if (!Eng.active) return -1;
    return write(Eng.in_fd, msg, strlen(msg));
}

static int engine_readline(char *buf, int maxlen, int timeout_ms) {
    if (!Eng.active) return -1;
    int pos=0;
    buf[0]=0;
    struct timeval tv;
    tv.tv_sec  = timeout_ms/1000;
    tv.tv_usec = (timeout_ms%1000)*1000;
    while(pos<maxlen-1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(Eng.out_fd, &fds);
        int r = select(Eng.out_fd+1, &fds, NULL, NULL, &tv);
        if (r<=0) return pos;
        char c;
        int n = read(Eng.out_fd, &c, 1);
        if (n<=0) return pos;
        if (c=='\n') { buf[pos]=0; return pos; }
        if (c!='\r') buf[pos++]=c;
        tv.tv_sec=0; tv.tv_usec=50000; /* subsequent chars faster */
    }
    buf[pos]=0;
    return pos;
}

static int start_engine(const char *path) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe)||pipe(out_pipe)) return 0;
    pid_t pid = fork();
    if (pid<0) return 0;
    if (pid==0) {
        /* Child */
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execlp(path, path, NULL);
        _exit(1);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    Eng.pid    = pid;
    Eng.in_fd  = in_pipe[1];
    Eng.out_fd = out_pipe[0];
    Eng.active = 1;
    Eng.ready  = 0;
    /* Set non-blocking */
    fcntl(Eng.out_fd, F_SETFL, O_NONBLOCK);

    /* UCI handshake */
    engine_write("uci\n");
    char line[512];
    for (int i=0;i<100;i++) {
        engine_readline(line, sizeof(line), 500);
        if (strncmp(line,"uciok",5)==0) break;
        if (strncmp(line,"id name",7)==0) {
            strncpy(Eng.name, line+8, 127);
        }
    }
    engine_write("isready\n");
    for (int i=0;i<50;i++) {
        engine_readline(line, sizeof(line), 500);
        if (strncmp(line,"readyok",7)==0) { Eng.ready=1; break; }
    }
    return Eng.ready;
}

static void engine_move(UCIMove *result) {
    if (!Eng.active || !Eng.ready) return;

    /* Build position command */
    char cmd[8192];
    char fen[256];
    init_board(&G); /* temp - we'll use proper approach */

    /* Build moves list from history */
    GameState tmp; init_board(&tmp);
    get_fen(&tmp, fen);

    strcpy(cmd, "position startpos");
    if (G.hist_count>0) {
        strcat(cmd," moves");
        for (int i=0;i<G.hist_count;i++) {
            Move *m=&G.history[i];
            char ms[8];
            char fn[3],tn[3];
            sq_name(m->from,fn); sq_name(m->to,tn);
            if (m->promotion) {
                char pp[]=" pnbrqk";
                snprintf(ms,sizeof(ms)," %s%s%c",fn,tn,pp[m->promotion]);
            } else {
                snprintf(ms,sizeof(ms)," %s%s",fn,tn);
            }
            strcat(cmd,ms);
        }
    }
    strcat(cmd,"\n");
    engine_write(cmd);

    /* Go command */
    char go[128];
    if (UI.depth_limit>0)
        snprintf(go,sizeof(go),"go depth %d\n",UI.depth_limit);
    else if (UI.nodes_limit>0)
        snprintf(go,sizeof(go),"go nodes %d\n",UI.nodes_limit);
    else if (UI.time_limit_ms>0)
        snprintf(go,sizeof(go),"go movetime %d\n",UI.time_limit_ms);
    else
        snprintf(go,sizeof(go),"go depth 6\n");
    engine_write(go);

    snprintf(UI.engine_status, sizeof(UI.engine_status),
             "Engine thinking...");

    /* Read bestmove */
    char line[512];
    result->from=result->to=-1;
    result->promotion=0;
    for (int i=0;i<5000;i++) {
        int n = engine_readline(line, sizeof(line), 100);
        if (n<=0) continue;
        /* Update engine status with info lines */
        if (strncmp(line,"info",4)==0) {
            /* Parse depth and score for display */
            char *dp=strstr(line,"depth ");
            char *sc=strstr(line,"score cp ");
            char *mate=strstr(line,"score mate ");
            char info_str[128]="";
            if (dp) {
                int d=0; sscanf(dp,"depth %d",&d);
                snprintf(info_str,sizeof(info_str),"depth:%d ",d);
            }
            if (sc) {
                int cp=0; sscanf(sc,"score cp %d",&cp);
                char tmp2[64]; snprintf(tmp2,sizeof(tmp2),"score:%.2f",cp/100.0);
                strcat(info_str,tmp2);
            } else if (mate) {
                int mt=0; sscanf(mate,"score mate %d",&mt);
                char tmp2[64]; snprintf(tmp2,sizeof(tmp2),"mate:%d",mt);
                strcat(info_str,tmp2);
            }
            if (info_str[0])
                snprintf(UI.engine_status,sizeof(UI.engine_status),
                         "Engine: %s",info_str);
        }
        if (strncmp(line,"bestmove",8)==0) {
            char *mv = line+9;
            result->from = sq_from_name(mv);
            result->to   = sq_from_name(mv+2);
            if (mv[4]&&mv[4]!='\n'&&mv[4]!=' ') {
                switch(tolower(mv[4])) {
                    case 'n': result->promotion=KNIGHT; break;
                    case 'b': result->promotion=BISHOP; break;
                    case 'r': result->promotion=ROOK;   break;
                    case 'q': result->promotion=QUEEN;  break;
                }
            }
            return;
        }
    }
}

/* ─── Display ────────────────────────────────────────────────────────────── */
#define BOARD_X 2   /* terminal column offset */
#define BOARD_Y 1   /* terminal row offset    */

static void goto_xy(int x, int y) {
    sb_printf("\033[%d;%dH", y, x);
}

static const char *sq_bg(int s) {
    int r=sq_rank(s), f=sq_file(s);
    int is_light = (r+f)%2==0;

    if (s==UI.cursor_sq)   return BG_CURSOR;
    if (s==UI.selected_sq) return BG_SELECT;
    /* Legal move highlight */
    for (int i=0;i<UI.legal_count;i++)
        if (UI.legal[i]==s) return BG_LEGAL;
    if (s==UI.last_from||s==UI.last_to) return BG_LAST;
    /* Check highlight */
    if (G.board[s]==( G.side|KING) && in_check(&G,G.side)) return BG_CHECK;
    return is_light?BG_LIGHT:BG_DARK;
}

static void draw_board(void) {
    sb_print("\033[?25l");   /* hide cursor */

    /* Board is drawn as 8 ranks, each 3 rows tall for piece display */
    /* Each square: 5 columns wide (3 chars + 2 padding) */
    int cell_w = 5;
    int cell_h = 3;
    int board_screen_col = 4;  /* leave space for rank labels */
    int board_screen_row = 2;

    /* Title */
    goto_xy(1,1);
    sb_print(BOLD FG_YELLOW);
    sb_print("  ♟ Terminal Chess GUI  ");
    if (Eng.active)
        sb_printf("[%s]", Eng.name[0]?Eng.name:"UCI Engine");
    sb_print(RESET);

    /* Top border */
    goto_xy(board_screen_col-1, board_screen_row);
    sb_print(FG_WHITE DIM);
    sb_print("  "); /* rank label space */
    sb_print(TL);
    for (int f=0;f<8;f++) {
        for (int i=0;i<cell_w;i++) sb_print(HZ);
        if (f<7) sb_print(TM);
    }
    sb_print(TR);
    sb_print(RESET);

    /* Rows */
    for (int r=7;r>=0;r--) {
        /* 3 rows per rank: top spacer, piece row, bottom spacer */
        for (int row=0;row<cell_h;row++) {
            int screen_row = board_screen_row + 1 + (7-r)*cell_h + row;
            goto_xy(1, screen_row);

            if (row==1) {
                /* Rank label */
                sb_printf(FG_CYAN "%d " RESET, r+1);
            } else {
                sb_print("   ");
            }

            /* Left border */
            sb_print(FG_WHITE DIM VT RESET);

            for (int f=0;f<8;f++) {
                int s = sq(r,f);
                const char *bg = sq_bg(s);
                int piece = G.board[s];
                int ptype = PIECE_TYPE(piece);
                int pcolor = IS_WHITE(piece)?0:1;

                sb_print(bg);

                if (row==1 && piece!=EMPTY) {
                    /* Piece row: " ♙ " style */
                    sb_print("  ");
                    /* Piece color fg */
                    if (IS_WHITE(piece)) sb_print(FG_WHITE_P BOLD);
                    else                 sb_print(FG_BLACK_P BOLD);
                    sb_print(PIECE_GLYPH[pcolor][ptype]);
                    sb_print(RESET);
                    sb_print(bg);
                    sb_print("  ");
                } else if (row==1) {
                    /* Dot for legal move */
                    int is_legal=0;
                    for (int i=0;i<UI.legal_count;i++)
                        if (UI.legal[i]==s) { is_legal=1; break; }
                    if (is_legal) {
                        sb_print("  ");
                        sb_print(FG_WHITE DIM "·" RESET);
                        sb_print(bg);
                        sb_print("  ");
                    } else {
                        sb_print("     ");
                    }
                } else {
                    sb_print("     ");
                }
                sb_print(RESET);

                /* Separator */
                sb_print(FG_WHITE DIM VT RESET);
            }

            /* File labels below board */
            if (r==0 && row==2) {
                /* Will add after board */
            }
        }

        /* Horizontal separator between ranks */
        if (r>0) {
            int screen_row = board_screen_row+1+(7-r)*cell_h+cell_h;
            goto_xy(1, screen_row);
            sb_print("   ");
            sb_print(FG_WHITE DIM LM RESET);
            for (int f=0;f<8;f++) {
                sb_print(FG_WHITE DIM);
                for (int i=0;i<cell_w;i++) sb_print(SH);
                if (f<7) sb_print(CR);
                sb_print(RESET);
            }
            sb_print(FG_WHITE DIM RM RESET);
        }
    }

    /* Bottom border */
    int bot_row = board_screen_row+1+8*cell_h;
    goto_xy(1, bot_row);
    sb_print("   ");
    sb_print(FG_WHITE DIM BL RESET);
    for (int f=0;f<8;f++) {
        sb_print(FG_WHITE DIM);
        for (int i=0;i<cell_w;i++) sb_print(HZ);
        if (f<7) sb_print(BM);
        sb_print(RESET);
    }
    sb_print(FG_WHITE DIM BR RESET);

    /* File labels */
    goto_xy(1, bot_row+1);
    sb_print("    ");
    const char *files="abcdefgh";
    for (int f=0;f<8;f++) {
        sb_printf(FG_CYAN "  %c  " RESET, files[f]);
    }

    /* ── Right panel ── */
    int panel_col = board_screen_col + 8*cell_w + 5;
    int panel_row = 2;

    /* Turn indicator */
    goto_xy(panel_col, panel_row++);
    sb_print(BOLD FG_YELLOW "═══ Game Info ═══" RESET);

    goto_xy(panel_col, panel_row++);
    if (G.side==WHITE)
        sb_print(BG_LIGHT FG_BLACK_P BOLD "  WHITE to move  " RESET);
    else
        sb_print(BG_DARK  FG_WHITE_P BOLD "  BLACK to move  " RESET);

    panel_row++;
    goto_xy(panel_col, panel_row++);
    sb_printf(FG_WHITE "Move: %d  Half: %d" RESET, G.fullmove, G.halfmove);

    /* Check/game status */
    if (in_check(&G,G.side)) {
        goto_xy(panel_col, panel_row++);
        sb_print(FG_RED BOLD "  ⚠  CHECK!  ⚠" RESET);
    } else {
        panel_row++;
    }

    /* Game over */
    if (UI.game_over) {
        goto_xy(panel_col, panel_row++);
        sb_printf(FG_MAGENTA BOLD "GAME OVER: %s" RESET, UI.result);
    } else {
        panel_row++;
    }

    panel_row++;
    goto_xy(panel_col, panel_row++);
    sb_print(BOLD FG_CYAN "═══ Engine ════" RESET);
    goto_xy(panel_col, panel_row++);
    if (Eng.active)
        sb_printf(FG_GREEN "Engine: %s" RESET, Eng.name[0]?Eng.name:"Connected");
    else
        sb_print(FG_RED "No engine loaded" RESET);

    goto_xy(panel_col, panel_row++);
    sb_printf(FG_WHITE "Depth: %s%d  " RESET,
              UI.depth_limit>0?FG_GREEN:FG_GRAY,
              UI.depth_limit>0?UI.depth_limit:6);
    goto_xy(panel_col, panel_row++);
    sb_printf(FG_WHITE "Time:  %s%d ms  " RESET,
              UI.time_limit_ms>0?FG_GREEN:FG_GRAY,
              UI.time_limit_ms>0?UI.time_limit_ms:0);
    goto_xy(panel_col, panel_row++);
    sb_printf(FG_WHITE "Nodes: %s%d  " RESET,
              UI.nodes_limit>0?FG_GREEN:FG_GRAY,
              UI.nodes_limit>0?UI.nodes_limit:0);

    goto_xy(panel_col, panel_row++);
    sb_printf(FG_YELLOW "%.50s" RESET, UI.engine_status);
    panel_row++;

    /* Human color */
    goto_xy(panel_col, panel_row++);
    if (UI.human_color==0)
        sb_print(FG_WHITE "Mode: Both human" RESET);
    else if (UI.human_color==WHITE)
        sb_print(FG_WHITE "Mode: Human=White" RESET);
    else
        sb_print(FG_WHITE "Mode: Human=Black" RESET);

    panel_row++;
    goto_xy(panel_col, panel_row++);
    sb_print(BOLD FG_CYAN "═══ Controls ══" RESET);
    goto_xy(panel_col, panel_row++);
    sb_print(FG_GRAY "Arrow/WASD: move cursor" RESET);
    goto_xy(panel_col, panel_row++);
    sb_print(FG_GRAY "Enter/Spc:  select/move" RESET);
    goto_xy(panel_col, panel_row++);
    sb_print(FG_GRAY "U: undo  N: new game" RESET);
    goto_xy(panel_col, panel_row++);
    sb_print(FG_GRAY "E: engine move" RESET);
    goto_xy(panel_col, panel_row++);
    sb_print(FG_GRAY "F: flip board" RESET);
    goto_xy(panel_col, panel_row++);
    sb_print(FG_GRAY "D/T/O: set depth/time/nodes" RESET);
    goto_xy(panel_col, panel_row++);
    sb_print(FG_GRAY "C: set human color" RESET);
    goto_xy(panel_col, panel_row++);
    sb_print(FG_GRAY "Q: quit" RESET);

    /* Status bar */
    int status_row = bot_row+3;
    goto_xy(1, status_row);
    sb_printf(FG_YELLOW "Status: %-60s" RESET, UI.status);

    /* ── PGN panel ── */
    int pgn_row = status_row+2;
    goto_xy(1, pgn_row++);
    sb_print(BOLD FG_CYAN "═══ PGN ══════════════════════════════════════════════════════" RESET);

    /* Word-wrap PGN at ~70 chars */
    char pgn_copy[MAX_PGN_LEN];
    strncpy(pgn_copy, UI.pgn, sizeof(pgn_copy)-1);
    if (UI.game_over) {
        strncat(pgn_copy, UI.result,
                sizeof(pgn_copy)-strlen(pgn_copy)-1);
    }

    int pgn_len = strlen(pgn_copy);
    int line_pos=0, ci=0, lines_shown=0;
    char line_buf[128]; int lbi=0;

    while(ci<=pgn_len && lines_shown<4) {
        char c = pgn_copy[ci++];
        if (c==' '||c==0) {
            if (line_pos+lbi>68 && lbi>0) {
                line_buf[lbi]=0;
                /* flush current line */
                goto_xy(1, pgn_row);
                sb_printf(FG_WHITE "%-70s" RESET, "");
                goto_xy(1, pgn_row++);
                lines_shown++;
                line_pos=0; lbi=0;
                /* start new line with this token */
            }
            if (lbi>0) {
                line_buf[lbi]=0;
                /* print token */
                goto_xy(1+line_pos, pgn_row);
                /* determine if it's a move number */
                if (line_buf[lbi-1]=='.') {
                    sb_printf(FG_CYAN "%s " RESET, line_buf);
                } else {
                    sb_printf(FG_WHITE "%s " RESET, line_buf);
                }
                line_pos += lbi+1;
                lbi=0;
            }
        } else {
            if (lbi<(int)sizeof(line_buf)-1) line_buf[lbi++]=c;
        }
    }
    /* flush remaining */
    if (line_pos>0 || lbi>0) {
        if (lbi>0) {
            line_buf[lbi]=0;
            goto_xy(1+line_pos, pgn_row);
            sb_printf(FG_WHITE "%s" RESET, line_buf);
        }
        pgn_row++;
    }

    /* Clear remaining pgn lines */
    for (int i=pgn_row;i<pgn_row+2;i++) {
        goto_xy(1,i);
        sb_print("                                                                      ");
    }

    sb_flush();
}

static int flip_board = 0;

static int display_sq(int s) {
    /* Returns the square index accounting for board flip */
    if (!flip_board) return s;
    int r=sq_rank(s), f=sq_file(s);
    return sq(7-r, 7-f);
}

/* ─── UI Logic ───────────────────────────────────────────────────────────── */
static void clear_selection(void) {
    UI.selected_sq = -1;
    UI.legal_count = 0;
    memset(UI.legal, 0, sizeof(UI.legal));
}

static void select_square(int s) {
    int piece = G.board[s];
    if (!piece || PIECE_COLOR(piece)!=G.side) {
        clear_selection();
        return;
    }
    UI.selected_sq = s;
    UI.legal_count = 0;
    Move moves[128];
    int cnt = gen_legal_from(&G, s, moves);
    for (int i=0;i<cnt;i++) {
        UI.legal[UI.legal_count++] = moves[i].to;
    }
}

/* Try to execute a move from selected_sq to target */
static int try_move(int from, int to, int promo) {
    Move moves[128];
    int cnt = gen_legal_from(&G, from, moves);
    for (int i=0;i<cnt;i++) {
        if (moves[i].to==to) {
            if (moves[i].promotion && !promo) promo=QUEEN; /* default */
            if (moves[i].promotion && moves[i].promotion!=promo) continue;
            GameState before = G;
            /* Save SAN before making move */
            append_pgn_move(&G, &moves[i]);
            do_move(&G, &moves[i]);
            UI.last_from = from;
            UI.last_to   = to;
            clear_selection();
            snprintf(UI.status, sizeof(UI.status), "Moved %c%d → %c%d",
                     'a'+sq_file(from),'1'+sq_rank(from),
                     'a'+sq_file(to),'1'+sq_rank(to));
            (void)before;
            return 1;
        }
    }
    return 0;
}

static void check_game_over(void) {
    Move legal[256];
    int cnt = gen_legal_moves(&G, G.side, legal);
    if (cnt==0) {
        UI.game_over=1;
        if (in_check(&G,G.side)) {
            snprintf(UI.result,sizeof(UI.result),
                     "%s wins by checkmate",
                     G.side==WHITE?"Black":"White");
            snprintf(UI.status,sizeof(UI.status),
                     "Checkmate! %s wins!", G.side==WHITE?"Black":"White");
            strcat(UI.pgn, G.side==WHITE?" 0-1":" 1-0");
        } else {
            strcpy(UI.result,"Draw by stalemate");
            snprintf(UI.status,sizeof(UI.status),"Stalemate! Draw.");
            strcat(UI.pgn," 1/2-1/2");
        }
    }
    if (G.halfmove>=100) {
        UI.game_over=1;
        strcpy(UI.result,"Draw by 50-move rule");
        strcpy(UI.status,"50-move rule: Draw!");
        strcat(UI.pgn," 1/2-1/2");
    }
}

/* ─── Input Handling (full terminal menu for settings) ──────────────────── */
static void show_cursor_input(void) {
    /* Restore terminal briefly for line input */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h");
    fflush(stdout);
}

static void hide_cursor_input(void) {
    term_raw();
    printf("\033[?25l");
    fflush(stdout);
}

static int read_int_setting(const char *prompt, int current) {
    show_cursor_input();
    /* Find a good row to print */
    printf("\033[50;1H\033[2K");
    printf(FG_YELLOW "%s (current: %d, 0=off): " RESET, prompt, current);
    fflush(stdout);
    char buf[64]; buf[0]=0;
    if (!fgets(buf, sizeof(buf), stdin)) { hide_cursor_input(); return current; }
    hide_cursor_input();
    int v = atoi(buf);
    return v;
}

static void handle_key(int key, int key2, int key3) {
    if (UI.game_over && key!='n' && key!='N' && key!='q' && key!='Q') {
        snprintf(UI.status,sizeof(UI.status),"Game over. Press N for new game.");
        return;
    }

    int r = sq_rank(UI.cursor_sq);
    int f = sq_file(UI.cursor_sq);

    /* Arrow keys: key=27 key2=91 key3=A/B/C/D */
    if (key==27 && key2==91) {
        switch(key3) {
            case 'A': r = (r<7)?r+1:r; break; /* up    */
            case 'B': r = (r>0)?r-1:r; break; /* down  */
            case 'C': f = (f<7)?f+1:f; break; /* right */
            case 'D': f = (f>0)?f-1:f; break; /* left  */
        }
        UI.cursor_sq = sq(r,f);
        return;
    }

    switch(key) {
    /* WASD movement */
    case 'w': case 'W': r=(r<7)?r+1:r; UI.cursor_sq=sq(r,f); break;
    case 's': case 'S': r=(r>0)?r-1:r; UI.cursor_sq=sq(r,f); break;
    case 'd': case 'D':
        if (key=='d') { f=(f<7)?f+1:f; UI.cursor_sq=sq(r,f); break; }
        /* fall through if 'D' not used for movement */
        goto settings;
    case 'a': case 'A': f=(f>0)?f-1:f; UI.cursor_sq=sq(r,f); break;

    case '\r': case '\n': case ' ': {
        int s = UI.cursor_sq;
        if (UI.selected_sq<0) {
            /* Select */
            if (G.board[s] && PIECE_COLOR(G.board[s])==G.side)
                select_square(s);
            else
                snprintf(UI.status,sizeof(UI.status),
                         "No piece to select at %c%d",
                         'a'+sq_file(s),'1'+sq_rank(s));
        } else {
            if (s==UI.selected_sq) {
                clear_selection();
            } else {
                int moved = try_move(UI.selected_sq, s, 0);
                if (!moved) {
                    /* Try selecting new piece */
                    if (G.board[s]&&PIECE_COLOR(G.board[s])==G.side)
                        select_square(s);
                    else {
                        snprintf(UI.status,sizeof(UI.status),
                                 "Illegal move to %c%d",
                                 'a'+sq_file(s),'1'+sq_rank(s));
                        clear_selection();
                    }
                } else {
                    check_game_over();
                }
            }
        }
        break;
    }

    case 'u': case 'U':
        /* Undo */
        if (G.hist_count==0) {
            strcpy(UI.status,"Nothing to undo.");
        } else {
            undo_move(&G);
            UI.game_over=0;
            UI.result[0]=0;
            clear_selection();
            UI.last_from=UI.last_to=-1;
            rebuild_pgn();
            /* Strip trailing result token if any */
            snprintf(UI.status,sizeof(UI.status),"Undo! Move %d.",G.fullmove);
        }
        break;

    case 'n': case 'N':
        /* New game */
        init_board(&G);
        UI.game_over=0;
        UI.result[0]=0;
        UI.pgn[0]=0;
        UI.last_from=UI.last_to=-1;
        clear_selection();
        UI.cursor_sq=0;
        UI.engine_status[0]=0;
        snprintf(UI.status,sizeof(UI.status),"New game started.");
        break;

    case 'e': case 'E':
        /* Engine move */
        if (!Eng.active) {
            strcpy(UI.status,"No engine loaded.");
        } else if (!UI.game_over) {
            draw_board(); /* show "thinking" */
            UCIMove em; em.from=em.to=-1; em.promotion=0;
            engine_move(&em);
            if (em.from>=0&&em.to>=0) {
                if (!try_move(em.from,em.to,em.promotion)) {
                    snprintf(UI.status,sizeof(UI.status),
                             "Engine returned illegal move!");
                } else {
                    check_game_over();
                    snprintf(UI.status,sizeof(UI.status),
                             "Engine: %c%d→%c%d",
                             'a'+sq_file(em.from),'1'+sq_rank(em.from),
                             'a'+sq_file(em.to),'1'+sq_rank(em.to));
                }
            } else {
                strcpy(UI.status,"Engine returned no move.");
            }
        }
        break;

    case 'f': case 'F':
        flip_board = !flip_board;
        snprintf(UI.status,sizeof(UI.status),"Board %s.",
                 flip_board?"flipped":"normal");
        break;

    settings:
    case 't': case 'T':
        UI.time_limit_ms = read_int_setting("Time limit (ms)", UI.time_limit_ms);
        if (UI.time_limit_ms>0) { UI.depth_limit=0; UI.nodes_limit=0; }
        snprintf(UI.status,sizeof(UI.status),
                 "Time limit set to %d ms.",UI.time_limit_ms);
        break;

    case 'o': case 'O':
        UI.nodes_limit = read_int_setting("Node limit", UI.nodes_limit);
        if (UI.nodes_limit>0) { UI.depth_limit=0; UI.time_limit_ms=0; }
        snprintf(UI.status,sizeof(UI.status),
                 "Node limit set to %d.",UI.nodes_limit);
        break;

    case 'c': case 'C': {
        show_cursor_input();
        printf("\033[50;1H\033[2K");
        printf(FG_YELLOW "Human plays: (w)hite, (b)lack, (0)both: " RESET);
        fflush(stdout);
        char buf[4]={0}; fgets(buf,sizeof(buf),stdin);
        hide_cursor_input();
        if (buf[0]=='w'||buf[0]=='W') UI.human_color=WHITE;
        else if (buf[0]=='b'||buf[0]=='B') UI.human_color=BLACK;
        else UI.human_color=0;
        snprintf(UI.status,sizeof(UI.status),"Color set.");
        break;
    }

    case 'q': case 'Q':
        cleanup();
        /* Clear screen */
        printf("\033[2J\033[1;1H\033[?25h");
        printf("Thanks for playing! Goodbye.\n");
        exit(0);

    default:
        break;
    }

    /* Handle 'd' for depth separately since 'D' could be movement too */
    if (key=='d' && key2==0 && key3==0) {
        /* 'd' alone = move right, but we want depth via capital D */
        f=(f<7)?f+1:f; UI.cursor_sq=sq(r,f);
    }
}

/* ─── Main Loop ──────────────────────────────────────────────────────────── */
static void print_help(void) {
    printf("\nTerminal Chess GUI\n");
    printf("Usage: chess_gui [engine_path]\n\n");
    printf("Controls:\n");
    printf("  Arrow keys / WASD  : Move cursor\n");
    printf("  Enter / Space      : Select piece / Move\n");
    printf("  U                  : Undo last move\n");
    printf("  N                  : New game\n");
    printf("  E                  : Engine makes a move\n");
    printf("  F                  : Flip board\n");
    printf("  D (capital)        : Set engine depth\n");
    printf("  T                  : Set engine time limit (ms)\n");
    printf("  O                  : Set engine node limit\n");
    printf("  C                  : Set human color\n");
    printf("  Q                  : Quit\n\n");
}

int main(int argc, char **argv) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Init state */
    memset(&G,   0, sizeof(G));
    memset(&UI,  0, sizeof(UI));
    memset(&Eng, 0, sizeof(Eng));

    init_board(&G);
    UI.cursor_sq   = sq(0,0);
    UI.selected_sq = -1;
    UI.last_from   = -1;
    UI.last_to     = -1;
    UI.human_color = 0;  /* both sides human by default */
    UI.depth_limit = 6;
    UI.time_limit_ms = 0;
    UI.nodes_limit   = 0;
    strcpy(UI.status, "Welcome! Arrow keys/WASD to move, Enter to select.");

    /* Start engine if provided */
    if (argc>1) {
        printf("Starting engine: %s\n", argv[1]);
        fflush(stdout);
        if (start_engine(argv[1])) {
            printf("Engine ready: %s\n", Eng.name[0]?Eng.name:argv[1]);
            snprintf(UI.engine_status,sizeof(UI.engine_status),
                     "Ready: %s", Eng.name[0]?Eng.name:"engine");
            UI.human_color = WHITE; /* default: human=white, engine=black */
        } else {
            printf("Failed to start engine.\n");
        }
        fflush(stdout);
        usleep(500000);
    }

    /* Clear screen and enter raw mode */
    printf("\033[2J\033[1;1H");
    fflush(stdout);
    term_raw();

    /* Main loop */
    for (;;) {
        /* Auto-engine move if it's engine's turn */
        if (!UI.game_over && Eng.active &&
            UI.human_color!=0 && G.side!=UI.human_color) {
            draw_board();
            UCIMove em; em.from=em.to=-1; em.promotion=0;
            engine_move(&em);
            if (em.from>=0&&em.to>=0) {
                if (try_move(em.from,em.to,em.promotion)) {
                    check_game_over();
                    snprintf(UI.status,sizeof(UI.status),
                             "Engine played: %c%d→%c%d",
                             'a'+sq_file(em.from),'1'+sq_rank(em.from),
                             'a'+sq_file(em.to),'1'+sq_rank(em.to));
                }
            }
        }

        draw_board();

        /* Read input */
        unsigned char c1=0, c2=0, c3=0;
        int n = read(STDIN_FILENO, &c1, 1);
        if (n<=0) { usleep(10000); continue; }
        if (c1==27) {
            /* Possible escape sequence */
            usleep(5000);
            n = read(STDIN_FILENO, &c2, 1);
            if (n>0 && c2==91) {
                n = read(STDIN_FILENO, &c3, 1);
            }
        }
        handle_key(c1, c2, c3);
    }
    return 0;
}
