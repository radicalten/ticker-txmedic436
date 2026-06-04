// chess_tui.c - single-file macOS Terminal chess TUI with UCI engine support.
// Build: clang -O2 -std=c11 -Wall -Wextra -pedantic chess_tui.c -o chess_tui
// Usage: ./chess_tui [-engine /path/to/engine] [-movetime ms | -depth N | -nodes N] [-human white|black]

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_MOVES 256
#define MAX_PLY   2048

// 0x88 board helpers
#define SQ(file, rank) (((rank) << 4) | (file))
#define FILE_OF(sq)    ((sq) & 7)
#define RANK_OF(sq)    ((sq) >> 4)
#define ONBOARD(sq)    ((((sq) & 0x88) == 0))

enum {
    EMPTY = 0,
    WP=1, WN, WB, WR, WQ, WK,
    BP=7, BN, BB, BR, BQ, BK
};

static inline int is_white(int p){ return p>=WP && p<=WK; }
static inline int is_black(int p){ return p>=BP && p<=BK; }
static inline int side_of(int p){ return is_black(p) ? 1 : 0; }
static inline int piece_type(int p){
    if(p==EMPTY) return 0;
    int t = p;
    if(t>=BP) t -= 6;
    return t; // 1..6 like white
}

static const char *piece_to_char = ".PNBRQKpnbrqk"; // index by piece code (0..12); note '.' for empty
static const char piece_letter[7] = {0,'P','N','B','R','Q','K'};

enum {
    MF_CAPTURE = 1<<0,
    MF_EP      = 1<<1,
    MF_CASTLE  = 1<<2,
    MF_PROMO   = 1<<3,
    MF_DOUBLE  = 1<<4
};

typedef struct {
    uint8_t from, to;
    uint8_t prom;   // 0 or piece type 2..5 (N,B,R,Q) in "white type space"
    uint8_t flags;
} Move;

typedef struct {
    Move m;
    int captured;
    int ep_sq;          // -1 if none
    uint8_t castle;     // bitmask KQkq
    int halfmove;
    int fullmove;
    int moved_piece;
} Undo;

typedef struct {
    int board[128];
    int side;           // 0 white, 1 black
    uint8_t castle;     // 1 WK, 2 WQ, 4 BK, 8 BQ
    int ep_sq;          // -1 or square
    int halfmove;
    int fullmove;

    Undo undo[MAX_PLY];
    int ply;

    // move lists for UCI "position startpos moves ..."
    char uci_moves[MAX_PLY][8];
    char san_moves[MAX_PLY][32];
    int move_count; // number of plies actually recorded in uci_moves/san_moves
} Game;

typedef struct {
    pid_t pid;
    int in_fd;   // write to engine stdin
    int out_fd;  // read from engine stdout
    FILE *in;
    FILE *out;
    bool ok;
} Engine;

typedef struct {
    int cursor_file; // 0..7
    int cursor_rank; // 0..7
    int selected_sq; // -1 none
    bool targets[128];
    bool captures[128];

    bool promote_pending;
    int promo_from, promo_to;
    char promo_choice; // 'q','r','b','n'
    char status[256];
} UI;

typedef enum { TC_MOVETIME, TC_DEPTH, TC_NODES } TcMode;
typedef struct {
    char engine_path[1024];
    bool have_engine;
    TcMode tc_mode;
    int tc_value; // ms or depth or nodes
    int human_side; // 0 white, 1 black, -1 both humans
} Config;

// -------- Terminal raw mode --------

static struct termios g_orig_termios;
static bool g_raw = false;

static void term_disable_raw(void){
    if(g_raw){
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw = false;
        // show cursor, reset
        write(STDOUT_FILENO, "\x1b[0m\x1b[?25h", 10);
    }
}

static void term_enable_raw(void){
    if(tcgetattr(STDIN_FILENO, &g_orig_termios)==-1) exit(1);
    atexit(term_disable_raw);

    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1; // 100ms

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)==-1) exit(1);
    g_raw = true;
    write(STDOUT_FILENO, "\x1b[?25l", 6); // hide cursor
}

static void die(const char *fmt, ...){
    term_disable_raw();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void on_signal(int sig){
    (void)sig;
    term_disable_raw();
    _exit(128 + sig);
}

// -------- Utilities --------

static void get_term_size(int *rows, int *cols){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)==0 && ws.ws_col>0){
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24; *cols = 80;
    }
}

static void sq_to_alg(int sq, char out[3]){
    out[0] = (char)('a' + FILE_OF(sq));
    out[1] = (char)('1' + RANK_OF(sq));
    out[2] = '\0';
}

static int alg_to_sq(const char *a){
    if(!a || strlen(a)<2) return -1;
    char f = a[0], r = a[1];
    if(f<'a'||f>'h'||r<'1'||r>'8') return -1;
    return SQ(f-'a', r-'1');
}

static int find_king(const Game *g, int side){
    int k = (side==0)?WK:BK;
    for(int sq=0; sq<128; sq++){
        if(!ONBOARD(sq)){ sq = (sq|0x0F)+1; continue; }
        if(g->board[sq]==k) return sq;
    }
    return -1;
}

// -------- Attack detection --------

static bool square_attacked(const Game *g, int sq, int by_side){
    // pawn attacks
    if(by_side==0){
        int s1 = sq - 15;
        int s2 = sq - 17;
        if(ONBOARD(s1) && g->board[s1]==WP) return true;
        if(ONBOARD(s2) && g->board[s2]==WP) return true;
    } else {
        int s1 = sq + 15;
        int s2 = sq + 17;
        if(ONBOARD(s1) && g->board[s1]==BP) return true;
        if(ONBOARD(s2) && g->board[s2]==BP) return true;
    }

    // knights
    static const int n_off[8] = { 31,33,14,-14,18,-18,-31,-33 };
    int n = (by_side==0)?WN:BN;
    for(int i=0;i<8;i++){
        int t = sq + n_off[i];
        if(ONBOARD(t) && g->board[t]==n) return true;
    }

    // kings (adjacent)
    static const int k_off[8] = { 16,-16,1,-1,15,17,-15,-17 };
    int k = (by_side==0)?WK:BK;
    for(int i=0;i<8;i++){
        int t = sq + k_off[i];
        if(ONBOARD(t) && g->board[t]==k) return true;
    }

    // bishops/queens diagonals
    static const int b_dir[4] = { 15,17,-15,-17 };
    int b = (by_side==0)?WB:BB;
    int q = (by_side==0)?WQ:BQ;
    for(int d=0; d<4; d++){
        int t = sq + b_dir[d];
        while(ONBOARD(t)){
            int p = g->board[t];
            if(p!=EMPTY){
                if(p==b || p==q) return true;
                break;
            }
            t += b_dir[d];
        }
    }

    // rooks/queens orthogonal
    static const int r_dir[4] = { 16,-16,1,-1 };
    int r = (by_side==0)?WR:BR;
    for(int d=0; d<4; d++){
        int t = sq + r_dir[d];
        while(ONBOARD(t)){
            int p = g->board[t];
            if(p!=EMPTY){
                if(p==r || p==q) return true;
                break;
            }
            t += r_dir[d];
        }
    }

    return false;
}

