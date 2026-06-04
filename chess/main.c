#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

/* ─── ANSI / Terminal ─────────────────────────────────────────────────────── */
#define CLEAR_SCREEN    "\033[2J\033[H"
#define HIDE_CURSOR     "\033[?25l"
#define SHOW_CURSOR     "\033[?25h"
#define RESET           "\033[0m"
#define BOLD            "\033[1m"

/* Foreground colours */
#define FG_BLACK        "\033[30m"
#define FG_WHITE        "\033[97m"
#define FG_RED          "\033[91m"
#define FG_YELLOW       "\033[93m"
#define FG_CYAN         "\033[96m"
#define FG_GREEN        "\033[92m"
#define FG_MAGENTA      "\033[95m"
#define FG_BLUE         "\033[94m"
#define FG_BRIGHT_WHITE "\033[97m"

/* Background colours */
#define BG_DARK_SQ      "\033[48;5;94m"   /* dark square  – brown */
#define BG_LIGHT_SQ     "\033[48;5;229m"  /* light square – cream */
#define BG_SELECTED     "\033[48;5;226m"  /* selected piece */
#define BG_LEGAL        "\033[48;5;118m"  /* legal move target */
#define BG_LAST_MOVE    "\033[48;5;220m"  /* last move highlight */
#define BG_CHECK        "\033[48;5;196m"  /* king in check */
#define BG_CAPTURE      "\033[48;5;208m"  /* capture target */
#define BG_STATUS       "\033[48;5;235m"  /* status bar */

/* ─── Piece constants ─────────────────────────────────────────────────────── */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE_SIDE  8
#define BLACK_SIDE 16

#define WHITE_PAWN   (PAWN   | WHITE_SIDE)
#define WHITE_KNIGHT (KNIGHT | WHITE_SIDE)
#define WHITE_BISHOP (BISHOP | WHITE_SIDE)
#define WHITE_ROOK   (ROOK   | WHITE_SIDE)
#define WHITE_QUEEN  (QUEEN  | WHITE_SIDE)
#define WHITE_KING   (KING   | WHITE_SIDE)

#define BLACK_PAWN   (PAWN   | BLACK_SIDE)
#define BLACK_KNIGHT (KNIGHT | BLACK_SIDE)
#define BLACK_BISHOP (BISHOP | BLACK_SIDE)
#define BLACK_ROOK   (ROOK   | BLACK_SIDE)
#define BLACK_QUEEN  (QUEEN  | BLACK_SIDE)
#define BLACK_KING   (KING   | BLACK_SIDE)

#define PIECE_TYPE(p)  ((p) & 7)
#define PIECE_COLOR(p) ((p) & 24)
#define IS_WHITE(p)    (((p) & WHITE_SIDE) != 0)
#define IS_BLACK(p)    (((p) & BLACK_SIDE) != 0)

/* ─── Board / Move structs ────────────────────────────────────────────────── */
typedef struct {
    int board[8][8];           /* [rank][file] rank0 = rank1 (white back) */
    int side_to_move;          /* WHITE_SIDE or BLACK_SIDE */
    int castling;              /* bits: 1=WK 2=WQ 4=BK 8=BQ */
    int en_passant;            /* file (0-7) or -1 */
    int halfmove;
    int fullmove;
} Position;

typedef struct {
    int from_r, from_f;
    int to_r,   to_f;
    int promo;                 /* promotion piece type or 0 */
    int captured;              /* piece on destination before move */
    int flags;                 /* special flags */
    /* saved state for undo */
    int old_castling;
    int old_ep;
    int old_halfmove;
    /* for castling undo */
    int rook_from_f, rook_to_f, rook_rank;
    /* captured en passant pawn rank/file */
    int ep_cap_r, ep_cap_f;
    /* PGN text */
    char pgn[16];
} Move;

#define FLAG_CASTLE_K  1
#define FLAG_CASTLE_Q  2
#define FLAG_EP        4
#define FLAG_PROMO     8

/* ─── History ─────────────────────────────────────────────────────────────── */
#define MAX_HISTORY 512

typedef struct {
    Move      move;
    Position  pos_before;
} HistoryEntry;

/* ─── Engine process ──────────────────────────────────────────────────────── */
typedef struct {
    pid_t pid;
    int   in_fd;   /* we write here  → engine stdin  */
    int   out_fd;  /* we read here   → engine stdout */
    char  name[128];
    int   ready;
} Engine;

/* ─── Time-control modes ──────────────────────────────────────────────────── */
typedef enum { TC_TIME, TC_DEPTH, TC_NODES } TCMode;

typedef struct {
    TCMode mode;
    int    time_ms;   /* ms per move (TC_TIME) */
    int    depth;     /* (TC_DEPTH)  */
    long   nodes;     /* (TC_NODES)  */
} TimeControl;

/* ─── Global state ────────────────────────────────────────────────────────── */
static Position      g_pos;
static HistoryEntry  g_history[MAX_HISTORY];
static int           g_hist_len  = 0;
static Engine        g_engine    = {0};
static int           g_engine_active = 0;
static TimeControl   g_tc        = { TC_TIME, 1000, 5, 100000 };

/* Cursor / selection */
static int g_cur_r = 0, g_cur_f = 0;   /* 0-7, rank 0 = rank1 */
static int g_sel_r = -1, g_sel_f = -1; /* selected square, -1 = none */

/* Legal-move cache for selected piece */
static int  g_legal[64][2];
static int  g_nlegal = 0;

/* Last engine / human move */
static int  g_last_from_r = -1, g_last_from_f = -1;
static int  g_last_to_r   = -1, g_last_to_f   = -1;

/* Flags */
static int  g_in_check    = 0;
static int  g_game_over   = 0;
static char g_status_msg[128] = "";

/* Flip board (black at bottom) */
static int  g_flip = 0;

/* PGN move list */
static char g_pgn_moves[MAX_HISTORY][16];
static int  g_pgn_count = 0;

/* Terminal saved state */
static struct termios g_old_term;

/* ─── Unicode chess pieces ────────────────────────────────────────────────── */
static const char *PIECE_UNICODE[2][7] = {
    /* WHITE: empty, P, N, B, R, Q, K */
    { "  ", "♙ ", "♘ ", "♗ ", "♖ ", "♕ ", "♔ " },
    /* BLACK: empty, p, n, b, r, q, k */
    { "  ", "♟ ", "♞ ", "♝ ", "♜ ", "♛ ", "♚ " },
};

