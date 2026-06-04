/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 * 
 * Compile: gcc -o chess_gui chess_gui.c
 * Run:     ./chess_gui [uci_engine_path]
 * 
 * Controls:
 *   Arrow Keys    - Move cursor
 *   Enter/Space   - Select piece / Make move
 *   U             - Undo last move (takeback)
 *   Q             - Quit
 *   F             - Flip board
 *   N             - New game
 *   H             - Help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

/* ─── ANSI Colors & Styles ────────────────────────────────────────────── */
#define ANSI_RESET        "\033[0m"
#define ANSI_BOLD         "\033[1m"
#define ANSI_DIM          "\033[2m"

/* True-color helpers */
#define FG(r,g,b)         "\033[38;2;" #r ";" #g ";" #b "m"
#define BG(r,g,b)         "\033[48;2;" #r ";" #g ";" #b "m"

/* Board square colors */
#define BG_LIGHT          "\033[48;2;240;217;181m"   /* cream */
#define BG_DARK           "\033[48;2;181;136;99m"    /* brown */
#define BG_SEL            "\033[48;2;100;200;100m"   /* green  - selected */
#define BG_MOVE           "\033[48;2;200;230;100m"   /* yellow - valid move */
#define BG_CURSOR         "\033[48;2;80;160;220m"    /* blue   - cursor */
#define BG_LAST           "\033[48;2;205;210;106m"   /* gold   - last move */
#define BG_CHECK          "\033[48;2;220;80;80m"     /* red    - king in check */

#define FG_WHITE_PIECE    "\033[38;2;255;255;255m"
#define FG_BLACK_PIECE    "\033[38;2;20;20;20m"
#define FG_BORDER         "\033[38;2;200;200;200m"
#define BG_BORDER         "\033[48;2;50;50;60m"
#define FG_LABEL          "\033[38;2;180;180;180m"
#define BG_PANEL          "\033[48;2;35;35;45m"
#define FG_PANEL          "\033[38;2;220;220;220m"
#define FG_HIGHLIGHT      "\033[38;2;255;220;50m"
#define FG_ERROR          "\033[38;2;255;80;80m"
#define FG_SUCCESS        "\033[38;2;80;255;120m"
#define FG_INFO           "\033[38;2;100;180;255m"

/* ─── Chess Constants ─────────────────────────────────────────────────── */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  8
#define BLACK  16

#define MAX_MOVES      512
#define MAX_HISTORY    512
#define MAX_PGN        8192
#define MAX_VALID      256
#define ENGINE_BUF     4096

/* Piece encoding: type | color */
#define WP (WHITE|PAWN)
#define WN (WHITE|KNIGHT)
#define WB (WHITE|BISHOP)
#define WR (WHITE|ROOK)
#define WQ (WHITE|QUEEN)
#define WK (WHITE|KING)
#define BP (BLACK|PAWN)
#define BN (BLACK|KNIGHT)
#define BB (BLACK|BISHOP)
#define BR (BLACK|ROOK)
#define BQ (BLACK|QUEEN)
#define BK (BLACK|KING)

#define PIECE_TYPE(p)  ((p) & 7)
#define PIECE_COLOR(p) ((p) & 24)
#define IS_WHITE(p)    (((p) & 24) == WHITE)
#define IS_BLACK(p)    (((p) & 24) == BLACK)

/* Unicode chess pieces */
static const char *PIECE_UNICODE[2][7] = {
    /* empty, pawn,   knight, bishop, rook,   queen,  king  */
    { "  ", " ♟ ", " ♞ ", " ♝ ", " ♜ ", " ♛ ", " ♚ " }, /* black */
    { "  ", " ♙ ", " ♘ ", " ♗ ", " ♖ ", " ♕ ", " ♔ " }, /* white */
};

static const char PIECE_CHAR[] = ".PNBRQK";

/* ─── Move Representation ─────────────────────────────────────────────── */
typedef struct {
    int from, to;          /* 0-63 */
    int piece;             /* piece moved */
    int captured;          /* piece captured (0 if none) */
    int promotion;         /* promotion piece type (0 if none) */
    int castle;            /* 0=none,1=kingside,2=queenside */
    int en_passant;        /* was this move en passant? */
    int ep_square_before;  /* en passant square before this move */
    int castling_before;   /* castling rights before this move */
    int halfmove_before;   /* halfmove clock before this move */
    char san[16];          /* Standard Algebraic Notation */
    char uci[6];           /* UCI notation e.g. "e2e4" */
} Move;

/* ─── Game State ──────────────────────────────────────────────────────── */
typedef struct {
    int board[64];
    int side;              /* WHITE or BLACK to move */
    int ep_square;         /* en passant target square (-1 if none) */
    int castling;          /* bits: 1=WK,2=WQ,4=BK,8=BQ */
    int halfmove;          /* halfmove clock */
    int fullmove;          /* fullmove number */
    Move history[MAX_HISTORY];
    int hist_count;
    char pgn[MAX_PGN];
    int pgn_len;
    int in_check;          /* is current side in check? */
    int game_over;         /* 0=ongoing,1=white wins,2=black wins,3=draw */
    char result_str[64];
} GameState;

/* ─── UI State ────────────────────────────────────────────────────────── */
typedef struct {
    int cursor_sq;         /* cursor position 0-63 */
    int selected_sq;       /* selected piece square (-1 if none) */
    int flipped;           /* board orientation */
    int valid_moves[MAX_VALID];
    int valid_count;
    char status_msg[256];
    int status_color;      /* 0=normal,1=error,2=success,3=info */
    char promotion_pending;/* awaiting promotion choice */
    int prom_from, prom_to;
} UIState;

/* ─── Engine State ────────────────────────────────────────────────────── */
typedef struct {
    pid_t pid;
    int in_fd;             /* write to engine */
    int out_fd;            /* read from engine */
    int active;
    char name[128];
    char best_move[8];
    int thinking;
    int engine_color;      /* which color engine plays (WHITE/BLACK/0=none) */
} EngineState;

/* ─── Globals ─────────────────────────────────────────────────────────── */
static GameState G;
static UIState   UI;
static EngineState ENG;
static struct termios orig_termios;
static int terminal_rows = 40, terminal_cols = 120;

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Terminal Utilities                                                     */
/* ═══════════════════════════════════════════════════════════════════════ */

static void get_terminal_size(void) {
    /* Use tput or default */
    FILE *f = popen("tput lines 2>/dev/null", "r");
    if (f) { fscanf(f, "%d", &terminal_rows); pclose(f); }
    f = popen("tput cols 2>/dev/null", "r");
    if (f) { fscanf(f, "%d", &terminal_cols); pclose(f); }
    if (terminal_rows < 30) terminal_rows = 30;
    if (terminal_cols < 80) terminal_cols = 80;
}