static bool in_check(const Game *g, int side){
    int ksq = find_king(g, side);
    if(ksq<0) return false;
    return square_attacked(g, ksq, side^1);
}

// -------- Move generation --------

static void add_move(Move *list, int *n, int from, int to, int flags, int prom){
    Move m;
    m.from = (uint8_t)from;
    m.to = (uint8_t)to;
    m.flags = (uint8_t)flags;
    m.prom = (uint8_t)prom;
    list[(*n)++] = m;
}

static void gen_pseudo_moves(const Game *g, Move *list, int *n){
    *n = 0;
    int us = g->side;

    for(int sq=0; sq<128; sq++){
        if(!ONBOARD(sq)){ sq = (sq|0x0F)+1; continue; }

        int p = g->board[sq];
        if(p==EMPTY) continue;
        if(side_of(p)!=us) continue;

        int pt = piece_type(p);

        if(pt==1){ // pawn
            int dir = (us==0)?16:-16;
            int start_rank = (us==0)?1:6;
            int promo_rank = (us==0)?7:0;

            int fwd = sq + dir;
            if(ONBOARD(fwd) && g->board[fwd]==EMPTY){
                if(RANK_OF(fwd)==promo_rank){
                    add_move(list,n,sq,fwd,MF_PROMO,5); // Q
                    add_move(list,n,sq,fwd,MF_PROMO,4); // R
                    add_move(list,n,sq,fwd,MF_PROMO,3); // B
                    add_move(list,n,sq,fwd,MF_PROMO,2); // N
                } else {
                    add_move(list,n,sq,fwd,0,0);
                    // double
                    if(RANK_OF(sq)==start_rank){
                        int fwd2 = fwd + dir;
                        if(ONBOARD(fwd2) && g->board[fwd2]==EMPTY){
                            add_move(list,n,sq,fwd2,MF_DOUBLE,0);
                        }
                    }
                }
            }

            // captures
            int cap1 = sq + dir + 1;
            int cap2 = sq + dir - 1;
            int them = us^1;
            if(ONBOARD(cap1)){
                int tp = g->board[cap1];
                if(tp!=EMPTY && side_of(tp)==them){
                    int flags = MF_CAPTURE;
                    if(RANK_OF(cap1)==promo_rank){
                        add_move(list,n,sq,cap1,flags|MF_PROMO,5);
                        add_move(list,n,sq,cap1,flags|MF_PROMO,4);
                        add_move(list,n,sq,cap1,flags|MF_PROMO,3);
                        add_move(list,n,sq,cap1,flags|MF_PROMO,2);
                    } else add_move(list,n,sq,cap1,flags,0);
                }
            }
            if(ONBOARD(cap2)){
                int tp = g->board[cap2];
                if(tp!=EMPTY && side_of(tp)==them){
                    int flags = MF_CAPTURE;
                    if(RANK_OF(cap2)==promo_rank){
                        add_move(list,n,sq,cap2,flags|MF_PROMO,5);
                        add_move(list,n,sq,cap2,flags|MF_PROMO,4);
                        add_move(list,n,sq,cap2,flags|MF_PROMO,3);
                        add_move(list,n,sq,cap2,flags|MF_PROMO,2);
                    } else add_move(list,n,sq,cap2,flags,0);
                }
            }

            // en passant
            if(g->ep_sq!=-1){
                if(cap1==g->ep_sq) add_move(list,n,sq,cap1,MF_EP|MF_CAPTURE,0);
                if(cap2==g->ep_sq) add_move(list,n,sq,cap2,MF_EP|MF_CAPTURE,0);
            }

        } else if(pt==2){ // knight
            static const int off[8] = { 31,33,14,-14,18,-18,-31,-33 };
            for(int i=0;i<8;i++){
                int to = sq + off[i];
                if(!ONBOARD(to)) continue;
                int tp = g->board[to];
                if(tp==EMPTY) add_move(list,n,sq,to,0,0);
                else if(side_of(tp)!=(int)us) add_move(list,n,sq,to,MF_CAPTURE,0);
            }

        } else if(pt==3 || pt==4 || pt==5){ // bishop/rook/queen
            static const int bdir[4] = {15,17,-15,-17};
            static const int rdir[4] = {16,-16,1,-1};
            const int *dirs = NULL;
            int dcount = 0;
            int tmp[8];

            if(pt==3){
                for(int i=0;i<4;i++) tmp[i]=bdir[i];
                dirs = tmp; dcount=4;
            } else if(pt==4){
                for(int i=0;i<4;i++) tmp[i]=rdir[i];
                dirs = tmp; dcount=4;
            } else {
                for(int i=0;i<4;i++) tmp[i]=bdir[i];
                for(int i=0;i<4;i++) tmp[4+i]=rdir[i];
                dirs = tmp; dcount=8;
            }

            for(int d=0; d<dcount; d++){
                int to = sq + dirs[d];
                while(ONBOARD(to)){
                    int tp = g->board[to];
                    if(tp==EMPTY){
                        add_move(list,n,sq,to,0,0);
                    } else {
                        if(side_of(tp)!=(int)us) add_move(list,n,sq,to,MF_CAPTURE,0);
                        break;
                    }
                    to += dirs[d];
                }
            }

        } else if(pt==6){ // king
            static const int off[8] = { 16,-16,1,-1,15,17,-15,-17 };
            for(int i=0;i<8;i++){
                int to = sq + off[i];
                if(!ONBOARD(to)) continue;
                int tp = g->board[to];
                if(tp==EMPTY) add_move(list,n,sq,to,0,0);
                else if(side_of(tp)!=(int)us) add_move(list,n,sq,to,MF_CAPTURE,0);
            }

            // castling
            if(us==0 && sq==SQ(4,0)){
                if((g->castle & 1)){
                    // squares f1 g1 empty and not attacked
                    if(g->board[SQ(5,0)]==EMPTY && g->board[SQ(6,0)]==EMPTY){
                        if(!square_attacked(g,SQ(4,0),1) && !square_attacked(g,SQ(5,0),1) && !square_attacked(g,SQ(6,0),1)){
                            add_move(list,n,sq,SQ(6,0),MF_CASTLE,0);
                        }
                    }
                }
                if((g->castle & 2)){
                    // squares d1 c1 b1 empty and not attacked (d1,c1; b1 just empty)
                    if(g->board[SQ(3,0)]==EMPTY && g->board[SQ(2,0)]==EMPTY && g->board[SQ(1,0)]==EMPTY){
                        if(!square_attacked(g,SQ(4,0),1) && !square_attacked(g,SQ(3,0),1) && !square_attacked(g,SQ(2,0),1)){
                            add_move(list,n,sq,SQ(2,0),MF_CASTLE,0);
                        }
                    }
                }
            } else if(us==1 && sq==SQ(4,7)){
                if((g->castle & 4)){
                    if(g->board[SQ(5,7)]==EMPTY && g->board[SQ(6,7)]==EMPTY){
                        if(!square_attacked(g,SQ(4,7),0) && !square_attacked(g,SQ(5,7),0) && !square_attacked(g,SQ(6,7),0)){
                            add_move(list,n,sq,SQ(6,7),MF_CASTLE,0);
                        }
                    }
                }
                if((g->castle & 8)){
                    if(g->board[SQ(3,7)]==EMPTY && g->board[SQ(2,7)]==EMPTY && g->board[SQ(1,7)]==EMPTY){
                        if(!square_attacked(g,SQ(4,7),0) && !square_attacked(g,SQ(3,7),0) && !square_attacked(g,SQ(2,7),0)){
                            add_move(list,n,sq,SQ(2,7),MF_CASTLE,0);
                        }
                    }
                }
            }
        }
    }
}

