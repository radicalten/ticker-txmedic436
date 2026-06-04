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
#include <stdint.h>
#include <time.h>
#include <sys/wait.h>

/* ─── Terminal dimensions ─────────────────────────────────────────────────── */
#define BOARD_ROWS     8
#define BOARD_COLS     8
#define CELL_W         5   /* chars per cell */
#define CELL_H         2   /* lines per cell */
#define MAX_MOVES      512
#define MAX_HISTORY    256
#define MAX_PGN_LEN    8192
#define MAX_PGNTOK     128

/* ─── ANSI helpers ────────────────────────────────────────────────────────── */
#define ESC "\033"
#define CSI "\033["
#define RESET       CSI "0m"
#define BOLD        CSI "1m"
#define CLEAR       CSI "2J"
#define HOME        CSI "H"
#define HIDE_CUR    CSI "?25l"
#define SHOW_CUR    CSI "?25h"
#define ALT_BUF     CSI "?1049h"
#define NORM_BUF    CSI "?1049l"

/* 256‑colour helpers */
#define BG(n)  CSI "48;5;" #n "m"
#define FG(n)  CSI "38;5;" #n "m"

/* True‑colour */
static void set_bg(int r,int g,int b){printf(CSI "48;2;%d;%d;%dm",r,g,b);}
static void set_fg(int r,int g,int b){printf(CSI "38;2;%d;%d;%dm",r,g,b);}
static void move_cur(int row,int col){printf(CSI "%d;%dH",row,col);}

/* ─── Piece definitions ───────────────────────────────────────────────────── */
/* Piece values: 0=empty, 1=P,2=N,3=B,4=R,5=Q,6=K  positive=white, negative=black */
typedef int8_t Piece;

static const char *PIECE_UNICODE[7] = {
    " ",
    "♟","♞","♝","♜","♛","♚"   /* black set (we flip colour via bg) */
};
static const char *PIECE_WHITE[7] = {
    " ",
    "♙","♘","♗","♖","♕","♔"
};

/* ─── Board & game state ──────────────────────────────────────────────────── */
typedef struct {
    Piece sq[64];          /* 0..63  a1=0 h8=63  rank*(8)+file */
    int   castling;        /* bits: 1=WK,2=WQ,4=BK,8=BQ */
    int   ep;              /* en‑passant target square, -1 if none */
    int   halfmove;
    int   fullmove;
    int   side;            /* 0=white,1=black */
} Board;

typedef struct {
    int   from, to;
    Piece captured;
    Piece promo;           /* 0 if not promotion */
    int   castling;
    int   ep;
    int   halfmove;
    char  san[16];         /* SAN notation */
} Move;

static Board      gBoard;
static Move       gHistory[MAX_HISTORY];
static int        gHistCount = 0;
static char       gPGN[MAX_PGN_LEN];

/* ─── Cursor / selection ──────────────────────────────────────────────────── */
static int  gCurSq   = 0;   /* 0..63 */
static int  gSelSq   = -1;  /* -1 = nothing selected */
static int  gLegal[64];     /* 1 if this square is a legal destination */
static int  gLastFrom = -1, gLastTo = -1;

/* ─── Engine pipe ─────────────────────────────────────────────────────────── */
static int  gEnginePID = 0;
static int  gEngineIn  = -1;   /* write to engine */
static int  gEngineOut = -1;   /* read from engine */
static int  gEngineActive = 0;
static int  gHumanSide = 0;    /* 0=white,1=black */

/* time control */
typedef enum { TC_DEPTH=0, TC_NODES, TC_TIME } TCMode;
static TCMode gTCMode   = TC_DEPTH;
static int    gTCValue  = 6;   /* default depth 6 */

/* ─── Terminal raw mode ───────────────────────────────────────────────────── */
static struct termios gOldTerm;

static void restore_term(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &gOldTerm);
    printf(SHOW_CUR NORM_BUF);
    fflush(stdout);
}

static void init_term(void) {
    tcgetattr(STDIN_FILENO, &gOldTerm);
    atexit(restore_term);
    struct termios raw = gOldTerm;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf(ALT_BUF HIDE_CUR CLEAR);
    fflush(stdout);
}

/* ─── Board helpers ───────────────────────────────────────────────────────── */
static inline int rank_of(int sq){return sq>>3;}
static inline int file_of(int sq){return sq&7;}
static inline int sq(int r,int f){return r*8+f;}
static inline int abs_piece(Piece p){return p<0?-p:p;}
static inline int piece_color(Piece p){return p>0?0:1;}/* 0=white,1=black */

static void board_start(Board *b) {
    memset(b,0,sizeof(*b));
    /* white pieces */
    static const Piece back[8]={4,2,3,5,6,3,2,4};
    for(int f=0;f<8;f++){
        b->sq[sq(0,f)]= back[f];
        b->sq[sq(1,f)]= 1;
        b->sq[sq(6,f)]=-1;
        b->sq[sq(7,f)]=-back[f];
    }
    b->castling=15; b->ep=-1; b->fullmove=1; b->side=0;
}

static void board_from_fen(Board *b, const char *fen) {
    memset(b,0,sizeof(*b));
    b->ep=-1;
    int r=7,f=0;
    const char *p=fen;
    static const char *pcs="PNBRQKpnbrqk";
    static const Piece vals[]={1,2,3,4,5,6,-1,-2,-3,-4,-5,-6};
    while(*p && *p!=' '){
        if(*p=='/'){r--;f=0;}
        else if(*p>='1'&&*p<='8'){f+=*p-'0';}
        else{
            const char *idx=strchr(pcs,*p);
            if(idx) b->sq[sq(r,f++)]=vals[idx-pcs];
        }
        p++;
    }
    if(*p==' ') p++;
    b->side=(*p=='b')?1:0; p++;
    if(*p==' ') p++;
    b->castling=0;
    while(*p && *p!=' '){
        if(*p=='K') b->castling|=1;
        if(*p=='Q') b->castling|=2;
        if(*p=='k') b->castling|=4;
        if(*p=='q') b->castling|=8;
        p++;
    }
    if(*p==' ') p++;
    if(*p!='-'){
        int ef=*p-'a'; p++;
        int er=*p-'1';
        b->ep=sq(er,ef);
    } else p++;
    if(*p==' ') p++;
    b->halfmove=atoi(p);
    while(*p && *p!=' ') p++;
    if(*p==' ') p++;
    b->fullmove=atoi(p);
}

