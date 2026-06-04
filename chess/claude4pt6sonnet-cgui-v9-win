/*
 * chess_gui.c - Terminal Chess GUI for Mac
 * 
 * Compile: gcc -o chess_gui chess_gui.c
 * Run:     ./chess_gui [engine_path]
 *
 * Controls:
 *   Arrow Keys  - Move cursor
 *   Enter/Space - Select/Move piece
 *   U           - Undo last move
 *   Q           - Quit
 *   N           - New game
 *   F           - Flip board
 *   H           - Help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>

/* ─────────────────────────────────────────────────────────────────────────────
   ANSI / Terminal helpers
───────────────────────────────────────────────────────────────────────────── */

#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define DIM         "\033[2m"

/* 256-colour backgrounds */
#define BG_LIGHT    "\033[48;5;223m"   /* light square  */
#define BG_DARK     "\033[48;5;136m"   /* dark  square  */
#define BG_SELECT   "\033[48;5;226m"   /* selected      */
#define BG_MOVE     "\033[48;5;154m"   /* valid move    */
#define BG_CURSOR   "\033[48;5;39m"    /* cursor        */
#define BG_CHECK    "\033[48;5;196m"   /* king in check */
#define BG_LAST     "\033[48;5;178m"   /* last move     */

/* Foreground colours for pieces */
#define FG_WHITE    "\033[38;5;255m"
#define FG_BLACK    "\033[38;5;232m"

#define HIDE_CURSOR "\033[?25l"
#define SHOW_CURSOR "\033[?25h"
#define CLEAR       "\033[2J\033[H"
#define SAVE_POS    "\033[s"
#define REST_POS    "\033[u"

static void move_to(int row, int col) { printf("\033[%d;%dH", row, col); }
static void clear_screen(void)        { printf(CLEAR); fflush(stdout); }

/* ─────────────────────────────────────────────────────────────────────────────
   Chess constants & types
───────────────────────────────────────────────────────────────────────────── */

typedef unsigned long long U64;

#define EMPTY  0
#define WP 1
#define WN 2
#define WB 3
#define WR 4
#define WQ 5
#define WK 6
#define BP 7
#define BN 8
#define BB 9
#define BR 10
#define BQ 11
#define BK 12

#define WHITE 0
#define BLACK 1

#define MAX_MOVES   256
#define MAX_HISTORY 512
#define MAX_PGN     8192

/* Castling rights bits */
#define CASTLE_WK 1
#define CASTLE_WQ 2
#define CASTLE_BK 4
#define CASTLE_BQ 8

/* Move flags */
#define FLAG_NONE      0
#define FLAG_CASTLE_KS 1
#define FLAG_CASTLE_QS 2
#define FLAG_EP        3
#define FLAG_PROMO_Q   4
#define FLAG_PROMO_R   5
#define FLAG_PROMO_B   6
#define FLAG_PROMO_N   7

typedef struct {
    int from, to;       /* 0-63 */
    int piece;          /* moving piece */
    int captured;       /* captured piece (EMPTY if none) */
    int flag;
    int castle_rights;  /* rights BEFORE move */
    int ep_sq;          /* en-passant square BEFORE move */
    int halfmove;       /* halfmove clock BEFORE move */
} Move;

typedef struct {
    int  board[64];
    int  side;           /* WHITE / BLACK */
    int  castle;         /* castling rights */
    int  ep;             /* en-passant square (-1 = none) */
    int  halfmove;       /* fifty-move counter */
    int  fullmove;
    Move history[MAX_HISTORY];
    int  hist_cnt;
    char pgn[MAX_PGN];
    int  pgn_len;
    /* For PGN move numbering */
    int  move_num;
} Board;

/* ─────────────────────────────────────────────────────────────────────────────
   Piece display (UTF-8 chess pieces)
───────────────────────────────────────────────────────────────────────────── */

static const char *PIECE_UTF8[13] = {
    "  ",               /* EMPTY */
    "\xe2\x99\x99 ",   /* WP ♙ */
    "\xe2\x99\x98 ",   /* WN ♘ */
    "\xe2\x99\x97 ",   /* WB ♗ */
    "\xe2\x99\x96 ",   /* WR ♖ */
    "\xe2\x99\x95 ",   /* WQ ♕ */
    "\xe2\x99\x94 ",   /* WK ♔ */
    "\xe2\x99\x9f ",   /* BP ♟ */
    "\xe2\x99\x9e ",   /* BN ♞ */
    "\xe2\x99\x9d ",   /* BB ♝ */
    "\xe2\x99\x9c ",   /* BR ♜ */
    "\xe2\x99\x9b ",   /* BQ ♛ */
    "\xe2\x99\x9a ",   /* BK ♚ */
};

static const char PIECE_CHAR[13] = {
    '.','P','N','B','R','Q','K','p','n','b','r','q','k'
};

static inline int piece_color(int p) { return p >= BP ? BLACK : WHITE; }
static inline int is_white(int p)    { return p >= WP && p <= WK; }
static inline int is_black(int p)    { return p >= BP && p <= BK; }

/* ─────────────────────────────────────────────────────────────────────────────
   Square helpers
───────────────────────────────────────────────────────────────────────────── */

static inline int sq(int r, int c)   { return r*8+c; }
static inline int sq_rank(int s)     { return s/8; }
static inline int sq_file(int s)     { return s%8; }

static void sq_name(int s, char *buf) {
    buf[0] = 'a' + sq_file(s);
    buf[1] = '1' + sq_rank(s);
    buf[2] = '\0';
}

/* ─────────────────────────────────────────────────────────────────────────────
   Board initialisation
───────────────────────────────────────────────────────────────────────────── */

