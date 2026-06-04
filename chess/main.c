/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 * 
 * Compile: gcc -o chess_gui chess_gui.c
 * Run:     ./chess_gui [engine_path]
 *
 * Controls:
 *   Arrow Keys / HJKL - Move cursor
 *   Enter / Space      - Select piece / Confirm move
 *   U                  - Undo last move
 *   T                  - Change time controls
 *   Q                  - Quit
 *   R                  - Restart game
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

/* ─────────────────────────────────────────────
   CONSTANTS & TYPES
───────────────────────────────────────────── */

#define BOARD_SIZE    8
#define MAX_MOVES     512
#define MAX_PGN_LEN   8192
#define MAX_HIST      256
#define MAX_LEGAL     256
#define ENGINE_BUF    4096

/* Piece definitions (positive = white, negative = black) */
typedef enum {
    EMPTY  =  0,
    PAWN   =  1,
    KNIGHT =  2,
    BISHOP =  3,
    ROOK   =  4,
    QUEEN  =  5,
    KING   =  6,
} PieceType;

typedef struct {
    int piece;   /* positive=white, negative=black, 0=empty */
} Square;

typedef struct {
    int from_row, from_col;
    int to_row,   to_col;
    int promotion;         /* 0 = none, else PieceType */
    int captured_piece;
    int en_passant;        /* 1 if this move was en passant */
    int castling;          /* 0=none,1=kingside,2=queenside */
    /* State saved for undo */
    int prev_ep_col;       /* en passant target column before move (-1=none) */
    int prev_ep_row;
    int prev_castling_rights[2][2]; /* [color][side] */
    int prev_halfmove;
} Move;

typedef struct {
    Square board[8][8];
    int    white_to_move;
    int    castling_rights[2][2]; /* [0=white,1=black][0=kingside,1=queenside] */
    int    ep_col;    /* en passant target col (-1 = none) */
    int    ep_row;    /* en passant target row */
    int    halfmove_clock;
    int    fullmove_number;
    Move   history[MAX_HIST];
    int    hist_count;
} GameState;

typedef enum {
    TC_TIME  = 0,
    TC_DEPTH = 1,
    TC_NODES = 2,
} TCType;

typedef struct {
    TCType type;
    int    time_ms;   /* milliseconds for TC_TIME */
    int    depth;     /* for TC_DEPTH */
    long   nodes;     /* for TC_NODES */
} TimeControl;

/* ─────────────────────────────────────────────
   GLOBALS
───────────────────────────────────────────── */

static GameState  gs;
static int        cursor_row = 7, cursor_col = 0;
static int        selected_row = -1, selected_col = -1;
static int        legal_count = 0;
static Move       legal_moves[MAX_LEGAL];
static int        last_from_row = -1, last_from_col = -1;
static int        last_to_row   = -1, last_to_col   = -1;
static int        player_color  = 1;  /* 1=white, -1=black, 0=both */
static char       pgn_buf[MAX_PGN_LEN];
static int        pgn_len = 0;
static int        game_over = 0;
static char       game_over_msg[128];

/* Engine process */
static pid_t  engine_pid  = -1;
static int    engine_in   = -1;   /* write to engine */
static int    engine_out  = -1;   /* read from engine */
static char   engine_path[512] = "";
static int    engine_ready = 0;
static int    engine_thinking = 0;
static char   engine_name[128] = "No Engine";

/* Time control */
static TimeControl tc = { TC_TIME, 1000, 5, 100000 };

/* Terminal state */
static struct termios orig_termios;
static int            term_restored = 0;

/* Color scheme */
#define C_RESET      "\033[0m"
#define C_BOLD       "\033[1m"
#define C_DIM        "\033[2m"

/* True-color backgrounds */
#define BG_LIGHT     "\033[48;2;240;217;181m"   /* light square  */
#define BG_DARK      "\033[48;2;181;136;99m"    /* dark square   */
#define BG_CURSOR    "\033[48;2;100;180;255m"   /* cursor        */
#define BG_SELECTED  "\033[48;2;50;200;50m"     /* selected      */
#define BG_LEGAL     "\033[48;2;180;230;120m"   /* legal move    */
#define BG_LAST_FROM "\033[48;2;220;200;80m"    /* last from     */
#define BG_LAST_TO   "\033[48;2;245;222;50m"    /* last to       */
#define BG_CHECK     "\033[48;2;220;60;60m"     /* king in check */
#define BG_CAPTURE   "\033[48;2;255;140;80m"    /* legal capture */

#define FG_WHITE_PC  "\033[38;2;255;255;255m"
#define FG_BLACK_PC  "\033[38;2;20;20;20m"
#define FG_COORDS    "\033[38;2;180;180;180m"

/* Unicode chess pieces */
static const char *piece_unicode[2][7] = {
    /* index: 0=empty,1=P,2=N,3=B,4=R,5=Q,6=K */
    { " ", "♙", "♘", "♗", "♖", "♕", "♔" },  /* white */
    { " ", "♟", "♞", "♝", "♜", "♛", "♚" },  /* black */
};

/* ─────────────────────────────────────────────
   TERMINAL UTILS
───────────────────────────────────────────── */

static void restore_terminal(void) {
    if (!term_restored) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\033[?25h");  /* show cursor */
        fflush(stdout);
        term_restored = 1;
    }
}

static void setup_terminal(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=  (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l");  /* hide cursor */
    fflush(stdout);
}

static void clear_screen(void) {
    printf("\033[2J\033[H");
}

static void move_cursor_to(int row, int col) {
    printf("\033[%d;%dH", row + 1, col + 1);
}

/* Read a key (handles escape sequences for arrow keys) */
static int read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;
    if (c == '\033') {
        unsigned char seq[4] = {0};
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';
            switch (seq[1]) {
                case 'A': return 1000; /* up    */
                case 'B': return 1001; /* down  */
                case 'C': return 1002; /* right */
                case 'D': return 1003; /* left  */
            }
        }
        return '\033';
    }
    return (int)c;
}

/* ─────────────────────────────────────────────
   CHESS LOGIC
───────────────────────────────────────────── */

static void init_board(void) {
    memset(&gs, 0, sizeof(gs));
    gs.white_to_move = 1;
    gs.ep_col = -1;
    gs.ep_row = -1;
    gs.fullmove_number = 1;
    gs.castling_rights[0][0] = 1;
    gs.castling_rights[0][1] = 1;
    gs.castling_rights[1][0] = 1;
    gs.castling_rights[1][1] = 1;

    /* White pieces (row 0 = rank 1 in our mapping: row 7=rank8 for display) */
    /* We use row 0 = rank 1 (white back rank), row 7 = rank 8 (black back rank) */
    int back_pieces[] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
    for (int c = 0; c < 8; c++) {
        gs.board[0][c].piece =  back_pieces[c]; /* white back rank */
        gs.board[1][c].piece =  PAWN;           /* white pawns */
        gs.board[6][c].piece = -PAWN;           /* black pawns */
        gs.board[7][c].piece = -back_pieces[c]; /* black back rank */
    }

    pgn_buf[0] = '\0';
    pgn_len = 0;
    game_over = 0;
    game_over_msg[0] = '\0';
    last_from_row = last_from_col = last_to_row = last_to_col = -1;
}

