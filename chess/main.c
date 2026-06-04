#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>

/* ─── Constants ─────────────────────────────────────────────── */
#define MAX_MOVES      512
#define MAX_PGN        8192
#define MAX_HIST       256
#define ENGINE_BUF     4096
#define MOVE_STR       6
#define MAX_LEGAL      256

/* Piece codes (positive = White, negative = Black, 0 = empty) */
#define EMPTY   0
#define PAWN    1
#define KNIGHT  2
#define BISHOP  3
#define ROOK    4
#define QUEEN   5
#define KING    6

/* Colors */
#define WHITE   1
#define BLACK  -1

/* ANSI color codes */
#define ANSI_RESET       "\x1b[0m"
#define ANSI_BOLD        "\x1b[1m"
#define ANSI_BG_DARK     "\x1b[48;5;94m"
#define ANSI_BG_LIGHT    "\x1b[48;5;223m"
#define ANSI_BG_SELECT   "\x1b[48;5;28m"
#define ANSI_BG_LEGAL    "\x1b[48;5;22m"
#define ANSI_BG_LASTMV   "\x1b[48;5;58m"
#define ANSI_BG_CHECK    "\x1b[48;5;160m"
#define ANSI_FG_WHITE    "\x1b[97m"
#define ANSI_FG_BLACK    "\x1b[30m"
#define ANSI_FG_CYAN     "\x1b[96m"
#define ANSI_FG_YELLOW   "\x1b[93m"
#define ANSI_FG_GREEN    "\x1b[92m"
#define ANSI_FG_RED      "\x1b[91m"
#define ANSI_FG_MAGENTA  "\x1b[95m"
#define ANSI_FG_GRAY     "\x1b[90m"
#define ANSI_CLEAR       "\x1b[2J\x1b[H"
#define ANSI_HIDE_CURSOR "\x1b[?25l"
#define ANSI_SHOW_CURSOR "\x1b[?25h"

/* ─── Data structures ────────────────────────────────────────── */

typedef struct {
    int board[8][8];          /* board[rank][file], rank 0=rank1, rank 7=rank8 */
    int side;                 /* whose turn: WHITE or BLACK */
    int castling;             /* bits: 0=WK,1=WQ,2=BK,3=BQ */
    int ep_file;              /* en passant target file (-1 if none) */
    int halfmove;
    int fullmove;
} Position;

typedef struct {
    int from_rank, from_file;
    int to_rank,   to_file;
    int promo;                /* 0 or piece code */
    int captured;
    int flags;                /* special move flags */
    /* saved state for undo */
    int castling_before;
    int ep_file_before;
    int halfmove_before;
    char uci[MOVE_STR];       /* e.g. "e2e4" */
    char san[16];             /* SAN notation */
} Move;

/* Move flags */
#define FLAG_CASTLE_K   1
#define FLAG_CASTLE_Q   2
#define FLAG_EP         4
#define FLAG_PROMO      8

typedef struct {
    Position pos;
    Move     move;            /* move that led to this position */
} HistEntry;

typedef struct {
    pid_t  pid;
    int    in_fd;             /* GUI writes to engine */
    int    out_fd;            /* GUI reads from engine */
    char   buf[ENGINE_BUF];
    int    buf_len;
    int    ready;
    char   name[128];
} Engine;

/* ─── Globals ────────────────────────────────────────────────── */

static Position   g_pos;
static HistEntry  g_history[MAX_HIST];
static int        g_hist_len  = 0;
static Move       g_legal[MAX_LEGAL];
static int        g_nlegal    = 0;

static int        g_cursor_rank = 0;
static int        g_cursor_file = 4;
static int        g_sel_rank    = -1;
static int        g_sel_file    = -1;
static int        g_selected    = 0;

static Engine     g_engine;
static int        g_engine_active = 0;
static int        g_engine_side   = BLACK; /* engine plays BLACK by default */

static char       g_pgn[MAX_PGN];
static int        g_pgn_len = 0;

static char       g_status[256];
static int        g_game_over = 0;

static struct termios g_orig_termios;
static int        g_flip_board = 0; /* 0=white at bottom */

/* ─── Forward declarations ───────────────────────────────────── */
void render(void);
int  is_in_check(const Position *p, int side);
void generate_legal_moves(const Position *p, Move *moves, int *n);
void apply_move(Position *p, const Move *m);
int  move_from_uci(const Position *p, const char *uci, Move *out);
void build_pgn_move(const Position *before, const Move *m, char *out);
void pgn_append(const Move *m, int move_num, int side);
void engine_send(const char *msg);
void engine_start(const char *path);
void engine_go(void);
int engine_read_response(int);
void cleanup(void);

/* ─── Terminal helpers ───────────────────────────────────────── */

void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf(ANSI_HIDE_CURSOR);
}

void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    printf(ANSI_SHOW_CURSOR);
}

void cleanup(void) {
    disable_raw_mode();
    if (g_engine_active) {
        engine_send("quit\n");
        waitpid(g_engine.pid, NULL, 0);
    }
    printf(ANSI_RESET "\n");
}

/* ─── Position helpers ───────────────────────────────────────── */