static void make_move(Game *g, Move m){
    Undo *u = &g->undo[g->ply++];
    u->m = m;
    u->ep_sq = g->ep_sq;
    u->castle = g->castle;
    u->halfmove = g->halfmove;
    u->fullmove = g->fullmove;

    int from = m.from, to = m.to;
    int moving = g->board[from];
    u->moved_piece = moving;

    int captured = g->board[to];
    u->captured = captured;

    // halfmove clock
    if(piece_type(moving)==1 || (m.flags & MF_CAPTURE)) g->halfmove = 0;
    else g->halfmove++;

    // clear ep by default
    g->ep_sq = -1;

    // move piece
    g->board[to] = moving;
    g->board[from] = EMPTY;

    // en passant capture
    if(m.flags & MF_EP){
        int cap_sq = (g->side==0) ? (to-16) : (to+16);
        u->captured = g->board[cap_sq];
        g->board[cap_sq] = EMPTY;
    }

    // promotion
    if(m.flags & MF_PROMO){
        int base = (g->side==0) ? 0 : 6;
        int newp = base + m.prom; // prom is "white type"
        g->board[to] = newp;
    }

    // castling rook move
    if(m.flags & MF_CASTLE){
        if(to==SQ(6,0)){ // white O-O
            g->board[SQ(5,0)] = WR;
            g->board[SQ(7,0)] = EMPTY;
        } else if(to==SQ(2,0)){ // white O-O-O
            g->board[SQ(3,0)] = WR;
            g->board[SQ(0,0)] = EMPTY;
        } else if(to==SQ(6,7)){ // black O-O
            g->board[SQ(5,7)] = BR;
            g->board[SQ(7,7)] = EMPTY;
        } else if(to==SQ(2,7)){ // black O-O-O
            g->board[SQ(3,7)] = BR;
            g->board[SQ(0,7)] = EMPTY;
        }
    }

    // set EP square after double pawn push
    if(m.flags & MF_DOUBLE){
        g->ep_sq = (g->side==0) ? (to-16) : (to+16);
    }

    // update castling rights
    // if king moves
    if(moving==WK) g->castle &= ~(1|2);
    if(moving==BK) g->castle &= ~(4|8);

    // if rook moves or rook captured from initial squares
    if(from==SQ(0,0) || to==SQ(0,0)) g->castle &= ~2;
    if(from==SQ(7,0) || to==SQ(7,0)) g->castle &= ~1;
    if(from==SQ(0,7) || to==SQ(0,7)) g->castle &= ~8;
    if(from==SQ(7,7) || to==SQ(7,7)) g->castle &= ~4;

    // side and fullmove
    g->side ^= 1;
    if(g->side==0) g->fullmove++;
}

static void unmake_move(Game *g){
    if(g->ply<=0) return;
    Undo *u = &g->undo[--g->ply];
    Move m = u->m;

    g->side ^= 1;
    g->ep_sq = u->ep_sq;
    g->castle = u->castle;
    g->halfmove = u->halfmove;
    g->fullmove = u->fullmove;

    int from = m.from, to = m.to;

    // undo castling rook move first (based on king destination)
    if(m.flags & MF_CASTLE){
        if(to==SQ(6,0)){
            g->board[SQ(7,0)] = WR; g->board[SQ(5,0)] = EMPTY;
        } else if(to==SQ(2,0)){
            g->board[SQ(0,0)] = WR; g->board[SQ(3,0)] = EMPTY;
        } else if(to==SQ(6,7)){
            g->board[SQ(7,7)] = BR; g->board[SQ(5,7)] = EMPTY;
        } else if(to==SQ(2,7)){
            g->board[SQ(0,7)] = BR; g->board[SQ(3,7)] = EMPTY;
        }
    }

    // restore moving piece to from
    g->board[from] = u->moved_piece;

    // restore captured / destination
    if(m.flags & MF_EP){
        g->board[to] = EMPTY;
        int cap_sq = (g->side==0) ? (to-16) : (to+16);
        g->board[cap_sq] = u->captured;
    } else {
        g->board[to] = u->captured;
    }
}

static int gen_legal_moves(Game *g, Move *out){
    Move tmp[MAX_MOVES];
    int n=0;
    gen_pseudo_moves(g, tmp, &n);
    int k=0;
    int us = g->side;
    for(int i=0;i<n;i++){
        make_move(g, tmp[i]);
        bool illegal = in_check(g, us);
        unmake_move(g);
        if(!illegal) out[k++] = tmp[i];
    }
    return k;
}

static bool moves_equal(Move a, Move b){
    return a.from==b.from && a.to==b.to && a.prom==b.prom && a.flags==b.flags;
}

// -------- SAN (PGN) generation --------

static int count_legal_moves(Game *g){
    Move m[MAX_MOVES];
    return gen_legal_moves(g, m);
}

static bool has_legal_move_to(Game *g, int from, int to, int prom_type_white){
    Move ms[MAX_MOVES];
    int n = gen_legal_moves(g, ms);
    for(int i=0;i<n;i++){
        if(ms[i].from==from && ms[i].to==to){
            if((ms[i].flags & MF_PROMO)){
                if(ms[i].prom == prom_type_white) return true;
            } else return true;
        }
    }
    return false;
}