static int piece_color(int p) {
    if (p > 0) return 1;
    if (p < 0) return -1;
    return 0;
}

static int on_board(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

/* Check if square (r,c) is attacked by 'attacker_color' */
static int is_attacked(const Square board[8][8], int r, int c, int attacker_color) {
    int ac = attacker_color;

    /* Pawns */
    int pawn_dir = (ac == 1) ? -1 : 1;  /* white pawns attack upward (row+1 in our scheme)? */
    /* White is row 0 (rank1). Pawns move increasing row. White pawn at row r attacks r+1. */
    /* Actually let's clarify: white pawns at row r attack squares (r+1, c±1) */
    /* An attacker white pawn at (r - pawn_dir, c±1) would attack (r,c) */
    /* For white attacker: pawn_dir=+1, pawn would be at r-1 */
    int pawn_from_row = r - (ac == 1 ? 1 : -1);
    if (on_board(pawn_from_row, c - 1)) {
        int p = board[pawn_from_row][c-1].piece;
        if (p == ac * PAWN) return 1;
    }
    if (on_board(pawn_from_row, c + 1)) {
        int p = board[pawn_from_row][c+1].piece;
        if (p == ac * PAWN) return 1;
    }

    /* Knights */
    int kn_dr[] = {-2,-2,-1,-1,1,1,2,2};
    int kn_dc[] = {-1,1,-2,2,-2,2,-1,1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_dr[i], nc = c + kn_dc[i];
        if (on_board(nr, nc) && board[nr][nc].piece == ac * KNIGHT) return 1;
    }

    /* Bishops / Queens (diagonals) */
    int diag_dr[] = {-1,-1,1,1};
    int diag_dc[] = {-1,1,-1,1};
    for (int d = 0; d < 4; d++) {
        int nr = r + diag_dr[d], nc = c + diag_dc[d];
        while (on_board(nr, nc)) {
            int p = board[nr][nc].piece;
            if (p != 0) {
                if (p == ac * BISHOP || p == ac * QUEEN) return 1;
                break;
            }
            nr += diag_dr[d]; nc += diag_dc[d];
        }
    }

    /* Rooks / Queens (straights) */
    int str_dr[] = {-1,1,0,0};
    int str_dc[] = {0,0,-1,1};
    for (int d = 0; d < 4; d++) {
        int nr = r + str_dr[d], nc = c + str_dc[d];
        while (on_board(nr, nc)) {
            int p = board[nr][nc].piece;
            if (p != 0) {
                if (p == ac * ROOK || p == ac * QUEEN) return 1;
                break;
            }
            nr += str_dr[d]; nc += str_dc[d];
        }
    }

    /* King */
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (on_board(nr, nc) && board[nr][nc].piece == ac * KING) return 1;
        }
    }

    return 0;
}

static int find_king(const Square board[8][8], int color) {
    /* returns row*8+col or -1 */
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (board[r][c].piece == color * KING)
                return r * 8 + c;
    return -1;
}

static int in_check(const Square board[8][8], int color) {
    int k = find_king(board, color);
    if (k < 0) return 0;
    return is_attacked(board, k / 8, k % 8, -color);
}

/* Apply a move to a board copy, returns 1 if legal (king not in check after) */
static int apply_move(Square board[8][8], const Move *m, int color) {
    int fr = m->from_row, fc = m->from_col;
    int tr = m->to_row,   tc = m->to_col;
    int piece = board[fr][fc].piece;

    board[tr][tc].piece = piece;
    board[fr][fc].piece = EMPTY;

    /* En passant capture */
    if (m->en_passant) {
        int cap_row = tr - (color == 1 ? 1 : -1);
        board[cap_row][tc].piece = EMPTY;
    }

    /* Castling rook move */
    if (m->castling == 1) { /* kingside */
        int rook_from_col = 7, rook_to_col = 5;
        board[fr][rook_to_col].piece = board[fr][rook_from_col].piece;
        board[fr][rook_from_col].piece = EMPTY;
    } else if (m->castling == 2) { /* queenside */
        int rook_from_col = 0, rook_to_col = 3;
        board[fr][rook_to_col].piece = board[fr][rook_from_col].piece;
        board[fr][rook_from_col].piece = EMPTY;
    }

    /* Promotion */
    if (m->promotion) {
        board[tr][tc].piece = color * m->promotion;
    }

    return !in_check(board, color);
}

