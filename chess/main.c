#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

/* ─── Terminal colours / box-drawing ─────────────────────────────────────── */
#define ESC "\033["
#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define CLEAR_SCREEN "\033[2J"
#define HOME        "\033[H"

/* 256-colour palette */
#define BG_LIGHT    "\033[48;5;180m"   /* light square  */
#define BG_DARK     "\033[48;5;101m"   /* dark square   */
#define BG_SEL      "\033[48;5;220m"   /* selected      */
#define BG_MOVE     "\033[48;5;154m"   /* legal-move dot*/
#define BG_LAST     "\033[48;5;143m"   /* last-move highlight */
#define BG_CHECK    "\033[48;5;196m"   /* king in check */
#define FG_WHITE_P  "\033[38;5;255m"   /* white piece   */
#define FG_BLACK_P  "\033[38;5;232m"   /* black piece   */
#define BG_STATUS   "\033[48;5;236m"
#define FG_STATUS   "\033[38;5;250m"
#define FG_LABEL    "\033[38;5;244m"

/* ─── Chess constants ────────────────────────────────────────────────────── */
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

#define PIECE_TYPE(p)  ((p)&7)
#define PIECE_COLOR(p) ((p)&24)
#define IS_WHITE(p)    (((p)&24)==WHITE)
#define IS_BLACK(p)    (((p)&24)==BLACK)

/* castling rights bits */
#define CASTLE_WK 1
#define CASTLE_WQ 2
#define CASTLE_BK 4
#define CASTLE_BQ 8

/* ─── Unicode pieces ─────────────────────────────────────────────────────── */
static const char *piece_unicode[2][7] = {
    /* WHITE */  {"  ","♙ ","♘ ","♗ ","♖ ","♕ ","♔ "},
    /* BLACK */  {"  ","♟ ","♞ ","♝ ","♜ ","♛ ","♚ "}
};

/* ─── Move record ────────────────────────────────────────────────────────── */
typedef struct {
    int from, to;          /* 0-63 */
    int piece, captured;
    int promotion;         /* piece type, or 0 */
    int ep_square;         /* en-passant target before move */
    int castle_rights;     /* before move */
    int ep_captured_sq;    /* square of captured pawn (en-passant) */
    int is_castle;         /* 0,1 */
    int rook_from, rook_to;
    char san[16];          /* SAN notation */
    char uci[6];
} Move;

/* ─── Game state ─────────────────────────────────────────────────────────── */
#define MAX_MOVES 512
typedef struct {
    int board[64];
    int side;              /* WHITE or BLACK */
    int castle_rights;
    int ep_square;         /* -1 if none */
    int halfmove;
    int fullmove;
    Move history[MAX_MOVES];
    int hist_count;
    int cursor;            /* 0-63 cursor position */
    int selected;          /* -1 or square */
    int legal_targets[64];
    int n_legal;
    /* last move highlight */
    int last_from, last_to;
    /* UCI engine */
    pid_t eng_pid;
    int eng_in[2], eng_out[2];  /* pipes */
    int engine_active;
    int engine_color;      /* WHITE or BLACK, -1=none */
    char engine_path[256];
    /* time control */
    int tc_mode;           /* 0=nodes 1=depth 2=time */
    int tc_nodes;
    int tc_depth;
    int tc_time_ms;
    /* status message */
    char status[128];
    int game_over;
    char result[8];
    /* pgn move list */
    char pgn_moves[MAX_MOVES][16];
    int pgn_count;
    /* promotion pending */
    int promo_pending;
    int promo_from, promo_to;
    int flipped;
} Game;

static Game G;
static struct termios orig_term;

/* ────────────────────────────────────────────────────────────────────────── */
/*  Terminal helpers                                                           */
/* ────────────────────────────────────────────────────────────────────────── */
static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &orig_term);
    t = orig_term;
    t.c_lflag &= ~(ICANON|ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}
static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
}
static void hide_cursor(void) { printf("\033[?25l"); }
static void show_cursor(void) { printf("\033[?25h"); }
static void goto_xy(int row, int col) { printf("\033[%d;%dH", row, col); }

/* ────────────────────────────────────────────────────────────────────────── */
/*  Board helpers                                                              */
/* ────────────────────────────────────────────────────────────────────────── */
static int rank_of(int sq) { return sq/8; }
static int file_of(int sq) { return sq%8; }
static int sq(int r,int f) { return r*8+f; }

