#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>

/* ─────────────────────────── ANSI / TERMINAL ─────────────────────────── */
#define CLR_RESET       "\033[0m"
#define CLR_BOLD        "\033[1m"
#define CLR_DIM         "\033[2m"

/* 256-colour backgrounds */
#define BG_LIGHT_SQ     "\033[48;5;223m"   /* warm cream               */
#define BG_DARK_SQ      "\033[48;5;137m"   /* warm brown               */
#define BG_CURSOR       "\033[48;5;226m"   /* bright yellow            */
#define BG_SELECTED     "\033[48;5;214m"   /* orange                   */
#define BG_LEGAL        "\033[48;5;149m"   /* light green              */
#define BG_LAST_MOVE    "\033[48;5;74m"    /* steel blue               */
#define BG_CHECK        "\033[48;5;196m"   /* red                      */

/* foreground */
#define FG_WHITE_PIECE  "\033[38;5;255m"
#define FG_BLACK_PIECE  "\033[38;5;232m"

#define HIDE_CURSOR     "\033[?25l"
#define SHOW_CURSOR     "\033[?25h"
#define CLEAR_SCREEN    "\033[2J\033[H"

/* ─────────────────────────── CHESS CONSTANTS ─────────────────────────── */
#define EMPTY  0
#define WPAWN  1
#define WKNIGHT 2
#define WBISHOP 3
#define WROOK  4
#define WQUEEN 5
#define WKING  6
#define BPAWN  7
#define BKNIGHT 8
#define BBISHOP 9
#define BROOK  10
#define BQUEEN 11
#define BKING  12

#define WHITE 0
#define BLACK 1

#define MAX_MOVES 256
#define MAX_HISTORY 512
#define MAX_PGN_MOVES 1024

/* ─────────────────────────── DATA STRUCTURES ─────────────────────────── */

typedef struct {
    int from_sq;   /* 0-63 */
    int to_sq;
    int piece;
    int captured;
    int promotion; /* piece type if promotion */
    int flags;     /* castling, en passant */
} Move;

#define FLAG_CASTLE_K   0x01
#define FLAG_CASTLE_Q   0x02
#define FLAG_EP         0x04
#define FLAG_PROMO      0x08

typedef struct {
    int board[64];
    int side;          /* WHITE or BLACK */
    int ep_sq;         /* en-passant target square, -1 if none */
    int castle;        /* bits: wK wQ bK bQ => bits 0-3 */
    int halfmove;      /* for 50-move rule */
    int fullmove;
} Position;

#define CAST_WK 0x1
#define CAST_WQ 0x2
#define CAST_BK 0x4
#define CAST_BQ 0x8

typedef struct {
    Position pos;
    Move move;          /* move that led to this position */
    char pgn_move[16];  /* SAN of the move */
} HistoryEntry;

typedef enum {
    TC_DEPTH, TC_NODES, TC_TIME
} TCMode;

typedef struct {
    TCMode mode;
    int depth;          /* plies */
    long long nodes;
    int time_ms;        /* milliseconds */
} TimeControl;

typedef struct {
    /* engine process */
    pid_t pid;
    int to_engine[2];   /* pipe: GUI writes, engine reads  */
    int from_engine[2]; /* pipe: engine writes, GUI reads  */
    char name[128];
    int ready;
    char bestmove[16];  /* last bestmove received */
} Engine;

/* ─────────────────────────── GLOBALS ─────────────────────────── */
static Position g_pos;
static HistoryEntry g_history[MAX_HISTORY];
static int g_hist_count = 0;

static int g_cursor_sq  = 36; /* e4 */
static int g_selected_sq = -1;
static int g_last_from   = -1;
static int g_last_to     = -1;

static Move g_legal_moves[MAX_MOVES];
static int  g_legal_count = 0;

static Engine g_engine;
static int    g_engine_active = 0;

static TimeControl g_tc = { TC_DEPTH, 6, 0, 1000 };

static struct termios g_orig_termios;
static int g_running = 1;

/* for promotion UI */
static int g_awaiting_promo = 0;
static int g_promo_from = -1;
static int g_promo_to   = -1;

/* ─────────────────────────────────────────────────────────────────
   PIECE DISPLAY
   We use Unicode chess symbols drawn with two-char wide cells.
   ───────────────────────────────────────────────────────────────── */
static const char *piece_glyph[13] = {
    "  ",   /* EMPTY  */
    "♙ ",   /* WPAWN  */
    "♘ ",   /* WKNIGHT*/
    "♗ ",   /* WBISHOP*/
    "♖ ",   /* WROOK  */
    "♕ ",   /* WQUEEN */
    "♔ ",   /* WKING  */
    "♟ ",   /* BPAWN  */
    "♞ ",   /* BKNIGHT*/
    "♝ ",   /* BBISHOP*/
    "♜ ",   /* BROOK  */
    "♛ ",   /* BQUEEN */
    "♚ ",   /* BKING  */
};

/* ═══════════════════════════════════════════════════════════════
   TERMINAL HELPERS
   ═══════════════════════════════════════════════════════════════ */
static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    t = g_orig_termios;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    printf(SHOW_CURSOR);
    fflush(stdout);
}

static void cleanup_and_exit(int code) {
    term_restore();
    if (g_engine_active) {
        write(g_engine.to_engine[1], "quit\n", 5);
        usleep(100000);
        kill(g_engine.pid, SIGTERM);
    }
    printf(CLEAR_SCREEN);
    exit(code);
}

/* ═══════════════════════════════════════════════════════════════
   POSITION INIT
   ═══════════════════════════════════════════════════════════════ */
static void pos_start(Position *p) {
    memset(p, 0, sizeof(*p));
    /* rank 8 (index 56-63) black */
    int back_black[] = {BROOK,BKNIGHT,BBISHOP,BQUEEN,BKING,BBISHOP,BKNIGHT,BROOK};
    int back_white[] = {WROOK,WKNIGHT,WBISHOP,WQUEEN,WKING,WBISHOP,WKNIGHT,WROOK};
    for (int f = 0; f < 8; f++) {
        p->board[56+f] = back_black[f];
        p->board[48+f] = BPAWN;
        p->board[8+f]  = WPAWN;
        p->board[f]    = back_white[f];
    }
    p->side     = WHITE;
    p->ep_sq    = -1;
    p->castle   = CAST_WK | CAST_WQ | CAST_BK | CAST_BQ;
    p->halfmove = 0;
    p->fullmove = 1;
}