/* Generate pseudo-legal then filter for legality */
static int generate_legal_moves_for(Move *out, int max_out,
                                     const GameState *g, int color) {
    int count = 0;
    int dir = (color == 1) ? 1 : -1;  /* pawn move direction */
    int pawn_start_row = (color == 1) ? 1 : 6;
    int pawn_promo_row = (color == 1) ? 7 : 0;

    for (int fr = 0; fr < 8 && count < max_out; fr++) {
        for (int fc = 0; fc < 8 && count < max_out; fc++) {
            int piece = g->board[fr][fc].piece;
            if (piece_color(piece) != color) continue;
            int pt = abs(piece);

            /* Helper lambda-equivalent: try adding move */
#define TRY_MOVE(TR, TC, PROMO, EP, CASTLE) \
            do { \
                Move _m; \
                memset(&_m, 0, sizeof(_m)); \
                _m.from_row = fr; _m.from_col = fc; \
                _m.to_row = (TR); _m.to_col = (TC); \
                _m.promotion = (PROMO); \
                _m.en_passant = (EP); \
                _m.castling = (CASTLE); \
                _m.captured_piece = g->board[(TR)][(TC)].piece; \
                Square tmp[8][8]; \
                memcpy(tmp, g->board, sizeof(tmp)); \
                if (apply_move(tmp, &_m, color)) { \
                    if (count < max_out) out[count++] = _m; \
                } \
            } while(0)

            if (pt == PAWN) {
                /* Forward one */
                int nr = fr + dir;
                if (on_board(nr, fc) && g->board[nr][fc].piece == EMPTY) {
                    if (nr == pawn_promo_row) {
                        TRY_MOVE(nr, fc, QUEEN,  0, 0);
                        TRY_MOVE(nr, fc, ROOK,   0, 0);
                        TRY_MOVE(nr, fc, BISHOP, 0, 0);
                        TRY_MOVE(nr, fc, KNIGHT, 0, 0);
                    } else {
                        TRY_MOVE(nr, fc, 0, 0, 0);
                    }
                    /* Forward two from start */
                    if (fr == pawn_start_row) {
                        int nr2 = fr + 2 * dir;
                        if (g->board[nr2][fc].piece == EMPTY) {
                            TRY_MOVE(nr2, fc, 0, 0, 0);
                        }
                    }
                }
                /* Captures */
                for (int dc = -1; dc <= 1; dc += 2) {
                    int nc = fc + dc;
                    if (!on_board(nr, nc)) continue;
                    int target = g->board[nr][nc].piece;
                    int is_ep = (g->ep_col == nc && g->ep_row == nr);
                    if (piece_color(target) == -color || is_ep) {
                        if (nr == pawn_promo_row) {
                            TRY_MOVE(nr, nc, QUEEN,  0, 0);
                            TRY_MOVE(nr, nc, ROOK,   0, 0);
                            TRY_MOVE(nr, nc, BISHOP, 0, 0);
                            TRY_MOVE(nr, nc, KNIGHT, 0, 0);
                        } else {
                            TRY_MOVE(nr, nc, 0, is_ep, 0);
                        }
                    }
                }
            } else if (pt == KNIGHT) {
                int kn_dr[] = {-2,-2,-1,-1,1,1,2,2};
                int kn_dc[] = {-1,1,-2,2,-2,2,-1,1};
                for (int i = 0; i < 8; i++) {
                    int nr = fr + kn_dr[i], nc = fc + kn_dc[i];
                    if (!on_board(nr, nc)) continue;
                    if (piece_color(g->board[nr][nc].piece) != color)
                        TRY_MOVE(nr, nc, 0, 0, 0);
                }
            } else if (pt == BISHOP || pt == QUEEN) {
                int diag_dr[] = {-1,-1,1,1};
                int diag_dc[] = {-1,1,-1,1};
                for (int d = 0; d < 4; d++) {
                    int nr = fr + diag_dr[d], nc = fc + diag_dc[d];
                    while (on_board(nr, nc)) {
                        int pc = piece_color(g->board[nr][nc].piece);
                        if (pc == color) break;
                        TRY_MOVE(nr, nc, 0, 0, 0);
                        if (pc == -color) break;
                        nr += diag_dr[d]; nc += diag_dc[d];
                    }
                }
                if (pt == BISHOP) goto next_piece;
            }
            if (pt == ROOK || pt == QUEEN) {
                int str_dr[] = {-1,1,0,0};
                int str_dc[] = {0,0,-1,1};
                for (int d = 0; d < 4; d++) {
                    int nr = fr + str_dr[d], nc = fc + str_dc[d];
                    while (on_board(nr, nc)) {
                        int pc = piece_color(g->board[nr][nc].piece);
                        if (pc == color) break;
                        TRY_MOVE(nr, nc, 0, 0, 0);
                        if (pc == -color) break;
                        nr += str_dr[d]; nc += str_dc[d];
                    }
                }
            }
            if (pt == KING) {
                for (int dr = -1; dr <= 1; dr++) {
                    for (int dc = -1; dc <= 1; dc++) {
                        if (dr == 0 && dc == 0) continue;
                        int nr = fr + dr, nc = fc + dc;
                        if (!on_board(nr, nc)) continue;
                        if (piece_color(g->board[nr][nc].piece) != color)
                            TRY_MOVE(nr, nc, 0, 0, 0);
                    }
                }
                /* Castling */
                int ci = (color == 1) ? 0 : 1;
                int king_row = (color == 1) ? 0 : 7;
                if (fr == king_row && fc == 4 && !in_check(g->board, color)) {
                    /* Kingside */
                    if (g->castling_rights[ci][0] &&
                        g->board[king_row][5].piece == EMPTY &&
                        g->board[king_row][6].piece == EMPTY &&
                        !is_attacked(g->board, king_row, 5, -color) &&
                        !is_attacked(g->board, king_row, 6, -color)) {
                        TRY_MOVE(king_row, 6, 0, 0, 1);
                    }
                    /* Queenside */
                    if (g->castling_rights[ci][1] &&
                        g->board[king_row][3].piece == EMPTY &&
                        g->board[king_row][2].piece == EMPTY &&
                        g->board[king_row][1].piece == EMPTY &&
                        !is_attacked(g->board, king_row, 3, -color) &&
                        !is_attacked(g->board, king_row, 2, -color)) {
                        TRY_MOVE(king_row, 2, 0, 0, 2);
                    }
                }
            }
            next_piece:;
#undef TRY_MOVE
        }
    }
    return count;
}

static int generate_legal_moves(Move *out, int max_out, const GameState *g) {
    int color = g->white_to_move ? 1 : -1;
    return generate_legal_moves_for(out, max_out, g, color);
}

/* Convert row/col to algebraic (e.g., row=0,col=0 -> "a1") */
static void sq_to_alg(int row, int col, char *out) {
    out[0] = 'a' + col;
    out[1] = '1' + row;
    out[2] = '\0';
}

static char promo_char(int p) {
    switch (p) {
        case QUEEN:  return 'q';
        case ROOK:   return 'r';
        case BISHOP: return 'b';
        case KNIGHT: return 'n';
    }
    return '?';
}

/* Move to UCI string */
static void move_to_uci(const Move *m, char *out) {
    sq_to_alg(m->from_row, m->from_col, out);
    sq_to_alg(m->to_row,   m->to_col,   out + 2);
    if (m->promotion) {
        out[4] = promo_char(m->promotion);
        out[5] = '\0';
    } else {
        out[4] = '\0';
    }
}

/* Build PGN notation for a move (before it's applied) */
static void build_pgn_move(const GameState *g, const Move *m, char *out) {
    int color  = g->white_to_move ? 1 : -1;
    int pt     = abs(g->board[m->from_row][m->from_col].piece);
    char to_s[3], from_s[3];
    sq_to_alg(m->to_row,   m->to_col,   to_s);
    sq_to_alg(m->from_row, m->from_col, from_s);

    char buf[32] = {0};
    int  pos = 0;

    if (m->castling == 1) { strcpy(buf, "O-O");   goto suffix; }
    if (m->castling == 2) { strcpy(buf, "O-O-O"); goto suffix; }

    /* Piece letter */
    const char *piece_letters = " PNBRQK";
    if (pt != PAWN) buf[pos++] = piece_letters[pt];

    /* Disambiguation */
    if (pt != PAWN) {
        int ambig_file = 0, ambig_rank = 0, ambig = 0;
        Move all[MAX_LEGAL]; int cnt;
        cnt = generate_legal_moves_for(all, MAX_LEGAL, g, color);
        for (int i = 0; i < cnt; i++) {
            if (i == 0 && all[i].from_row == m->from_row && all[i].from_col == m->from_col)
                continue;
            if (abs(g->board[all[i].from_row][all[i].from_col].piece) != pt) continue;
            if (all[i].to_row != m->to_row || all[i].to_col != m->to_col)   continue;
            if (all[i].from_row == m->from_row && all[i].from_col == m->from_col) continue;
            ambig++;
            if (all[i].from_col == m->from_col) ambig_rank++;
            if (all[i].from_row == m->from_row) ambig_file++;
        }
        if (ambig > 0) {
            if (!ambig_rank) buf[pos++] = 'a' + m->from_col;
            else if (!ambig_file) buf[pos++] = '1' + m->from_row;
            else { buf[pos++] = 'a' + m->from_col; buf[pos++] = '1' + m->from_row; }
        }
    }

    /* Pawn file on capture */
    int is_capture = (m->captured_piece != 0) || m->en_passant;
    if (pt == PAWN && is_capture) buf[pos++] = 'a' + m->from_col;

    /* Capture */
    if (is_capture) buf[pos++] = 'x';

    /* Destination */
    buf[pos++] = to_s[0];
    buf[pos++] = to_s[1];

    /* Promotion */
    if (m->promotion) {
        buf[pos++] = '=';
        buf[pos++] = toupper(promo_char(m->promotion));
    }

    buf[pos] = '\0';

    suffix:;
    /* Check / checkmate detection */
    /* Apply move and see if opponent is in check */
    Square tmp[8][8];
    memcpy(tmp, g->board, sizeof(tmp));
    Move mc = *m;
    apply_move(tmp, &mc, color);

    /* Generate moves for opponent to detect checkmate */
    GameState tmp_gs = *g;
    memcpy(tmp_gs.board, tmp, sizeof(tmp));
    tmp_gs.white_to_move = !g->white_to_move;
    tmp_gs.ep_col = -1;
    int opp_moves = generate_legal_moves(legal_moves, MAX_LEGAL, &tmp_gs);
    int check = in_check(tmp, -color);
    if (check && opp_moves == 0) strcat(buf, "#");
    else if (check)              strcat(buf, "+");

    strcpy(out, buf);
}

