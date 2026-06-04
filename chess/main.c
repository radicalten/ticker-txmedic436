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

/* ─── Constants ─────────────────────────────────────────────── */
#define MAX_MOVES      512
#define MAX_HISTORY    256
#define MAX_PGN        8192
#define ENGINE_BUF     4096
#define MAX_LEGAL      256
#define SETTINGS_ITEMS 6

/* Piece codes (0=empty) */
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

/* Colors */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"

/* 256-color helpers */
#define FG(n)  "\033[38;5;" #n "m"
#define BG(n)  "\033[48;5;" #n "m"

/* Board square background colors */
#define BG_LIGHT        "\033[48;5;229m"   /* cream        */
#define BG_DARK         "\033[48;5;94m"    /* brown        */
#define BG_SEL          "\033[48;5;226m"   /* bright yel   */
#define BG_LEGAL        "\033[48;5;118m"   /* green        */
#define BG_LAST_LIGHT   "\033[48;5;190m"   /* yellow-green */
#define BG_LAST_DARK    "\033[48;5;100m"   /* dark yel-grn */
#define BG_CHECK        "\033[48;5;196m"   /* red          */
#define BG_CURSOR       "\033[48;5;45m"    /* cyan         */

/* Foreground for white/black pieces */
#define FG_WHITE_PIECE  "\033[38;5;255m\033[1m"
#define FG_BLACK_PIECE  "\033[38;5;232m\033[1m"

/* UI panel colors */
#define C_TITLE   "\033[38;5;220m\033[1m"
#define C_BORDER  "\033[38;5;240m"
#define C_HEADER  "\033[38;5;75m\033[1m"
#define C_INFO    "\033[38;5;252m"
#define C_HILITE  "\033[38;5;226m\033[1m"
#define C_GREEN   "\033[38;5;118m\033[1m"
#define C_RED     "\033[38;5;196m\033[1m"
#define C_CYAN    "\033[38;5;87m"
#define C_MENU_SEL "\033[48;5;236m\033[38;5;226m\033[1m"
#define C_MENU_NRM "\033[38;5;252m"

/* ─── Structs ────────────────────────────────────────────────── */
typedef struct {
    int from, to;          /* 0-63 */
    int promo;             /* piece code or 0 */
    int captured;
    int castling;          /* 0=none,1=K,2=Q,3=k,4=q */
    int ep_square;         /* en-passant target before move (-1=none) */
    int ep_capture;        /* was this move an ep capture? */
    int halfmove_before;
    unsigned long long hash_before;
    /* castling rights before */
    int cr_before;         /* bits: 1=WK,2=WQ,4=BK,8=BQ */
} Move;

typedef struct {
    int  board[64];
    int  side;             /* 0=white,1=black */
    int  ep;               /* en-passant target square (-1) */
    int  castling;         /* bits */
    int  halfmove;
    int  fullmove;
    Move history[MAX_HISTORY];
    int  hist_count;
    char pgn[MAX_PGN];
    int  pgn_len;
    /* engine pipe fds */
    int  eng_in[2];        /* parent writes to eng_in[1]  */
    int  eng_out[2];       /* parent reads from eng_out[0] */
    pid_t eng_pid;
    int  eng_active;
    /* settings */
    int  depth_limit;      /* 0=off */
    long nodes_limit;      /* 0=off */
    int  movetime;         /* ms, 0=off */
    int  wtime, btime;     /* ms */
    int  winc,  binc;
    int  use_clock;        /* use wtime/btime instead of movetime */
    /* UI state */
    int  cursor;           /* 0-63 */
    int  selected;         /* -1 = none */
    int  legal[MAX_LEGAL];
    int  nlegal;
    int  last_from, last_to;
    int  in_check;
    int  game_over;        /* 0=playing,1=white wins,2=black wins,3=draw */
    int  screen_mode;      /* 0=board,1=settings,2=promo */
    int  settings_sel;
    int  engine_thinking;
    char engine_path[512];
    char status_msg[256];
    int  flip_board;       /* 0=white bottom */
    int  move_count_display; /* for pgn scroll */
} Game;

/* ─── Globals ────────────────────────────────────────────────── */
static struct termios g_orig_termios;
static Game G;

/* ─── Terminal helpers ───────────────────────────────────────── */
static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    t = g_orig_termios;
    t.c_lflag &= ~(ECHO | ICANON);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}
static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}
static void hide_cursor(void) { printf("\033[?25l"); }
static void show_cursor(void) { printf("\033[?25h"); }
static void clear_screen(void) { printf("\033[2J\033[H"); }
static void move_to(int r, int c) { printf("\033[%d;%dH", r, c); }
static void save_cursor(void)    { printf("\033[s"); }
static void restore_cursor(void) { printf("\033[u"); }

/* ─── Zobrist (simple) ───────────────────────────────────────── */
static unsigned long long zob[64][13];
static unsigned long long zob_side;
static unsigned long long zob_ep[8];
static unsigned long long zob_castle[16];
static unsigned long long g_hash;

static void init_zobrist(void) {
    /* deterministic LCG */
    unsigned long long s = 0xdeadbeefcafeULL;
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 13; j++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            zob[i][j] = s;
        }
    s ^= s << 13; s ^= s >> 7; zob_side = s;
    for (int i = 0; i < 8; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        zob_ep[i] = s;
    }
    for (int i = 0; i < 16; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        zob_castle[i] = s;
    }
}

static unsigned long long compute_hash(void) {
    unsigned long long h = 0;
    for (int i = 0; i < 64; i++) h ^= zob[i][G.board[i]];
    if (G.side)  h ^= zob_side;
    if (G.ep >= 0) h ^= zob_ep[G.ep & 7];
    h ^= zob_castle[G.castling & 15];
    return h;
}

/* ─── Board helpers ──────────────────────────────────────────── */
static int piece_color(int p) { return (p>=1&&p<=6)?0:(p>=7&&p<=12)?1:-1; }
static int is_white(int p)    { return p>=1&&p<=6; }
static int is_black(int p)    { return p>=7&&p<=12; }
static int rank_of(int sq)    { return sq/8; }
static int file_of(int sq)    { return sq%8; }
static int sq(int r,int f)    { return r*8+f; }

static const char *piece_glyph(int p) {
    /* Unicode chess pieces */
    switch(p) {
        case WK: return "♔"; case WQ: return "♕";
        case WR: return "♖"; case WB: return "♗";
        case WN: return "♘"; case WP: return "♙";
        case BK: return "♚"; case BQ: return "♛";
        case BR: return "♜"; case BB: return "♝";
        case BN: return "♞"; case BP: return "♟";
        default: return " ";
    }
}

static char piece_letter(int p) {
    switch(p%6==0&&p!=0?6:p%6) {
        case 1: return 'P'; case 2: return 'N'; case 3: return 'B';
        case 4: return 'R'; case 5: return 'Q'; case 0: return 'K';
    }
    return '?';
}