void position_start(Position *p) {
    memset(p, 0, sizeof(*p));
    /* Rank 0 = rank 1 (White's back rank) */
    static const int back[] = {ROOK,KNIGHT,BISHOP,QUEEN,KING,BISHOP,KNIGHT,ROOK};
    for (int f = 0; f < 8; f++) {
        p->board[0][f] =  back[f];
        p->board[1][f] =  PAWN;
        p->board[6][f] = -PAWN;
        p->board[7][f] = -back[f];
    }
    p->side     = WHITE;
    p->castling = 0xF;
    p->ep_file  = -1;
    p->halfmove = 0;
    p->fullmove = 1;
}

int piece_color(int pc) {
    return (pc > 0) ? WHITE : (pc < 0) ? BLACK : 0;
}

/* ─── Move generation helpers ───────────────────────────────── */

static int on_board(int r, int f) { return r>=0&&r<8&&f>=0&&f<8; }

static void add_move(Move *moves, int *n,
                     const Position *p,
                     int fr,int ff,int tr,int tf,
                     int promo,int flags) {
    if (*n >= MAX_LEGAL) return;
    Move *m = &moves[*n];
    memset(m, 0, sizeof(*m));
    m->from_rank = fr; m->from_file = ff;
    m->to_rank   = tr; m->to_file   = tf;
    m->promo     = promo;
    m->flags     = flags;
    m->captured  = p->board[tr][tf];
    if (flags & FLAG_EP)
        m->captured = p->board[fr][tf]; /* ep capture */
    /* Build UCI string */
    m->uci[0] = 'a' + ff;
    m->uci[1] = '1' + fr;
    m->uci[2] = 'a' + tf;
    m->uci[3] = '1' + tr;
    if (promo) {
        static const char pchars[] = ".pnbrqk";
        m->uci[4] = pchars[promo];
        m->uci[5] = '\0';
    } else {
        m->uci[4] = '\0';
    }
    (*n)++;
}

/* Generate pseudo-legal moves for one piece */
static void gen_piece_moves(const Position *p, int r, int f, Move *moves, int *n) {
    int pc   = p->board[r][f];
    int side = piece_color(pc);
    int type = abs(pc);

    switch (type) {
    case PAWN: {
        int dir = (side == WHITE) ? 1 : -1;
        int start_rank = (side == WHITE) ? 1 : 6;
        int promo_rank = (side == WHITE) ? 7 : 0;
        /* Forward */
        int nr = r + dir;
        if (on_board(nr,f) && p->board[nr][f] == EMPTY) {
            if (nr == promo_rank) {
                add_move(moves,n,p,r,f,nr,f,QUEEN, FLAG_PROMO);
                add_move(moves,n,p,r,f,nr,f,ROOK,  FLAG_PROMO);
                add_move(moves,n,p,r,f,nr,f,BISHOP,FLAG_PROMO);
                add_move(moves,n,p,r,f,nr,f,KNIGHT,FLAG_PROMO);
            } else {
                add_move(moves,n,p,r,f,nr,f,0,0);
                /* Double push */
                if (r == start_rank && p->board[r+2*dir][f] == EMPTY)
                    add_move(moves,n,p,r,f,r+2*dir,f,0,0);
            }
        }
        /* Captures */
        for (int df = -1; df <= 1; df += 2) {
            int nf = f + df;
            if (!on_board(nr,nf)) continue;
            int target = p->board[nr][nf];
            if (target != EMPTY && piece_color(target) != side) {
                if (nr == promo_rank) {
                    add_move(moves,n,p,r,f,nr,nf,QUEEN, FLAG_PROMO);
                    add_move(moves,n,p,r,f,nr,nf,ROOK,  FLAG_PROMO);
                    add_move(moves,n,p,r,f,nr,nf,BISHOP,FLAG_PROMO);
                    add_move(moves,n,p,r,f,nr,nf,KNIGHT,FLAG_PROMO);
                } else {
                    add_move(moves,n,p,r,f,nr,nf,0,0);
                }
            }
            /* En passant */
            if (p->ep_file == nf && r == (side==WHITE ? 4 : 3)) {
                add_move(moves,n,p,r,f,nr,nf,0,FLAG_EP);
            }
        }
        break;
    }
    case KNIGHT: {
        static const int kd[8][2]={{2,1},{2,-1},{-2,1},{-2,-1},{1,2},{1,-2},{-1,2},{-1,-2}};
        for (int i=0;i<8;i++){
            int nr=r+kd[i][0], nf=f+kd[i][1];
            if (!on_board(nr,nf)) continue;
            int t=p->board[nr][nf];
            if (t==EMPTY || piece_color(t)!=side)
                add_move(moves,n,p,r,f,nr,nf,0,0);
        }
        break;
    }
    case BISHOP:
    case ROOK:
    case QUEEN: {
        static const int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        int start = (type==BISHOP)?4 : (type==ROOK)?0 : 0;
        int end   = (type==BISHOP)?8 : (type==ROOK)?4 : 8;
        for (int d=start;d<end;d++){
            for (int s=1;s<8;s++){
                int nr=r+s*dirs[d][0], nf=f+s*dirs[d][1];
                if (!on_board(nr,nf)) break;
                int t=p->board[nr][nf];
                if (t==EMPTY){ add_move(moves,n,p,r,f,nr,nf,0,0); continue; }
                if (piece_color(t)!=side) add_move(moves,n,p,r,f,nr,nf,0,0);
                break;
            }
        }
        break;
    }
    case KING: {
        static const int kd[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        for (int i=0;i<8;i++){
            int nr=r+kd[i][0], nf=f+kd[i][1];
            if (!on_board(nr,nf)) continue;
            int t=p->board[nr][nf];
            if (t==EMPTY || piece_color(t)!=side)
                add_move(moves,n,p,r,f,nr,nf,0,0);
        }
        /* Castling */
        int back_rank = (side==WHITE)?0:7;
        if (r==back_rank && f==4) {
            /* Kingside */
            int km = (side==WHITE)?0:2;
            if ((p->castling>>km)&1) {
                if (p->board[back_rank][5]==EMPTY && p->board[back_rank][6]==EMPTY)
                    add_move(moves,n,p,r,f,back_rank,6,0,FLAG_CASTLE_K);
            }
            /* Queenside */
            int qm = (side==WHITE)?1:3;
            if ((p->castling>>qm)&1) {
                if (p->board[back_rank][3]==EMPTY && p->board[back_rank][2]==EMPTY && p->board[back_rank][1]==EMPTY)
                    add_move(moves,n,p,r,f,back_rank,2,0,FLAG_CASTLE_Q);
            }
        }
        break;
    }
    }
}

/* Check if square (r,f) is attacked by 'attacker' side */
int is_attacked(const Position *p, int r, int f, int attacker) {
    /* Knight */
    static const int kn[8][2]={{2,1},{2,-1},{-2,1},{-2,-1},{1,2},{1,-2},{-1,2},{-1,-2}};
    for (int i=0;i<8;i++){
        int nr=r+kn[i][0], nf=f+kn[i][1];
        if (!on_board(nr,nf)) continue;
        if (p->board[nr][nf] == attacker*KNIGHT) return 1;
    }
    /* Sliding pieces + queen */
    static const int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int d=0;d<8;d++){
        for (int s=1;s<8;s++){
            int nr=r+s*dirs[d][0], nf=f+s*dirs[d][1];
            if (!on_board(nr,nf)) break;
            int pc=p->board[nr][nf];
            if (pc==EMPTY) continue;
            if (piece_color(pc)!=attacker) break;
            int t=abs(pc);
            if (t==QUEEN) { goto attacked; }
            if (d<4 && t==ROOK)   { goto attacked; }
            if (d>=4 && t==BISHOP){ goto attacked; }
            break;
attacked:   return 1;
        }
    }
    /* Pawns */
    int pdir = (attacker==WHITE)?-1:1; /* pawn attacks from below if white */
    for (int df=-1;df<=1;df+=2){
        int nr=r+pdir, nf=f+df;
        if (!on_board(nr,nf)) continue;
        if (p->board[nr][nf]==attacker*PAWN) return 1;
    }
    /* King */
    static const int kd[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int i=0;i<8;i++){
        int nr=r+kd[i][0], nf=f+kd[i][1];
        if (!on_board(nr,nf)) continue;
        if (p->board[nr][nf]==attacker*KING) return 1;
    }
    return 0;
}