static void board_to_fen(const Board *b, char *buf, size_t sz) {
    char *out=buf; size_t rem=sz;
    for(int r=7;r>=0;r--){
        int empty=0;
        for(int f=0;f<8;f++){
            Piece p=b->sq[sq(r,f)];
            if(!p){empty++;}
            else{
                if(empty){*out++=(char)('0'+empty);rem--;empty=0;}
                static const char wpc[]=" PNBRQK";
                static const char bpc[]=" pnbrqk";
                *out++=(p>0)?wpc[p]:bpc[-p]; rem--;
            }
        }
        if(empty){*out++=(char)('0'+empty);rem--;}
        if(r>0){*out++='/';rem--;}
    }
    snprintf(out,rem," %c %s%s%s%s %s %d %d",
        b->side?'b':'w',
        (b->castling&1)?"K":"",
        (b->castling&2)?"Q":"",
        (b->castling&4)?"k":"",
        (b->castling&8)?"q":"",
        (b->castling)?"":"-",
        b->ep>=0?(char)('a'+file_of(b->ep)):'-',
        b->halfmove,b->fullmove);
    /* ep rank */
    (void)rem;
}

/* ─── Attack / move generation ────────────────────────────────────────────── */
static int in_bounds(int r,int f){return r>=0&&r<8&&f>=0&&f<8;}

