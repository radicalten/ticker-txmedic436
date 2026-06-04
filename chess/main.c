/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 *
 * Compile: gcc -o chess_gui chess_gui.c
 * Run:     ./chess_gui [uci_engine_path]
 *
 * Controls:
 *   Arrow keys    - Move cursor
 *   Enter/Space   - Select piece / confirm move
 *   U             - Undo last move
 *   Q             - Quit
 *   F             - Flip board
 *   N             - New game
 *   H             - Show help
 *
 * If no engine path is given, play Human vs Human.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>

/* ─────────────────────────────────────────────────────────────────────────── */
/*  ANSI / Terminal helpers                                                    */
/* ─────────────────────────────────────────────────────────────────────────── */

#define CLEAR_SCREEN    "\033[2J\033[H"
#define HIDE_CURSOR     "\033[?25l"
#define SHOW_CURSOR     "\033[?25h"
#define RESET_COLOR     "\033[0m"
#define BOLD            "\033[1m"

/* 256-color background/foreground */
#define BG(n)  "\033[48;5;" #n "m"
#define FG(n)  "\033[38;5;" #n "m"

/* Named colour indices */
#define COL_LIGHT_SQ      230   /* pale yellow */
#define COL_DARK_SQ       94    /* brown       */
#define COL_SELECTED      220   /* bright gold */
#define COL_CURSOR        214   /* orange      */
#define COL_LAST_MOVE     107   /* light green */
#define COL_VALID_MOVE    74    /* sky blue    */
#define COL_CHECK         196   /* red         */
#define COL_WHITE_PIECE   255   /* near white  */
#define COL_BLACK_PIECE   232   /* near black  */
#define COL_PANEL_BG      235   /* dark grey   */
#define COL_PANEL_FG      252   /* light grey  */
#define COL_TITLE_FG      220   /* gold        */
#define COL_PGN_FG        117   /* light blue  */
#define COL_BORDER        240   /* mid grey    */

static void set_bg(int n){ printf("\033[48;5;%dm", n); }
static void set_fg(int n){ printf("\033[38;5;%dm", n); }
static void move_to(int row, int col){ printf("\033[%d;%dH", row, col); }
static void save_cursor(void){ printf("\033[s"); }
static void restore_cursor(void){ printf("\033[u"); }

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Piece definitions                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef enum {
    EMPTY=0,
    W_PAWN=1, W_KNIGHT=2, W_BISHOP=3, W_ROOK=4, W_QUEEN=5, W_KING=6,
    B_PAWN=7, B_KNIGHT=8, B_BISHOP=9, B_ROOK=10,B_QUEEN=11,B_KING=12
} Piece;

#define IS_WHITE(p)  ((p)>=W_PAWN  && (p)<=W_KING)
#define IS_BLACK(p)  ((p)>=B_PAWN  && (p)<=B_KING)
#define IS_PIECE(p)  ((p)!=EMPTY)
#define PIECE_COLOR(p) (IS_WHITE(p)?0:1)   /* 0=white,1=black */
#define PIECE_TYPE(p)  (IS_WHITE(p)?(p):((p)-6))  /* 1-6 */

/* Unicode chess pieces (double-wide) */
static const char *PIECE_GLYPHS[13] = {
    "  ",   /* EMPTY     */
    "♙ ", "♘ ", "♗ ", "♖ ", "♕ ", "♔ ",  /* white */
    "♟ ", "♞ ", "♝ ", "♜ ", "♛ ", "♚ "   /* black */
};

static char PIECE_LETTERS[13] = {
    '.','P','N','B','R','Q','K','p','n','b','r','q','k'
};

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Board & game state                                                         */
/* ─────────────────────────────────────────────────────────────────────────── */

#define MAX_MOVES   512
#define MAX_HISTORY 512

typedef struct {
    int from_sq;       /* 0-63 */
    int to_sq;
    Piece moved;
    Piece captured;
    Piece promoted;    /* EMPTY if none */
    int castle_side;   /* 0=none,1=kingside,2=queenside */
    int ep_capture_sq; /* -1 if none */
    /* saved state for undo */
    int prev_ep_sq;
    int prev_castling; /* bitmask: 1=WK,2=WQ,4=BK,8=BQ */
    int prev_halfmove;
    /* PGN annotation */
    char pgn[16];
} Move;

typedef struct {
    Piece board[64];
    int   side;          /* 0=white,1=black to move */
    int   castling;      /* bitmask */
    int   ep_sq;         /* -1 or target square */
    int   halfmove;
    int   fullmove;
    Move  history[MAX_HISTORY];
    int   hist_count;
} GameState;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  UCI engine pipe state                                                      */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    pid_t pid;
    int   to_engine[2];    /* parent writes [1], engine reads [0]  */
    int   from_engine[2];  /* engine writes [1], parent reads [0]  */
    int   active;
    char  name[128];
    char  bestmove[16];
    int   thinking;
} Engine;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  UI state                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    int cursor_row;   /* 0-7, board row (visual) */
    int cursor_col;   /* 0-7, board col (visual) */
    int selected;     /* -1 or square index       */
    int flipped;      /* 1 = black at bottom      */
    int valid_moves[64]; /* bitmask of valid targets */
    int show_help;
    int message_timer;
    char message[128];
    int engine_color; /* 0=white,1=black,-1=none */
    int game_over;
    char game_result[64];
} UIState;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Globals                                                                    */
/* ─────────────────────────────────────────────────────────────────────────── */

static GameState  G;
static UIState    UI;
static Engine     ENG;
static struct termios ORIG_TERM;
static int        RUNNING = 1;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Terminal raw mode                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

static void term_raw(void){
    struct termios raw;
    tcgetattr(STDIN_FILENO, &ORIG_TERM);
    raw = ORIG_TERM;
    raw.c_lflag &= ~(ECHO|ICANON|ISIG);
    raw.c_iflag &= ~(IXON|ICRNL);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
static void term_restore(void){
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &ORIG_TERM);
    printf(SHOW_CURSOR RESET_COLOR "\n");
    fflush(stdout);
}

static void cleanup(void){
    term_restore();
    if(ENG.active){
        write(ENG.to_engine[1],"quit\n",5);
        close(ENG.to_engine[1]);
        close(ENG.from_engine[0]);
    }
}

static void sig_handler(int s){ (void)s; RUNNING=0; cleanup(); exit(0); }

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Square helpers                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

#define SQ(r,c)  ((r)*8+(c))
#define ROW(sq)  ((sq)/8)
#define COL(sq)  ((sq)%8)

/* visual row/col → board square (accounting for flip) */
static int visual_to_sq(int vr, int vc){
    int r = UI.flipped ? vr : (7-vr);
    int c = UI.flipped ? (7-vc) : vc;
    return SQ(r,c);
}

/* board square → visual row,col */
static void sq_to_visual(int sq, int *vr, int *vc){
    int r=ROW(sq), c=COL(sq);
    *vr = UI.flipped ? r     : (7-r);
    *vc = UI.flipped ? (7-c) : c;
}