int is_in_check(const Position *p, int side) {
    /* Find king */
    int kr=-1,kf=-1;
    for (int r=0;r<8;r++) for (int f=0;f<8;f++)
        if (p->board[r][f]==side*KING){kr=r;kf=f;}
    if (kr<0) return 0;
    return is_attacked(p,kr,kf,-side);
}

void apply_move(Position *p, const Move *m) {
    int pc = p->board[m->from_rank][m->from_file];
    int side = piece_color(pc);

    /* Move piece */
    p->board[m->to_rank][m->to_file]     = pc;
    p->board[m->from_rank][m->from_file] = EMPTY;

    /* Promotion */
    if (m->flags & FLAG_PROMO)
        p->board[m->to_rank][m->to_file] = side * m->promo;

    /* En passant capture */
    if (m->flags & FLAG_EP)
        p->board[m->from_rank][m->to_file] = EMPTY;

    /* Castling rook move */
    if (m->flags & FLAG_CASTLE_K) {
        int rank = m->from_rank;
        p->board[rank][5] = p->board[rank][7];
        p->board[rank][7] = EMPTY;
    }
    if (m->flags & FLAG_CASTLE_Q) {
        int rank = m->from_rank;
        p->board[rank][3] = p->board[rank][0];
        p->board[rank][0] = EMPTY;
    }

    /* Update castling rights */
    if (abs(pc)==KING) {
        if (side==WHITE) p->castling &= ~0x3;
        else             p->castling &= ~0xC;
    }
    if (abs(pc)==ROOK) {
        if (side==WHITE) {
            if (m->from_file==7 && m->from_rank==0) p->castling &= ~0x1;
            if (m->from_file==0 && m->from_rank==0) p->castling &= ~0x2;
        } else {
            if (m->from_file==7 && m->from_rank==7) p->castling &= ~0x4;
            if (m->from_file==0 && m->from_rank==7) p->castling &= ~0x8;
        }
    }
    /* If rook was captured on its home square */
    if (m->to_rank==0 && m->to_file==7) p->castling &= ~0x1;
    if (m->to_rank==0 && m->to_file==0) p->castling &= ~0x2;
    if (m->to_rank==7 && m->to_file==7) p->castling &= ~0x4;
    if (m->to_rank==7 && m->to_file==0) p->castling &= ~0x8;

    /* En passant file */
    p->ep_file = -1;
    if (abs(pc)==PAWN && abs(m->to_rank-m->from_rank)==2)
        p->ep_file = m->from_file;

    /* Half/full move */
    if (abs(pc)==PAWN || m->captured)
        p->halfmove = 0;
    else
        p->halfmove++;
    if (side==BLACK) p->fullmove++;

    p->side = -side;
}