static void move_to_uci(const Move *m, char out[8]){
    char a[3], b[3];
    sq_to_alg(m->from, a);
    sq_to_alg(m->to, b);
    out[0]=a[0]; out[1]=a[1]; out[2]=b[0]; out[3]=b[1];
    int idx=4;
    if(m->flags & MF_PROMO){
        char c='q';
        if(m->prom==2) c='n';
        else if(m->prom==3) c='b';
        else if(m->prom==4) c='r';
        else if(m->prom==5) c='q';
        out[idx++] = c;
    }
    out[idx]='\0';
}

static void san_for_move(Game *g, Move m, char out[32]){
    // Assumes m is legal in current position.
    int us = g->side;
    int moving = g->board[m.from];
    int pt = piece_type(moving);

    // Castling
    if(m.flags & MF_CASTLE){
        if(m.to==SQ(6,0) || m.to==SQ(6,7)) strcpy(out, "O-O");
        else strcpy(out, "O-O-O");
    } else {
        char buf[64]; int bi=0;

        bool is_capture = (m.flags & MF_CAPTURE) != 0;
        char to_alg[3]; sq_to_alg(m.to, to_alg);

        if(pt==1){
            // pawn SAN: file if capture, then 'x', then destination; promotion suffix
            if(is_capture){
                buf[bi++] = (char)('a' + FILE_OF(m.from));
                buf[bi++] = 'x';
            }
            buf[bi++] = to_alg[0];
            buf[bi++] = to_alg[1];
            if(m.flags & MF_PROMO){
                buf[bi++]='=';
                buf[bi++]=piece_letter[m.prom]; // N/B/R/Q
            }
        } else {
            // piece letter
            buf[bi++] = piece_letter[pt];

            // disambiguation: if another same piece can also go to m.to
            // Determine minimal disambiguator: file, rank, or both.
            bool need_file=false, need_rank=false;
            {
                // collect other legal moves by same piece type to same destination
                Move ms[MAX_MOVES];
                int n = gen_legal_moves(g, ms);
                int others = 0;
                for(int i=0;i<n;i++){
                    if(ms[i].to != m.to) continue;
                    if(ms[i].from == m.from) continue;
                    int p = g->board[ms[i].from];
                    if(p==EMPTY) continue;
                    if(side_of(p)!=us) continue;
                    if(piece_type(p)!=pt) continue;
                    others++;
                    if(FILE_OF(ms[i].from) != FILE_OF(m.from)) need_file = true;
                    if(RANK_OF(ms[i].from) != RANK_OF(m.from)) need_rank = true;
                }
                if(others>0){
                    // SAN rules: if file alone distinguishes, use file; else if rank alone distinguishes, use rank; else both.
                    // Our need_file/need_rank indicate differences exist; but we must test uniqueness.
                    bool file_unique=true, rank_unique=true;
                    for(int i=0;i<n;i++){
                        if(ms[i].to != m.to) continue;
                        if(ms[i].from == m.from) continue;
                        int p = g->board[ms[i].from];
                        if(p==EMPTY) continue;
                        if(side_of(p)!=us) continue;
                        if(piece_type(p)!=pt) continue;
                        if(FILE_OF(ms[i].from) == FILE_OF(m.from)) file_unique=false;
                        if(RANK_OF(ms[i].from) == RANK_OF(m.from)) rank_unique=false;
                    }
                    if(file_unique){
                        need_file = true; need_rank = false;
                    } else if(rank_unique){
                        need_rank = true; need_file = false;
                    } else {
                        need_file = true; need_rank = true;
                    }
                } else {
                    need_file=false; need_rank=false;
                }
            }

            if(need_file) buf[bi++] = (char)('a' + FILE_OF(m.from));
            if(need_rank) buf[bi++] = (char)('1' + RANK_OF(m.from));

            if(is_capture) buf[bi++]='x';
            buf[bi++] = to_alg[0];
            buf[bi++] = to_alg[1];

            if(m.flags & MF_PROMO){
                buf[bi++]='=';
                buf[bi++]=piece_letter[m.prom];
            }
        }

        buf[bi]='\0';
        strcpy(out, buf);
    }

    // Check / mate suffix
    // Temporarily make move, then test.
    int saved_ply = g->ply;
    make_move(g, m);
    bool gives_check = in_check(g, g->side);
    bool mate = false;
    if(gives_check){
        int legal = count_legal_moves(g);
        if(legal==0) mate = true;
    } else {
        int legal = count_legal_moves(g);
        // stalemate isn't indicated in SAN, so ignore here
        (void)legal;
    }
    // restore
    while(g->ply > saved_ply) unmake_move(g);

    if(mate) strcat(out, "#");
    else if(gives_check) strcat(out, "+");
}

// -------- Game setup / reset --------

static void game_clear(Game *g){
    memset(g, 0, sizeof(*g));
    for(int i=0;i<128;i++) g->board[i]=EMPTY;
    g->ep_sq = -1;
    g->side = 0;
    g->castle = 0;
    g->halfmove = 0;
    g->fullmove = 1;
    g->ply = 0;
    g->move_count = 0;
}

static void game_startpos(Game *g){
    game_clear(g);
    // place pieces
    // white
    g->board[SQ(0,0)] = WR; g->board[SQ(1,0)] = WN; g->board[SQ(2,0)] = WB; g->board[SQ(3,0)] = WQ;
    g->board[SQ(4,0)] = WK; g->board[SQ(5,0)] = WB; g->board[SQ(6,0)] = WN; g->board[SQ(7,0)] = WR;
    for(int f=0;f<8;f++) g->board[SQ(f,1)] = WP;
    // black
    g->board[SQ(0,7)] = BR; g->board[SQ(1,7)] = BN; g->board[SQ(2,7)] = BB; g->board[SQ(3,7)] = BQ;
    g->board[SQ(4,7)] = BK; g->board[SQ(5,7)] = BB; g->board[SQ(6,7)] = BN; g->board[SQ(7,7)] = BR;
    for(int f=0;f<8;f++) g->board[SQ(f,6)] = BP;

    g->side = 0;
    g->castle = 1|2|4|8;
    g->ep_sq = -1;
    g->halfmove = 0;
    g->fullmove = 1;
}

static void ui_reset(UI *ui){
    memset(ui, 0, sizeof(*ui));
    ui->cursor_file = 4;
    ui->cursor_rank = 1;
    ui->selected_sq = -1;
    ui->promote_pending = false;
    ui->promo_choice = 'q';
    ui->status[0] = '\0';
}