/* Execute a move on the game state */
static void do_move(GameState *g, Move *m) {
    int color  = g->white_to_move ? 1 : -1;
    int ci     = (color == 1) ? 0 : 1;

    /* Save state for undo */
    m->prev_ep_col = g->ep_col;
    m->prev_ep_row = g->ep_row;
    memcpy(m->prev_castling_rights, g->castling_rights, sizeof(g->castling_rights));
    m->prev_halfmove = g->halfmove_clock;
    m->captured_piece = g->board[m->to_row][m->to_col].piece;

    /* PGN before applying */
    char pgn_move[32];
    build_pgn_move(g, m, pgn_move);

    /* Apply */
    Square tmp[8][8];
    memcpy(tmp, g->board, sizeof(tmp));
    apply_move(tmp, m, color);
    memcpy(g->board, tmp, sizeof(tmp));

    /* Update en passant */
    g->ep_col = -1; g->ep_row = -1;
    int pt = abs(g->board[m->to_row][m->to_col].piece);
    if (pt == PAWN && abs(m->to_row - m->from_row) == 2) {
        g->ep_col = m->from_col;
        g->ep_row = (m->from_row + m->to_row) / 2;
    }

    /* Update castling rights */
    int king_row = (color == 1) ? 0 : 7;
    if (pt == KING) {
        g->castling_rights[ci][0] = 0;
        g->castling_rights[ci][1] = 0;
    }
    if (m->from_row == 0 && m->from_col == 0) g->castling_rights[0][1] = 0;
    if (m->from_row == 0 && m->from_col == 7) g->castling_rights[0][0] = 0;
    if (m->from_row == 7 && m->from_col == 0) g->castling_rights[1][1] = 0;
    if (m->from_row == 7 && m->from_col == 7) g->castling_rights[1][0] = 0;
    if (m->to_row   == 0 && m->to_col   == 0) g->castling_rights[0][1] = 0;
    if (m->to_row   == 0 && m->to_col   == 7) g->castling_rights[0][0] = 0;
    if (m->to_row   == 7 && m->to_col   == 0) g->castling_rights[1][1] = 0;
    if (m->to_row   == 7 && m->to_col   == 7) g->castling_rights[1][0] = 0;

    /* Halfmove clock */
    if (pt == PAWN || m->captured_piece != 0 || m->en_passant)
        g->halfmove_clock = 0;
    else
        g->halfmove_clock++;

    /* Fullmove */
    if (!g->white_to_move) g->fullmove_number++;
    g->white_to_move = !g->white_to_move;

    /* Store in history */
    if (g->hist_count < MAX_HIST)
        g->history[g->hist_count++] = *m;

    /* Update last move display */
    last_from_row = m->from_row; last_from_col = m->from_col;
    last_to_row   = m->to_row;   last_to_col   = m->to_col;

    /* Append PGN */
    if (color == 1) {
        char num_str[16];
        snprintf(num_str, sizeof(num_str), "%d. ", g->fullmove_number - 1);
        int l = strlen(num_str);
        if (pgn_len + l < MAX_PGN_LEN - 64) {
            strcat(pgn_buf, num_str);
            pgn_len += l;
        }
    }
    int ml = strlen(pgn_move);
    if (pgn_len + ml + 2 < MAX_PGN_LEN) {
        strcat(pgn_buf, pgn_move);
        strcat(pgn_buf, " ");
        pgn_len += ml + 1;
    }
}