void generate_legal_moves(const Position *p, Move *moves, int *n) {
    Move pseudo[MAX_LEGAL*4];
    int  np = 0;
    *n = 0;
    for (int r=0;r<8;r++) for (int f=0;f<8;f++){
        if (piece_color(p->board[r][f])==p->side)
            gen_piece_moves(p,r,f,pseudo,&np);
    }
    for (int i=0;i<np;i++){
        /* Check castling legality: king cannot pass through check */
        if (pseudo[i].flags & FLAG_CASTLE_K){
            int back=pseudo[i].from_rank;
            if (is_attacked(p,back,4,-p->side)||
                is_attacked(p,back,5,-p->side)||
                is_attacked(p,back,6,-p->side)) continue;
        }
        if (pseudo[i].flags & FLAG_CASTLE_Q){
            int back=pseudo[i].from_rank;
            if (is_attacked(p,back,4,-p->side)||
                is_attacked(p,back,3,-p->side)||
                is_attacked(p,back,2,-p->side)) continue;
        }
        Position tmp = *p;
        apply_move(&tmp, &pseudo[i]);
        if (!is_in_check(&tmp, p->side))
            moves[(*n)++] = pseudo[i];
    }
}

/* ─── SAN / PGN ──────────────────────────────────────────────── */

static const char *piece_char = ".PNBRQK";

void build_san(const Position *before, const Move *m, char *out) {
    int pc   = before->board[m->from_rank][m->from_file];
    int type = abs(pc);
    int side = piece_color(pc);
    char buf[32]; int bp=0;

    if (m->flags & FLAG_CASTLE_K){ strcpy(out,"O-O");   goto check_suffix; }
    if (m->flags & FLAG_CASTLE_Q){ strcpy(out,"O-O-O"); goto check_suffix; }

    if (type != PAWN)
        buf[bp++] = piece_char[type];

    /* Disambiguation */
    if (type != PAWN) {
        Move all[MAX_LEGAL]; int na=0;
        generate_legal_moves(before, all, &na);
        int ambig_file=0, ambig_rank=0;
        for (int i=0;i<na;i++){
            if (&all[i]==m) continue;
            if (all[i].to_rank==m->to_rank && all[i].to_file==m->to_file &&
                abs(before->board[all[i].from_rank][all[i].from_file])==type){
                if (all[i].from_file==m->from_file) ambig_rank=1;
                else ambig_file=1;
            }
        }
        if (ambig_file || ambig_rank) {
            if (!ambig_file) buf[bp++]='a'+m->from_file;
            else if (!ambig_rank) buf[bp++]='1'+m->from_rank;
            else { buf[bp++]='a'+m->from_file; buf[bp++]='1'+m->from_rank; }
        }
    }

    /* Pawn capture file */
    if (type==PAWN && m->from_file!=m->to_file)
        buf[bp++]='a'+m->from_file;

    /* Capture */
    if (m->captured || (m->flags&FLAG_EP))
        buf[bp++]='x';

    buf[bp++]='a'+m->to_file;
    buf[bp++]='1'+m->to_rank;

    if (m->flags & FLAG_PROMO){
        buf[bp++]='=';
        buf[bp++]=piece_char[m->promo];
    }
    buf[bp]='\0';
    strcpy(out,buf);

check_suffix: {
    /* Apply move and check result */
    Position tmp = *before;
    apply_move(&tmp, m);
    Move reply[MAX_LEGAL]; int nr=0;
    generate_legal_moves(&tmp, reply, &nr);
    if (is_in_check(&tmp, tmp.side)){
        if (nr==0) strcat(out,"#");
        else       strcat(out,"+");
    }
  }
}

void pgn_append(const Move *m, int move_num, int side) {
    char token[32]="";
    if (side==WHITE){
        char num[8]; sprintf(num,"%d.",move_num);
        strcat(token,num);
    }
    strcat(token,m->san);
    strcat(token," ");
    int tlen=(int)strlen(token);
    if (g_pgn_len+tlen < MAX_PGN-2){
        memcpy(g_pgn+g_pgn_len, token, tlen);
        g_pgn_len+=tlen;
        g_pgn[g_pgn_len]='\0';
    }
}

/* ─── Board rendering ────────────────────────────────────────── */

/* Unicode chess pieces */
static const char *piece_unicode[2][7] = {
    /* empty, pawn, knight, bishop, rook, queen, king */
    {"  ","♟ ","♞ ","♝ ","♜ ","♛ ","♚ "}, /* BLACK pieces */
    {"  ","♙ ","♘ ","♗ ","♖ ","♕ ","♔ "}, /* WHITE pieces */
};

int vis_rank(int r){ return g_flip_board ? r : 7-r; }
int vis_file(int f){ return g_flip_board ? 7-f : f; }
int board_rank(int vr){ return g_flip_board ? vr : 7-vr; }
int board_file(int vf){ return g_flip_board ? 7-vf : vf; }