static char *sq_name(int sq, char buf[3]){
    buf[0]='a'+COL(sq);
    buf[1]='1'+ROW(sq);
    buf[2]='\0';
    return buf;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Move generation                                                            */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Ray directions */
static const int ROOK_DIRS[4]   = {8,-8,1,-1};
static const int BISHOP_DIRS[4] = {9,-9,7,-7};
static const int KNIGHT_OFFS[8] = {17,15,10,6,-6,-10,-15,-17};

static int is_on_board(int sq){ return sq>=0 && sq<64; }

/* check if sliding move crosses board edge */
static int ray_step_ok(int from, int to, int dir){
    if(!is_on_board(to)) return 0;
    /* prevent wrap-around for horizontal dirs */
    if(dir==1 || dir==-1){
        if(ROW(from)!=ROW(to)) return 0;
    }
    if(dir==7 || dir==-7 || dir==9 || dir==-9){
        /* diagonal: col difference must be 1 */
        if(abs(COL(to)-COL(from))!=1) return 0;
    }
    return 1;
}

static int knight_move_ok(int from, int to){
    if(!is_on_board(to)) return 0;
    int dr=abs(ROW(to)-ROW(from)), dc=abs(COL(to)-COL(from));
    return (dr==2&&dc==1)||(dr==1&&dc==2);
}

/* returns 1 if sq attacked by 'attacker_color' (0=white,1=black) */
static int is_attacked(const Piece board[64], int sq, int attacker_color){
    /* Pawns */
    Piece apawn = attacker_color ? B_PAWN : W_PAWN;
    int pdir    = attacker_color ? -1 : 1; /* row direction of pawn attack */
    for(int dc=-1;dc<=1;dc+=2){
        int r=ROW(sq)-pdir, c=COL(sq)+dc;
        if(r>=0&&r<8&&c>=0&&c<8 && board[SQ(r,c)]==apawn) return 1;
    }
    /* Knights */
    Piece aknight = attacker_color ? B_KNIGHT : W_KNIGHT;
    for(int i=0;i<8;i++){
        int t=sq+KNIGHT_OFFS[i];
        if(knight_move_ok(sq,t) && board[t]==aknight) return 1;
    }
    /* Bishops/Queens (diagonals) */
    Piece abishop = attacker_color ? B_BISHOP : W_BISHOP;
    Piece aqueen  = attacker_color ? B_QUEEN  : W_QUEEN;
    for(int i=0;i<4;i++){
        int cur=sq;
        while(1){
            int nxt=cur+BISHOP_DIRS[i];
            if(!ray_step_ok(cur,nxt,BISHOP_DIRS[i])) break;
            if(board[nxt]!=EMPTY){
                if(board[nxt]==abishop||board[nxt]==aqueen) return 1;
                break;
            }
            cur=nxt;
        }
    }
    /* Rooks/Queens (straights) */
    Piece arook = attacker_color ? B_ROOK : W_ROOK;
    for(int i=0;i<4;i++){
        int cur=sq;
        while(1){
            int nxt=cur+ROOK_DIRS[i];
            if(!ray_step_ok(cur,nxt,ROOK_DIRS[i])) break;
            if(board[nxt]!=EMPTY){
                if(board[nxt]==arook||board[nxt]==aqueen) return 1;
                break;
            }
            cur=nxt;
        }
    }
    /* King */
    Piece aking = attacker_color ? B_KING : W_KING;
    for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++){
        if(!dr&&!dc) continue;
        int r=ROW(sq)+dr,c=COL(sq)+dc;
        if(r>=0&&r<8&&c>=0&&c<8&&board[SQ(r,c)]==aking) return 1;
    }
    return 0;
}

static int find_king(const Piece board[64], int color){
    Piece k = color ? B_KING : W_KING;
    for(int i=0;i<64;i++) if(board[i]==k) return i;
    return -1;
}

static int in_check(const Piece board[64], int color){
    int ksq=find_king(board,color);
    if(ksq<0) return 0;
    return is_attacked(board,ksq,1-color);
}

/* Try a pseudo-legal move; return 1 if king not in check after it */
static int try_move(Piece board[64], int from, int to, Piece promo, int ep_sq){
    Piece tmp[64];
    memcpy(tmp,board,sizeof(tmp));
    int color = IS_WHITE(tmp[from]) ? 0 : 1;
    Piece moved=tmp[from];

    /* en-passant capture */
    if((moved==W_PAWN||moved==B_PAWN) && to==ep_sq && ep_sq>=0){
        int cap_row = ROW(to)+(color?1:-1);
        tmp[SQ(cap_row,COL(to))]=EMPTY;
    }
    tmp[to]= promo!=EMPTY ? promo : moved;
    tmp[from]=EMPTY;

    /* castling: move rook */
    if((moved==W_KING||moved==B_KING) && abs(COL(to)-COL(from))==2){
        int row=ROW(from);
        if(COL(to)==6){ tmp[SQ(row,5)]=tmp[SQ(row,7)]; tmp[SQ(row,7)]=EMPTY; }
        else           { tmp[SQ(row,3)]=tmp[SQ(row,0)]; tmp[SQ(row,0)]=EMPTY; }
    }
    return !in_check(tmp,color);
}

typedef struct { int from,to; Piece promo; } PseudoMove;