static void enable_raw_mode(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void clear_screen(void) { printf("\033[2J\033[H"); }
static void hide_cursor(void)  { printf("\033[?25l"); }
static void show_cursor(void)  { printf("\033[?25h"); }
static void move_cursor(int r, int c) { printf("\033[%d;%dH", r, c); }

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Chess Logic                                                            */
/* ═══════════════════════════════════════════════════════════════════════ */

static int sq(int rank, int file) { return rank * 8 + file; }
static int rank_of(int s) { return s >> 3; }
static int file_of(int s) { return s & 7; }

static void init_board(void) {
    memset(&G, 0, sizeof(G));
    /* White pieces */
    G.board[sq(0,0)]=WR; G.board[sq(0,1)]=WN; G.board[sq(0,2)]=WB;
    G.board[sq(0,3)]=WQ; G.board[sq(0,4)]=WK; G.board[sq(0,5)]=WB;
    G.board[sq(0,6)]=WN; G.board[sq(0,7)]=WR;
    for(int f=0;f<8;f++) G.board[sq(1,f)]=WP;
    /* Black pieces */
    G.board[sq(7,0)]=BR; G.board[sq(7,1)]=BN; G.board[sq(7,2)]=BB;
    G.board[sq(7,3)]=BQ; G.board[sq(7,4)]=BK; G.board[sq(7,5)]=BB;
    G.board[sq(7,6)]=BN; G.board[sq(7,7)]=BR;
    for(int f=0;f<8;f++) G.board[sq(6,f)]=BP;
    G.side      = WHITE;
    G.ep_square = -1;
    G.castling  = 15; /* all rights */
    G.halfmove  = 0;
    G.fullmove  = 1;
    G.hist_count = 0;
    G.pgn_len   = 0;
    G.pgn[0]    = '\0';
    G.in_check  = 0;
    G.game_over = 0;
    G.result_str[0] = '\0';
}

static int find_king(int color) {
    int king = (color == WHITE) ? WK : BK;
    for(int s=0;s<64;s++) if(G.board[s]==king) return s;
    return -1;
}

/* Check if square 'sq' is attacked by 'color' */
static int is_attacked(int target, int color) {
    int opp = (color == WHITE) ? BLACK : WHITE;
    (void)opp;

    /* Pawns */
    if (color == WHITE) {
        int r = rank_of(target), f = file_of(target);
        if (r > 0) {
            if (f > 0 && G.board[sq(r-1,f-1)] == WP) return 1;
            if (f < 7 && G.board[sq(r-1,f+1)] == WP) return 1;
        }
    } else {
        int r = rank_of(target), f = file_of(target);
        if (r < 7) {
            if (f > 0 && G.board[sq(r+1,f-1)] == BP) return 1;
            if (f < 7 && G.board[sq(r+1,f+1)] == BP) return 1;
        }
    }

    /* Knights */
    {
        int knight = (color == WHITE) ? WN : BN;
        int r = rank_of(target), f = file_of(target);
        int jumps[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for(int i=0;i<8;i++) {
            int nr=r+jumps[i][0], nf=f+jumps[i][1];
            if(nr>=0&&nr<8&&nf>=0&&nf<8&&G.board[sq(nr,nf)]==knight) return 1;
        }
    }

    /* Bishops / Queens (diagonals) */
    {
        int bishop = (color==WHITE)?WB:BB;
        int queen  = (color==WHITE)?WQ:BQ;
        int dirs[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
        for(int d=0;d<4;d++) {
            int r=rank_of(target)+dirs[d][0], f=file_of(target)+dirs[d][1];
            while(r>=0&&r<8&&f>=0&&f<8) {
                int p=G.board[sq(r,f)];
                if(p) { if(p==bishop||p==queen) return 1; break; }
                r+=dirs[d][0]; f+=dirs[d][1];
            }
        }
    }

    /* Rooks / Queens (straights) */
    {
        int rook  = (color==WHITE)?WR:BR;
        int queen = (color==WHITE)?WQ:BQ;
        int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        for(int d=0;d<4;d++) {
            int r=rank_of(target)+dirs[d][0], f=file_of(target)+dirs[d][1];
            while(r>=0&&r<8&&f>=0&&f<8) {
                int p=G.board[sq(r,f)];
                if(p) { if(p==rook||p==queen) return 1; break; }
                r+=dirs[d][0]; f+=dirs[d][1];
            }
        }
    }

    /* King */
    {
        int king = (color==WHITE)?WK:BK;
        int r=rank_of(target), f=file_of(target);
        for(int dr=-1;dr<=1;dr++) for(int df=-1;df<=1;df++) {
            if(!dr&&!df) continue;
            int nr=r+dr, nf=f+df;
            if(nr>=0&&nr<8&&nf>=0&&nf<8&&G.board[sq(nr,nf)]==king) return 1;
        }
    }
    return 0;
}

static int in_check(int color) {
    int ks = find_king(color);
    if(ks<0) return 0;
    return is_attacked(ks, (color==WHITE)?BLACK:WHITE);
}

/* Apply move, return 1 if legal (doesn't leave own king in check) */
static int apply_move(Move *m, int check_legal) {
    int board_save[64];
    int ep_save = G.ep_square;
    int castle_save = G.castling;
    int side_save = G.side;
    memcpy(board_save, G.board, sizeof(G.board));

    int p = G.board[m->from];
    int type = PIECE_TYPE(p);
    int color = PIECE_COLOR(p);

    m->ep_square_before = G.ep_square;
    m->castling_before  = G.castling;
    m->halfmove_before  = G.halfmove;
    m->piece    = p;
    m->captured = 0;
    m->castle   = 0;
    m->en_passant = 0;

    /* En passant capture */
    if(type == PAWN && m->to == G.ep_square) {
        m->en_passant = 1;
        int cap_sq = (color==WHITE) ? m->to - 8 : m->to + 8;
        m->captured = G.board[cap_sq];
        G.board[cap_sq] = EMPTY;
    } else {
        m->captured = G.board[m->to];
    }

    G.board[m->to]   = p;
    G.board[m->from] = EMPTY;

    /* Promotion */
    if(type==PAWN && (rank_of(m->to)==7||rank_of(m->to)==0)) {
        if(m->promotion == 0) m->promotion = QUEEN;
        G.board[m->to] = color | m->promotion;
    }

    /* Castling move */
    if(type==KING) {
        int df = file_of(m->to) - file_of(m->from);
        if(df==2) { /* kingside */
            m->castle = 1;
            int r = rank_of(m->from);
            G.board[sq(r,5)] = G.board[sq(r,7)];
            G.board[sq(r,7)] = EMPTY;
        } else if(df==-2) { /* queenside */
            m->castle = 2;
            int r = rank_of(m->from);
            G.board[sq(r,3)] = G.board[sq(r,0)];
            G.board[sq(r,0)] = EMPTY;
        }
    }

    /* Update en passant square */
    G.ep_square = -1;
    if(type==PAWN) {
        int dr = rank_of(m->to) - rank_of(m->from);
        if(dr==2) G.ep_square = m->from + 8;
        else if(dr==-2) G.ep_square = m->from - 8;
    }

    /* Update castling rights */
    if(p==WK) G.castling &= ~3;
    if(p==BK) G.castling &= ~12;
    if(m->from==sq(0,0)||m->to==sq(0,0)) G.castling &= ~2;
    if(m->from==sq(0,7)||m->to==sq(0,7)) G.castling &= ~1;
    if(m->from==sq(7,0)||m->to==sq(7,0)) G.castling &= ~8;
    if(m->from==sq(7,7)||m->to==sq(7,7)) G.castling &= ~4;

    /* Halfmove clock */
    if(type==PAWN || m->captured) G.halfmove = 0;
    else G.halfmove++;

    G.side = (color==WHITE) ? BLACK : WHITE;

    if(check_legal) {
        if(in_check(color)) {
            /* Illegal: restore */
            memcpy(G.board, board_save, sizeof(G.board));
            G.ep_square = ep_save;
            G.castling  = castle_save;
            G.side      = side_save;
            G.halfmove  = m->halfmove_before;
            return 0;
        }
    }
    return 1;
}

static void undo_move(void) {
    if(G.hist_count == 0) return;
    Move *m = &G.history[G.hist_count-1];

    /* Restore board */
    G.board[m->from] = m->piece;
    G.board[m->to]   = m->captured ? m->captured : EMPTY;

    if(m->en_passant) {
        int color = PIECE_COLOR(m->piece);
        int cap_sq = (color==WHITE) ? m->to - 8 : m->to + 8;
        G.board[cap_sq] = m->captured;
        G.board[m->to]  = EMPTY;
    }

    if(m->castle==1) {
        int r = rank_of(m->from);
        G.board[sq(r,7)] = G.board[sq(r,5)];
        G.board[sq(r,5)] = EMPTY;
    } else if(m->castle==2) {
        int r = rank_of(m->from);
        G.board[sq(r,0)] = G.board[sq(r,3)];
        G.board[sq(r,3)] = EMPTY;
    }

    G.ep_square = m->ep_square_before;
    G.castling  = m->castling_before;
    G.halfmove  = m->halfmove_before;
    G.side      = PIECE_COLOR(m->piece);
    if(G.side==BLACK) G.fullmove--;

    G.hist_count--;
    G.game_over = 0;
    G.in_check  = in_check(G.side);

    /* Rebuild PGN */
    G.pgn[0]   = '\0';
    G.pgn_len  = 0;
    int save_hist = G.hist_count;
    /* The SAN strings are already stored in history */
    int fn = 1;
    for(int i=0;i<save_hist;i++) {
        char tmp[32];
        int color_i = PIECE_COLOR(G.history[i].piece);
        if(color_i==WHITE) {
            snprintf(tmp,sizeof(tmp),"%d. ",fn++);
            int l=strlen(tmp);
            if(G.pgn_len+l<MAX_PGN-1){memcpy(G.pgn+G.pgn_len,tmp,l);G.pgn_len+=l;}
        }
        int l=strlen(G.history[i].san);
        if(G.pgn_len+l+2<MAX_PGN-1){
            memcpy(G.pgn+G.pgn_len,G.history[i].san,l);
            G.pgn_len+=l;
            G.pgn[G.pgn_len++]=' ';
        }
    }
    G.pgn[G.pgn_len]='\0';
}

/* Generate SAN for a move (called after move is validated) */
static void make_san(Move *m, int board_before[64], int ep_before,
                     int castle_before, int side_before) {
    char san[16];
    int p   = m->piece;
    int type = PIECE_TYPE(p);
    int color= PIECE_COLOR(p);
    int pos = 0;

    if(m->castle==1) { strcpy(san,"O-O");   goto check_suffix; }
    if(m->castle==2) { strcpy(san,"O-O-O"); goto check_suffix; }

    if(type != PAWN) {
        san[pos++] = PIECE_CHAR[type];
        /* Disambiguation */
        int ambig_rank=0, ambig_file=0, ambig=0;
        for(int s=0;s<64;s++) {
            if(s==m->from) continue;
            if(board_before[s] != p) continue;
            /* Can this piece also go to m->to? */
            Move tm;
            int b2[64]; memcpy(b2,board_before,sizeof(b2));
            /* Quick check without full generation */
            /* We'll use a simplified approach */
            int br=rank_of(s), bf=file_of(s);
            int tr=rank_of(m->to), tf=file_of(m->to);
            int can=0;
            if(type==KNIGHT) {
                int dr=abs(br-tr),df=abs(bf-tf);
                can=(dr==2&&df==1)||(dr==1&&df==2);
            } else if(type==BISHOP) {
                if(abs(br-tr)==abs(bf-tf)) {
                    can=1;
                    int dr=(tr>br)?1:-1, df=(tf>bf)?1:-1;
                    for(int rr=br+dr,ff=bf+df;rr!=tr||ff!=tf;rr+=dr,ff+=df)
                        if(board_before[sq(rr,ff)]){can=0;break;}
                }
            } else if(type==ROOK) {
                if(br==tr||bf==tf) {
                    can=1;
                    if(br==tr){int df2=(tf>bf)?1:-1;for(int ff=bf+df2;ff!=tf;ff+=df2)if(board_before[sq(br,ff)]){can=0;break;}}
                    else{int dr=(tr>br)?1:-1;for(int rr=br+dr;rr!=tr;rr+=dr)if(board_before[sq(rr,bf)]){can=0;break;}}
                }
            } else if(type==QUEEN) {
                if(br==tr||bf==tf) {
                    can=1;
                    if(br==tr){int df2=(tf>bf)?1:-1;for(int ff=bf+df2;ff!=tf;ff+=df2)if(board_before[sq(br,ff)]){can=0;break;}}
                    else{int dr=(tr>br)?1:-1;for(int rr=br+dr;rr!=tr;rr+=dr)if(board_before[sq(rr,bf)]){can=0;break;}}
                } else if(abs(br-tr)==abs(bf-tf)) {
                    can=1;
                    int dr=(tr>br)?1:-1, df=(tf>bf)?1:-1;
                    for(int rr=br+dr,ff=bf+df;rr!=tr||ff!=tf;rr+=dr,ff+=df)
                        if(board_before[sq(rr,ff)]){can=0;break;}
                }
            }
            if(can) {
                /* Check legality of this alternate move */
                int save[64]; memcpy(save,G.board,sizeof(G.board));
                int save_ep=G.ep_square,save_c=G.castling,save_s=G.side;
                int save_hm=G.halfmove;
                memcpy(G.board,board_before,sizeof(G.board));
                G.ep_square=ep_before; G.castling=castle_before;
                G.side=side_before;
                Move tm2; memset(&tm2,0,sizeof(tm2));
                tm2.from=s; tm2.to=m->to;
                int legal=apply_move(&tm2,1);
                memcpy(G.board,save,sizeof(G.board));
                G.ep_square=save_ep; G.castling=save_c;
                G.side=save_s; G.halfmove=save_hm;
                if(legal) {
                    ambig=1;
                    if(bf==file_of(m->from)) ambig_rank=1;
                    else ambig_file=1;
                }
            }
        }
        if(ambig) {
            if(ambig_file && !ambig_rank) san[pos++]='a'+file_of(m->from);
            else if(!ambig_file && ambig_rank) san[pos++]='1'+rank_of(m->from);
            else { san[pos++]='a'+file_of(m->from); san[pos++]='1'+rank_of(m->from); }
        }
        if(m->captured) san[pos++]='x';
    } else {
        /* Pawn */
        if(m->captured || m->en_passant) {
            san[pos++]='a'+file_of(m->from);
            san[pos++]='x';
        }
    }

    san[pos++]='a'+file_of(m->to);
    san[pos++]='1'+rank_of(m->to);

    if(m->promotion) {
        san[pos++]='=';
        san[pos++]=PIECE_CHAR[m->promotion];
    }

check_suffix:
    san[pos]='\0';
    /* Check/checkmate */
    int opp=(color==WHITE)?BLACK:WHITE;
    if(in_check(opp)) {
        /* Check for checkmate - simplified: try to find any legal move */
        /* We'll add + and # later in display; for now just add + */
        /* Full checkmate detection in generate_valid_moves */
        san[pos++]='+'; san[pos]='\0';
    }
    strncpy(m->san,san,15);
}

/* Encode move as UCI string */
static void make_uci(Move *m) {
    char *u = m->uci;
    u[0]='a'+file_of(m->from); u[1]='1'+rank_of(m->from);
    u[2]='a'+file_of(m->to);   u[3]='1'+rank_of(m->to);
    u[4]='\0';
    if(m->promotion) {
        u[4]=tolower(PIECE_CHAR[m->promotion]);
        u[5]='\0';
    }
}

/* Generate all pseudo-legal to-squares for piece on 'from' */
static int generate_moves_for(int from, Move *moves) {
    int count = 0;
    int p = G.board[from];
    if(!p) return 0;
    int type  = PIECE_TYPE(p);
    int color = PIECE_COLOR(p);
    int opp   = (color==WHITE)?BLACK:WHITE;
    int r=rank_of(from), f=file_of(from);

    #define ADD(to_sq) do { \
        Move *mv=&moves[count++]; \
        memset(mv,0,sizeof(*mv)); \
        mv->from=from; mv->to=(to_sq); \
    } while(0)

    switch(type) {
    case PAWN: {
        int dir=(color==WHITE)?1:-1;
        int start_rank=(color==WHITE)?1:6;
        int promo_rank=(color==WHITE)?7:0;
        int nr=r+dir;
        if(nr>=0&&nr<8) {
            if(!G.board[sq(nr,f)]) {
                ADD(sq(nr,f));
                if(r==start_rank && !G.board[sq(r+2*dir,f)])
                    ADD(sq(r+2*dir,f));
            }
            if(f>0 && (PIECE_COLOR(G.board[sq(nr,f-1)])==opp||sq(nr,f-1)==G.ep_square))
                ADD(sq(nr,f-1));
            if(f<7 && (PIECE_COLOR(G.board[sq(nr,f+1)])==opp||sq(nr,f+1)==G.ep_square))
                ADD(sq(nr,f+1));
        }
        /* Handle promotions inline */
        for(int i=0;i<count;i++) {
            if(rank_of(moves[i].to)==promo_rank) {
                moves[i].promotion=QUEEN; /* default; UI will ask */
            }
        }
        break;
    }
    case KNIGHT: {
        int jumps[8][2]={{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for(int i=0;i<8;i++){
            int nr2=r+jumps[i][0],nf=f+jumps[i][1];
            if(nr2>=0&&nr2<8&&nf>=0&&nf<8&&PIECE_COLOR(G.board[sq(nr2,nf)])!=color)
                ADD(sq(nr2,nf));
        }
        break;
    }
    case BISHOP: {
        int dirs[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
        for(int d=0;d<4;d++){
            for(int i=1;i<8;i++){
                int nr2=r+i*dirs[d][0],nf=f+i*dirs[d][1];
                if(nr2<0||nr2>7||nf<0||nf>7) break;
                int pc=G.board[sq(nr2,nf)];
                if(PIECE_COLOR(pc)==color) break;
                ADD(sq(nr2,nf));
                if(pc) break;
            }
        }
        break;
    }
    case ROOK: {
        int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        for(int d=0;d<4;d++){
            for(int i=1;i<8;i++){
                int nr2=r+i*dirs[d][0],nf=f+i*dirs[d][1];
                if(nr2<0||nr2>7||nf<0||nf>7) break;
                int pc=G.board[sq(nr2,nf)];
                if(PIECE_COLOR(pc)==color) break;
                ADD(sq(nr2,nf));
                if(pc) break;
            }
        }
        break;
    }
    case QUEEN: {
        int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        for(int d=0;d<8;d++){
            for(int i=1;i<8;i++){
                int nr2=r+i*dirs[d][0],nf=f+i*dirs[d][1];
                if(nr2<0||nr2>7||nf<0||nf>7) break;
                int pc=G.board[sq(nr2,nf)];
                if(PIECE_COLOR(pc)==color) break;
                ADD(sq(nr2,nf));
                if(pc) break;
            }
        }
        break;
    }
    case KING: {
        for(int dr=-1;dr<=1;dr++) for(int df2=-1;df2<=1;df2++){
            if(!dr&&!df2) continue;
            int nr2=r+dr,nf=f+df2;
            if(nr2>=0&&nr2<8&&nf>=0&&nf<8&&PIECE_COLOR(G.board[sq(nr2,nf)])!=color)
                ADD(sq(nr2,nf));
        }
        /* Castling */
        int back=(color==WHITE)?0:7;
        if(r==back && f==4 && !in_check(color)) {
            /* Kingside */
            if((G.castling&(color==WHITE?1:4)) &&
               !G.board[sq(back,5)]&&!G.board[sq(back,6)] &&
               !is_attacked(sq(back,5),(color==WHITE)?BLACK:WHITE) &&
               !is_attacked(sq(back,6),(color==WHITE)?BLACK:WHITE))
                ADD(sq(back,6));
            /* Queenside */
            if((G.castling&(color==WHITE?2:8)) &&
               !G.board[sq(back,3)]&&!G.board[sq(back,2)]&&!G.board[sq(back,1)] &&
               !is_attacked(sq(back,3),(color==WHITE)?BLACK:WHITE) &&
               !is_attacked(sq(back,2),(color==WHITE)?BLACK:WHITE))
                ADD(sq(back,2));
        }
        break;
    }
    }
    return count;
    #undef ADD
}

/* Return valid (legal) destination squares for piece on 'from' */
static int get_valid_destinations(int from, int *dests) {
    Move moves[64];
    int n = generate_moves_for(from, moves);
    int count = 0;
    for(int i=0;i<n;i++) {
        /* Save state */
        int board_save[64]; memcpy(board_save,G.board,sizeof(G.board));
        int ep_save=G.ep_square, cs=G.castling, ss=G.side, hm=G.halfmove;
        moves[i].ep_square_before=G.ep_square;
        moves[i].castling_before =G.castling;
        moves[i].halfmove_before =G.halfmove;
        int legal = apply_move(&moves[i],1);
        /* Restore */
        memcpy(G.board,board_save,sizeof(G.board));
        G.ep_square=ep_save; G.castling=cs; G.side=ss; G.halfmove=hm;
        if(legal) dests[count++]=moves[i].to;
    }
    return count;
}

/* Check if side has any legal move */
static int has_legal_move(int color) {
    int old_side = G.side;
    G.side = color;
    for(int s=0;s<64;s++) {
        if(!G.board[s]||PIECE_COLOR(G.board[s])!=color) continue;
        Move moves[64];
        int n=generate_moves_for(s,moves);
        for(int i=0;i<n;i++){
            int board_save[64]; memcpy(board_save,G.board,sizeof(G.board));
            int ep_save=G.ep_square,cs=G.castling,ss=G.side,hm=G.halfmove;
            moves[i].ep_square_before=G.ep_square;
            moves[i].castling_before =G.castling;
            moves[i].halfmove_before =G.halfmove;
            int legal=apply_move(&moves[i],1);
            memcpy(G.board,board_save,sizeof(G.board));
            G.ep_square=ep_save;G.castling=cs;G.side=ss;G.halfmove=hm;
            if(legal){G.side=old_side;return 1;}
        }
    }
    G.side=old_side;
    return 0;
}

static int is_insufficient_material(void) {
    int wn=0,wb=0,bn=0,bb=0;
    for(int s=0;s<64;s++){
        int p=G.board[s];
        if(!p) continue;
        int t=PIECE_TYPE(p);
        if(t==PAWN||t==ROOK||t==QUEEN) return 0;
        if(IS_WHITE(p)){if(t==KNIGHT)wn++;if(t==BISHOP)wb++;}
        else{if(t==KNIGHT)bn++;if(t==BISHOP)bb++;}
    }
    if(wn==0&&wb==0&&bn==0&&bb==0) return 1; /* K vs K */
    if(wb==1&&wn==0&&bn==0&&bb==0) return 1; /* K+B vs K */
    if(bb==1&&bn==0&&wn==0&&wb==0) return 1;
    if(wn==1&&wb==0&&bn==0&&bb==0) return 1; /* K+N vs K */
    if(bn==1&&bb==0&&wn==0&&wb==0) return 1;
    return 0;
}

/* Make a move from from->to, return 1 if successful */
static int make_move(int from, int to, int promo) {
    if(G.game_over) return 0;
    int p=G.board[from];
    if(!p||PIECE_COLOR(p)!=G.side) return 0;

    Move moves[64];
    int n=generate_moves_for(from,moves);
    Move *chosen=NULL;
    for(int i=0;i<n;i++){
        if(moves[i].to==to) {
            if(promo && moves[i].promotion) moves[i].promotion=promo;
            chosen=&moves[i]; break;
        }
    }
    if(!chosen) return 0;

    /* Save board before move for SAN generation */
    int board_before[64]; memcpy(board_before,G.board,sizeof(G.board));
    int ep_before=G.ep_square, cb=G.castling, sb=G.side;

    if(!apply_move(chosen,1)) return 0;

    /* Update fullmove */
    if(G.side==WHITE) G.fullmove++;

    /* Generate SAN and UCI */
    make_san(chosen, board_before, ep_before, cb, sb);
    make_uci(chosen);

    /* Check for checkmate/stalemate */
    G.in_check = in_check(G.side);
    if(!has_legal_move(G.side)) {
        if(G.in_check) {
            /* Checkmate - fix SAN: replace + with # */
            int l=strlen(chosen->san);
            if(l>0&&chosen->san[l-1]=='+') chosen->san[l-1]='#';
            else { chosen->san[l]='#'; chosen->san[l+1]='\0'; }
            G.game_over = (G.side==WHITE)?2:1;
            snprintf(G.result_str,sizeof(G.result_str),
                     "Checkmate! %s wins.",(G.game_over==1)?"White":"Black");
        } else {
            G.game_over=3;
            snprintf(G.result_str,sizeof(G.result_str),"Stalemate! Draw.");
        }
    } else if(G.halfmove>=100) {
        G.game_over=3;
        snprintf(G.result_str,sizeof(G.result_str),"Draw by 50-move rule.");
    } else if(is_insufficient_material()) {
        G.game_over=3;
        snprintf(G.result_str,sizeof(G.result_str),"Draw: Insufficient material.");
    }

    /* Store in history */
    G.history[G.hist_count++] = *chosen;

    /* Append to PGN */
    char tmp[32];
    if(PIECE_COLOR(chosen->piece)==WHITE) {
        snprintf(tmp,sizeof(tmp),"%d. ",G.fullmove-1);
        int l=strlen(tmp);
        if(G.pgn_len+l<MAX_PGN-1){memcpy(G.pgn+G.pgn_len,tmp,l);G.pgn_len+=l;}
    }
    {
        int l=strlen(chosen->san);
        if(G.pgn_len+l+2<MAX_PGN-1){
            memcpy(G.pgn+G.pgn_len,chosen->san,l);
            G.pgn_len+=l;
            G.pgn[G.pgn_len++]=' ';
            G.pgn[G.pgn_len]='\0';
        }
    }

    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  UCI Engine Interface                                                   */
/* ═══════════════════════════════════════════════════════════════════════ */

static void engine_write(const char *msg) {
    if(!ENG.active) return;
    write(ENG.in_fd, msg, strlen(msg));
    write(ENG.in_fd, "\n", 1);
}

static int engine_read_line(char *buf, int maxlen, int timeout_ms) {
    if(!ENG.active) return 0;
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(ENG.out_fd, &fds);
    tv.tv_sec  = timeout_ms/1000;
    tv.tv_usec = (timeout_ms%1000)*1000;
    int r=select(ENG.out_fd+1,&fds,NULL,NULL,&tv);
    if(r<=0) return 0;
    int n=read(ENG.out_fd,buf,maxlen-1);
    if(n<=0) return 0;
    buf[n]='\0';
    return n;
}

static int start_engine(const char *path) {
    int pin[2], pout[2];
    if(pipe(pin)||pipe(pout)) return 0;
    pid_t pid=fork();
    if(pid<0) return 0;
    if(pid==0) {
        dup2(pin[0],STDIN_FILENO);
        dup2(pout[1],STDOUT_FILENO);
        close(pin[1]); close(pout[0]);
        execl(path,path,(char*)NULL);
        exit(1);
    }
    close(pin[0]); close(pout[1]);
    ENG.in_fd  = pin[1];
    ENG.out_fd = pout[0];
    ENG.pid    = pid;
    ENG.active = 1;
    /* Make reads non-blocking */
    int flags=fcntl(ENG.out_fd,F_GETFL,0);
    fcntl(ENG.out_fd,F_SETFL,flags|O_NONBLOCK);

    engine_write("uci");
    char buf[ENGINE_BUF];
    int waited=0;
    while(waited<3000) {
        int n=engine_read_line(buf,sizeof(buf),100);
        waited+=100;
        if(n>0) {
            buf[n]='\0';
            if(strstr(buf,"uciok")) break;
            if(strstr(buf,"id name")) {
                char *s=strstr(buf,"id name");
                if(s) sscanf(s,"id name %127[^\n]",ENG.name);
            }
        }
    }
    engine_write("isready");
    waited=0;
    while(waited<3000) {
        int n=engine_read_line(buf,sizeof(buf),100);
        waited+=100;
        if(n>0&&strstr(buf,"readyok")) break;
    }
    engine_write("ucinewgame");
    return 1;
}

static void stop_engine(void) {
    if(!ENG.active) return;
    engine_write("quit");
    usleep(100000);
    close(ENG.in_fd);
    close(ENG.out_fd);
    int status;
    waitpid(ENG.pid,&status,WNOHANG);
    ENG.active=0;
}

static void engine_go(int movetime_ms) {
    if(!ENG.active) return;
    /* Build position string */
    char pos[4096];
    int n=snprintf(pos,sizeof(pos),"position startpos");
    if(G.hist_count>0) {
        n+=snprintf(pos+n,sizeof(pos)-n," moves");
        for(int i=0;i<G.hist_count;i++)
            n+=snprintf(pos+n,sizeof(pos)-n," %s",G.history[i].uci);
    }
    engine_write(pos);
    char go[64];
    snprintf(go,sizeof(go),"go movetime %d",movetime_ms);
    engine_write(go);
    ENG.thinking=1;
}

static int engine_poll_bestmove(void) {
    if(!ENG.active||!ENG.thinking) return 0;
    char buf[ENGINE_BUF];
    int n=engine_read_line(buf,sizeof(buf),0);
    if(n<=0) return 0;
    buf[n]='\0';
    char *bm=strstr(buf,"bestmove");
    if(bm) {
        char mv[6];
        sscanf(bm,"bestmove %5s",mv);
        strncpy(ENG.best_move,mv,5);
        ENG.thinking=0;
        return 1;
    }
    return 0;
}

static void engine_apply_bestmove(void) {
    char *bm=ENG.best_move;
    if(!bm[0]) return;
    int ff=bm[0]-'a', fr=bm[1]-'1';
    int tf=bm[2]-'a', tr=bm[3]-'1';
    int from=sq(fr,ff), to=sq(tr,tf);
    int promo=0;
    if(bm[4]) {
        switch(tolower(bm[4])){
            case 'q': promo=QUEEN;  break;
            case 'r': promo=ROOK;   break;
            case 'b': promo=BISHOP; break;
            case 'n': promo=KNIGHT; break;
        }
    }
    make_move(from,to,promo);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Display / Rendering                                                    */
/* ═══════════════════════════════════════════════════════════════════════ */

/* Returns display row,col for board start */
static void board_origin(int *row, int *col) {
    *row = 3;
    *col = 4;
}

/* Convert board square to visual square based on flip */
static int vis_sq(int s) {
    if(!UI.flipped) return s;
    /* flip: mirror rank and file */
    return sq(7-rank_of(s), 7-file_of(s));
}
/* Convert visual row/col to board square */
static int vis_to_sq(int vr, int vf) {
    if(!UI.flipped) return sq(vr,vf);
    return sq(7-vr,7-vf);
}
/* Visual rank/file from square */
static int vrank(int s) { return UI.flipped ? 7-rank_of(s) : rank_of(s); }
static int vfile(int s) { return UI.flipped ? 7-file_of(s) : file_of(s); }

static int is_valid_dest(int sq2) {
    for(int i=0;i<UI.valid_count;i++)
        if(UI.valid_moves[i]==sq2) return 1;
    return 0;
}

static int last_move_sq(int s) {
    if(G.hist_count==0) return 0;
    Move *m=&G.history[G.hist_count-1];
    return (s==m->from||s==m->to);
}

static void draw_board(void) {
    int br, bc;
    board_origin(&br, &bc);

    /* Board frame top */
    move_cursor(br-1, bc-1);
    printf(BG_BORDER FG_BORDER ANSI_BOLD);
    printf(" ╔════════════════════════════════╗ ");
    printf(ANSI_RESET);

    for(int vr=7;vr>=0;vr--) {
        int term_row = br + (7-vr);
        move_cursor(term_row, bc-1);
        printf(BG_BORDER FG_LABEL ANSI_BOLD " %d ", UI.flipped?8-vr:vr+1);
        printf(ANSI_RESET);

        for(int vf=0;vf<8;vf++) {
            int s = vis_to_sq(vr,vf);
            int is_light = (rank_of(s)+file_of(s))%2==1;

            /* Determine background */
            const char *bg;
            if(s==UI.cursor_sq)               bg=BG_CURSOR;
            else if(s==UI.selected_sq)         bg=BG_SEL;
            else if(is_valid_dest(s))          bg=BG_MOVE;
            else if(G.in_check && G.board[s]==(G.side==WHITE?WK:BK)) bg=BG_CHECK;
            else if(last_move_sq(s))           bg=BG_LAST;
            else                               bg=is_light?BG_LIGHT:BG_DARK;

            int p=G.board[s];
            int is_white_piece=IS_WHITE(p);
            const char *fg=(p&&is_white_piece)?FG_WHITE_PIECE:FG_BLACK_PIECE;
            int pt=PIECE_TYPE(p);

            printf("%s%s", bg, fg);
            if(p) printf(ANSI_BOLD "%s" ANSI_RESET, PIECE_UNICODE[is_white_piece][pt]);
            else  printf("    ");
            printf(ANSI_RESET);
        }

        /* Right border */
        printf(BG_BORDER FG_BORDER " ║");
        printf(ANSI_RESET);
    }

    /* File labels */
    move_cursor(br+8, bc-1);
    printf(BG_BORDER FG_LABEL ANSI_BOLD "   ");
    for(int vf=0;vf<8;vf++) {
        int f = UI.flipped ? 7-vf : vf;
        printf("  %c ", 'a'+f);
    }
    printf(" " ANSI_RESET);

    /* Bottom border */
    move_cursor(br+9, bc-1);
    printf(BG_BORDER FG_BORDER ANSI_BOLD);
    printf(" ╚════════════════════════════════╝ ");
    printf(ANSI_RESET);
}

static void draw_panel(void) {
    int pr=3, pc=42; /* panel column */
    int pw=36;       /* panel width */

    /* Title */
    move_cursor(1, pc);
    printf(BG_PANEL ANSI_BOLD FG_HIGHLIGHT);
    printf(" ♟  TERMINAL CHESS  ♟             ");
    printf(ANSI_RESET);

    move_cursor(2, pc);
    printf(BG_PANEL FG_PANEL ANSI_DIM);
    printf("─────────────────────────────────── ");
    printf(ANSI_RESET);

    /* Engine info */
    move_cursor(pr, pc);
    printf(BG_PANEL FG_INFO ANSI_BOLD " Engine: " ANSI_RESET BG_PANEL FG_PANEL);
    if(ENG.active) {
        char eng_name[24];
        strncpy(eng_name, ENG.name[0]?ENG.name:"Unknown", 23);
        printf("%-23s", eng_name);
        printf(ENG.thinking ? FG_HIGHLIGHT " ⟳ Thinking..." : FG_SUCCESS "  ✓ Ready    ");
    } else {
        printf("%-23s" FG_ERROR " ✗ No engine   ", "None");
    }
    printf(ANSI_RESET);

    /* Captured pieces / material */
    /* Side to move */
    move_cursor(pr+1, pc);
    printf(BG_PANEL FG_INFO ANSI_BOLD " Turn:   " ANSI_RESET BG_PANEL);
    if(G.game_over) {
        printf(FG_HIGHLIGHT ANSI_BOLD " %-27s", G.result_str);
    } else {
        printf(G.side==WHITE ? FG_WHITE_PIECE ANSI_BOLD " White ●" : FG_BLACK_PIECE ANSI_BOLD " Black ●");
        printf(G.in_check ? FG_ERROR ANSI_BOLD "   ⚠ CHECK!" : "             ");
    }
    printf(ANSI_RESET);

    move_cursor(pr+2, pc);
    printf(BG_PANEL FG_INFO ANSI_BOLD " Move:   " ANSI_RESET BG_PANEL FG_PANEL);
    printf(" %-27d", G.fullmove);
    printf(ANSI_RESET);

    move_cursor(pr+3, pc);
    printf(BG_PANEL FG_PANEL ANSI_DIM "─────────────────────────────────── " ANSI_RESET);

    /* PGN display */
    move_cursor(pr+4, pc);
    printf(BG_PANEL FG_HIGHLIGHT ANSI_BOLD " PGN Moves:                          " ANSI_RESET);

    /* Display last 12 moves worth of PGN */
    int pgn_row = pr+5;
    char pgn_copy[MAX_PGN];
    strncpy(pgn_copy, G.pgn, MAX_PGN-1);
    pgn_copy[MAX_PGN-1]='\0';

    /* Split into display lines of ~pw chars */
    char lines[12][40];
    int  lcount=0;
    char *tok=strtok(pgn_copy," ");
    char cur_line[40]=""; int cur_len=0;
    while(tok&&lcount<12){
        int tl=strlen(tok);
        if(cur_len+tl+1 >= 36) {
            strncpy(lines[lcount++],cur_line,39);
            cur_line[0]='\0'; cur_len=0;
        }
        if(cur_len>0){strcat(cur_line," ");cur_len++;}
        strcat(cur_line,tok); cur_len+=tl;
        tok=strtok(NULL," ");
    }
    if(cur_len>0&&lcount<12) strncpy(lines[lcount++],cur_line,39);

    /* Show last 8 lines */
    int start=lcount>8?lcount-8:0;
    for(int i=0;i<8;i++) {
        move_cursor(pgn_row+i, pc);
        printf(BG_PANEL FG_PANEL);
        if(start+i<lcount)
            printf(" %-35s", lines[start+i]);
        else
            printf(" %-35s", "");
        printf(ANSI_RESET);
    }

    /* Divider */
    move_cursor(pgn_row+8, pc);
    printf(BG_PANEL FG_PANEL ANSI_DIM "─────────────────────────────────── " ANSI_RESET);

    /* Controls */
    const char *controls[] = {
        " ↑↓←→  Move cursor",
        " ENTER  Select/Move piece",
        " U      Undo last move",
        " F      Flip board",
        " N      New game",
        " H      Help",
        " Q      Quit",
        NULL
    };
    int cr=pgn_row+9;
    move_cursor(cr-1,pc);
    printf(BG_PANEL FG_HIGHLIGHT ANSI_BOLD " Controls:                           " ANSI_RESET);
    for(int i=0;controls[i];i++){
        move_cursor(cr+i,pc);
        printf(BG_PANEL FG_PANEL " %-35s " ANSI_RESET, controls[i]);
    }

    /* Status message */
    move_cursor(pgn_row+16, pc);
    printf(BG_PANEL);
    switch(UI.status_color){
        case 1: printf(FG_ERROR);   break;
        case 2: printf(FG_SUCCESS); break;
        case 3: printf(FG_INFO);    break;
        default:printf(FG_PANEL);   break;
    }
    printf(" %-35s " ANSI_RESET, UI.status_msg);
}

static void draw_help_overlay(void) {
    int r=5, c=10;
    const char *lines[] = {
        "╔══════════════════════════════════════════╗",
        "║         TERMINAL CHESS  HELP             ║",
        "╠══════════════════════════════════════════╣",
        "║  MOVEMENT                                ║",
        "║  ↑ ↓ ← →    Move cursor on board        ║",
        "║                                          ║",
        "║  ACTIONS                                 ║",
        "║  ENTER/SPACE Select piece or make move   ║",
        "║  U            Undo last move (takeback)  ║",
        "║  ESC          Deselect piece             ║",
        "║  F            Flip board orientation     ║",
        "║  N            Start a new game           ║",
        "║                                          ║",
        "║  PROMOTION                               ║",
        "║  When a pawn promotes, press:            ║",
        "║  Q=Queen  R=Rook  B=Bishop  N=Knight     ║",
        "║                                          ║",
        "║  ENGINE                                  ║",
        "║  Run: ./chess_gui /path/to/engine        ║",
        "║  Engine plays Black by default           ║",
        "║                                          ║",
        "║  Press any key to close help...          ║",
        "╚══════════════════════════════════════════╝",
        NULL
    };
    for(int i=0;lines[i];i++){
        move_cursor(r+i,c);
        printf(BG_PANEL FG_HIGHLIGHT ANSI_BOLD "%s" ANSI_RESET, lines[i]);
    }
    fflush(stdout);
    /* Wait for keypress */
    char buf[4]; read(STDIN_FILENO,buf,sizeof(buf));
}

static void draw_promotion_prompt(void) {
    int r=14, c=10;
    move_cursor(r,c);
    printf(BG_PANEL FG_HIGHLIGHT ANSI_BOLD
           "╔══════════════════════════════╗" ANSI_RESET);
    move_cursor(r+1,c);
    printf(BG_PANEL FG_HIGHLIGHT ANSI_BOLD
           "║  PROMOTION - Choose piece:   ║" ANSI_RESET);
    move_cursor(r+2,c);
    printf(BG_PANEL FG_PANEL ANSI_BOLD
           "║  Q=♕ Queen   R=♖ Rook       ║" ANSI_RESET);
    move_cursor(r+3,c);
    printf(BG_PANEL FG_PANEL ANSI_BOLD
           "║  B=♗ Bishop  N=♘ Knight     ║" ANSI_RESET);
    move_cursor(r+4,c);
    printf(BG_PANEL FG_HIGHLIGHT ANSI_BOLD
           "╚══════════════════════════════╝" ANSI_RESET);
    fflush(stdout);
}

static void render(void) {
    move_cursor(1,1);
    /* Top title bar */
    printf(BG_BORDER FG_HIGHLIGHT ANSI_BOLD);
    printf(" Terminal Chess  │  %s  │  Move %d  │  %s  │  50-move: %d/100  ",
           G.side==WHITE?"White to move":"Black to move",
           G.fullmove,
           ENG.active?ENG.name:"No engine",
           G.halfmove);
    printf(ANSI_RESET);

    draw_board();
    draw_panel();

    if(UI.promotion_pending) draw_promotion_prompt();

    /* Hide terminal cursor */
    move_cursor(terminal_rows-1,1);
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Input Handling                                                         */
/* ═══════════════════════════════════════════════════════════════════════ */

static void set_status(const char *msg, int color) {
    strncpy(UI.status_msg, msg, 255);
    UI.status_color = color;
}

static void cursor_move(int dr, int df) {
    int vr=vrank(UI.cursor_sq)+dr;
    int vf=vfile(UI.cursor_sq)+df;
    if(vr<0) vr=0; if(vr>7) vr=7;
    if(vf<0) vf=0; if(vf>7) vf=7;
    UI.cursor_sq = vis_to_sq(vr,vf);
}

static void do_select_or_move(void) {
    if(UI.promotion_pending) return;
    if(G.game_over) {
        set_status(G.result_str, 2);
        return;
    }

    int cs=UI.cursor_sq;

    if(UI.selected_sq < 0) {
        /* Select a piece */
        int p=G.board[cs];
        if(!p||PIECE_COLOR(p)!=G.side) {
            set_status("No piece to select here.", 1);
            return;
        }
        if(ENG.active && ENG.engine_color==G.side) {
            set_status("Engine's turn to move.", 3);
            return;
        }
        UI.selected_sq = cs;
        UI.valid_count = get_valid_destinations(cs, UI.valid_moves);
        if(UI.valid_count==0) {
            set_status("No legal moves for this piece.", 1);
            UI.selected_sq=-1;
        } else {
            char msg[64];
            snprintf(msg,sizeof(msg),"Selected %c%d. Choose destination.",
                     'a'+file_of(cs), rank_of(cs)+1);
            set_status(msg,3);
        }
    } else {
        /* Try to move */
        if(cs==UI.selected_sq) {
            UI.selected_sq=-1;
            UI.valid_count=0;
            set_status("Deselected.",0);
            return;
        }
        if(!is_valid_dest(cs)) {
            /* Maybe select different piece */
            int p=G.board[cs];
            if(p&&PIECE_COLOR(p)==G.side) {
                UI.selected_sq=cs;
                UI.valid_count=get_valid_destinations(cs,UI.valid_moves);
                set_status("Switched selection.",3);
            } else {
                set_status("Invalid move. Try again.",1);
                UI.selected_sq=-1;
                UI.valid_count=0;
            }
            return;
        }

        /* Check if promotion needed */
        int p=G.board[UI.selected_sq];
        int type=PIECE_TYPE(p);
        int to_rank=rank_of(cs);
        if(type==PAWN&&(to_rank==7||to_rank==0)) {
            UI.promotion_pending='?';
            UI.prom_from=UI.selected_sq;
            UI.prom_to=cs;
            set_status("Choose promotion: Q R B N",3);
            return;
        }

        int ok=make_move(UI.selected_sq,cs,0);
        int from_backup=UI.selected_sq;
        UI.selected_sq=-1;
        UI.valid_count=0;

        if(ok) {
            Move *m=&G.history[G.hist_count-1];
            char msg[64];
            snprintf(msg,sizeof(msg),"Moved: %s",m->san);
            set_status(msg,2);
            if(G.game_over) set_status(G.result_str,2);
            /* Trigger engine if needed */
            if(ENG.active&&!G.game_over&&G.side==ENG.engine_color)
                engine_go(1500);
        } else {
            set_status("Illegal move.",1);
        }
    }
}

static void do_undo(void) {
    if(G.hist_count==0){set_status("Nothing to undo.",1);return;}
    /* If engine is playing, undo twice (engine move + player move) */
    int undo_count=(ENG.active&&G.hist_count>=2)?2:1;
    for(int i=0;i<undo_count&&G.hist_count>0;i++) undo_move();
    UI.selected_sq=-1;
    UI.valid_count=0;
    ENG.thinking=0;
    set_status("Move undone.",3);
}

static void do_new_game(void) {
    init_board();
    UI.cursor_sq=sq(0,4);
    UI.selected_sq=-1;
    UI.valid_count=0;
    UI.promotion_pending=0;
    ENG.thinking=0;
    set_status("New game started! White to move.",2);
    if(ENG.active) {
        engine_write("ucinewgame");
        engine_write("isready");
        /* Wait briefly */
        char buf[256]; engine_read_line(buf,sizeof(buf),500);
        /* Engine plays black by default */
        if(ENG.engine_color==BLACK) {
            /* White moves first, engine waits */
        }
    }
}

static void handle_promotion_key(char k) {
    int promo=0;
    switch(toupper(k)){
        case 'Q': promo=QUEEN;  break;
        case 'R': promo=ROOK;   break;
        case 'B': promo=BISHOP; break;
        case 'N': promo=KNIGHT; break;
        default:
            set_status("Press Q, R, B, or N for promotion.",1);
            return;
    }
    int ok=make_move(UI.prom_from,UI.prom_to,promo);
    UI.promotion_pending=0;
    UI.selected_sq=-1;
    UI.valid_count=0;
    if(ok){
        Move *m=&G.history[G.hist_count-1];
        char msg[64]; snprintf(msg,sizeof(msg),"Promoted: %s",m->san);
        set_status(msg,2);
        if(G.game_over) set_status(G.result_str,2);
        if(ENG.active&&!G.game_over&&G.side==ENG.engine_color)
            engine_go(1500);
    } else set_status("Promotion failed.",1);
}

/* Read keyboard; return 1 if should redraw */
static int handle_input(void) {
    unsigned char buf[8];
    int n=read(STDIN_FILENO,buf,sizeof(buf));
    if(n<=0) return 0;

    if(UI.promotion_pending) {
        handle_promotion_key(buf[0]);
        return 1;
    }

    /* Escape sequences for arrow keys */
    if(n>=3 && buf[0]=='\033' && buf[1]=='[') {
        switch(buf[2]) {
            case 'A': cursor_move( 1, 0); return 1; /* up    */
            case 'B': cursor_move(-1, 0); return 1; /* down  */
            case 'C': cursor_move( 0, 1); return 1; /* right */
            case 'D': cursor_move( 0,-1); return 1; /* left  */
        }
    }
    if(n==1) {
        char k=buf[0];
        switch(k) {
            case '\r': case '\n': case ' ':
                do_select_or_move(); return 1;
            case 'u': case 'U':
                do_undo(); return 1;
            case 'f': case 'F':
                UI.flipped=!UI.flipped;
                set_status(UI.flipped?"Board flipped (Black POV)":"Board flipped (White POV)",3);
                return 1;
            case 'n': case 'N':
                do_new_game(); return 1;
            case 'h': case 'H':
                draw_help_overlay(); return 1;
            case 'q': case 'Q':
                return -1; /* quit */
            case '\033': /* ESC */
                UI.selected_sq=-1; UI.valid_count=0;
                set_status("Deselected.",0); return 1;
            /* WASD alternative movement */
            case 'w': cursor_move( 1, 0); return 1;
            case 's': cursor_move(-1, 0); return 1;
            case 'd': cursor_move( 0, 1); return 1;
            case 'a': cursor_move( 0,-1); return 1;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Main                                                                   */
/* ═══════════════════════════════════════════════════════════════════════ */

static void cleanup(void) {
    stop_engine();
    disable_raw_mode();
    show_cursor();
    clear_screen();
    move_cursor(1,1);
    printf(ANSI_RESET "Thanks for playing Terminal Chess!\n\n");
    fflush(stdout);
}

static void sig_handler(int sig) {
    (void)sig;
    cleanup();
    exit(0);
}

int main(int argc, char *argv[]) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    get_terminal_size();
    enable_raw_mode();
    hide_cursor();
    clear_screen();

    /* Init game */
    memset(&G,   0, sizeof(G));
    memset(&UI,  0, sizeof(UI));
    memset(&ENG, 0, sizeof(ENG));

    init_board();
    UI.cursor_sq  = sq(1,4); /* e2 */
    UI.selected_sq = -1;
    strcpy(UI.status_msg, "Welcome! Arrow keys + Enter to play. H for help.");

    /* Start engine if provided */
    if(argc >= 2) {
        if(start_engine(argv[1])) {
            ENG.engine_color = BLACK;
            char msg[128];
            snprintf(msg,sizeof(msg),"Engine loaded: %s  (playing Black)",
                     ENG.name[0]?ENG.name:argv[1]);
            set_status(msg,2);
        } else {
            set_status("Failed to start engine. Human vs Human mode.",1);
        }
    } else {
        set_status("No engine. Human vs Human. Use H for help.",3);
    }

    render();

    /* Main event loop */
    while(1) {
        /* Poll engine for best move */
        if(ENG.active && ENG.thinking) {
            if(engine_poll_bestmove()) {
                engine_apply_bestmove();
                ENG.best_move[0]='\0';
                if(G.game_over) set_status(G.result_str,2);
                else {
                    char msg[64];
                    snprintf(msg,sizeof(msg),"Engine played: %s",
                             G.hist_count>0?G.history[G.hist_count-1].san:"?");
                    set_status(msg,3);
                }
                render();
                continue;
            }
        }

        /* Handle user input (with short timeout for engine polling) */
        fd_set fds;
        struct timeval tv = {0, 100000}; /* 100ms */
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int sel = select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);

        if(sel > 0) {
            int result = handle_input();
            if(result == -1) break; /* quit */
            if(result == 1)  render();
        } else if(sel == 0 && ENG.thinking) {
            /* Redraw to show "thinking" indicator */
            static int think_tick=0;
            if(++think_tick % 5 == 0) render();
        }
    }

    cleanup();
    return 0;
}
