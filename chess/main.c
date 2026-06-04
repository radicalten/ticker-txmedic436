#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

/* ─── Terminal / ANSI ──────────────────────────────────────────────── */
#define ANSI_RESET       "\033[0m"
#define ANSI_BOLD        "\033[1m"
#define ANSI_REVERSE     "\033[7m"

/* 256-color backgrounds */
#define BG_LIGHT         "\033[48;5;223m"   /* warm cream  */
#define BG_DARK          "\033[48;5;137m"   /* warm brown  */
#define BG_SEL           "\033[48;5;184m"   /* yellow      */
#define BG_LEGAL         "\033[48;5;107m"   /* olive-green */
#define BG_LASTMOVE      "\033[48;5;179m"   /* amber       */
#define BG_CHECK         "\033[48;5;160m"   /* red         */
#define BG_CURSOR        "\033[48;5;75m"    /* sky-blue    */

#define FG_WHITE_PIECE   "\033[38;5;255m"
#define FG_BLACK_PIECE   "\033[38;5;16m"

/* ─── Board constants ──────────────────────────────────────────────── */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  0
#define BLACK  1

#define MAX_MOVES      512
#define MAX_HIST       1024
#define MAX_PGN_LEN    65536
#define ENGINE_BUF     4096

/* ─── Piece encoding:  positive = white, negative = black ─────────── */
typedef int8_t  Piece;   /* 0 empty, 1-6 white, -1 to -6 black */
typedef uint8_t Square;  /* 0-63  a1=0 … h8=63 */

/* ─── Move ─────────────────────────────────────────────────────────── */
typedef struct {
    Square from, to;
    Piece  promo;          /* 0 = none, else piece type (white-relative) */
    /* flags */
    uint8_t castle_ks : 1;
    uint8_t castle_qs : 1;
    uint8_t ep        : 1; /* en-passant capture */
    uint8_t is_null   : 1;
    /* for undo */
    Piece   captured;
    Square  ep_sq_before;  /* en-passant square before this move */
    uint8_t castle_rights_before;
    uint16_t halfmove_before;
} Move;

/* castle rights bits */
#define CR_WK  1
#define CR_WQ  2
#define CR_BK  4
#define CR_BQ  8

/* ─── Board state ──────────────────────────────────────────────────── */
typedef struct {
    Piece   sq[64];
    int     side;          /* WHITE or BLACK to move */
    uint8_t castle;        /* bitmask CR_* */
    Square  ep;            /* en-passant target square, 255 = none */
    uint16_t halfmove;     /* 50-move rule counter */
    uint32_t fullmove;
} Board;

/* ─── Game history ─────────────────────────────────────────────────── */
typedef struct {
    Move  move;
    Board board_before;
    char  san[16];
} HistEntry;

/* ─── Global game state ────────────────────────────────────────────── */
static Board     g_board;
static HistEntry g_hist[MAX_HIST];
static int       g_hist_len = 0;

/* Cursor / selection */
static int g_cursor_sq  = 0;  /* square under cursor (0-63) */
static int g_sel_sq     = -1; /* selected piece square (-1 = none) */
static int g_legal_buf[MAX_MOVES];
static int g_legal_cnt  = 0;

/* Engine */
static pid_t g_eng_pid  = -1;
static int   g_eng_in   = -1; /* write to engine */
static int   g_eng_out  = -1; /* read from engine */
static char  g_eng_path[512] = "";
static int   g_eng_depth= 15;
static int   g_eng_nodes= 0;   /* 0 = unlimited */
static int   g_eng_time = 2000;/* ms */
static int   g_engine_thinking = 0;
static char  g_eng_best[16]    = "";
static int   g_human_side      = WHITE; /* human plays white by default */
static int   g_engine_enabled  = 0;

/* PGN */
static char  g_pgn[MAX_PGN_LEN];
static int   g_pgn_len = 0;

/* Terminal */
static struct termios g_old_term;
static int   g_running = 1;

/* Status message */
static char  g_status[256] = "Welcome! Arrow keys move cursor, Enter selects, U=undo, Q=quit";

/* ═══════════════════════════════════════════════════════════════════
   SECTION 1 – Utility helpers
   ═══════════════════════════════════════════════════════════════════ */

static inline int sq(int file, int rank){ return rank*8 + file; } /* rank 0=rank1 */
static inline int file_of(int s){ return s & 7; }
static inline int rank_of(int s){ return s >> 3; }
static inline int piece_type(Piece p){ return p < 0 ? -p : p; }
static inline int piece_color(Piece p){ return p < 0 ? BLACK : WHITE; }

static const char *piece_unicode_white[] = {
    " ", "♙", "♘", "♗", "♖", "♕", "♔"
};
static const char *piece_unicode_black[] = {
    " ", "♟", "♞", "♝", "♜", "♛", "♚"
};

static const char piece_char[] = " PNBRQK";