static void ui_recalc_targets(UI *ui, Game *g){
    memset(ui->targets, 0, sizeof(ui->targets));
    memset(ui->captures, 0, sizeof(ui->captures));
    if(ui->selected_sq<0) return;

    Move ms[MAX_MOVES];
    int n = gen_legal_moves(g, ms);
    for(int i=0;i<n;i++){
        if(ms[i].from == ui->selected_sq){
            ui->targets[ms[i].to] = true;
            if(ms[i].flags & MF_CAPTURE) ui->captures[ms[i].to] = true;
        }
    }
}

// -------- UCI engine I/O --------

static bool engine_readline(Engine *e, char *buf, size_t buflen, int timeout_ms){
    if(!e || !e->ok) return false;

    fd_set set;
    FD_ZERO(&set);
    FD_SET(e->out_fd, &set);

    struct timeval tv;
    tv.tv_sec = timeout_ms/1000;
    tv.tv_usec = (timeout_ms%1000)*1000;

    int r = select(e->out_fd+1, &set, NULL, NULL, (timeout_ms<0)?NULL:&tv);
    if(r<=0) return false;

    if(!fgets(buf, (int)buflen, e->out)) return false;
    // strip newline
    size_t L = strlen(buf);
    while(L>0 && (buf[L-1]=='\n' || buf[L-1]=='\r')) buf[--L] = '\0';
    return true;
}

static void engine_send(Engine *e, const char *fmt, ...){
    if(!e || !e->ok) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(e->in, fmt, ap);
    va_end(ap);
    fputc('\n', e->in);
    fflush(e->in);
}

static bool engine_wait_for(Engine *e, const char *token, int timeout_ms){
    char line[4096];
    int waited = 0;
    const int step = 50;
    while(timeout_ms<0 || waited < timeout_ms){
        if(engine_readline(e, line, sizeof(line), step)){
            if(strstr(line, token)) return true;
        } else {
            waited += step;
        }
    }
    return false;
}

static void engine_stop(Engine *e){
    if(!e || !e->ok) return;
    engine_send(e, "stop");
}

static void engine_quit(Engine *e){
    if(!e || !e->ok) return;
    engine_send(e, "quit");
}

static bool engine_start(Engine *e, const char *path){
    memset(e, 0, sizeof(*e));

    int to_engine[2], from_engine[2];
    if(pipe(to_engine)==-1) return false;
    if(pipe(from_engine)==-1) return false;

    pid_t pid = fork();
    if(pid==-1) return false;

    if(pid==0){
        // child: hook pipes
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        dup2(from_engine[1], STDERR_FILENO);

        close(to_engine[0]); close(to_engine[1]);
        close(from_engine[0]); close(from_engine[1]);

        execl(path, path, (char*)NULL);
        // if execl fails
        _exit(127);
    }

    // parent
    close(to_engine[0]);
    close(from_engine[1]);

    e->pid = pid;
    e->in_fd = to_engine[1];
    e->out_fd = from_engine[0];
    e->in = fdopen(e->in_fd, "w");
    e->out = fdopen(e->out_fd, "r");
    if(!e->in || !e->out){
        return false;
    }
    setvbuf(e->in, NULL, _IONBF, 0);
    setvbuf(e->out, NULL, _IONBF, 0);
    e->ok = true;

    engine_send(e, "uci");
    if(!engine_wait_for(e, "uciok", 3000)) return false;
    engine_send(e, "isready");
    if(!engine_wait_for(e, "readyok", 3000)) return false;
    engine_send(e, "ucinewgame");
    engine_send(e, "isready");
    if(!engine_wait_for(e, "readyok", 3000)) return false;
    return true;
}

static void engine_kill(Engine *e){
    if(!e || !e->ok) return;
    engine_quit(e);
    // wait a bit
    for(int i=0;i<20;i++){
        int status=0;
        pid_t r = waitpid(e->pid, &status, WNOHANG);
        if(r==e->pid) break;
        usleep(50000);
    }
    kill(e->pid, SIGTERM);
    waitpid(e->pid, NULL, 0);
    e->ok=false;
}

static void uci_position_from_game(Engine *e, const Game *g){
    engine_send(e, "position startpos%s", (g->move_count>0) ? " moves" : "");
    if(g->move_count>0){
        // The above sent only "position startpos moves" without moves.
        // For simplicity, send a second line with full command:
        // We'll re-send full in one command to be safe.
        // Build into a buffer (bounded).
        char cmd[8192];
        strcpy(cmd, "position startpos moves");
        size_t used = strlen(cmd);
        for(int i=0;i<g->move_count;i++){
            size_t L = strlen(g->uci_moves[i]);
            if(used + 1 + L + 1 >= sizeof(cmd)) break;
            cmd[used++] = ' ';
            memcpy(cmd+used, g->uci_moves[i], L);
            used += L;
            cmd[used] = '\0';
        }
        engine_send(e, "%s", cmd);
    }
}

static bool parse_uci_move_string(const char *s, int *from, int *to, int *prom_type_white){
    if(!s || strlen(s)<4) return false;
    char a[3] = {s[0], s[1], 0};
    char b[3] = {s[2], s[3], 0};
    int fsq = alg_to_sq(a);
    int tsq = alg_to_sq(b);
    if(fsq<0 || tsq<0) return false;
    *from = fsq;
    *to = tsq;
    *prom_type_white = 0;
    if(strlen(s)>=5){
        char pc = (char)tolower((unsigned char)s[4]);
        if(pc=='n') *prom_type_white = 2;
        else if(pc=='b') *prom_type_white = 3;
        else if(pc=='r') *prom_type_white = 4;
        else if(pc=='q') *prom_type_white = 5;
    }
    return true;
}