void render(void) {
    printf(ANSI_CLEAR);

    /* Title */
    printf(ANSI_BOLD ANSI_FG_CYAN
           "  ╔══════════════════════════════════════════╗\n"
           "  ║         TERMINAL CHESS  v1.0             ║\n"
           "  ╚══════════════════════════════════════════╝\n"
           ANSI_RESET "\n");

    /* Last move for highlighting */
    int lm_fr=-1,lm_ff=-1,lm_tr=-1,lm_tf=-1;
    if (g_hist_len>0){
        lm_fr=g_history[g_hist_len-1].move.from_rank;
        lm_ff=g_history[g_hist_len-1].move.from_file;
        lm_tr=g_history[g_hist_len-1].move.to_rank;
        lm_tf=g_history[g_hist_len-1].move.to_file;
    }

    /* Build legal target set for highlighting */
    int legal_sq[8][8];
    memset(legal_sq,0,sizeof(legal_sq));
    if (g_selected){
        for (int i=0;i<g_nlegal;i++){
            if (g_legal[i].from_rank==g_sel_rank && g_legal[i].from_file==g_sel_file)
                legal_sq[g_legal[i].to_rank][g_legal[i].to_file]=1;
        }
    }

    /* Column labels */
    printf("    ");
    for (int vf=0;vf<8;vf++){
        int f=board_file(vf);
        printf(ANSI_FG_YELLOW ANSI_BOLD " %c " ANSI_RESET, 'a'+f);
    }
    printf("\n");
    printf("  ┌");
    for(int i=0;i<8;i++) printf("───");
    printf("┐\n");

    for (int vr=0;vr<8;vr++){
        int r=board_rank(vr);
        printf(ANSI_FG_YELLOW ANSI_BOLD "%d " ANSI_RESET, r+1);
        printf("│");
        for (int vf=0;vf<8;vf++){
            int f=board_file(vf);
            int pc=g_pos.board[r][f];
            int is_cursor=(r==g_cursor_rank && f==g_cursor_file);
            int is_sel   =(r==g_sel_rank    && f==g_sel_file && g_selected);
            int is_legal = legal_sq[r][f];
            int is_lmfr  =(r==lm_fr && f==lm_ff);
            int is_lmto  =(r==lm_tr && f==lm_tf);
            int is_chk   = 0;
            if (abs(pc)==KING && piece_color(pc)==g_pos.side && is_in_check(&g_pos,g_pos.side))
                is_chk=1;

            /* Choose background */
            int light = (r+f)%2==0;
            const char *bg;
            if      (is_cursor) bg = "\x1b[48;5;39m";
            else if (is_sel)    bg = ANSI_BG_SELECT;
            else if (is_legal && legal_sq[r][f]) {
                bg = pc ? "\x1b[48;5;130m" : ANSI_BG_LEGAL;
            }
            else if (is_chk)    bg = ANSI_BG_CHECK;
            else if (is_lmfr || is_lmto) bg = ANSI_BG_LASTMV;
            else bg = light ? ANSI_BG_LIGHT : ANSI_BG_DARK;

            /* Choose foreground based on piece color */
            const char *fg;
            if (pc>0) fg = ANSI_FG_WHITE;
            else       fg = "\x1b[38;5;232m";

            /* Piece symbol */
            const char *sym = "  ";
            if (pc!=0){
                int idx=(pc>0)?1:0;
                sym=piece_unicode[idx][abs(pc)];
            }
            /* Legal move dot overlay */
            if (is_legal && pc==EMPTY)
                printf("%s%s · " ANSI_RESET, bg, fg);
            else if (is_legal && pc!=EMPTY)
                printf("%s" "\x1b[38;5;196m" "(%s)" ANSI_RESET, bg, sym);
            else
                printf("%s%s%s" ANSI_RESET, bg, fg, sym);
        }
        printf("│" ANSI_FG_YELLOW ANSI_BOLD " %d" ANSI_RESET, r+1);

        /* Side panel */
        if (vr==0)
            printf("   " ANSI_FG_CYAN ANSI_BOLD "Controls:" ANSI_RESET);
        else if (vr==1)
            printf("   " ANSI_FG_GREEN "Arrow/WASD" ANSI_RESET " - Move cursor");
        else if (vr==2)
            printf("   " ANSI_FG_GREEN "ENTER/Space" ANSI_RESET " - Select/Move");
        else if (vr==3)
            printf("   " ANSI_FG_GREEN "U" ANSI_RESET "         - Undo move");
        else if (vr==4)
            printf("   " ANSI_FG_GREEN "F" ANSI_RESET "         - Flip board");
        else if (vr==5)
            printf("   " ANSI_FG_GREEN "Q" ANSI_RESET "         - Quit");
        else if (vr==6){
            printf("   Turn: ");
            if (g_pos.side==WHITE) printf(ANSI_FG_WHITE ANSI_BOLD "White" ANSI_RESET);
            else                   printf(ANSI_FG_GRAY  ANSI_BOLD "Black" ANSI_RESET);
        } else if (vr==7){
            if (g_engine_active)
                printf("   Engine: " ANSI_FG_GREEN "%s" ANSI_RESET, g_engine.name);
        }
        printf("\n");
    }

    printf("  └");
    for(int i=0;i<8;i++) printf("───");
    printf("┘\n");
    printf("    ");
    for (int vf=0;vf<8;vf++){
        int f=board_file(vf);
        printf(ANSI_FG_YELLOW ANSI_BOLD " %c " ANSI_RESET,'a'+f);
    }
    printf("\n\n");

    /* Status line */
    if (strlen(g_status))
        printf("  " ANSI_FG_RED ANSI_BOLD "▶ %s" ANSI_RESET "\n\n", g_status);
    else
        printf("\n");

    /* PGN moves display */
    printf(ANSI_FG_CYAN ANSI_BOLD "  PGN Moves:\n" ANSI_RESET);
    printf("  ┌─────────────────────────────────────────┐\n");
    /* Word-wrap pgn at 43 chars */
    char pgn_display[MAX_PGN];
    strncpy(pgn_display, g_pgn, MAX_PGN-1);
    pgn_display[MAX_PGN-1]='\0';
    int pg_len=(int)strlen(pgn_display);
    int pos=0, line_len=0;
    printf("  │ ");
    while(pos<pg_len){
        /* find next word */
        int word_start=pos;
        while(pos<pg_len && pgn_display[pos]!=' ') pos++;
        int word_len=pos-word_start;
        if(line_len+word_len+1 > 41){
            /* pad and newline */
            for(int s=line_len;s<41;s++) printf(" ");
            printf(" │\n  │ ");
            line_len=0;
        }
        for(int i=word_start;i<pos;i++) printf("%c",pgn_display[i]);
        printf(" ");
        line_len+=word_len+1;
        while(pos<pg_len && pgn_display[pos]==' ') pos++;
    }
    for(int s=line_len;s<41;s++) printf(" ");
    printf(" │\n");
    printf("  └─────────────────────────────────────────┘\n");

    /* Game over message */
    if (g_game_over){
        printf("\n  " ANSI_FG_YELLOW ANSI_BOLD "══ %s ══" ANSI_RESET "\n", g_status);
    }

    fflush(stdout);
}