/* FEN helpers */
static void board_to_fen(char *buf, int bufsz) {
    int pos = 0;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int p = G.board[sq(r,f)];
            if (p == EMPTY) { empty++; }
            else {
                if (empty) { buf[pos++] = '0'+empty; empty=0; }
                const char *pcs = ".PNBRQKpnbrqk";
                buf[pos++] = pcs[p];
            }
        }
        if (empty) buf[pos++] = '0'+empty;
        if (r > 0) buf[pos++] = '/';
    }
    buf[pos++] = ' ';
    buf[pos++] = G.side ? 'b' : 'w';
    buf[pos++] = ' ';
    if (!G.castling) buf[pos++] = '-';
    else {
        if (G.castling&1) buf[pos++]='K';
        if (G.castling&2) buf[pos++]='Q';
        if (G.castling&4) buf[pos++]='k';
        if (G.castling&8) buf[pos++]='q';
    }
    buf[pos++] = ' ';
    if (G.ep < 0) buf[pos++] = '-';
    else {
        buf[pos++] = 'a'+file_of(G.ep);
        buf[pos++] = '1'+rank_of(G.ep);
    }
    pos += snprintf(buf+pos, bufsz-pos, " %d %d", G.halfmove, G.fullmove);
    buf[pos] = '\0';
}

static void init_board(void) {
    memset(G.board, 0, sizeof(G.board));
    /* White */
    G.board[0]=WR; G.board[1]=WN; G.board[2]=WB; G.board[3]=WQ;
    G.board[4]=WK; G.board[5]=WB; G.board[6]=WN; G.board[7]=WR;
    for(int f=0;f<8;f++) G.board[8+f]=WP;
    /* Black */
    G.board[56]=BR; G.board[57]=BN; G.board[58]=BB; G.board[59]=BQ;
    G.board[60]=BK; G.board[61]=BB; G.board[62]=BN; G.board[63]=BR;
    for(int f=0;f<8;f++) G.board[48+f]=BP;
    G.side=0; G.ep=-1; G.castling=15;
    G.halfmove=0; G.fullmove=1;
    G.hist_count=0;
    G.pgn[0]='\0'; G.pgn_len=0;
    G.last_from=-1; G.last_to=-1;
    G.game_over=0;
    g_hash = compute_hash();
}

/* ─── Move generation ────────────────────────────────────────── */
/* Raw attack check (ignores pins, just for king safety) */
static int sq_attacked_by(int s, int by_side);

static int is_in_check(int side) {
    /* find king */
    int kp = (side==0)?WK:BK;
    int ksq=-1;
    for(int i=0;i<64;i++) if(G.board[i]==kp){ksq=i;break;}
    if(ksq<0) return 1;
    return sq_attacked_by(ksq, 1-side);
}