static inline int sq_rank(int sq) { return sq >> 3; }
static inline int sq_file(int sq) { return sq & 7; }
static inline int sq(int r, int f) { return r*8+f; }

static inline int is_white(int pc) { return pc >= WPAWN && pc <= WKING; }
static inline int is_black(int pc) { return pc >= BPAWN && pc <= BKING; }
static inline int piece_color(int pc) { return is_black(pc) ? BLACK : WHITE; }

/* ═══════════════════════════════════════════════════════════════
   MOVE GENERATION  (pseudo-legal + legality filter)
   ═══════════════════════════════════════════════════════════════ */

/* Is square `sq` attacked by `attacker_side`? */
static int is_attacked(const Position *p, int target, int attacker_side);

static void add_move(Move *list, int *cnt, int from, int to, int piece,
                     int captured, int flags, int promo) {
    Move *m = &list[(*cnt)++];
    m->from_sq   = from;
    m->to_sq     = to;
    m->piece     = piece;
    m->captured  = captured;
    m->flags     = flags;
    m->promotion = promo;
}

/* Apply move to position (returns 1 on success) */
static void apply_move(Position *p, const Move *m) {
    int from = m->from_sq, to = m->to_sq;
    int piece = p->board[from];

    p->board[from] = EMPTY;

    /* en passant capture */
    if (m->flags & FLAG_EP) {
        int cap_sq = (p->side == WHITE) ? to - 8 : to + 8;
        p->board[cap_sq] = EMPTY;
    }

    /* promotion */
    if (m->flags & FLAG_PROMO) {
        p->board[to] = m->promotion;
    } else {
        p->board[to] = piece;
    }

    /* castling: move rook */
    if (m->flags & FLAG_CASTLE_K) {
        if (p->side == WHITE) { p->board[5]=WROOK; p->board[7]=EMPTY; }
        else                  { p->board[61]=BROOK; p->board[63]=EMPTY; }
    }
    if (m->flags & FLAG_CASTLE_Q) {
        if (p->side == WHITE) { p->board[3]=WROOK; p->board[0]=EMPTY; }
        else                  { p->board[59]=BROOK; p->board[56]=EMPTY; }
    }

    /* update castling rights */
    if (piece == WKING) p->castle &= ~(CAST_WK | CAST_WQ);
    if (piece == BKING) p->castle &= ~(CAST_BK | CAST_BQ);
    if (from == 0  || to == 0)  p->castle &= ~CAST_WQ;
    if (from == 7  || to == 7)  p->castle &= ~CAST_WK;
    if (from == 56 || to == 56) p->castle &= ~CAST_BQ;
    if (from == 63 || to == 63) p->castle &= ~CAST_BK;

    /* en passant square */
    if ((piece == WPAWN) && (to - from == 16)) p->ep_sq = from + 8;
    else if ((piece == BPAWN) && (from - to == 16)) p->ep_sq = to + 8;
    else p->ep_sq = -1;

    /* halfmove clock */
    if (piece == WPAWN || piece == BPAWN || m->captured != EMPTY)
        p->halfmove = 0;
    else
        p->halfmove++;

    if (p->side == BLACK) p->fullmove++;
    p->side ^= 1;
}

/* Generate pseudo-legal moves for one piece */
static void gen_piece(const Position *p, int from, Move *list, int *cnt) {
    int piece = p->board[from];
    if (piece == EMPTY) return;
    int color = piece_color(piece);
    if (color != p->side) return;

    int r = sq_rank(from), f = sq_file(from);

    /* helper to add quiet/capture if in-bounds */
#define TRY(R,F) do { \
    int _r=(R),_f=(F); \
    if(_r>=0&&_r<8&&_f>=0&&_f<8){ \
        int _s=sq(_r,_f); \
        int _pc=p->board[_s]; \
        if(_pc==EMPTY) add_move(list,cnt,from,_s,piece,EMPTY,0,0); \
        else if(piece_color(_pc)!=color) add_move(list,cnt,from,_s,piece,_pc,0,0); \
    } \
} while(0)