/* ─── Forward declarations ────────────────────────────────────────────────── */
static void init_position(Position *p);
static int  generate_moves(const Position *p, Move *moves);
static int  is_in_check(const Position *p, int side);
static void do_move(Position *p, Move *m);
static void undo_move(Position *p, const Move *m, const Position *before);
static int  move_leaves_in_check(Position *p, Move *m);
static void generate_pgn_text(const Position *before, const Move *m,
                               const Position *after, char *buf);
static void render(void);
static void send_engine(const char *cmd);
static char *read_engine_line(char *buf, int sz, int timeout_ms);
static void  engine_go(void);
static void  parse_engine_best_move(const char *line, Move *out);
static int   find_and_apply_move(const char *uci_str);
static void  legal_moves_for_square(int r, int f);
static int   is_legal_target(int r, int f);
static void  apply_player_move(int fr, int ff, int tr, int tf);
static void  set_raw_mode(void);
static void  restore_terminal(void);
static void  handle_sigchld(int sig);
static void  run_ui_loop(void);
static void  undo_last_move(void);
static void  show_tc_menu(void);
static void  show_engine_menu(void);
static void  position_to_fen(const Position *p, char *buf);
static int   algebraic_to_sq(const char *s, int *r, int *f);
static int   sq_attacked(const Position *p, int r, int f, int by_side);
static void  build_pgn_list(void);

/* ═══════════════════════════════════════════════════════════════════════════
   POSITION INIT
   ═══════════════════════════════════════════════════════════════════════════ */