/* ─── UCI Engine interface ───────────────────────────────────── */

void engine_send(const char *msg) {
    if (!g_engine_active) return;
    write(g_engine.in_fd, msg, strlen(msg));
}

void engine_start(const char *path) {
    int to_engine[2], from_engine[2];
    if (pipe(to_engine)||pipe(from_engine)){
        fprintf(stderr,"pipe failed\n"); return;
    }
    pid_t pid=fork();
    if (pid==0){
        /* child */
        dup2(to_engine[0],   STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]); close(from_engine[0]);
        execlp(path, path, NULL);
        exit(1);
    }
    close(to_engine[0]); close(from_engine[1]);
    g_engine.pid    = pid;
    g_engine.in_fd  = to_engine[1];
    g_engine.out_fd = from_engine[0];
    g_engine.buf_len= 0;
    g_engine.ready  = 0;
    g_engine_active = 1;

    /* Set non-blocking */
    fcntl(g_engine.out_fd, F_SETFL, O_NONBLOCK);

    engine_send("uci\n");
    /* Wait for uciok */
    char rbuf[1024]; int got;
    time_t start=time(NULL);
    while(time(NULL)-start<5){
        got=(int)read(g_engine.out_fd, rbuf, sizeof(rbuf)-1);
        if(got>0){
            rbuf[got]='\0';
            if(strstr(rbuf,"uciok")){
                /* Extract name */
                char *nm=strstr(rbuf,"id name ");
                if(nm){ nm+=8; char *nl=strchr(nm,'\n'); if(nl)*nl='\0'; strncpy(g_engine.name,nm,127); }
                else strcpy(g_engine.name,path);
                break;
            }
        }
        usleep(50000);
    }
    engine_send("isready\n");
    start=time(NULL);
    while(time(NULL)-start<5){
        got=(int)read(g_engine.out_fd, rbuf, sizeof(rbuf)-1);
        if(got>0){
            rbuf[got]='\0';
            if(strstr(rbuf,"readyok")){ g_engine.ready=1; break; }
        }
        usleep(50000);
    }
}

/* Build position string for UCI */
void build_uci_position(char *out, int maxlen){
    int pos=0;
    pos+=snprintf(out+pos,maxlen-pos,"position startpos");
    if(g_hist_len>0){
        pos+=snprintf(out+pos,maxlen-pos," moves");
        for(int i=0;i<g_hist_len;i++)
            pos+=snprintf(out+pos,maxlen-pos," %s",g_history[i].move.uci);
    }
    pos+=snprintf(out+pos,maxlen-pos,"\n");
}

void engine_go(void){
    if(!g_engine_active||!g_engine.ready) return;
    if(g_pos.side!=g_engine_side) return;
    char buf[4096];
    build_uci_position(buf,sizeof(buf));
    engine_send(buf);
    engine_send("go movetime 1000\n");
}