#define SLIDE(DR,DF) do { \
    for(int _i=1;_i<8;_i++){ \
        int _r=r+_i*(DR),_f=f+_i*(DF); \
        if(_r<0||_r>=8||_f<0||_f>=8) break; \
        int _s=sq(_r,_f),_pc=p->board[_s]; \
        if(_pc==EMPTY){ add_move(list,cnt,from,_s,piece,EMPTY,0,0); } \
        else { if(piece_color(_pc)!=color) add_move(list,cnt,from,_s,piece,_pc,0,0); break; } \
    } \
} while(0)

    switch (piece) {
    case WPAWN: {
        /* single push */
        if (r+1 < 8 && p->board[sq(r+1,f)] == EMPTY) {
            if (r+1 == 7) { /* promotion */
                for (int pr=WQUEEN;pr>=WKNIGHT;pr--)
                    add_move(list,cnt,from,sq(7,f),piece,EMPTY,FLAG_PROMO,pr);
            } else add_move(list,cnt,from,sq(r+1,f),piece,EMPTY,0,0);
            /* double push */
            if (r == 1 && p->board[sq(r+2,f)] == EMPTY)
                add_move(list,cnt,from,sq(r+2,f),piece,EMPTY,0,0);
        }
        /* captures */
        for (int df=-1;df<=1;df+=2) {
            if (f+df<0||f+df>=8) continue;
            int ts=sq(r+1,f+df);
            if (r+1>=8) continue;
            int tpc=p->board[ts];
            if (tpc!=EMPTY && is_black(tpc)) {
                if (r+1==7) for(int pr=WQUEEN;pr>=WKNIGHT;pr--)
                    add_move(list,cnt,from,ts,piece,tpc,FLAG_PROMO,pr);
                else add_move(list,cnt,from,ts,piece,tpc,0,0);
            }
            if (ts == p->ep_sq && p->ep_sq != -1)
                add_move(list,cnt,from,ts,piece,BPAWN,FLAG_EP,0);
        }
        break;
    }
    case BPAWN: {
        if (r-1 >= 0 && p->board[sq(r-1,f)] == EMPTY) {
            if (r-1 == 0) {
                for (int pr=BQUEEN;pr>=BKNIGHT;pr--)
                    add_move(list,cnt,from,sq(0,f),piece,EMPTY,FLAG_PROMO,pr);
            } else add_move(list,cnt,from,sq(r-1,f),piece,EMPTY,0,0);
            if (r == 6 && p->board[sq(r-2,f)] == EMPTY)
                add_move(list,cnt,from,sq(r-2,f),piece,EMPTY,0,0);
        }
        for (int df=-1;df<=1;df+=2) {
            if (f+df<0||f+df>=8) continue;
            if (r-1 < 0) continue;
            int ts=sq(r-1,f+df);
            int tpc=p->board[ts];
            if (tpc!=EMPTY && is_white(tpc)) {
                if (r-1==0) for(int pr=BQUEEN;pr>=BKNIGHT;pr--)
                    add_move(list,cnt,from,ts,piece,tpc,FLAG_PROMO,pr);
                else add_move(list,cnt,from,ts,piece,tpc,0,0);
            }
            if (ts == p->ep_sq && p->ep_sq != -1)
                add_move(list,cnt,from,ts,piece,WPAWN,FLAG_EP,0);
        }
        break;
    }
    case WKNIGHT: case BKNIGHT: {
        int deltas[8][2]={{2,1},{2,-1},{-2,1},{-2,-1},{1,2},{1,-2},{-1,2},{-1,-2}};
        for(int i=0;i<8;i++) TRY(r+deltas[i][0],f+deltas[i][1]);
        break;
    }
    case WBISHOP: case BBISHOP:
        SLIDE(1,1); SLIDE(1,-1); SLIDE(-1,1); SLIDE(-1,-1);
        break;
    case WROOK: case BROOK:
        SLIDE(1,0); SLIDE(-1,0); SLIDE(0,1); SLIDE(0,-1);
        break;
    case WQUEEN: case BQUEEN:
        SLIDE(1,1); SLIDE(1,-1); SLIDE(-1,1); SLIDE(-1,-1);
        SLIDE(1,0); SLIDE(-1,0); SLIDE(0,1); SLIDE(0,-1);
        break;
    case WKING: case BKING: {
        for(int dr=-1;dr<=1;dr++) for(int df2=-1;df2<=1;df2++)
            if(dr||df2) TRY(r+dr,f+df2);
        /* castling */
        if (piece == WKING && from == 4) {
            if ((p->castle & CAST_WK) &&
                p->board[5]==EMPTY && p->board[6]==EMPTY &&
                !is_attacked(p,4,BLACK) && !is_attacked(p,5,BLACK) && !is_attacked(p,6,BLACK))
                add_move(list,cnt,from,6,piece,EMPTY,FLAG_CASTLE_K,0);
            if ((p->castle & CAST_WQ) &&
                p->board[3]==EMPTY && p->board[2]==EMPTY && p->board[1]==EMPTY &&
                !is_attacked(p,4,BLACK) && !is_attacked(p,3,BLACK) && !is_attacked(p,2,BLACK))
                add_move(list,cnt,from,2,piece,EMPTY,FLAG_CASTLE_Q,0);
        }
        if (piece == BKING && from == 60) {
            if ((p->castle & CAST_BK) &&
                p->board[61]==EMPTY && p->board[62]==EMPTY &&
                !is_attacked(p,60,WHITE) && !is_attacked(p,61,WHITE) && !is_attacked(p,62,WHITE))
                add_move(list,cnt,from,62,piece,EMPTY,FLAG_CASTLE_K,0);
            if ((p->castle & CAST_BQ) &&
                p->board[59]==EMPTY && p->board[58]==EMPTY && p->board[57]==EMPTY &&
                !is_attacked(p,60,WHITE) && !is_attacked(p,59,WHITE) && !is_attacked(p,58,WHITE))
                add_move(list,cnt,from,58,piece,EMPTY,FLAG_CASTLE_Q,0);
        }
        break;
    }
    }
#undef TRY
#undef SLIDE
}