static void board_start(void) {
    int b[64] = {
        W_ROOK,W_KNIGHT,W_BISHOP,W_QUEEN,W_KING,W_BISHOP,W_KNIGHT,W_ROOK,
        W_PAWN,W_PAWN,W_PAWN,W_PAWN,W_PAWN,W_PAWN,W_PAWN,W_PAWN,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        B_PAWN,B_PAWN,B_PAWN,B_PAWN,B_PAWN,B_PAWN,B_PAWN,B_PAWN,
        B_ROOK,B_KNIGHT,B_BISHOP,B_QUEEN,B_KING,B_BISHOP,B_KNIGHT,B_ROOK
    };
    memcpy(G.board, b, sizeof(b));
    G.side = WHITE;
    G.castle_rights = CASTLE_WK|CASTLE_WQ|CASTLE_BK|CASTLE_BQ;
    G.ep_square = -1;
    G.halfmove = 0; G.fullmove = 1;
    G.hist_count = 0;
    G.cursor = 4; G.selected = -1;
    G.last_from = G.last_to = -1;
    G.n_legal = 0;
    G.game_over = 0;
    G.pgn_count = 0;
    G.promo_pending = 0;
    G.flipped = 0;
    strcpy(G.status, "Welcome! Arrow keys move cursor, Enter selects.");
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  Legal move generation                                                     */
/* ────────────────────────────────────────────────────────────────────────── */

/* Simple attack detection – does 'color' attack square 'sq'? */
static int attacks(int *board, int sq_t, int color) {
    for (int s = 0; s < 64; s++) {
        int p = board[s];
        if (!p || PIECE_COLOR(p)!=color) continue;
        int t = PIECE_TYPE(p);
        int rf = rank_of(s), ff = file_of(s);
        int rt = rank_of(sq_t), ft = file_of(sq_t);
        int dr = rt-rf, df = ft-ff;
        switch(t) {
        case PAWN: {
            int dir = (color==WHITE)?1:-1;
            if (dr==dir && (df==1||df==-1)) return 1;
            break;
        }
        case KNIGHT:
            if ((abs(dr)==2&&abs(df)==1)||(abs(dr)==1&&abs(df)==2)) return 1;
            break;
        case BISHOP:
            if (abs(dr)==abs(df) && dr!=0) {
                int ok=1;
                int sr=rf+(dr>0?1:-1), sf=ff+(df>0?1:-1);
                while (sr!=rt||sf!=ft) {
                    if (board[sq(sr,sf)]) { ok=0; break; }
                    sr+=(dr>0?1:-1); sf+=(df>0?1:-1);
                }
                if (ok) return 1;
            }
            break;
        case ROOK:
            if (dr==0||df==0) {
                int ok=1;
                int sr=rf+(dr==0?0:(dr>0?1:-1));
                int sf=ff+(df==0?0:(df>0?1:-1));
                while (sr!=rt||sf!=ft) {
                    if (board[sq(sr,sf)]) { ok=0; break; }
                    sr+=(dr==0?0:(dr>0?1:-1));
                    sf+=(df==0?0:(df>0?1:-1));
                }
                if (ok) return 1;
            }
            break;
        case QUEEN: {
            /* rook part */
            if (dr==0||df==0) {
                int ok=1;
                int sr=rf+(dr==0?0:(dr>0?1:-1));
                int sf=ff+(df==0?0:(df>0?1:-1));
                while (sr!=rt||sf!=ft) {
                    if (board[sq(sr,sf)]) { ok=0; break; }
                    sr+=(dr==0?0:(dr>0?1:-1));
                    sf+=(df==0?0:(df>0?1:-1));
                }
                if (ok) return 1;
            }
            /* bishop part */
            if (abs(dr)==abs(df) && dr!=0) {
                int ok=1;
                int sr=rf+(dr>0?1:-1), sf=ff+(df>0?1:-1);
                while (sr!=rt||sf!=ft) {
                    if (board[sq(sr,sf)]) { ok=0; break; }
                    sr+=(dr>0?1:-1); sf+=(df>0?1:-1);
                }
                if (ok) return 1;
            }
            break;
        }
        case KING:
            if (abs(dr)<=1 && abs(df)<=1) return 1;
            break;
        }
    }
    return 0;
}

static int king_sq(int *board, int color) {
    int king = color|KING;
    for (int i=0;i<64;i++) if (board[i]==king) return i;
    return -1;
}

static int in_check(int *board, int color) {
    int ks = king_sq(board, color);
    if (ks<0) return 1;
    int opp = (color==WHITE)?BLACK:WHITE;
    return attacks(board, ks, opp);
}

/* Try applying move, return 1 if leaves own king NOT in check */
typedef struct { int from,to,promo,ep_sq,ep_captured,castle,rook_from,rook_to; } TryMove;

static int try_move_legal(TryMove *m) {
    int board[64];
    memcpy(board, G.board, sizeof(board));
    board[m->to] = m->promo ? (G.side|m->promo) : board[m->from];
    board[m->from] = EMPTY;
    if (m->ep_captured>=0) board[m->ep_captured] = EMPTY;
    if (m->castle) { board[m->rook_to]=board[m->rook_from]; board[m->rook_from]=EMPTY; }
    return !in_check(board, G.side);
}

typedef struct { int to; TryMove tm; } LegalMove;
static LegalMove legal_buf[256];
static int legal_count;

static void gen_legal_from(int from) {
    legal_count = 0;
    int p = G.board[from];
    if (!p || PIECE_COLOR(p)!=G.side) return;
    int t = PIECE_TYPE(p);
    int rf=rank_of(from), ff=file_of(from);
    int opp = (G.side==WHITE)?BLACK:WHITE;

    auto void try_add(int to, int promo, int ep_cap, int castle, int rf2, int rt2);
    void try_add(int to, int promo, int ep_cap, int castle, int rf2, int rt2) {
        if (to<0||to>63) return;
        TryMove tm = {from,to,promo,G.ep_square,ep_cap,castle,rf2,rt2};
        if (try_move_legal(&tm)) {
            legal_buf[legal_count++] = (LegalMove){to, tm};
        }
    }

    switch(t) {
    case PAWN: {
        int dir = (G.side==WHITE)?1:-1;
        int start_rank = (G.side==WHITE)?1:6;
        int promo_rank = (G.side==WHITE)?7:0;
        /* forward */
        int t1 = sq(rf+dir, ff);
        if (rf+dir>=0 && rf+dir<=7 && !G.board[t1]) {
            if (rf+dir==promo_rank) {
                for (int pr=KNIGHT;pr<=QUEEN;pr++) try_add(t1,pr,-1,0,0,0);
            } else {
                try_add(t1,0,-1,0,0,0);
                /* double push */
                if (rf==start_rank) {
                    int t2=sq(rf+2*dir,ff);
                    if (!G.board[t2]) try_add(t2,0,-1,0,0,0);
                }
            }
        }
        /* captures */
        for (int df2=-1;df2<=1;df2+=2) {
            int nf=ff+df2, nr=rf+dir;
            if (nf<0||nf>7||nr<0||nr>7) continue;
            int ts=sq(nr,nf);
            if (G.board[ts] && PIECE_COLOR(G.board[ts])==opp) {
                if (nr==promo_rank) {
                    for (int pr=KNIGHT;pr<=QUEEN;pr++) try_add(ts,pr,-1,0,0,0);
                } else try_add(ts,0,-1,0,0,0);
            }
            /* en passant */
            if (G.ep_square==ts) {
                int cap_sq=sq(rf,nf);
                try_add(ts,0,cap_sq,0,0,0);
            }
        }
        break;
    }
    case KNIGHT: {
        int deltas[8][2]={{2,1},{2,-1},{-2,1},{-2,-1},{1,2},{1,-2},{-1,2},{-1,-2}};
        for (int i=0;i<8;i++) {
            int nr=rf+deltas[i][0], nf2=ff+deltas[i][1];
            if (nr<0||nr>7||nf2<0||nf2>7) continue;
            int ts=sq(nr,nf2);
            if (!G.board[ts]||PIECE_COLOR(G.board[ts])==opp) try_add(ts,0,-1,0,0,0);
        }
        break;
    }
    case BISHOP: {
        int dirs[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
        for (int d=0;d<4;d++) {
            int nr=rf+dirs[d][0], nf2=ff+dirs[d][1];
            while (nr>=0&&nr<=7&&nf2>=0&&nf2<=7) {
                int ts=sq(nr,nf2);
                if (G.board[ts]) {
                    if (PIECE_COLOR(G.board[ts])==opp) try_add(ts,0,-1,0,0,0);
                    break;
                }
                try_add(ts,0,-1,0,0,0);
                nr+=dirs[d][0]; nf2+=dirs[d][1];
            }
        }
        break;
    }
    case ROOK: {
        int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        for (int d=0;d<4;d++) {
            int nr=rf+dirs[d][0], nf2=ff+dirs[d][1];
            while (nr>=0&&nr<=7&&nf2>=0&&nf2<=7) {
                int ts=sq(nr,nf2);
                if (G.board[ts]) {
                    if (PIECE_COLOR(G.board[ts])==opp) try_add(ts,0,-1,0,0,0);
                    break;
                }
                try_add(ts,0,-1,0,0,0);
                nr+=dirs[d][0]; nf2+=dirs[d][1];
            }
        }
        break;
    }
    case QUEEN: {
        int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        for (int d=0;d<8;d++) {
            int nr=rf+dirs[d][0], nf2=ff+dirs[d][1];
            while (nr>=0&&nr<=7&&nf2>=0&&nf2<=7) {
                int ts=sq(nr,nf2);
                if (G.board[ts]) {
                    if (PIECE_COLOR(G.board[ts])==opp) try_add(ts,0,-1,0,0,0);
                    break;
                }
                try_add(ts,0,-1,0,0,0);
                nr+=dirs[d][0]; nf2+=dirs[d][1];
            }
        }
        break;
    }
    case KING: {
        int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        for (int d=0;d<8;d++) {
            int nr=rf+dirs[d][0], nf2=ff+dirs[d][1];
            if (nr<0||nr>7||nf2<0||nf2>7) continue;
            int ts=sq(nr,nf2);
            if (!G.board[ts]||PIECE_COLOR(G.board[ts])==opp) try_add(ts,0,-1,0,0,0);
        }
        /* castling */
        if (G.side==WHITE && rf==0 && ff==4) {
            if ((G.castle_rights&CASTLE_WK) && !G.board[sq(0,5)] && !G.board[sq(0,6)] &&
                !attacks(G.board,sq(0,4),BLACK) && !attacks(G.board,sq(0,5),BLACK) && !attacks(G.board,sq(0,6),BLACK)) {
                TryMove tm={from,sq(0,6),0,-1,-1,1,sq(0,7),sq(0,5)};
                if (try_move_legal(&tm)) legal_buf[legal_count++]=(LegalMove){sq(0,6),tm};
            }
            if ((G.castle_rights&CASTLE_WQ) && !G.board[sq(0,3)] && !G.board[sq(0,2)] && !G.board[sq(0,1)] &&
                !attacks(G.board,sq(0,4),BLACK) && !attacks(G.board,sq(0,3),BLACK) && !attacks(G.board,sq(0,2),BLACK)) {
                TryMove tm={from,sq(0,2),0,-1,-1,1,sq(0,0),sq(0,3)};
                if (try_move_legal(&tm)) legal_buf[legal_count++]=(LegalMove){sq(0,2),tm};
            }
        }
        if (G.side==BLACK && rf==7 && ff==4) {
            if ((G.castle_rights&CASTLE_BK) && !G.board[sq(7,5)] && !G.board[sq(7,6)] &&
                !attacks(G.board,sq(7,4),WHITE) && !attacks(G.board,sq(7,5),WHITE) && !attacks(G.board,sq(7,6),WHITE)) {
                TryMove tm={from,sq(7,6),0,-1,-1,1,sq(7,7),sq(7,5)};
                if (try_move_legal(&tm)) legal_buf[legal_count++]=(LegalMove){sq(7,6),tm};
            }
            if ((G.castle_rights&CASTLE_BQ) && !G.board[sq(7,3)] && !G.board[sq(7,2)] && !G.board[sq(7,1)] &&
                !attacks(G.board,sq(7,4),WHITE) && !attacks(G.board,sq(7,3),WHITE) && !attacks(G.board,sq(7,2),WHITE)) {
                TryMove tm={from,sq(7,2),0,-1,-1,1,sq(7,0),sq(7,3)};
                if (try_move_legal(&tm)) legal_buf[legal_count++]=(LegalMove){sq(7,2),tm};
            }
        }
        break;
    }
    }
}

/* Count all legal moves for current side */
static int count_all_legal(void) {
    int total=0;
    for (int s=0;s<64;s++) {
        if (G.board[s] && PIECE_COLOR(G.board[s])==G.side) {
            gen_legal_from(s);
            total+=legal_count;
        }
    }
    return total;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  SAN builder                                                               */
/* ────────────────────────────────────────────────────────────────────────── */
static const char piece_char[] = " PNBRQK";
static const char file_char[] = "abcdefgh";

static void build_san(int from, int to, int promo, int captured, int ep_cap,
                       int castle, int rook_from, char *out) {
    (void)rook_from;
    int p = G.board[from];
    int pt = PIECE_TYPE(p);
    char buf[16]; int bi=0;

    if (castle) {
        /* king-side or queen-side */
        strcpy(out, (file_of(to)>file_of(from)) ? "O-O" : "O-O-O");
        /* add + or # later */
        return;
    }

    if (pt!=PAWN) buf[bi++]=piece_char[pt];

    /* disambiguation */
    if (pt!=PAWN) {
        int ambig_rank=0, ambig_file=0, ambig=0;
        for (int s=0;s<64;s++) {
            if (s==from) continue;
            if (G.board[s]!=p) continue;
            gen_legal_from(s);
            for (int i=0;i<legal_count;i++) {
                if (legal_buf[i].to==to) {
                    ambig++;
                    if (file_of(s)==file_of(from)) ambig_rank=1;
                    else ambig_file=1;
                }
            }
        }
        if (ambig) {
            if (!ambig_rank) buf[bi++]=file_char[file_of(from)];
            else if (!ambig_file) buf[bi++]='1'+rank_of(from);
            else { buf[bi++]=file_char[file_of(from)]; buf[bi++]='1'+rank_of(from); }
        }
    }

    if (pt==PAWN && (captured||ep_cap>=0)) buf[bi++]=file_char[file_of(from)];
    if (captured||ep_cap>=0) buf[bi++]='x';
    buf[bi++]=file_char[file_of(to)];
    buf[bi++]='1'+rank_of(to);
    if (promo) { buf[bi++]='='; buf[bi++]=piece_char[promo]; }
    buf[bi]=0;

    /* Check / checkmate – apply move temporarily */
    int board2[64]; memcpy(board2,G.board,sizeof(board2));
    board2[to] = promo?(G.side|promo):board2[from];
    board2[from]=EMPTY;
    if (ep_cap>=0) board2[ep_cap]=EMPTY;
    int opp=(G.side==WHITE)?BLACK:WHITE;
    int opp_in_check = 0;
    {
        int ks=0;
        for (int i=0;i<64;i++) if (board2[i]==(opp|KING)) { ks=i; break; }
        opp_in_check = attacks(board2, ks, G.side);
    }
    /* count opp legal moves */
    if (opp_in_check) {
        /* approximate – just add + or # */
        /* full count would require swapping side temporarily */
        buf[bi++]='+'; buf[bi]=0;
    }
    strcpy(out, buf);
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  Apply / undo move                                                         */
/* ────────────────────────────────────────────────────────────────────────── */
static void apply_move(TryMove *tm, int promo) {
    if (G.hist_count>=MAX_MOVES-1) return;
    int from=tm->from, to=tm->to;
    int p=G.board[from];
    int cap=G.board[to];
    int ep_cap_sq = tm->ep_captured; /* square of ep-captured pawn */

    Move *m = &G.history[G.hist_count];
    m->from=from; m->to=to;
    m->piece=p; m->captured=cap;
    m->promotion=promo;
    m->ep_square=G.ep_square;
    m->castle_rights=G.castle_rights;
    m->ep_captured_sq=ep_cap_sq;
    m->is_castle=tm->castle;
    m->rook_from=tm->rook_from; m->rook_to=tm->rook_to;

    /* UCI string */
    m->uci[0]=file_char[file_of(from)];
    m->uci[1]='1'+rank_of(from);
    m->uci[2]=file_char[file_of(to)];
    m->uci[3]='1'+rank_of(to);
    if (promo) { m->uci[4]=tolower(piece_char[promo]); m->uci[5]=0; }
    else m->uci[4]=0;

    /* SAN */
    build_san(from,to,promo,cap,ep_cap_sq,tm->castle,tm->rook_from,m->san);

    /* update check/mate suffix on san */
    /* (done approximately in build_san) */

    /* apply */
    G.board[to] = promo?(G.side|promo):p;
    G.board[from]=EMPTY;
    if (ep_cap_sq>=0) G.board[ep_cap_sq]=EMPTY;
    if (tm->castle) { G.board[tm->rook_to]=G.board[tm->rook_from]; G.board[tm->rook_from]=EMPTY; }

    /* update ep square */
    G.ep_square=-1;
    if (PIECE_TYPE(p)==PAWN && abs(rank_of(to)-rank_of(from))==2) {
        G.ep_square=sq((rank_of(from)+rank_of(to))/2, file_of(from));
    }

    /* update castling rights */
    if (PIECE_TYPE(p)==KING) {
        if (G.side==WHITE) G.castle_rights &= ~(CASTLE_WK|CASTLE_WQ);
        else               G.castle_rights &= ~(CASTLE_BK|CASTLE_BQ);
    }
    if (from==sq(0,0)||to==sq(0,0)) G.castle_rights &= ~CASTLE_WQ;
    if (from==sq(0,7)||to==sq(0,7)) G.castle_rights &= ~CASTLE_WK;
    if (from==sq(7,0)||to==sq(7,0)) G.castle_rights &= ~CASTLE_BQ;
    if (from==sq(7,7)||to==sq(7,7)) G.castle_rights &= ~CASTLE_BK;

    G.halfmove = (PIECE_TYPE(p)==PAWN||cap) ? 0 : G.halfmove+1;
    if (G.side==BLACK) G.fullmove++;

    G.last_from=from; G.last_to=to;
    G.hist_count++;

    /* PGN */
    if (G.side==WHITE) {
        snprintf(G.pgn_moves[G.pgn_count],16,"%d.%s",G.fullmove,m->san);
    } else {
        snprintf(G.pgn_moves[G.pgn_count],16,"%s",m->san);
    }
    G.pgn_count++;

    G.side = (G.side==WHITE)?BLACK:WHITE;
    G.selected=-1; G.n_legal=0;

    /* check game over */
    int al=count_all_legal();
    int ic=in_check(G.board,G.side);
    if (al==0) {
        G.game_over=1;
        if (ic) {
            strcpy(G.result, (G.side==WHITE)?"0-1":"1-0");
            snprintf(G.status,sizeof(G.status),"Checkmate! %s", G.result);
            /* fix san to use # */
            char *s=G.history[G.hist_count-1].san;
            int l=strlen(s);
            if (l>0&&s[l-1]=='+') s[l-1]='#';
            else { s[l]='#'; s[l+1]=0; }
            /* fix pgn */
            char *pg=G.pgn_moves[G.pgn_count-1];
            l=strlen(pg);
            if (l>0&&pg[l-1]=='+') pg[l-1]='#';
            else { pg[l]='#'; pg[l+1]=0; }
        } else {
            strcpy(G.result,"1/2-1/2");
            snprintf(G.status,sizeof(G.status),"Stalemate! Draw.");
        }
    } else if (ic) {
        snprintf(G.status,sizeof(G.status),"Check!");
    } else if (G.halfmove>=100) {
        G.game_over=1; strcpy(G.result,"1/2-1/2");
        snprintf(G.status,sizeof(G.status),"Draw by 50-move rule.");
    } else {
        snprintf(G.status,sizeof(G.status),"%s to move.", (G.side==WHITE)?"White":"Black");
    }
}

static void undo_move(void) {
    if (G.hist_count==0) { strcpy(G.status,"Nothing to undo."); return; }
    /* if engine just moved, undo twice */
    int undo_count=1;
    if (G.engine_active && G.engine_color!=(-1) && G.hist_count>=2) undo_count=2;
    for (int u=0;u<undo_count&&G.hist_count>0;u++) {
        G.hist_count--;
        G.pgn_count--;
        Move *m=&G.history[G.hist_count];
        G.side=(G.side==WHITE)?BLACK:WHITE;
        G.board[m->from]=m->piece;
        G.board[m->to]=m->captured;
        if (m->ep_captured_sq>=0) {
            int opp=(G.side==WHITE)?BLACK:WHITE;
            G.board[m->ep_captured_sq]=opp|PAWN;
        }
        if (m->is_castle) {
            G.board[m->rook_from]=G.board[m->rook_to];
            G.board[m->rook_to]=EMPTY;
        }
        G.ep_square=m->ep_square;
        G.castle_rights=m->castle_rights;
        if (G.side==BLACK) G.fullmove--;
    }
    G.game_over=0;
    G.selected=-1; G.n_legal=0;
    G.last_from = (G.hist_count>0)?G.history[G.hist_count-1].from:-1;
    G.last_to   = (G.hist_count>0)?G.history[G.hist_count-1].to:-1;
    snprintf(G.status,sizeof(G.status),"Undo. %s to move.",(G.side==WHITE)?"White":"Black");
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  FEN builder (for UCI)                                                     */
/* ────────────────────────────────────────────────────────────────────────── */
static void build_fen(char *fen) {
    int bi=0;
    for (int r=7;r>=0;r--) {
        int empty=0;
        for (int f=0;f<8;f++) {
            int p=G.board[sq(r,f)];
            if (!p) { empty++; continue; }
            if (empty) { fen[bi++]='0'+empty; empty=0; }
            int pt=PIECE_TYPE(p); int wh=IS_WHITE(p);
            char c=piece_char[pt];
            fen[bi++]=wh?c:tolower(c);
        }
        if (empty) fen[bi++]='0'+empty;
        if (r>0) fen[bi++]='/';
    }
    fen[bi++]=' ';
    fen[bi++]=(G.side==WHITE)?'w':'b';
    fen[bi++]=' ';
    int any=0;
    if (G.castle_rights&CASTLE_WK){fen[bi++]='K';any=1;}
    if (G.castle_rights&CASTLE_WQ){fen[bi++]='Q';any=1;}
    if (G.castle_rights&CASTLE_BK){fen[bi++]='k';any=1;}
    if (G.castle_rights&CASTLE_BQ){fen[bi++]='q';any=1;}
    if (!any) fen[bi++]='-';
    fen[bi++]=' ';
    if (G.ep_square>=0) {
        fen[bi++]=file_char[file_of(G.ep_square)];
        fen[bi++]='1'+rank_of(G.ep_square);
    } else fen[bi++]='-';
    bi+=sprintf(fen+bi," %d %d",G.halfmove,G.fullmove);
    fen[bi]=0;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  UCI engine interface                                                      */
/* ────────────────────────────────────────────────────────────────────────── */
static void eng_write(const char *s) {
    if (!G.engine_active) return;
    write(G.eng_in[1],s,strlen(s));
}

static int eng_readline(char *buf, int sz, int timeout_ms) {
    if (!G.engine_active) return 0;
    int idx=0;
    struct timeval tv;
    while (idx<sz-1) {
        fd_set fds; FD_ZERO(&fds); FD_SET(G.eng_out[0],&fds);
        tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
        int r=select(G.eng_out[0]+1,&fds,NULL,NULL,&tv);
        if (r<=0) break;
        char c;
        if (read(G.eng_out[0],&c,1)!=1) break;
        if (c=='\n') { buf[idx]=0; return 1; }
        buf[idx++]=c;
    }
    buf[idx]=0;
    return idx>0;
}

static int start_engine(const char *path) {
    if (G.engine_active) {
        eng_write("quit\n");
        usleep(100000);
        kill(G.eng_pid,SIGTERM);
        waitpid(G.eng_pid,NULL,WNOHANG);
        close(G.eng_in[0]); close(G.eng_in[1]);
        close(G.eng_out[0]); close(G.eng_out[1]);
        G.engine_active=0;
    }
    if (!path||!path[0]) return 0;
    if (pipe(G.eng_in)<0||pipe(G.eng_out)<0) return 0;
    G.eng_pid=fork();
    if (G.eng_pid<0) return 0;
    if (G.eng_pid==0) {
        dup2(G.eng_in[0],STDIN_FILENO);
        dup2(G.eng_out[1],STDOUT_FILENO);
        close(G.eng_in[1]); close(G.eng_out[0]);
        close(STDERR_FILENO);
        int fd=open("/dev/null",O_WRONLY);
        if (fd>=0) dup2(fd,STDERR_FILENO);
        execlp(path,path,(char*)NULL);
        exit(1);
    }
    close(G.eng_in[0]); close(G.eng_out[1]);
    /* set non-blocking */
    fcntl(G.eng_out[0],F_SETFL,O_NONBLOCK);
    G.engine_active=1;
    strncpy(G.engine_path,path,255);
    eng_write("uci\n");
    /* drain until uciok */
    char buf[512]; int tries=50;
    while (tries--) { if (eng_readline(buf,sizeof(buf),100)&&strstr(buf,"uciok")) break; }
    eng_write("isready\n");
    tries=50;
    while (tries--) { if (eng_readline(buf,sizeof(buf),100)&&strstr(buf,"readyok")) break; }
    eng_write("ucinewgame\n");
    return 1;
}

static void engine_request_move(void) {
    if (!G.engine_active) return;
    char cmd[4096];
    /* build position command */
    char fen[128]; build_fen(fen);
    int n=snprintf(cmd,sizeof(cmd),"position fen %s",fen);
    if (G.hist_count>0) {
        n+=snprintf(cmd+n,sizeof(cmd)-n," moves");
        for (int i=0;i<G.hist_count;i++)
            n+=snprintf(cmd+n,sizeof(cmd)-n," %s",G.history[i].uci);
    }
    strcat(cmd,"\n"); eng_write(cmd);

    /* go command */
    switch(G.tc_mode) {
    case 0: snprintf(cmd,sizeof(cmd),"go nodes %d\n",G.tc_nodes); break;
    case 1: snprintf(cmd,sizeof(cmd),"go depth %d\n",G.tc_depth); break;
    case 2: snprintf(cmd,sizeof(cmd),"go movetime %d\n",G.tc_time_ms); break;
    default: snprintf(cmd,sizeof(cmd),"go movetime 1000\n"); break;
    }
    eng_write(cmd);
}

/* Parse bestmove and apply */
static void engine_poll(void) {
    if (!G.engine_active) return;
    char buf[512];
    while (eng_readline(buf,sizeof(buf),0)) {
        if (strncmp(buf,"bestmove",8)==0) {
            char *mv=buf+9;
            /* parse uci move: e2e4 or e7e8q */
            if (strlen(mv)<4) continue;
            int ff2=mv[0]-'a', rf2=mv[1]-'1';
            int ft=mv[2]-'a', rt=mv[3]-'1';
            if (ff2<0||ff2>7||rf2<0||rf2>7||ft<0||ft>7||rt<0||rt>7) continue;
            int from2=sq(rf2,ff2), to2=sq(rt,ft);
            int promo2=0;
            if (mv[4]&&mv[4]!='\n'&&mv[4]!=' ') {
                char pc=mv[4];
                for (int i=1;i<=6;i++) if (tolower(piece_char[i])==pc){promo2=i;break;}
            }
            /* find matching legal move */
            gen_legal_from(from2);
            for (int i=0;i<legal_count;i++) {
                if (legal_buf[i].to==to2) {
                    TryMove tm=legal_buf[i].tm;
                    int need_promo=(tm.promo!=0);
                    if (need_promo && !promo2) promo2=QUEEN;
                    if (!need_promo) promo2=0;
                    apply_move(&tm,promo2);
                    break;
                }
            }
        }
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  Rendering                                                                 */
/* ────────────────────────────────────────────────────────────────────────── */
#define BOARD_ROW 2
#define BOARD_COL 4
#define SQ_W 4     /* chars per square width */
#define SQ_H 2     /* rows per square height */

static int display_sq(int sq_idx) {
    /* returns the display square given flip */
    return G.flipped ? (63-sq_idx) : sq_idx;
}

static void draw_board(void) {
    /* pre-build target set */
    int targets[64]={0};
    for (int i=0;i<G.n_legal;i++) targets[G.legal_targets[i]]=1;

    for (int display_r=7;display_r>=0;display_r--) {
        int board_r = G.flipped ? (7-display_r) : display_r;
        for (int half=0;half<SQ_H;half++) {
            int row = BOARD_ROW + (7-display_r)*SQ_H + half;
            goto_xy(row, BOARD_COL);
            /* rank label */
            if (half==0) printf("%s%d%s",FG_LABEL,board_r+1,RESET);
            else printf("  ");
            for (int display_f=0;display_f<8;display_f++) {
                int board_f = G.flipped ? (7-display_f) : display_f;
                int s = sq(board_r, board_f);
                int is_light=((board_r+board_f)%2==1);
                int p=G.board[s];

                /* choose background */
                const char *bg;
                if (s==G.selected)                       bg=BG_SEL;
                else if (targets[s] && G.selected>=0)    bg=BG_MOVE;
                else if (s==G.last_from||s==G.last_to)   bg=BG_LAST;
                else if (p==(G.side|KING) && in_check(G.board,G.side) &&
                         PIECE_TYPE(p)==KING)             bg=BG_CHECK;
                else if (is_light)                        bg=BG_LIGHT;
                else                                      bg=BG_DARK;

                printf("%s", bg);

                /* cursor indicator */
                int is_cursor=(s==G.cursor);

                if (half==0) {
                    /* top half of square: show cursor brackets or spaces */
                    if (is_cursor) printf(BOLD"[");
                    else printf(" ");
                    /* piece */
                    if (p) {
                        int wh=IS_WHITE(p);
                        printf("%s%s%s", wh?FG_WHITE_P:FG_BLACK_P,
                               piece_unicode[wh?0:1][PIECE_TYPE(p)], bg);
                    } else {
                        /* legal move dot */
                        if (targets[s]&&G.selected>=0) printf("· ");
                        else printf("  ");
                    }
                    if (is_cursor) printf(BOLD"]"RESET"%s",bg);
                    else printf(" ");
                } else {
                    printf("    ");
                }
                printf("%s",RESET);
            }
            /* right edge */
            printf("\n");
        }
    }
    /* file labels */
    goto_xy(BOARD_ROW+16, BOARD_COL+2);
    printf("%s",FG_LABEL);
    for (int f=0;f<8;f++) {
        int bf = G.flipped?(7-f):f;
        printf("  %c ", 'a'+bf);
    }
    printf("%s\n",RESET);
}

#define INFO_COL 42
static void draw_info(void) {
    int row=BOARD_ROW;

    /* Title */
    goto_xy(row,INFO_COL);
    printf(BOLD"♟  Terminal Chess ♟"RESET);
    row+=2;

    /* Status */
    goto_xy(row,INFO_COL);
    printf("%s%s%-36s%s",BG_STATUS,FG_STATUS,G.status,RESET);
    row+=2;

    /* Engine info */
    goto_xy(row,INFO_COL);
    if (G.engine_active) {
        char side[8]; 
        if (G.engine_color==WHITE) strcpy(side,"White");
        else if (G.engine_color==BLACK) strcpy(side,"Black");
        else strcpy(side,"Off");
        printf("%sEngine: %.20s [%s]%s",FG_LABEL,G.engine_path,side,RESET);
    } else {
        printf("%sEngine: None%s",FG_LABEL,RESET);
    }
    row++;

    /* Time control */
    goto_xy(row,INFO_COL);
    const char *tcnames[]={"Nodes","Depth","Time(ms)"};
    printf("%sTC: %s = %d%s",FG_LABEL,tcnames[G.tc_mode],
           G.tc_mode==0?G.tc_nodes:G.tc_mode==1?G.tc_depth:G.tc_time_ms,RESET);
    row+=2;

    /* PGN moves */
    goto_xy(row,INFO_COL);
    printf(BOLD"Moves:"RESET); row++;
    /* display last 14 move-tokens */
    int start = G.pgn_count>14 ? G.pgn_count-14 : 0;
    for (int i=start;i<G.pgn_count;i++) {
        goto_xy(row,INFO_COL);
        if (i==G.pgn_count-1)
            printf(BOLD"\033[38;5;220m%-18s"RESET,G.pgn_moves[i]);
        else
            printf("%-18s",G.pgn_moves[i]);
        row++;
        if (row>BOARD_ROW+18) break;
    }

    /* Controls help */
    row=BOARD_ROW+14;
    goto_xy(row,INFO_COL);
    printf("%s── Controls ────────────────────%s",FG_LABEL,RESET); row++;
    goto_xy(row,INFO_COL); printf("%s←↑↓→ Move cursor | Enter Select%s",FG_LABEL,RESET); row++;
    goto_xy(row,INFO_COL); printf("%su Undo | f Flip | n New game%s",FG_LABEL,RESET); row++;
    goto_xy(row,INFO_COL); printf("%se Set engine | c Time control%s",FG_LABEL,RESET); row++;
    goto_xy(row,INFO_COL); printf("%sw Engine color | q Quit%s",FG_LABEL,RESET); row++;
}

static void render(void) {
    printf(CLEAR_SCREEN HOME);
    draw_board();
    draw_info();
    fflush(stdout);
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  Input helpers                                                             */
/* ────────────────────────────────────────────────────────────────────────── */
static int read_char(void) {
    unsigned char c=0;
    if (read(STDIN_FILENO,&c,1)==1) return c;
    return -1;
}

/* prompt for string input in-place */
static void prompt_string(const char *prompt, char *buf, int sz) {
    term_restore();
    show_cursor();
    goto_xy(BOARD_ROW+18,1);
    printf("\033[2K"); /* clear line */
    printf("%s%s%s: ",BOLD,prompt,RESET);
    fflush(stdout);
    if (fgets(buf,sz,stdin)) {
        int l=strlen(buf);
        if (l>0&&buf[l-1]=='\n') buf[l-1]=0;
    } else buf[0]=0;
    term_raw();
    hide_cursor();
}

static void prompt_int(const char *prompt, int *val) {
    char buf[32]; prompt_string(prompt,buf,sizeof(buf));
    if (buf[0]) *val=atoi(buf);
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  Promotion menu                                                            */
/* ────────────────────────────────────────────────────────────────────────── */
static int promotion_menu(void) {
    /* draw simple overlay */
    goto_xy(BOARD_ROW+18,1);
    printf("\033[2KPromote to: Q)ueen N)knight R)ook B)ishop > ");
    fflush(stdout);
    while(1) {
        int c=read_char();
        switch(tolower(c)) {
        case 'q': return QUEEN;
        case 'n': return KNIGHT;
        case 'r': return ROOK;
        case 'b': return BISHOP;
        }
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  Handle selection / move                                                   */
/* ────────────────────────────────────────────────────────────────────────── */
static void do_select_or_move(void) {
    int s=G.cursor;
    if (G.selected<0) {
        /* select a piece */
        int p=G.board[s];
        if (!p||PIECE_COLOR(p)!=G.side) {
            snprintf(G.status,sizeof(G.status),"No %s piece there.",
                     G.side==WHITE?"white":"black");
            return;
        }
        G.selected=s;
        gen_legal_from(s);
        G.n_legal=legal_count;
        for (int i=0;i<legal_count;i++) G.legal_targets[i]=legal_buf[i].to;
        snprintf(G.status,sizeof(G.status),"Piece selected. %d legal moves.",G.n_legal);
    } else {
        if (s==G.selected) { G.selected=-1; G.n_legal=0; strcpy(G.status,"Deselected."); return; }
        /* check if target is legal */
        gen_legal_from(G.selected);
        int found=-1;
        for (int i=0;i<legal_count;i++) {
            if (legal_buf[i].to==s) { found=i; break; }
        }
        if (found<0) {
            /* maybe select another own piece */
            int p=G.board[s];
            if (p&&PIECE_COLOR(p)==G.side) {
                G.selected=s;
                gen_legal_from(s);
                G.n_legal=legal_count;
                for (int i=0;i<legal_count;i++) G.legal_targets[i]=legal_buf[i].to;
                snprintf(G.status,sizeof(G.status),"Piece selected. %d legal moves.",G.n_legal);
            } else {
                G.selected=-1; G.n_legal=0;
                snprintf(G.status,sizeof(G.status),"Illegal move.");
            }
            return;
        }
        TryMove tm=legal_buf[found].tm;
        int promo=0;
        if (tm.promo) {
            promo=promotion_menu();
            /* find the move with this promotion */
            for (int i=0;i<legal_count;i++) {
                if (legal_buf[i].to==s&&legal_buf[i].tm.promo==promo) { tm=legal_buf[i].tm; break; }
            }
        }
        apply_move(&tm,promo);
        /* if engine's turn, request move */
        if (!G.game_over && G.engine_active && G.engine_color==G.side) {
            snprintf(G.status,sizeof(G.status),"Engine thinking...");
            render();
            engine_request_move();
        }
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  Time control menu                                                         */
/* ────────────────────────────────────────────────────────────────────────── */
static void tc_menu(void) {
    goto_xy(BOARD_ROW+18,1);
    printf("\033[2KTime control: 0=Nodes 1=Depth 2=Time(ms) [current=%d] > ",G.tc_mode);
    fflush(stdout);
    /* read single char */
    while(1){
        int c=read_char();
        if(c=='0'){G.tc_mode=0;break;}
        if(c=='1'){G.tc_mode=1;break;}
        if(c=='2'){G.tc_mode=2;break;}
        if(c=='q'||c==27)return;
    }
    switch(G.tc_mode){
    case 0: prompt_int("Nodes",&G.tc_nodes); break;
    case 1: prompt_int("Depth",&G.tc_depth); break;
    case 2: prompt_int("Time (ms)",&G.tc_time_ms); break;
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  Engine color menu                                                         */
/* ────────────────────────────────────────────────────────────────────────── */
static void engine_color_menu(void) {
    goto_xy(BOARD_ROW+18,1);
    printf("\033[2KEngine plays: w=White b=Black n=None > ");
    fflush(stdout);
    while(1){
        int c=read_char();
        if(c=='w'){G.engine_color=WHITE;break;}
        if(c=='b'){G.engine_color=BLACK;break;}
        if(c=='n'){G.engine_color=-1;break;}
        if(c=='q'||c==27)return;
    }
    /* if engine's turn now, request move */
    if (!G.game_over && G.engine_active && G.engine_color==G.side) {
        snprintf(G.status,sizeof(G.status),"Engine thinking...");
        render();
        engine_request_move();
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  Main loop                                                                 */
/* ────────────────────────────────────────────────────────────────────────── */
int main(void) {
    /* init game */
    memset(&G,0,sizeof(G));
    board_start();
    G.tc_mode=1; G.tc_depth=10; G.tc_nodes=100000; G.tc_time_ms=1000;
    G.engine_color=-1;

    term_raw();
    hide_cursor();
    atexit(term_restore);
    atexit(show_cursor);

    render();

    /* engine poll timer */
    int engine_waiting=0;

    while(1) {
        /* poll engine */
        if (G.engine_active && engine_waiting) {
            engine_poll();
            if (G.hist_count>0) {
                /* check if the last move was by engine */
                int last_side=(G.side==WHITE)?BLACK:WHITE;
                if (last_side==G.engine_color) engine_waiting=0;
            }
            render();
        }

        /* non-blocking key read */
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO,&fds);
        struct timeval tv={0,50000}; /* 50ms */
        int r=select(STDIN_FILENO+1,&fds,NULL,NULL,&tv);
        if (r<=0) {
            /* no input; if engine is thinking, poll */
            if (G.engine_active && !G.game_over && G.engine_color==G.side && !engine_waiting) {
                engine_waiting=1;
            }
            if (engine_waiting) { engine_poll(); render(); }
            continue;
        }

        int c=read_char();
        if (c<0) continue;

        if (c==27) { /* escape sequence */
            int c2=read_char();
            if (c2=='[') {
                int c3=read_char();
                int cr=rank_of(G.cursor), cf=file_of(G.cursor);
                switch(c3) {
                case 'A': /* up */
                    if (G.flipped) { if (cr>0) G.cursor=sq(cr-1,cf); }
                    else           { if (cr<7) G.cursor=sq(cr+1,cf); }
                    break;
                case 'B': /* down */
                    if (G.flipped) { if (cr<7) G.cursor=sq(cr+1,cf); }
                    else           { if (cr>0) G.cursor=sq(cr-1,cf); }
                    break;
                case 'C': /* right */
                    if (G.flipped) { if (cf>0) G.cursor=sq(cr,cf-1); }
                    else           { if (cf<7) G.cursor=sq(cr,cf+1); }
                    break;
                case 'D': /* left */
                    if (G.flipped) { if (cf<7) G.cursor=sq(cr,cf+1); }
                    else           { if (cf>0) G.cursor=sq(cr,cf-1); }
                    break;
                }
            }
        } else switch(c) {
        case '\r': case '\n': case ' ':
            if (!G.game_over && !(G.engine_active && G.engine_color==G.side))
                do_select_or_move();
            else if (G.game_over) strcpy(G.status,"Game over. Press 'n' for new game.");
            /* after human move, trigger engine if needed */
            if (!G.game_over && G.engine_active && G.engine_color==G.side) {
                engine_waiting=1;
                snprintf(G.status,sizeof(G.status),"Engine thinking...");
                engine_request_move();
            }
            break;
        case 'u': case 'U':
            undo_move();
            engine_waiting=0;
            break;
        case 'f': case 'F':
            G.flipped=!G.flipped;
            snprintf(G.status,sizeof(G.status),"Board %s.",(G.flipped)?"flipped":"normal");
            break;
        case 'n': case 'N':
            board_start();
            engine_waiting=0;
            if (G.engine_active) eng_write("ucinewgame\n");
            /* if engine plays white, start immediately */
            if (!G.game_over && G.engine_active && G.engine_color==G.side) {
                engine_waiting=1;
                snprintf(G.status,sizeof(G.status),"Engine thinking...");
                engine_request_move();
            }
            break;
        case 'e': case 'E': {
            char path[256]={0};
            prompt_string("Engine path (empty to disable)",path,sizeof(path));
            if (!path[0]) {
                if (G.engine_active) {
                    eng_write("quit\n"); usleep(100000);
                    kill(G.eng_pid,SIGTERM);
                    waitpid(G.eng_pid,NULL,WNOHANG);
                    close(G.eng_in[0]); close(G.eng_in[1]);
                    close(G.eng_out[0]); close(G.eng_out[1]);
                    G.engine_active=0;
                }
                strcpy(G.status,"Engine disabled.");
            } else {
                if (start_engine(path))
                    snprintf(G.status,sizeof(G.status),"Engine loaded: %s",path);
                else
                    snprintf(G.status,sizeof(G.status),"Failed to load engine.");
            }
            break;
        }
        case 'c': case 'C':
            tc_menu();
            break;
        case 'w': case 'W':
            if (G.engine_active) engine_color_menu();
            else strcpy(G.status,"Load engine first (press 'e').");
            break;
        case 'q': case 'Q':
            if (G.engine_active) { eng_write("quit\n"); usleep(100000); kill(G.eng_pid,SIGTERM); }
            printf(CLEAR_SCREEN HOME RESET);
            show_cursor();
            term_restore();
            printf("Thanks for playing!\n");
            exit(0);
        }
        render();
    }
    return 0;
}