static bool engine_get_bestmove(Engine *e, const Config *cfg, Game *g, Move *out_move, char *out_uci, size_t out_uci_sz){
    if(!e || !e->ok) return false;

    // sync position
    uci_position_from_game(e, g);

    // go command
    if(cfg->tc_mode==TC_MOVETIME) engine_send(e, "go movetime %d", cfg->tc_value);
    else if(cfg->tc_mode==TC_DEPTH) engine_send(e, "go depth %d", cfg->tc_value);
    else engine_send(e, "go nodes %d", cfg->tc_value);

    char line[4096];
    while(engine_readline(e, line, sizeof(line), 30000)){
        if(strncmp(line, "bestmove ", 9)==0){
            const char *bm = line + 9;
            char uci[16]={0};
            int i=0;
            while(bm[i] && !isspace((unsigned char)bm[i]) && i<15){
                uci[i]=bm[i]; i++;
            }
            uci[i]='\0';
            if(strcmp(uci,"(none)")==0 || strcmp(uci,"0000")==0) return false;

            if(out_uci){
                strncpy(out_uci, uci, out_uci_sz-1);
                out_uci[out_uci_sz-1]='\0';
            }

            // match to a legal move in current position
            int from,to,prom;
            if(!parse_uci_move_string(uci, &from, &to, &prom)) return false;

            Move ms[MAX_MOVES];
            int n = gen_legal_moves(g, ms);
            for(int k=0;k<n;k++){
                if(ms[k].from==from && ms[k].to==to){
                    if((ms[k].flags & MF_PROMO)){
                        if(prom==0) continue; // engine should specify
                        if(ms[k].prom != prom) continue;
                    } else {
                        if(prom!=0) continue;
                    }
                    *out_move = ms[k];
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

// -------- Drawing --------

static const char *ansi_reset = "\x1b[0m";
static const char *ansi_inv   = "\x1b[7m";
static const char *ansi_dim   = "\x1b[2m";
static const char *ansi_bold  = "\x1b[1m";
static const char *ansi_sel   = "\x1b[42m\x1b[30m"; // green bg, black fg
static const char *ansi_tgt   = "\x1b[43m\x1b[30m"; // yellow bg
static const char *ansi_cap   = "\x1b[41m\x1b[37m"; // red bg, white fg
static const char *ansi_dark  = "\x1b[100m\x1b[37m"; // gray bg
static const char *ansi_light = "\x1b[49m\x1b[37m"; // default bg

static void draw(Game *g, UI *ui, const Config *cfg, const Engine *eng){
    int rows, cols;
    get_term_size(&rows, &cols);

    // clear screen, home
    write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7);

    // header
    char header[512];
    const char *stm = (g->side==0) ? "White" : "Black";
    const char *hside = (cfg->human_side==-1) ? "both" : (cfg->human_side==0 ? "white" : "black");
    const char *emode = (cfg->tc_mode==TC_MOVETIME) ? "movetime" : (cfg->tc_mode==TC_DEPTH ? "depth" : "nodes");
    snprintf(header, sizeof(header),
             "Terminal Chess (UCI) | Side to move: %s | Human: %s | Engine: %s | TC: %s=%d\n",
             stm, hside, (cfg->have_engine && eng && eng->ok) ? "on" : "off", emode, cfg->tc_value);
    write(STDOUT_FILENO, ansi_bold, strlen(ansi_bold));
    write(STDOUT_FILENO, header, strlen(header));
    write(STDOUT_FILENO, ansi_reset, strlen(ansi_reset));

    // status
    if(ui->status[0]){
        write(STDOUT_FILENO, ui->status, strlen(ui->status));
        write(STDOUT_FILENO, "\n", 1);
    } else {
        write(STDOUT_FILENO, "\n", 1);
    }

    // board (left) and move list (right)
    int board_w = 4*8 + 4; // "8 " + 8*3 + margins
    int right_x = board_w + 2;
    int max_right = cols - right_x;

    for(int r=7;r>=0;r--){
        char line[1024]; int li=0;
        li += snprintf(line+li, sizeof(line)-li, " %d ", r+1);

        for(int f=0;f<8;f++){
            int sq = SQ(f,r);
            bool dark = ((f+r)&1)!=0;

            const char *bg = dark ? ansi_dark : ansi_light;

            bool is_cursor = (ui->cursor_file==f && ui->cursor_rank==r);
            bool is_sel = (ui->selected_sq==sq);
            bool is_tgt = ui->targets[sq];
            bool is_cap = ui->captures[sq];

            const char *hl = NULL;
            if(is_sel) hl = ansi_sel;
            else if(is_cap) hl = ansi_cap;
            else if(is_tgt) hl = ansi_tgt;

            // compose square
            const char *pre = ansi_reset;
            (void)pre;

            // background + highlight + cursor inverse
            if(hl){
                li += snprintf(line+li, sizeof(line)-li, "%s", hl);
            } else {
                li += snprintf(line+li, sizeof(line)-li, "%s", bg);
            }
            if(is_cursor){
                li += snprintf(line+li, sizeof(line)-li, "%s", ansi_inv);
            }

            int p = g->board[sq];
            char pc = '.';
            if(p==EMPTY) pc = '.';
            else {
                // map to piece_to_char index: EMPTY=0, WP..BK=1..12 but our string includes '.' at 0 then 'P'..'K' then lowercase
                // piece_to_char is ".PNBRQKpnbrqk" indexed by piece code (0..12)
                pc = piece_to_char[p];
            }

            // Render as " X "
            li += snprintf(line+li, sizeof(line)-li, " %c %s", pc, ansi_reset);
        }

        // Right side move list (simple wrapping)
        if(max_right > 10){
            // print last ~16 plies, formatted as PGN lines
            char right[1024]; right[0]='\0';
            int start = g->move_count - 16;
            if(start<0) start=0;

            int ri=0;
            for(int i=start; i<g->move_count; ){
                int move_no = (i/2)+1;
                ri += snprintf(right+ri, sizeof(right)-ri, "%d. ", move_no);
                ri += snprintf(right+ri, sizeof(right)-ri, "%s ", g->san_moves[i]);
                i++;
                if(i<g->move_count){
                    ri += snprintf(right+ri, sizeof(right)-ri, "%s ", g->san_moves[i]);
                    i++;
                }
            }
            // truncate to fit line
            right[sizeof(right)-1]='\0';
            if((int)strlen(right) > max_right){
                right[max_right-3] = '.';
                right[max_right-2] = '.';
                right[max_right-1] = '\0';
            }

            li += snprintf(line+li, sizeof(line)-li, "%*s%s", (right_x - li), "", right);
        }

        li += snprintf(line+li, sizeof(line)-li, "\n");
        write(STDOUT_FILENO, line, strlen(line));
    }

    // files
    write(STDOUT_FILENO, "    a  b  c  d  e  f  g  h\n\n", 29);

    // footer help
    const char *help =
        "Keys: Arrows=move cursor | Space/Enter=select/move | u=undo ply | U=undo turn | e=engine move | r=reset | q=quit\n";
    write(STDOUT_FILENO, ansi_dim, strlen(ansi_dim));
    write(STDOUT_FILENO, help, strlen(help));
    write(STDOUT_FILENO, ansi_reset, strlen(ansi_reset));

    if(ui->promote_pending){
        const char *p = "Promotion: press q r b n (Enter defaults to q)\n";
        write(STDOUT_FILENO, p, strlen(p));
    }
}

// -------- Input --------

typedef enum {
    KEY_NONE=0,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_ENTER, KEY_SPACE,
    KEY_CHAR
} KeyKind;

typedef struct {
    KeyKind kind;
    char ch;
} Key;

static Key read_key(void){
    Key k = {KEY_NONE,0};
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if(n<=0) return k;

    if(c=='\r' || c=='\n'){ k.kind=KEY_ENTER; return k; }
    if(c==' '){ k.kind=KEY_SPACE; return k; }

    if(c==0x1b){
        char seq[2];
        if(read(STDIN_FILENO, &seq[0], 1)!=1) return k;
        if(read(STDIN_FILENO, &seq[1], 1)!=1) return k;
        if(seq[0]=='['){
            if(seq[1]=='A') k.kind=KEY_UP;
            else if(seq[1]=='B') k.kind=KEY_DOWN;
            else if(seq[1]=='C') k.kind=KEY_RIGHT;
            else if(seq[1]=='D') k.kind=KEY_LEFT;
        }
        return k;
    }

    k.kind=KEY_CHAR;
    k.ch=c;
    return k;
}

// -------- Applying moves, history, undo, end detection --------

static bool legal_move_from_to(Game *g, int from, int to, int prom_type_white, Move *out){
    Move ms[MAX_MOVES];
    int n = gen_legal_moves(g, ms);
    for(int i=0;i<n;i++){
        if(ms[i].from==from && ms[i].to==to){
            if(ms[i].flags & MF_PROMO){
                if(prom_type_white==0) prom_type_white=5; // default queen
                if(ms[i].prom!=prom_type_white) continue;
            } else {
                if(prom_type_white!=0) continue;
            }
            *out = ms[i];
            return true;
        }
    }
    return false;
}

static void apply_move_record(Game *g, Move m){
    char uci[8];
    move_to_uci(&m, uci);

    char san[32];
    san_for_move(g, m, san);

    make_move(g, m);

    // record for engine position + PGN display
    strncpy(g->uci_moves[g->move_count], uci, sizeof(g->uci_moves[g->move_count])-1);
    g->uci_moves[g->move_count][sizeof(g->uci_moves[g->move_count])-1] = '\0';

    strncpy(g->san_moves[g->move_count], san, sizeof(g->san_moves[g->move_count])-1);
    g->san_moves[g->move_count][sizeof(g->san_moves[g->move_count])-1] = '\0';

    g->move_count++;
}

static void undo_one_ply(Game *g){
    if(g->move_count<=0) return;
    unmake_move(g);
    g->move_count--;
}

static void update_end_status(Game *g, UI *ui){
    Move ms[MAX_MOVES];
    int n = gen_legal_moves(g, ms);
    if(n==0){
        if(in_check(g, g->side)){
            snprintf(ui->status, sizeof(ui->status), "Checkmate. %s wins.",
                     (g->side==0) ? "Black" : "White");
        } else {
            snprintf(ui->status, sizeof(ui->status), "Stalemate.");
        }
    } else {
        if(in_check(g, g->side)){
            snprintf(ui->status, sizeof(ui->status), "Check.");
        } else {
            ui->status[0] = '\0';
        }
    }
}

// -------- Engine turn handling --------

static bool side_is_human(const Config *cfg, int side){
    if(cfg->human_side==-1) return true;
    return cfg->human_side==side;
}

static bool side_is_engine(const Config *cfg, int side){
    if(!cfg->have_engine) return false;
    if(cfg->human_side==-1) return false;
    return cfg->human_side != side;
}

static void maybe_engine_move(Game *g, UI *ui, const Config *cfg, Engine *eng){
    if(!cfg->have_engine || !eng || !eng->ok) return;
    if(!side_is_engine(cfg, g->side)) return;

    Move bm;
    char uci[16];
    snprintf(ui->status, sizeof(ui->status), "Engine thinking...");
    // draw immediate feedback
    draw(g, ui, cfg, eng);

    if(engine_get_bestmove(eng, cfg, g, &bm, uci, sizeof(uci))){
        ui->status[0] = '\0';
        apply_move_record(g, bm);
        update_end_status(g, ui);
    } else {
        snprintf(ui->status, sizeof(ui->status), "Engine error: no bestmove.");
    }
}

// -------- Main loop actions --------

static void try_select_or_move(Game *g, UI *ui, const Config *cfg){
    int cur_sq = SQ(ui->cursor_file, ui->cursor_rank);

    if(ui->promote_pending){
        // should not happen here; handled in key processing
        return;
    }

    if(ui->selected_sq<0){
        // select piece if human to move and piece belongs to side-to-move
        if(cfg->have_engine && !side_is_human(cfg, g->side)){
            snprintf(ui->status, sizeof(ui->status), "It's engine's turn.");
            return;
        }
        int p = g->board[cur_sq];
        if(p!=EMPTY && side_of(p)==g->side){
            ui->selected_sq = cur_sq;
            ui_recalc_targets(ui, g);
            ui->status[0]='\0';
        }
        return;
    }

    // attempt move from selected to cursor
    int from = ui->selected_sq;
    int to = cur_sq;

    // if same square, deselect
    if(from==to){
        ui->selected_sq = -1;
        ui_recalc_targets(ui, g);
        ui->status[0]='\0';
        return;
    }

    // handle promotion selection if needed
    int moving = g->board[from];
    if(piece_type(moving)==1){
        int promo_rank = (g->side==0)?7:0;
        if(RANK_OF(to)==promo_rank){
            // enter promote pending; default queen if user presses Enter
            ui->promote_pending = true;
            ui->promo_from = from;
            ui->promo_to = to;
            ui->promo_choice = 'q';
            snprintf(ui->status, sizeof(ui->status), "Promotion pending.");
            return;
        }
    }

    Move m;
    if(legal_move_from_to(g, from, to, 0, &m)){
        apply_move_record(g, m);
        ui->selected_sq = -1;
        ui_recalc_targets(ui, g);
        update_end_status(g, ui);
    } else {
        snprintf(ui->status, sizeof(ui->status), "Illegal move.");
        ui->selected_sq = -1;
        ui_recalc_targets(ui, g);
    }
}

static void finalize_promotion(Game *g, UI *ui){
    int prom = 5;
    char c = (char)tolower((unsigned char)ui->promo_choice);
    if(c=='n') prom=2;
    else if(c=='b') prom=3;
    else if(c=='r') prom=4;
    else prom=5;

    Move m;
    if(legal_move_from_to(g, ui->promo_from, ui->promo_to, prom, &m)){
        apply_move_record(g, m);
        ui->status[0]='\0';
    } else {
        snprintf(ui->status, sizeof(ui->status), "Illegal promotion move.");
    }

    ui->promote_pending=false;
    ui->selected_sq = -1;
    ui_recalc_targets(ui, g);
    update_end_status(g, ui);
}

// -------- CLI parsing --------

static void usage(const char *argv0){
    fprintf(stderr,
        "Usage: %s [-engine /path/to/uci_engine] [-movetime ms | -depth N | -nodes N] [-human white|black]\n"
        "If -human not given and -engine given: default human=white.\n"
        "Controls: arrows, space/enter select/move, u undo ply, U undo full turn, e engine move, r reset, q quit.\n",
        argv0);
    exit(2);
}

static void config_default(Config *cfg){
    memset(cfg, 0, sizeof(*cfg));
    cfg->have_engine = false;
    cfg->tc_mode = TC_MOVETIME;
    cfg->tc_value = 200;
    cfg->human_side = -1; // both humans by default
    cfg->engine_path[0]='\0';
}

static void parse_args(Config *cfg, int argc, char **argv){
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i], "-engine")==0){
            if(i+1>=argc) usage(argv[0]);
            strncpy(cfg->engine_path, argv[++i], sizeof(cfg->engine_path)-1);
            cfg->engine_path[sizeof(cfg->engine_path)-1]='\0';
            cfg->have_engine = true;
            if(cfg->human_side==-1) cfg->human_side = 0; // default: human white, engine black
        } else if(strcmp(argv[i], "-movetime")==0){
            if(i+1>=argc) usage(argv[0]);
            cfg->tc_mode = TC_MOVETIME;
            cfg->tc_value = atoi(argv[++i]);
        } else if(strcmp(argv[i], "-depth")==0){
            if(i+1>=argc) usage(argv[0]);
            cfg->tc_mode = TC_DEPTH;
            cfg->tc_value = atoi(argv[++i]);
        } else if(strcmp(argv[i], "-nodes")==0){
            if(i+1>=argc) usage(argv[0]);
            cfg->tc_mode = TC_NODES;
            cfg->tc_value = atoi(argv[++i]);
        } else if(strcmp(argv[i], "-human")==0){
            if(i+1>=argc) usage(argv[0]);
            const char *s = argv[++i];
            if(strcmp(s,"white")==0) cfg->human_side = 0;
            else if(strcmp(s,"black")==0) cfg->human_side = 1;
            else usage(argv[0]);
        } else if(strcmp(argv[i], "-h")==0 || strcmp(argv[i], "--help")==0){
            usage(argv[0]);
        } else {
            usage(argv[0]);
        }
    }

    if(cfg->tc_value<=0) cfg->tc_value = (cfg->tc_mode==TC_MOVETIME)?200:10;
}