static int gen_pseudo(const GameState *g, int from, PseudoMove out[], int max_out){
    int cnt=0;
    Piece p=g->board[from];
    if(p==EMPTY) return 0;
    int color=IS_WHITE(p)?0:1;
    if(color!=g->side) return 0;

    int r=ROW(from),c=COL(from);

#define ADD(t,pr) do{ if(cnt<max_out){ out[cnt].from=from; out[cnt].to=(t); out[cnt].promo=(pr); cnt++; } }while(0)

    if(p==W_PAWN){
        /* one forward */
        if(r<7 && g->board[SQ(r+1,c)]==EMPTY){
            if(r+1==7){ ADD(SQ(r+1,c),W_QUEEN); ADD(SQ(r+1,c),W_ROOK); ADD(SQ(r+1,c),W_BISHOP); ADD(SQ(r+1,c),W_KNIGHT); }
            else ADD(SQ(r+1,c),EMPTY);
            /* two forward */
            if(r==1 && g->board[SQ(r+2,c)]==EMPTY) ADD(SQ(r+2,c),EMPTY);
        }
        /* captures */
        for(int dc=-1;dc<=1;dc+=2){
            int nc=c+dc;
            if(nc<0||nc>7) continue;
            int t=SQ(r+1,nc);
            if(IS_BLACK(g->board[t])){
                if(r+1==7){ ADD(t,W_QUEEN); ADD(t,W_ROOK); ADD(t,W_BISHOP); ADD(t,W_KNIGHT); }
                else ADD(t,EMPTY);
            }
            if(t==g->ep_sq) ADD(t,EMPTY);
        }
    } else if(p==B_PAWN){
        if(r>0 && g->board[SQ(r-1,c)]==EMPTY){
            if(r-1==0){ ADD(SQ(r-1,c),B_QUEEN); ADD(SQ(r-1,c),B_ROOK); ADD(SQ(r-1,c),B_BISHOP); ADD(SQ(r-1,c),B_KNIGHT); }
            else ADD(SQ(r-1,c),EMPTY);
            if(r==6 && g->board[SQ(r-2,c)]==EMPTY) ADD(SQ(r-2,c),EMPTY);
        }
        for(int dc=-1;dc<=1;dc+=2){
            int nc=c+dc;
            if(nc<0||nc>7) continue;
            int t=SQ(r-1,nc);
            if(IS_WHITE(g->board[t])){
                if(r-1==0){ ADD(t,B_QUEEN); ADD(t,B_ROOK); ADD(t,B_BISHOP); ADD(t,B_KNIGHT); }
                else ADD(t,EMPTY);
            }
            if(t==g->ep_sq) ADD(t,EMPTY);
        }
    } else if(p==W_KNIGHT||p==B_KNIGHT){
        for(int i=0;i<8;i++){
            int t=from+KNIGHT_OFFS[i];
            if(!knight_move_ok(from,t)) continue;
            if(g->board[t]==EMPTY||(color==0&&IS_BLACK(g->board[t]))||(color==1&&IS_WHITE(g->board[t])))
                ADD(t,EMPTY);
        }
    } else if(p==W_BISHOP||p==B_BISHOP||p==W_QUEEN||p==B_QUEEN||p==W_ROOK||p==B_ROOK){
        int is_diag = (p==W_BISHOP||p==B_BISHOP||p==W_QUEEN||p==B_QUEEN);
        int is_str  = (p==W_ROOK  ||p==B_ROOK  ||p==W_QUEEN||p==B_QUEEN);
        const int *dirs[2]={BISHOP_DIRS,ROOK_DIRS};
        for(int d=0;d<2;d++){
            if(d==0&&!is_diag) continue;
            if(d==1&&!is_str)  continue;
            for(int i=0;i<4;i++){
                int cur=from;
                while(1){
                    int nxt=cur+dirs[d][i];
                    if(!ray_step_ok(cur,nxt,dirs[d][i])) break;
                    if(g->board[nxt]==EMPTY){ ADD(nxt,EMPTY); cur=nxt; }
                    else {
                        if((color==0&&IS_BLACK(g->board[nxt]))||(color==1&&IS_WHITE(g->board[nxt])))
                            ADD(nxt,EMPTY);
                        break;
                    }
                }
            }
        }
    } else if(p==W_KING||p==B_KING){
        for(int dr=-1;dr<=1;dr++) for(int dc2=-1;dc2<=1;dc2++){
            if(!dr&&!dc2) continue;
            int nr=r+dr,nc2=c+dc2;
            if(nr<0||nr>7||nc2<0||nc2>7) continue;
            int t=SQ(nr,nc2);
            if(g->board[t]==EMPTY||(color==0&&IS_BLACK(g->board[t]))||(color==1&&IS_WHITE(g->board[t])))
                ADD(t,EMPTY);
        }
        /* Castling */
        if(color==0){
            if((g->castling&1) && g->board[SQ(0,5)]==EMPTY && g->board[SQ(0,6)]==EMPTY
               && !is_attacked(g->board,SQ(0,4),1) && !is_attacked(g->board,SQ(0,5),1)
               && !is_attacked(g->board,SQ(0,6),1))
                ADD(SQ(0,6),EMPTY);
            if((g->castling&2) && g->board[SQ(0,3)]==EMPTY && g->board[SQ(0,2)]==EMPTY && g->board[SQ(0,1)]==EMPTY
               && !is_attacked(g->board,SQ(0,4),1) && !is_attacked(g->board,SQ(0,3),1)
               && !is_attacked(g->board,SQ(0,2),1))
                ADD(SQ(0,2),EMPTY);
        } else {
            if((g->castling&4) && g->board[SQ(7,5)]==EMPTY && g->board[SQ(7,6)]==EMPTY
               && !is_attacked(g->board,SQ(7,4),0) && !is_attacked(g->board,SQ(7,5),0)
               && !is_attacked(g->board,SQ(7,6),0))
                ADD(SQ(7,6),EMPTY);
            if((g->castling&8) && g->board[SQ(7,3)]==EMPTY && g->board[SQ(7,2)]==EMPTY && g->board[SQ(7,1)]==EMPTY
               && !is_attacked(g->board,SQ(7,4),0) && !is_attacked(g->board,SQ(7,3),0)
               && !is_attacked(g->board,SQ(7,2),0))
                ADD(SQ(7,2),EMPTY);
        }
    }
#undef ADD
    return cnt;
}

/* Generate legal moves from a square; fill UI.valid_moves */
static void compute_valid_moves(int from){
    memset(UI.valid_moves,0,sizeof(UI.valid_moves));
    if(from<0) return;
    PseudoMove pm[256];
    int n=gen_pseudo(&G,from,pm,256);
    for(int i=0;i<n;i++){
        if(try_move(G.board,pm[i].from,pm[i].to,pm[i].promo,G.ep_sq))
            UI.valid_moves[pm[i].to]=1;
    }
}