static int is_attacked(const Position *p, int target, int attacker_side) {
    int tr = sq_rank(target), tf = sq_file(target);

    /* knight */
    int kd[8][2]={{2,1},{2,-1},{-2,1},{-2,-1},{1,2},{1,-2},{-1,2},{-1,-2}};
    int knight_pc = (attacker_side==WHITE) ? WKNIGHT : BKNIGHT;
    for(int i=0;i<8;i++){
        int r2=tr+kd[i][0], f2=tf+kd[i][1];
        if(r2>=0&&r2<8&&f2>=0&&f2<8 && p->board[sq(r2,f2)]==knight_pc) return 1;
    }

    /* king */
    int king_pc = (attacker_side==WHITE) ? WKING : BKING;
    for(int dr=-1;dr<=1;dr++) for(int df=-1;df<=1;df++){
        if(!dr&&!df) continue;
        int r2=tr+dr,f2=tf+df;
        if(r2>=0&&r2<8&&f2>=0&&f2<8 && p->board[sq(r2,f2)]==king_pc) return 1;
    }

    /* pawn */
    if(attacker_side==WHITE){
        /* white pawn attacks upward, so it attacks target from below */
        for(int df=-1;df<=1;df+=2){
            int r2=tr-1,f2=tf+df;
            if(r2>=0&&r2<8&&f2>=0&&f2<8 && p->board[sq(r2,f2)]==WPAWN) return 1;
        }
    } else {
        for(int df=-1;df<=1;df+=2){
            int r2=tr+1,f2=tf+df;
            if(r2>=0&&r2<8&&f2>=0&&f2<8 && p->board[sq(r2,f2)]==BPAWN) return 1;
        }
    }

    /* sliders */
    int rook_pc  = (attacker_side==WHITE) ? WROOK  : BROOK;
    int bishop_pc= (attacker_side==WHITE) ? WBISHOP: BBISHOP;
    int queen_pc = (attacker_side==WHITE) ? WQUEEN : BQUEEN;

    /* rook / queen (orthogonal) */
    int dirs4[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
    for(int d=0;d<4;d++){
        for(int i=1;i<8;i++){
            int r2=tr+i*dirs4[d][0], f2=tf+i*dirs4[d][1];
            if(r2<0||r2>=8||f2<0||f2>=8) break;
            int pc=p->board[sq(r2,f2)];
            if(pc==EMPTY) continue;
            if(pc==rook_pc||pc==queen_pc) return 1;
            break;
        }
    }

    /* bishop / queen (diagonal) */
    int dirs8[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
    for(int d=0;d<4;d++){
        for(int i=1;i<8;i++){
            int r2=tr+i*dirs8[d][0], f2=tf+i*dirs8[d][1];
            if(r2<0||r2>=8||f2<0||f2>=8) break;
            int pc=p->board[sq(r2,f2)];
            if(pc==EMPTY) continue;
            if(pc==bishop_pc||pc==queen_pc) return 1;
            break;
        }
    }
    return 0;
}

/* Find king square */
static int find_king(const Position *p, int color) {
    int kpc = (color==WHITE) ? WKING : BKING;
    for(int i=0;i<64;i++) if(p->board[i]==kpc) return i;
    return -1;
}

/* Is the side-to-move in check? */
static int in_check(const Position *p) {
    int ksq = find_king(p, p->side);
    if(ksq<0) return 0;
    return is_attacked(p, ksq, p->side^1);
}

/* Generate fully legal moves */
static int gen_legal(const Position *p, Move *list) {
    Move pseudo[MAX_MOVES];
    int pcnt = 0;
    for(int sq2=0;sq2<64;sq2++) gen_piece(p, sq2, pseudo, &pcnt);

    int legal = 0;
    for(int i=0;i<pcnt;i++){
        Position tmp = *p;
        apply_move(&tmp, &pseudo[i]);
        /* after move, check if OUR king is in check */
        int ksq = find_king(&tmp, p->side);
        if(ksq < 0) continue;
        if(!is_attacked(&tmp, ksq, p->side^1))
            list[legal++] = pseudo[i];
    }
    return legal;
}

/* ═══════════════════════════════════════════════════════════════
   SAN (PGN) NOTATION
   ═══════════════════════════════════════════════════════════════ */
static const char *piece_letter(int pc) {
    switch(pc){
        case WKNIGHT: case BKNIGHT: return "N";
        case WBISHOP: case BBISHOP: return "B";
        case WROOK:   case BROOK:   return "R";
        case WQUEEN:  case BQUEEN:  return "Q";
        case WKING:   case BKING:   return "K";
        default: return "";
    }
}

static void move_to_san(const Position *p, const Move *m, char *buf) {
    buf[0] = '\0';
    int piece = m->piece;
    int from  = m->from_sq;
    int to    = m->to_sq;

    /* castling */
    if (m->flags & FLAG_CASTLE_K) { strcpy(buf,"O-O"); goto check_suffix; }
    if (m->flags & FLAG_CASTLE_Q) { strcpy(buf,"O-O-O"); goto check_suffix; }

    int is_pawn = (piece==WPAWN||piece==BPAWN);

    if (!is_pawn) {
        strcat(buf, piece_letter(piece));

        /* disambiguation: find other same pieces that can go to `to` */
        Move all[MAX_MOVES];
        int ac = gen_legal(p, all);
        int amb_file=0, amb_rank=0, amb=0;
        for(int i=0;i<ac;i++){
            if(all[i].piece==piece && all[i].to_sq==to && all[i].from_sq!=from){
                amb=1;
                if(sq_file(all[i].from_sq)==sq_file(from)) amb_rank=1;
                else amb_file=1;
            }
        }
        if(amb){
            if(amb_file && !amb_rank){
                char tmp[2]={(char)('a'+sq_file(from)),'\0'}; strcat(buf,tmp);
            } else if(amb_rank && !amb_file){
                char tmp[2]={(char)('1'+sq_rank(from)),'\0'}; strcat(buf,tmp);
            } else {
                char tmp[3]={(char)('a'+sq_file(from)),(char)('1'+sq_rank(from)),'\0'};
                strcat(buf,tmp);
            }
        }

        if(m->captured != EMPTY) strcat(buf,"x");
    } else {
        if(m->captured != EMPTY || (m->flags & FLAG_EP)){
            char tmp[2]={(char)('a'+sq_file(from)),'\0'};
            strcat(buf,tmp); strcat(buf,"x");
        }
    }

    /* destination */
    char dest[3]={(char)('a'+sq_file(to)),(char)('1'+sq_rank(to)),'\0'};
    strcat(buf,dest);

    /* promotion */
    if(m->flags & FLAG_PROMO){
        strcat(buf,"=");
        int pp = m->promotion;
        if(pp==WQUEEN||pp==BQUEEN) strcat(buf,"Q");
        else if(pp==WROOK||pp==BROOK) strcat(buf,"R");
        else if(pp==WBISHOP||pp==BBISHOP) strcat(buf,"B");
        else strcat(buf,"N");
    }

check_suffix:;
    /* check / checkmate */
    Position tmp2 = *p;
    apply_move(&tmp2, m);
    Move replies[MAX_MOVES];
    int rc = gen_legal(&tmp2, replies);
    if(in_check(&tmp2)){
        if(rc==0) strcat(buf,"#");
        else strcat(buf,"+");
    }
}

/* ═══════════════════════════════════════════════════════════════
   FEN
   ═══════════════════════════════════════════════════════════════ */
static void pos_to_fen(const Position *p, char *fen) {
    static const char pc_chars[] = ".PNBRQKpnbrqk";
    int idx = 0;
    for(int r=7;r>=0;r--){
        int empty=0;
        for(int f=0;f<8;f++){
            int pc=p->board[sq(r,f)];
            if(pc==EMPTY){ empty++; }
            else {
                if(empty){ fen[idx++]='0'+empty; empty=0; }
                fen[idx++]=pc_chars[pc];
            }
        }
        if(empty) fen[idx++]='0'+empty;
        if(r>0) fen[idx++]='/';
    }
    fen[idx++]=' ';
    fen[idx++]=(p->side==WHITE)?'w':'b';
    fen[idx++]=' ';
    if(!p->castle){ fen[idx++]='-'; }
    else {
        if(p->castle&CAST_WK) fen[idx++]='K';
        if(p->castle&CAST_WQ) fen[idx++]='Q';
        if(p->castle&CAST_BK) fen[idx++]='k';
        if(p->castle&CAST_BQ) fen[idx++]='q';
    }
    fen[idx++]=' ';
    if(p->ep_sq<0){ fen[idx++]='-'; }
    else {
        fen[idx++]='a'+sq_file(p->ep_sq);
        fen[idx++]='1'+sq_rank(p->ep_sq);
    }
    fen[idx]='\0';
    char tmp[32];
    snprintf(tmp,32," %d %d",p->halfmove,p->fullmove);
    strcat(fen,tmp);
}

/* ═══════════════════════════════════════════════════════════════
   UCI ENGINE
   ═══════════════════════════════════════════════════════════════ */
static int engine_launch(const char *path) {
    if(pipe(g_engine.to_engine)<0) return 0;
    if(pipe(g_engine.from_engine)<0) return 0;

    g_engine.pid = fork();
    if(g_engine.pid < 0) return 0;
    if(g_engine.pid == 0){
        /* child */
        dup2(g_engine.to_engine[0], STDIN_FILENO);
        dup2(g_engine.from_engine[1], STDOUT_FILENO);
        close(g_engine.to_engine[1]);
        close(g_engine.from_engine[0]);
        close(g_engine.to_engine[0]);
        close(g_engine.from_engine[1]);
        /* suppress stderr */
        int devnull=open("/dev/null",O_WRONLY);
        if(devnull>=0){ dup2(devnull,STDERR_FILENO); close(devnull); }
        execlp(path, path, NULL);
        exit(1);
    }
    close(g_engine.to_engine[0]);
    close(g_engine.from_engine[1]);
    /* make read end non-blocking */
    fcntl(g_engine.from_engine[0], F_SETFL, O_NONBLOCK);
    g_engine.ready = 0;
    strcpy(g_engine.name,"UCI Engine");

    /* send uci */
    write(g_engine.to_engine[1],"uci\n",4);
    return 1;
}

static void engine_send(const char *cmd) {
    if(!g_engine_active) return;
    write(g_engine.to_engine[1], cmd, strlen(cmd));
    write(g_engine.to_engine[1], "\n", 1);
}

/* Read lines from engine, return 1 if bestmove received */
static int engine_read(void) {
    static char buf[4096];
    static int  buf_len = 0;
    int got_best = 0;

    while(1){
        char c;
        ssize_t n = read(g_engine.from_engine[0], &c, 1);
        if(n<=0) break;
        if(c=='\n'){
            buf[buf_len]='\0';
            /* parse line */
            if(strncmp(buf,"uciok",5)==0){ /* ok */ }
            else if(strncmp(buf,"readyok",7)==0){ g_engine.ready=1; }
            else if(strncmp(buf,"id name ",8)==0){
                strncpy(g_engine.name, buf+8, 127);
            }
            else if(strncmp(buf,"bestmove",8)==0){
                /* bestmove e2e4 [ponder ...] */
                char *sp=strchr(buf,' ');
                if(sp){
                    sp++;
                    char *ep=strchr(sp,' ');
                    if(ep) *ep='\0';
                    if(strcmp(sp,"(none)")!=0 && strlen(sp)>=4)
                        strncpy(g_engine.bestmove, sp, 15);
                    else
                        g_engine.bestmove[0]='\0';
                    got_best=1;
                }
            }
            buf_len=0;
        } else {
            if(buf_len < (int)sizeof(buf)-1)
                buf[buf_len++]=c;
        }
    }
    return got_best;
}

/* Ask engine for best move */
static void engine_go(void) {
    if(!g_engine_active || !g_engine.ready) return;
    char fen[128];
    pos_to_fen(&g_pos, fen);
    char cmd[256];
    snprintf(cmd,256,"position fen %s",fen);
    engine_send(cmd);

    switch(g_tc.mode){
    case TC_DEPTH:
        snprintf(cmd,256,"go depth %d",g_tc.depth);
        break;
    case TC_NODES:
        snprintf(cmd,256,"go nodes %lld",(long long)g_tc.nodes);
        break;
    case TC_TIME:
        snprintf(cmd,256,"go movetime %d",g_tc.time_ms);
        break;
    }
    engine_send(cmd);
}

/* Parse UCI move string like "e2e4" or "e7e8q" */
static int parse_uci_move(const char *str, Move *out, const Position *p) {
    if(strlen(str)<4) return 0;
    int ff=str[0]-'a', fr=str[1]-'1';
    int tf=str[2]-'a', tr=str[3]-'1';
    if(ff<0||ff>7||fr<0||fr>7||tf<0||tf>7||tr<0||tr>7) return 0;
    int from=sq(fr,ff), to=sq(tr,tf);

    Move legal[MAX_MOVES];
    int lc=gen_legal(p,legal);
    for(int i=0;i<lc;i++){
        if(legal[i].from_sq!=from||legal[i].to_sq!=to) continue;
        if(legal[i].flags & FLAG_PROMO){
            if(strlen(str)<5) continue;
            char pc=tolower(str[4]);
            int want_promo;
            if(p->side==WHITE){
                if(pc=='q') want_promo=WQUEEN;
                else if(pc=='r') want_promo=WROOK;
                else if(pc=='b') want_promo=WBISHOP;
                else want_promo=WKNIGHT;
            } else {
                if(pc=='q') want_promo=BQUEEN;
                else if(pc=='r') want_promo=BROOK;
                else if(pc=='b') want_promo=BBISHOP;
                else want_promo=BKNIGHT;
            }
            if(legal[i].promotion==want_promo){ *out=legal[i]; return 1; }
        } else {
            *out=legal[i]; return 1;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   MOVE HISTORY & UNDO
   ═══════════════════════════════════════════════════════════════ */
static void push_history(const Position *before, const Move *m, const char *san) {
    if(g_hist_count >= MAX_HISTORY) return;
    g_history[g_hist_count].pos = *before;
    g_history[g_hist_count].move = *m;
    strncpy(g_history[g_hist_count].pgn_move, san, 15);
    g_hist_count++;
}

static int undo_move(void) {
    if(g_hist_count == 0) return 0;
    g_hist_count--;
    g_pos = g_history[g_hist_count].pos;
    if(g_hist_count > 0){
        g_last_from = g_history[g_hist_count-1].move.from_sq;
        g_last_to   = g_history[g_hist_count-1].move.to_sq;
    } else {
        g_last_from = g_last_to = -1;
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
   UI DRAWING
   ═══════════════════════════════════════════════════════════════ */

/* Determine if square is a legal destination from selected piece */
static int is_legal_dest(int s) {
    if(g_selected_sq < 0) return 0;
    for(int i=0;i<g_legal_count;i++)
        if(g_legal_moves[i].from_sq==g_selected_sq && g_legal_moves[i].to_sq==s)
            return 1;
    return 0;
}

/* Determine bg colour for a square */
static const char *sq_bg(int s) {
    int r=sq_rank(s), f=sq_file(s);
    int is_light = (r+f)%2==1;

    if(s==g_cursor_sq)   return BG_CURSOR;
    if(s==g_selected_sq) return BG_SELECTED;
    if(is_legal_dest(s)) return BG_LEGAL;
    if(s==g_last_from||s==g_last_to) return BG_LAST_MOVE;

    /* check highlight */
    int ksq=find_king(&g_pos, g_pos.side);
    if(s==ksq && in_check(&g_pos)) return BG_CHECK;

    return is_light ? BG_LIGHT_SQ : BG_DARK_SQ;
}

static void draw_board(void) {
    printf(CLEAR_SCREEN);
    printf(HIDE_CURSOR);

    /* title */
    printf(CLR_BOLD "  ╔══════════════════════════════════════════╗\n");
    printf(  "  ║          TERMINAL CHESS  ♔  UCI GUI        ║\n");
    printf(  "  ╚══════════════════════════════════════════╝\n" CLR_RESET);

    /* Engine name */
    if(g_engine_active)
        printf("  Engine: %s%s%s\n", CLR_BOLD, g_engine.name, CLR_RESET);
    else
        printf("  Engine: %s(none – human vs human)%s\n", CLR_DIM, CLR_RESET);

    /* Time control */
    printf("  TC: ");
    switch(g_tc.mode){
    case TC_DEPTH: printf("Depth %d",g_tc.depth); break;
    case TC_NODES: printf("Nodes %lld",(long long)g_tc.nodes); break;
    case TC_TIME:  printf("Time %d ms",g_tc.time_ms); break;
    }
    printf("    Turn: %s%s%s\n",
           CLR_BOLD,
           g_pos.side==WHITE?"WHITE":"BLACK",
           CLR_RESET);

    printf("\n");

    /* file labels */
    printf("    ");
    for(int f=0;f<8;f++) printf(" %c  ",'a'+f);
    printf("\n");
    printf("   ╔");
    for(int f=0;f<8;f++) printf("════%s", f<7?"":"╗");
    printf("\n");

    /* board rows top-down (rank 8 first) */
    for(int r=7;r>=0;r--){
        printf(" %c ║",'1'+r);
        for(int f=0;f<8;f++){
            int s=sq(r,f);
            int pc=g_pos.board[s];
            const char *bg=sq_bg(s);
            const char *fg=(pc!=EMPTY && is_white(pc)) ? FG_WHITE_PIECE : FG_BLACK_PIECE;
            printf("%s%s%s%s%s",bg,fg,CLR_BOLD,piece_glyph[pc],CLR_RESET);
        }
        printf("║ %c\n",'1'+r);

        if(r>0){
            printf("   ╠");
            for(int f=0;f<8;f++) printf("════%s",f<7?"":"╣");
            printf("\n");
        }
    }

    printf("   ╚");
    for(int f=0;f<8;f++) printf("════%s",f<7?"":"╝");
    printf("\n");

    printf("    ");
    for(int f=0;f<8;f++) printf(" %c  ",'a'+f);
    printf("\n\n");

    /* cursor position label */
    int cr=sq_rank(g_cursor_sq), cf=sq_file(g_cursor_sq);
    printf("  Cursor: %c%c   ",'a'+cf,'1'+cr);
    if(g_selected_sq>=0)
        printf("Selected: %c%c   ",'a'+sq_file(g_selected_sq),'1'+sq_rank(g_selected_sq));
    printf("\n");

    /* ── Recent moves (PGN) ── */
    printf("\n  %s── Recent Moves ──%s\n",CLR_BOLD,CLR_RESET);

    /* show up to 5 most recent half-moves */
    int start = (g_hist_count > 5) ? g_hist_count-5 : 0;
    /* find the base move number for `start` */
    for(int i=start;i<g_hist_count;i++){
        /* figure out move number from position stored */
        int mnum = g_history[i].pos.fullmove;
        int side  = g_history[i].pos.side;  /* side that played the move */
        if(side==WHITE)
            printf("  %d. %s%s%s ", mnum, CLR_BOLD, g_history[i].pgn_move, CLR_RESET);
        else
            printf("%s%s%s\n", CLR_BOLD, g_history[i].pgn_move, CLR_RESET);
    }
    /* if last move was white's, newline */
    if(g_hist_count > 0 && g_history[g_hist_count-1].pos.side == BLACK)
        printf("\n");

    /* ── Status / check / mate ── */
    Move lm[MAX_MOVES];
    int lc2 = gen_legal(&g_pos, lm);
    printf("\n  ");
    if(lc2==0){
        if(in_check(&g_pos))
            printf("%s✖ CHECKMATE! %s wins.%s",
                   CLR_BOLD, g_pos.side==WHITE?"Black":"White", CLR_RESET);
        else
            printf("%s½ STALEMATE.%s", CLR_BOLD, CLR_RESET);
    } else if(in_check(&g_pos)){
        printf("%s⚠ CHECK!%s", CLR_BOLD, CLR_RESET);
    } else {
        printf("  ");
    }
    printf("\n");

    /* ── Controls ── */
    printf("\n  %sControls:%s\n",CLR_BOLD,CLR_RESET);
    printf("  Arrow keys / hjkl : move cursor\n");
    printf("  Space / Enter      : select / move piece\n");
    printf("  u                  : undo last move\n");
    printf("  e                  : load engine (enter path)\n");
    printf("  t                  : change time control\n");
    printf("  g                  : ask engine for move\n");
    printf("  q                  : quit\n");

    fflush(stdout);
}

/* Promotion selection menu */
static int prompt_promotion(int color) {
    printf("\n  %sPromotion! Choose piece:%s\n",CLR_BOLD,CLR_RESET);
    if(color==WHITE)
        printf("  [q] Queen  [r] Rook  [b] Bishop  [n] Knight\n");
    else
        printf("  [q] Queen  [r] Rook  [b] Bishop  [n] Knight\n");
    fflush(stdout);

    while(1){
        char c=0;
        read(STDIN_FILENO,&c,1);
        if(color==WHITE){
            if(c=='q') return WQUEEN;
            if(c=='r') return WROOK;
            if(c=='b') return WBISHOP;
            if(c=='n') return WKNIGHT;
        } else {
            if(c=='q') return BQUEEN;
            if(c=='r') return BROOK;
            if(c=='b') return BBISHOP;
            if(c=='n') return BKNIGHT;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   TIME CONTROL MENU
   ═══════════════════════════════════════════════════════════════ */
static void menu_time_control(void) {
    /* restore for line input */
    term_restore();
    printf(CLEAR_SCREEN);
    printf(CLR_BOLD "── Time Control ──\n" CLR_RESET);
    printf("1) Depth   (current: %d plies)\n", g_tc.depth);
    printf("2) Nodes   (current: %lld)\n",(long long)g_tc.nodes);
    printf("3) Time    (current: %d ms)\n", g_tc.time_ms);
    printf("Choice [1-3]: ");
    fflush(stdout);

    char line[64];
    if(!fgets(line,64,stdin)) goto done;
    int ch=atoi(line);

    if(ch==1){
        g_tc.mode=TC_DEPTH;
        printf("Enter depth (plies): ");
        fflush(stdout);
        if(fgets(line,64,stdin)){ g_tc.depth=atoi(line); if(g_tc.depth<1)g_tc.depth=1; }
    } else if(ch==2){
        g_tc.mode=TC_NODES;
        printf("Enter nodes: ");
        fflush(stdout);
        if(fgets(line,64,stdin)){ g_tc.nodes=atoll(line); if(g_tc.nodes<1)g_tc.nodes=1; }
    } else if(ch==3){
        g_tc.mode=TC_TIME;
        printf("Enter time in ms: ");
        fflush(stdout);
        if(fgets(line,64,stdin)){ g_tc.time_ms=atoi(line); if(g_tc.time_ms<1)g_tc.time_ms=1; }
    }
done:
    term_raw();
}

/* ═══════════════════════════════════════════════════════════════
   ENGINE LOAD MENU
   ═══════════════════════════════════════════════════════════════ */
static void menu_load_engine(void) {
    term_restore();
    printf(CLEAR_SCREEN);
    printf(CLR_BOLD "── Load UCI Engine ──\n" CLR_RESET);
    printf("Enter full path to engine binary\n");
    printf("(e.g. /usr/local/bin/stockfish): ");
    fflush(stdout);

    char path[512];
    if(!fgets(path,512,stdin)){ term_raw(); return; }
    /* trim newline */
    path[strcspn(path,"\n")]='\0';
    if(strlen(path)==0){ term_raw(); return; }

    /* kill existing engine */
    if(g_engine_active){
        engine_send("quit");
        usleep(200000);
        kill(g_engine.pid, SIGTERM);
        close(g_engine.to_engine[1]);
        close(g_engine.from_engine[0]);
        g_engine_active=0;
    }

    if(engine_launch(path)){
        g_engine_active=1;
        /* wait for uciok */
        printf("Connecting to engine...\n");
        fflush(stdout);
        /* send isready */
        engine_send("isready");
        time_t start=time(NULL);
        while(!g_engine.ready && time(NULL)-start<5){
            engine_read();
            usleep(50000);
        }
        if(g_engine.ready)
            printf("Engine ready: %s\n",g_engine.name);
        else
            printf("Engine may not be ready (timeout).\n");
    } else {
        printf("Failed to launch engine.\n");
    }
    printf("Press Enter to continue...");
    fflush(stdout);
    char tmp[4]; fgets(tmp,4,stdin);
    term_raw();
}

/* ═══════════════════════════════════════════════════════════════
   EXECUTE A MOVE (human or engine)
   ═══════════════════════════════════════════════════════════════ */
static void execute_move(Move *m) {
    char san[32];
    move_to_san(&g_pos, m, san);
    push_history(&g_pos, m, san);
    g_last_from = m->from_sq;
    g_last_to   = m->to_sq;
    apply_move(&g_pos, m);
    g_selected_sq = -1;
    g_legal_count = gen_legal(&g_pos, g_legal_moves);
}

/* ═══════════════════════════════════════════════════════════════
   INPUT HANDLING
   ═══════════════════════════════════════════════════════════════ */
static void handle_key(int c) {
    /* Arrow keys come as ESC [ A/B/C/D */
    int cr=sq_rank(g_cursor_sq), cf=sq_file(g_cursor_sq);

    /* movement */
    int moved=0;
    if(c=='h'||c=='a'){ cf--; moved=1; }
    if(c=='l'||c=='d'){ cf++; moved=1; }
    if(c=='k'||c=='w'){ cr++; moved=1; }
    if(c=='j'||c=='s'){ cr--; moved=1; }
    if(moved){
        if(cf<0)cf=0; if(cf>7)cf=7;
        if(cr<0)cr=0; if(cr>7)cr=7;
        g_cursor_sq=sq(cr,cf);
        return;
    }

    /* select / move */
    if(c==' '||c=='\n'||c=='\r'){
        if(g_selected_sq < 0){
            /* select if our piece */
            int pc=g_pos.board[g_cursor_sq];
            if(pc!=EMPTY && piece_color(pc)==g_pos.side){
                g_selected_sq=g_cursor_sq;
                /* pre-compute legal moves */
                g_legal_count=0;
                Move all[MAX_MOVES];
                int ac=gen_legal(&g_pos,all);
                for(int i=0;i<ac;i++)
                    if(all[i].from_sq==g_selected_sq)
                        g_legal_moves[g_legal_count++]=all[i];
            }
        } else {
            /* try to move */
            if(g_cursor_sq==g_selected_sq){
                g_selected_sq=-1; g_legal_count=0; return;
            }
            /* find matching legal move */
            Move candidates[16]; int nc=0;
            for(int i=0;i<g_legal_count;i++)
                if(g_legal_moves[i].from_sq==g_selected_sq &&
                   g_legal_moves[i].to_sq==g_cursor_sq)
                    candidates[nc++]=g_legal_moves[i];

            if(nc==0){
                /* maybe selecting a new piece */
                int pc=g_pos.board[g_cursor_sq];
                if(pc!=EMPTY && piece_color(pc)==g_pos.side){
                    g_selected_sq=g_cursor_sq;
                    g_legal_count=0;
                    Move all[MAX_MOVES];
                    int ac=gen_legal(&g_pos,all);
                    for(int i=0;i<ac;i++)
                        if(all[i].from_sq==g_selected_sq)
                            g_legal_moves[g_legal_count++]=all[i];
                } else {
                    g_selected_sq=-1; g_legal_count=0;
                }
            } else if(nc==1){
                Move m=candidates[0];
                if(m.flags & FLAG_PROMO){
                    /* need to ask which piece */
                    draw_board();
                    int pp=prompt_promotion(g_pos.side);
                    m.promotion=pp;
                }
                execute_move(&m);
            } else {
                /* multiple candidates = promotion choices; ask */
                draw_board();
                int pp=prompt_promotion(g_pos.side);
                for(int i=0;i<nc;i++){
                    if(candidates[i].promotion==pp){
                        execute_move(&candidates[i]); break;
                    }
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   MAIN LOOP
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    /* Init position */
    pos_start(&g_pos);
    g_legal_count = gen_legal(&g_pos, g_legal_moves);

    term_raw();
    printf(HIDE_CURSOR);

    int engine_thinking = 0;

    while(g_running){
        draw_board();

        /* if engine is thinking, poll for result */
        if(engine_thinking && g_engine_active){
            /* non-blocking poll loop until bestmove */
            time_t think_start=time(NULL);
            int max_wait = 60; /* seconds */
            while(engine_thinking){
                /* draw loading indicator */
                /* check for key input (to allow quit) */
                fd_set fds;
                FD_ZERO(&fds); FD_SET(STDIN_FILENO,&fds);
                struct timeval tv={0,50000};
                int sel=select(STDIN_FILENO+1,&fds,NULL,NULL,&tv);
                if(sel>0){
                    char c=0;
                    read(STDIN_FILENO,&c,1);
                    if(c=='q'){ g_running=0; break; }
                }

                if(engine_read()){
                    /* got bestmove */
                    engine_thinking=0;
                    if(strlen(g_engine.bestmove)>=4){
                        Move m;
                        if(parse_uci_move(g_engine.bestmove,&m,&g_pos))
                            execute_move(&m);
                    }
                    draw_board();
                    break;
                }
                if(time(NULL)-think_start > max_wait){
                    engine_thinking=0;
                    engine_send("stop");
                    break;
                }
            }
            continue;
        }

        /* read key */
        unsigned char seq[4]={0};
        int n=(int)read(STDIN_FILENO,seq,1);
        if(n<=0) continue;

        unsigned char c=seq[0];

        if(c=='q'){ g_running=0; break; }

        if(c=='u'){
            undo_move();
            /* if engine just moved, undo again to get back to human turn */
            if(g_engine_active && g_hist_count>0 &&
               g_history[g_hist_count-1].pos.side!=g_pos.side)
                undo_move();
            g_selected_sq=-1; g_legal_count=0;
            g_legal_count=gen_legal(&g_pos,g_legal_moves);
            continue;
        }

        if(c=='e'){ menu_load_engine(); continue; }
        if(c=='t'){ menu_time_control(); continue; }

        if(c=='g'){
            /* force engine to move for current side */
            if(g_engine_active && g_engine.ready){
                engine_go();
                engine_thinking=1;
            }
            continue;
        }

        /* escape sequence (arrow keys) */
        if(c==27){
            /* try to read [ and direction */
            struct timeval tv={0,50000};
            fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO,&fds);
            if(select(STDIN_FILENO+1,&fds,NULL,NULL,&tv)>0){
                read(STDIN_FILENO,seq+1,1);
                if(seq[1]=='['){
                    FD_ZERO(&fds); FD_SET(STDIN_FILENO,&fds);
                    tv.tv_usec=50000;
                    if(select(STDIN_FILENO+1,&fds,NULL,NULL,&tv)>0){
                        read(STDIN_FILENO,seq+2,1);
                        switch(seq[2]){
                        case 'A': handle_key('k'); break; /* up    */
                        case 'B': handle_key('j'); break; /* down  */
                        case 'C': handle_key('l'); break; /* right */
                        case 'D': handle_key('h'); break; /* left  */
                        }
                    }
                }
            }
            continue;
        }

        /* normal key */
        handle_key((int)c);

        /* After human move, trigger engine if active and it's engine's turn */
        /* (We treat engine as always playing BLACK for simplicity, 
            but 'g' lets you manually request any side) */
        if(g_engine_active && g_engine.ready && !engine_thinking){
            /* auto-play: engine plays as black */
            if(g_pos.side==BLACK){
                /* check game not over */
                Move lm2[MAX_MOVES];
                if(gen_legal(&g_pos,lm2)>0){
                    engine_go();
                    engine_thinking=1;
                }
            }
        }
    }

    cleanup_and_exit(0);
    return 0;
}