static int is_attacked(const Board *b,int s,int by_side){
    int r=rank_of(s),f=file_of(s);
    int dir=by_side?-1:1;
    /* pawn */
    if(in_bounds(r-dir,f-1)&&b->sq[sq(r-dir,f-1)]==(by_side?-1:1)) return 1;
    if(in_bounds(r-dir,f+1)&&b->sq[sq(r-dir,f+1)]==(by_side?-1:1)) return 1;
    /* knight */
    static const int kn[][2]={{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for(int i=0;i<8;i++){
        int nr=r+kn[i][0],nf=f+kn[i][1];
        if(in_bounds(nr,nf)&&b->sq[sq(nr,nf)]==(by_side?-2:2)) return 1;
    }
    /* bishop/queen */
    static const int dd[][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
    for(int i=0;i<4;i++){
        for(int d=1;d<8;d++){
            int nr=r+dd[i][0]*d,nf=f+dd[i][1]*d;
            if(!in_bounds(nr,nf)) break;
            Piece p=b->sq[sq(nr,nf)];
            if(p){
                if(by_side?p<0:p>0){
                    int ap=abs_piece(p);
                    if(ap==3||ap==5) return 1;
                }
                break;
            }
        }
    }
    /* rook/queen */
    static const int dr[][2]={{1,0},{-1,0},{0,1},{0,-1}};
    for(int i=0;i<4;i++){
        for(int d=1;d<8;d++){
            int nr=r+dr[i][0]*d,nf=f+dr[i][1]*d;
            if(!in_bounds(nr,nf)) break;
            Piece p=b->sq[sq(nr,nf)];
            if(p){
                if(by_side?p<0:p>0){
                    int ap=abs_piece(p);
                    if(ap==4||ap==5) return 1;
                }
                break;
            }
        }
    }
    /* king */
    for(int dr2=-1;dr2<=1;dr2++) for(int df=-1;df<=1;df++){
        if(!dr2&&!df) continue;
        int nr=r+dr2,nf=f+df;
        if(in_bounds(nr,nf)&&b->sq[sq(nr,nf)]==(by_side?-6:6)) return 1;
    }
    return 0;
}

static int find_king(const Board *b,int side){
    Piece k=side?-6:6;
    for(int i=0;i<64;i++) if(b->sq[i]==k) return i;
    return -1;
}

static int in_check(const Board *b,int side){
    int ks=find_king(b,side);
    if(ks<0) return 0;
    return is_attacked(b,ks,!side);
}

/* Apply a pseudo‑legal move and return 1 if legal (king not in check) */
typedef struct {
    Board b;
    int   ep_cap;   /* square of captured EP pawn */
    int   rook_from,rook_to; /* for castling */
    int   is_castle;
} ApplyResult;

static void apply_move(const Board *src, Move *m, Board *dst) {
    *dst=*src;
    int side=src->side;
    Piece mover=dst->sq[m->from];
    int ap=abs_piece(mover);

    dst->sq[m->to]=m->promo ? (side?-(m->promo):(m->promo)) : mover;
    dst->sq[m->from]=0;
    dst->ep=-1;
    dst->halfmove++;

    /* en passant capture */
    if(ap==1 && m->to==src->ep){
        int cap_sq=sq(rank_of(m->to)+(side?1:-1),file_of(m->to));
        dst->sq[cap_sq]=0;
    }
    /* double push → set ep */
    if(ap==1 && abs(rank_of(m->to)-rank_of(m->from))==2){
        dst->ep=sq((rank_of(m->from)+rank_of(m->to))/2,file_of(m->from));
    }
    /* castling */
    if(ap==6){
        int df=file_of(m->to)-file_of(m->from);
        if(df==2){  /* kingside */
            int rf=sq(rank_of(m->from),7);
            dst->sq[sq(rank_of(m->from),5)]=dst->sq[rf];
            dst->sq[rf]=0;
        } else if(df==-2){ /* queenside */
            int rf=sq(rank_of(m->from),0);
            dst->sq[sq(rank_of(m->from),3)]=dst->sq[rf];
            dst->sq[rf]=0;
        }
    }
    /* update castling rights */
    if(ap==6){
        if(!side){dst->castling&=~3;}
        else{dst->castling&=~12;}
    }
    if(m->from==0||m->to==0) dst->castling&=~2;
    if(m->from==7||m->to==7) dst->castling&=~1;
    if(m->from==56||m->to==56) dst->castling&=~8;
    if(m->from==63||m->to==63) dst->castling&=~4;

    if(ap==1||m->captured) dst->halfmove=0;

    dst->side=!side;
    if(side==1) dst->fullmove++;
}

/* Generate all pseudo‑legal moves from square `from` */
static int gen_moves_from(const Board *b, int from, Move *moves) {
    int n=0;
    Piece mover=b->sq[from];
    if(!mover) return 0;
    int side=piece_color(mover);
    if(side!=b->side) return 0;
    int ap=abs_piece(mover);
    int r=rank_of(from),f=file_of(from);

    /* helper */
#define ADD(t,pr) do{ \
    Piece cap=b->sq[t]; \
    if(cap && piece_color(cap)==side) break; \
    moves[n].from=from; moves[n].to=(t); \
    moves[n].captured=cap; moves[n].promo=(pr); \
    moves[n].castling=b->castling; moves[n].ep=b->ep; \
    moves[n].halfmove=b->halfmove; \
    Board tmp; apply_move(b,&moves[n],&tmp); \
    if(!in_check(&tmp,side)) n++; \
}while(0)

    if(ap==1){ /* pawn */
        int dir=side?-1:1;
        int start_rank=side?6:1;
        int promo_rank=side?0:7;
        /* push */
        if(in_bounds(r+dir,f)&&!b->sq[sq(r+dir,f)]){
            if(r+dir==promo_rank){
                for(int pr=2;pr<=5;pr++){
                    moves[n].from=from;moves[n].to=sq(r+dir,f);
                    moves[n].captured=0;moves[n].promo=pr;
                    moves[n].castling=b->castling;moves[n].ep=b->ep;
                    moves[n].halfmove=b->halfmove;
                    Board tmp;apply_move(b,&moves[n],&tmp);
                    if(!in_check(&tmp,side))n++;
                }
            } else {ADD(sq(r+dir,f),0);}
            /* double */
            if(r==start_rank&&!b->sq[sq(r+2*dir,f)]){
                ADD(sq(r+2*dir,f),0);
            }
        }
        /* captures */
        for(int df2=-1;df2<=1;df2+=2){
            if(!in_bounds(r+dir,f+df2)) continue;
            int ts=sq(r+dir,f+df2);
            Piece cap=b->sq[ts];
            int is_ep=(ts==b->ep);
            if((!cap&&!is_ep)||(cap&&piece_color(cap)==side)) continue;
            if(is_ep) cap=(side?1:-1);
            if(r+dir==promo_rank){
                for(int pr=2;pr<=5;pr++){
                    moves[n].from=from;moves[n].to=ts;
                    moves[n].captured=cap;moves[n].promo=pr;
                    moves[n].castling=b->castling;moves[n].ep=b->ep;
                    moves[n].halfmove=b->halfmove;
                    Board tmp;apply_move(b,&moves[n],&tmp);
                    if(!in_check(&tmp,side))n++;
                }
            } else {
                moves[n].from=from;moves[n].to=ts;
                moves[n].captured=cap;moves[n].promo=0;
                moves[n].castling=b->castling;moves[n].ep=b->ep;
                moves[n].halfmove=b->halfmove;
                Board tmp;apply_move(b,&moves[n],&tmp);
                if(!in_check(&tmp,side))n++;
            }
        }
    } else if(ap==2){ /* knight */
        static const int kn[][2]={{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for(int i=0;i<8;i++){
            int nr=r+kn[i][0],nf=f+kn[i][1];
            if(!in_bounds(nr,nf)) continue;
            ADD(sq(nr,nf),0);
        }
    } else if(ap==3||ap==5){ /* bishop or queen */
        static const int dd[][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
        for(int i=0;i<4;i++){
            for(int d=1;d<8;d++){
                int nr=r+dd[i][0]*d,nf=f+dd[i][1]*d;
                if(!in_bounds(nr,nf)) break;
                int ts=sq(nr,nf);
                Piece cap=b->sq[ts];
                if(cap&&piece_color(cap)==side) break;
                ADD(ts,0);
                if(cap) break;
            }
        }
        if(ap==3) goto done;
        goto rook_part;
    } else if(ap==4){ /* rook */
        rook_part:;
        static const int dr2[][2]={{1,0},{-1,0},{0,1},{0,-1}};
        for(int i=0;i<4;i++){
            for(int d=1;d<8;d++){
                int nr=r+dr2[i][0]*d,nf=f+dr2[i][1]*d;
                if(!in_bounds(nr,nf)) break;
                int ts=sq(nr,nf);
                Piece cap=b->sq[ts];
                if(cap&&piece_color(cap)==side) break;
                ADD(ts,0);
                if(cap) break;
            }
        }
    } else if(ap==6){ /* king */
        for(int dr3=-1;dr3<=1;dr3++) for(int df3=-1;df3<=1;df3++){
            if(!dr3&&!df3) continue;
            int nr=r+dr3,nf=f+df3;
            if(!in_bounds(nr,nf)) continue;
            ADD(sq(nr,nf),0);
        }
        /* castling */
        if(side==0&&from==4&&!(b->castling&3)==0){
            if((b->castling&1)&&!b->sq[5]&&!b->sq[6]
               &&!is_attacked(b,4,1)&&!is_attacked(b,5,1)&&!is_attacked(b,6,1)){
                ADD(6,0);
            }
            if((b->castling&2)&&!b->sq[3]&&!b->sq[2]&&!b->sq[1]
               &&!is_attacked(b,4,1)&&!is_attacked(b,3,1)&&!is_attacked(b,2,1)){
                ADD(2,0);
            }
        }
        if(side==1&&from==60){
            if((b->castling&4)&&!b->sq[61]&&!b->sq[62]
               &&!is_attacked(b,60,0)&&!is_attacked(b,61,0)&&!is_attacked(b,62,0)){
                ADD(62,0);
            }
            if((b->castling&8)&&!b->sq[59]&&!b->sq[58]&&!b->sq[57]
               &&!is_attacked(b,60,0)&&!is_attacked(b,59,0)&&!is_attacked(b,58,0)){
                ADD(58,0);
            }
        }
    }
    done:;
    return n;
}

/* Generate all legal moves for current side */
static int gen_all_moves(const Board *b, Move *moves) {
    int total=0;
    for(int s=0;s<64;s++){
        Piece p=b->sq[s];
        if(!p||piece_color(p)!=b->side) continue;
        total+=gen_moves_from(b,s,moves+total);
    }
    return total;
}

/* ─── SAN generation ──────────────────────────────────────────────────────── */
static void move_to_san(const Board *b, Move *m, char *out) {
    int ap=abs_piece(b->sq[m->from]);
    int side=b->side;
    /* castling */
    if(ap==6&&abs(file_of(m->to)-file_of(m->from))==2){
        strcpy(out,file_of(m->to)==6?"O-O":"O-O-O");
        Board tmp; apply_move(b,m,&tmp);
        if(in_check(&tmp,!side)) strcat(out,"+");
        return;
    }
    char buf[16]; int bi=0;
    static const char pnames[]=" PNBRQK";
    if(ap!=1) buf[bi++]=pnames[ap];
    /* disambiguation */
    if(ap!=1){
        Move all[MAX_MOVES]; int cnt=gen_all_moves(b,all);
        int ambig_file=0,ambig_rank=0,ambig=0;
        for(int i=0;i<cnt;i++){
            if(all[i].to==m->to&&all[i].from!=m->from&&abs_piece(b->sq[all[i].from])==ap){
                ambig++;
                if(file_of(all[i].from)==file_of(m->from)) ambig_file=1;
                if(rank_of(all[i].from)==rank_of(m->from)) ambig_rank=1;
            }
        }
        if(ambig){
            if(!ambig_file) buf[bi++]='a'+file_of(m->from);
            else if(!ambig_rank) buf[bi++]='1'+rank_of(m->from);
            else{buf[bi++]='a'+file_of(m->from);buf[bi++]='1'+rank_of(m->from);}
        }
    }
    if(ap==1&&m->captured){buf[bi++]='a'+file_of(m->from);}
    if(m->captured) buf[bi++]='x';
    buf[bi++]='a'+file_of(m->to);
    buf[bi++]='1'+rank_of(m->to);
    if(m->promo){
        static const char pp[]=" PNBRQK";
        buf[bi++]='='; buf[bi++]=pp[m->promo];
    }
    buf[bi]='\0';
    Board tmp; apply_move(b,m,&tmp);
    Move resp[MAX_MOVES];
    int rcnt=gen_all_moves(&tmp,resp);
    if(in_check(&tmp,!side)){
        if(rcnt==0) strcat(buf,"#"); else strcat(buf,"+");
    }
    strcpy(out,buf);
}

/* ─── PGN update ──────────────────────────────────────────────────────────── */
static void rebuild_pgn(void) {
    gPGN[0]='\0';
    for(int i=0;i<gHistCount;i++){
        char tmp[64];
        if(i%2==0){
            char n[8]; snprintf(n,sizeof(n),"%d. ",i/2+1);
            strncat(gPGN,n,MAX_PGN_LEN-strlen(gPGN)-1);
        }
        snprintf(tmp,sizeof(tmp),"%s ",gHistory[i].san);
        strncat(gPGN,tmp,MAX_PGN_LEN-strlen(gPGN)-1);
    }
}

/* ─── Legal destination highlighting ─────────────────────────────────────── */
static void compute_legal(void) {
    memset(gLegal,0,sizeof(gLegal));
    if(gSelSq<0) return;
    Move moves[MAX_MOVES];
    int n=gen_moves_from(&gBoard,gSelSq,moves);
    for(int i=0;i<n;i++) gLegal[moves[i].to]=1;
}

/* ─── Move string helpers ─────────────────────────────────────────────────── */
static void sq_to_alg(int s,char *buf){buf[0]='a'+file_of(s);buf[1]='1'+rank_of(s);buf[2]='\0';}

static int alg_to_sq(const char *s){
    if(!s||strlen(s)<2) return -1;
    int f=s[0]-'a', r=s[1]-'1';
    if(f<0||f>7||r<0||r>7) return -1;
    return sq(r,f);
}

static void move_to_uci(const Move *m, char *buf) {
    sq_to_alg(m->from,buf);
    sq_to_alg(m->to,buf+2);
    if(m->promo){
        static const char pp[]=" pnbrqk";
        buf[4]=pp[m->promo]; buf[5]='\0';
    } else buf[4]='\0';
}

/* ─── Execute a move on gBoard ────────────────────────────────────────────── */
static int do_move(Move *m) {
    if(gHistCount>=MAX_HISTORY) return 0;
    move_to_san(&gBoard,m,gHistory[gHistCount].san);
    gHistory[gHistCount]=*m;
    gHistory[gHistCount].san[0]=m->san[0]; /* already set above... copy san properly */
    /* re-copy san */
    char san[16]; move_to_san(&gBoard,m,san);
    memcpy(gHistory[gHistCount].san,san,16);
    gLastFrom=m->from; gLastTo=m->to;
    apply_move(&gBoard,m,&gBoard);
    gHistCount++;
    gSelSq=-1;
    memset(gLegal,0,sizeof(gLegal));
    rebuild_pgn();
    return 1;
}

static void do_undo(void) {
    if(gHistCount==0) return;
    /* rebuild board from start */
    board_start(&gBoard);
    gHistCount--;
    int saved=gHistCount;
    gHistCount=0;
    /* replay */
    Board replay; board_start(&replay);
    for(int i=0;i<saved;i++){
        Move *m=&gHistory[i];
        Board tmp; apply_move(&replay,m,&tmp); replay=tmp;
    }
    gBoard=replay;
    gHistCount=saved;
    if(gHistCount>0){
        gLastFrom=gHistory[gHistCount-1].from;
        gLastTo=gHistory[gHistCount-1].to;
    } else {gLastFrom=-1;gLastTo=-1;}
    gSelSq=-1;
    memset(gLegal,0,sizeof(gLegal));
    rebuild_pgn();
}

/* ─── Engine communication ────────────────────────────────────────────────── */
static void engine_send(const char *cmd) {
    if(gEngineIn<0) return;
    write(gEngineIn,cmd,strlen(cmd));
    write(gEngineIn,"\n",1);
}

static int engine_readline(char *buf, int sz, int timeout_ms) {
    if(gEngineOut<0) return -1;
    struct timeval tv;
    tv.tv_sec=timeout_ms/1000;
    tv.tv_usec=(timeout_ms%1000)*1000;
    fd_set fds; FD_ZERO(&fds); FD_SET(gEngineOut,&fds);
    if(select(gEngineOut+1,&fds,NULL,NULL,&tv)<=0) return -1;
    int n=(int)read(gEngineOut,buf,sz-1);
    if(n<=0) return -1;
    buf[n]='\0';
    /* strip newline */
    for(int i=0;i<n;i++) if(buf[i]=='\n'||buf[i]=='\r') buf[i]='\0';
    return n;
}

static int start_engine(const char *path) {
    int to_engine[2],from_engine[2];
    if(pipe(to_engine)<0||pipe(from_engine)<0) return 0;
    pid_t pid=fork();
    if(pid<0) return 0;
    if(pid==0){
        close(to_engine[1]); close(from_engine[0]);
        dup2(to_engine[0],STDIN_FILENO);
        dup2(from_engine[1],STDOUT_FILENO);
        dup2(from_engine[1],STDERR_FILENO);
        execl(path,path,(char*)NULL);
        exit(1);
    }
    close(to_engine[0]); close(from_engine[1]);
    gEngineIn=to_engine[1]; gEngineOut=from_engine[0];
    gEnginePID=pid;
    /* set non-blocking */
    fcntl(gEngineOut,F_SETFL,O_NONBLOCK);
    engine_send("uci");
    /* wait for uciok */
    char buf[1024];
    for(int i=0;i<50;i++){
        if(engine_readline(buf,sizeof(buf),200)>0)
            if(strstr(buf,"uciok")) break;
    }
    engine_send("isready");
    for(int i=0;i<50;i++){
        if(engine_readline(buf,sizeof(buf),200)>0)
            if(strstr(buf,"readyok")) break;
    }
    gEngineActive=1;
    return 1;
}

static void stop_engine(void) {
    if(!gEngineActive) return;
    engine_send("quit");
    usleep(100000);
    if(gEnginePID) kill(gEnginePID,SIGTERM);
    close(gEngineIn); close(gEngineOut);
    gEngineIn=gEngineOut=-1;
    gEngineActive=0;
}

static void engine_go(void) {
    if(!gEngineActive) return;
    /* build position string */
    char fen[128]; board_from_fen(&gBoard,"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    /* send start position + moves */
    char cmd[4096];
    strcpy(cmd,"position startpos moves");
    for(int i=0;i<gHistCount;i++){
        char mv[8]; move_to_uci(&gHistory[i],mv);
        strcat(cmd," "); strcat(cmd,mv);
    }
    engine_send(cmd);
    char go[64];
    if(gTCMode==TC_DEPTH)
        snprintf(go,sizeof(go),"go depth %d",gTCValue);
    else if(gTCMode==TC_NODES)
        snprintf(go,sizeof(go),"go nodes %d",gTCValue);
    else
        snprintf(go,sizeof(go),"go movetime %d",gTCValue);
    engine_send(go);
}

/* Read best move from engine (blocking up to 30s) */
static int engine_bestmove(char *mv_out) {
    char buf[1024];
    for(int i=0;i<600;i++){
        if(engine_readline(buf,sizeof(buf),50)>0){
            if(strncmp(buf,"bestmove",8)==0){
                char *sp=strchr(buf,' ');
                if(sp){
                    strncpy(mv_out,sp+1,8);
                    /* cut at space */
                    for(int j=0;mv_out[j];j++)
                        if(mv_out[j]==' '||mv_out[j]=='\n'){mv_out[j]='\0';break;}
                    return 1;
                }
            }
        }
        usleep(5000);
    }
    return 0;
}

/* Apply engine move string to gBoard */
static int apply_engine_move(const char *mv) {
    if(!mv||strlen(mv)<4) return 0;
    int from=alg_to_sq(mv);
    int to=alg_to_sq(mv+2);
    if(from<0||to<0) return 0;
    int promo=0;
    if(strlen(mv)>=5){
        char pc=tolower(mv[4]);
        if(pc=='n') promo=2;
        else if(pc=='b') promo=3;
        else if(pc=='r') promo=4;
        else if(pc=='q') promo=5;
    }
    /* find matching legal move */
    Move legal[MAX_MOVES];
    int n=gen_all_moves(&gBoard,legal);
    for(int i=0;i<n;i++){
        if(legal[i].from==from&&legal[i].to==to&&legal[i].promo==promo){
            return do_move(&legal[i]);
        }
    }
    return 0;
}

/* ─── Rendering ───────────────────────────────────────────────────────────── */
/* Colours */
#define COL_LIGHT_R 240
#define COL_LIGHT_G 217
#define COL_LIGHT_B 181

#define COL_DARK_R  181
#define COL_DARK_G  136
#define COL_DARK_B   99

#define COL_SEL_R   106
#define COL_SEL_G   168
#define COL_SEL_B    79

#define COL_LAST_R  205
#define COL_LAST_G  210
#define COL_LAST_B   71

#define COL_CUR_R    66
#define COL_CUR_G   144
#define COL_CUR_B   245

#define COL_DOT_R   20
#define COL_DOT_G   20
#define COL_DOT_B   20

/* board top‑left terminal position */
#define BRD_ROW  3
#define BRD_COL  5

static int sq_is_light(int s){return (rank_of(s)+file_of(s))%2==1;}

static void draw_square(int s, int cursor) {
    Piece p=gBoard.sq[s];
    int r=rank_of(s), f=file_of(s);
    /* compute display row/col (rank 7 at top) */
    int dr=BRD_ROW + (7-r)*CELL_H;
    int dc=BRD_COL + f*CELL_W;

    /* pick background */
    int br,bg,bb;
    if(s==gSelSq){br=COL_SEL_R;bg=COL_SEL_G;bb=COL_SEL_B;}
    else if(cursor){br=COL_CUR_R;bg=COL_CUR_G;bb=COL_CUR_B;}
    else if(s==gLastFrom||s==gLastTo){br=COL_LAST_R;bg=COL_LAST_G;bb=COL_LAST_B;}
    else if(sq_is_light(s)){br=COL_LIGHT_R;bg=COL_LIGHT_G;bb=COL_LIGHT_B;}
    else{br=COL_DARK_R;bg=COL_DARK_G;bb=COL_DARK_B;}

    /* Draw 2 rows × 5 cols */
    for(int row=0;row<CELL_H;row++){
        move_cur(dr+row,dc);
        set_bg(br,bg,bb);
        if(row==0){
            /* top row: just spaces except middle shows legal dot */
            if(gLegal[s] && !p){
                /* dot in center */
                printf("  ");
                set_fg(COL_DOT_R,COL_DOT_G,COL_DOT_B);
                printf("•");
                set_bg(br,bg,bb);
                printf("  ");
            } else {
                printf("     ");
            }
        } else {
            /* bottom row: piece char in center */
            printf(" ");
            if(p){
                int pc=piece_color(p);
                int ap2=abs_piece(p);
                /* ring for captures */
                if(gLegal[s] && p && piece_color(p)!=gBoard.side){
                    set_fg(COL_DOT_R,COL_DOT_G,COL_DOT_B);
                    printf("◌");
                    set_bg(br,bg,bb);
                    printf("\b");
                }
                /* piece colour */
                if(pc==0) set_fg(255,255,255);
                else       set_fg(20,20,20);
                const char *glyph=(pc==0)?PIECE_WHITE[ap2]:PIECE_UNICODE[ap2];
                printf("%s",glyph);
            } else {
                printf(" ");
            }
            printf("   ");
        }
    }
    printf(RESET);
}

static void draw_board(void) {
    /* rank labels */
    for(int r=0;r<8;r++){
        move_cur(BRD_ROW+(7-r)*CELL_H+1, BRD_COL-2);
        printf(CSI "37m" "%d" RESET,r+1);
    }
    /* file labels */
    for(int f=0;f<8;f++){
        move_cur(BRD_ROW+8*CELL_H+1, BRD_COL+f*CELL_W+2);
        printf(CSI "37m" "%c" RESET,'a'+f);
    }
    for(int s=0;s<64;s++){
        draw_square(s, s==gCurSq);
    }
}

/* ─── Info panel ──────────────────────────────────────────────────────────── */
#define INFO_COL (BRD_COL + 8*CELL_W + 4)

static void draw_info(void) {
    int row=BRD_ROW;
    int col=INFO_COL;
    /* clear info area */
    for(int i=0;i<30;i++){
        move_cur(row+i,col);
        printf("                                        ");
    }

    move_cur(row,col);
    printf(BOLD CSI "36m" "♔ Chess Terminal GUI" RESET);
    row+=2;

    move_cur(row++,col);
    printf(CSI "33m" "Turn: %s" RESET, gBoard.side==0?"White ♙":"Black ♟");

    /* check/mate status */
    int chk=in_check(&gBoard,gBoard.side);
    Move all2[MAX_MOVES]; int mcnt=gen_all_moves(&gBoard,all2);
    if(chk&&mcnt==0){move_cur(row++,col);printf(CSI "31;1m" "✗ CHECKMATE" RESET);}
    else if(!chk&&mcnt==0){move_cur(row++,col);printf(CSI "33m" "½ STALEMATE" RESET);}
    else if(chk){move_cur(row++,col);printf(CSI "31m" "! Check" RESET);}
    else row++;

    row++;
    move_cur(row++,col); printf(BOLD "Controls:" RESET);
    move_cur(row++,col); printf("  Arrow keys / hjkl - move cursor");
    move_cur(row++,col); printf("  Enter / Space      - select/move");
    move_cur(row++,col); printf("  u                  - undo");
    move_cur(row++,col); printf("  e                  - engine move");
    move_cur(row++,col); printf("  l                  - load engine");
    move_cur(row++,col); printf("  t                  - time control");
    move_cur(row++,col); printf("  n                  - new game");
    move_cur(row++,col); printf("  f                  - load FEN");
    move_cur(row++,col); printf("  q                  - quit");
    row++;

    /* TC info */
    const char *tcnames[]={"Depth","Nodes","Time(ms)"};
    move_cur(row++,col);
    printf(CSI "35m" "TC: %s = %d" RESET, tcnames[gTCMode], gTCValue);
    row++;

    /* engine status */
    move_cur(row++,col);
    if(gEngineActive)
        printf(CSI "32m" "Engine: connected (%s)" RESET, (gHumanSide==0)?"you=White":"you=Black");
    else
        printf(CSI "31m" "Engine: none" RESET);
    row++;

    /* PGN */
    move_cur(row++,col);
    printf(BOLD CSI "36m" "PGN:" RESET);
    /* wrap PGN at ~40 chars */
    int plen=(int)strlen(gPGN);
    int pi=0, line=0;
    while(pi<plen&&line<10){
        move_cur(row+line,col);
        char linebuf[45]; int li=0;
        while(li<40&&pi<plen&&gPGN[pi]!='\n') linebuf[li++]=gPGN[pi++];
        linebuf[li]='\0';
        printf("%.44s",linebuf);
        line++;
    }
}

static void draw_status(const char *msg) {
    move_cur(BRD_ROW+8*CELL_H+3,BRD_COL);
    printf(CSI "2K"); /* clear line */
    if(msg) printf(CSI "33m" "%s" RESET, msg);
}

static void full_redraw(void) {
    printf(CLEAR HOME);
    /* Title */
    move_cur(1,BRD_COL);
    printf(BOLD CSI "36m" "  ♜ Terminal Chess ♜" RESET);
    draw_board();
    draw_info();
    fflush(stdout);
}

/* ─── Input handling ──────────────────────────────────────────────────────── */
static int read_key(void) {
    unsigned char c;
    if(read(STDIN_FILENO,&c,1)!=1) return -1;
    if(c==27){
        unsigned char c2,c3;
        /* try to read escape sequence */
        fd_set fds; struct timeval tv={0,50000};
        FD_ZERO(&fds);FD_SET(STDIN_FILENO,&fds);
        if(select(STDIN_FILENO+1,&fds,NULL,NULL,&tv)>0){
            read(STDIN_FILENO,&c2,1);
            if(c2=='['){
                if(select(STDIN_FILENO+1,&fds,NULL,NULL,&tv)>0){
                    read(STDIN_FILENO,&c3,1);
                    if(c3=='A') return 1000; /* up */
                    if(c3=='B') return 1001; /* down */
                    if(c3=='C') return 1002; /* right */
                    if(c3=='D') return 1003; /* left */
                }
            }
        }
        return 27;
    }
    return c;
}

/* ─── Promotion dialog ────────────────────────────────────────────────────── */
static int promotion_dialog(void) {
    int row=BRD_ROW+8*CELL_H+3;
    move_cur(row,BRD_COL);
    printf(CSI "2K" CSI "33m" "Promote to: (q)ueen (r)ook (b)ishop (n)knight: " RESET);
    fflush(stdout);
    struct termios cooked=gOldTerm;
    tcsetattr(STDIN_FILENO,TCSANOW,&cooked);
    int c=getchar();
    struct termios raw=gOldTerm;
    raw.c_lflag&=~(ICANON|ECHO);
    raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&raw);
    if(c=='n') return 2;
    if(c=='b') return 3;
    if(c=='r') return 4;
    return 5; /* default queen */
}

/* ─── TC dialog ───────────────────────────────────────────────────────────── */
static void tc_dialog(void) {
    int row=BRD_ROW+8*CELL_H+3;
    /* Show current TC, cycle mode */
    move_cur(row,BRD_COL);
    printf(CSI "2K" CSI "33m" "TC mode: (d)epth (n)odes (t)ime: " RESET);
    fflush(stdout);
    struct termios cooked=gOldTerm; tcsetattr(STDIN_FILENO,TCSANOW,&cooked);
    int c=getchar();
    if(c=='d') gTCMode=TC_DEPTH;
    else if(c=='n') gTCMode=TC_NODES;
    else if(c=='t') gTCMode=TC_TIME;
    move_cur(row,BRD_COL);
    const char *tcn[]={"depth","nodes","movetime(ms)"};
    printf(CSI "2K" CSI "33m" "Enter %s value: " RESET, tcn[gTCMode]);
    fflush(stdout);
    char vbuf[32]; fgets(vbuf,sizeof(vbuf),stdin);
    gTCValue=atoi(vbuf);
    if(gTCValue<=0) gTCValue=(gTCMode==TC_TIME)?1000:6;
    struct termios raw=gOldTerm;
    raw.c_lflag&=~(ICANON|ECHO);
    raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&raw);
}

/* ─── Load engine dialog ──────────────────────────────────────────────────── */
static void load_engine_dialog(void) {
    int row=BRD_ROW+8*CELL_H+3;
    move_cur(row,BRD_COL);
    printf(CSI "2K" CSI "33m" "Engine path (e.g. /usr/local/bin/stockfish): " RESET);
    fflush(stdout);
    struct termios cooked=gOldTerm; tcsetattr(STDIN_FILENO,TCSANOW,&cooked);
    char path[256]; fgets(path,sizeof(path),stdin);
    /* strip newline */
    for(int i=0;path[i];i++) if(path[i]=='\n'){path[i]='\0';break;}
    if(gEngineActive) stop_engine();
    if(strlen(path)>0 && access(path,X_OK)==0){
        if(start_engine(path)){
            /* ask human colour */
            move_cur(row+1,BRD_COL);
            printf(CSI "33m" "Play as (w)hite or (b)lack? " RESET);
            fflush(stdout);
            int c=getchar();
            gHumanSide=(c=='b')?1:0;
            draw_status("Engine loaded!");
        } else draw_status("Failed to start engine.");
    } else draw_status("Invalid path or not executable.");
    struct termios raw=gOldTerm;
    raw.c_lflag&=~(ICANON|ECHO);
    raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&raw);
}

/* ─── FEN dialog ──────────────────────────────────────────────────────────── */
static void fen_dialog(void) {
    int row=BRD_ROW+8*CELL_H+3;
    move_cur(row,BRD_COL);
    printf(CSI "2K" CSI "33m" "FEN: " RESET);
    fflush(stdout);
    struct termios cooked=gOldTerm; tcsetattr(STDIN_FILENO,TCSANOW,&cooked);
    char fen[256]; fgets(fen,sizeof(fen),stdin);
    for(int i=0;fen[i];i++) if(fen[i]=='\n'){fen[i]='\0';break;}
    if(strlen(fen)>10){
        board_from_fen(&gBoard,fen);
        gHistCount=0; gSelSq=-1; gLastFrom=-1; gLastTo=-1;
        memset(gLegal,0,sizeof(gLegal));
        rebuild_pgn();
    }
    struct termios raw=gOldTerm;
    raw.c_lflag&=~(ICANON|ECHO);
    raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&raw);
}

/* ─── Handle user selecting/moving ───────────────────────────────────────── */
static void handle_select(void) {
    int s=gCurSq;
    Piece p=gBoard.sq[s];

    if(gSelSq<0){
        /* select piece */
        if(p && piece_color(p)==gBoard.side){
            gSelSq=s;
            compute_legal();
        }
    } else {
        /* try to move */
        if(s==gSelSq){ /* deselect */
            gSelSq=-1; memset(gLegal,0,sizeof(gLegal));
            return;
        }
        if(gLegal[s]){
            /* find the move */
            Move moves[MAX_MOVES];
            int n=gen_moves_from(&gBoard,gSelSq,moves);
            int promo=0;
            /* check if pawn reaches back rank */
            Piece mover=gBoard.sq[gSelSq];
            int ap2=abs_piece(mover);
            if(ap2==1){
                int side=gBoard.side;
                if((side==0&&rank_of(s)==7)||(side==1&&rank_of(s)==0))
                    promo=promotion_dialog();
            }
            for(int i=0;i<n;i++){
                if(moves[i].to==s && moves[i].promo==promo){
                    do_move(&moves[i]);
                    break;
                }
            }
        } else if(p && piece_color(p)==gBoard.side){
            /* reselect */
            gSelSq=s; compute_legal();
        } else {
            gSelSq=-1; memset(gLegal,0,sizeof(gLegal));
        }
    }
}

/* ─── Engine auto‑play loop (called after each human move) ───────────────── */
static void engine_think_and_move(void) {
    if(!gEngineActive) return;
    if(gBoard.side==gHumanSide) return;
    /* check not game over */
    Move all3[MAX_MOVES]; int mc=gen_all_moves(&gBoard,all3);
    if(mc==0) return;
    draw_status("Engine thinking...");
    fflush(stdout);
    engine_go();
    char mv[16];
    if(engine_bestmove(mv)){
        apply_engine_move(mv);
        full_redraw();
        char msg[64]; snprintf(msg,sizeof(msg),"Engine played: %s",mv);
        draw_status(msg);
    } else {
        draw_status("Engine timeout.");
    }
    fflush(stdout);
}

/* ─── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    init_term();
    board_start(&gBoard);
    gCurSq=0; /* a1 */
    full_redraw();

    for(;;){
        fflush(stdout);
        int k=read_key();
        if(k<0) continue;

        /* movement keys */
        int moved_cur=0;
        if(k==1000||k=='k'||k=='K'){ /* up = rank+ */
            if(rank_of(gCurSq)<7){gCurSq+=8;moved_cur=1;}
        } else if(k==1001||k=='j'||k=='J'){ /* down */
            if(rank_of(gCurSq)>0){gCurSq-=8;moved_cur=1;}
        } else if(k==1002||k=='l'){ /* right */
            if(file_of(gCurSq)<7){gCurSq+=1;moved_cur=1;}
        } else if(k==1003||k=='h'){ /* left */
            if(file_of(gCurSq)>0){gCurSq-=1;moved_cur=1;}
        } else if(k=='\r'||k=='\n'||k==' '){
            handle_select();
            /* after human move, let engine respond */
            engine_think_and_move();
        } else if(k=='u'||k=='U'){
            do_undo();
            /* if engine had just moved, undo twice */
            if(gEngineActive && gHistCount>0 && gBoard.side==gHumanSide){
                /* already undid engine move implicitly—check if we need another */
            }
            draw_status("Undo.");
        } else if(k=='e'||k=='E'){
            /* manual engine trigger */
            if(gEngineActive){
                engine_think_and_move();
            } else draw_status("No engine loaded. Press 'l' to load.");
        } else if(k=='l'||k=='L'){
            load_engine_dialog();
        } else if(k=='t'||k=='T'){
            tc_dialog();
        } else if(k=='n'||k=='N'){
            board_start(&gBoard);
            gHistCount=0; gSelSq=-1; gLastFrom=-1; gLastTo=-1;
            memset(gLegal,0,sizeof(gLegal)); rebuild_pgn();
            draw_status("New game.");
        } else if(k=='f'||k=='F'){
            fen_dialog();
        } else if(k=='q'||k=='Q'||k==27){
            stop_engine();
            break;
        }

        /* check game over */
        Move all4[MAX_MOVES]; int mc2=gen_all_moves(&gBoard,all4);
        if(mc2==0){
            if(in_check(&gBoard,gBoard.side))
                draw_status(gBoard.side==0?"Black wins by checkmate!":"White wins by checkmate!");
            else
                draw_status("Stalemate – draw!");
        }

        full_redraw();
        if(moved_cur||1) fflush(stdout);
    }
    return 0;
}