/* Parse engine output, return 1 if bestmove found */
int engine_read_response(int){
    if(!g_engine_active) return 0;
    char rbuf[2048];
    int got=(int)read(g_engine.out_fd,rbuf,sizeof(rbuf)-1);
    if(got<=0) return 0;
    rbuf[got]='\0';

    /* Append to engine buf */
    int space=ENGINE_BUF-g_engine.buf_len-1;
    int copy=got<space?got:space;
    memcpy(g_engine.buf+g_engine.buf_len,rbuf,copy);
    g_engine.buf_len+=copy;
    g_engine.buf[g_engine.buf_len]='\0';

    char *bm=strstr(g_engine.buf,"bestmove ");
    if(!bm) return 0;
    bm+=9;
    char uci[8]={0};
    int ui=0;
    while(*bm && *bm!=' ' && *bm!='\n' && ui<7) uci[ui++]=*bm++;
    uci[ui]='\0';
    g_engine.buf_len=0;

    if(strcmp(uci,"(none)")==0||strcmp(uci,"0000")==0) return 0;

    Move m;
    if(!move_from_uci(&g_pos,uci,&m)){
        snprintf(g_status,sizeof(g_status),"Engine illegal move: %s",uci);
        return 0;
    }
    /* Apply engine move */
    g_history[g_hist_len].pos=g_pos;
    build_san(&g_pos,&m,m.san);
    pgn_append(&m,g_pos.fullmove,g_pos.side);
    g_history[g_hist_len].move=m;
    g_hist_len++;
    apply_move(&g_pos,&m);
    g_sel_rank=-1; g_sel_file=-1; g_selected=0;

    /* Check game over */
    Move legal[MAX_LEGAL]; int nl=0;
    generate_legal_moves(&g_pos,legal,&nl);
    if(nl==0){
        if(is_in_check(&g_pos,g_pos.side)){
            snprintf(g_status,sizeof(g_status),"%s wins by checkmate!",
                     g_pos.side==WHITE?"Black":"White");
        } else {
            snprintf(g_status,sizeof(g_status),"Stalemate - Draw!");
        }
        g_game_over=1;
    } else if(g_pos.halfmove>=100){
        snprintf(g_status,sizeof(g_status),"Draw by 50-move rule");
        g_game_over=1;
    } else {
        g_status[0]='\0';
    }
    generate_legal_moves(&g_pos,g_legal,&g_nlegal);
    return 1;
}

/* ─── Move from UCI string ───────────────────────────────────── */

int move_from_uci(const Position *p, const char *uci, Move *out){
    if(strlen(uci)<4) return 0;
    int ff=uci[0]-'a', fr=uci[1]-'1';
    int tf=uci[2]-'a', tr=uci[3]-'1';
    int promo=0;
    if(uci[4]){
        char c=tolower(uci[4]);
        if(c=='q') promo=QUEEN;
        else if(c=='r') promo=ROOK;
        else if(c=='b') promo=BISHOP;
        else if(c=='n') promo=KNIGHT;
    }
    Move all[MAX_LEGAL]; int n=0;
    generate_legal_moves(p,all,&n);
    for(int i=0;i<n;i++){
        if(all[i].from_rank==fr && all[i].from_file==ff &&
           all[i].to_rank==tr   && all[i].to_file==tf){
            /* For promotions match promo piece or default to queen */
            if((all[i].flags&FLAG_PROMO)){
                if(promo==0) promo=QUEEN;
                if(all[i].promo!=promo) continue;
            }
            *out=all[i];
            return 1;
        }
    }
    return 0;
}

/* ─── User move handling ─────────────────────────────────────── */

void try_move(int tr, int tf){
    /* Find legal move from sel to target */
    Move *best=NULL;
    for(int i=0;i<g_nlegal;i++){
        if(g_legal[i].from_rank==g_sel_rank && g_legal[i].from_file==g_sel_file &&
           g_legal[i].to_rank==tr && g_legal[i].to_file==tf){
            /* Prefer queen promotion */
            if(!best || (g_legal[i].flags&FLAG_PROMO && g_legal[i].promo==QUEEN))
                best=&g_legal[i];
        }
    }
    if(!best){
        /* Maybe selecting new piece */
        if(piece_color(g_pos.board[tr][tf])==g_pos.side){
            g_sel_rank=tr; g_sel_file=tf; g_selected=1;
        } else {
            g_sel_rank=-1; g_sel_file=-1; g_selected=0;
            snprintf(g_status,sizeof(g_status),"Illegal move!");
        }
        return;
    }
    g_status[0]='\0';

    /* Save history */
    g_history[g_hist_len].pos=g_pos;
    Move m=*best;
    build_san(&g_pos,&m,m.san);
    pgn_append(&m,g_pos.fullmove,g_pos.side);
    g_history[g_hist_len].move=m;
    g_hist_len++;

    apply_move(&g_pos,&m);
    g_sel_rank=-1; g_sel_file=-1; g_selected=0;

    /* Check game over */
    Move legal[MAX_LEGAL]; int nl=0;
    generate_legal_moves(&g_pos,legal,&nl);
    if(nl==0){
        if(is_in_check(&g_pos,g_pos.side)){
            snprintf(g_status,sizeof(g_status),"%s wins by checkmate!",
                     g_pos.side==WHITE?"Black":"White");
            if(g_pos.side==WHITE)
                g_pgn[g_pgn_len-1]=' ', strcat(g_pgn,"0-1 ");
            else
                strcat(g_pgn,"1-0 ");
            g_pgn_len=(int)strlen(g_pgn);
        } else {
            snprintf(g_status,sizeof(g_status),"Stalemate - Draw!");
            strcat(g_pgn,"1/2-1/2 ");
            g_pgn_len=(int)strlen(g_pgn);
        }
        g_game_over=1;
    } else if(g_pos.halfmove>=100){
        snprintf(g_status,sizeof(g_status),"Draw by 50-move rule");
        g_game_over=1;
    }

    generate_legal_moves(&g_pos,g_legal,&g_nlegal);

    /* Ask engine to move */
    if(!g_game_over && g_engine_active && g_pos.side==g_engine_side)
        engine_go();
}