/* Undo last move */
static int undo_move(GameState *g) {
    if (g->hist_count == 0) return 0;
    Move *m = &g->history[--g->hist_count];
    int color = g->white_to_move ? -1 : 1; /* who just moved */

    /* Reverse move */
    int piece = g->board[m->to_row][m->to_col].piece;
    if (m->promotion) piece = color * PAWN;

    g->board[m->from_row][m->from_col].piece = piece;
    g->board[m->to_row][m->to_col].piece = m->captured_piece;

    /* En passant: restore captured pawn */
    if (m->en_passant) {
        int cap_row = m->to_row - (color == 1 ? 1 : -1);
        g->board[cap_row][m->to_col].piece = -color * PAWN;
        g->board[m->to_row][m->to_col].piece = EMPTY;
    }

    /* Castling: restore rook */
    if (m->castling == 1) {
        g->board[m->from_row][7].piece = color * ROOK;
        g->board[m->from_row][5].piece = EMPTY;
    } else if (m->castling == 2) {
        g->board[m->from_row][0].piece = color * ROOK;
        g->board[m->from_row][3].piece = EMPTY;
    }

    /* Restore state */
    g->ep_col  = m->prev_ep_col;
    g->ep_row  = m->prev_ep_row;
    memcpy(g->castling_rights, m->prev_castling_rights, sizeof(g->castling_rights));
    g->halfmove_clock = m->prev_halfmove;
    g->white_to_move  = (color == 1);
    if (color == -1) g->fullmove_number--;

    /* Rebuild PGN from history */
    pgn_buf[0] = '\0'; pgn_len = 0;
    GameState tmp = gs; /* not ideal but works for PGN rebuild */
    /* Actually rebuild from scratch using a temp game state */
    /* Save history then reinit */
    Move saved_hist[MAX_HIST];
    int  saved_cnt = g->hist_count;
    memcpy(saved_hist, g->history, sizeof(Move) * saved_cnt);

    /* Re-init board */
    Square save_board[8][8];
    memcpy(save_board, g->board, sizeof(g->board));

    init_board();
    memcpy(g->board, save_board, sizeof(g->board));
    g->ep_col  = m->prev_ep_col;
    g->ep_row  = m->prev_ep_row;
    memcpy(g->castling_rights, m->prev_castling_rights, sizeof(g->castling_rights));
    g->halfmove_clock = m->prev_halfmove;
    g->white_to_move  = (color == 1);
    if (color == -1 && g->fullmove_number > 1) g->fullmove_number--;
    memcpy(g->history, saved_hist, sizeof(Move) * saved_cnt);
    g->hist_count = saved_cnt;

    /* Rebuild PGN properly */
    pgn_buf[0] = '\0'; pgn_len = 0;
    /* Re-play moves through a shadow state for PGN */
    GameState shadow;
    init_board();
    /* Overwrite gs with reconstructed then replay for PGN */
    /* This is tricky; let's just rebuild PGN from stored history */
    /* We'll use a simpler approach: replay all saved moves from initial pos */
    GameState replay;
    memset(&replay, 0, sizeof(replay));
    init_board(); /* resets gs */
    /* init_board overwrites gs, so copy back */
    {
        GameState fresh;
        memset(&fresh, 0, sizeof(fresh));
        fresh.white_to_move = 1;
        fresh.ep_col = fresh.ep_row = -1;
        fresh.fullmove_number = 1;
        fresh.castling_rights[0][0] = fresh.castling_rights[0][1] = 1;
        fresh.castling_rights[1][0] = fresh.castling_rights[1][1] = 1;
        int bp[] = {ROOK,KNIGHT,BISHOP,QUEEN,KING,BISHOP,KNIGHT,ROOK};
        for (int cc = 0; cc < 8; cc++) {
            fresh.board[0][cc].piece =  bp[cc];
            fresh.board[1][cc].piece =  PAWN;
            fresh.board[6][cc].piece = -PAWN;
            fresh.board[7][cc].piece = -bp[cc];
        }
        /* Replay */
        for (int i = 0; i < saved_cnt; i++) {
            char pgn_m[32];
            build_pgn_move(&fresh, &saved_hist[i], pgn_m);
            int fc2 = fresh.white_to_move ? 1 : -1;
            /* Append PGN */
            if (fc2 == 1) {
                char ns[16];
                snprintf(ns, sizeof(ns), "%d. ", fresh.fullmove_number);
                int l = strlen(ns);
                if (pgn_len + l < MAX_PGN_LEN - 64) { strcat(pgn_buf, ns); pgn_len += l; }
            }
            int ml = strlen(pgn_m);
            if (pgn_len + ml + 2 < MAX_PGN_LEN) {
                strcat(pgn_buf, pgn_m); strcat(pgn_buf, " ");
                pgn_len += ml + 1;
            }
            /* Actually apply move to fresh */
            Square ftmp[8][8];
            memcpy(ftmp, fresh.board, sizeof(ftmp));
            apply_move(ftmp, &saved_hist[i], fc2);
            memcpy(fresh.board, ftmp, sizeof(fresh.board));
            /* Update ep, castling, etc. */
            fresh.ep_col = -1; fresh.ep_row = -1;
            int fpt = abs(fresh.board[saved_hist[i].to_row][saved_hist[i].to_col].piece);
            if (fpt == PAWN && abs(saved_hist[i].to_row - saved_hist[i].from_row) == 2) {
                fresh.ep_col = saved_hist[i].from_col;
                fresh.ep_row = (saved_hist[i].from_row + saved_hist[i].to_row) / 2;
            }
            int fci = (fc2 == 1) ? 0 : 1;
            if (fpt == KING) { fresh.castling_rights[fci][0] = 0; fresh.castling_rights[fci][1] = 0; }
            if (saved_hist[i].from_row==0&&saved_hist[i].from_col==0) fresh.castling_rights[0][1]=0;
            if (saved_hist[i].from_row==0&&saved_hist[i].from_col==7) fresh.castling_rights[0][0]=0;
            if (saved_hist[i].from_row==7&&saved_hist[i].from_col==0) fresh.castling_rights[1][1]=0;
            if (saved_hist[i].from_row==7&&saved_hist[i].from_col==7) fresh.castling_rights[1][0]=0;
            if (!fresh.white_to_move) fresh.fullmove_number++;
            fresh.white_to_move = !fresh.white_to_move;
        }
    }
    /* Restore gs to the actual undo'd state */
    memcpy(g->board, save_board, sizeof(g->board));
    g->ep_col = m->prev_ep_col; g->ep_row = m->prev_ep_row;
    memcpy(g->castling_rights, m->prev_castling_rights, sizeof(g->castling_rights));
    g->halfmove_clock = m->prev_halfmove;
    g->white_to_move  = (color == 1);
    if (color == -1) g->fullmove_number--;
    g->hist_count = saved_cnt;
    memcpy(g->history, saved_hist, sizeof(Move) * saved_cnt);

    /* Update last move highlight */
    if (g->hist_count > 0) {
        Move *lm = &g->history[g->hist_count - 1];
        last_from_row = lm->from_row; last_from_col = lm->from_col;
        last_to_row   = lm->to_row;   last_to_col   = lm->to_col;
    } else {
        last_from_row = last_from_col = last_to_row = last_to_col = -1;
    }

    game_over = 0;
    game_over_msg[0] = '\0';
    return 1;
}

/* FEN generation for UCI */
static void generate_fen(const GameState *g, char *fen) {
    int pos = 0;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            int p = g->board[r][c].piece;
            if (p == 0) { empty++; continue; }
            if (empty) { fen[pos++] = '0' + empty; empty = 0; }
            const char *wpc = " PNBRQK";
            char ch = wpc[abs(p)];
            fen[pos++] = (p > 0) ? ch : tolower(ch);
        }
        if (empty) fen[pos++] = '0' + empty;
        if (r > 0) fen[pos++] = '/';
    }
    fen[pos++] = ' ';
    fen[pos++] = g->white_to_move ? 'w' : 'b';
    fen[pos++] = ' ';
    int any_castle = 0;
    if (g->castling_rights[0][0]) { fen[pos++] = 'K'; any_castle++; }
    if (g->castling_rights[0][1]) { fen[pos++] = 'Q'; any_castle++; }
    if (g->castling_rights[1][0]) { fen[pos++] = 'k'; any_castle++; }
    if (g->castling_rights[1][1]) { fen[pos++] = 'q'; any_castle++; }
    if (!any_castle) fen[pos++] = '-';
    fen[pos++] = ' ';
    if (g->ep_col >= 0) {
        fen[pos++] = 'a' + g->ep_col;
        fen[pos++] = '1' + g->ep_row;
    } else {
        fen[pos++] = '-';
    }
    pos += snprintf(fen + pos, 32, " %d %d", g->halfmove_clock, g->fullmove_number);
    fen[pos] = '\0';
}

/* ─────────────────────────────────────────────
   ENGINE COMMUNICATION
───────────────────────────────────────────── */

static void engine_write(const char *msg) {
    if (engine_in < 0) return;
    write(engine_in, msg, strlen(msg));
}