static const char *piece_str(Piece p){
    if(p == 0) return " ";
    if(p > 0)  return piece_unicode_white[p];
    return piece_unicode_black[-p];
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 2 – Board initialisation / FEN
   ═══════════════════════════════════════════════════════════════════ */

static void board_clear(Board *b){
    memset(b->sq, 0, 64);
    b->side     = WHITE;
    b->castle   = 0;
    b->ep       = 255;
    b->halfmove = 0;
    b->fullmove = 1;
}

static void board_start(Board *b){
    board_clear(b);
    /* white pieces on ranks 1-2 */
    int back_w[] = {ROOK,KNIGHT,BISHOP,QUEEN,KING,BISHOP,KNIGHT,ROOK};
    for(int f=0;f<8;f++){
        b->sq[sq(f,0)] = (Piece)back_w[f];
        b->sq[sq(f,1)] = PAWN;
        b->sq[sq(f,6)] = -PAWN;
        b->sq[sq(f,7)] = -(Piece)back_w[f];
    }
    b->castle = CR_WK|CR_WQ|CR_BK|CR_BQ;
}

static void board_from_fen(Board *b, const char *fen){
    board_clear(b);
    int f=0, r=7;
    while(*fen && *fen != ' '){
        char c = *fen++;
        if(c == '/'){  r--; f=0; continue; }
        if(c >= '1' && c <= '8'){ f += c-'0'; continue; }
        Piece p = 0;
        switch(tolower((unsigned char)c)){
            case 'p': p=PAWN;   break;
            case 'n': p=KNIGHT; break;
            case 'b': p=BISHOP; break;
            case 'r': p=ROOK;   break;
            case 'q': p=QUEEN;  break;
            case 'k': p=KING;   break;
        }
        if(islower((unsigned char)c)) p = -p;
        b->sq[sq(f,r)] = p;
        f++;
    }
    if(*fen == ' ') fen++;
    b->side = (*fen == 'b') ? BLACK : WHITE;
    fen++;
    if(*fen == ' ') fen++;
    /* castling */
    while(*fen && *fen != ' '){
        if(*fen == 'K') b->castle |= CR_WK;
        if(*fen == 'Q') b->castle |= CR_WQ;
        if(*fen == 'k') b->castle |= CR_BK;
        if(*fen == 'q') b->castle |= CR_BQ;
        fen++;
    }
    if(*fen == ' ') fen++;
    /* en-passant */
    if(*fen != '-'){
        int epf = fen[0]-'a', epr = fen[1]-'1';
        b->ep = (Square)sq(epf,epr);
        fen += 2;
    } else { b->ep=255; fen++; }
    if(*fen==' ') fen++;
    b->halfmove = (uint16_t)atoi(fen);
    while(*fen && *fen != ' ') fen++;
    if(*fen==' ') fen++;
    b->fullmove = (uint32_t)atoi(fen);
}

/* FEN export */
static void board_to_fen(const Board *b, char *out){
    int pos=0;
    for(int r=7;r>=0;r--){
        int empty=0;
        for(int f=0;f<8;f++){
            Piece p = b->sq[sq(f,r)];
            if(p==0){ empty++; continue; }
            if(empty){ out[pos++]='0'+empty; empty=0; }
            int t = piece_type(p);
            char c = piece_char[t];
            if(piece_color(p)==BLACK) c = tolower((unsigned char)c);
            out[pos++]=c;
        }
        if(empty) out[pos++]='0'+empty;
        if(r>0) out[pos++]='/';
    }
    out[pos++]=' ';
    out[pos++] = b->side==WHITE?'w':'b';
    out[pos++]=' ';
    if(!b->castle){ out[pos++]='-'; }
    else{
        if(b->castle&CR_WK) out[pos++]='K';
        if(b->castle&CR_WQ) out[pos++]='Q';
        if(b->castle&CR_BK) out[pos++]='k';
        if(b->castle&CR_BQ) out[pos++]='q';
    }
    out[pos++]=' ';
    if(b->ep==255){ out[pos++]='-'; }
    else{
        out[pos++]='a'+file_of(b->ep);
        out[pos++]='1'+rank_of(b->ep);
    }
    pos += sprintf(out+pos," %d %d",b->halfmove,b->fullmove);
    out[pos]='\0';
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 3 – Move generation
   ═══════════════════════════════════════════════════════════════════ */

/* We store moves in a simple array */
typedef struct { Move m[MAX_MOVES]; int n; } MoveList;

static void ml_add(MoveList *ml, Square from, Square to,
                   Piece cap, Piece promo,
                   int ks, int qs, int ep_flag,
                   const Board *b)
{
    if(ml->n >= MAX_MOVES) return;
    Move *m = &ml->m[ml->n++];
    m->from  = from; m->to = to;
    m->promo = promo; m->captured = cap;
    m->castle_ks = ks; m->castle_qs = qs;
    m->ep    = ep_flag; m->is_null = 0;
    m->ep_sq_before       = b->ep;
    m->castle_rights_before = b->castle;
    m->halfmove_before    = b->halfmove;
}

/* direction arrays */
static const int KNIGHT_D[8][2]={{-2,-1},{-2,1},{-1,-2},{-1,2},
                                  {1,-2},{1,2},{2,-1},{2,1}};
static const int BISHOP_D[4][2]={{-1,-1},{-1,1},{1,-1},{1,1}};
static const int ROOK_D[4][2]  ={{-1,0},{1,0},{0,-1},{0,1}};
static const int QUEEN_D[8][2] ={{-1,-1},{-1,1},{1,-1},{1,1},
                                  {-1,0},{1,0},{0,-1},{0,1}};

static void gen_slider(MoveList *ml, const Board *b, Square s,
                       const int dirs[][2], int nd)
{
    int color = piece_color(b->sq[s]);
    for(int d=0;d<nd;d++){
        int f=file_of(s)+dirs[d][0];
        int r=rank_of(s)+dirs[d][1];
        while(f>=0&&f<8&&r>=0&&r<8){
            Square t = (Square)sq(f,r);
            Piece  p = b->sq[t];
            if(p==0)         ml_add(ml,s,t,0,0,0,0,0,b);
            else{
                if(piece_color(p)!=color) ml_add(ml,s,t,p,0,0,0,0,b);
                break;
            }
            f+=dirs[d][0]; r+=dirs[d][1];
        }
    }
}

static void gen_pawn(MoveList *ml, const Board *b, Square s){
    int color = piece_color(b->sq[s]);
    int dir   = color==WHITE ? 1 : -1;
    int start = color==WHITE ? 1 : 6;
    int promo_rank = color==WHITE ? 7 : 0;
    int f = file_of(s), r = rank_of(s);

    /* push one */
    int nr = r+dir;
    if(nr>=0&&nr<8){
        Square t=(Square)sq(f,nr);
        if(b->sq[t]==0){
            if(nr==promo_rank){
                for(int pt=QUEEN;pt>=KNIGHT;pt--)
                    ml_add(ml,s,t,0,(Piece)pt,0,0,0,b);
            } else {
                ml_add(ml,s,t,0,0,0,0,0,b);
                /* push two */
                if(r==start){
                    Square t2=(Square)sq(f,r+2*dir);
                    if(b->sq[t2]==0)
                        ml_add(ml,s,t2,0,0,0,0,0,b);
                }
            }
        }
        /* captures */
        for(int df=-1;df<=1;df+=2){
            int nf=f+df;
            if(nf<0||nf>=8) continue;
            Square t2=(Square)sq(nf,nr);
            Piece  cap=b->sq[t2];
            if(cap!=0 && piece_color(cap)!=color){
                if(nr==promo_rank){
                    for(int pt=QUEEN;pt>=KNIGHT;pt--)
                        ml_add(ml,s,t2,cap,(Piece)pt,0,0,0,b);
                } else ml_add(ml,s,t2,cap,0,0,0,0,b);
            }
            /* en-passant */
            if(b->ep!=255 && t2==b->ep)
                ml_add(ml,s,t2,
                       color==WHITE ? -PAWN : PAWN,
                       0,0,0,1,b);
        }
    }
}

static void gen_moves_pseudo(MoveList *ml, const Board *b){
    for(int s=0;s<64;s++){
        Piece p=b->sq[s];
        if(p==0) continue;
        if(piece_color(p)!=b->side) continue;
        int t=piece_type(p);
        switch(t){
        case PAWN:   gen_pawn(ml,b,(Square)s); break;
        case KNIGHT:{
            int f=file_of(s),r=rank_of(s);
            for(int d=0;d<8;d++){
                int nf=f+KNIGHT_D[d][0],nr=r+KNIGHT_D[d][1];
                if(nf<0||nf>=8||nr<0||nr>=8) continue;
                Square t2=(Square)sq(nf,nr);
                Piece cap=b->sq[t2];
                if(cap==0||piece_color(cap)!=b->side)
                    ml_add(ml,s,t2,cap,0,0,0,0,b);
            }
            break;
        }
        case BISHOP: gen_slider(ml,b,(Square)s,BISHOP_D,4); break;
        case ROOK:   gen_slider(ml,b,(Square)s,ROOK_D,4);   break;
        case QUEEN:  gen_slider(ml,b,(Square)s,QUEEN_D,8);  break;
        case KING:{
            int f=file_of(s),r=rank_of(s);
            for(int d=0;d<8;d++){
                int nf=f+QUEEN_D[d][0],nr=r+QUEEN_D[d][1];
                if(nf<0||nf>=8||nr<0||nr>=8) continue;
                Square t2=(Square)sq(nf,nr);
                Piece cap=b->sq[t2];
                if(cap==0||piece_color(cap)!=b->side)
                    ml_add(ml,s,t2,cap,0,0,0,0,b);
            }
            /* castling */
            if(b->side==WHITE && s==sq(4,0)){
                if((b->castle&CR_WK) &&
                   b->sq[sq(5,0)]==0 && b->sq[sq(6,0)]==0)
                    ml_add(ml,(Square)s,(Square)sq(6,0),0,0,1,0,0,b);
                if((b->castle&CR_WQ) &&
                   b->sq[sq(3,0)]==0 && b->sq[sq(2,0)]==0 && b->sq[sq(1,0)]==0)
                    ml_add(ml,(Square)s,(Square)sq(2,0),0,0,0,1,0,b);
            }
            if(b->side==BLACK && s==sq(4,7)){
                if((b->castle&CR_BK) &&
                   b->sq[sq(5,7)]==0 && b->sq[sq(6,7)]==0)
                    ml_add(ml,(Square)s,(Square)sq(6,7),0,0,1,0,0,b);
                if((b->castle&CR_BQ) &&
                   b->sq[sq(3,7)]==0 && b->sq[sq(2,7)]==0 && b->sq[sq(1,7)]==0)
                    ml_add(ml,(Square)s,(Square)sq(2,7),0,0,0,1,0,b);
            }
            break;
        }
        }
    }
}

/* ─── Apply / undo move ────────────────────────────────────────────── */
static void apply_move(Board *b, const Move *m){
    Piece p = b->sq[m->from];
    int   color = piece_color(p);

    b->sq[m->from] = EMPTY;

    /* en-passant capture removes pawn on different square */
    if(m->ep){
        int cap_rank = color==WHITE ? rank_of(m->to)-1 : rank_of(m->to)+1;
        b->sq[sq(file_of(m->to),cap_rank)] = EMPTY;
    }

    /* place piece */
    if(m->promo)
        b->sq[m->to] = color==WHITE ? m->promo : -(m->promo);
    else
        b->sq[m->to] = p;

    /* castling – move rook */
    if(m->castle_ks){
        Square rr = color==WHITE ? sq(7,0) : sq(7,7);
        Square rd = color==WHITE ? sq(5,0) : sq(5,7);
        b->sq[rd] = b->sq[rr]; b->sq[rr] = EMPTY;
    }
    if(m->castle_qs){
        Square rr = color==WHITE ? sq(0,0) : sq(0,7);
        Square rd = color==WHITE ? sq(3,0) : sq(3,7);
        b->sq[rd] = b->sq[rr]; b->sq[rr] = EMPTY;
    }

    /* update castling rights */
    if(piece_type(p)==KING){
        if(color==WHITE) b->castle &= ~(CR_WK|CR_WQ);
        else             b->castle &= ~(CR_BK|CR_BQ);
    }
    if(m->from==sq(0,0)||m->to==sq(0,0)) b->castle &= ~CR_WQ;
    if(m->from==sq(7,0)||m->to==sq(7,0)) b->castle &= ~CR_WK;
    if(m->from==sq(0,7)||m->to==sq(0,7)) b->castle &= ~CR_BQ;
    if(m->from==sq(7,7)||m->to==sq(7,7)) b->castle &= ~CR_BK;

    /* en-passant target */
    b->ep = 255;
    if(piece_type(p)==PAWN && abs((int)rank_of(m->to)-(int)rank_of(m->from))==2){
        b->ep = (Square)sq(file_of(m->from),
                           (rank_of(m->from)+rank_of(m->to))/2);
    }

    /* half-move clock */
    if(piece_type(p)==PAWN || m->captured) b->halfmove=0;
    else b->halfmove++;

    if(b->side==BLACK) b->fullmove++;
    b->side ^= 1;
}

static void undo_move(Board *b, const Move *m, const Board *before){
    *b = *before;
}

/* ─── Is square attacked by `attacker` ────────────────────────────── */
static int sq_attacked(const Board *b, Square s, int attacker){
    /* pawns */
    int pdir = attacker==WHITE ? -1 : 1;
    Piece apawn = attacker==WHITE ? PAWN : -PAWN;
    for(int df=-1;df<=1;df+=2){
        int nf=file_of(s)+df, nr=rank_of(s)+pdir;
        if(nf>=0&&nf<8&&nr>=0&&nr<8)
            if(b->sq[sq(nf,nr)]==apawn) return 1;
    }
    /* knights */
    for(int d=0;d<8;d++){
        int nf=file_of(s)+KNIGHT_D[d][0],nr=rank_of(s)+KNIGHT_D[d][1];
        if(nf<0||nf>=8||nr<0||nr>=8) continue;
        Piece p=b->sq[sq(nf,nr)];
        if(piece_type(p)==KNIGHT && piece_color(p)==attacker) return 1;
    }
    /* bishops/queens */
    for(int d=0;d<4;d++){
        int f=file_of(s)+BISHOP_D[d][0],r=rank_of(s)+BISHOP_D[d][1];
        while(f>=0&&f<8&&r>=0&&r<8){
            Piece p=b->sq[sq(f,r)];
            if(p){
                if(piece_color(p)==attacker &&
                   (piece_type(p)==BISHOP||piece_type(p)==QUEEN)) return 1;
                break;
            }
            f+=BISHOP_D[d][0]; r+=BISHOP_D[d][1];
        }
    }
    /* rooks/queens */
    for(int d=0;d<4;d++){
        int f=file_of(s)+ROOK_D[d][0],r=rank_of(s)+ROOK_D[d][1];
        while(f>=0&&f<8&&r>=0&&r<8){
            Piece p=b->sq[sq(f,r)];
            if(p){
                if(piece_color(p)==attacker &&
                   (piece_type(p)==ROOK||piece_type(p)==QUEEN)) return 1;
                break;
            }
            f+=ROOK_D[d][0]; r+=ROOK_D[d][1];
        }
    }
    /* king */
    for(int d=0;d<8;d++){
        int nf=file_of(s)+QUEEN_D[d][0],nr=rank_of(s)+QUEEN_D[d][1];
        if(nf<0||nf>=8||nr<0||nr>=8) continue;
        Piece p=b->sq[sq(nf,nr)];
        if(piece_type(p)==KING && piece_color(p)==attacker) return 1;
    }
    return 0;
}

static int in_check(const Board *b, int color){
    /* find king */
    Piece king = color==WHITE ? KING : -KING;
    Square ks  = 0;
    for(int s=0;s<64;s++) if(b->sq[s]==king){ks=(Square)s;break;}
    return sq_attacked(b, ks, color^1);
}

/* Generate strictly legal moves */
static void gen_legal(MoveList *ml, const Board *b){
    MoveList pseudo; pseudo.n=0;
    gen_moves_pseudo(&pseudo, b);
    for(int i=0;i<pseudo.n;i++){
        const Move *m = &pseudo.m[i];
        /* check castling squares not attacked */
        if(m->castle_ks || m->castle_qs){
            int r = b->side==WHITE ? 0 : 7;
            int mid = m->castle_ks ? sq(5,r) : sq(3,r);
            int king_sq = sq(4,r);
            if(in_check(b,b->side)) continue;
            if(sq_attacked(b,(Square)mid,b->side^1)) continue;
            if(sq_attacked(b,(Square)m->to,b->side^1)) continue;
            (void)king_sq;
        }
        Board after = *b;
        apply_move(&after, m);
        if(!in_check(&after, b->side))
            ml->m[ml->n++] = *m;
    }
}

/* ─── Check/mate/stale detection ───────────────────────────────────── */
static int is_checkmate(const Board *b){
    MoveList ml; ml.n=0;
    gen_legal(&ml, b);
    return ml.n==0 && in_check(b,b->side);
}

static int is_stalemate(const Board *b){
    MoveList ml; ml.n=0;
    gen_legal(&ml, b);
    return ml.n==0 && !in_check(b,b->side);
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 4 – SAN notation
   ═══════════════════════════════════════════════════════════════════ */

static void move_to_uci(const Move *m, char *out){
    out[0]='a'+file_of(m->from);
    out[1]='1'+rank_of(m->from);
    out[2]='a'+file_of(m->to);
    out[3]='1'+rank_of(m->to);
    if(m->promo){
        char pc = tolower((unsigned char)piece_char[m->promo]);
        out[4]=pc; out[5]='\0';
    } else out[4]='\0';
}

static void move_to_san(const Board *b, const Move *m, char *out){
    /* generates SAN string for move m on board b (before move applied) */
    char tmp[16]; int pos=0;
    int pt = piece_type(b->sq[m->from]);

    /* castling */
    if(m->castle_ks){ strcpy(out,"O-O");   return; }
    if(m->castle_qs){ strcpy(out,"O-O-O"); return; }

    /* piece letter */
    if(pt != PAWN) tmp[pos++] = piece_char[pt];

    /* disambiguation */
    if(pt != PAWN){
        MoveList ml; ml.n=0;
        gen_legal(&ml, b);
        int ambig_file=0, ambig_rank=0, ambig=0;
        for(int i=0;i<ml.n;i++){
            const Move *o=&ml.m[i];
            if(o->from==m->from) continue;
            if(o->to!=m->to) continue;
            if(piece_type(b->sq[o->from])!=pt) continue;
            ambig=1;
            if(file_of(o->from)==file_of(m->from)) ambig_file=1;
            if(rank_of(o->from)==rank_of(m->from)) ambig_rank=1;
        }
        if(ambig){
            if(!ambig_file)                   tmp[pos++]='a'+file_of(m->from);
            else if(!ambig_rank)              tmp[pos++]='1'+rank_of(m->from);
            else{ tmp[pos++]='a'+file_of(m->from); tmp[pos++]='1'+rank_of(m->from); }
        }
    }

    /* pawn capture file */
    if(pt==PAWN && (m->captured || m->ep))
        tmp[pos++]='a'+file_of(m->from);

    /* capture */
    if(m->captured || m->ep) tmp[pos++]='x';

    /* destination */
    tmp[pos++]='a'+file_of(m->to);
    tmp[pos++]='1'+rank_of(m->to);

    /* promotion */
    if(m->promo){ tmp[pos++]='='; tmp[pos++]=piece_char[m->promo]; }

    tmp[pos]='\0';

    /* check / mate */
    Board after = *b;
    apply_move(&after, m);
    if(is_checkmate(&after))      tmp[pos++]='+', tmp[pos++]='+', tmp[pos]='\0';
    else if(in_check(&after, after.side)) { tmp[pos++]='+'; tmp[pos]='\0'; }

    strcpy(out, tmp);
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 5 – PGN management
   ═══════════════════════════════════════════════════════════════════ */

static void pgn_reset(void){
    /* header */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    g_pgn_len = snprintf(g_pgn, MAX_PGN_LEN,
        "[Event \"Terminal Chess\"]\n"
        "[Date \"%04d.%02d.%02d\"]\n"
        "[White \"%s\"]\n"
        "[Black \"%s\"]\n\n",
        t->tm_year+1900, t->tm_mon+1, t->tm_mday,
        g_human_side==WHITE ? "Human" : g_eng_path[0]?g_eng_path:"Engine",
        g_human_side==BLACK ? "Human" : g_eng_path[0]?g_eng_path:"Engine");
}

static void pgn_append_move(int move_num, int side, const char *san){
    char buf[64];
    if(side==WHITE)
        snprintf(buf,64,"%d. %s ", move_num, san);
    else
        snprintf(buf,64,"%s ", san);
    int l=strlen(buf);
    if(g_pgn_len+l < MAX_PGN_LEN-1){
        memcpy(g_pgn+g_pgn_len, buf, l);
        g_pgn_len += l;
        g_pgn[g_pgn_len]='\0';
    }
}

static void pgn_rebuild(void){
    pgn_reset();
    for(int i=0;i<g_hist_len;i++){
        int mn = (int)(g_hist[i].board_before.fullmove);
        int side = g_hist[i].board_before.side;
        pgn_append_move(mn, side, g_hist[i].san);
    }
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 6 – Engine I/O
   ═══════════════════════════════════════════════════════════════════ */

static void eng_write(const char *s){
    if(g_eng_in<0) return;
    write(g_eng_in, s, strlen(s));
    write(g_eng_in, "\n", 1);
}

static int eng_readline(char *buf, int max, int timeout_ms){
    if(g_eng_out<0) return 0;
    fd_set fds; FD_ZERO(&fds); FD_SET(g_eng_out,&fds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms/1000;
    tv.tv_usec = (timeout_ms%1000)*1000;
    int r = select(g_eng_out+1,&fds,NULL,NULL,&tv);
    if(r<=0) return 0;
    int pos=0;
    while(pos<max-1){
        char c;
        int n=read(g_eng_out,&c,1);
        if(n<=0) break;
        if(c=='\n') break;
        if(c!='\r') buf[pos++]=c;
    }
    buf[pos]='\0';
    return pos>0;
}

static int engine_start(const char *path){
    int pin[2], pout[2];
    if(pipe(pin)||pipe(pout)) return 0;
    g_eng_pid = fork();
    if(g_eng_pid==0){
        /* child */
        dup2(pin[0],STDIN_FILENO);
        dup2(pout[1],STDOUT_FILENO);
        close(pin[0]); close(pin[1]);
        close(pout[0]); close(pout[1]);
        execl(path,path,(char*)NULL);
        exit(1);
    }
    close(pin[0]); close(pout[1]);
    g_eng_in  = pin[1];
    g_eng_out = pout[0];
    /* set non-blocking */
    fcntl(g_eng_out, F_SETFL, O_NONBLOCK);
    /* handshake */
    eng_write("uci");
    char buf[ENGINE_BUF];
    int ok=0;
    for(int i=0;i<50;i++){
        if(eng_readline(buf,ENGINE_BUF,100)){
            if(strncmp(buf,"uciok",5)==0){ok=1;break;}
        }
    }
    if(!ok) return 0;
    eng_write("isready");
    for(int i=0;i<50;i++){
        if(eng_readline(buf,ENGINE_BUF,100)){
            if(strncmp(buf,"readyok",7)==0) break;
        }
    }
    eng_write("ucinewgame");
    return 1;
}

static void engine_stop(void){
    if(g_eng_in<0) return;
    eng_write("stop");
    eng_write("quit");
    close(g_eng_in); close(g_eng_out);
    g_eng_in=g_eng_out=-1;
    g_eng_pid=-1;
}

/* Build position string from history */
static void engine_send_position(void){
    char cmd[8192];
    int pos = sprintf(cmd,"position startpos");
    if(g_hist_len>0){
        pos += sprintf(cmd+pos," moves");
        char uci[8];
        for(int i=0;i<g_hist_len;i++){
            move_to_uci(&g_hist[i].move, uci);
            pos += sprintf(cmd+pos," %s",uci);
        }
    }
    eng_write(cmd);
}

static void engine_go(void){
    char cmd[256];
    int pos = sprintf(cmd,"go");
    if(g_eng_depth>0) pos += sprintf(cmd+pos," depth %d",g_eng_depth);
    if(g_eng_nodes>0) pos += sprintf(cmd+pos," nodes %d",g_eng_nodes);
    /* always add movetime */
    pos += sprintf(cmd+pos," movetime %d",g_eng_time);
    eng_write(cmd);
    g_engine_thinking=1;
    g_eng_best[0]='\0';
}

/* Poll engine for bestmove – non-blocking */
static int engine_poll(void){
    if(!g_engine_thinking || g_eng_out<0) return 0;
    char buf[ENGINE_BUF];
    int got=0;
    while(eng_readline(buf,ENGINE_BUF,0)){
        if(strncmp(buf,"bestmove",8)==0){
            /* parse: bestmove e2e4 */
            char *p=buf+9;
            int i=0;
            while(*p && *p!=' ' && i<15) g_eng_best[i++]=*p++;
            g_eng_best[i]='\0';
            g_engine_thinking=0;
            got=1;
        }
    }
    return got;
}

/* Parse UCI move string into Move on current board */
static int uci_to_move(const Board *b, const char *uci, Move *out){
    if(strlen(uci)<4) return 0;
    int ff=uci[0]-'a', fr=uci[1]-'1';
    int tf=uci[2]-'a', tr=uci[3]-'1';
    if(ff<0||ff>7||fr<0||fr>7||tf<0||tf>7||tr<0||tr>7) return 0;
    Square from=(Square)sq(ff,fr), to=(Square)sq(tf,tr);
    Piece promo=0;
    if(uci[4]){
        switch(tolower((unsigned char)uci[4])){
            case 'q': promo=QUEEN;  break;
            case 'r': promo=ROOK;   break;
            case 'b': promo=BISHOP; break;
            case 'n': promo=KNIGHT; break;
        }
    }
    MoveList ml; ml.n=0;
    gen_legal(&ml, b);
    for(int i=0;i<ml.n;i++){
        Move *m=&ml.m[i];
        if(m->from==from && m->to==to &&
           (!promo || piece_type(m->promo)==promo)){
            *out=*m; return 1;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 7 – Terminal raw mode
   ═══════════════════════════════════════════════════════════════════ */

static void term_raw(void){
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_old_term);
    raw = g_old_term;
    raw.c_lflag &= ~(ECHO|ICANON|ISIG);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void term_restore(void){
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old_term);
}

/* read key, returns special codes */
#define KEY_UP     256
#define KEY_DOWN   257
#define KEY_LEFT   258
#define KEY_RIGHT  259
#define KEY_ENTER  260
#define KEY_ESC    261
#define KEY_NONE   -1

static int read_key(void){
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO,&fds);
    struct timeval tv={0,50000}; /* 50ms */
    int r=select(STDIN_FILENO+1,&fds,NULL,NULL,&tv);
    if(r<=0) return KEY_NONE;
    unsigned char c; read(STDIN_FILENO,&c,1);
    if(c=='\r'||c=='\n') return KEY_ENTER;
    if(c==27){
        struct timeval tv2={0,10000};
        FD_ZERO(&fds); FD_SET(STDIN_FILENO,&fds);
        if(select(STDIN_FILENO+1,&fds,NULL,NULL,&tv2)<=0) return KEY_ESC;
        unsigned char seq[3]={0};
        read(STDIN_FILENO,seq,1);
        if(seq[0]=='['){
            read(STDIN_FILENO,seq+1,1);
            switch(seq[1]){
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return KEY_ESC;
    }
    return (int)(unsigned char)c;
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 8 – Rendering
   ═══════════════════════════════════════════════════════════════════ */

/* We'll hide cursor during rendering to avoid flicker */
#define HIDE_CURSOR "\033[?25l"
#define SHOW_CURSOR "\033[?25h"
#define CLEAR_SCREEN "\033[2J\033[H"
#define MOVE_TO(r,c) printf("\033[%d;%Hf",r,c)

static int is_legal_dest(int s){
    for(int i=0;i<g_legal_cnt;i++)
        if(g_legal_buf[i]==s) return 1;
    return 0;
}

static int last_from(void){
    return g_hist_len>0 ? (int)g_hist[g_hist_len-1].move.from : -1;
}
static int last_to(void){
    return g_hist_len>0 ? (int)g_hist[g_hist_len-1].move.to : -1;
}

/* Draw board – viewing from white's perspective by default */
static void draw_board(void){
    /* Compute king check square */
    int check_sq = -1;
    if(in_check(&g_board, g_board.side)){
        Piece king = g_board.side==WHITE ? KING : -KING;
        for(int s=0;s<64;s++) if(g_board.sq[s]==king){check_sq=s;break;}
    }

    printf(HIDE_CURSOR);
    printf(CLEAR_SCREEN);

    /* Title */
    printf(ANSI_BOLD "  ╔══════════════════════════════════════╗\n");
    printf("  ║       TERMINAL CHESS  (UCI)          ║\n");
    printf("  ╚══════════════════════════════════════╝" ANSI_RESET "\n\n");

    /* File labels */
    printf("     a   b   c   d   e   f   g   h\n");
    printf("   ┌───┬───┬───┬───┬───┬───┬───┬───┐\n");

    for(int r=7;r>=0;r--){
        printf(" %d │", r+1);
        for(int f=0;f<8;f++){
            int s = sq(f,r);
            int light = (f+r)%2 == 1;
            Piece p   = g_board.sq[s];

            /* choose background */
            const char *bg;
            if(s==g_cursor_sq && g_sel_sq<0)        bg = BG_CURSOR;
            else if(s==g_sel_sq)                     bg = BG_SEL;
            else if(g_sel_sq>=0 && is_legal_dest(s)) bg = BG_LEGAL;
            else if(s==check_sq)                     bg = BG_CHECK;
            else if(s==last_from()||s==last_to())    bg = BG_LASTMOVE;
            else                                     bg = light?BG_LIGHT:BG_DARK;

            /* cursor on top of everything when piece selected */
            if(s==g_cursor_sq && g_sel_sq>=0)        bg = BG_CURSOR;

            const char *fg = (p>0) ? FG_WHITE_PIECE : FG_BLACK_PIECE;
            const char *ps = piece_str(p);
            printf("%s%s %s " ANSI_RESET "│", bg, fg, ps);
        }
        printf(" %d\n", r+1);
        if(r>0) printf("   ├───┼───┼───┼───┼───┼───┼───┼───┤\n");
    }
    printf("   └───┴───┴───┴───┴───┴───┴───┴───┘\n");
    printf("     a   b   c   d   e   f   g   h\n\n");
}

/* PGN display – wrap at 60 chars */
static void draw_pgn(void){
    printf(ANSI_BOLD "  Moves:\n" ANSI_RESET);
    const char *p = g_pgn;
    /* skip headers */
    while(*p){ if(*p=='\n'&&*(p+1)=='\n'){p+=2;break;} p++; }
    int col=2; printf("  ");
    while(*p){
        if(*p=='\n'){p++;continue;}
        char word[64]; int wl=0;
        const char *s=p;
        while(*s&&*s!=' '&&*s!='\n') s++;
        wl=(int)(s-p);
        if(col+wl+1>62){printf("\n  ");col=2;}
        fwrite(p,1,wl,stdout); p=s;
        if(*p==' '){putchar(' ');col++;p++;}
        col+=wl;
    }
    printf("\n");
}

static void draw_status(void){
    printf("\n" ANSI_BOLD "  Status: " ANSI_RESET "%s\n", g_status);
}

static void draw_controls(void){
    printf("\n  " ANSI_BOLD "Controls:" ANSI_RESET
           " Arrows=move cursor  Enter=select/move  U=undo\n"
           "  Q=quit  N=new game  E=engine settings"
           "  P=flip  F=load FEN\n"
           "  Engine: %s  Depth:%d  Time:%dms  Nodes:%d\n",
           g_engine_enabled ? g_eng_path : "(disabled)",
           g_eng_depth, g_eng_time, g_eng_nodes);
}

static void draw_all(void){
    draw_board();
    draw_pgn();
    draw_status();
    draw_controls();
    printf(SHOW_CURSOR);
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 9 – Game logic helpers
   ═══════════════════════════════════════════════════════════════════ */

static void compute_legal_for_sel(void){
    g_legal_cnt=0;
    if(g_sel_sq<0) return;
    MoveList ml; ml.n=0;
    gen_legal(&ml, &g_board);
    for(int i=0;i<ml.n;i++){
        if(ml.m[i].from==(Square)g_sel_sq)
            g_legal_buf[g_legal_cnt++]=ml.m[i].to;
    }
}

/* Execute a move: record history, update board, update PGN */
static void do_move(const Move *m){
    if(g_hist_len>=MAX_HIST) return;
    HistEntry *h = &g_hist[g_hist_len];
    h->board_before = g_board;
    h->move         = *m;
    /* SAN before applying */
    move_to_san(&g_board, m, h->san);
    /* PGN entry */
    pgn_append_move((int)g_board.fullmove, g_board.side, h->san);
    /* apply */
    apply_move(&g_board, m);
    g_hist_len++;
}

static void do_undo(void){
    if(g_hist_len<=0){ snprintf(g_status,256,"Nothing to undo."); return; }
    /* If engine is thinking, stop it */
    if(g_engine_thinking){ eng_write("stop"); g_engine_thinking=0; }
    /* If engine just moved, undo two plies so human gets to move */
    int undo_count = (g_engine_enabled && g_hist_len>1) ? 2 : 1;
    for(int i=0;i<undo_count&&g_hist_len>0;i++){
        g_hist_len--;
        undo_move(&g_board, &g_hist[g_hist_len].move,
                             &g_hist[g_hist_len].board_before);
    }
    pgn_rebuild();
    g_sel_sq=-1; g_legal_cnt=0;
    snprintf(g_status,256,"Move taken back.");
}

static void new_game(void){
    if(g_engine_thinking){ eng_write("stop"); g_engine_thinking=0; }
    board_start(&g_board);
    g_hist_len=0;
    g_sel_sq=-1; g_legal_cnt=0;
    pgn_reset();
    snprintf(g_status,256,"New game started. You play %s.",
             g_human_side==WHITE?"White":"Black");
}

/* ─── Promotion selection ──────────────────────────────────────────── */
static int prompt_promotion(void){
    /* Simple: just ask in status + read key */
    printf("\033[2J\033[H");
    printf("  Choose promotion: Q=Queen  R=Rook  B=Bishop  N=Knight\n");
    fflush(stdout);
    while(1){
        int k=read_key();
        if(k=='q'||k=='Q') return QUEEN;
        if(k=='r'||k=='R') return ROOK;
        if(k=='b'||k=='B') return BISHOP;
        if(k=='n'||k=='N') return KNIGHT;
    }
}

/* Try to make human's move from sel_sq -> cursor_sq */
static void try_move(void){
    if(g_sel_sq<0) return;
    if(g_board.sq[g_sel_sq]==EMPTY ||
       piece_color(g_board.sq[g_sel_sq])!=g_human_side){
        g_sel_sq=-1; g_legal_cnt=0; return;
    }

    Square from=(Square)g_sel_sq, to=(Square)g_cursor_sq;
    MoveList ml; ml.n=0;
    gen_legal(&ml, &g_board);

    /* collect matching moves */
    Move candidates[8]; int nc=0;
    for(int i=0;i<ml.n;i++){
        if(ml.m[i].from==from && ml.m[i].to==to)
            candidates[nc++]=ml.m[i];
    }
    if(nc==0){
        snprintf(g_status,256,"Illegal move.");
        g_sel_sq=-1; g_legal_cnt=0;
        return;
    }
    Move chosen = candidates[0];
    /* if promotion, pick */
    if(nc>1){
        /* multiple promotions */
        int pt = prompt_promotion();
        for(int i=0;i<nc;i++)
            if(piece_type(candidates[i].promo)==pt){ chosen=candidates[i]; break; }
    } else if(chosen.promo==0 && piece_type(g_board.sq[from])==PAWN){
        int pr=rank_of(to);
        if((g_board.side==WHITE&&pr==7)||(g_board.side==BLACK&&pr==0)){
            int pt=prompt_promotion();
            /* find matching */
            for(int i=0;i<ml.n;i++)
                if(ml.m[i].from==from&&ml.m[i].to==to&&
                   piece_type(ml.m[i].promo)==pt){chosen=ml.m[i];break;}
        }
    }

    char san_buf[16]; move_to_san(&g_board,&chosen,san_buf);
    do_move(&chosen);
    snprintf(g_status,256,"Played: %s", san_buf);

    g_sel_sq=-1; g_legal_cnt=0;

    /* check game over */
    if(is_checkmate(&g_board)){
        snprintf(g_status,256,"Checkmate! %s wins.",
                 g_board.side==WHITE?"Black":"White");
        return;
    }
    if(is_stalemate(&g_board)){
        snprintf(g_status,256,"Stalemate! Draw.");
        return;
    }
    if(in_check(&g_board,g_board.side))
        snprintf(g_status,256,"Check! %s to move.", g_board.side==WHITE?"White":"Black");

    /* trigger engine if enabled */
    if(g_engine_enabled && g_eng_in>=0 && g_board.side!=g_human_side){
        engine_send_position();
        engine_go();
        snprintf(g_status,256,"Played: %s | Engine thinking...", san_buf);
    }
}

/* ─── Engine settings prompt ───────────────────────────────────────── */
static void prompt_engine_settings(void){
    term_restore();
    printf("\033[2J\033[H");
    printf("=== Engine Settings ===\n");
    printf("Current engine: %s\n", g_eng_path[0]?g_eng_path:"(none)");
    printf("Enter engine path (blank to keep): ");
    fflush(stdout);
    char buf[512]; buf[0]='\0';
    if(fgets(buf,512,stdin)){
        buf[strcspn(buf,"\n")]='\0';
        if(buf[0]) strncpy(g_eng_path,buf,511);
    }
    printf("Max depth (%d): ", g_eng_depth);
    fflush(stdout);
    buf[0]='\0';
    if(fgets(buf,512,stdin)){
        int v=atoi(buf);
        if(v>0) g_eng_depth=v;
    }
    printf("Max nodes (0=unlimited, %d): ", g_eng_nodes);
    fflush(stdout);
    buf[0]='\0';
    if(fgets(buf,512,stdin)){
        int v=atoi(buf);
        if(v>=0) g_eng_nodes=v;
    }
    printf("Move time ms (%d): ", g_eng_time);
    fflush(stdout);
    buf[0]='\0';
    if(fgets(buf,512,stdin)){
        int v=atoi(buf);
        if(v>0) g_eng_time=v;
    }
    printf("Human plays (w/b, current=%c): ",
           g_human_side==WHITE?'w':'b');
    fflush(stdout);
    buf[0]='\0';
    if(fgets(buf,512,stdin)){
        if(buf[0]=='b') g_human_side=BLACK;
        else if(buf[0]=='w') g_human_side=WHITE;
    }
    /* (re)start engine */
    if(g_eng_in>=0) engine_stop();
    if(g_eng_path[0]){
        if(engine_start(g_eng_path)){
            g_engine_enabled=1;
            snprintf(g_status,256,"Engine loaded: %s", g_eng_path);
        } else {
            snprintf(g_status,256,"Failed to start engine: %s", g_eng_path);
            g_engine_enabled=0;
        }
    } else g_engine_enabled=0;
    term_raw();
    new_game();
}

/* ─── FEN prompt ───────────────────────────────────────────────────── */
static void prompt_fen(void){
    term_restore();
    printf("\033[2J\033[H");
    printf("Enter FEN string:\n");
    fflush(stdout);
    char buf[256]; buf[0]='\0';
    if(fgets(buf,256,stdin)){
        buf[strcspn(buf,"\n")]='\0';
        if(strlen(buf)>10){
            board_from_fen(&g_board, buf);
            g_hist_len=0;
            pgn_reset();
            g_sel_sq=-1; g_legal_cnt=0;
            snprintf(g_status,256,"FEN loaded.");
        }
    }
    term_raw();
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 10 – Main loop
   ═══════════════════════════════════════════════════════════════════ */

static void handle_key(int k){
    /* flip board perspective is stored but we always draw from white side
       in this implementation — P key toggles g_human_side perspective (future) */
    switch(k){
    case KEY_UP:
        if(rank_of(g_cursor_sq)<7) g_cursor_sq+=8;
        break;
    case KEY_DOWN:
        if(rank_of(g_cursor_sq)>0) g_cursor_sq-=8;
        break;
    case KEY_LEFT:
        if(file_of(g_cursor_sq)>0) g_cursor_sq--;
        break;
    case KEY_RIGHT:
        if(file_of(g_cursor_sq)<7) g_cursor_sq++;
        break;
    case KEY_ENTER: case ' ':
        if(g_engine_thinking) break;
        if(g_board.side != g_human_side) break;
        if(g_sel_sq < 0){
            /* select */
            Piece p = g_board.sq[g_cursor_sq];
            if(p!=EMPTY && piece_color(p)==g_human_side){
                g_sel_sq=g_cursor_sq;
                compute_legal_for_sel();
                if(g_legal_cnt==0){
                    snprintf(g_status,256,"No legal moves for that piece.");
                    g_sel_sq=-1;
                } else {
                    snprintf(g_status,256,"Selected %s at %c%d – %d legal moves",
                             piece_str(p),
                             'a'+file_of(g_sel_sq),
                             rank_of(g_sel_sq)+1,
                             g_legal_cnt);
                }
            } else {
                snprintf(g_status,256,"No piece to select there.");
            }
        } else {
            /* deselect if same square */
            if(g_cursor_sq==g_sel_sq){
                g_sel_sq=-1; g_legal_cnt=0;
                snprintf(g_status,256,"Deselected.");
            } else {
                try_move();
            }
        }
        break;
    case 'u': case 'U':
        do_undo();
        break;
    case 'n': case 'N':
        new_game();
        break;
    case 'e': case 'E':
        prompt_engine_settings();
        break;
    case 'f': case 'F':
        prompt_fen();
        break;
    case 'q': case 'Q':
        g_running=0;
        break;
    case KEY_ESC:
        g_sel_sq=-1; g_legal_cnt=0;
        snprintf(g_status,256,"Deselected.");
        break;
    default: break;
    }
}

int main(int argc, char *argv[]){
    /* Optional: engine path as argument */
    if(argc>=2){
        strncpy(g_eng_path, argv[1], 511);
    }

    /* Init */
    board_start(&g_board);
    pgn_reset();
    g_cursor_sq = sq(4,1); /* e2 as starting cursor */

    /* Start engine if provided */
    if(g_eng_path[0]){
        if(engine_start(g_eng_path)){
            g_engine_enabled=1;
            snprintf(g_status,256,"Engine loaded: %s | You play White.",
                     g_eng_path);
        } else {
            snprintf(g_status,256,"Engine failed to start. Playing without engine.");
        }
    }

    term_raw();

    /* If human plays black, engine moves first */
    if(g_engine_enabled && g_human_side==BLACK){
        engine_send_position();
        engine_go();
    }

    while(g_running){
        draw_all();

        /* Poll engine */
        if(g_engine_enabled && engine_poll()){
            /* got best move */
            Move em;
            if(uci_to_move(&g_board, g_eng_best, &em)){
                char san_buf[16];
                move_to_san(&g_board,&em,san_buf);
                do_move(&em);
                snprintf(g_status,256,"Engine played: %s",san_buf);
                if(is_checkmate(&g_board))
                    snprintf(g_status,256,"Checkmate! %s wins.",
                             g_board.side==WHITE?"Black":"White");
                else if(is_stalemate(&g_board))
                    snprintf(g_status,256,"Stalemate! Draw.");
                else if(in_check(&g_board,g_board.side))
                    snprintf(g_status+strlen(g_status),
                             256-strlen(g_status)," Check!");
            } else {
                snprintf(g_status,256,"Engine returned invalid move: %s",g_eng_best);
            }
        }

        int k = read_key();
        if(k != KEY_NONE) handle_key(k);
    }

    term_restore();
    printf(CLEAR_SCREEN);
    printf("Thanks for playing! Final PGN:\n\n%s\n",g_pgn);
    if(g_engine_enabled) engine_stop();
    return 0;
}