static int sq_attacked_by(int s, int by_side) {
    int r=rank_of(s), f=file_of(s);
    /* pawns */
    if(by_side==0) {
        if(r>0&&f>0&&G.board[sq(r-1,f-1)]==WP) return 1;
        if(r>0&&f<7&&G.board[sq(r-1,f+1)]==WP) return 1;
    } else {
        if(r<7&&f>0&&G.board[sq(r+1,f-1)]==BP) return 1;
        if(r<7&&f<7&&G.board[sq(r+1,f+1)]==BP) return 1;
    }
    /* knights */
    int kn = by_side?BN:WN;
    int kdx[]={-2,-2,-1,-1,1,1,2,2};
    int kdy[]={-1,1,-2,2,-2,2,-1,1};
    for(int i=0;i<8;i++){
        int nr=r+kdy[i],nf=f+kdx[i];
        if(nr>=0&&nr<8&&nf>=0&&nf<8&&G.board[sq(nr,nf)]==kn) return 1;
    }
    /* king */
    int kg=by_side?BK:WK;
    for(int dr=-1;dr<=1;dr++) for(int df=-1;df<=1;df++){
        if(!dr&&!df) continue;
        int nr=r+dr,nf=f+df;
        if(nr>=0&&nr<8&&nf>=0&&nf<8&&G.board[sq(nr,nf)]==kg) return 1;
    }
    /* rook/queen (horizontal/vertical) */
    int rq1=by_side?BR:WR, rq2=by_side?BQ:WQ;
    int dirs4[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
    for(int d=0;d<4;d++){
        int nr=r+dirs4[d][0], nf=f+dirs4[d][1];
        while(nr>=0&&nr<8&&nf>=0&&nf<8){
            int p=G.board[sq(nr,nf)];
            if(p!=EMPTY){
                if(p==rq1||p==rq2) return 1;
                break;
            }
            nr+=dirs4[d][0]; nf+=dirs4[d][1];
        }
    }
    /* bishop/queen (diagonal) */
    int bq1=by_side?BB:WB, bq2=by_side?BQ:WQ;
    int dirs8[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
    for(int d=0;d<4;d++){
        int nr=r+dirs8[d][0], nf=f+dirs8[d][1];
        while(nr>=0&&nr<8&&nf>=0&&nf<8){
            int p=G.board[sq(nr,nf)];
            if(p!=EMPTY){
                if(p==bq1||p==bq2) return 1;
                break;
            }
            nr+=dirs8[d][0]; nf+=dirs8[d][1];
        }
    }
    return 0;
}

/* Generate pseudo-legal moves for piece on 'from', push to list */
static int gen_pseudo(int from, int *list, int *n) {
    int p=G.board[from];
    if(!p) return 0;
    int side=piece_color(p);
    if(side!=G.side) return 0;
    int r=rank_of(from), f=file_of(from);
#define ADD(t) do{ list[(*n)++]=(t); }while(0)

    if(p==WP) {
        /* advance 1 */
        if(r<7&&G.board[sq(r+1,f)]==EMPTY){
            ADD(sq(r+1,f));
            /* advance 2 from rank 1 */
            if(r==1&&G.board[sq(r+2,f)]==EMPTY) ADD(sq(r+2,f));
        }
        /* captures */
        if(r<7&&f>0&&is_black(G.board[sq(r+1,f-1)])) ADD(sq(r+1,f-1));
        if(r<7&&f<7&&is_black(G.board[sq(r+1,f+1)])) ADD(sq(r+1,f+1));
        /* en-passant */
        if(G.ep>=0){
            if(r<7&&f>0&&sq(r+1,f-1)==G.ep) ADD(G.ep);
            if(r<7&&f<7&&sq(r+1,f+1)==G.ep) ADD(G.ep);
        }
    } else if(p==BP) {
        if(r>0&&G.board[sq(r-1,f)]==EMPTY){
            ADD(sq(r-1,f));
            if(r==6&&G.board[sq(r-2,f)]==EMPTY) ADD(sq(r-2,f));
        }
        if(r>0&&f>0&&is_white(G.board[sq(r-1,f-1)])) ADD(sq(r-1,f-1));
        if(r>0&&f<7&&is_white(G.board[sq(r-1,f+1)])) ADD(sq(r-1,f+1));
        if(G.ep>=0){
            if(r>0&&f>0&&sq(r-1,f-1)==G.ep) ADD(G.ep);
            if(r>0&&f<7&&sq(r-1,f+1)==G.ep) ADD(G.ep);
        }
    } else if(p==WN||p==BN) {
        int dx[]={-2,-2,-1,-1,1,1,2,2};
        int dy[]={-1,1,-2,2,-2,2,-1,1};
        for(int i=0;i<8;i++){
            int nr=r+dy[i],nf=f+dx[i];
            if(nr<0||nr>7||nf<0||nf>7) continue;
            int t=G.board[sq(nr,nf)];
            if(t==EMPTY||piece_color(t)!=side) ADD(sq(nr,nf));
        }
    } else if(p==WB||p==BB) {
        int dirs[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
        for(int d=0;d<4;d++){
            int nr=r+dirs[d][0],nf=f+dirs[d][1];
            while(nr>=0&&nr<8&&nf>=0&&nf<8){
                int t=G.board[sq(nr,nf)];
                if(t!=EMPTY){if(piece_color(t)!=side)ADD(sq(nr,nf));break;}
                ADD(sq(nr,nf));
                nr+=dirs[d][0]; nf+=dirs[d][1];
            }
        }
    } else if(p==WR||p==BR) {
        int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        for(int d=0;d<4;d++){
            int nr=r+dirs[d][0],nf=f+dirs[d][1];
            while(nr>=0&&nr<8&&nf>=0&&nf<8){
                int t=G.board[sq(nr,nf)];
                if(t!=EMPTY){if(piece_color(t)!=side)ADD(sq(nr,nf));break;}
                ADD(sq(nr,nf));
                nr+=dirs[d][0]; nf+=dirs[d][1];
            }
        }
    } else if(p==WQ||p==BQ) {
        int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        for(int d=0;d<8;d++){
            int nr=r+dirs[d][0],nf=f+dirs[d][1];
            while(nr>=0&&nr<8&&nf>=0&&nf<8){
                int t=G.board[sq(nr,nf)];
                if(t!=EMPTY){if(piece_color(t)!=side)ADD(sq(nr,nf));break;}
                ADD(sq(nr,nf));
                nr+=dirs[d][0]; nf+=dirs[d][1];
            }
        }
    } else if(p==WK||p==BK) {
        for(int dr=-1;dr<=1;dr++) for(int df=-1;df<=1;df++){
            if(!dr&&!df) continue;
            int nr=r+dr,nf=f+df;
            if(nr<0||nr>7||nf<0||nf>7) continue;
            int t=G.board[sq(nr,nf)];
            if(t==EMPTY||piece_color(t)!=side) ADD(sq(nr,nf));
        }
        /* Castling */
        if(p==WK&&from==4&&r==0){
            /* Kingside */
            if((G.castling&1)&&G.board[5]==EMPTY&&G.board[6]==EMPTY
               &&!sq_attacked_by(4,1)&&!sq_attacked_by(5,1)&&!sq_attacked_by(6,1))
                ADD(6);
            /* Queenside */
            if((G.castling&2)&&G.board[3]==EMPTY&&G.board[2]==EMPTY&&G.board[1]==EMPTY
               &&!sq_attacked_by(4,1)&&!sq_attacked_by(3,1)&&!sq_attacked_by(2,1))
                ADD(2);
        }
        if(p==BK&&from==60&&r==7){
            if((G.castling&4)&&G.board[61]==EMPTY&&G.board[62]==EMPTY
               &&!sq_attacked_by(60,0)&&!sq_attacked_by(61,0)&&!sq_attacked_by(62,0))
                ADD(62);
            if((G.castling&8)&&G.board[59]==EMPTY&&G.board[58]==EMPTY&&G.board[57]==EMPTY
               &&!sq_attacked_by(60,0)&&!sq_attacked_by(59,0)&&!sq_attacked_by(58,0))
                ADD(58);
        }
    }
#undef ADD
    return 1;
}

/* Make/unmake for legality test */
static void make_move_temp(int from, int to, int promo) {
    int p = G.board[from];
    int side = piece_color(p);
    G.board[to] = promo ? promo : p;
    G.board[from] = EMPTY;
    /* En-passant capture */
    if((p==WP||p==BP)&&to==G.ep&&G.ep>=0){
        int cap_sq = (side==0)?to-8:to+8;
        G.board[cap_sq]=EMPTY;
    }
    /* Castling rook move */
    if(p==WK&&from==4){
        if(to==6){G.board[7]=EMPTY;G.board[5]=WR;}
        if(to==2){G.board[0]=EMPTY;G.board[3]=WR;}
    }
    if(p==BK&&from==60){
        if(to==62){G.board[63]=EMPTY;G.board[61]=BR;}
        if(to==58){G.board[56]=EMPTY;G.board[59]=BR;}
    }
}

static void unmake_move_temp(int from, int to, int promo,
                              int captured, int ep_cap, int ep_sq,
                              int orig_piece) {
    int side = (orig_piece>=1&&orig_piece<=6)?0:1;
    G.board[from] = orig_piece;
    G.board[to]   = ep_cap ? EMPTY : captured;
    if(ep_cap && ep_sq>=0){
        int cap_sq = (side==0)?to-8:to+8;
        G.board[cap_sq] = (side==0)?BP:WP;
    }
    /* Castling undo */
    if(orig_piece==WK&&from==4){
        if(to==6){G.board[7]=WR;G.board[5]=EMPTY;}
        if(to==2){G.board[0]=WR;G.board[3]=EMPTY;}
    }
    if(orig_piece==BK&&from==60){
        if(to==62){G.board[63]=BR;G.board[61]=EMPTY;}
        if(to==58){G.board[56]=BR;G.board[59]=EMPTY;}
    }
    (void)promo;
}

static int is_legal(int from, int to, int promo) {
    int p = G.board[from];
    int captured = G.board[to];
    int ep_sq = G.ep;
    int ep_cap = (p==WP||p==BP)&&to==G.ep&&G.ep>=0;
    int side = piece_color(p);
    make_move_temp(from,to,promo);
    int check = is_in_check(side);
    unmake_move_temp(from,to,promo,captured,ep_cap,ep_sq,p);
    return !check;
}

/* Compute legal destinations for 'from' */
static void compute_legal(int from) {
    G.nlegal = 0;
    int pseudo[64]; int np=0;
    gen_pseudo(from, pseudo, &np);
    for(int i=0;i<np;i++){
        if(is_legal(from, pseudo[i], 0))
            G.legal[G.nlegal++] = pseudo[i];
    }
}

/* Does the current side have any legal move? */
static int has_legal_move(void) {
    for(int from=0;from<64;from++){
        if(G.board[from]==EMPTY) continue;
        if(piece_color(G.board[from])!=G.side) continue;
        int pseudo[64]; int np=0;
        gen_pseudo(from,pseudo,&np);
        for(int i=0;i<np;i++)
            if(is_legal(from,pseudo[i],0)) return 1;
    }
    return 0;
}

/* ─── PGN helpers ────────────────────────────────────────────── */
static int is_legal_square(int to) {
    for(int i=0;i<G.nlegal;i++) if(G.legal[i]==to) return 1;
    return 0;
}

/* Build SAN for a move (before making it) */
static void move_to_san(int from, int to, int promo, char *san) {
    int p = G.board[from];
    int cap = G.board[to] || ((p==WP||p==BP)&&to==G.ep&&G.ep>=0);
    int pos=0;

    if(p==WK||p==BK) {
        /* castling? */
        if(abs(file_of(to)-file_of(from))==2) {
            if(file_of(to)==6) { strcpy(san,"O-O"); }
            else               { strcpy(san,"O-O-O"); }
            /* check/checkmate appended later */
            return;
        }
    }

    char pl = piece_letter(p);
    if(pl!='P') san[pos++]=pl;

    /* disambiguation */
    if(pl!='P') {
        /* Check if another piece of same type can go to 'to' */
        int ambig_file=0, ambig_rank=0, ambig=0;
        for(int sq2=0;sq2<64;sq2++){
            if(sq2==from) continue;
            if(G.board[sq2]!=p) continue;
            int pseudo2[64]; int np2=0;
            gen_pseudo(sq2,pseudo2,&np2);
            for(int i=0;i<np2;i++){
                if(pseudo2[i]==to&&is_legal(sq2,to,0)){
                    ambig=1;
                    if(file_of(sq2)==file_of(from)) ambig_rank=1;
                    else                             ambig_file=1;
                }
            }
        }
        if(ambig){
            if(ambig_file&&!ambig_rank) san[pos++]='1'+rank_of(from);
            else if(!ambig_file)        san[pos++]='a'+file_of(from);
            else { san[pos++]='a'+file_of(from); san[pos++]='1'+rank_of(from); }
        }
    } else if(cap) {
        san[pos++]='a'+file_of(from);
    }
    if(cap) san[pos++]='x';
    san[pos++]='a'+file_of(to);
    san[pos++]='1'+rank_of(to);
    if(promo){
        san[pos++]='=';
        san[pos++]=piece_letter(promo);
    }
    san[pos]='\0';
}

static void append_pgn(const char *san, int is_check, int is_mate) {
    char buf[32];
    int side_before = G.side; /* side that just moved */
    /* We append AFTER making the move, so G.side is already flipped */
    /* The mover was 1-G.side */
    int mover = 1-G.side;
    if(mover==0) {
        /* White just moved: add move number */
        snprintf(buf,sizeof(buf),"%d. ",G.fullmove-1? G.fullmove: G.fullmove);
        /* Actually fullmove increments after black, so white's move number: */
        snprintf(buf,sizeof(buf),"%d. ",G.fullmove - (G.side==0?0:0));
        /* simpler: track in hist */
    }
    (void)side_before;
    /* We'll rebuild PGN from history */
    char tmp[MAX_PGN]; tmp[0]='\0';
    int pos=0;
    for(int i=0;i<G.hist_count;i++){
        Move *m=&G.history[i];
        if(i%2==0){
            int num=i/2+1;
            pos+=snprintf(tmp+pos,MAX_PGN-pos,"%d. ",num);
        }
        /* Reconstruct SAN – we stored it in a separate array */
        /* Actually let's just use a simple move notation */
        char sq_from[3],sq_to[3];
        sq_from[0]='a'+file_of(m->from); sq_from[1]='1'+rank_of(m->from); sq_from[2]=0;
        sq_to[0]  ='a'+file_of(m->to);   sq_to[1]  ='1'+rank_of(m->to);   sq_to[2]  =0;
        pos+=snprintf(tmp+pos,MAX_PGN-pos,"%s%s ",sq_from,sq_to);
        if(i%2==1) pos+=snprintf(tmp+pos,MAX_PGN-pos," ");
    }
    /* Append result */
    if(G.game_over==1) pos+=snprintf(tmp+pos,MAX_PGN-pos,"1-0");
    else if(G.game_over==2) pos+=snprintf(tmp+pos,MAX_PGN-pos,"0-1");
    else if(G.game_over==3) pos+=snprintf(tmp+pos,MAX_PGN-pos,"1/2-1/2");
    (void)san;(void)is_check;(void)is_mate;
    strncpy(G.pgn,tmp,MAX_PGN-1); G.pgn_len=pos;
}

/* Rebuild PGN with SAN from scratch */
static char g_san[MAX_HISTORY][16];
static int  g_san_check[MAX_HISTORY];

static void rebuild_pgn(void) {
    char tmp[MAX_PGN]; int pos=0;
    for(int i=0;i<G.hist_count;i++){
        if(i%2==0) pos+=snprintf(tmp+pos,MAX_PGN-pos,"%d. ",i/2+1);
        pos+=snprintf(tmp+pos,MAX_PGN-pos,"%s",g_san[i]);
        if(g_san_check[i]==2) pos+=snprintf(tmp+pos,MAX_PGN-pos,"# ");
        else if(g_san_check[i]==1) pos+=snprintf(tmp+pos,MAX_PGN-pos,"+ ");
        else pos+=snprintf(tmp+pos,MAX_PGN-pos," ");
    }
    if(G.game_over==1)      pos+=snprintf(tmp+pos,MAX_PGN-pos,"1-0");
    else if(G.game_over==2) pos+=snprintf(tmp+pos,MAX_PGN-pos,"0-1");
    else if(G.game_over==3) pos+=snprintf(tmp+pos,MAX_PGN-pos,"1/2-1/2");
    strncpy(G.pgn,tmp,MAX_PGN-1); G.pgn[MAX_PGN-1]=0;
    G.pgn_len=pos;
}

/* ─── Apply/undo move ────────────────────────────────────────── */
static void do_move(int from, int to, int promo) {
    int p=G.board[from];
    int side=G.side;
    Move *m=&G.history[G.hist_count];
    m->from=from; m->to=to; m->promo=promo;
    m->captured=G.board[to];
    m->ep_square=G.ep;
    m->halfmove_before=G.halfmove;
    m->hash_before=g_hash;
    m->cr_before=G.castling;
    m->ep_capture=0;
    m->castling=0;

    /* SAN before making move */
    move_to_san(from,to,promo,g_san[G.hist_count]);

    /* En-passant? */
    if((p==WP||p==BP)&&to==G.ep&&G.ep>=0){
        m->ep_capture=1;
        int cap_sq=(side==0)?to-8:to+8;
        m->captured=G.board[cap_sq];
        G.board[cap_sq]=EMPTY;
    }

    /* Castling? */
    if(p==WK&&from==4){
        if(to==6){G.board[7]=EMPTY;G.board[5]=WR;m->castling=1;}
        if(to==2){G.board[0]=EMPTY;G.board[3]=WR;m->castling=2;}
    }
    if(p==BK&&from==60){
        if(to==62){G.board[63]=EMPTY;G.board[61]=BR;m->castling=3;}
        if(to==58){G.board[56]=EMPTY;G.board[59]=BR;m->castling=4;}
    }

    /* Move piece */
    G.board[to]=(promo?promo:p);
    G.board[from]=EMPTY;

    /* Update castling rights */
    if(p==WK) G.castling&=~3;
    if(p==BK) G.castling&=~12;
    if(from==0||to==0) G.castling&=~2;
    if(from==7||to==7) G.castling&=~1;
    if(from==56||to==56) G.castling&=~8;
    if(from==63||to==63) G.castling&=~4;

    /* En-passant target */
    G.ep=-1;
    if(p==WP&&rank_of(from)==1&&rank_of(to)==3) G.ep=sq(2,file_of(from));
    if(p==BP&&rank_of(from)==6&&rank_of(to)==4) G.ep=sq(5,file_of(from));

    /* Half/full move */
    if(p==WP||p==BP||m->captured) G.halfmove=0; else G.halfmove++;
    if(side==1) G.fullmove++;
    G.side^=1;
    g_hash=compute_hash();

    /* Check status for SAN annotation */
    int in_chk=is_in_check(G.side);
    int has_lm=has_legal_move();
    g_san_check[G.hist_count]= in_chk ? (has_lm?1:2) : 0;

    G.hist_count++;
    G.last_from=from; G.last_to=to;

    /* Check game over */
    G.in_check=in_chk;
    if(!has_lm){
        if(in_chk) G.game_over=(G.side==0)?2:1; /* the side that can't move lost */
        else G.game_over=3; /* stalemate */
    }
    if(G.halfmove>=100) G.game_over=3;

    rebuild_pgn();
}

static void undo_move(void) {
    if(G.hist_count==0) return;
    G.hist_count--;
    Move *m=&G.history[G.hist_count];
    G.side^=1;
    if(G.side==1) G.fullmove--;
    G.halfmove=m->halfmove_before;
    G.ep=m->ep_square;
    G.castling=m->cr_before;
    g_hash=m->hash_before;

    int p=G.board[m->to];
    if(m->promo) p=(G.side==0)?WP:BP;

    /* Undo castling rook */
    if(m->castling==1){G.board[5]=EMPTY;G.board[7]=WR;}
    if(m->castling==2){G.board[3]=EMPTY;G.board[0]=WR;}
    if(m->castling==3){G.board[61]=EMPTY;G.board[63]=BR;}
    if(m->castling==4){G.board[59]=EMPTY;G.board[56]=BR;}

    G.board[m->from]=p;
    G.board[m->to]=m->ep_capture?EMPTY:m->captured;
    if(m->ep_capture){
        int cap_sq=(G.side==0)?m->to-8:m->to+8;
        G.board[cap_sq]=m->captured;
    }

    G.last_from=(G.hist_count>0)?G.history[G.hist_count-1].from:-1;
    G.last_to  =(G.hist_count>0)?G.history[G.hist_count-1].to  :-1;
    G.in_check =is_in_check(G.side);
    G.game_over=0;
    rebuild_pgn();
}

/* ─── Engine I/O ─────────────────────────────────────────────── */
static void engine_write(const char *cmd) {
    if(!G.eng_active) return;
    write(G.eng_in[1], cmd, strlen(cmd));
    write(G.eng_in[1], "\n", 1);
}

static int engine_readline(char *buf, int sz, int timeout_ms) {
    if(!G.eng_active) return 0;
    int pos=0; char c;
    struct timeval tv; fd_set fds;
    long deadline_us=(long)timeout_ms*1000L;
    while(pos<sz-1){
        FD_ZERO(&fds); FD_SET(G.eng_out[0],&fds);
        tv.tv_sec=deadline_us/1000000L;
        tv.tv_usec=deadline_us%1000000L;
        int r=select(G.eng_out[0]+1,&fds,NULL,NULL,&tv);
        if(r<=0) break;
        if(read(G.eng_out[0],&c,1)!=1) break;
        if(c=='\n'){buf[pos]=0;return 1;}
        buf[pos++]=c;
    }
    buf[pos]=0;
    return pos>0;
}

static int start_engine(const char *path) {
    if(G.eng_active) return 1;
    if(pipe(G.eng_in)<0||pipe(G.eng_out)<0) return 0;
    G.eng_pid=fork();
    if(G.eng_pid<0) return 0;
    if(G.eng_pid==0){
        dup2(G.eng_in[0],STDIN_FILENO);
        dup2(G.eng_out[1],STDOUT_FILENO);
        close(G.eng_in[1]); close(G.eng_out[0]);
        execl(path,path,(char*)NULL);
        exit(1);
    }
    close(G.eng_in[0]); close(G.eng_out[1]);
    G.eng_active=1;
    /* Handshake */
    engine_write("uci");
    char buf[512];
    for(int i=0;i<50;i++){
        if(!engine_readline(buf,512,200)) break;
        if(strncmp(buf,"uciok",5)==0) break;
    }
    engine_write("isready");
    for(int i=0;i<50;i++){
        if(!engine_readline(buf,512,300)) break;
        if(strncmp(buf,"readyok",7)==0) break;
    }
    return 1;
}

static void stop_engine(void) {
    if(!G.eng_active) return;
    engine_write("quit");
    usleep(100000);
    kill(G.eng_pid,SIGTERM);
    waitpid(G.eng_pid,NULL,WNOHANG);
    close(G.eng_in[1]); close(G.eng_out[0]);
    G.eng_active=0;
}

/* Send position + go command to engine */
static void engine_go(void) {
    if(!G.eng_active||G.game_over) return;

    /* Build move list */
    char moves_str[4096]="";
    int pos=0;
    for(int i=0;i<G.hist_count;i++){
        Move *m=&G.history[i];
        int len=snprintf(moves_str+pos,sizeof(moves_str)-pos,
            " %c%c%c%c",
            'a'+file_of(m->from),'1'+rank_of(m->from),
            'a'+file_of(m->to),  '1'+rank_of(m->to));
        pos+=len;
        if(m->promo){
            char pl=tolower(piece_letter(m->promo));
            moves_str[pos++]=pl; moves_str[pos]=0;
        }
    }

    char cmd[5120];
    snprintf(cmd,sizeof(cmd),"position startpos moves%s",moves_str);
    engine_write(cmd);

    /* Build go command */
    char go[256]="go";
    int gp=2;
    if(G.depth_limit>0)
        gp+=snprintf(go+gp,sizeof(go)-gp," depth %d",G.depth_limit);
    if(G.nodes_limit>0)
        gp+=snprintf(go+gp,sizeof(go)-gp," nodes %ld",G.nodes_limit);
    if(G.movetime>0&&!G.use_clock)
        gp+=snprintf(go+gp,sizeof(go)-gp," movetime %d",G.movetime);
    if(G.use_clock){
        gp+=snprintf(go+gp,sizeof(go)-gp,
            " wtime %d btime %d winc %d binc %d",
            G.wtime,G.btime,G.winc,G.binc);
    }
    if(G.depth_limit==0&&G.nodes_limit==0&&G.movetime==0&&!G.use_clock)
        gp+=snprintf(go+gp,sizeof(go)-gp," movetime 1000");

    engine_write(go);
    G.engine_thinking=1;
}

/* Parse engine bestmove */
static int engine_parse_bestmove(const char *line, int *from, int *to, int *promo){
    const char *bm=strstr(line,"bestmove ");
    if(!bm) return 0;
    bm+=9;
    if(strlen(bm)<4) return 0;
    *from=sq(bm[1]-'1',bm[0]-'a');
    *to  =sq(bm[3]-'1',bm[2]-'a');
    *promo=0;
    if(bm[4]&&bm[4]!='\n'&&bm[4]!=' '){
        switch(bm[4]){
            case 'q': *promo=(G.side?BQ:WQ); break;
            case 'r': *promo=(G.side?BR:WR); break;
            case 'b': *promo=(G.side?BB:WB); break;
            case 'n': *promo=(G.side?BN:WN); break;
        }
    }
    return 1;
}

/* Poll engine without blocking */
static int poll_engine(void) {
    if(!G.eng_active||!G.engine_thinking) return 0;
    struct timeval tv={0,0};
    fd_set fds; FD_ZERO(&fds); FD_SET(G.eng_out[0],&fds);
    if(select(G.eng_out[0]+1,&fds,NULL,NULL,&tv)<=0) return 0;
    char buf[512]; int pos=0; char c;
    while(pos<511){
        struct timeval tv2={0,1000};
        fd_set fds2; FD_ZERO(&fds2); FD_SET(G.eng_out[0],&fds2);
        if(select(G.eng_out[0]+1,&fds2,NULL,NULL,&tv2)<=0) break;
        if(read(G.eng_out[0],&c,1)!=1) break;
        if(c=='\n') break;
        buf[pos++]=c;
    }
    buf[pos]=0;
    if(strncmp(buf,"bestmove",8)==0){
        int from,to,promo;
        if(engine_parse_bestmove(buf,&from,&to,&promo)){
            G.engine_thinking=0;
            do_move(from,to,promo);
            G.selected=-1; G.nlegal=0;
            snprintf(G.status_msg,sizeof(G.status_msg),"Engine played: %c%c%c%c",
                'a'+file_of(from),'1'+rank_of(from),
                'a'+file_of(to),  '1'+rank_of(to));
        }
        return 1;
    }
    return 0;
}

/* ─── Drawing ────────────────────────────────────────────────── */
#define BOARD_ROW 2
#define BOARD_COL 2
#define CELL_W    5  /* chars per cell */
#define CELL_H    2  /* rows per cell  */
#define PANEL_COL 47

static void draw_cell(int r, int c, int s, int piece, int is_sel,
                       int is_legal_sq, int is_last, int is_cursor,
                       int in_chk) {
    /* s = visual square index (0-63 after flip) */
    (void)s;
    const char *bg;
    int light = (r+c)%2==0;

    if(in_chk)       bg=BG_CHECK;
    else if(is_cursor)    bg=BG_CURSOR;
    else if(is_sel)       bg=BG_SEL;
    else if(is_legal_sq)  bg=BG_LEGAL;
    else if(is_last)      bg=(light?BG_LAST_LIGHT:BG_LAST_DARK);
    else                  bg=(light?BG_LIGHT:BG_DARK);

    const char *fg = is_white(piece)?FG_WHITE_PIECE:FG_BLACK_PIECE;
    const char *glyph = piece_glyph(piece);

    /* Top half of cell */
    int row = BOARD_ROW + (7-r)*CELL_H;
    int col = BOARD_COL + c*CELL_W;
    move_to(row,col);
    printf("%s     "C_RESET,bg);
    /* Bottom half with piece centered */
    move_to(row+1,col);
    if(piece)
        printf("%s  %s%s  "C_RESET, bg, fg, glyph);
    else
        printf("%s     "C_RESET, bg);

    /* Legal move dot */
    if(is_legal_sq&&!piece){
        move_to(row+1,col+2);
        printf("%s"FG(232)"·"C_RESET,bg);
    }
    if(is_legal_sq&&piece){
        /* show green corner indicators */
        move_to(row,col);
        printf("%s"FG(118)"◆"C_RESET,bg);
    }
}

static void draw_board(void) {
    /* File labels */
    move_to(BOARD_ROW+16,BOARD_COL);
    printf(C_DIM);
    for(int f=0;f<8;f++){
        char lbl = G.flip_board ? 'h'-f : 'a'+f;
        printf("  %c  ",lbl);
    }
    printf(C_RESET);

    for(int r=0;r<8;r++){
        int disp_r = G.flip_board ? 7-r : r;
        /* Rank label */
        move_to(BOARD_ROW+(7-r)*CELL_H+1, BOARD_COL-1);
        printf(C_DIM"%d"C_RESET, disp_r+1);

        for(int f=0;f<8;f++){
            int disp_f = G.flip_board ? 7-f : f;
            int board_sq = sq(disp_r,disp_f);
            int piece = G.board[board_sq];

            int is_sel     = (G.selected==board_sq);
            int is_cur     = (G.cursor==board_sq);
            int is_legal_s = 0;
            for(int i=0;i<G.nlegal;i++) if(G.legal[i]==board_sq){is_legal_s=1;break;}
            int is_last = (board_sq==G.last_from||board_sq==G.last_to);
            int in_chk  = (G.in_check &&
                           ((G.side==0&&piece==WK)||(G.side==1&&piece==BK)));

            draw_cell(r,f,board_sq,piece,is_sel,is_legal_s,is_last,is_cur,in_chk);
        }
    }
}

static void draw_panel(void) {
    int pc=PANEL_COL;
    /* Title */
    move_to(1,pc);
    printf(C_TITLE"╔══════════════════════════════╗"C_RESET);
    move_to(2,pc);
    printf(C_TITLE"║    ♔  Terminal Chess GUI  ♚   ║"C_RESET);
    move_to(3,pc);
    printf(C_TITLE"╚══════════════════════════════╝"C_RESET);

    /* Engine info */
    move_to(5,pc);
    printf(C_HEADER"  ENGINE"C_RESET);
    move_to(6,pc);
    if(G.eng_active)
        printf(C_GREEN"  ● Active: "C_INFO"%s"C_RESET"                ",
               G.engine_path[0]?G.engine_path:"<engine>");
    else
        printf(C_RED"  ○ No engine loaded           "C_RESET);

    move_to(7,pc);
    if(G.engine_thinking)
        printf(C_HILITE"  ⟳ Engine thinking...         "C_RESET);
    else
        printf("                               ");

    /* Turn indicator */
    move_to(9,pc);
    printf(C_HEADER"  TURN"C_RESET);
    move_to(10,pc);
    if(G.game_over){
        const char *res[]={"","White wins! 1-0","Black wins! 0-1","Draw 1/2-1/2"};
        printf(C_HILITE"  %s"C_RESET"              ",res[G.game_over]);
    } else {
        printf("  %s %s to move%s                  ",
               G.side==0?FG_WHITE_PIECE:FG_BLACK_PIECE,
               G.side==0?"White ♔":"Black ♚",C_RESET);
    }
    if(G.in_check&&!G.game_over){
        move_to(11,pc);
        printf(C_RED"  ⚠  CHECK!                    "C_RESET);
    } else {
        move_to(11,pc);
        printf("                               ");
    }

    /* Move history / PGN */
    move_to(13,pc);
    printf(C_HEADER"  MOVES (PGN)"C_RESET);
    move_to(14,pc);
    printf(C_BORDER"  ┌─────────────────────────┐"C_RESET);

    /* Display last ~8 rows of PGN wrapped */
    char pgn_display[MAX_PGN];
    strncpy(pgn_display,G.pgn,MAX_PGN-1);
    /* Wrap into lines of 26 chars */
    char lines[10][32]; int nlines=0;
    char *tok=strtok(pgn_display," ");
    char cur_line[32]=""; int cur_len=0;
    while(tok&&nlines<10){
        int tl=strlen(tok);
        if(cur_len+tl+1<=25){
            if(cur_len) {cur_line[cur_len++]=' ';cur_line[cur_len]=0;}
            strncat(cur_line,tok,31-cur_len);
            cur_len+=tl;
        } else {
            if(cur_len) strncpy(lines[nlines++],cur_line,31);
            strncpy(cur_line,tok,31); cur_len=tl;
        }
        tok=strtok(NULL," ");
    }
    if(cur_len&&nlines<10) strncpy(lines[nlines++],cur_line,31);

    int start_line=nlines>8?nlines-8:0;
    for(int i=0;i<8;i++){
        move_to(15+i,pc);
        int li=start_line+i;
        if(li<nlines)
            printf(C_BORDER"  │"C_INFO" %-25s"C_BORDER"│"C_RESET,lines[li]);
        else
            printf(C_BORDER"  │                         │"C_RESET);
    }
    move_to(23,pc);
    printf(C_BORDER"  └─────────────────────────┘"C_RESET);

    /* Status message */
    move_to(25,pc);
    printf(C_CYAN"  %-29s"C_RESET,G.status_msg);

    /* Controls */
    move_to(27,pc);
    printf(C_HEADER"  CONTROLS"C_RESET);
    move_to(28,pc); printf(C_DIM"  ←↑↓→/hjkl  Move cursor    "C_RESET);
    move_to(29,pc); printf(C_DIM"  Space/Enter Select/Move    "C_RESET);
    move_to(30,pc); printf(C_DIM"  u           Undo move      "C_RESET);
    move_to(31,pc); printf(C_DIM"  e           Engine move    "C_RESET);
    move_to(32,pc); printf(C_DIM"  s           Settings       "C_RESET);
    move_to(33,pc); printf(C_DIM"  f           Flip board     "C_RESET);
    move_to(34,pc); printf(C_DIM"  n           New game       "C_RESET);
    move_to(35,pc); printf(C_DIM"  q           Quit           "C_RESET);
}

/* ─── Settings screen ────────────────────────────────────────── */
static const char *settings_labels[SETTINGS_ITEMS]={
    "Engine Path",
    "Depth Limit (0=off)",
    "Nodes Limit (0=off)",
    "Move Time ms (0=off)",
    "Use Clock (wtime/btime)",
    "Clock Time ms each"
};

static void draw_settings(void) {
    clear_screen();
    move_to(1,2);
    printf(C_TITLE"╔══════════════════════════════════════════╗"C_RESET);
    move_to(2,2);
    printf(C_TITLE"║           ENGINE SETTINGS                ║"C_RESET);
    move_to(3,2);
    printf(C_TITLE"╚══════════════════════════════════════════╝"C_RESET);
    move_to(5,2);
    printf(C_DIM"Use ↑↓ to select, Enter to edit, Esc to return"C_RESET);

    for(int i=0;i<SETTINGS_ITEMS;i++){
        char val[64]="";
        switch(i){
            case 0: snprintf(val,64,"%s",G.engine_path[0]?G.engine_path:"(none)"); break;
            case 1: snprintf(val,64,"%d",G.depth_limit); break;
            case 2: snprintf(val,64,"%ld",G.nodes_limit); break;
            case 3: snprintf(val,64,"%d",G.movetime); break;
            case 4: snprintf(val,64,"%s",G.use_clock?"yes":"no"); break;
            case 5: snprintf(val,64,"%d",G.wtime); break;
        }
        move_to(7+i*2,2);
        if(i==G.settings_sel)
            printf(C_MENU_SEL" ▶  %-24s : %-20s "C_RESET,
                   settings_labels[i],val);
        else
            printf(C_MENU_NRM"    %-24s : %-20s "C_RESET,
                   settings_labels[i],val);
    }
    move_to(7+SETTINGS_ITEMS*2+2,2);
    printf(C_DIM"(E) Start/Restart engine   (K) Kill engine"C_RESET);
    move_to(7+SETTINGS_ITEMS*2+3,2);
    printf(C_DIM"(Esc) Back to game"C_RESET);
}

static void settings_edit(int item) {
    show_cursor();
    char buf[512]="";
    move_to(7+item*2,50);
    printf(C_HILITE"Edit: "C_RESET);
    printf("\033[K"); /* clear to end */
    /* Simple line input */
    struct termios t=g_orig_termios;
    t.c_lflag|=(ECHO|ICANON);
    tcsetattr(STDIN_FILENO,TCSANOW,&t);
    if(fgets(buf,sizeof(buf),stdin)){
        buf[strcspn(buf,"\n")]=0;
        switch(item){
            case 0: strncpy(G.engine_path,buf,511); break;
            case 1: G.depth_limit=atoi(buf); break;
            case 2: G.nodes_limit=atol(buf); break;
            case 3: G.movetime=atoi(buf); break;
            case 4: G.use_clock=(buf[0]=='y'||buf[0]=='1'); break;
            case 5: G.wtime=G.btime=atoi(buf); break;
        }
    }
    term_raw();
    hide_cursor();
}

/* ─── Promotion chooser ──────────────────────────────────────── */
static int g_promo_from, g_promo_to;
static int choose_promotion(void) {
    /* Return chosen piece or 0 for cancel */
    /* Overlay a small menu */
    int row=12, col=15;
    int side=G.side;
    int choices[4]={side?BQ:WQ, side?BR:WR, side?BB:WB, side?BN:WN};
    char *names[4]={"Queen","Rook","Bishop","Knight"};
    int sel=0;
    while(1){
        move_to(row,col);
        printf(C_TITLE"╔═══════════════════╗"C_RESET);
        move_to(row+1,col);
        printf(C_TITLE"║ Choose Promotion  ║"C_RESET);
        move_to(row+2,col);
        printf(C_TITLE"╠═══════════════════╣"C_RESET);
        for(int i=0;i<4;i++){
            move_to(row+3+i,col);
            if(i==sel)
                printf(C_MENU_SEL"║  %s %-13s ║"C_RESET,
                       piece_glyph(choices[i]),names[i]);
            else
                printf(C_MENU_NRM"║  %s %-13s ║"C_RESET,
                       piece_glyph(choices[i]),names[i]);
        }
        move_to(row+7,col);
        printf(C_TITLE"║  Esc=Cancel       ║"C_RESET);
        move_to(row+8,col);
        printf(C_TITLE"╚═══════════════════╝"C_RESET);
        fflush(stdout);

        /* read key */
        char k[4]={0};
        int n=read(STDIN_FILENO,k,3);
        if(n<=0) continue;
        if(k[0]==27&&k[1]==0) return 0; /* Esc */
        if(k[0]==27&&k[1]=='['){
            if(k[2]=='A') sel=(sel+3)%4;
            if(k[2]=='B') sel=(sel+1)%4;
        } else if(k[0]=='\n'||k[0]=='\r'||k[0]==' '){
            return choices[sel];
        } else if(k[0]=='q'||k[0]=='Q') return choices[0];
        else if(k[0]=='r'||k[0]=='R') return choices[1];
        else if(k[0]=='b'||k[0]=='B') return choices[2];
        else if(k[0]=='n'||k[0]=='N') return choices[3];
    }
}

/* ─── Render full screen ─────────────────────────────────────── */
static void render(void) {
    if(G.screen_mode==1){
        draw_settings();
        fflush(stdout);
        return;
    }
    /* Board + panel */
    move_to(1,1);
    draw_board();
    draw_panel();
    /* Border around board */
    move_to(1,BOARD_COL-1);
    printf(C_BORDER);
    for(int r=0;r<=17;r++){
        move_to(BOARD_ROW-1+r,BOARD_COL-1);
        if(r==0) printf("┌────────────────────────────────────────┐");
        else if(r==17) printf("└────────────────────────────────────────┘");
        else { printf("│"); move_to(BOARD_ROW-1+r,BOARD_COL+40); printf("│"); }
    }
    printf(C_RESET);
    fflush(stdout);
}

/* ─── Input handling ─────────────────────────────────────────── */
static void try_select_or_move(void) {
    if(G.game_over) return;
    int sq_here = G.cursor;

    if(G.selected<0){
        /* Select */
        if(G.board[sq_here]==EMPTY) return;
        if(piece_color(G.board[sq_here])!=G.side){
            snprintf(G.status_msg,sizeof(G.status_msg),"Not your piece!");
            return;
        }
        G.selected=sq_here;
        compute_legal(sq_here);
        if(G.nlegal==0){
            snprintf(G.status_msg,sizeof(G.status_msg),"No legal moves for this piece");
            G.selected=-1;
        } else {
            snprintf(G.status_msg,sizeof(G.status_msg),"Selected %c%c (%d moves)",
                'a'+file_of(sq_here),'1'+rank_of(sq_here),G.nlegal);
        }
    } else {
        /* Move or deselect */
        if(sq_here==G.selected){ G.selected=-1; G.nlegal=0; return; }

        /* Check if destination is legal */
        int legal=0;
        for(int i=0;i<G.nlegal;i++) if(G.legal[i]==sq_here){legal=1;break;}
        if(!legal){
            /* Maybe re-select another piece */
            if(G.board[sq_here]!=EMPTY&&piece_color(G.board[sq_here])==G.side){
                G.selected=sq_here;
                compute_legal(sq_here);
                snprintf(G.status_msg,sizeof(G.status_msg),"Selected %c%c (%d moves)",
                    'a'+file_of(sq_here),'1'+rank_of(sq_here),G.nlegal);
                return;
            }
            snprintf(G.status_msg,sizeof(G.status_msg),"Illegal move!");
            return;
        }

        /* Promotion? */
        int promo=0;
        int piece=G.board[G.selected];
        if((piece==WP&&rank_of(sq_here)==7)||(piece==BP&&rank_of(sq_here)==0)){
            g_promo_from=G.selected; g_promo_to=sq_here;
            promo=choose_promotion();
            if(!promo){ G.selected=-1; G.nlegal=0; return; }
        }

        int from=G.selected;
        G.selected=-1; G.nlegal=0;
        do_move(from,sq_here,promo);
        snprintf(G.status_msg,sizeof(G.status_msg),"Played %c%c%c%c",
            'a'+file_of(from),'1'+rank_of(from),
            'a'+file_of(sq_here),'1'+rank_of(sq_here));
    }
}

static void handle_key(char *k, int n) {
    if(n<=0) return;

    if(G.screen_mode==1){
        /* Settings */
        if(k[0]==27&&k[1]==0){ G.screen_mode=0; clear_screen(); return; }
        if(k[0]==27&&k[1]=='['&&k[2]=='A') G.settings_sel=(G.settings_sel+SETTINGS_ITEMS-1)%SETTINGS_ITEMS;
        if(k[0]==27&&k[1]=='['&&k[2]=='B') G.settings_sel=(G.settings_sel+1)%SETTINGS_ITEMS;
        if(k[0]=='\n'||k[0]=='\r') settings_edit(G.settings_sel);
        if(k[0]=='e'||k[0]=='E'){
            stop_engine();
            if(G.engine_path[0]) start_engine(G.engine_path);
        }
        if(k[0]=='k'||k[0]=='K') stop_engine();
        return;
    }

    /* Board mode */
    if(k[0]==27&&k[1]=='['){
        int cr=rank_of(G.cursor), cf=file_of(G.cursor);
        if(G.flip_board){
            if(k[2]=='A'&&cr>0) G.cursor=sq(cr-1,cf);
            if(k[2]=='B'&&cr<7) G.cursor=sq(cr+1,cf);
            if(k[2]=='C'&&cf<7) G.cursor=sq(cr,cf+1);
            if(k[2]=='D'&&cf>0) G.cursor=sq(cr,cf-1);
        } else {
            if(k[2]=='A'&&cr<7) G.cursor=sq(cr+1,cf);
            if(k[2]=='B'&&cr>0) G.cursor=sq(cr-1,cf);
            if(k[2]=='C'&&cf<7) G.cursor=sq(cr,cf+1);
            if(k[2]=='D'&&cf>0) G.cursor=sq(cr,cf-1);
        }
        return;
    }

    switch(k[0]){
        /* vim-style movement */
        case 'h':{ int cf=file_of(G.cursor),cr=rank_of(G.cursor);
                   if(cf>0) G.cursor=sq(cr,cf-1); break; }
        case 'l':{ int cf=file_of(G.cursor),cr=rank_of(G.cursor);
                   if(cf<7) G.cursor=sq(cr,cf+1); break; }
        case 'k':{ int cf=file_of(G.cursor),cr=rank_of(G.cursor);
                   if(cr<7) G.cursor=sq(cr+1,cf); break; }
        case 'j':{ int cf=file_of(G.cursor),cr=rank_of(G.cursor);
                   if(cr>0) G.cursor=sq(cr-1,cf); break; }
        case ' ':
        case '\n':
        case '\r':
            try_select_or_move(); break;
        case 'u': case 'U':
            if(G.engine_thinking){ engine_write("stop"); G.engine_thinking=0; }
            undo_move();
            if(G.hist_count>0&&G.eng_active) undo_move(); /* undo engine's reply too */
            G.selected=-1; G.nlegal=0;
            snprintf(G.status_msg,sizeof(G.status_msg),"Move undone");
            break;
        case 'e': case 'E':
            if(!G.game_over){
                engine_go();
                snprintf(G.status_msg,sizeof(G.status_msg),"Engine thinking...");
            }
            break;
        case 's': case 'S':
            G.screen_mode=1; G.settings_sel=0;
            break;
        case 'f': case 'F':
            G.flip_board^=1;
            snprintf(G.status_msg,sizeof(G.status_msg),"Board %s",G.flip_board?"flipped":"normal");
            break;
        case 'n': case 'N':
            if(G.engine_thinking){ engine_write("stop"); G.engine_thinking=0; }
            init_board(); G.selected=-1; G.nlegal=0;
            G.cursor=G.flip_board?63:0;
            snprintf(G.status_msg,sizeof(G.status_msg),"New game started");
            break;
        case 'q': case 'Q':
            show_cursor(); term_restore(); stop_engine();
            clear_screen(); move_to(1,1);
            printf("Thanks for playing! Goodbye.\n");
            exit(0);
        case 'a': /* 'a' for auto-play: engine plays for current side */
            if(!G.game_over&&G.eng_active&&!G.engine_thinking)
                engine_go();
            break;
    }
}

/* ─── Main loop ──────────────────────────────────────────────── */
int main(int argc, char **argv) {
    /* Init */
    init_zobrist();
    memset(&G,0,sizeof(G));
    G.selected=-1; G.last_from=-1; G.last_to=-1;
    G.depth_limit=10; G.movetime=2000; G.wtime=30000; G.btime=30000;
    G.winc=500; G.binc=500; G.use_clock=0; G.nodes_limit=0;
    G.screen_mode=0; G.settings_sel=0;
    G.cursor=0;

    if(argc>1){
        strncpy(G.engine_path,argv[1],511);
        if(!start_engine(G.engine_path)){
            fprintf(stderr,"Warning: Could not start engine: %s\n",argv[1]);
            G.engine_path[0]=0;
        }
    }
    init_board();

    term_raw();
    hide_cursor();
    clear_screen();

    snprintf(G.status_msg,sizeof(G.status_msg),
             argc>1?"Engine loaded. Press 'e' to get engine move.":
             "No engine. Press 's' for settings.");

    struct timespec last_render={0,0};

    while(1){
        /* Poll engine */
        int engine_moved=poll_engine();
        if(engine_moved) { /* full redraw */ }

        /* Throttle render to ~30fps */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        long elapsed_ms=(now.tv_sec-last_render.tv_sec)*1000L+
                        (now.tv_nsec-last_render.tv_nsec)/1000000L;
        if(elapsed_ms>=33||engine_moved){
            render();
            last_render=now;
        }

        /* Read input (non-blocking) */
        char k[8]={0};
        int n=read(STDIN_FILENO,k,7);
        if(n>0){
            handle_key(k,n);
        }

        usleep(10000); /* 10ms */
    }
    return 0;
}