/* Count all legal moves for current side */
static int count_legal_moves(void){
    int total=0;
    PseudoMove pm[256];
    for(int sq=0;sq<64;sq++){
        Piece p=G.board[sq];
        if(p==EMPTY) continue;
        if(IS_WHITE(p)&&G.side!=0) continue;
        if(IS_BLACK(p)&&G.side!=1) continue;
        int n=gen_pseudo(&G,sq,pm,256);
        for(int i=0;i<n;i++)
            if(try_move(G.board,pm[i].from,pm[i].to,pm[i].promo,G.ep_sq))
                total++;
    }
    return total;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  PGN helpers                                                                */
/* ─────────────────────────────────────────────────────────────────────────── */

static int is_ambiguous(int from, int to, Piece moved){
    /* Check if another piece of same type can move to same square */
    PseudoMove pm[256];
    for(int sq=0;sq<64;sq++){
        if(sq==from) continue;
        if(G.board[sq]!=moved) continue;
        int n=gen_pseudo(&G,sq,pm,256);
        for(int i=0;i<n;i++){
            if(pm[i].to==to && try_move(G.board,sq,to,pm[i].promo,G.ep_sq))
                return 1;
        }
    }
    return 0;
}

static void build_pgn(Move *m, int gives_check, int gives_mate){
    char buf[16]="";
    char fsn[3], tsn[3];
    sq_name(m->from_sq,fsn);
    sq_name(m->to_sq,tsn);
    Piece moved=m->moved;
    int type=PIECE_TYPE(moved);

    if(m->castle_side==1){ strcpy(buf,"O-O"); }
    else if(m->castle_side==2){ strcpy(buf,"O-O-O"); }
    else {
        int pos=0;
        if(type!=1){ /* not pawn */
            buf[pos++]=toupper(PIECE_LETTERS[type]);
            /* disambiguate */
            int amb=is_ambiguous(m->from_sq,m->to_sq,moved);
            if(amb){
                /* try file first */
                int file_uniq=1;
                PseudoMove pm[256];
                for(int sq=0;sq<64;sq++){
                    if(sq==m->from_sq) continue;
                    if(G.board[sq]!=moved) continue;
                    if(COL(sq)==COL(m->from_sq)){
                        int n=gen_pseudo(&G,sq,pm,256);
                        for(int i=0;i<n;i++)
                            if(pm[i].to==m->to_sq && try_move(G.board,sq,m->to_sq,pm[i].promo,G.ep_sq))
                                file_uniq=0;
                    }
                }
                if(file_uniq) buf[pos++]=fsn[0];
                else { buf[pos++]=fsn[0]; buf[pos++]=fsn[1]; }
            }
        } else {
            if(m->captured!=EMPTY || m->ep_capture_sq>=0)
                buf[pos++]=fsn[0];
        }
        if(m->captured!=EMPTY || m->ep_capture_sq>=0) buf[pos++]='x';
        buf[pos++]=tsn[0]; buf[pos++]=tsn[1];
        if(m->promoted!=EMPTY)
            buf[pos++]=toupper(PIECE_LETTERS[PIECE_TYPE(m->promoted)]);
        buf[pos]='\0';
    }
    if(gives_mate) strcat(buf,"#");
    else if(gives_check) strcat(buf,"+");
    strncpy(m->pgn,buf,15);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Apply / undo move                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

static void init_board(void){
    memset(&G,0,sizeof(G));
    /* White pieces */
    G.board[SQ(0,0)]=W_ROOK;  G.board[SQ(0,1)]=W_KNIGHT; G.board[SQ(0,2)]=W_BISHOP;
    G.board[SQ(0,3)]=W_QUEEN; G.board[SQ(0,4)]=W_KING;   G.board[SQ(0,5)]=W_BISHOP;
    G.board[SQ(0,6)]=W_KNIGHT;G.board[SQ(0,7)]=W_ROOK;
    for(int c=0;c<8;c++) G.board[SQ(1,c)]=W_PAWN;
    /* Black pieces */
    G.board[SQ(7,0)]=B_ROOK;  G.board[SQ(7,1)]=B_KNIGHT; G.board[SQ(7,2)]=B_BISHOP;
    G.board[SQ(7,3)]=B_QUEEN; G.board[SQ(7,4)]=B_KING;   G.board[SQ(7,5)]=B_BISHOP;
    G.board[SQ(7,6)]=B_KNIGHT;G.board[SQ(7,7)]=B_ROOK;
    for(int c=0;c<8;c++) G.board[SQ(6,c)]=B_PAWN;
    G.side=0; G.castling=15; G.ep_sq=-1; G.halfmove=0; G.fullmove=1;
    G.hist_count=0;
}

/* Returns 1 if applied, 0 if illegal */
static int apply_move(int from, int to, Piece promo){
    if(!try_move(G.board,from,to,promo,G.ep_sq)) return 0;

    Move m;
    memset(&m,0,sizeof(m));
    m.from_sq=from; m.to_sq=to;
    m.moved=G.board[from];
    m.captured=G.board[to];
    m.promoted=promo;
    m.ep_capture_sq=-1;
    m.prev_ep_sq=G.ep_sq;
    m.prev_castling=G.castling;
    m.prev_halfmove=G.halfmove;
    m.castle_side=0;

    int color=IS_WHITE(m.moved)?0:1;

    /* en-passant capture */
    if((m.moved==W_PAWN||m.moved==B_PAWN) && to==G.ep_sq && G.ep_sq>=0){
        int cap_row=ROW(to)+(color?1:-1);
        m.ep_capture_sq=SQ(cap_row,COL(to));
        m.captured=G.board[m.ep_capture_sq];
        G.board[m.ep_capture_sq]=EMPTY;
    }

    /* castling detection */
    if((m.moved==W_KING||m.moved==B_KING) && abs(COL(to)-COL(from))==2){
        m.castle_side=(COL(to)==6)?1:2;
        int row=ROW(from);
        if(m.castle_side==1){ G.board[SQ(row,5)]=G.board[SQ(row,7)]; G.board[SQ(row,7)]=EMPTY; }
        else                 { G.board[SQ(row,3)]=G.board[SQ(row,0)]; G.board[SQ(row,0)]=EMPTY; }
    }

    /* place piece */
    G.board[to]= (promo!=EMPTY) ? promo : m.moved;
    G.board[from]=EMPTY;

    /* update castling rights */
    if(m.moved==W_KING) G.castling &= ~3;
    if(m.moved==B_KING) G.castling &= ~12;
    if(from==SQ(0,0)||to==SQ(0,0)) G.castling &= ~2;
    if(from==SQ(0,7)||to==SQ(0,7)) G.castling &= ~1;
    if(from==SQ(7,0)||to==SQ(7,0)) G.castling &= ~8;
    if(from==SQ(7,7)||to==SQ(7,7)) G.castling &= ~4;

    /* en-passant square */
    G.ep_sq=-1;
    if((m.moved==W_PAWN||m.moved==B_PAWN) && abs(ROW(to)-ROW(from))==2)
        G.ep_sq=SQ((ROW(from)+ROW(to))/2,COL(from));

    /* halfmove clock */
    if(m.captured!=EMPTY||m.moved==W_PAWN||m.moved==B_PAWN) G.halfmove=0;
    else G.halfmove++;

    /* switch side */
    G.side=1-G.side;
    if(G.side==0) G.fullmove++;

    /* PGN (needs to be done BEFORE side switch for ambiguity but AFTER
       for check detection — we cheat slightly and build after switch) */
    int chk=in_check(G.board,G.side);
    int lm=count_legal_moves();
    int mat=(lm==0&&chk);
    build_pgn(&m, chk, mat);

    if(G.hist_count<MAX_HISTORY)
        G.history[G.hist_count++]=m;

    /* game over? */
    if(lm==0){
        if(chk) snprintf(UI.game_result,sizeof(UI.game_result),
                          "%s wins by checkmate!", color==0?"White":"Black");
        else    snprintf(UI.game_result,sizeof(UI.game_result),"Draw by stalemate!");
        UI.game_over=1;
    } else if(G.halfmove>=100){
        snprintf(UI.game_result,sizeof(UI.game_result),"Draw by 50-move rule!");
        UI.game_over=1;
    }
    return 1;
}

static void undo_move(void){
    if(G.hist_count==0) return;
    Move *m=&G.history[--G.hist_count];

    /* restore side */
    G.side=1-G.side;
    if(G.side==1) G.fullmove--;

    /* restore piece */
    G.board[m->from_sq]=m->moved;
    G.board[m->to_sq]=m->captured;

    /* un-promote */
    if(m->promoted!=EMPTY) G.board[m->from_sq]=m->moved;

    /* restore ep capture */
    if(m->ep_capture_sq>=0){
        G.board[m->ep_capture_sq]=(G.side==0)?B_PAWN:W_PAWN;
        G.board[m->to_sq]=EMPTY;
    }

    /* un-castle rook */
    if(m->castle_side==1){
        int row=ROW(m->from_sq);
        G.board[SQ(row,7)]=G.board[SQ(row,5)]; G.board[SQ(row,5)]=EMPTY;
    } else if(m->castle_side==2){
        int row=ROW(m->from_sq);
        G.board[SQ(row,0)]=G.board[SQ(row,3)]; G.board[SQ(row,3)]=EMPTY;
    }

    G.ep_sq=m->prev_ep_sq;
    G.castling=m->prev_castling;
    G.halfmove=m->prev_halfmove;
    UI.game_over=0;
    UI.game_result[0]='\0';
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  FEN builder (for UCI engine)                                               */
/* ─────────────────────────────────────────────────────────────────────────── */

static void build_fen(char *buf, int bufsz){
    char pos[128]="";
    int pos_len=0;
    for(int r=7;r>=0;r--){
        int empty=0;
        for(int c=0;c<8;c++){
            Piece p=G.board[SQ(r,c)];
            if(p==EMPTY){ empty++; }
            else {
                if(empty){ pos[pos_len++]='0'+empty; empty=0; }
                pos[pos_len++]=PIECE_LETTERS[p];
            }
        }
        if(empty) pos[pos_len++]='0'+empty;
        if(r>0) pos[pos_len++]='/';
    }
    pos[pos_len]='\0';

    char cast[5]="";
    int ci=0;
    if(G.castling&1) cast[ci++]='K';
    if(G.castling&2) cast[ci++]='Q';
    if(G.castling&4) cast[ci++]='k';
    if(G.castling&8) cast[ci++]='q';
    if(ci==0){ cast[0]='-'; cast[1]='\0'; }
    else cast[ci]='\0';

    char ep[3]="-";
    if(G.ep_sq>=0) sq_name(G.ep_sq,ep);

    snprintf(buf,bufsz,"%s %c %s %s %d %d",
             pos, G.side?'b':'w', cast, ep, G.halfmove, G.fullmove);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  UCI engine                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

static void engine_write(const char *s){
    if(!ENG.active) return;
    write(ENG.to_engine[1],s,strlen(s));
    write(ENG.to_engine[1],"\n",1);
}

static int engine_launch(const char *path){
    if(pipe(ENG.to_engine)<0||pipe(ENG.from_engine)<0) return 0;
    ENG.pid=fork();
    if(ENG.pid<0) return 0;
    if(ENG.pid==0){
        /* child */
        dup2(ENG.to_engine[0],STDIN_FILENO);
        dup2(ENG.from_engine[1],STDOUT_FILENO);
        close(ENG.to_engine[1]); close(ENG.from_engine[0]);
        close(ENG.to_engine[0]); close(ENG.from_engine[1]);
        execlp(path,path,(char*)NULL);
        exit(1);
    }
    close(ENG.to_engine[0]);
    close(ENG.from_engine[1]);
    /* set non-blocking */
    int flags=fcntl(ENG.from_engine[0],F_GETFL);
    fcntl(ENG.from_engine[0],F_SETFL,flags|O_NONBLOCK);
    ENG.active=1;
    ENG.thinking=0;
    ENG.bestmove[0]='\0';

    engine_write("uci");
    /* read until uciok with timeout */
    char line[512];
    char ibuf[4096]="";
    time_t start=time(NULL);
    while(time(NULL)-start<5){
        int n=read(ENG.from_engine[0],ibuf+strlen(ibuf),sizeof(ibuf)-strlen(ibuf)-1);
        if(n>0){
            ibuf[strlen(ibuf)]='\0';
            if(strstr(ibuf,"uciok")){
                /* grab engine name */
                char *np=strstr(ibuf,"id name ");
                if(np){
                    np+=8;
                    char *nl=strchr(np,'\n');
                    int len= nl ? (int)(nl-np) : (int)strlen(np);
                    if(len>127) len=127;
                    strncpy(ENG.name,np,len);
                    ENG.name[len]='\0';
                }
                break;
            }
        }
        usleep(50000);
    }
    if(!ENG.name[0]) strcpy(ENG.name,"UCI Engine");
    engine_write("isready");
    /* wait readyok */
    start=time(NULL);
    memset(ibuf,0,sizeof(ibuf));
    while(time(NULL)-start<5){
        int n=read(ENG.from_engine[0],ibuf+strlen(ibuf),sizeof(ibuf)-strlen(ibuf)-1);
        if(n>0 && strstr(ibuf,"readyok")) break;
        usleep(50000);
    }
    engine_write("ucinewgame");
    return 1;
}

static void engine_go(void){
    if(!ENG.active) return;
    char fen[256];
    build_fen(fen,sizeof(fen));

    /* Build moves string */
    char moves[4096]="";
    for(int i=0;i<G.hist_count;i++){
        Move *m=&G.history[i];
        char fsn[3],tsn[3];
        sq_name(m->from_sq,fsn); sq_name(m->to_sq,tsn);
        char mv[8];
        if(m->promoted!=EMPTY){
            char pc=tolower(PIECE_LETTERS[PIECE_TYPE(m->promoted)]);
            snprintf(mv,sizeof(mv),"%s%s%c",fsn,tsn,pc);
        } else {
            snprintf(mv,sizeof(mv),"%s%s",fsn,tsn);
        }
        if(i>0) strcat(moves," ");
        strcat(moves,mv);
    }

    char cmd[4096];
    if(moves[0])
        snprintf(cmd,sizeof(cmd),"position fen %s moves %s",fen,moves);
    else
        snprintf(cmd,sizeof(cmd),"position fen %s",fen);

    /* Use startpos if initial position */
    if(G.hist_count==0 && strcmp(fen,"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")==0)
        strcpy(cmd,"position startpos");

    engine_write(cmd);
    engine_write("go movetime 1000");
    ENG.thinking=1;
    ENG.bestmove[0]='\0';
}

/* Poll engine for bestmove; returns 1 if move received */
static int engine_poll(void){
    if(!ENG.active||!ENG.thinking) return 0;
    static char ibuf[8192]="";
    int n=read(ENG.from_engine[0],ibuf+strlen(ibuf),sizeof(ibuf)-strlen(ibuf)-1);
    if(n>0){
        ibuf[strlen(ibuf)]='\0';
        char *p=strstr(ibuf,"bestmove");
        if(p){
            char bm[16]="";
            sscanf(p,"bestmove %15s",bm);
            strncpy(ENG.bestmove,bm,15);
            ENG.thinking=0;
            /* consume buffer */
            memset(ibuf,0,sizeof(ibuf));
            return 1;
        }
    }
    return 0;
}

/* Parse engine bestmove and apply */
static void engine_apply_bestmove(void){
    const char *bm=ENG.bestmove;
    if(!bm[0]||strcmp(bm,"(none)")==0) return;
    int fc=bm[0]-'a', fr=bm[1]-'1';
    int tc=bm[2]-'a', tr=bm[3]-'1';
    int from=SQ(fr,fc), to=SQ(tr,tc);
    Piece promo=EMPTY;
    if(bm[4]){
        char pc=tolower(bm[4]);
        if(G.side==0){
            if(pc=='q') promo=W_QUEEN;
            else if(pc=='r') promo=W_ROOK;
            else if(pc=='b') promo=W_BISHOP;
            else if(pc=='n') promo=W_KNIGHT;
        } else {
            if(pc=='q') promo=B_QUEEN;
            else if(pc=='r') promo=B_ROOK;
            else if(pc=='b') promo=B_BISHOP;
            else if(pc=='n') promo=B_KNIGHT;
        }
    }
    apply_move(from,to,promo);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Rendering                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

#define BOARD_ORIGIN_ROW  2
#define BOARD_ORIGIN_COL  3
#define SQ_H              2   /* rows per square */
#define SQ_W              4   /* cols per square */
#define PANEL_COL        40   /* right panel start */

static void render_border(void){
    set_bg(COL_PANEL_BG);
    set_fg(COL_BORDER);
    /* top */
    move_to(BOARD_ORIGIN_ROW-1, BOARD_ORIGIN_COL-2);
    printf("  ╔");
    for(int c=0;c<8*SQ_W;c++) printf("═");
    printf("╗");
    /* bottom */
    move_to(BOARD_ORIGIN_ROW+8*SQ_H, BOARD_ORIGIN_COL-2);
    printf("  ╚");
    for(int c=0;c<8*SQ_W;c++) printf("═");
    printf("╝");
    /* sides */
    for(int r=0;r<8*SQ_H;r++){
        move_to(BOARD_ORIGIN_ROW+r, BOARD_ORIGIN_COL-4);
        set_fg(COL_BORDER);
        printf("  ║");
        move_to(BOARD_ORIGIN_ROW+r, BOARD_ORIGIN_COL+8*SQ_W);
        printf("║");
    }
    printf(RESET_COLOR);
}

static void render_board(void){
    /* last move squares */
    int lm_from=-1, lm_to=-1;
    if(G.hist_count>0){
        lm_from=G.history[G.hist_count-1].from_sq;
        lm_to  =G.history[G.hist_count-1].to_sq;
    }

    /* find king in check */
    int chk_sq=-1;
    if(in_check(G.board,G.side)) chk_sq=find_king(G.board,G.side);

    for(int vr=0;vr<8;vr++){
        for(int vc=0;vc<8;vc++){
            int sq=visual_to_sq(vr,vc);
            int r=ROW(sq),c=COL(sq);
            int light=((r+c)%2==0);

            /* choose background */
            int bg;
            if(sq==chk_sq)                       bg=COL_CHECK;
            else if(UI.selected==sq)              bg=COL_SELECTED;
            else if(UI.valid_moves[sq])           bg=COL_VALID_MOVE;
            else if(sq==lm_from||sq==lm_to)      bg=COL_LAST_MOVE;
            else                                  bg=light?COL_LIGHT_SQ:COL_DARK_SQ;

            /* cursor highlight (blend) */
            int is_cursor=(vr==UI.cursor_row&&vc==UI.cursor_col);
            if(is_cursor && UI.selected!=sq){
                bg=(bg==COL_LIGHT_SQ||bg==COL_DARK_SQ) ? COL_CURSOR : bg;
            }

            Piece p=G.board[sq];
            int fg=IS_WHITE(p)?COL_WHITE_PIECE:COL_BLACK_PIECE;
            if(p==EMPTY) fg=bg;

            for(int sub=0;sub<SQ_H;sub++){
                int row=BOARD_ORIGIN_ROW+vr*SQ_H+sub;
                int col=BOARD_ORIGIN_COL+vc*SQ_W;
                move_to(row,col);
                set_bg(bg);
                if(sub==SQ_H/2 && p!=EMPTY){
                    set_fg(fg);
                    /* cursor border */
                    if(is_cursor){
                        set_fg(COL_CURSOR);
                        printf("▐");
                        set_fg(fg);
                        printf("%s",PIECE_GLYPHS[p]);
                        set_fg(COL_CURSOR);
                        printf("▌");
                    } else {
                        printf(" %s ",PIECE_GLYPHS[p]);
                    }
                } else {
                    if(is_cursor && sub==0){
                        set_fg(COL_CURSOR);
                        printf("┌──┐");
                    } else if(is_cursor && sub==SQ_H-1){
                        set_fg(COL_CURSOR);
                        printf("└──┘");
                    } else {
                        printf("    ");
                    }
                }
            }
        }
    }

    /* rank labels */
    for(int vr=0;vr<8;vr++){
        int r=UI.flipped?vr:(7-vr);
        move_to(BOARD_ORIGIN_ROW+vr*SQ_H+SQ_H/2, BOARD_ORIGIN_COL-3);
        set_bg(COL_PANEL_BG);
        set_fg(COL_PANEL_FG);
        printf("%d ", r+1);
    }
    /* file labels */
    for(int vc=0;vc<8;vc++){
        int c=UI.flipped?(7-vc):vc;
        move_to(BOARD_ORIGIN_ROW+8*SQ_H+1, BOARD_ORIGIN_COL+vc*SQ_W+1);
        set_bg(COL_PANEL_BG);
        set_fg(COL_PANEL_FG);
        printf(" %c  ",'a'+c);
    }
    printf(RESET_COLOR);
}

static void render_pgn(void){
    int panel_row=BOARD_ORIGIN_ROW;
    int panel_col=PANEL_COL;
    int width=38;

    set_bg(COL_PANEL_BG);
    set_fg(COL_TITLE_FG);
    move_to(panel_row, panel_col);
    printf(BOLD "┌─ Move List ");
    set_fg(COL_BORDER);
    for(int i=13;i<width;i++) printf("─");
    printf("┐");
    printf(RESET_COLOR);

    /* print moves, 2 per line */
    int max_lines=16;
    int start_move=0;
    int total_pairs=(G.hist_count+1)/2;
    if(total_pairs>max_lines) start_move=total_pairs-max_lines;

    for(int line=0;line<max_lines;line++){
        move_to(panel_row+1+line, panel_col);
        set_bg(COL_PANEL_BG);
        set_fg(COL_BORDER);
        printf("│");
        int pair=start_move+line;
        if(pair<total_pairs){
            set_fg(COL_PANEL_FG);
            printf("%3d. ",pair+1);
            set_fg(COL_PGN_FG);
            /* white move */
            int wi=pair*2;
            if(wi<G.hist_count){
                char s[20];
                snprintf(s,sizeof(s),"%-8s",G.history[wi].pgn);
                /* highlight last move */
                if(wi==G.hist_count-1){ printf(BOLD); set_fg(COL_TITLE_FG); }
                printf("%s",s);
                printf(RESET_COLOR);
                set_bg(COL_PANEL_BG);
            } else printf("        ");
            /* black move */
            set_fg(COL_PGN_FG);
            int bi=pair*2+1;
            if(bi<G.hist_count){
                char s[20];
                snprintf(s,sizeof(s),"%-8s",G.history[bi].pgn);
                if(bi==G.hist_count-1){ printf(BOLD); set_fg(COL_TITLE_FG); }
                printf("%s",s);
                printf(RESET_COLOR);
                set_bg(COL_PANEL_BG);
            } else printf("        ");
            /* pad */
            set_fg(COL_PANEL_FG);
            printf("          ");
        } else {
            printf("%-*s",(int)width-2,"");
        }
        set_fg(COL_BORDER);
        printf("│");
    }

    /* bottom bar */
    move_to(panel_row+1+max_lines, panel_col);
    set_bg(COL_PANEL_BG);
    set_fg(COL_BORDER);
    printf("└");
    for(int i=0;i<width-2;i++) printf("─");
    printf("┘");
    printf(RESET_COLOR);
}

static void render_info(void){
    int panel_row=BOARD_ORIGIN_ROW+18;
    int panel_col=PANEL_COL;

    set_bg(COL_PANEL_BG);
    set_fg(COL_TITLE_FG);
    move_to(panel_row, panel_col);
    printf(BOLD "┌─ Game Info ");
    set_fg(COL_BORDER);
    for(int i=13;i<38;i++) printf("─");
    printf("┐");
    printf(RESET_COLOR);

    /* engine info */
    move_to(panel_row+1, panel_col);
    set_bg(COL_PANEL_BG); set_fg(COL_PANEL_FG);
    printf("│ Engine: ");
    set_fg(COL_PGN_FG);
    if(ENG.active)
        printf("%-27s",ENG.name);
    else
        printf("%-27s","Human vs Human");
    set_fg(COL_BORDER);
    printf("│");

    /* engine color */
    move_to(panel_row+2, panel_col);
    set_bg(COL_PANEL_BG); set_fg(COL_PANEL_FG);
    printf("│ Engine plays: ");
    set_fg(COL_PGN_FG);
    if(ENG.active){
        if(UI.engine_color==0) printf("%-22s","White");
        else                   printf("%-22s","Black");
    } else printf("%-22s","N/A");
    set_fg(COL_BORDER); printf("│");

    /* to move */
    move_to(panel_row+3, panel_col);
    set_bg(COL_PANEL_BG); set_fg(COL_PANEL_FG);
    printf("│ To move: ");
    set_fg(G.side==0?COL_TITLE_FG:COL_PGN_FG);
    printf("%-27s",G.side==0?"White":"Black");
    set_fg(COL_BORDER); printf("│");

    /* halfmove clock */
    move_to(panel_row+4, panel_col);
    set_bg(COL_PANEL_BG); set_fg(COL_PANEL_FG);
    printf("│ Half-move clock: ");
    set_fg(COL_PGN_FG);
    printf("%-19d",G.halfmove);
    set_fg(COL_BORDER); printf("│");

    /* check/game over */
    move_to(panel_row+5, panel_col);
    set_bg(COL_PANEL_BG);
    if(UI.game_over){
        set_fg(COL_CHECK);
        printf("│ %-36s│", UI.game_result);
    } else if(in_check(G.board,G.side)){
        set_fg(COL_CHECK);
        printf("│ %-36s│","CHECK!");
    } else if(ENG.thinking){
        set_fg(COL_TITLE_FG);
        printf("│ %-36s│","Engine thinking...");
    } else {
        set_fg(COL_PANEL_FG);
        printf("│ %-36s│","");
    }

    /* message */
    move_to(panel_row+6, panel_col);
    set_bg(COL_PANEL_BG);
    if(UI.message_timer>0){
        set_fg(COL_TITLE_FG);
        printf("│ %-36s│",UI.message);
        UI.message_timer--;
    } else {
        set_fg(COL_PANEL_FG);
        printf("│ %-36s│","");
    }

    /* controls */
    move_to(panel_row+7, panel_col);
    set_bg(COL_PANEL_BG); set_fg(COL_BORDER);
    printf("├");
    for(int i=0;i<36;i++) printf("─");
    printf("┤");

    const char *controls[][2]={
        {"Arrows","Move cursor"},
        {"Enter/Space","Select/move piece"},
        {"U","Undo last move"},
        {"F","Flip board"},
        {"N","New game"},
        {"H","Toggle help"},
        {"Q","Quit"},
    };
    for(int i=0;i<7;i++){
        move_to(panel_row+8+i, panel_col);
        set_bg(COL_PANEL_BG);
        set_fg(COL_TITLE_FG);
        printf("│ %-12s",controls[i][0]);
        set_fg(COL_PANEL_FG);
        printf("%-24s",controls[i][1]);
        set_fg(COL_BORDER);
        printf("│");
    }

    move_to(panel_row+15, panel_col);
    set_bg(COL_PANEL_BG); set_fg(COL_BORDER);
    printf("└");
    for(int i=0;i<36;i++) printf("─");
    printf("┘");
    printf(RESET_COLOR);
}

static void render_title(void){
    move_to(1,1);
    set_bg(COL_PANEL_BG); set_fg(COL_TITLE_FG);
    printf(BOLD " ♔ Terminal Chess ");
    set_fg(COL_PANEL_FG);
    printf("Move %d  ", G.fullmove);
    if(ENG.thinking){ set_fg(COL_CHECK); printf("[Engine thinking]"); }
    printf("     ");
    printf(RESET_COLOR);
}

static void render_help(void){
    if(!UI.show_help) return;
    /* overlay */
    int sr=5, sc=10;
    const char *lines[]={
        "╔══════════════════════════════════════╗",
        "║           CHESS GUI HELP             ║",
        "╠══════════════════════════════════════╣",
        "║  Arrow Keys  : Move cursor           ║",
        "║  Enter/Space : Select / Move piece   ║",
        "║  U           : Undo last move        ║",
        "║  F           : Flip board            ║",
        "║  N           : New game              ║",
        "║  H           : Close this help       ║",
        "║  Q           : Quit                  ║",
        "╠══════════════════════════════════════╣",
        "║  Highlighted squares:                ║",
        "║  Orange   = cursor position          ║",
        "║  Gold     = selected piece           ║",
        "║  Blue     = valid move targets       ║",
        "║  Green    = last move                ║",
        "║  Red      = king in check            ║",
        "╚══════════════════════════════════════╝",
        NULL
    };
    for(int i=0;lines[i];i++){
        move_to(sr+i,sc);
        set_bg(232); set_fg(COL_TITLE_FG);
        printf("%s",lines[i]);
    }
    printf(RESET_COLOR);
}

static void render_all(void){
    /* fill background */
    printf(HIDE_CURSOR);
    set_bg(COL_PANEL_BG);
    /* clear screen efficiently row by row */
    for(int r=1;r<=36;r++){
        move_to(r,1);
        printf("%-79s","");
    }
    render_title();
    render_border();
    render_board();
    render_pgn();
    render_info();
    render_help();
    fflush(stdout);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Promotion dialog                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static Piece promotion_dialog(int color){
    /* simple inline prompt */
    move_to(BOARD_ORIGIN_ROW+8*SQ_H+3,BOARD_ORIGIN_COL);
    set_bg(COL_PANEL_BG); set_fg(COL_TITLE_FG);
    printf("Promote to: (Q)ueen (R)ook (B)ishop (K)night > ");
    fflush(stdout);
    while(1){
        char c=0;
        read(STDIN_FILENO,&c,1);
        c=tolower(c);
        if(c=='q') return color?B_QUEEN :W_QUEEN;
        if(c=='r') return color?B_ROOK  :W_ROOK;
        if(c=='b') return color?B_BISHOP:W_BISHOP;
        if(c=='k'||c=='n') return color?B_KNIGHT:W_KNIGHT;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Input handling                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

static void set_message(const char *msg){
    strncpy(UI.message,msg,127);
    UI.message_timer=30;
}

static void handle_select_or_move(void){
    if(UI.game_over){ set_message("Game over! Press N for new game."); return; }
    /* engine's turn */
    if(ENG.active && G.side==UI.engine_color){ set_message("Wait for engine!"); return; }

    int sq=visual_to_sq(UI.cursor_row, UI.cursor_col);

    if(UI.selected<0){
        /* select */
        Piece p=G.board[sq];
        if(p==EMPTY||PIECE_COLOR(p)!=G.side){
            set_message("Select your own piece!");
            return;
        }
        UI.selected=sq;
        compute_valid_moves(sq);
        /* check if any valid moves exist */
        int has=0;
        for(int i=0;i<64;i++) if(UI.valid_moves[i]){ has=1; break; }
        if(!has){ set_message("No legal moves for that piece!"); UI.selected=-1; }
    } else {
        if(sq==UI.selected){
            /* deselect */
            UI.selected=-1;
            memset(UI.valid_moves,0,sizeof(UI.valid_moves));
            return;
        }
        if(!UI.valid_moves[sq]){
            /* re-select another own piece */
            Piece p=G.board[sq];
            if(IS_PIECE(p) && PIECE_COLOR(p)==G.side){
                UI.selected=sq;
                compute_valid_moves(sq);
            } else {
                set_message("Invalid move!");
            }
            return;
        }
        /* execute move */
        Piece moved=G.board[UI.selected];
        int color=IS_WHITE(moved)?0:1;
        Piece promo=EMPTY;
        /* pawn promotion? */
        if((moved==W_PAWN && ROW(sq)==7)||(moved==B_PAWN && ROW(sq)==0)){
            render_all();
            promo=promotion_dialog(color);
        }
        if(apply_move(UI.selected,sq,promo)){
            if(ENG.active && !UI.game_over) engine_go();
        }
        UI.selected=-1;
        memset(UI.valid_moves,0,sizeof(UI.valid_moves));
    }
}

static void handle_undo(void){
    if(G.hist_count==0){ set_message("Nothing to undo!"); return; }
    undo_move();
    /* if engine was thinking, cancel (send stop) */
    if(ENG.active && ENG.thinking){
        engine_write("stop");
        ENG.thinking=0;
    }
    /* undo engine move too */
    if(ENG.active && G.hist_count>0 && G.side==UI.engine_color){
        undo_move();
    }
    UI.selected=-1;
    memset(UI.valid_moves,0,sizeof(UI.valid_moves));
    set_message("Move undone.");
}

static void handle_new_game(void){
    if(ENG.active && ENG.thinking){ engine_write("stop"); ENG.thinking=0; }
    init_board();
    UI.selected=-1;
    memset(UI.valid_moves,0,sizeof(UI.valid_moves));
    UI.game_over=0;
    UI.game_result[0]='\0';
    set_message("New game started!");
    if(ENG.active && UI.engine_color==0) engine_go();
}

static void process_key(void){
    char buf[8]={0};
    int n=read(STDIN_FILENO,buf,sizeof(buf));
    if(n<=0) return;

    /* escape sequence */
    if(buf[0]==27 && n>=3 && buf[1]=='['){
        switch(buf[2]){
            case 'A': /* up */
                if(UI.cursor_row>0) UI.cursor_row--;
                break;
            case 'B': /* down */
                if(UI.cursor_row<7) UI.cursor_row++;
                break;
            case 'C': /* right */
                if(UI.cursor_col<7) UI.cursor_col++;
                break;
            case 'D': /* left */
                if(UI.cursor_col>0) UI.cursor_col--;
                break;
        }
        return;
    }

    if(buf[0]==27){ /* lone ESC = deselect */
        UI.selected=-1;
        memset(UI.valid_moves,0,sizeof(UI.valid_moves));
        return;
    }

    char k=tolower(buf[0]);
    switch(k){
        case '\r': case '\n': case ' ':
            handle_select_or_move();
            break;
        case 'u':
            handle_undo();
            break;
        case 'f':
            UI.flipped=!UI.flipped;
            set_message(UI.flipped?"Board flipped (Black at bottom)":"Board flipped (White at bottom)");
            break;
        case 'n':
            handle_new_game();
            break;
        case 'h':
            UI.show_help=!UI.show_help;
            break;
        case 'q':
            RUNNING=0;
            break;
        /* WASD as alternative cursor */
        case 'w': if(UI.cursor_row>0) UI.cursor_row--; break;
        case 's': if(UI.cursor_row<7) UI.cursor_row++; break;
        case 'd': if(UI.cursor_col<7) UI.cursor_col++; break;
        case 'a': if(UI.cursor_col>0) UI.cursor_col--; break;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Main loop                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]){
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    memset(&G,  0, sizeof(G));
    memset(&UI, 0, sizeof(UI));
    memset(&ENG,0, sizeof(ENG));

    init_board();
    UI.cursor_row=6; UI.cursor_col=4; /* e2 default */
    UI.selected=-1;
    UI.engine_color=-1;

    if(argc>=2){
        printf("Launching engine: %s\n", argv[1]);
        fflush(stdout);
        if(!engine_launch(argv[1])){
            fprintf(stderr,"Failed to launch engine: %s\n",argv[1]);
            return 1;
        }
        /* engine plays black by default; pass "white" as 3rd arg for engine white */
        UI.engine_color=1;
        if(argc>=3 && strcmp(argv[2],"white")==0){
            UI.engine_color=0;
            engine_go(); /* engine plays first */
        }
        printf("Engine '%s' loaded. Engine plays %s.\n",
               ENG.name, UI.engine_color==0?"White":"Black");
        fflush(stdout);
        usleep(200000);
    }

    term_raw();
    printf(CLEAR_SCREEN HIDE_CURSOR);

    struct timeval tv;
    fd_set fds;

    while(RUNNING){
        render_all();

        /* poll with short timeout so engine moves can come in */
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        tv.tv_sec=0; tv.tv_usec=50000; /* 50ms */
        int r=select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);
        if(r>0 && FD_ISSET(STDIN_FILENO,&fds))
            process_key();

        /* check engine */
        if(ENG.active && ENG.thinking && engine_poll()){
            engine_apply_bestmove();
        }
    }

    cleanup();
    printf(CLEAR_SCREEN);
    printf("Thanks for playing! Final position:\n");
    /* print simple board */
    for(int r=7;r>=0;r--){
        printf("%d  ",r+1);
        for(int c=0;c<8;c++){
            Piece p=G.board[SQ(r,c)];
            printf("%c ",PIECE_LETTERS[p]);
        }
        printf("\n");
    }
    printf("   a b c d e f g h\n");
    if(G.hist_count>0){
        printf("\nPGN moves:\n");
        for(int i=0;i<G.hist_count;i++){
            if(i%2==0) printf("%d. ",i/2+1);
            printf("%s ",G.history[i].pgn);
            if(i%2==1) printf("\n");
        }
        printf("\n");
    }
    return 0;
}