static int engine_read_line(char *buf, int maxlen, int timeout_ms) {
    if (engine_out < 0) return 0;
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(engine_out, &fds);
    int ret = select(engine_out + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return 0;
    int pos = 0;
    while (pos < maxlen - 1) {
        char c;
        int r = read(engine_out, &c, 1);
        if (r <= 0) break;
        if (c == '\n') break;
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static int start_engine(const char *path) {
    int to_engine[2], from_engine[2];
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return 0;

    engine_pid = fork();
    if (engine_pid < 0) return 0;
    if (engine_pid == 0) {
        /* Child */
        dup2(to_engine[0],   STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]); close(from_engine[0]);
        close(to_engine[0]); close(from_engine[1]);
        execlp(path, path, NULL);
        _exit(1);
    }
    close(to_engine[0]);
    close(from_engine[1]);
    engine_in  = to_engine[1];
    engine_out = from_engine[0];

    /* Set non-blocking */
    int flags = fcntl(engine_out, F_GETFL, 0);
    fcntl(engine_out, F_SETFL, flags | O_NONBLOCK);

    /* UCI handshake */
    engine_write("uci\n");
    char buf[ENGINE_BUF];
    int tries = 0;
    while (tries++ < 100) {
        int n = engine_read_line(buf, sizeof(buf), 50);
        if (n > 0) {
            if (strncmp(buf, "id name", 7) == 0) {
                strncpy(engine_name, buf + 8, 127);
                engine_name[127] = '\0';
            }
            if (strcmp(buf, "uciok") == 0) break;
        }
    }
    engine_write("isready\n");
    tries = 0;
    while (tries++ < 100) {
        int n = engine_read_line(buf, sizeof(buf), 50);
        if (n > 0 && strcmp(buf, "readyok") == 0) {
            engine_ready = 1;
            break;
        }
    }
    engine_write("ucinewgame\n");
    return engine_ready;
}

static void stop_engine(void) {
    if (engine_in >= 0) {
        engine_write("quit\n");
        close(engine_in);
        engine_in = -1;
    }
    if (engine_out >= 0) {
        close(engine_out);
        engine_out = -1;
    }
    engine_pid = -1;
    engine_ready = 0;
}

/* Send position and go command */
static void engine_go(void) {
    if (!engine_ready || engine_in < 0) return;

    char fen[128];
    generate_fen(&gs, fen);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "position fen %s\n", fen);
    engine_write(cmd);

    if (tc.type == TC_TIME) {
        snprintf(cmd, sizeof(cmd), "go movetime %d\n", tc.time_ms);
    } else if (tc.type == TC_DEPTH) {
        snprintf(cmd, sizeof(cmd), "go depth %d\n", tc.depth);
    } else {
        snprintf(cmd, sizeof(cmd), "go nodes %ld\n", tc.nodes);
    }
    engine_write(cmd);
    engine_thinking = 1;
}

/* Poll engine for best move (non-blocking) */
static int engine_poll(char *bestmove_out) {
    if (!engine_thinking || engine_out < 0) return 0;
    char buf[ENGINE_BUF];
    int n = engine_read_line(buf, sizeof(buf), 0);
    if (n > 0) {
        if (strncmp(buf, "bestmove", 8) == 0) {
            engine_thinking = 0;
            /* Parse bestmove e2e4 or bestmove e7e8q */
            char *sp = buf + 9;
            if (sp && strlen(sp) >= 4) {
                strncpy(bestmove_out, sp, 5);
                bestmove_out[5] = '\0';
                /* trim trailing whitespace */
                for (int i = strlen(bestmove_out)-1; i >= 0 && bestmove_out[i] == ' '; i--)
                    bestmove_out[i] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/* Parse UCI move string into Move */
static int parse_uci_move(const char *uci, Move *m) {
    if (strlen(uci) < 4) return 0;
    int fc = uci[0] - 'a';
    int fr = uci[1] - '1';
    int tc2 = uci[2] - 'a';
    int tr = uci[3] - '1';
    if (fc<0||fc>7||fr<0||fr>7||tc2<0||tc2>7||tr<0||tr>7) return 0;

    m->from_col = fc; m->from_row = fr;
    m->to_col   = tc2; m->to_row  = tr;
    m->promotion = 0;
    if (strlen(uci) >= 5) {
        switch (tolower(uci[4])) {
            case 'q': m->promotion = QUEEN;  break;
            case 'r': m->promotion = ROOK;   break;
            case 'b': m->promotion = BISHOP; break;
            case 'n': m->promotion = KNIGHT; break;
        }
    }

    /* Find matching legal move */
    Move legal[MAX_LEGAL];
    int cnt = generate_legal_moves(legal, MAX_LEGAL, &gs);
    for (int i = 0; i < cnt; i++) {
        if (legal[i].from_row == m->from_row && legal[i].from_col == m->from_col &&
            legal[i].to_row   == m->to_row   && legal[i].to_col   == m->to_col   &&
            legal[i].promotion == m->promotion) {
            *m = legal[i];
            return 1;
        }
    }
    return 0;
}

/* ─────────────────────────────────────────────
   DISPLAY / RENDERING
───────────────────────────────────────────── */

/* Is (r,c) a legal move destination from selected piece? */
static int is_legal_dest(int r, int c) {
    for (int i = 0; i < legal_count; i++)
        if (legal_moves[i].to_row == r && legal_moves[i].to_col == c) return 1;
    return 0;
}

static int is_legal_dest_capture(int r, int c) {
    for (int i = 0; i < legal_count; i++)
        if (legal_moves[i].to_row == r && legal_moves[i].to_col == c &&
            (legal_moves[i].captured_piece != 0 || legal_moves[i].en_passant))
            return 1;
    return 0;
}

/* Wrap PGN text at 60 chars */
static void print_pgn_wrapped(const char *pgn, int max_lines) {
    int len = strlen(pgn);
    int line_len = 0;
    int lines = 0;
    int start = (len > 60 * max_lines) ? len - 60 * max_lines : 0;
    for (int i = start; i < len; i++) {
        if (lines >= max_lines) break;
        printf("%c", pgn[i]);
        line_len++;
        if (line_len >= 60 && pgn[i] == ' ') {
            printf("\r\n");
            line_len = 0;
            lines++;
        }
    }
    while (lines < max_lines) { printf("\r\n"); lines++; }
}

static void render(void) {
    /* Board is displayed rank 8 (row 7) at top, rank 1 (row 0) at bottom */
    /* Clear and redraw */
    printf("\033[H"); /* go home */

    int color_turn = gs.white_to_move ? 1 : -1;
    int king_pos = find_king(gs.board, color_turn);
    int king_in_check = in_check(gs.board, color_turn);
    int king_r = (king_pos >= 0) ? king_pos / 8 : -1;
    int king_c = (king_pos >= 0) ? king_pos % 8 : -1;

    /* Header */
    printf(C_BOLD "\r\n  ♟  Terminal Chess  ♙   " C_RESET);
    printf(" Engine: " C_BOLD "%s" C_RESET, engine_ready ? engine_name : "None");
    if (engine_thinking) printf("  " C_BOLD "\033[33m[Thinking...]\033[0m" C_RESET);
    printf("\r\n");

    /* Time control info */
    printf("  TC: ");
    if (tc.type == TC_TIME)       printf("Time %dms", tc.time_ms);
    else if (tc.type == TC_DEPTH) printf("Depth %d", tc.depth);
    else                           printf("Nodes %ld", tc.nodes);
    printf("   Move: %d   %s to move\r\n",
           gs.fullmove_number,
           gs.white_to_move ? "White" : "Black");

    printf("\r\n");

    /* Column labels */
    printf("    ");
    for (int c = 0; c < 8; c++) printf("  %c  ", 'a' + c);
    printf("\r\n");
    printf("    ");
    for (int c = 0; c < 8; c++) printf("─────");
    printf("\r\n");

    for (int display_r = 7; display_r >= 0; display_r--) {
        /* row label */
        printf("  %d │", display_r + 1);

        for (int c = 0; c < 8; c++) {
            int piece = gs.board[display_r][c].piece;
            int pc    = piece_color(piece);
            int pt    = abs(piece);

            /* Determine background */
            int is_light = (display_r + c) % 2 == 0;
            const char *bg;

            if (display_r == cursor_row && c == cursor_col) {
                bg = BG_CURSOR;
            } else if (display_r == selected_row && c == selected_col) {
                bg = BG_SELECTED;
            } else if (king_in_check && display_r == king_r && c == king_c) {
                bg = BG_CHECK;
            } else if (selected_row >= 0 && is_legal_dest_capture(display_r, c)) {
                bg = BG_CAPTURE;
            } else if (selected_row >= 0 && is_legal_dest(display_r, c)) {
                bg = BG_LEGAL;
            } else if (display_r == last_to_row && c == last_to_col) {
                bg = BG_LAST_TO;
            } else if (display_r == last_from_row && c == last_from_col) {
                bg = BG_LAST_FROM;
            } else {
                bg = is_light ? BG_LIGHT : BG_DARK;
            }

            printf("%s", bg);

            /* Piece color */
            const char *fg = (pc >= 0) ? FG_WHITE_PC : FG_BLACK_PC;
            if (pc < 0) fg = FG_BLACK_PC;

            /* Piece unicode */
            const char *sym;
            if (piece == 0) {
                /* Show legal move dot if applicable */
                if (selected_row >= 0 && is_legal_dest(display_r, c)) {
                    sym = " • ";
                    printf("%s%s  %s  " C_RESET, bg, "\033[38;2;60;60;60m", sym);
                } else {
                    printf("%s     " C_RESET, bg);
                }
                continue;
            } else {
                int idx = (pc == 1) ? 0 : 1;
                sym = piece_unicode[idx][pt];
                printf("%s%s%s %s %s" C_RESET,
                       bg, C_BOLD, fg, sym, C_RESET);
            }
        }
        printf("│ %d\r\n", display_r + 1);
    }

    /* Bottom border */
    printf("    ");
    for (int c = 0; c < 8; c++) printf("─────");
    printf("\r\n");
    printf("    ");
    for (int c = 0; c < 8; c++) printf("  %c  ", 'a' + c);
    printf("\r\n\r\n");

    /* Game over message */
    if (game_over) {
        printf(C_BOLD "  *** %s ***\r\n" C_RESET, game_over_msg);
    }

    /* PGN display */
    printf(C_BOLD "  Moves:\r\n" C_RESET);
    printf("  ────────────────────────────────────────────\r\n");
    printf("  ");
    print_pgn_wrapped(pgn_buf, 5);
    printf("\r\n");

    /* Controls help */
    printf(C_DIM
           "  Controls: Arrows/HJKL=move  Enter/Space=select  U=undo\r\n"
           "            T=time control  R=restart  Q=quit\r\n"
           C_RESET "\r\n");

    fflush(stdout);
}

/* ─────────────────────────────────────────────
   TIME CONTROL MENU
───────────────────────────────────────────── */

static void time_control_menu(void) {
    /* Simple in-place menu */
    printf("\033[2J\033[H");
    printf(C_BOLD "\r\n  Time Control Settings\r\n" C_RESET);
    printf("  ─────────────────────\r\n");
    printf("  1) Time per move (ms)\r\n");
    printf("  2) Fixed depth\r\n");
    printf("  3) Fixed nodes\r\n");
    printf("  Current: ");
    if (tc.type == TC_TIME)       printf("Time %dms\r\n", tc.time_ms);
    else if (tc.type == TC_DEPTH) printf("Depth %d\r\n", tc.depth);
    else                           printf("Nodes %ld\r\n", tc.nodes);
    printf("\r\n  Choose (1-3): ");
    fflush(stdout);

    /* Temporarily restore terminal for input */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h");
    char choice[32] = {0};
    if (fgets(choice, sizeof(choice), stdin)) {
        int ch = atoi(choice);
        if (ch == 1) {
            tc.type = TC_TIME;
            printf("  Time (ms) [current %d]: ", tc.time_ms);
            fflush(stdout);
            char val[32] = {0};
            if (fgets(val, sizeof(val), stdin)) {
                int v = atoi(val);
                if (v > 0) tc.time_ms = v;
            }
        } else if (ch == 2) {
            tc.type = TC_DEPTH;
            printf("  Depth [current %d]: ", tc.depth);
            fflush(stdout);
            char val[32] = {0};
            if (fgets(val, sizeof(val), stdin)) {
                int v = atoi(val);
                if (v > 0) tc.depth = v;
            }
        } else if (ch == 3) {
            tc.type = TC_NODES;
            printf("  Nodes [current %ld]: ", tc.nodes);
            fflush(stdout);
            char val[32] = {0};
            if (fgets(val, sizeof(val), stdin)) {
                long v = atol(val);
                if (v > 0) tc.nodes = v;
            }
        }
    }
    /* Re-setup raw terminal */
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=  (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l");
    clear_screen();
}

/* ─────────────────────────────────────────────
   PROMOTION MENU
───────────────────────────────────────────── */

static int promotion_menu(void) {
    /* Show inline promotion selection */
    printf("\033[2J\033[H");
    printf(C_BOLD "\r\n  Pawn Promotion!\r\n" C_RESET);
    printf("  Q) Queen  R) Rook  B) Bishop  N) Knight\r\n");
    printf("  Choose: ");
    fflush(stdout);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h");
    char c[4] = {0};
    fgets(c, sizeof(c), stdin);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=  (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l");
    clear_screen();

    switch (tolower(c[0])) {
        case 'r': return ROOK;
        case 'b': return BISHOP;
        case 'n': return KNIGHT;
        default:  return QUEEN;
    }
}

/* ─────────────────────────────────────────────
   GAME LOGIC / MOVE HANDLING
───────────────────────────────────────────── */

static void compute_legal_for_selected(void) {
    legal_count = 0;
    if (selected_row < 0) return;
    int piece = gs.board[selected_row][selected_col].piece;
    int color = gs.white_to_move ? 1 : -1;
    if (piece_color(piece) != color) { selected_row = -1; return; }

    Move all[MAX_LEGAL];
    int cnt = generate_legal_moves(all, MAX_LEGAL, &gs);
    for (int i = 0; i < cnt; i++) {
        if (all[i].from_row == selected_row && all[i].from_col == selected_col) {
            legal_moves[legal_count++] = all[i];
        }
    }
}

static void check_game_over(void) {
    int color = gs.white_to_move ? 1 : -1;
    Move all[MAX_LEGAL];
    int cnt = generate_legal_moves(all, MAX_LEGAL, &gs);
    if (cnt == 0) {
        if (in_check(gs.board, color)) {
            snprintf(game_over_msg, sizeof(game_over_msg),
                     "%s wins by Checkmate!", gs.white_to_move ? "Black" : "White");
            /* PGN result */
            if (pgn_len + 8 < MAX_PGN_LEN)
                strcat(pgn_buf, gs.white_to_move ? "0-1" : "1-0");
        } else {
            snprintf(game_over_msg, sizeof(game_over_msg), "Stalemate! Draw.");
            if (pgn_len + 8 < MAX_PGN_LEN) strcat(pgn_buf, "1/2-1/2");
        }
        game_over = 1;
    }
    /* 50-move rule */
    if (gs.halfmove_clock >= 100 && !game_over) {
        snprintf(game_over_msg, sizeof(game_over_msg), "Draw by 50-move rule.");
        if (pgn_len + 8 < MAX_PGN_LEN) strcat(pgn_buf, "1/2-1/2");
        game_over = 1;
    }
}

/* Try to make a move from selected to (r,c) */
static void try_move_to(int r, int c) {
    if (selected_row < 0) return;
    /* Find matching legal move */
    Move *match = NULL;
    int promo_needed = 0;
    for (int i = 0; i < legal_count; i++) {
        if (legal_moves[i].to_row == r && legal_moves[i].to_col == c) {
            if (legal_moves[i].promotion) promo_needed = 1;
            match = &legal_moves[i];
            break;
        }
    }
    if (!match) {
        /* Clicked on own piece? Re-select */
        int pc = piece_color(gs.board[r][c].piece);
        int color = gs.white_to_move ? 1 : -1;
        if (pc == color) {
            selected_row = r; selected_col = c;
            compute_legal_for_selected();
        } else {
            selected_row = -1; legal_count = 0;
        }
        return;
    }

    Move m = *match;

    /* Handle promotion selection */
    if (promo_needed) {
        /* Find all promotions for this square */
        int promos[4] = {0}; int np = 0;
        for (int i = 0; i < legal_count; i++) {
            if (legal_moves[i].to_row == r && legal_moves[i].to_col == c &&
                legal_moves[i].promotion) {
                promos[np++] = legal_moves[i].promotion;
            }
        }
        if (np > 1) {
            int chosen = promotion_menu();
            for (int i = 0; i < legal_count; i++) {
                if (legal_moves[i].to_row == r && legal_moves[i].to_col == c &&
                    legal_moves[i].promotion == chosen) {
                    m = legal_moves[i];
                    break;
                }
            }
        }
    }

    do_move(&gs, &m);
    selected_row = -1; selected_col = -1; legal_count = 0;
    check_game_over();

    /* Trigger engine if it's engine's turn */
    if (!game_over && engine_ready) {
        int eturn = gs.white_to_move ? 1 : -1;
        if (player_color == 0 || eturn != player_color) {
            engine_go();
        }
    }
}

/* Handle user input */
static void handle_key(int key) {
    if (game_over && key != 'r' && key != 'R' && key != 'q' && key != 'Q') {
        if (key == 'u' || key == 'U') goto do_undo;
        return;
    }

    switch (key) {
        case 1000: /* up */
            if (cursor_row < 7) cursor_row++;
            break;
        case 1001: /* down */
            if (cursor_row > 0) cursor_row--;
            break;
        case 1002: /* right */
            if (cursor_col < 7) cursor_col++;
            break;
        case 1003: /* left */
            if (cursor_col > 0) cursor_col--;
            break;
        case 'k': case 'K': if (cursor_row < 7) cursor_row++; break;
        case 'j': case 'J': if (cursor_row > 0) cursor_row--; break;
        case 'l': case 'L': if (cursor_col < 7) cursor_col++; break;
        case 'h': case 'H': if (cursor_col > 0) cursor_col--; break;

        case '\r': case '\n': case ' ': {
            /* Check if it's player's turn */
            int color = gs.white_to_move ? 1 : -1;
            if (player_color != 0 && color != player_color) break;
            if (engine_thinking) break;

            if (selected_row < 0) {
                /* Select piece */
                int pc = piece_color(gs.board[cursor_row][cursor_col].piece);
                if (pc == color) {
                    selected_row = cursor_row;
                    selected_col = cursor_col;
                    compute_legal_for_selected();
                }
            } else {
                /* Try move */
                try_move_to(cursor_row, cursor_col);
            }
            break;
        }

        do_undo:
        case 'u': case 'U': {
            if (engine_thinking) {
                engine_write("stop\n");
                engine_thinking = 0;
            }
            /* Undo twice if playing against engine */
            int undo_count = (engine_ready && player_color != 0) ? 2 : 1;
            for (int i = 0; i < undo_count && gs.hist_count > 0; i++)
                undo_move(&gs);
            selected_row = -1; legal_count = 0;
            game_over = 0; game_over_msg[0] = '\0';
            break;
        }

        case 't': case 'T':
            if (engine_thinking) { engine_write("stop\n"); engine_thinking = 0; }
            time_control_menu();
            break;

        case 'r': case 'R':
            if (engine_thinking) { engine_write("stop\n"); engine_thinking = 0; }
            init_board();
            selected_row = -1; legal_count = 0;
            if (engine_ready) engine_write("ucinewgame\n");
            break;

        case 'q': case 'Q':
            restore_terminal();
            stop_engine();
            clear_screen();
            printf("Thanks for playing!\n");
            exit(0);

        case '\033':
            selected_row = -1; legal_count = 0;
            break;
    }
}

/* ─────────────────────────────────────────────
   MAIN
───────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    setup_terminal();
    clear_screen();

    init_board();
    player_color = 1;  /* Human plays white by default */

    /* Start engine if provided */
    if (argc >= 2) {
        strncpy(engine_path, argv[1], sizeof(engine_path) - 1);
        if (!start_engine(engine_path)) {
            engine_ready = 0;
            snprintf(engine_name, sizeof(engine_name), "Failed: %s", argv[1]);
        }
    }

    /* Initial render */
    render();

    /* Main loop */
    while (1) {
        /* Poll for engine move */
        if (engine_thinking) {
            char bestmove[16] = {0};
            if (engine_poll(bestmove)) {
                Move m;
                memset(&m, 0, sizeof(m));
                if (parse_uci_move(bestmove, &m)) {
                    do_move(&gs, &m);
                    check_game_over();
                }
                render();
            }
        }

        /* Poll for user key (with short timeout so engine poll keeps running) */
        struct timeval tv = { 0, 50000 }; /* 50ms */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            int key = read_key();
            if (key) {
                handle_key(key);
                render();
            }
        } else if (!engine_thinking) {
            /* Gentle re-render occasionally */
            static int idle = 0;
            if (++idle > 20) { idle = 0; render(); }
        }
    }

    return 0;
}