static void init_position(Position *p)
{
    memset(p, 0, sizeof(*p));
    /* Rank 0 = rank-1 (white back rank) */
    p->board[0][0] = WHITE_ROOK;
    p->board[0][1] = WHITE_KNIGHT;
    p->board[0][2] = WHITE_BISHOP;
    p->board[0][3] = WHITE_QUEEN;
    p->board[0][4] = WHITE_KING;
    p->board[0][5] = WHITE_BISHOP;
    p->board[0][6] = WHITE_KNIGHT;
    p->board[0][7] = WHITE_ROOK;
    for (int f = 0; f < 8; f++) p->board[1][f] = WHITE_PAWN;
    for (int f = 0; f < 8; f++) p->board[6][f] = BLACK_PAWN;
    p->board[7][0] = BLACK_ROOK;
    p->board[7][1] = BLACK_KNIGHT;
    p->board[7][2] = BLACK_BISHOP;
    p->board[7][3] = BLACK_QUEEN;
    p->board[7][4] = BLACK_KING;
    p->board[7][5] = BLACK_BISHOP;
    p->board[7][6] = BLACK_KNIGHT;
    p->board[7][7] = BLACK_ROOK;
    p->side_to_move = WHITE_SIDE;
    p->castling = 15; /* all rights */
    p->en_passant = -1;
    p->halfmove = 0;
    p->fullmove = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   ATTACK / SQUARE DETECTION
   ═══════════════════════════════════════════════════════════════════════════ */
static int in_bounds(int r, int f) { return r>=0 && r<8 && f>=0 && f<8; }

static int sq_attacked(const Position *p, int r, int f, int by_side)
{
    int opp = by_side;

    /* Pawn attacks */
    int pawn_dir = (opp == WHITE_SIDE) ? -1 : 1;
    for (int df = -1; df <= 1; df += 2) {
        int pr = r - pawn_dir, pf = f + df;
        if (in_bounds(pr, pf)) {
            int piece = p->board[pr][pf];
            if (PIECE_COLOR(piece) == opp && PIECE_TYPE(piece) == PAWN)
                return 1;
        }
    }
    /* Knight */
    int kn_dr[] = {-2,-2,-1,-1,1,1,2,2};
    int kn_df[] = {-1,1,-2,2,-2,2,-1,1};
    for (int i=0;i<8;i++){
        int nr=r+kn_dr[i], nf=f+kn_df[i];
        if(in_bounds(nr,nf)){
            int piece=p->board[nr][nf];
            if(PIECE_COLOR(piece)==opp && PIECE_TYPE(piece)==KNIGHT) return 1;
        }
    }
    /* Sliding pieces */
    int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    for(int d=0;d<8;d++){
        int nr=r+dirs[d][0], nf=f+dirs[d][1];
        while(in_bounds(nr,nf)){
            int piece=p->board[nr][nf];
            if(piece!=EMPTY){
                if(PIECE_COLOR(piece)==opp){
                    int t=PIECE_TYPE(piece);
                    int diag=(dirs[d][0]!=0 && dirs[d][1]!=0);
                    if(t==QUEEN) return 1;
                    if(diag && t==BISHOP) return 1;
                    if(!diag && t==ROOK) return 1;
                }
                break;
            }
            nr+=dirs[d][0]; nf+=dirs[d][1];
        }
    }
    /* King */
    for(int dr=-1;dr<=1;dr++) for(int df=-1;df<=1;df++){
        if(!dr && !df) continue;
        int nr=r+dr, nf=f+df;
        if(in_bounds(nr,nf)){
            int piece=p->board[nr][nf];
            if(PIECE_COLOR(piece)==opp && PIECE_TYPE(piece)==KING) return 1;
        }
    }
    return 0;
}

static int is_in_check(const Position *p, int side)
{
    /* Find king */
    int kr=-1,kf=-1;
    for(int r=0;r<8;r++) for(int f=0;f<8;f++){
        if(p->board[r][f]==(KING|side)){ kr=r;kf=f; break; }
    }
    if(kr<0) return 0;
    int opp = (side==WHITE_SIDE)?BLACK_SIDE:WHITE_SIDE;
    return sq_attacked(p,kr,kf,opp);
}

/* ═══════════════════════════════════════════════════════════════════════════
   MOVE GENERATION
   ═══════════════════════════════════════════════════════════════════════════ */
static int generate_moves(const Position *p, Move *moves)
{
    int n=0;
    int side=p->side_to_move;
    int opp=(side==WHITE_SIDE)?BLACK_SIDE:WHITE_SIDE;
    int pdir=(side==WHITE_SIDE)?1:-1;
    int pstart=(side==WHITE_SIDE)?1:6;
    int pprom=(side==WHITE_SIDE)?7:0;

#define ADD_MOVE(fr,ff,tr,tf,cap,fl,pro) do{ \
    moves[n].from_r=(fr); moves[n].from_f=(ff); \
    moves[n].to_r=(tr);   moves[n].to_f=(tf); \
    moves[n].captured=(cap); moves[n].flags=(fl); moves[n].promo=(pro); \
    moves[n].old_castling=p->castling; moves[n].old_ep=p->en_passant; \
    moves[n].old_halfmove=p->halfmove; \
    moves[n].rook_from_f=moves[n].rook_to_f=moves[n].rook_rank=0; \
    moves[n].ep_cap_r=moves[n].ep_cap_f=0; \
    n++; }while(0)

    for(int r=0;r<8;r++) for(int f=0;f<8;f++){
        int piece=p->board[r][f];
        if(!piece || PIECE_COLOR(piece)!=side) continue;
        int type=PIECE_TYPE(piece);

        if(type==PAWN){
            /* single push */
            int nr=r+pdir;
            if(in_bounds(nr,f) && p->board[nr][f]==EMPTY){
                if(nr==pprom){
                    for(int pro=KNIGHT;pro<=QUEEN;pro++)
                        ADD_MOVE(r,f,nr,f,EMPTY,FLAG_PROMO,pro);
                } else {
                    ADD_MOVE(r,f,nr,f,EMPTY,0,0);
                    /* double push */
                    if(r==pstart){
                        int nr2=r+2*pdir;
                        if(p->board[nr2][f]==EMPTY)
                            ADD_MOVE(r,f,nr2,f,EMPTY,0,0);
                    }
                }
            }
            /* captures */
            for(int df=-1;df<=1;df+=2){
                int nf2=f+df;
                if(!in_bounds(nr,nf2)) continue;
                int cap=p->board[nr][nf2];
                if(cap && PIECE_COLOR(cap)==opp){
                    if(nr==pprom){
                        for(int pro=KNIGHT;pro<=QUEEN;pro++)
                            ADD_MOVE(r,f,nr,nf2,cap,FLAG_PROMO,pro);
                    } else {
                        ADD_MOVE(r,f,nr,nf2,cap,0,0);
                    }
                }
                /* en passant */
                if(p->en_passant==nf2 && nr==(side==WHITE_SIDE?5:2)){
                    int epr=r, epf=nf2;
                    int epcap=p->board[epr][epf];
                    int idx=n;
                    ADD_MOVE(r,f,nr,nf2,epcap,FLAG_EP,0);
                    moves[idx].ep_cap_r=epr; moves[idx].ep_cap_f=epf;
                }
            }
        } else if(type==KNIGHT){
            int dr[]={-2,-2,-1,-1,1,1,2,2};
            int df[]={-1,1,-2,2,-2,2,-1,1};
            for(int i=0;i<8;i++){
                int nr=r+dr[i], nf=f+df[i];
                if(!in_bounds(nr,nf)) continue;
                int cap=p->board[nr][nf];
                if(cap && PIECE_COLOR(cap)==side) continue;
                ADD_MOVE(r,f,nr,nf,cap,0,0);
            }
        } else if(type==KING){
            for(int dr=-1;dr<=1;dr++) for(int df=-1;df<=1;df++){
                if(!dr&&!df) continue;
                int nr=r+dr, nf=f+df;
                if(!in_bounds(nr,nf)) continue;
                int cap=p->board[nr][nf];
                if(cap && PIECE_COLOR(cap)==side) continue;
                ADD_MOVE(r,f,nr,nf,cap,0,0);
            }
            /* Castling */
            if(side==WHITE_SIDE && r==0 && f==4){
                if((p->castling&1) && p->board[0][5]==EMPTY && p->board[0][6]==EMPTY &&
                   !sq_attacked(p,0,4,opp) && !sq_attacked(p,0,5,opp) && !sq_attacked(p,0,6,opp)){
                    int idx=n;
                    ADD_MOVE(0,4,0,6,EMPTY,FLAG_CASTLE_K,0);
                    moves[idx].rook_from_f=7; moves[idx].rook_to_f=5; moves[idx].rook_rank=0;
                }
                if((p->castling&2) && p->board[0][3]==EMPTY && p->board[0][2]==EMPTY && p->board[0][1]==EMPTY &&
                   !sq_attacked(p,0,4,opp) && !sq_attacked(p,0,3,opp) && !sq_attacked(p,0,2,opp)){
                    int idx=n;
                    ADD_MOVE(0,4,0,2,EMPTY,FLAG_CASTLE_Q,0);
                    moves[idx].rook_from_f=0; moves[idx].rook_to_f=3; moves[idx].rook_rank=0;
                }
            } else if(side==BLACK_SIDE && r==7 && f==4){
                if((p->castling&4) && p->board[7][5]==EMPTY && p->board[7][6]==EMPTY &&
                   !sq_attacked(p,7,4,opp) && !sq_attacked(p,7,5,opp) && !sq_attacked(p,7,6,opp)){
                    int idx=n;
                    ADD_MOVE(7,4,7,6,EMPTY,FLAG_CASTLE_K,0);
                    moves[idx].rook_from_f=7; moves[idx].rook_to_f=5; moves[idx].rook_rank=7;
                }
                if((p->castling&8) && p->board[7][3]==EMPTY && p->board[7][2]==EMPTY && p->board[7][1]==EMPTY &&
                   !sq_attacked(p,7,4,opp) && !sq_attacked(p,7,3,opp) && !sq_attacked(p,7,2,opp)){
                    int idx=n;
                    ADD_MOVE(7,4,7,2,EMPTY,FLAG_CASTLE_Q,0);
                    moves[idx].rook_from_f=0; moves[idx].rook_to_f=3; moves[idx].rook_rank=7;
                }
            }
        } else {
            /* Sliding: Bishop, Rook, Queen */
            int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
            int start=0, end=8;
            if(type==BISHOP){ start=4; end=8; }
            if(type==ROOK)  { start=0; end=4; }
            for(int d=start;d<end;d++){
                int nr=r+dirs[d][0], nf=f+dirs[d][1];
                while(in_bounds(nr,nf)){
                    int cap=p->board[nr][nf];
                    if(cap && PIECE_COLOR(cap)==side) break;
                    ADD_MOVE(r,f,nr,nf,cap,0,0);
                    if(cap) break;
                    nr+=dirs[d][0]; nf+=dirs[d][1];
                }
            }
        }
    }
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAKE / UNDO MOVE
   ═══════════════════════════════════════════════════════════════════════════ */
static void do_move(Position *p, Move *m)
{
    int piece=p->board[m->from_r][m->from_f];
    int type=PIECE_TYPE(piece);
    int side=PIECE_COLOR(piece);

    /* Remove from source */
    p->board[m->from_r][m->from_f]=EMPTY;

    /* En passant capture */
    if(m->flags & FLAG_EP){
        p->board[m->ep_cap_r][m->ep_cap_f]=EMPTY;
    }

    /* Place piece (with promotion) */
    if(m->flags & FLAG_PROMO){
        p->board[m->to_r][m->to_f]=(m->promo | side);
    } else {
        p->board[m->to_r][m->to_f]=piece;
    }

    /* Castling: move rook */
    if(m->flags & (FLAG_CASTLE_K|FLAG_CASTLE_Q)){
        int rr=m->rook_rank;
        p->board[rr][m->rook_to_f]=p->board[rr][m->rook_from_f];
        p->board[rr][m->rook_from_f]=EMPTY;
    }

    /* Update castling rights */
    if(type==KING){
        if(side==WHITE_SIDE) p->castling &= ~3;
        else                  p->castling &= ~12;
    }
    if(type==ROOK){
        if(m->from_r==0 && m->from_f==7) p->castling &= ~1;
        if(m->from_r==0 && m->from_f==0) p->castling &= ~2;
        if(m->from_r==7 && m->from_f==7) p->castling &= ~4;
        if(m->from_r==7 && m->from_f==0) p->castling &= ~8;
    }
    /* If rook captured */
    if(m->to_r==0 && m->to_f==7) p->castling &= ~1;
    if(m->to_r==0 && m->to_f==0) p->castling &= ~2;
    if(m->to_r==7 && m->to_f==7) p->castling &= ~4;
    if(m->to_r==7 && m->to_f==0) p->castling &= ~8;

    /* En passant square */
    p->en_passant=-1;
    if(type==PAWN && abs(m->to_r-m->from_r)==2){
        p->en_passant=m->from_f;
    }

    /* Halfmove clock */
    if(type==PAWN || m->captured) p->halfmove=0;
    else p->halfmove++;

    /* Fullmove */
    if(side==BLACK_SIDE) p->fullmove++;

    p->side_to_move=(side==WHITE_SIDE)?BLACK_SIDE:WHITE_SIDE;
}

static void undo_move(Position *p, const Move *m, const Position *before)
{
    (void)m;
    *p = *before;
}

static int move_leaves_in_check(Position *p, Move *m)
{
    Position tmp=*p;
    do_move(&tmp,m);
    int myside=PIECE_COLOR(p->board[m->from_r][m->from_f]);
    return is_in_check(&tmp,myside);
}

/* ═══════════════════════════════════════════════════════════════════════════
   FEN
   ═══════════════════════════════════════════════════════════════════════════ */
static void position_to_fen(const Position *p, char *buf)
{
    char *s=buf;
    static const char piece_chars[2][7]={
        {'.','P','N','B','R','Q','K'},
        {'.','p','n','b','r','q','k'}
    };
    for(int r=7;r>=0;r--){
        int empty=0;
        for(int f=0;f<8;f++){
            int pc=p->board[r][f];
            if(!pc){ empty++; }
            else {
                if(empty){ *s++='0'+empty; empty=0; }
                int col=IS_WHITE(pc)?0:1;
                *s++=piece_chars[col][PIECE_TYPE(pc)];
            }
        }
        if(empty) *s++='0'+empty;
        if(r>0) *s++='/';
    }
    *s++=' ';
    *s++=(p->side_to_move==WHITE_SIDE)?'w':'b';
    *s++=' ';
    if(!p->castling){ *s++='-'; }
    else {
        if(p->castling&1) *s++='K';
        if(p->castling&2) *s++='Q';
        if(p->castling&4) *s++='k';
        if(p->castling&8) *s++='q';
    }
    *s++=' ';
    if(p->en_passant<0){ *s++='-'; }
    else {
        *s++='a'+p->en_passant;
        *s++=(p->side_to_move==WHITE_SIDE)?'6':'3';
    }
    sprintf(s," %d %d",p->halfmove,p->fullmove);
}

/* ═══════════════════════════════════════════════════════════════════════════
   PGN TEXT
   ═══════════════════════════════════════════════════════════════════════════ */
static void generate_pgn_text(const Position *before, const Move *m,
                               const Position *after, char *buf)
{
    char *s=buf;
    int type=PIECE_TYPE(before->board[m->from_r][m->from_f]);
    static const char type_chars[]="  PNBRQK";
    static const char *promo_str[]={"","","","n","b","r","q"};

    if(m->flags & FLAG_CASTLE_K){ strcpy(buf,"O-O"); }
    else if(m->flags & FLAG_CASTLE_Q){ strcpy(buf,"O-O-O"); }
    else {
        if(type!=PAWN) *s++=type_chars[type];
        /* Disambiguation: check other pieces of same type that can reach to_sq */
        if(type!=PAWN){
            Move tmp_moves[256]; int nm=generate_moves(before,tmp_moves);
            int ambig_r=0, ambig_f=0;
            for(int i=0;i<nm;i++){
                if(&tmp_moves[i]==m) continue;
                if(tmp_moves[i].to_r==m->to_r && tmp_moves[i].to_f==m->to_f &&
                   PIECE_TYPE(before->board[tmp_moves[i].from_r][tmp_moves[i].from_f])==type &&
                   !move_leaves_in_check((Position*)before,&tmp_moves[i])){
                    if(tmp_moves[i].from_f!=m->from_f) ambig_f=1;
                    else ambig_r=1;
                }
            }
            if(ambig_f) *s++='a'+m->from_f;
            else if(ambig_r) *s++='1'+m->from_r;
        } else if(m->captured || (m->flags&FLAG_EP)){
            *s++='a'+m->from_f;
        }
        if(m->captured || (m->flags&FLAG_EP)) *s++='x';
        *s++='a'+m->to_f;
        *s++='1'+m->to_r;
        if(m->flags & FLAG_PROMO){
            *s++='=';
            *s++=toupper(promo_str[m->promo][0]);
        }
    }
    /* Check / checkmate */
    if(is_in_check(after, after->side_to_move)){
        Move chk_moves[256]; int nc=generate_moves(after,chk_moves);
        int has_legal=0;
        for(int i=0;i<nc;i++){
            if(!move_leaves_in_check((Position*)after,&chk_moves[i])){ has_legal=1; break; }
        }
        *s++=has_legal?'+':'#';
    }
    *s='\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
   LEGAL MOVE HIGHLIGHT CACHE
   ═══════════════════════════════════════════════════════════════════════════ */
static void legal_moves_for_square(int r, int f)
{
    g_nlegal=0;
    Move moves[256]; int n=generate_moves(&g_pos,moves);
    for(int i=0;i<n;i++){
        if(moves[i].from_r==r && moves[i].from_f==f){
            if(!move_leaves_in_check(&g_pos,&moves[i])){
                g_legal[g_nlegal][0]=moves[i].to_r;
                g_legal[g_nlegal][1]=moves[i].to_f;
                g_nlegal++;
            }
        }
    }
}

static int is_legal_target(int r, int f)
{
    for(int i=0;i<g_nlegal;i++)
        if(g_legal[i][0]==r && g_legal[i][1]==f) return 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   APPLY PLAYER MOVE
   ═══════════════════════════════════════════════════════════════════════════ */
static void apply_player_move(int fr, int ff, int tr, int tf)
{
    Move moves[256]; int n=generate_moves(&g_pos,moves);
    for(int i=0;i<n;i++){
        Move *m=&moves[i];
        if(m->from_r!=fr||m->from_f!=ff||m->to_r!=tr||m->to_f!=tf) continue;
        if(move_leaves_in_check(&g_pos,m)) continue;

        /* Promotion: default to queen */
        if(m->flags & FLAG_PROMO){
            /* find queen promo */
            int found=0;
            for(int j=i;j<n;j++){
                if(moves[j].from_r==fr&&moves[j].from_f==ff&&
                   moves[j].to_r==tr&&moves[j].to_f==tf&&
                   moves[j].promo==QUEEN){
                    m=&moves[j]; found=1; break;
                }
            }
            if(!found) m->promo=QUEEN;
        }

        /* Save history */
        Position before=g_pos;
        Position after=g_pos;
        do_move(&after,m);
        generate_pgn_text(&before,m,&after,m->pgn);

        g_history[g_hist_len].move=*m;
        g_history[g_hist_len].pos_before=before;
        g_hist_len++;

        g_last_from_r=fr; g_last_from_f=ff;
        g_last_to_r=tr;   g_last_to_f=tf;

        do_move(&g_pos,m);

        /* PGN list */
        strncpy(g_pgn_moves[g_pgn_count++],m->pgn,15);

        g_in_check=is_in_check(&g_pos,g_pos.side_to_move);
        return;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   UNDO
   ═══════════════════════════════════════════════════════════════════════════ */
static void undo_last_move(void)
{
    /* Undo two plies if engine is playing (engine + player) */
    int undo_count = g_engine_active ? 2 : 1;
    for(int u=0;u<undo_count&&g_hist_len>0;u++){
        g_hist_len--;
        g_pos=g_history[g_hist_len].pos_before;
        if(g_pgn_count>0) g_pgn_count--;
    }
    g_in_check=is_in_check(&g_pos,g_pos.side_to_move);
    g_sel_r=-1; g_sel_f=-1; g_nlegal=0;
    g_last_from_r=-1; g_last_from_f=-1;
    g_last_to_r=-1;   g_last_to_f=-1;
    if(g_hist_len>0){
        Move *last=&g_history[g_hist_len-1].move;
        g_last_from_r=last->from_r; g_last_from_f=last->from_f;
        g_last_to_r=last->to_r;     g_last_to_f=last->to_f;
    }
    g_game_over=0;
    snprintf(g_status_msg,sizeof(g_status_msg),"Move taken back.");
}

/* ═══════════════════════════════════════════════════════════════════════════
   ENGINE COMMUNICATION
   ═══════════════════════════════════════════════════════════════════════════ */
static void send_engine(const char *cmd)
{
    if(g_engine.in_fd<=0) return;
    char buf[512];
    snprintf(buf,sizeof(buf),"%s\n",cmd);
    write(g_engine.in_fd,buf,strlen(buf));
}

static char *read_engine_line(char *buf, int sz, int timeout_ms)
{
    if(g_engine.out_fd<=0) return NULL;
    int elapsed=0;
    int pos=0;
    while(elapsed<timeout_ms){
        char c;
        int r=read(g_engine.out_fd,&c,1);
        if(r==1){
            if(c=='\n'||c=='\r'){
                buf[pos]='\0';
                if(pos>0) return buf;
                pos=0;
                continue;
            }
            if(pos<sz-1) buf[pos++]=c;
        } else if(r<0 && errno==EAGAIN){
            usleep(1000); elapsed++;
        } else if(r==0){
            break;
        }
    }
    buf[pos]='\0';
    return pos>0?buf:NULL;
}

static void engine_go(void)
{
    if(!g_engine_active||g_engine.in_fd<=0) return;
    char fen[128]; position_to_fen(&g_pos,fen);
    char cmd[256];
    snprintf(cmd,sizeof(cmd),"position fen %s",fen);
    send_engine(cmd);

    char go_cmd[128];
    switch(g_tc.mode){
        case TC_TIME:
            snprintf(go_cmd,sizeof(go_cmd),"go movetime %d",g_tc.time_ms);
            break;
        case TC_DEPTH:
            snprintf(go_cmd,sizeof(go_cmd),"go depth %d",g_tc.depth);
            break;
        case TC_NODES:
            snprintf(go_cmd,sizeof(go_cmd),"go nodes %ld",(long)g_tc.nodes);
            break;
    }
    send_engine(go_cmd);
}

static void parse_engine_best_move(const char *line, Move *out)
{
    /* bestmove e2e4 [ponder ...] */
    const char *p=strstr(line,"bestmove ");
    if(!p) return;
    p+=9;
    char mv[8]; int i=0;
    while(*p && *p!=' ' && i<7) mv[i++]=*p++;
    mv[i]='\0';
    if(i<4) return;
    out->from_f=mv[0]-'a';
    out->from_r=mv[1]-'1';
    out->to_f  =mv[2]-'a';
    out->to_r  =mv[3]-'1';
    out->promo =0;
    if(mv[4]){
        char pc=tolower(mv[4]);
        if(pc=='n') out->promo=KNIGHT;
        else if(pc=='b') out->promo=BISHOP;
        else if(pc=='r') out->promo=ROOK;
        else out->promo=QUEEN;
    }
}

static int find_and_apply_move(const char *uci_str)
{
    int ff=uci_str[0]-'a';
    int fr=uci_str[1]-'1';
    int tf=uci_str[2]-'a';
    int tr=uci_str[3]-'1';
    int promo=0;
    if(uci_str[4]){
        char pc=tolower(uci_str[4]);
        if(pc=='n') promo=KNIGHT;
        else if(pc=='b') promo=BISHOP;
        else if(pc=='r') promo=ROOK;
        else promo=QUEEN;
    }

    Move moves[256]; int n=generate_moves(&g_pos,moves);
    for(int i=0;i<n;i++){
        Move *m=&moves[i];
        if(m->from_r!=fr||m->from_f!=ff||m->to_r!=tr||m->to_f!=tf) continue;
        if(promo && m->promo!=promo) continue;
        if(!promo && (m->flags&FLAG_PROMO)) continue;
        if(move_leaves_in_check(&g_pos,m)) continue;

        Position before=g_pos;
        Position after=g_pos;
        do_move(&after,m);
        generate_pgn_text(&before,m,&after,m->pgn);

        g_history[g_hist_len].move=*m;
        g_history[g_hist_len].pos_before=before;
        g_hist_len++;

        g_last_from_r=fr; g_last_from_f=ff;
        g_last_to_r=tr;   g_last_to_f=tf;

        do_move(&g_pos,m);
        strncpy(g_pgn_moves[g_pgn_count++],m->pgn,15);
        g_in_check=is_in_check(&g_pos,g_pos.side_to_move);
        return 1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   GAME OVER CHECK
   ═══════════════════════════════════════════════════════════════════════════ */
static void check_game_over(void)
{
    Move moves[256]; int n=generate_moves(&g_pos,moves);
    int has_legal=0;
    for(int i=0;i<n;i++){
        if(!move_leaves_in_check(&g_pos,&moves[i])){ has_legal=1; break; }
    }
    if(!has_legal){
        g_game_over=1;
        if(g_in_check){
            snprintf(g_status_msg,sizeof(g_status_msg),
                     "%s wins by checkmate!",
                     g_pos.side_to_move==WHITE_SIDE?"Black":"White");
        } else {
            snprintf(g_status_msg,sizeof(g_status_msg),"Stalemate! Draw.");
        }
    } else if(g_pos.halfmove>=100){
        g_game_over=1;
        snprintf(g_status_msg,sizeof(g_status_msg),"Draw by 50-move rule.");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   BUILD PGN LIST (from history)
   ═══════════════════════════════════════════════════════════════════════════ */
static void build_pgn_list(void)
{
    g_pgn_count=0;
    for(int i=0;i<g_hist_len;i++){
        strncpy(g_pgn_moves[g_pgn_count++],g_history[i].move.pgn,15);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   RENDER
   ═══════════════════════════════════════════════════════════════════════════ */
static void render(void)
{
    /* Move cursor to top-left without clearing — in-place update */
    printf("\033[H");

    /* ── Title bar ── */
    printf(BOLD FG_CYAN);
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║          ♟  Terminal Chess  ♙   [UCI Engine]         ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf(RESET);

    /* ── Board ── */
    /* We display rank 7 at top if not flipped */
    for(int disp_r=7;disp_r>=0;disp_r--){
        int r = g_flip ? (7-disp_r) : disp_r;

        /* Rank label */
        printf(BOLD FG_CYAN " %d " RESET, r+1);

        for(int disp_f=0;disp_f<8;disp_f++){
            int f = g_flip ? (7-disp_f) : disp_f;

            int piece=g_pos.board[r][f];
            int is_dark=((r+f)%2==0);
            int is_cursor=(r==g_cur_r && f==g_cur_f);
            int is_selected=(r==g_sel_r && f==g_sel_f);
            int is_legal_mv=is_legal_target(r,f);
            int is_last_from=(r==g_last_from_r && f==g_last_from_f);
            int is_last_to  =(r==g_last_to_r   && f==g_last_to_f);
            int is_king_check=0;
            if(g_in_check && piece==(KING|g_pos.side_to_move)) is_king_check=1;

            /* Choose background */
            if(is_king_check)          printf(BG_CHECK);
            else if(is_selected)       printf(BG_SELECTED);
            else if(is_cursor)         printf(BG_SELECTED);
            else if(is_legal_mv && piece && PIECE_COLOR(piece)!=g_pos.side_to_move)
                                       printf(BG_CAPTURE);
            else if(is_legal_mv)       printf(BG_LEGAL);
            else if(is_last_from||is_last_to) printf(BG_LAST_MOVE);
            else if(is_dark)           printf(BG_DARK_SQ);
            else                       printf(BG_LIGHT_SQ);

            /* Cursor border */
            if(is_cursor) printf(BOLD);

            /* Piece */
            if(piece){
                int col=IS_WHITE(piece)?0:1;
                int type=PIECE_TYPE(piece);
                if(IS_WHITE(piece)) printf(FG_BRIGHT_WHITE);
                else                printf(FG_BLACK);
                printf("%s",PIECE_UNICODE[col][type]);
            } else {
                /* Show dot for legal move targets */
                if(is_legal_mv) printf(FG_GREEN BOLD "· " RESET);
                else printf("  ");
            }
            printf(RESET);
        }

        /* Side panel — PGN moves */
        if(disp_r==7){
            printf(BOLD FG_CYAN "   Moves:" RESET);
        } else {
            /* Show pairs of moves */
            int base=(7-disp_r-1)*2; /* offset into pgn list per display row */
            /* Show last 14 moves (7 rows × 2) */
            int start_idx = g_pgn_count > 14 ? g_pgn_count-14 : 0;
            int row_in_panel = 7-disp_r-1;
            int w_idx = start_idx + row_in_panel*2;
            int b_idx = w_idx+1;
            int move_num = (start_idx/2)+1+row_in_panel;
            (void)base;
            if(w_idx < g_pgn_count){
                int mn=(w_idx/2)+1;
                printf(FG_CYAN " %2d." RESET, mn);
                printf(FG_WHITE "%-8s" RESET, g_pgn_moves[w_idx]);
                if(b_idx < g_pgn_count)
                    printf(FG_WHITE "%-8s" RESET, g_pgn_moves[b_idx]);
                else
                    printf("        ");
            } else {
                printf("                    ");
                (void)move_num;
            }
        }
        printf("\n");
    }

    /* File labels */
    printf(BOLD FG_CYAN "    ");
    for(int i=0;i<8;i++){
        int f= g_flip ? (7-i) : i;
        printf("%c " RESET BOLD FG_CYAN, 'a'+f);
    }
    printf(RESET "\n\n");

    /* ── Status / info panel ── */
    printf(BG_STATUS FG_CYAN BOLD);
    printf(" %-52s" RESET "\n", "");

    char turn_str[64];
    if(g_game_over){
        snprintf(turn_str,sizeof(turn_str)," %s",g_status_msg);
    } else {
        const char *turn=(g_pos.side_to_move==WHITE_SIDE)?"White":"Black";
        const char *chk=g_in_check?" [CHECK!]":"";
        snprintf(turn_str,sizeof(turn_str)," %s to move%s  Move: %d",
                 turn,chk,g_pos.fullmove);
    }
    printf(BG_STATUS FG_WHITE BOLD " %-52s" RESET "\n", turn_str);

    /* Engine line */
    char eng_str[64];
    if(g_engine_active){
        char tc_str[32];
        switch(g_tc.mode){
            case TC_TIME:  snprintf(tc_str,32,"%dms",g_tc.time_ms); break;
            case TC_DEPTH: snprintf(tc_str,32,"depth %d",g_tc.depth); break;
            case TC_NODES: snprintf(tc_str,32,"%ld nodes",(long)g_tc.nodes); break;
        }
        snprintf(eng_str,sizeof(eng_str)," Engine: %.30s [%s]",g_engine.name,tc_str);
    } else {
        snprintf(eng_str,sizeof(eng_str)," Engine: (none) — 2-player mode");
    }
    printf(BG_STATUS FG_CYAN " %-52s" RESET "\n", eng_str);

    printf(BG_STATUS FG_WHITE);
    printf(" Cursor: %c%d   ",'a'+g_cur_f, g_cur_r+1);
    if(g_sel_r>=0) printf("Selected: %c%d  ",'a'+g_sel_f,g_sel_r+1);
    else           printf("                ");
    printf("%-16s" RESET "\n","");

    printf(BG_STATUS FG_YELLOW BOLD);
    printf(" Keys: Arrows=move  ENTER=select  U=undo  F=flip   " RESET "\n");
    printf(BG_STATUS FG_YELLOW BOLD);
    printf(" E=engine  T=time-ctrl  N=new game  Q=quit        " RESET "\n");
    printf(RESET);

    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TERMINAL RAW MODE
   ═══════════════════════════════════════════════════════════════════════════ */
static void set_raw_mode(void)
{
    tcgetattr(STDIN_FILENO,&g_old_term);
    struct termios raw=g_old_term;
    raw.c_iflag &= ~(BRKINT|ICRNL|INPCK|ISTRIP|IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO|ICANON|IEXTEN|ISIG);
    raw.c_cc[VMIN]=0;
    raw.c_cc[VTIME]=1; /* 100ms timeout */
    tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw);
}

static void restore_terminal(void)
{
    tcsetattr(STDIN_FILENO,TCSAFLUSH,&g_old_term);
    printf(SHOW_CURSOR RESET "\n");
}

static void handle_sigchld(int sig)
{
    (void)sig;
    int status;
    while(waitpid(-1,&status,WNOHANG)>0){}
    g_engine_active=0;
    snprintf(g_status_msg,sizeof(g_status_msg),"Engine process ended.");
}

/* ═══════════════════════════════════════════════════════════════════════════
   ENGINE MENU
   ═══════════════════════════════════════════════════════════════════════════ */
static void show_engine_menu(void)
{
    restore_terminal();
    printf(CLEAR_SCREEN);
    printf(BOLD FG_CYAN "═══ Engine Setup ═══\n\n" RESET);
    printf("Enter path to UCI engine binary (or blank to disable):\n> ");
    fflush(stdout);

    char path[256]="";
    if(fgets(path,sizeof(path),stdin)){
        path[strcspn(path,"\n")]='\0';
    }

    if(strlen(path)==0){
        if(g_engine_active){
            send_engine("quit");
            close(g_engine.in_fd);
            close(g_engine.out_fd);
            kill(g_engine.pid,SIGTERM);
            g_engine_active=0;
        }
        set_raw_mode();
        printf(CLEAR_SCREEN);
        return;
    }

    /* Kill existing engine */
    if(g_engine_active){
        send_engine("quit");
        usleep(50000);
        close(g_engine.in_fd);
        close(g_engine.out_fd);
        kill(g_engine.pid,SIGTERM);
        g_engine_active=0;
    }

    /* Pipes */
    int pipe_in[2], pipe_out[2];
    if(pipe(pipe_in)<0||pipe(pipe_out)<0){
        printf("pipe() failed\n"); sleep(1); set_raw_mode(); return;
    }

    pid_t pid=fork();
    if(pid<0){ printf("fork() failed\n"); sleep(1); set_raw_mode(); return; }

    if(pid==0){
        /* Child */
        dup2(pipe_in[0],STDIN_FILENO);
        dup2(pipe_out[1],STDOUT_FILENO);
        close(pipe_in[1]); close(pipe_out[0]);
        close(pipe_in[0]); close(pipe_out[1]);
        execlp(path,path,(char*)NULL);
        exit(1);
    }

    /* Parent */
    close(pipe_in[0]); close(pipe_out[1]);
    g_engine.pid=pid;
    g_engine.in_fd=pipe_in[1];
    g_engine.out_fd=pipe_out[0];

    /* Non-blocking read */
    fcntl(g_engine.out_fd,F_SETFL,O_NONBLOCK);

    /* UCI handshake */
    send_engine("uci");
    char buf[512];
    strncpy(g_engine.name,path,127);
    for(int i=0;i<200;i++){
        char *line=read_engine_line(buf,sizeof(buf),20);
        if(!line) continue;
        if(strncmp(line,"id name",7)==0){
            strncpy(g_engine.name,line+8,127);
        }
        if(strcmp(line,"uciok")==0) break;
    }
    send_engine("isready");
    for(int i=0;i<200;i++){
        char *line=read_engine_line(buf,sizeof(buf),20);
        if(line && strcmp(line,"readyok")==0) break;
    }
    send_engine("ucinewgame");

    g_engine_active=1;
    g_engine.ready=1;
    snprintf(g_status_msg,sizeof(g_status_msg),"Engine loaded: %.40s",g_engine.name);

    set_raw_mode();
    printf(CLEAR_SCREEN);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TIME CONTROL MENU
   ═══════════════════════════════════════════════════════════════════════════ */
static void show_tc_menu(void)
{
    restore_terminal();
    printf(CLEAR_SCREEN);
    printf(BOLD FG_CYAN "═══ Time Control ═══\n\n" RESET);
    printf("1) Time per move (ms)  [current: %d ms]\n", g_tc.time_ms);
    printf("2) Fixed depth         [current: %d]\n",    g_tc.depth);
    printf("3) Fixed nodes         [current: %ld]\n",   (long)g_tc.nodes);
    printf("\nMode (1/2/3): ");
    fflush(stdout);

    char inp[16]="";
    if(fgets(inp,sizeof(inp),stdin)) inp[strcspn(inp,"\n")]='\0';
    int choice=atoi(inp);

    if(choice==1){
        printf("Time per move (ms): ");
        fflush(stdout);
        if(fgets(inp,sizeof(inp),stdin)) inp[strcspn(inp,"\n")]='\0';
        int v=atoi(inp);
        if(v>0){ g_tc.mode=TC_TIME; g_tc.time_ms=v; }
    } else if(choice==2){
        printf("Depth: ");
        fflush(stdout);
        if(fgets(inp,sizeof(inp),stdin)) inp[strcspn(inp,"\n")]='\0';
        int v=atoi(inp);
        if(v>0){ g_tc.mode=TC_DEPTH; g_tc.depth=v; }
    } else if(choice==3){
        printf("Nodes: ");
        fflush(stdout);
        if(fgets(inp,sizeof(inp),stdin)) inp[strcspn(inp,"\n")]='\0';
        long v=atol(inp);
        if(v>0){ g_tc.mode=TC_NODES; g_tc.nodes=v; }
    }

    set_raw_mode();
    printf(CLEAR_SCREEN);
}

/* ═══════════════════════════════════════════════════════════════════════════
   NEW GAME
   ═══════════════════════════════════════════════════════════════════════════ */
static void new_game(void)
{
    init_position(&g_pos);
    g_hist_len=0; g_pgn_count=0;
    g_sel_r=-1; g_sel_f=-1; g_nlegal=0;
    g_last_from_r=-1; g_last_from_f=-1;
    g_last_to_r=-1; g_last_to_f=-1;
    g_in_check=0; g_game_over=0;
    snprintf(g_status_msg,sizeof(g_status_msg),"New game started.");
    if(g_engine_active) send_engine("ucinewgame");
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN UI LOOP
   ═══════════════════════════════════════════════════════════════════════════ */
static void run_ui_loop(void)
{
    int engine_thinking=0;
    char eng_buf[512];

    while(1){
        render();

        /* If engine should move */
        if(!g_game_over && g_engine_active && !engine_thinking &&
           g_pos.side_to_move==BLACK_SIDE){
            engine_go();
            engine_thinking=1;
        }

        /* Read engine output (non-blocking) */
        if(engine_thinking && g_engine_active){
            char *line=read_engine_line(eng_buf,sizeof(eng_buf),50);
            if(line && strncmp(line,"bestmove",8)==0){
                Move em; memset(&em,0,sizeof(em));
                parse_engine_best_move(line,&em);
                char uci[8];
                snprintf(uci,sizeof(uci),"%c%d%c%d",
                         'a'+em.from_f,'1'+em.from_r,
                         'a'+em.to_f,'1'+em.to_r);
                if(em.promo){
                    static const char pc[]=" pnbrqk";
                    char pstr[2]={pc[em.promo],'\0'};
                    strncat(uci,pstr,1);
                }
                find_and_apply_move(uci);
                engine_thinking=0;
                check_game_over();
                render();
                continue;
            }
        }

        /* Read keyboard */
        unsigned char c=0;
        int r=read(STDIN_FILENO,&c,1);
        if(r<=0) continue;

        if(c=='\033'){
            /* Escape sequence */
            unsigned char seq[3]={0,0,0};
            read(STDIN_FILENO,&seq[0],1);
            read(STDIN_FILENO,&seq[1],1);
            if(seq[0]=='['){
                int vis_r = g_flip ? (7-g_cur_r) : g_cur_r;
                int vis_f = g_flip ? (7-g_cur_f) : g_cur_f;
                if(seq[1]=='A')      { if(vis_r<7) vis_r++; else vis_r=0; } /* Up */
                else if(seq[1]=='B'){ if(vis_r>0) vis_r--; else vis_r=7; } /* Down */
                else if(seq[1]=='C'){ if(vis_f<7) vis_f++; else vis_f=0; } /* Right */
                else if(seq[1]=='D'){ if(vis_f>0) vis_f--; else vis_f=7; } /* Left */
                g_cur_r = g_flip ? (7-vis_r) : vis_r;
                g_cur_f = g_flip ? (7-vis_f) : vis_f;
            }
            continue;
        }

        /* Printable keys */
        char key=tolower((char)c);

        if(key=='q'){ break; }

        else if(key=='n'){
            new_game();
        }
        else if(key=='u'){
            undo_last_move();
            engine_thinking=0;
        }
        else if(key=='f'){
            g_flip=!g_flip;
        }
        else if(key=='e'){
            engine_thinking=0;
            show_engine_menu();
        }
        else if(key=='t'){
            show_tc_menu();
        }
        else if(c=='\r'||c=='\n'||c==' '){
            /* Select / move */
            if(g_game_over) continue;
            /* Only allow human to move if not waiting for engine */
            if(engine_thinking) continue;
            /* In engine mode, human plays white only */
            if(g_engine_active && g_pos.side_to_move!=WHITE_SIDE) continue;

            int pr=g_cur_r, pf=g_cur_f;
            int piece=g_pos.board[pr][pf];

            if(g_sel_r<0){
                /* Select */
                if(piece && PIECE_COLOR(piece)==g_pos.side_to_move){
                    g_sel_r=pr; g_sel_f=pf;
                    legal_moves_for_square(pr,pf);
                }
            } else {
                if(pr==g_sel_r && pf==g_sel_f){
                    /* Deselect */
                    g_sel_r=-1; g_sel_f=-1; g_nlegal=0;
                } else if(is_legal_target(pr,pf)){
                    /* Move */
                    apply_player_move(g_sel_r,g_sel_f,pr,pf);
                    g_sel_r=-1; g_sel_f=-1; g_nlegal=0;
                    check_game_over();
                } else if(piece && PIECE_COLOR(piece)==g_pos.side_to_move){
                    /* Re-select */
                    g_sel_r=pr; g_sel_f=pf;
                    legal_moves_for_square(pr,pf);
                } else {
                    g_sel_r=-1; g_sel_f=-1; g_nlegal=0;
                }
            }
        }

        /* WASD / hjkl alternative navigation */
        else if(key=='w'||key=='k'){
            int vis_r = g_flip ? (7-g_cur_r) : g_cur_r;
            if(vis_r<7) vis_r++; else vis_r=0;
            g_cur_r = g_flip ? (7-vis_r) : vis_r;
        }
        else if(key=='s'||key=='j'){
            int vis_r = g_flip ? (7-g_cur_r) : g_cur_r;
            if(vis_r>0) vis_r--; else vis_r=7;
            g_cur_r = g_flip ? (7-vis_r) : vis_r;
        }
        else if(key=='d'||key=='l'){
            int vis_f = g_flip ? (7-g_cur_f) : g_cur_f;
            if(vis_f<7) vis_f++; else vis_f=0;
            g_cur_f = g_flip ? (7-vis_f) : vis_f;
        }
        else if(key=='a'||key=='h'){
            int vis_f = g_flip ? (7-g_cur_f) : g_cur_f;
            if(vis_f>0) vis_f--; else vis_f=7;
            g_cur_f = g_flip ? (7-vis_f) : vis_f;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    signal(SIGCHLD, handle_sigchld);

    init_position(&g_pos);
    snprintf(g_status_msg,sizeof(g_status_msg),
             "Welcome! Press E to load engine, T for time controls.");

    set_raw_mode();
    printf(HIDE_CURSOR);
    printf(CLEAR_SCREEN);

    run_ui_loop();

    /* Cleanup */
    if(g_engine_active){
        send_engine("quit");
        usleep(50000);
        close(g_engine.in_fd);
        close(g_engine.out_fd);
        kill(g_engine.pid,SIGTERM);
    }
    restore_terminal();
    printf("Thanks for playing!\n");
    return 0;
}