// -------- Main --------

int main(int argc, char **argv){
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    Config cfg;
    config_default(&cfg);
    parse_args(&cfg, argc, argv);

    Game g;
    UI ui;
    Engine eng;

    game_startpos(&g);
    ui_reset(&ui);

    bool engine_ready=false;
    if(cfg.have_engine){
        if(!engine_start(&eng, cfg.engine_path)){
            die("Failed to start engine: %s", cfg.engine_path);
        }
        engine_ready=true;
    } else {
        memset(&eng, 0, sizeof(eng));
    }

    term_enable_raw();

    // If engine plays White, let it move first.
    if(engine_ready) maybe_engine_move(&g, &ui, &cfg, &eng);

    while(1){
        draw(&g, &ui, &cfg, &eng);

        Key k = read_key();
        if(k.kind==KEY_NONE) continue;

        // handle promotion prompt
        if(ui.promote_pending){
            if(k.kind==KEY_ENTER || k.kind==KEY_SPACE){
                ui.promo_choice = 'q';
                finalize_promotion(&g, &ui);
                if(engine_ready) maybe_engine_move(&g, &ui, &cfg, &eng);
                continue;
            }
            if(k.kind==KEY_CHAR){
                char c = (char)tolower((unsigned char)k.ch);
                if(c=='q'||c=='r'||c=='b'||c=='n'){
                    ui.promo_choice = c;
                    finalize_promotion(&g, &ui);
                    if(engine_ready) maybe_engine_move(&g, &ui, &cfg, &eng);
                } else if(c=='q'){ /*already handled*/ }
                else if(c==27){
                    ui.promote_pending=false;
                    ui.selected_sq=-1;
                    ui_recalc_targets(&ui, &g);
                }
            }
            continue;
        }

        if(k.kind==KEY_UP){
            if(ui.cursor_rank<7) ui.cursor_rank++;
        } else if(k.kind==KEY_DOWN){
            if(ui.cursor_rank>0) ui.cursor_rank--;
        } else if(k.kind==KEY_LEFT){
            if(ui.cursor_file>0) ui.cursor_file--;
        } else if(k.kind==KEY_RIGHT){
            if(ui.cursor_file<7) ui.cursor_file++;
        } else if(k.kind==KEY_SPACE || k.kind==KEY_ENTER){
            try_select_or_move(&g, &ui, &cfg);
            if(engine_ready) maybe_engine_move(&g, &ui, &cfg, &eng);
        } else if(k.kind==KEY_CHAR){
            char c = k.ch;
            if(c=='q'){
                break;
            } else if(c=='u'){
                undo_one_ply(&g);
                ui.selected_sq=-1;
                ui_recalc_targets(&ui, &g);
                update_end_status(&g, &ui);
            } else if(c=='U'){
                // undo a full turn (helpful vs engine)
                undo_one_ply(&g);
                if(cfg.have_engine && cfg.human_side!=-1){
                    if(g.move_count>0) undo_one_ply(&g);
                }
                ui.selected_sq=-1;
                ui_recalc_targets(&ui, &g);
                update_end_status(&g, &ui);
            } else if(c=='r'){
                game_startpos(&g);
                ui_reset(&ui);
                update_end_status(&g, &ui);
                if(engine_ready){
                    engine_send(&eng, "ucinewgame");
                    engine_send(&eng, "isready");
                    engine_wait_for(&eng, "readyok", 3000);
                    maybe_engine_move(&g, &ui, &cfg, &eng);
                }
            } else if(c=='e'){
                if(engine_ready){
                    // Force engine to move for side-to-move
                    if(engine_get_bestmove(&eng, &cfg, &g, &(Move){0}, NULL, 0)){
                        // that call above isn't used; instead just call normal path:
                    }
                    maybe_engine_move(&g, &ui, &cfg, &eng);
                } else {
                    snprintf(ui.status, sizeof(ui.status), "No engine configured.");
                }
            }
        }
    }

    term_disable_raw();

    // Print final PGN-style move list to stdout on exit
    printf("\nPGN moves (SAN):\n");
    for(int i=0;i<g.move_count; ){
        int move_no = (i/2)+1;
        printf("%d. %s", move_no, g.san_moves[i]);
        i++;
        if(i<g.move_count) printf(" %s", g.san_moves[i++]);
        printf("\n");
    }

    if(cfg.have_engine){
        engine_kill(&eng);
    }
    return 0;
}