void undo_move(void){
    if(g_hist_len==0){ snprintf(g_status,sizeof(g_status),"Nothing to undo!"); return; }
    /* If engine is playing, undo 2 plies */
    int undo_count=(g_engine_active)?2:1;
    if(undo_count>g_hist_len) undo_count=g_hist_len;
    /* Rebuild pgn: remove last undo_count moves */
    g_hist_len-=undo_count;
    g_pos=g_history[0].pos; /* restore to start */
    /* Actually restore from history entry */
    g_pos=g_history[g_hist_len].pos;

    /* Rebuild PGN from scratch */
    g_pgn_len=0; g_pgn[0]='\0';
    /* We need a temp position to rebuild SAN correctly */
    Position tmp; position_start(&tmp);
    for(int i=0;i<g_hist_len;i++){
        Move m=g_history[i].move;
        pgn_append(&m,tmp.fullmove,tmp.side);
        apply_move(&tmp,&m);
    }

    g_sel_rank=-1; g_sel_file=-1; g_selected=0;
    g_game_over=0;
    generate_legal_moves(&g_pos,g_legal,&g_nlegal);
    snprintf(g_status,sizeof(g_status),"Move undone.");
}

/* ─── Main input loop ────────────────────────────────────────── */

void handle_select(void){
    int r=g_cursor_rank, f=g_cursor_file;
    if(!g_selected){
        if(piece_color(g_pos.board[r][f])==g_pos.side){
            g_sel_rank=r; g_sel_file=f; g_selected=1;
            g_status[0]='\0';
        } else {
            snprintf(g_status,sizeof(g_status),"No piece to select there.");
        }
    } else {
        if(r==g_sel_rank && f==g_sel_file){
            /* Deselect */
            g_sel_rank=-1; g_sel_file=-1; g_selected=0;
        } else {
            try_move(r,f);
        }
    }
}

int main(int argc, char *argv[]){
    /* Parse arguments: chess_gui [engine_path] [engine_color] */
    char *engine_path=NULL;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--white")==0) g_engine_side=WHITE;
        else if(strcmp(argv[i],"--black")==0) g_engine_side=BLACK;
        else engine_path=argv[i];
    }

    atexit(cleanup);
    enable_raw_mode();

    position_start(&g_pos);
    generate_legal_moves(&g_pos,g_legal,&g_nlegal);

    g_cursor_rank=0; g_cursor_file=4;
    g_status[0]='\0';
    g_pgn[0]='\0';

    if(engine_path){
        engine_start(engine_path);
        if(!g_engine_active)
            snprintf(g_status,sizeof(g_status),"Failed to start engine: %s",engine_path);
        else if(!g_engine.ready)
            snprintf(g_status,sizeof(g_status),"Engine not ready: %s",engine_path);
        else{
            snprintf(g_status,sizeof(g_status),"Engine loaded: %s",g_engine.name);
            /* If engine plays white, go immediately */
            if(g_pos.side==g_engine_side) engine_go();
        }
    }

    render();

    while(1){
        /* Check for engine response */
        if(g_engine_active && !g_game_over && g_pos.side==g_engine_side){
            if(engine_read_response())
                render();
        }

        /* Read keyboard input (non-blocking) */
        unsigned char c=0;
        int n=(int)read(STDIN_FILENO,&c,1);
        if(n<=0){ usleep(20000); continue; }

        if(c=='\x1b'){
            /* Escape sequence */
            unsigned char seq[4]={0};
            read(STDIN_FILENO,seq,1);
            if(seq[0]=='['){
                read(STDIN_FILENO,seq+1,1);
                if(!g_game_over && !(g_engine_active && g_pos.side==g_engine_side)){
                    switch(seq[1]){
                    case 'A': /* Up */
                        if(g_flip_board){ if(g_cursor_rank>0) g_cursor_rank--; }
                        else            { if(g_cursor_rank<7) g_cursor_rank++; }
                        break;
                    case 'B': /* Down */
                        if(g_flip_board){ if(g_cursor_rank<7) g_cursor_rank++; }
                        else            { if(g_cursor_rank>0) g_cursor_rank--; }
                        break;
                    case 'C': /* Right */
                        if(g_cursor_file<7) g_cursor_file++; break;
                    case 'D': /* Left */
                        if(g_cursor_file>0) g_cursor_file--; break;
                    }
                }
            }
        } else {
            char ch=tolower((char)c);
            switch(ch){
            case 'q': exit(0);
            case 'f':
                g_flip_board=!g_flip_board;
                /* Adjust cursor */
                g_cursor_rank=7-g_cursor_rank;
                g_cursor_file=7-g_cursor_file;
                break;
            case 'u':
                if(!g_game_over) undo_move();
                break;
            case '\r': case '\n': case ' ':
                if(!g_game_over && !(g_engine_active && g_pos.side==g_engine_side))
                    handle_select();
                break;
            /* WASD movement */
            case 'w':
                if(!g_game_over){
                    if(g_flip_board){ if(g_cursor_rank>0) g_cursor_rank--; }
                    else            { if(g_cursor_rank<7) g_cursor_rank++; }
                }
                break;
            case 's':
                if(!g_game_over){
                    if(g_flip_board){ if(g_cursor_rank<7) g_cursor_rank++; }
                    else            { if(g_cursor_rank>0) g_cursor_rank--; }
                }
                break;
            case 'a':
                if(!g_game_over && g_cursor_file>0) g_cursor_file--; break;
            case 'd':
                if(!g_game_over && g_cursor_file<7) g_cursor_file++; break;
            }
        }
        render();
    }
    return 0;
}