static void board_init(Board *b) {
    memset(b, 0, sizeof(Board));
    b->ep = -1;
    b->castle = CASTLE_WK|CASTLE_WQ|CASTLE_BK|CASTLE_BQ;
    b->fullmove = 1;
    b->move_num  = 1;

    /* White pieces */
    int back_w[8] = {WR,WN,WB,WQ,WK,WB,WN,WR};
    for(int c=0;c<8;c++) { b->board[sq(0,c)]=back_w[c]; b->board[sq(1,c)]=WP; }
    /* Black pieces */
    int back_b[8] = {BR,BN,BB,BQ,BK,BB,BN,BR};
    for(int c=0;c<8;c++) { b->board[sq(7,c)]=back_b[c]; b->board[sq(6,c)]=BP; }

    strcpy(b->pgn, "");
    b->pgn_len = 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Attack / move generation helpers
───────────────────────────────────────────────────────────────────────────── */

static int in_board(int r, int c) { return r>=0&&r<8&&c>=0&&c<8; }

/* Is square s attacked by 'attacker_side'? */
static int sq_attacked(Board *b, int s, int attacker_side) {
    int r = sq_rank(s), c = sq_file(s);

    /* Pawn attacks */
    if(attacker_side == WHITE) {
        if(in_board(r-1,c-1) && b->board[sq(r-1,c-1)]==WP) return 1;
        if(in_board(r-1,c+1) && b->board[sq(r-1,c+1)]==WP) return 1;
    } else {
        if(in_board(r+1,c-1) && b->board[sq(r+1,c-1)]==BP) return 1;
        if(in_board(r+1,c+1) && b->board[sq(r+1,c+1)]==BP) return 1;
    }

    /* Knight */
    int kn[8][2]={{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    int knight = attacker_side==WHITE ? WN : BN;
    for(int i=0;i<8;i++) {
        int nr=r+kn[i][0], nc=c+kn[i][1];
        if(in_board(nr,nc) && b->board[sq(nr,nc)]==knight) return 1;
    }

    /* Rook / Queen (horizontal + vertical) */
    int rq_w[2]={WR,WQ}, rq_b[2]={BR,BQ};
    int *rq = attacker_side==WHITE ? rq_w : rq_b;
    int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
    for(int d=0;d<4;d++) {
        int nr=r+dirs[d][0], nc=c+dirs[d][1];
        while(in_board(nr,nc)) {
            int p=b->board[sq(nr,nc)];
            if(p!=EMPTY) {
                if(p==rq[0]||p==rq[1]) return 1;
                break;
            }
            nr+=dirs[d][0]; nc+=dirs[d][1];
        }
    }

    /* Bishop / Queen (diagonal) */
    int bq_w[2]={WB,WQ}, bq_b[2]={BB,BQ};
    int *bq = attacker_side==WHITE ? bq_w : bq_b;
    int ddiag[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
    for(int d=0;d<4;d++) {
        int nr=r+ddiag[d][0], nc=c+ddiag[d][1];
        while(in_board(nr,nc)) {
            int p=b->board[sq(nr,nc)];
            if(p!=EMPTY) {
                if(p==bq[0]||p==bq[1]) return 1;
                break;
            }
            nr+=ddiag[d][0]; nc+=ddiag[d][1];
        }
    }

    /* King */
    int king = attacker_side==WHITE ? WK : BK;
    for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++) {
        if(!dr&&!dc) continue;
        int nr=r+dr, nc=c+dc;
        if(in_board(nr,nc) && b->board[sq(nr,nc)]==king) return 1;
    }

    return 0;
}

static int king_sq(Board *b, int side) {
    int king = side==WHITE ? WK : BK;
    for(int i=0;i<64;i++) if(b->board[i]==king) return i;
    return -1;
}

static int in_check(Board *b, int side) {
    int ks = king_sq(b, side);
    if(ks<0) return 0;
    return sq_attacked(b, ks, 1-side);
}

/* Generate pseudo-legal moves, return count */
static int gen_moves(Board *b, Move *moves) {
    int cnt=0;
    int side=b->side;

    for(int from=0;from<64;from++) {
        int piece=b->board[from];
        if(piece==EMPTY) continue;
        if(piece_color(piece)!=side) continue;

        int r=sq_rank(from), c=sq_file(from);

#define ADD_MOVE(T,PC,CAP,FL) do { \
    moves[cnt].from=from; moves[cnt].to=(T); \
    moves[cnt].piece=(PC); moves[cnt].captured=(CAP); \
    moves[cnt].flag=(FL); \
    moves[cnt].castle_rights=b->castle; \
    moves[cnt].ep_sq=b->ep; \
    moves[cnt].halfmove=b->halfmove; \
    cnt++; \
} while(0)

        if(piece==WP) {
            /* Single push */
            if(r+1<=7 && b->board[sq(r+1,c)]==EMPTY) {
                if(r+1==7) {
                    ADD_MOVE(sq(r+1,c),WP,EMPTY,FLAG_PROMO_Q);
                    ADD_MOVE(sq(r+1,c),WP,EMPTY,FLAG_PROMO_R);
                    ADD_MOVE(sq(r+1,c),WP,EMPTY,FLAG_PROMO_B);
                    ADD_MOVE(sq(r+1,c),WP,EMPTY,FLAG_PROMO_N);
                } else ADD_MOVE(sq(r+1,c),WP,EMPTY,FLAG_NONE);
                /* Double push */
                if(r==1 && b->board[sq(r+2,c)]==EMPTY)
                    ADD_MOVE(sq(r+2,c),WP,EMPTY,FLAG_NONE);
            }
            /* Captures */
            for(int dc=-1;dc<=1;dc+=2) {
                if(!in_board(r+1,c+dc)) continue;
                int ts=sq(r+1,c+dc);
                if(is_black(b->board[ts])) {
                    if(r+1==7) {
                        ADD_MOVE(ts,WP,b->board[ts],FLAG_PROMO_Q);
                        ADD_MOVE(ts,WP,b->board[ts],FLAG_PROMO_R);
                        ADD_MOVE(ts,WP,b->board[ts],FLAG_PROMO_B);
                        ADD_MOVE(ts,WP,b->board[ts],FLAG_PROMO_N);
                    } else ADD_MOVE(ts,WP,b->board[ts],FLAG_NONE);
                }
                if(ts==b->ep) ADD_MOVE(ts,WP,BP,FLAG_EP);
            }
        } else if(piece==BP) {
            if(r-1>=0 && b->board[sq(r-1,c)]==EMPTY) {
                if(r-1==0) {
                    ADD_MOVE(sq(r-1,c),BP,EMPTY,FLAG_PROMO_Q);
                    ADD_MOVE(sq(r-1,c),BP,EMPTY,FLAG_PROMO_R);
                    ADD_MOVE(sq(r-1,c),BP,EMPTY,FLAG_PROMO_B);
                    ADD_MOVE(sq(r-1,c),BP,EMPTY,FLAG_PROMO_N);
                } else ADD_MOVE(sq(r-1,c),BP,EMPTY,FLAG_NONE);
                if(r==6 && b->board[sq(r-2,c)]==EMPTY)
                    ADD_MOVE(sq(r-2,c),BP,EMPTY,FLAG_NONE);
            }
            for(int dc=-1;dc<=1;dc+=2) {
                if(!in_board(r-1,c+dc)) continue;
                int ts=sq(r-1,c+dc);
                if(is_white(b->board[ts])) {
                    if(r-1==0) {
                        ADD_MOVE(ts,BP,b->board[ts],FLAG_PROMO_Q);
                        ADD_MOVE(ts,BP,b->board[ts],FLAG_PROMO_R);
                        ADD_MOVE(ts,BP,b->board[ts],FLAG_PROMO_B);
                        ADD_MOVE(ts,BP,b->board[ts],FLAG_PROMO_N);
                    } else ADD_MOVE(ts,BP,b->board[ts],FLAG_NONE);
                }
                if(ts==b->ep) ADD_MOVE(ts,BP,WP,FLAG_EP);
            }
        } else if(piece==WN||piece==BN) {
            int kn[8][2]={{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
            for(int i=0;i<8;i++) {
                int nr=r+kn[i][0], nc=c+kn[i][1];
                if(!in_board(nr,nc)) continue;
                int ts=sq(nr,nc);
                int cap=b->board[ts];
                if(cap==EMPTY||piece_color(cap)!=side)
                    ADD_MOVE(ts,piece,cap,FLAG_NONE);
            }
        } else if(piece==WB||piece==BB) {
            int dd[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
            for(int d=0;d<4;d++) {
                int nr=r+dd[d][0], nc=c+dd[d][1];
                while(in_board(nr,nc)) {
                    int ts=sq(nr,nc); int cap=b->board[ts];
                    if(cap==EMPTY) ADD_MOVE(ts,piece,EMPTY,FLAG_NONE);
                    else { if(piece_color(cap)!=side) ADD_MOVE(ts,piece,cap,FLAG_NONE); break; }
                    nr+=dd[d][0]; nc+=dd[d][1];
                }
            }
        } else if(piece==WR||piece==BR) {
            int dd[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
            for(int d=0;d<4;d++) {
                int nr=r+dd[d][0], nc=c+dd[d][1];
                while(in_board(nr,nc)) {
                    int ts=sq(nr,nc); int cap=b->board[ts];
                    if(cap==EMPTY) ADD_MOVE(ts,piece,EMPTY,FLAG_NONE);
                    else { if(piece_color(cap)!=side) ADD_MOVE(ts,piece,cap,FLAG_NONE); break; }
                    nr+=dd[d][0]; nc+=dd[d][1];
                }
            }
        } else if(piece==WQ||piece==BQ) {
            int dd[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
            for(int d=0;d<8;d++) {
                int nr=r+dd[d][0], nc=c+dd[d][1];
                while(in_board(nr,nc)) {
                    int ts=sq(nr,nc); int cap=b->board[ts];
                    if(cap==EMPTY) ADD_MOVE(ts,piece,EMPTY,FLAG_NONE);
                    else { if(piece_color(cap)!=side) ADD_MOVE(ts,piece,cap,FLAG_NONE); break; }
                    nr+=dd[d][0]; nc+=dd[d][1];
                }
            }
        } else if(piece==WK||piece==BK) {
            for(int dr=-1;dr<=1;dr++) for(int dc2=-1;dc2<=1;dc2++) {
                if(!dr&&!dc2) continue;
                int nr=r+dr, nc=c+dc2;
                if(!in_board(nr,nc)) continue;
                int ts=sq(nr,nc); int cap=b->board[ts];
                if(cap==EMPTY||piece_color(cap)!=side)
                    ADD_MOVE(ts,piece,cap,FLAG_NONE);
            }
            /* Castling */
            if(side==WHITE && from==sq(0,4)) {
                if((b->castle&CASTLE_WK) &&
                   b->board[sq(0,5)]==EMPTY && b->board[sq(0,6)]==EMPTY &&
                   !sq_attacked(b,sq(0,4),BLACK) &&
                   !sq_attacked(b,sq(0,5),BLACK) &&
                   !sq_attacked(b,sq(0,6),BLACK))
                    ADD_MOVE(sq(0,6),WK,EMPTY,FLAG_CASTLE_KS);
                if((b->castle&CASTLE_WQ) &&
                   b->board[sq(0,3)]==EMPTY && b->board[sq(0,2)]==EMPTY && b->board[sq(0,1)]==EMPTY &&
                   !sq_attacked(b,sq(0,4),BLACK) &&
                   !sq_attacked(b,sq(0,3),BLACK) &&
                   !sq_attacked(b,sq(0,2),BLACK))
                    ADD_MOVE(sq(0,2),WK,EMPTY,FLAG_CASTLE_QS);
            }
            if(side==BLACK && from==sq(7,4)) {
                if((b->castle&CASTLE_BK) &&
                   b->board[sq(7,5)]==EMPTY && b->board[sq(7,6)]==EMPTY &&
                   !sq_attacked(b,sq(7,4),WHITE) &&
                   !sq_attacked(b,sq(7,5),WHITE) &&
                   !sq_attacked(b,sq(7,6),WHITE))
                    ADD_MOVE(sq(7,6),BK,EMPTY,FLAG_CASTLE_KS);
                if((b->castle&CASTLE_BQ) &&
                   b->board[sq(7,3)]==EMPTY && b->board[sq(7,2)]==EMPTY && b->board[sq(7,1)]==EMPTY &&
                   !sq_attacked(b,sq(7,4),WHITE) &&
                   !sq_attacked(b,sq(7,3),WHITE) &&
                   !sq_attacked(b,sq(7,2),WHITE))
                    ADD_MOVE(sq(7,2),BK,EMPTY,FLAG_CASTLE_QS);
            }
        }
    }
#undef ADD_MOVE
    return cnt;
}

/* Apply move, return 1 if legal (not leaving king in check) */
static int apply_move(Board *b, Move *m) {
    int from=m->from, to=m->to, piece=m->piece, flag=m->flag;

    b->board[from] = EMPTY;
    b->board[to]   = piece;

    /* Handle special flags */
    if(flag==FLAG_EP) {
        int ep_cap = b->side==WHITE ? to-8 : to+8;
        b->board[ep_cap] = EMPTY;
    }
    if(flag==FLAG_CASTLE_KS) {
        if(b->side==WHITE) { b->board[sq(0,7)]=EMPTY; b->board[sq(0,5)]=WR; }
        else               { b->board[sq(7,7)]=EMPTY; b->board[sq(7,5)]=BR; }
    }
    if(flag==FLAG_CASTLE_QS) {
        if(b->side==WHITE) { b->board[sq(0,0)]=EMPTY; b->board[sq(0,3)]=WR; }
        else               { b->board[sq(7,0)]=EMPTY; b->board[sq(7,3)]=BR; }
    }
    /* Promotion */
    if(flag>=FLAG_PROMO_Q) {
        int promo_map[4]={WQ,WR,WB,WN};
        int p=promo_map[flag-FLAG_PROMO_Q];
        if(b->side==BLACK) p+=(BP-WP); /* adjust for black */
        b->board[to]=p;
    }

    /* Check legality */
    if(in_check(b, b->side)) {
        /* Undo */
        b->board[from]=piece;
        b->board[to]=m->captured;
        if(flag==FLAG_EP) {
            int ep_cap = b->side==WHITE ? to-8 : to+8;
            b->board[ep_cap]= b->side==WHITE ? BP : WP;
            b->board[to]=EMPTY;
        }
        if(flag==FLAG_CASTLE_KS) {
            if(b->side==WHITE) { b->board[sq(0,7)]=WR; b->board[sq(0,5)]=EMPTY; }
            else               { b->board[sq(7,7)]=BR; b->board[sq(7,5)]=EMPTY; }
        }
        if(flag==FLAG_CASTLE_QS) {
            if(b->side==WHITE) { b->board[sq(0,0)]=WR; b->board[sq(0,3)]=EMPTY; }
            else               { b->board[sq(7,0)]=BR; b->board[sq(7,3)]=EMPTY; }
        }
        return 0;
    }

    /* Update state */
    b->ep = -1;
    /* En passant square */
    if(piece==WP && sq_rank(to)-sq_rank(from)==2) b->ep=from+8;
    if(piece==BP && sq_rank(from)-sq_rank(to)==2) b->ep=from-8;

    /* Castling rights update */
    if(piece==WK) b->castle &= ~(CASTLE_WK|CASTLE_WQ);
    if(piece==BK) b->castle &= ~(CASTLE_BK|CASTLE_BQ);
    if(from==sq(0,0)||to==sq(0,0)) b->castle &= ~CASTLE_WQ;
    if(from==sq(0,7)||to==sq(0,7)) b->castle &= ~CASTLE_WK;
    if(from==sq(7,0)||to==sq(7,0)) b->castle &= ~CASTLE_BQ;
    if(from==sq(7,7)||to==sq(7,7)) b->castle &= ~CASTLE_BK;

    /* Halfmove clock */
    if(piece==WP||piece==BP||m->captured!=EMPTY) b->halfmove=0;
    else b->halfmove++;

    if(b->side==BLACK) b->fullmove++;
    b->side = 1-b->side;

    return 1;
}

static void undo_move(Board *b) {
    if(b->hist_cnt==0) return;
    Move *m = &b->history[--b->hist_cnt];

    b->side    = 1-b->side;
    b->castle  = m->castle_rights;
    b->ep      = m->ep_sq;
    b->halfmove= m->halfmove;
    if(b->side==BLACK) b->fullmove--;

    b->board[m->from] = m->piece;
    b->board[m->to]   = m->captured;

    if(m->flag==FLAG_EP) {
        int ep_cap = b->side==WHITE ? m->to-8 : m->to+8;
        b->board[ep_cap] = b->side==WHITE ? BP : WP;
        b->board[m->to]  = EMPTY;
    }
    if(m->flag==FLAG_CASTLE_KS) {
        if(b->side==WHITE) { b->board[sq(0,7)]=WR; b->board[sq(0,5)]=EMPTY; }
        else               { b->board[sq(7,7)]=BR; b->board[sq(7,5)]=EMPTY; }
    }
    if(m->flag==FLAG_CASTLE_QS) {
        if(b->side==WHITE) { b->board[sq(0,0)]=WR; b->board[sq(0,3)]=EMPTY; }
        else               { b->board[sq(7,0)]=BR; b->board[sq(7,3)]=EMPTY; }
    }
    if(m->flag>=FLAG_PROMO_Q) {
        b->board[m->from] = m->piece; /* pawn restored */
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
   PGN helpers
───────────────────────────────────────────────────────────────────────────── */

static const char *PIECE_PGN[13] = {
    "","","N","B","R","Q","K","","N","B","R","Q","K"
};

/* Check if any other same piece can reach the same square (disambiguation) */
static void pgn_disambig(Board *b, Move *m, char *out) {
    Move all[MAX_MOVES]; int cnt=gen_moves(b,all); int n=0;
    int same_file=0, same_rank=0;
    for(int i=0;i<cnt;i++) {
        Move *a=&all[i];
        if(a->piece==m->piece && a->to==m->to && a->from!=m->from) {
            /* Check legality */
            Board tmp=*b;
            Move am=*a;
            if(apply_move(&tmp,&am)) {
                n++;
                if(sq_file(a->from)==sq_file(m->from)) same_file++;
                if(sq_rank(a->from)==sq_rank(m->from)) same_rank++;
            }
        }
    }
    out[0]='\0';
    if(n>0) {
        if(!same_file) { out[0]='a'+sq_file(m->from); out[1]='\0'; }
        else if(!same_rank) { out[0]='1'+sq_rank(m->from); out[1]='\0'; }
        else { out[0]='a'+sq_file(m->from); out[1]='1'+sq_rank(m->from); out[2]='\0'; }
    }
}

/* Build SAN for a move (before applying it) */
static void move_to_san(Board *b, Move *m, char *san) {
    char ts[3]; sq_name(m->to, ts);
    san[0]='\0';

    if(m->flag==FLAG_CASTLE_KS) { strcpy(san,"O-O"); return; }
    if(m->flag==FLAG_CASTLE_QS) { strcpy(san,"O-O-O"); return; }

    int piece=m->piece;
    int is_pawn=(piece==WP||piece==BP);

    if(!is_pawn) {
        char dis[3]; pgn_disambig(b,m,dis);
        sprintf(san,"%s%s",PIECE_PGN[piece],dis);
    } else if(m->captured!=EMPTY||m->flag==FLAG_EP) {
        san[0]='a'+sq_file(m->from); san[1]='\0';
    }

    if(m->captured!=EMPTY||m->flag==FLAG_EP) strcat(san,"x");
    strcat(san,ts);

    /* Promotion */
    if(m->flag>=FLAG_PROMO_Q) {
        const char *promo[4]={"Q","R","B","N"};
        strcat(san,"="); strcat(san,promo[m->flag-FLAG_PROMO_Q]);
    }

    /* Check / checkmate */
    Board tmp=*b;
    Move tm=*m;
    if(apply_move(&tmp,&tm)) {
        if(in_check(&tmp,tmp.side)) {
            /* Checkmate? */
            Move legal[MAX_MOVES]; int lc=0, mc=gen_moves(&tmp,legal);
            for(int i=0;i<mc;i++) {
                Board t2=tmp; Move mv2=legal[i];
                if(apply_move(&t2,&mv2)) { lc++; break; }
            }
            strcat(san, lc==0 ? "#" : "+");
        }
    }
}

static void pgn_append(Board *b, const char *san) {
    char buf[64];
    if(b->side==WHITE) {
        snprintf(buf,sizeof(buf),"%d. %s ", b->fullmove, san);
    } else {
        snprintf(buf,sizeof(buf),"%s ", san);
    }
    int len=strlen(buf);
    if(b->pgn_len+len < MAX_PGN-1) {
        strcat(b->pgn, buf);
        b->pgn_len += len;
    }
}

/* Rebuild PGN from scratch (used after undo) */
static void pgn_rebuild(Board *b) {
    /* We keep history; rebuild from initial position */
    Board tmp; board_init(&tmp);
    b->pgn[0]='\0'; b->pgn_len=0;
    /* Temporarily use b->pgn but apply from history */
    /* Copy history */
    Move hist[MAX_HISTORY];
    int hcnt=b->hist_cnt;
    memcpy(hist,b->history,hcnt*sizeof(Move));

    Board scratch; board_init(&scratch);
    for(int i=0;i<hcnt;i++) {
        char san[32];
        move_to_san(&scratch, &hist[i], san);
        pgn_append(&scratch, san);
        apply_move(&scratch, &hist[i]);
    }
    strncpy(b->pgn, scratch.pgn, MAX_PGN-1);
    b->pgn_len = strlen(b->pgn);
}

/* ─────────────────────────────────────────────────────────────────────────────
   Legal move list for highlighting
───────────────────────────────────────────────────────────────────────────── */

static int legal_targets[64];
static int legal_target_cnt=0;

static void compute_legal_targets(Board *b, int from) {
    legal_target_cnt=0;
    Move moves[MAX_MOVES];
    int cnt=gen_moves(b,moves);
    for(int i=0;i<cnt;i++) {
        if(moves[i].from!=from) continue;
        Board tmp=*b; Move m=moves[i];
        if(apply_move(&tmp,&m))
            legal_targets[legal_target_cnt++]=moves[i].to;
    }
}

static int is_legal_target(int sq2) {
    for(int i=0;i<legal_target_cnt;i++)
        if(legal_targets[i]==sq2) return 1;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Game state
───────────────────────────────────────────────────────────────────────────── */

typedef enum {
    GAME_ACTIVE, GAME_CHECKMATE, GAME_STALEMATE, GAME_DRAW_50
} GameState;

static GameState check_game_state(Board *b) {
    if(b->halfmove>=100) return GAME_DRAW_50;
    Move moves[MAX_MOVES]; int cnt=gen_moves(b,moves);
    int legal=0;
    for(int i=0;i<cnt;i++) {
        Board tmp=*b; Move m=moves[i];
        if(apply_move(&tmp,&m)) { legal++; break; }
    }
    if(legal==0) return in_check(b,b->side) ? GAME_CHECKMATE : GAME_STALEMATE;
    return GAME_ACTIVE;
}

/* ─────────────────────────────────────────────────────────────────────────────
   UCI Engine communication
───────────────────────────────────────────────────────────────────────────── */

typedef struct {
    int  active;
    int  to_engine[2];   /* pipe: GUI writes, engine reads */
    int  from_engine[2]; /* pipe: engine writes, GUI reads */
    pid_t pid;
    char name[128];
    int  thinking;
} Engine;

static Engine engine;

static int engine_start(const char *path) {
    if(pipe(engine.to_engine)<0||pipe(engine.from_engine)<0) return 0;
    engine.pid=fork();
    if(engine.pid<0) return 0;
    if(engine.pid==0) {
        /* child */
        dup2(engine.to_engine[0],  STDIN_FILENO);
        dup2(engine.from_engine[1], STDOUT_FILENO);
        close(engine.to_engine[1]);
        close(engine.from_engine[0]);
        execlp(path, path, NULL);
        exit(1);
    }
    close(engine.to_engine[0]);
    close(engine.from_engine[1]);
    /* Make read non-blocking */
    int flags=fcntl(engine.from_engine[0],F_GETFL,0);
    fcntl(engine.from_engine[0],F_SETFL,flags|O_NONBLOCK);
    engine.active=1;
    engine.thinking=0;
    strcpy(engine.name,"Engine");
    /* Send UCI handshake */
    dprintf(engine.to_engine[1],"uci\n");
    /* Read uciok */
    char buf[1024]; int attempts=50;
    while(attempts-->0) {
        usleep(50000);
        ssize_t n=read(engine.from_engine[0],buf,sizeof(buf)-1);
        if(n>0) { buf[n]='\0';
            /* Extract name */
            char *nm=strstr(buf,"id name ");
            if(nm) { nm+=8; char *end=strchr(nm,'\n'); if(end)*end='\0'; strncpy(engine.name,nm,127); }
            if(strstr(buf,"uciok")) break;
        }
    }
    dprintf(engine.to_engine[1],"isready\n");
    attempts=50;
    while(attempts-->0) {
        usleep(50000);
        ssize_t n=read(engine.from_engine[0],buf,sizeof(buf)-1);
        if(n>0) { buf[n]='\0'; if(strstr(buf,"readyok")) break; }
    }
    return 1;
}

static void engine_stop(void) {
    if(!engine.active) return;
    dprintf(engine.to_engine[1],"stop\nquit\n");
    usleep(200000);
    close(engine.to_engine[1]);
    close(engine.from_engine[0]);
    engine.active=0;
}

/* Build position string from history */
static void engine_send_position(Board *b) {
    char cmd[4096]="position startpos";
    if(b->hist_cnt>0) {
        strcat(cmd," moves");
        for(int i=0;i<b->hist_cnt;i++) {
            Move *m=&b->history[i];
            char fs[3],ts2[3]; sq_name(m->from,fs); sq_name(m->to,ts2);
            char mv[8]; snprintf(mv,sizeof(mv)," %s%s",fs,ts2);
            if(m->flag>=FLAG_PROMO_Q) {
                const char *pp[4]={"q","r","b","n"};
                strcat(mv,pp[m->flag-FLAG_PROMO_Q]);
            }
            strcat(cmd,mv);
        }
    }
    strcat(cmd,"\n");
    dprintf(engine.to_engine[1],"%s",cmd);
}

static void engine_go(int movetime_ms) {
    char cmd[64];
    snprintf(cmd,sizeof(cmd),"go movetime %d\n",movetime_ms);
    dprintf(engine.to_engine[1],"%s",cmd);
    engine.thinking=1;
}

/* Parse "bestmove e2e4" -> returns from/to squares and flag */
static int engine_parse_bestmove(const char *line, int *from, int *to, int *flag) {
    const char *bm=strstr(line,"bestmove ");
    if(!bm) return 0;
    bm+=9;
    if(strlen(bm)<4) return 0;
    *from = sq(bm[1]-'1', bm[0]-'a');
    *to   = sq(bm[3]-'1', bm[2]-'a');
    *flag = FLAG_NONE;
    if(bm[4]&&bm[4]!='\n'&&bm[4]!=' ') {
        switch(tolower(bm[4])) {
            case 'q': *flag=FLAG_PROMO_Q; break;
            case 'r': *flag=FLAG_PROMO_R; break;
            case 'b': *flag=FLAG_PROMO_B; break;
            case 'n': *flag=FLAG_PROMO_N; break;
        }
    }
    return 1;
}

/* Poll engine for bestmove (non-blocking) */
static int engine_poll(int *from, int *to, int *flag) {
    if(!engine.active||!engine.thinking) return 0;
    char buf[2048];
    ssize_t n=read(engine.from_engine[0],buf,sizeof(buf)-1);
    if(n<=0) return 0;
    buf[n]='\0';
    if(strstr(buf,"bestmove")) {
        engine.thinking=0;
        return engine_parse_bestmove(buf,from,to,flag);
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Terminal raw mode
───────────────────────────────────────────────────────────────────────────── */

static struct termios orig_term;
static void restore_term(void) {
    tcsetattr(STDIN_FILENO,TCSANOW,&orig_term);
    printf(SHOW_CURSOR);
    fflush(stdout);
}

static void setup_term(void) {
    tcgetattr(STDIN_FILENO,&orig_term);
    atexit(restore_term);
    struct termios raw=orig_term;
    cfmakeraw(&raw);
    raw.c_lflag &= ~(ECHO|ICANON);
    raw.c_cc[VMIN]=0;
    raw.c_cc[VTIME]=1;
    tcsetattr(STDIN_FILENO,TCSANOW,&raw);
    printf(HIDE_CURSOR);
}

/* Read a key; returns 0 if no key. Special keys as negative codes. */
#define KEY_UP    -1
#define KEY_DOWN  -2
#define KEY_LEFT  -3
#define KEY_RIGHT -4
#define KEY_ENTER -5

static int read_key(void) {
    unsigned char c;
    if(read(STDIN_FILENO,&c,1)!=1) return 0;
    if(c=='\033') {
        unsigned char seq[3]={0,0,0};
        read(STDIN_FILENO,&seq[0],1);
        read(STDIN_FILENO,&seq[1],1);
        if(seq[0]=='[') {
            switch(seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return 0;
    }
    if(c=='\r'||c=='\n') return KEY_ENTER;
    return (int)c;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Rendering
───────────────────────────────────────────────────────────────────────────── */

/* Layout constants (terminal rows/cols) */
#define BOARD_TOP    3     /* row where board starts */
#define BOARD_LEFT   4     /* col where board starts */
#define CELL_W       4     /* characters per cell */
#define CELL_H       2     /* rows per cell */
#define INFO_COL     40    /* column for info panel */

static int flipped=0;     /* board orientation */

/* Map board square to display row,col */
static void sq_to_display(int s, int *row, int *col) {
    int r=sq_rank(s), c=sq_file(s);
    int dr = flipped ? r        : 7-r;
    int dc = flipped ? 7-c      : c;
    *row = BOARD_TOP + dr*CELL_H;
    *col = BOARD_LEFT + dc*CELL_W;
}

static void render_board(Board *b, int cursor, int selected, int last_from, int last_to) {
    /* Determine check square */
    int check_sq=-1;
    if(in_check(b,b->side)) check_sq=king_sq(b,b->side);

    for(int s=0;s<64;s++) {
        int row,col; sq_to_display(s,&row,&col);
        int r=sq_rank(s), c=sq_file(s);
        int light = (r+c)%2==0;

        const char *bg;
        if(s==cursor)              bg=BG_CURSOR;
        else if(s==selected)       bg=BG_SELECT;
        else if(selected>=0 && is_legal_target(s)) bg=BG_MOVE;
        else if(s==check_sq)       bg=BG_CHECK;
        else if(s==last_from||s==last_to) bg=BG_LAST;
        else                       bg=light ? BG_LIGHT : BG_DARK;

        /* Draw cell: 2 rows high, 4 chars wide */
        int piece=b->board[s];
        const char *fg = (piece!=EMPTY && is_white(piece)) ? FG_WHITE : FG_BLACK;

        /* Top half of cell */
        move_to(row, col);
        printf("%s%s    " RESET, bg, fg);

        /* Bottom half with piece */
        move_to(row+1, col);
        if(piece!=EMPTY)
            printf("%s%s %s " RESET, bg, fg, PIECE_UTF8[piece]);
        else
            printf("%s    " RESET, bg);
    }

    /* Rank labels */
    for(int r=0;r<8;r++) {
        int dr = flipped ? r : 7-r;
        int row = BOARD_TOP + dr*CELL_H + 1;
        move_to(row, BOARD_LEFT-2);
        printf(BOLD "%d" RESET, r+1);
    }
    /* File labels */
    for(int c=0;c<8;c++) {
        int dc = flipped ? 7-c : c;
        int col = BOARD_LEFT + dc*CELL_W + 1;
        move_to(BOARD_TOP + 8*CELL_H, col);
        printf(BOLD "%c" RESET, 'a'+c);
    }
}

static void render_info(Board *b, GameState gs, int cursor, const char *status) {
    int row=BOARD_TOP;
    int col=INFO_COL;

    /* Title */
    move_to(1,1);
    printf(BOLD "♔ Terminal Chess" RESET);

    if(engine.active) {
        move_to(1,20);
        printf(DIM "Engine: %s" RESET, engine.name);
    }

    /* Side to move */
    move_to(row,col);
    printf(BOLD "%-20s" RESET, b->side==WHITE ? "White to move" : "Black to move");
    row++;

    /* Game state */
    move_to(row,col);
    switch(gs) {
        case GAME_CHECKMATE: printf(BOLD "\033[31mCheckmate!\033[0m            "); break;
        case GAME_STALEMATE: printf(BOLD "\033[33mStalemate!\033[0m            "); break;
        case GAME_DRAW_50:   printf(BOLD "\033[33m50-move draw!\033[0m         "); break;
        default:
            if(in_check(b,b->side)) printf(BOLD "\033[31mCheck!\033[0m                ");
            else                     printf("%-22s","");
    }
    row+=2;

    /* Cursor position */
    char csq[4]="--"; if(cursor>=0) sq_name(cursor,csq);
    move_to(row,col);
    printf("Cursor: %-10s", csq);
    row++;

    /* Move count */
    move_to(row,col);
    printf("Move:   %-10d", b->fullmove);
    row++;

    /* Half move clock */
    move_to(row,col);
    printf("Half:   %-10d", b->halfmove);
    row+=2;

    /* Controls */
    move_to(row,col); printf(BOLD "Controls:" RESET "          "); row++;
    move_to(row,col); printf("Arrows  - Move cursor   "); row++;
    move_to(row,col); printf("Enter   - Select/Move   "); row++;
    move_to(row,col); printf("U       - Undo          "); row++;
    move_to(row,col); printf("N       - New game      "); row++;
    move_to(row,col); printf("F       - Flip board    "); row++;
    move_to(row,col); printf("Q       - Quit          "); row++;
    row++;

    /* Status line */
    move_to(row,col);
    printf("\033[36m%-30s\033[0m", status ? status : "");
    row+=2;

    /* PGN moves */
    move_to(row,col);
    printf(BOLD "Moves:" RESET "                        ");
    row++;

    /* Display PGN wrapped at 30 chars per line */
    char pgn_copy[MAX_PGN];
    strncpy(pgn_copy, b->pgn, MAX_PGN-1);
    pgn_copy[MAX_PGN-1]='\0';

    char *tok=strtok(pgn_copy," ");
    int line_len=0;
    int pgn_row=row;
    char line_buf[64]="";
    while(tok && pgn_row < BOARD_TOP+CELL_H*8+4) {
        if(line_len+strlen(tok)+1>28) {
            move_to(pgn_row,col);
            printf("%-30s",line_buf);
            pgn_row++; line_len=0; line_buf[0]='\0';
        }
        if(line_len>0) strcat(line_buf," ");
        strcat(line_buf,tok);
        line_len+=strlen(tok)+1;
        tok=strtok(NULL," ");
    }
    if(line_len>0) { move_to(pgn_row,col); printf("%-30s",line_buf); pgn_row++; }
    /* Clear remaining lines */
    for(;pgn_row<BOARD_TOP+CELL_H*8+6;pgn_row++) {
        move_to(pgn_row,col); printf("%-30s","");
    }
}

static void render_all(Board *b, GameState gs, int cursor, int selected,
                        int last_from, int last_to, const char *status) {
    render_board(b, cursor, selected, last_from, last_to);
    render_info(b, gs, cursor, status);
    move_to(BOARD_TOP+CELL_H*8+5, 1);
    fflush(stdout);
}

/* ─────────────────────────────────────────────────────────────────────────────
   Promotion selection UI
───────────────────────────────────────────────────────────────────────────── */

/* Returns FLAG_PROMO_Q/R/B/N */
static int ask_promotion(int side) {
    int row=BOARD_TOP+4, col=INFO_COL;
    move_to(row,col);   printf(BOLD "Promote to:" RESET "           ");
    move_to(row+1,col); printf("[Q]ueen  [R]ook  ");
    move_to(row+2,col); printf("[B]ishop [N]knight");
    fflush(stdout);
    while(1) {
        int k=read_key();
        if(k==0) continue;
        switch(tolower(k)) {
            case 'q': return FLAG_PROMO_Q;
            case 'r': return FLAG_PROMO_R;
            case 'b': return FLAG_PROMO_B;
            case 'n': return FLAG_PROMO_N;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
   Find a legal move matching from->to (returns 1 if found)
───────────────────────────────────────────────────────────────────────────── */

static int find_move(Board *b, int from, int to, int promo_flag, Move *out) {
    Move moves[MAX_MOVES];
    int cnt=gen_moves(b,moves);
    for(int i=0;i<cnt;i++) {
        if(moves[i].from!=from||moves[i].to!=to) continue;
        /* Check legality */
        Board tmp=*b; Move m=moves[i];
        if(!apply_move(&tmp,&m)) continue;
        /* Handle promotion */
        if(m.flag>=FLAG_PROMO_Q) {
            if(promo_flag==FLAG_NONE) continue; /* need flag */
            if(m.flag!=promo_flag) continue;
        }
        *out=moves[i];
        return 1;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Main
───────────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* Check terminal size */
    printf("\033[8;24;80t"); /* request 80x24 minimum */

    setup_term();
    clear_screen();

    Board board;
    board_init(&board);

    memset(&engine, 0, sizeof(engine));
    int engine_side = -1; /* -1 = no engine, WHITE=0 BLACK=1 */

    if(argc>1) {
        printf("Starting engine: %s ...\n", argv[1]);
        fflush(stdout);
        if(engine_start(argv[1])) {
            engine_side = BLACK; /* Engine plays black by default */
            /* Ask which side */
            printf("Engine loaded: %s\n", engine.name);
            printf("Engine plays [W]hite or [B]lack? ");
            fflush(stdout);
            unsigned char ch;
            while(read(STDIN_FILENO,&ch,1)!=1);
            if(tolower(ch)=='w') engine_side=WHITE;
            else engine_side=BLACK;
        } else {
            printf("Failed to start engine. Continuing without engine.\n");
            fflush(stdout);
            sleep(1);
        }
    }

    clear_screen();

    /* Cursor starts at e2 (white pawn) */
    int cursor_sq = flipped ? sq(6,4) : sq(1,4);
    int selected  = -1;
    int last_from = -1, last_to = -1;
    char status[128] = "Welcome! Use arrows + Enter to play.";
    GameState gs = GAME_ACTIVE;

    /* If engine plays white, kick it off immediately */
    if(engine.active && engine_side==WHITE && board.side==WHITE) {
        engine_send_position(&board);
        engine_go(2000);
    }

    render_all(&board, gs, cursor_sq, selected, last_from, last_to, status);

    while(1) {
        /* Poll engine */
        if(engine.active && engine.thinking && board.side==engine_side) {
            int ef, et, efl;
            if(engine_poll(&ef,&et,&efl)) {
                /* Find and apply move */
                Move em;
                /* For promotions, default to queen */
                int pfl=efl==FLAG_NONE ? FLAG_NONE : efl;
                if(find_move(&board,ef,et,pfl,&em)) {
                    char san[32];
                    move_to_san(&board,&em,san);
                    pgn_append(&board,san);
                    board.history[board.hist_cnt++]=em;
                    apply_move(&board,&em);
                    last_from=ef; last_to=et;
                    gs=check_game_state(&board);
                    snprintf(status,sizeof(status),"Engine: %s",san);
                    /* cursor follows engine move target */
                    cursor_sq=et;
                }
                render_all(&board,gs,cursor_sq,selected,last_from,last_to,status);
            }
        }

        int key=read_key();
        if(key==0) {
            /* Small sleep to avoid busy-loop */
            usleep(20000);
            continue;
        }

        if(key=='q'||key=='Q') break;

        if(key=='n'||key=='N') {
            /* New game */
            board_init(&board);
            selected=-1; last_from=-1; last_to=-1; gs=GAME_ACTIVE;
            cursor_sq=flipped?sq(6,4):sq(1,4);
            strcpy(status,"New game started.");
            if(engine.active && engine_side==WHITE && board.side==WHITE) {
                engine_send_position(&board);
                engine_go(2000);
            }
            clear_screen();
            render_all(&board,gs,cursor_sq,selected,last_from,last_to,status);
            continue;
        }

        if(key=='f'||key=='F') {
            flipped=!flipped;
            clear_screen();
            render_all(&board,gs,cursor_sq,selected,last_from,last_to,status);
            continue;
        }

        if(key=='u'||key=='U') {
            /* Undo */
            int undo_cnt = (engine.active && engine_side>=0) ? 2 : 1;
            int done=0;
            for(int u=0;u<undo_cnt&&board.hist_cnt>0;u++) {
                undo_move(&board);
                done++;
            }
            if(done>0) {
                selected=-1;
                if(board.hist_cnt>0) {
                    Move *lm=&board.history[board.hist_cnt-1];
                    last_from=lm->from; last_to=lm->to;
                } else { last_from=-1; last_to=-1; }
                gs=check_game_state(&board);
                pgn_rebuild(&board);
                strcpy(status,"Move undone.");
                if(engine.active) engine.thinking=0;
                clear_screen();
            } else {
                strcpy(status,"Nothing to undo.");
            }
            render_all(&board,gs,cursor_sq,selected,last_from,last_to,status);
            continue;
        }

        if(key=='h'||key=='H') {
            strcpy(status,"Arrows=Move  Enter=Select  U=Undo  F=Flip  N=New  Q=Quit");
            render_all(&board,gs,cursor_sq,selected,last_from,last_to,status);
            continue;
        }

        /* Arrow keys: move cursor */
        {
            int r=sq_rank(cursor_sq), c=sq_file(cursor_sq);
            int moved=1;
            switch(key) {
                case KEY_UP:    r+=flipped?-1:1; break;
                case KEY_DOWN:  r+=flipped?1:-1; break;
                case KEY_LEFT:  c+=flipped?1:-1; break;
                case KEY_RIGHT: c+=flipped?-1:1; break;
                default: moved=0;
            }
            if(moved) {
                r=r<0?0:r>7?7:r;
                c=c<0?0:c>7?7:c;
                cursor_sq=sq(r,c);
                render_all(&board,gs,cursor_sq,selected,last_from,last_to,status);
                continue;
            }
        }

        /* Enter / Space: select or move */
        if((key==KEY_ENTER||key==' ') && gs==GAME_ACTIVE) {
            /* If it's the engine's turn, ignore */
            if(engine.active && board.side==engine_side) {
                strcpy(status,"Engine is thinking...");
                render_all(&board,gs,cursor_sq,selected,last_from,last_to,status);
                continue;
            }

            if(selected<0) {
                /* Select a piece */
                int piece=board.board[cursor_sq];
                if(piece!=EMPTY && piece_color(piece)==board.side) {
                    selected=cursor_sq;
                    compute_legal_targets(&board,selected);
                    if(legal_target_cnt==0) {
                        selected=-1;
                        strcpy(status,"No legal moves for that piece.");
                    } else {
                        char sn[4]; sq_name(cursor_sq,sn);
                        snprintf(status,sizeof(status),"Selected %s. Choose destination.",sn);
                    }
                } else {
                    strcpy(status,"No piece to select here.");
                }
            } else {
                /* Try to move */
                if(cursor_sq==selected) {
                    /* Deselect */
                    selected=-1; legal_target_cnt=0;
                    strcpy(status,"Deselected.");
                } else if(!is_legal_target(cursor_sq)) {
                    /* Re-select if own piece */
                    int piece=board.board[cursor_sq];
                    if(piece!=EMPTY && piece_color(piece)==board.side) {
                        selected=cursor_sq;
                        compute_legal_targets(&board,selected);
                        char sn[4]; sq_name(cursor_sq,sn);
                        snprintf(status,sizeof(status),"Selected %s.",sn);
                    } else {
                        selected=-1; legal_target_cnt=0;
                        strcpy(status,"Illegal move. Selection cleared.");
                    }
                } else {
                    /* Make the move */
                    int from=selected, to=cursor_sq;
                    int promo=FLAG_NONE;

                    /* Check if promotion needed */
                    int piece=board.board[from];
                    if((piece==WP&&sq_rank(to)==7)||(piece==BP&&sq_rank(to)==0)) {
                        /* Need promotion UI */
                        render_all(&board,gs,cursor_sq,selected,last_from,last_to,"Promote!");
                        promo=ask_promotion(board.side);
                    }

                    Move m;
                    if(find_move(&board,from,to,promo,&m)) {
                        char san[32];
                        move_to_san(&board,&m,san);
                        pgn_append(&board,san);
                        board.history[board.hist_cnt++]=m;
                        apply_move(&board,&m);
                        last_from=from; last_to=to;
                        selected=-1; legal_target_cnt=0;
                        gs=check_game_state(&board);
                        snprintf(status,sizeof(status),"Played: %s",san);

                        /* Engine reply */
                        if(engine.active && gs==GAME_ACTIVE && board.side==engine_side) {
                            engine_send_position(&board);
                            engine_go(2000);
                        }
                    } else {
                        selected=-1; legal_target_cnt=0;
                        strcpy(status,"Illegal move.");
                    }
                }
            }
            clear_screen();
            render_all(&board,gs,cursor_sq,selected,last_from,last_to,status);
            continue;
        }

        /* Announce game over */
        if(gs!=GAME_ACTIVE && (key==KEY_ENTER||key==' ')) {
            strcpy(status,"Game over. Press N for new game.");
            render_all(&board,gs,cursor_sq,selected,last_from,last_to,status);
        }
    }

    /* Cleanup */
    engine_stop();
    clear_screen();
    printf(SHOW_CURSOR);
    printf("Thanks for playing!\n");
    return 0;
}
