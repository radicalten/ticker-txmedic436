/*
 * chess_gui.c - Terminal Chess GUI with UCI Engine Support
 * 
 * Compile: gcc -o chess_gui chess_gui.c
 * Run:     ./chess_gui
 *
 * Controls:
 *   Arrow Keys    - Move cursor
 *   Enter/Space   - Select piece / Make move
 *   U             - Undo last move (takeback)
 *   Q             - Quit
 *   F             - Flip board
 *   N             - New game
 *   E             - Engine settings
 *   H             - Help
 */

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
#include <errno.h>
#include <time.h>
#include <stdarg.h>

/* ─── ANSI Color Codes ──────────────────────────────────────────────────── */
#define ANSI_RESET       "\033[0m"
#define ANSI_BOLD        "\033[1m"
#define ANSI_DIM         "\033[2m"

/* Foreground colors */
#define FG_BLACK         "\033[30m"
#define FG_RED           "\033[31m"
#define FG_GREEN         "\033[32m"
#define FG_YELLOW        "\033[33m"
#define FG_BLUE          "\033[34m"
#define FG_MAGENTA       "\033[35m"
#define FG_CYAN          "\033[36m"
#define FG_WHITE         "\033[37m"
#define FG_BRIGHT_BLACK  "\033[90m"
#define FG_BRIGHT_WHITE  "\033[97m"

/* Background colors */
#define BG_BLACK         "\033[40m"
#define BG_RED           "\033[41m"
#define BG_GREEN         "\033[42m"
#define BG_YELLOW        "\033[43m"
#define BG_BLUE          "\033[44m"
#define BG_MAGENTA       "\033[45m"
#define BG_CYAN          "\033[46m"
#define BG_WHITE         "\033[47m"
#define BG_BRIGHT_BLACK  "\033[100m"
#define BG_BRIGHT_RED    "\033[101m"
#define BG_BRIGHT_GREEN  "\033[102m"
#define BG_BRIGHT_YELLOW "\033[103m"
#define BG_BRIGHT_BLUE   "\033[104m"
#define BG_BRIGHT_WHITE  "\033[107m"

/* Terminal control */
#define CLEAR_SCREEN     "\033[2J"
#define CURSOR_HOME      "\033[H"
#define HIDE_CURSOR      "\033[?25l"
#define SHOW_CURSOR      "\033[?25h"
#define SAVE_CURSOR      "\033[s"
#define RESTORE_CURSOR   "\033[u"
#define CLEAR_LINE       "\033[2K"
#define CURSOR_UP(n)     "\033[" #n "A"

/* ─── Board Constants ────────────────────────────────────────────────────── */
#define BOARD_SIZE      8
#define MAX_MOVES       512
#define MAX_HISTORY     512
#define MAX_PGN_LEN     16384
#define MAX_ENGINE_PATH 512
#define MAX_LINE        4096
#define ENGINE_TIMEOUT  30

/* Piece definitions */
#define EMPTY   0
#define PAWN    1
#define KNIGHT  2
#define BISHOP  3
#define ROOK    4
#define QUEEN   5
#define KING    6

#define WHITE   8
#define BLACK   16

#define WHITE_PAWN   (WHITE | PAWN)
#define WHITE_KNIGHT (WHITE | KNIGHT)
#define WHITE_BISHOP (WHITE | BISHOP)
#define WHITE_ROOK   (WHITE | ROOK)
#define WHITE_QUEEN  (WHITE | QUEEN)
#define WHITE_KING   (WHITE | KING)
#define BLACK_PAWN   (BLACK | PAWN)
#define BLACK_KNIGHT (BLACK | KNIGHT)
#define BLACK_BISHOP (BLACK | BISHOP)
#define BLACK_ROOK   (BLACK | ROOK)
#define BLACK_QUEEN  (BLACK | QUEEN)
#define BLACK_KING   (BLACK | KING)

#define PIECE_TYPE(p)  ((p) & 7)
#define PIECE_COLOR(p) ((p) & 24)
#define IS_WHITE(p)    (((p) & 24) == WHITE)
#define IS_BLACK(p)    (((p) & 24) == BLACK)
#define IS_EMPTY(p)    ((p) == EMPTY)

/* Castling rights */
#define CASTLE_WK  1
#define CASTLE_WQ  2
#define CASTLE_BK  4
#define CASTLE_BQ  8

/* ─── Data Structures ────────────────────────────────────────────────────── */

typedef struct {
    int from_row, from_col;
    int to_row,   to_col;
    int piece;
    int captured;
    int promotion;
    int castling_rights_before;
    int en_passant_before;   /* column (-1 if none) */
    int is_castling;         /* 0=none,1=kingside,2=queenside */
    int is_en_passant;
    int is_promotion;
    int halfmove_clock_before;
    char algebraic[16];      /* e.g. "Nf3", "O-O", etc. */
    char uci[6];             /* e.g. "e2e4"              */
} Move;

typedef struct {
    int board[8][8];
    int turn;                /* WHITE or BLACK */
    int castling_rights;
    int en_passant_col;      /* -1 if no EP */
    int halfmove_clock;
    int fullmove_number;
    Move history[MAX_HISTORY];
    int history_count;
} GameState;

typedef enum {
    TC_MOVETIME = 0,
    TC_DEPTH    = 1,
    TC_NODES    = 2
} TimeControl;

typedef struct {
    char      path[MAX_ENGINE_PATH];
    int       enabled;
    TimeControl tc_mode;
    int       movetime;      /* ms */
    int       depth;
    long long nodes;
    int       plays_as;      /* WHITE or BLACK */
    pid_t     pid;
    int       in_fd;         /* write to engine  */
    int       out_fd;        /* read from engine */
    int       ready;
} EngineState;

typedef struct {
    int cursor_row, cursor_col;
    int selected_row, selected_col;
    int has_selection;
    int flipped;
    int valid_targets[8][8]; /* 1 = valid move destination */
    char status_msg[256];
    char pgn[MAX_PGN_LEN];
    int  pgn_len;
    int  game_over;
    char game_result[16];    /* "1-0", "0-1", "1/2-1/2", "*" */
} UIState;

/* ─── Global State ───────────────────────────────────────────────────────── */
static GameState  g_game;
static EngineState g_engine;
static UIState    g_ui;
static struct termios g_orig_termios;
static volatile int g_running = 1;

/* ─── Function Prototypes ────────────────────────────────────────────────── */
/* Terminal */
void term_init(void);
void term_restore(void);
void term_raw(void);
int  term_read_key(void);
void cursor_goto(int row, int col);

/* Board */
void board_init(void);
void board_set_start(void);
int  board_square_color(int row, int col);

/* Move generation */
int  is_in_check(GameState *g, int color);
int  generate_legal_moves(GameState *g, int row, int col,
                          int targets[8][8]);
int  is_checkmate(GameState *g);
int  is_stalemate(GameState *g);
int  is_insufficient_material(GameState *g);
int  is_fifty_move(GameState *g);

/* Move making */
int  make_move(GameState *g, int fr, int fc, int tr, int tc,
               int promotion, Move *out_move);
void undo_move(GameState *g);
char *move_to_algebraic(GameState *g_before, int fr, int fc,
                        int tr, int tc, int promo, char *buf);
void move_to_uci(int fr, int fc, int tr, int tc, int promo, char *buf);

/* PGN */
void pgn_append_move(UIState *ui, GameState *g_after, Move *m,
                     int move_number, int is_white);
void pgn_rebuild(UIState *ui, GameState *g);

/* Engine */
int  engine_start(EngineState *e);
void engine_stop(EngineState *e);
void engine_send(EngineState *e, const char *fmt, ...);
int  engine_readline(EngineState *e, char *buf, int timeout_ms);
void engine_new_game(EngineState *e);
void engine_set_position(EngineState *e, GameState *g);
void engine_go(EngineState *e);
int  engine_wait_bestmove(EngineState *e, char *best, int timeout_s);
void engine_parse_bestmove(const char *best, int *fr, int *fc,
                           int *tr, int *tc, int *promo);

/* UI */
void ui_draw(void);
void ui_draw_board(void);
void ui_draw_info(void);
void ui_draw_pgn(void);
void ui_draw_help(void);
void ui_draw_status(void);
void ui_draw_settings(void);
void ui_set_status(const char *fmt, ...);
void ui_handle_key(int key);
void ui_select_or_move(void);
void ui_do_engine_move(void);
void ui_settings_menu(void);
void ui_new_game(void);
void ui_show_help_screen(void);
void ui_configure_engine(void);

/* Misc */
const char *piece_glyph(int piece);
const char *piece_letter(int piece);
int  parse_uci_square(const char *s, int *row, int *col);

/* ─── Key Codes ──────────────────────────────────────────────────────────── */
#define KEY_UP     1000
#define KEY_DOWN   1001
#define KEY_LEFT   1002
#define KEY_RIGHT  1003
#define KEY_ENTER  10
#define KEY_SPACE  32
#define KEY_ESC    27
#define KEY_CTRL_C 3

/* ═══════════════════════════════════════════════════════════════════════════
 * Terminal
 * ═══════════════════════════════════════════════════════════════════════════ */
void term_init(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    atexit(term_restore);
    term_raw();
    printf(HIDE_CURSOR);
    fflush(stdout);
}

void term_restore(void) {
    printf(SHOW_CURSOR ANSI_RESET "\n");
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
}

void term_raw(void) {
    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=  (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;   /* 100ms timeout */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void cursor_goto(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

int term_read_key(void) {
    unsigned char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    if (c == KEY_CTRL_C) return KEY_CTRL_C;
    if (c == KEY_ESC) {
        unsigned char seq[4];
        int n1 = read(STDIN_FILENO, &seq[0], 1);
        if (n1 <= 0) return KEY_ESC;
        int n2 = read(STDIN_FILENO, &seq[1], 1);
        if (n2 <= 0) return KEY_ESC;
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return KEY_ESC;
    }
    return (int)c;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Piece Glyphs & Letters
 * ═══════════════════════════════════════════════════════════════════════════ */
const char *piece_glyph(int piece) {
    switch (piece) {
        case WHITE_KING:   return "♔";
        case WHITE_QUEEN:  return "♕";
        case WHITE_ROOK:   return "♖";
        case WHITE_BISHOP: return "♗";
        case WHITE_KNIGHT: return "♘";
        case WHITE_PAWN:   return "♙";
        case BLACK_KING:   return "♚";
        case BLACK_QUEEN:  return "♛";
        case BLACK_ROOK:   return "♜";
        case BLACK_BISHOP: return "♝";
        case BLACK_KNIGHT: return "♞";
        case BLACK_PAWN:   return "♟";
        default:           return " ";
    }
}

const char *piece_letter(int piece) {
    switch (PIECE_TYPE(piece)) {
        case KING:   return "K";
        case QUEEN:  return "Q";
        case ROOK:   return "R";
        case BISHOP: return "B";
        case KNIGHT: return "N";
        case PAWN:   return "";
        default:     return "";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Board Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */
void board_set_start(void) {
    GameState *g = &g_game;
    memset(g, 0, sizeof(*g));
    g->en_passant_col = -1;
    g->turn = WHITE;
    g->castling_rights = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    g->fullmove_number = 1;

    /* Black pieces (row 0 = rank 8) */
    g->board[0][0] = BLACK_ROOK;
    g->board[0][1] = BLACK_KNIGHT;
    g->board[0][2] = BLACK_BISHOP;
    g->board[0][3] = BLACK_QUEEN;
    g->board[0][4] = BLACK_KING;
    g->board[0][5] = BLACK_BISHOP;
    g->board[0][6] = BLACK_KNIGHT;
    g->board[0][7] = BLACK_ROOK;
    for (int c = 0; c < 8; c++) g->board[1][c] = BLACK_PAWN;

    /* White pieces (row 7 = rank 1) */
    g->board[7][0] = WHITE_ROOK;
    g->board[7][1] = WHITE_KNIGHT;
    g->board[7][2] = WHITE_BISHOP;
    g->board[7][3] = WHITE_QUEEN;
    g->board[7][4] = WHITE_KING;
    g->board[7][5] = WHITE_BISHOP;
    g->board[7][6] = WHITE_KNIGHT;
    g->board[7][7] = WHITE_ROOK;
    for (int c = 0; c < 8; c++) g->board[6][c] = WHITE_PAWN;
}

void board_init(void) {
    board_set_start();
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.cursor_row = 7;
    g_ui.cursor_col = 4;
    g_ui.selected_row = -1;
    g_ui.selected_col = -1;
    g_ui.has_selection = 0;
    g_ui.flipped = 0;
    strcpy(g_ui.game_result, "*");
    g_ui.pgn[0] = '\0';
    g_ui.pgn_len = 0;
    g_ui.game_over = 0;
    ui_set_status("Welcome! Arrow keys to move, Enter/Space to select.");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Move Generation (legal moves)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Apply a pseudo-legal move to a temporary board for check detection */
static void apply_move_temp(GameState *g, int fr, int fc, int tr, int tc,
                             int promo) {
    int piece = g->board[fr][fc];
    int type  = PIECE_TYPE(piece);
    int color = PIECE_COLOR(piece);

    /* En passant capture */
    if (type == PAWN && tc == g->en_passant_col &&
        ((color == WHITE && fr == 3) || (color == BLACK && fr == 4))) {
        int ep_row = (color == WHITE) ? 4 : 3;
        g->board[ep_row][tc] = EMPTY;
        g->en_passant_col = -1;
//        g->is_en_passant = 1; /* temp flag - not a real field, abuse */
    } else {
        g->en_passant_col = -1;
    }

    /* Castling */
    if (type == KING && abs(tc - fc) == 2) {
        int rook_from_col = (tc > fc) ? 7 : 0;
        int rook_to_col   = (tc > fc) ? fc + 1 : fc - 1;
        g->board[tr][rook_to_col] = g->board[tr][rook_from_col];
        g->board[tr][rook_from_col] = EMPTY;
    }

    /* Move piece */
    if (promo && type == PAWN && (tr == 0 || tr == 7)) {
        g->board[tr][tc] = color | promo;
    } else {
        g->board[tr][tc] = piece;
    }
    g->board[fr][fc] = EMPTY;

    /* Set en passant square */
    if (type == PAWN && abs(tr - fr) == 2) {
        g->en_passant_col = fc;
    }
}

/* Dummy field used by apply_move_temp (hack) */
int GameState_is_en_passant_dummy; /* not actually in struct, just a note */

static int find_king(GameState *g, int color, int *kr, int *kc) {
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (g->board[r][c] == (color | KING)) {
                *kr = r; *kc = c; return 1;
            }
    return 0;
}

static int square_attacked(GameState *g, int row, int col, int by_color) {
    /* Check attacks by by_color pieces */
    int opp = by_color;

    /* Pawn attacks */
    int pdir = (opp == WHITE) ? 1 : -1;  /* white pawns attack upward (decreasing row) */
    int pr = row + pdir;
    if (pr >= 0 && pr < 8) {
        if (col > 0 && g->board[pr][col-1] == (opp | PAWN)) return 1;
        if (col < 7 && g->board[pr][col+1] == (opp | PAWN)) return 1;
    }

    /* Knight attacks */
    int kd[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},
                    {1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = row + kd[i][0], nc = col + kd[i][1];
        if (nr>=0&&nr<8&&nc>=0&&nc<8 && g->board[nr][nc]==(opp|KNIGHT))
            return 1;
    }

    /* Bishop / Queen (diagonals) */
    int dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int d = 0; d < 4; d++) {
        int r = row + dirs[d][0], c = col + dirs[d][1];
        while (r>=0&&r<8&&c>=0&&c<8) {
            int p = g->board[r][c];
            if (p != EMPTY) {
                if (PIECE_COLOR(p)==opp &&
                    (PIECE_TYPE(p)==BISHOP||PIECE_TYPE(p)==QUEEN))
                    return 1;
                break;
            }
            r += dirs[d][0]; c += dirs[d][1];
        }
    }

    /* Rook / Queen (straight) */
    int rdirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int d = 0; d < 4; d++) {
        int r = row + rdirs[d][0], c = col + rdirs[d][1];
        while (r>=0&&r<8&&c>=0&&c<8) {
            int p = g->board[r][c];
            if (p != EMPTY) {
                if (PIECE_COLOR(p)==opp &&
                    (PIECE_TYPE(p)==ROOK||PIECE_TYPE(p)==QUEEN))
                    return 1;
                break;
            }
            r += rdirs[d][0]; c += rdirs[d][1];
        }
    }

    /* King attacks */
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            int nr = row+dr, nc = col+dc;
            if (nr>=0&&nr<8&&nc>=0&&nc<8 &&
                g->board[nr][nc]==(opp|KING)) return 1;
        }

    return 0;
}

int is_in_check(GameState *g, int color) {
    int kr, kc;
    if (!find_king(g, color, &kr, &kc)) return 0;
    int opp = (color == WHITE) ? BLACK : WHITE;
    return square_attacked(g, kr, kc, opp);
}

/* Test if a pseudo-legal move leaves own king in check */
static int move_leaves_in_check(GameState *g, int fr, int fc,
                                 int tr, int tc, int promo) {
    GameState tmp = *g;
    apply_move_temp(&tmp, fr, fc, tr, tc, promo);
    return is_in_check(&tmp, g->turn);
}

/* Generate pseudo-legal destinations for piece at (row,col) */
static void pseudo_moves(GameState *g, int row, int col,
                         int dests[8][8]) {
    memset(dests, 0, 64 * sizeof(int));
    int piece = g->board[row][col];
    if (piece == EMPTY) return;
    int type  = PIECE_TYPE(piece);
    int color = PIECE_COLOR(piece);
    int opp   = (color == WHITE) ? BLACK : WHITE;

    switch (type) {
    case PAWN: {
        int dir = (color == WHITE) ? -1 : 1;
        int start_row = (color == WHITE) ? 6 : 1;
        int nr = row + dir;
        /* Forward one */
        if (nr >= 0 && nr < 8 && g->board[nr][col] == EMPTY) {
            dests[nr][col] = 1;
            /* Forward two from starting rank */
            if (row == start_row) {
                int nr2 = row + 2*dir;
                if (g->board[nr2][col] == EMPTY)
                    dests[nr2][col] = 1;
            }
        }
        /* Captures */
        for (int dc = -1; dc <= 1; dc += 2) {
            int nc = col + dc;
            if (nc < 0 || nc >= 8) continue;
            if (nr < 0 || nr >= 8) continue;
            if (!IS_EMPTY(g->board[nr][nc]) &&
                PIECE_COLOR(g->board[nr][nc]) == opp)
                dests[nr][nc] = 1;
            /* En passant */
            if (g->en_passant_col == nc &&
                ((color == WHITE && row == 3) ||
                 (color == BLACK && row == 4)))
                dests[nr][nc] = 1;
        }
        break;
    }
    case KNIGHT: {
        int moves[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},
                           {1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = row + moves[i][0], nc = col + moves[i][1];
            if (nr<0||nr>=8||nc<0||nc>=8) continue;
            if (IS_EMPTY(g->board[nr][nc]) ||
                PIECE_COLOR(g->board[nr][nc]) == opp)
                dests[nr][nc] = 1;
        }
        break;
    }
    case BISHOP: {
        int dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
        for (int d = 0; d < 4; d++) {
            int r = row + dirs[d][0], c = col + dirs[d][1];
            while (r>=0&&r<8&&c>=0&&c<8) {
                if (IS_EMPTY(g->board[r][c])) { dests[r][c]=1; }
                else {
                    if (PIECE_COLOR(g->board[r][c]) == opp)
                        dests[r][c] = 1;
                    break;
                }
                r += dirs[d][0]; c += dirs[d][1];
            }
        }
        break;
    }
    case ROOK: {
        int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
        for (int d = 0; d < 4; d++) {
            int r = row + dirs[d][0], c = col + dirs[d][1];
            while (r>=0&&r<8&&c>=0&&c<8) {
                if (IS_EMPTY(g->board[r][c])) { dests[r][c]=1; }
                else {
                    if (PIECE_COLOR(g->board[r][c]) == opp)
                        dests[r][c] = 1;
                    break;
                }
                r += dirs[d][0]; c += dirs[d][1];
            }
        }
        break;
    }
    case QUEEN: {
        int dirs[8][2] = {{-1,0},{1,0},{0,-1},{0,1},
                          {-1,-1},{-1,1},{1,-1},{1,1}};
        for (int d = 0; d < 8; d++) {
            int r = row + dirs[d][0], c = col + dirs[d][1];
            while (r>=0&&r<8&&c>=0&&c<8) {
                if (IS_EMPTY(g->board[r][c])) { dests[r][c]=1; }
                else {
                    if (PIECE_COLOR(g->board[r][c]) == opp)
                        dests[r][c] = 1;
                    break;
                }
                r += dirs[d][0]; c += dirs[d][1];
            }
        }
        break;
    }
    case KING: {
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++) {
                if (!dr && !dc) continue;
                int nr = row+dr, nc = col+dc;
                if (nr<0||nr>=8||nc<0||nc>=8) continue;
                if (IS_EMPTY(g->board[nr][nc]) ||
                    PIECE_COLOR(g->board[nr][nc]) == opp)
                    dests[nr][nc] = 1;
            }
        /* Castling */
        if (!is_in_check(g, color)) {
            /* Kingside */
            int ks_flag = (color==WHITE) ? CASTLE_WK : CASTLE_BK;
            int r = (color==WHITE) ? 7 : 0;
            if ((g->castling_rights & ks_flag) &&
                g->board[r][5] == EMPTY &&
                g->board[r][6] == EMPTY &&
                !square_attacked(g, r, 5, opp) &&
                !square_attacked(g, r, 6, opp))
                dests[r][6] = 1;
            /* Queenside */
            int qs_flag = (color==WHITE) ? CASTLE_WQ : CASTLE_BQ;
            if ((g->castling_rights & qs_flag) &&
                g->board[r][3] == EMPTY &&
                g->board[r][2] == EMPTY &&
                g->board[r][1] == EMPTY &&
                !square_attacked(g, r, 3, opp) &&
                !square_attacked(g, r, 2, opp))
                dests[r][2] = 1;
        }
        break;
    }
    }
}

int generate_legal_moves(GameState *g, int row, int col, int targets[8][8]) {
    int pseudo[8][8];
    pseudo_moves(g, row, col, pseudo);
    memset(targets, 0, 64 * sizeof(int));
    int count = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            if (!pseudo[r][c]) continue;
            if (!move_leaves_in_check(g, row, col, r, c, QUEEN)) {
                targets[r][c] = 1;
                count++;
            }
        }
    return count;
}

/* Check if the current player has any legal moves */
static int has_any_legal_moves(GameState *g) {
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int p = g->board[r][c];
            if (IS_EMPTY(p) || PIECE_COLOR(p) != g->turn) continue;
            int targets[8][8];
            if (generate_legal_moves(g, r, c, targets) > 0) return 1;
        }
    return 0;
}

int is_checkmate(GameState *g) {
    return is_in_check(g, g->turn) && !has_any_legal_moves(g);
}

int is_stalemate(GameState *g) {
    return !is_in_check(g, g->turn) && !has_any_legal_moves(g);
}

int is_insufficient_material(GameState *g) {
    int wb = 0, wn = 0, bb = 0, bn = 0;
    int other = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int p = g->board[r][c];
            if (IS_EMPTY(p)) continue;
            int t = PIECE_TYPE(p);
            if (t == KING) continue;
            if (t == PAWN || t == ROOK || t == QUEEN) { other=1; break; }
            if (IS_WHITE(p)) { if(t==BISHOP)wb++; else if(t==KNIGHT)wn++; }
            else              { if(t==BISHOP)bb++; else if(t==KNIGHT)bn++; }
        }
    if (other) return 0;
    int wm = wb + wn, bm = bb + bn;
    if (wm == 0 && bm == 0) return 1; /* K vs K */
    if (wm == 1 && bm == 0 && (wb==1||wn==1)) return 1; /* K+B/N vs K */
    if (bm == 1 && wm == 0 && (bb==1||bn==1)) return 1;
    return 0;
}

int is_fifty_move(GameState *g) {
    return g->halfmove_clock >= 100;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Algebraic Notation
 * ═══════════════════════════════════════════════════════════════════════════ */
static char col_char(int c) { return 'a' + c; }
static char row_char(int r) { return '1' + (7 - r); }

char *move_to_algebraic(GameState *g_before, int fr, int fc,
                        int tr, int tc, int promo, char *buf) {
    int piece = g_before->board[fr][fc];
    int type  = PIECE_TYPE(piece);
    int color = PIECE_COLOR(piece);

    /* Castling */
    if (type == KING && abs(tc - fc) == 2) {
        strcpy(buf, tc > fc ? "O-O" : "O-O-O");
        /* Check/mate suffix added later */
        return buf;
    }

    char tmp[32] = {0};
    int pos = 0;

    /* Piece letter */
    if (type != PAWN) {
        const char *l = piece_letter(piece);
        tmp[pos++] = l[0];
    }

    /* Disambiguation */
    if (type != PAWN) {
        /* Find other same-type pieces that can also move to (tr,tc) */
        int ambig_file = 0, ambig_rank = 0, ambig_count = 0;
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                if (r == fr && c == fc) continue;
                if (g_before->board[r][c] != piece) continue;
                int t2[8][8];
                generate_legal_moves(g_before, r, c, t2);
                if (t2[tr][tc]) {
                    ambig_count++;
                    if (c == fc) ambig_rank = 1;
                    else         ambig_file = 1;
                }
            }
        if (ambig_count > 0) {
            if (!ambig_file)      tmp[pos++] = col_char(fc);
            else if (!ambig_rank) tmp[pos++] = row_char(fr);
            else { tmp[pos++] = col_char(fc); tmp[pos++] = row_char(fr); }
        }
    } else {
        /* Pawn capture: show source file */
        if (!IS_EMPTY(g_before->board[tr][tc]) || tc == g_before->en_passant_col)
            tmp[pos++] = col_char(fc);
    }

    /* Capture */
    if (!IS_EMPTY(g_before->board[tr][tc]) ||
        (type == PAWN && tc == g_before->en_passant_col &&
         ((color==WHITE&&fr==3)||(color==BLACK&&fr==4))))
        tmp[pos++] = 'x';

    /* Destination */
    tmp[pos++] = col_char(tc);
    tmp[pos++] = row_char(tr);

    /* Promotion */
    if (promo) {
        tmp[pos++] = '=';
        switch (promo) {
            case QUEEN:  tmp[pos++]='Q'; break;
            case ROOK:   tmp[pos++]='R'; break;
            case BISHOP: tmp[pos++]='B'; break;
            case KNIGHT: tmp[pos++]='N'; break;
        }
    }

    /* Apply move to temp and check for check/mate */
    GameState tmp_g = *g_before;
    apply_move_temp(&tmp_g, fr, fc, tr, tc, promo);
    int opp = (color == WHITE) ? BLACK : WHITE;
    tmp_g.turn = opp;
    if (is_in_check(&tmp_g, opp)) {
        tmp_g.turn = opp;
        if (is_checkmate(&tmp_g)) tmp[pos++] = '#';
        else                       tmp[pos++] = '+';
    }

    tmp[pos] = '\0';
    strcpy(buf, tmp);
    return buf;
}

void move_to_uci(int fr, int fc, int tr, int tc, int promo, char *buf) {
    buf[0] = col_char(fc);
    buf[1] = row_char(fr);
    buf[2] = col_char(tc);
    buf[3] = row_char(tr);
    if (promo) {
        switch (promo) {
            case QUEEN:  buf[4]='q'; break;
            case ROOK:   buf[4]='r'; break;
            case BISHOP: buf[4]='b'; break;
            case KNIGHT: buf[4]='n'; break;
            default:     buf[4]='q'; break;
        }
        buf[5] = '\0';
    } else {
        buf[4] = '\0';
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Make / Undo Move
 * ═══════════════════════════════════════════════════════════════════════════ */
int make_move(GameState *g, int fr, int fc, int tr, int tc,
              int promotion, Move *out_move) {
    int piece = g->board[fr][fc];
    int type  = PIECE_TYPE(piece);
    int color = PIECE_COLOR(piece);

    Move m;
    memset(&m, 0, sizeof(m));
    m.from_row = fr; m.from_col = fc;
    m.to_row   = tr; m.to_col   = tc;
    m.piece    = piece;
    m.captured = g->board[tr][tc];
    m.promotion = promotion;
    m.castling_rights_before = g->castling_rights;
    m.en_passant_before = g->en_passant_col;
    m.halfmove_clock_before = g->halfmove_clock;

    /* Save algebraic before modifying board */
    move_to_algebraic(g, fr, fc, tr, tc, promotion, m.algebraic);
    move_to_uci(fr, fc, tr, tc, promotion, m.uci);

    /* En passant capture */
    if (type == PAWN && tc == g->en_passant_col &&
        ((color == WHITE && fr == 3) || (color == BLACK && fr == 4))) {
        int ep_row = (color == WHITE) ? 4 : 3;
        m.captured = g->board[ep_row][tc];
        g->board[ep_row][tc] = EMPTY;
        m.is_en_passant = 1;
    }

    /* Castling */
    if (type == KING && abs(tc - fc) == 2) {
        int rook_from = (tc > fc) ? 7 : 0;
        int rook_to   = (tc > fc) ? fc + 1 : fc - 1;
        g->board[fr][rook_to]   = g->board[fr][rook_from];
        g->board[fr][rook_from] = EMPTY;
        m.is_castling = (tc > fc) ? 1 : 2;
    }

    /* Move piece */
    if (promotion && type == PAWN && (tr == 0 || tr == 7)) {
        g->board[tr][tc] = color | promotion;
        m.is_promotion = 1;
    } else {
        g->board[tr][tc] = piece;
    }
    g->board[fr][fc] = EMPTY;

    /* Update castling rights */
    if (type == KING) {
        if (color == WHITE) g->castling_rights &= ~(CASTLE_WK|CASTLE_WQ);
        else                g->castling_rights &= ~(CASTLE_BK|CASTLE_BQ);
    }
    if (type == ROOK) {
        if (color == WHITE) {
            if (fr==7 && fc==7) g->castling_rights &= ~CASTLE_WK;
            if (fr==7 && fc==0) g->castling_rights &= ~CASTLE_WQ;
        } else {
            if (fr==0 && fc==7) g->castling_rights &= ~CASTLE_BK;
            if (fr==0 && fc==0) g->castling_rights &= ~CASTLE_BQ;
        }
    }
    /* If rook on target square captured */
    if (tr==7&&tc==7) g->castling_rights &= ~CASTLE_WK;
    if (tr==7&&tc==0) g->castling_rights &= ~CASTLE_WQ;
    if (tr==0&&tc==7) g->castling_rights &= ~CASTLE_BK;
    if (tr==0&&tc==0) g->castling_rights &= ~CASTLE_BQ;

    /* En passant square */
    if (type == PAWN && abs(tr - fr) == 2)
        g->en_passant_col = fc;
    else
        g->en_passant_col = -1;

    /* Half-move clock */
    if (type == PAWN || m.captured != EMPTY)
        g->halfmove_clock = 0;
    else
        g->halfmove_clock++;

    /* Full-move number */
    if (color == BLACK) g->fullmove_number++;

    /* Switch turn */
    g->turn = (color == WHITE) ? BLACK : WHITE;

    /* Record history */
    if (g->history_count < MAX_HISTORY)
        g->history[g->history_count++] = m;

    if (out_move) *out_move = m;
    return 1;
}

void undo_move(GameState *g) {
    if (g->history_count == 0) return;
    Move *m = &g->history[--g->history_count];

    int piece = m->piece;
    int type  = PIECE_TYPE(piece);
    int color = PIECE_COLOR(piece);

    /* Restore turn */
    g->turn = color;

    /* Restore moved piece */
    g->board[m->from_row][m->from_col] = piece;

    /* Restore captured or empty */
    if (m->is_en_passant) {
        g->board[m->to_row][m->to_col] = EMPTY;
        int ep_row = (color == WHITE) ? 4 : 3;
        g->board[ep_row][m->to_col] = m->captured;
    } else {
        g->board[m->to_row][m->to_col] = m->captured;
    }

    /* Undo promotion */
    if (m->is_promotion) {
        g->board[m->from_row][m->from_col] = piece; /* original pawn */
    }

    /* Undo castling: move rook back */
    if (m->is_castling) {
        int r = m->from_row;
        if (m->is_castling == 1) { /* kingside */
            g->board[r][7] = g->board[r][m->from_col+1];
            g->board[r][m->from_col+1] = EMPTY;
        } else { /* queenside */
            g->board[r][0] = g->board[r][m->from_col-1];
            g->board[r][m->from_col-1] = EMPTY;
        }
    }

    /* Restore state */
    g->castling_rights = m->castling_rights_before;
    g->en_passant_col  = m->en_passant_before;
    g->halfmove_clock  = m->halfmove_clock_before;
    if (color == BLACK) g->fullmove_number--;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PGN
 * ═══════════════════════════════════════════════════════════════════════════ */
void pgn_rebuild(UIState *ui, GameState *g) {
    /* Rebuild PGN from scratch using history */
    ui->pgn[0] = '\0';
    ui->pgn_len = 0;

    /* Headers */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char date[32];
    snprintf(date, sizeof(date), "%04d.%02d.%02d",
             t->tm_year+1900, t->tm_mon+1, t->tm_mday);

    char header[512];
    snprintf(header, sizeof(header),
             "[Event \"Chess GUI Game\"]\n"
             "[Site \"Terminal\"]\n"
             "[Date \"%s\"]\n"
             "[Round \"?\"]\n"
             "[White \"?\"]\n"
             "[Black \"?\"]\n"
             "[Result \"%s\"]\n\n",
             date, ui->game_result);

    int hlen = strlen(header);
    if (hlen < MAX_PGN_LEN - 1) {
        memcpy(ui->pgn, header, hlen);
        ui->pgn_len = hlen;
    }

    /* Replay game on a temp board */
    GameState tmp;
    board_set_start();
    GameState saved = g_game;
    /* We need to replay moves stored in g->history */

    /* Temporarily replay moves from initial position */
    GameState replay;
    memset(&replay, 0, sizeof(replay));
    replay.en_passant_col = -1;
    replay.turn = WHITE;
    replay.castling_rights = CASTLE_WK|CASTLE_WQ|CASTLE_BK|CASTLE_BQ;
    replay.fullmove_number = 1;
    /* Set initial board */
    replay.board[0][0]=BLACK_ROOK; replay.board[0][1]=BLACK_KNIGHT;
    replay.board[0][2]=BLACK_BISHOP; replay.board[0][3]=BLACK_QUEEN;
    replay.board[0][4]=BLACK_KING; replay.board[0][5]=BLACK_BISHOP;
    replay.board[0][6]=BLACK_KNIGHT; replay.board[0][7]=BLACK_ROOK;
    for(int c=0;c<8;c++) replay.board[1][c]=BLACK_PAWN;
    replay.board[7][0]=WHITE_ROOK; replay.board[7][1]=WHITE_KNIGHT;
    replay.board[7][2]=WHITE_BISHOP; replay.board[7][3]=WHITE_QUEEN;
    replay.board[7][4]=WHITE_KING; replay.board[7][5]=WHITE_BISHOP;
    replay.board[7][6]=WHITE_KNIGHT; replay.board[7][7]=WHITE_ROOK;
    for(int c=0;c<8;c++) replay.board[6][c]=WHITE_PAWN;

    int col = 0;
    for (int i = 0; i < g->history_count; i++) {
        Move *m = &g->history[i];
        int is_white = (PIECE_COLOR(m->piece) == WHITE);
        int move_num = replay.fullmove_number;

        char token[32] = {0};
        int tpos = 0;
        if (is_white) {
            char num[16];
            snprintf(num, sizeof(num), "%d. ", move_num);
            strcat(token, num);
        }
        strcat(token, m->algebraic);
        strcat(token, " ");

        int tlen = strlen(token);
        if (col + tlen > 76) {
            if (ui->pgn_len < MAX_PGN_LEN - 2) {
                ui->pgn[ui->pgn_len++] = '\n';
                ui->pgn[ui->pgn_len] = '\0';
            }
            col = 0;
        }
        if (ui->pgn_len + tlen < MAX_PGN_LEN - 1) {
            memcpy(ui->pgn + ui->pgn_len, token, tlen);
            ui->pgn_len += tlen;
            ui->pgn[ui->pgn_len] = '\0';
            col += tlen;
        }

        /* Advance replay state */
        make_move(&replay, m->from_row, m->from_col,
                  m->to_row,   m->to_col, m->promotion, NULL);
    }

    /* Result */
    char res[16];
    snprintf(res, sizeof(res), "%s\n", ui->game_result);
    int rlen = strlen(res);
    if (ui->pgn_len + rlen < MAX_PGN_LEN - 1) {
        if (col + rlen > 76) {
            ui->pgn[ui->pgn_len++] = '\n';
            ui->pgn[ui->pgn_len] = '\0';
        }
        memcpy(ui->pgn + ui->pgn_len, res, rlen);
        ui->pgn_len += rlen;
        ui->pgn[ui->pgn_len] = '\0';
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Engine
 * ═══════════════════════════════════════════════════════════════════════════ */
int engine_start(EngineState *e) {
    if (!e->enabled || strlen(e->path) == 0) return 0;

    int to_engine[2], from_engine[2];
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        /* Child */
        dup2(to_engine[0],   STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[0]); close(to_engine[1]);
        close(from_engine[0]); close(from_engine[1]);
        /* Redirect stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        execl(e->path, e->path, NULL);
        _exit(1);
    }

    /* Parent */
    close(to_engine[0]);
    close(from_engine[1]);
    e->pid    = pid;
    e->in_fd  = to_engine[1];
    e->out_fd = from_engine[0];

    /* Set non-blocking */
    int flags = fcntl(e->out_fd, F_GETFL, 0);
    fcntl(e->out_fd, F_SETFL, flags | O_NONBLOCK);

    /* UCI handshake */
    engine_send(e, "uci\n");
    char line[MAX_LINE];
    int tries = 0;
    e->ready = 0;
    while (tries < 100) {
        if (engine_readline(e, line, 50) > 0) {
            if (strncmp(line, "uciok", 5) == 0) {
                e->ready = 1;
                break;
            }
        }
        tries++;
    }

    if (!e->ready) {
        engine_stop(e);
        return 0;
    }

    engine_send(e, "isready\n");
    tries = 0;
    while (tries < 100) {
        if (engine_readline(e, line, 50) > 0) {
            if (strncmp(line, "readyok", 7) == 0) break;
        }
        tries++;
    }

    return 1;
}

void engine_stop(EngineState *e) {
    if (e->pid > 0) {
        engine_send(e, "quit\n");
        usleep(100000);
        kill(e->pid, SIGTERM);
        waitpid(e->pid, NULL, WNOHANG);
        e->pid = 0;
    }
    if (e->in_fd  > 0) { close(e->in_fd);  e->in_fd  = 0; }
    if (e->out_fd > 0) { close(e->out_fd); e->out_fd = 0; }
    e->ready = 0;
}

void engine_send(EngineState *e, const char *fmt, ...) {
    if (e->in_fd <= 0) return;
    char buf[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(e->in_fd, buf, strlen(buf));
}

int engine_readline(EngineState *e, char *buf, int timeout_ms) {
    if (e->out_fd <= 0) return -1;
    fd_set fds;
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    FD_ZERO(&fds);
    FD_SET(e->out_fd, &fds);
    int sel = select(e->out_fd + 1, &fds, NULL, NULL, &tv);
    if (sel <= 0) return 0;

    int pos = 0;
    while (pos < MAX_LINE - 1) {
        char c;
        int n = read(e->out_fd, &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

void engine_new_game(EngineState *e) {
    if (!e->ready) return;
    engine_send(e, "ucinewgame\n");
    engine_send(e, "isready\n");
    char line[MAX_LINE];
    int tries = 0;
    while (tries < 100) {
        if (engine_readline(e, line, 50) > 0) {
            if (strncmp(line, "readyok", 7) == 0) break;
        }
        tries++;
    }
}

void engine_set_position(EngineState *e, GameState *g) {
    if (!e->ready) return;
    char cmd[MAX_LINE];
    int pos = 0;
    pos += snprintf(cmd + pos, sizeof(cmd) - pos, "position startpos");
    if (g->history_count > 0) {
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " moves");
        for (int i = 0; i < g->history_count; i++) {
            pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                            " %s", g->history[i].uci);
        }
    }
    pos += snprintf(cmd + pos, sizeof(cmd) - pos, "\n");
    engine_send(e, "%s", cmd);
}

void engine_go(EngineState *e) {
    if (!e->ready) return;
    switch (e->tc_mode) {
        case TC_MOVETIME:
            engine_send(e, "go movetime %d\n", e->movetime);
            break;
        case TC_DEPTH:
            engine_send(e, "go depth %d\n", e->depth);
            break;
        case TC_NODES:
            engine_send(e, "go nodes %lld\n", e->nodes);
            break;
    }
}

int engine_wait_bestmove(EngineState *e, char *best, int timeout_s) {
    if (!e->ready) return 0;
    char line[MAX_LINE];
    time_t start = time(NULL);
    while (time(NULL) - start < timeout_s) {
        if (engine_readline(e, line, 100) > 0) {
            if (strncmp(line, "bestmove", 8) == 0) {
                /* Parse: "bestmove e2e4 ponder e7e5" */
                char *sp = strchr(line, ' ');
                if (sp) {
                    sp++;
                    int i = 0;
                    while (*sp && *sp != ' ' && i < 5)
                        best[i++] = *sp++;
                    best[i] = '\0';
                    return 1;
                }
            }
        }
    }
    return 0;
}

void engine_parse_bestmove(const char *best, int *fr, int *fc,
                           int *tr, int *tc, int *promo) {
    /* "e2e4" or "e7e8q" */
    if (strlen(best) < 4) return;
    *fc = best[0] - 'a';
    *fr = 7 - (best[1] - '1');
    *tc = best[2] - 'a';
    *tr = 7 - (best[3] - '1');
    *promo = 0;
    if (strlen(best) >= 5) {
        switch (tolower(best[4])) {
            case 'q': *promo = QUEEN;  break;
            case 'r': *promo = ROOK;   break;
            case 'b': *promo = BISHOP; break;
            case 'n': *promo = KNIGHT; break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Status message
 * ═══════════════════════════════════════════════════════════════════════════ */
void ui_set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ui.status_msg, sizeof(g_ui.status_msg), fmt, ap);
    va_end(ap);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * UI Drawing
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Layout constants (terminal rows/cols) */
#define BOARD_TOP      3
#define BOARD_LEFT     4
#define CELL_W         5   /* chars per cell */
#define CELL_H         2   /* rows per cell  */
#define INFO_COL       50
#define PGN_START_ROW  3

void ui_draw_board(void) {
    GameState *g = &g_game;
    UIState   *ui = &g_ui;

    /* Column labels */
    cursor_goto(BOARD_TOP - 1, BOARD_LEFT + 2);
    printf(ANSI_BOLD FG_BRIGHT_WHITE);
    for (int c = 0; c < 8; c++) {
        int dc = ui->flipped ? (7 - c) : c;
        printf("  %c  ", 'a' + dc);
    }
    printf(ANSI_RESET);

    for (int vr = 0; vr < 8; vr++) {
        int r = ui->flipped ? vr : (7 - vr);
        /* Rank label - top of cell */
        cursor_goto(BOARD_TOP + vr * CELL_H, BOARD_LEFT - 2);
        printf(ANSI_BOLD FG_BRIGHT_WHITE "%d" ANSI_RESET, r + 1);

        for (int ch = 0; ch < CELL_H; ch++) {
            cursor_goto(BOARD_TOP + vr * CELL_H + ch, BOARD_LEFT);
            for (int vc = 0; vc < 8; vc++) {
                int c = ui->flipped ? (7 - vc) : vc;

                /* Determine cell color */
                int is_light = (r + c) % 2 == 0;

                /* Determine highlights */
                int is_cursor   = (r == ui->cursor_row && c == ui->cursor_col);
                int is_selected = ui->has_selection &&
                                  (r == ui->selected_row && c == ui->selected_col);
                int is_target   = ui->has_selection && ui->valid_targets[r][c];

                /* Check if king is in check */
                int piece = g->board[r][c];
                int is_check_king = 0;
                if (PIECE_TYPE(piece) == KING &&
                    PIECE_COLOR(piece) == g->turn &&
                    is_in_check(g, g->turn))
                    is_check_king = 1;

                /* Last move highlight */
                int is_last_from = 0, is_last_to = 0;
                if (g->history_count > 0) {
                    Move *lm = &g->history[g->history_count - 1];
                    is_last_from = (r == lm->from_row && c == lm->from_col);
                    is_last_to   = (r == lm->to_row   && c == lm->to_col);
                }

                /* Pick background */
                const char *bg;
                if (is_check_king) {
                    bg = BG_BRIGHT_RED;
                } else if (is_cursor && is_selected) {
                    bg = BG_BRIGHT_GREEN;
                } else if (is_cursor) {
                    bg = BG_CYAN;
                } else if (is_selected) {
                    bg = BG_GREEN;
                } else if (is_target) {
                    bg = is_light ? BG_BRIGHT_YELLOW : BG_YELLOW;
                } else if (is_last_from || is_last_to) {
                    bg = is_light ? "\033[48;2;205;210;106m"  /* yellowish */
                                  : "\033[48;2;170;162;58m";
                } else {
                    bg = is_light ? BG_BRIGHT_WHITE : BG_BRIGHT_BLACK;
                }

                /* Pick foreground for piece */
                const char *fg;
                if (IS_WHITE(piece))
                    fg = FG_BRIGHT_WHITE;
                else if (IS_BLACK(piece))
                    fg = FG_BLACK;
                else
                    fg = FG_BRIGHT_WHITE;

                printf("%s%s", bg, fg);

                if (ch == 0) {
                    /* Top row of cell: dot for valid target */
                    if (is_target && IS_EMPTY(piece)) {
                        printf("  " ANSI_BOLD "●" ANSI_RESET "%s%s  ", bg, fg);
                    } else {
                        printf("     ");
                    }
                } else {
                    /* Bottom row: piece glyph centered */
                    if (!IS_EMPTY(piece)) {
                        const char *glyph = piece_glyph(piece);
                        printf(" " ANSI_BOLD "%s%s%s " ANSI_RESET "%s%s",
                               fg, glyph, ANSI_RESET, bg, fg);
                        printf("  ");
                    } else if (is_target) {
                        /* Capture target dot */
                        printf("     ");
                    } else {
                        printf("     ");
                    }
                }
                printf(ANSI_RESET);
            }
            /* Right rank label */
            printf(" ");
            if (ch == 0) {
                printf(ANSI_BOLD FG_BRIGHT_WHITE "%d" ANSI_RESET, r + 1);
            }
        }
    }

    /* Bottom column labels */
    cursor_goto(BOARD_TOP + 8 * CELL_H, BOARD_LEFT + 2);
    printf(ANSI_BOLD FG_BRIGHT_WHITE);
    for (int c = 0; c < 8; c++) {
        int dc = ui->flipped ? (7 - c) : c;
        printf("  %c  ", 'a' + dc);
    }
    printf(ANSI_RESET);
}

void ui_draw_info(void) {
    UIState *ui = &g_ui;
    GameState *g = &g_game;
    int row = PGN_START_ROW;
    int col = INFO_COL;

    /* Engine info */
    cursor_goto(row++, col);
    printf(ANSI_BOLD FG_CYAN "┌─ ENGINE ─────────────────────────┐" ANSI_RESET);

    cursor_goto(row++, col);
    if (g_engine.enabled && g_engine.ready) {
        printf(FG_GREEN "│ %-35s│" ANSI_RESET,
               "● ACTIVE");
    } else if (g_engine.enabled) {
        printf(FG_RED "│ %-35s│" ANSI_RESET,
               "✗ NOT CONNECTED");
    } else {
        printf(FG_BRIGHT_BLACK "│ %-35s│" ANSI_RESET,
               "○ DISABLED");
    }

    cursor_goto(row++, col);
    char pathdisp[36] = {0};
    if (strlen(g_engine.path) > 0) {
        /* Show just the filename */
        const char *base = strrchr(g_engine.path, '/');
        base = base ? base + 1 : g_engine.path;
        snprintf(pathdisp, sizeof(pathdisp), "Engine: %.28s", base);
    } else {
        strcpy(pathdisp, "Engine: (none)");
    }
    printf("│ " FG_WHITE "%-33s" ANSI_RESET "  │", pathdisp);

    cursor_goto(row++, col);
    char tcstr[40];
    switch (g_engine.tc_mode) {
        case TC_MOVETIME:
            snprintf(tcstr, sizeof(tcstr), "TC: %dms/move", g_engine.movetime);
            break;
        case TC_DEPTH:
            snprintf(tcstr, sizeof(tcstr), "TC: depth %d", g_engine.depth);
            break;
        case TC_NODES:
            snprintf(tcstr, sizeof(tcstr), "TC: %lld nodes", g_engine.nodes);
            break;
    }
    printf("│ " FG_WHITE "%-33s" ANSI_RESET "  │", tcstr);

    cursor_goto(row++, col);
    char playas[40];
    if (g_engine.enabled) {
        snprintf(playas, sizeof(playas), "Plays: %s",
                 g_engine.plays_as == WHITE ? "White" : "Black");
    } else {
        strcpy(playas, "Plays: N/A");
    }
    printf("│ " FG_WHITE "%-33s" ANSI_RESET "  │", playas);

    cursor_goto(row++, col);
    printf(FG_CYAN "└───────────────────────────────────┘" ANSI_RESET);

    /* Game info box */
    row++;
    cursor_goto(row++, col);
    printf(ANSI_BOLD FG_YELLOW "┌─ GAME ───────────────────────────┐" ANSI_RESET);

    cursor_goto(row++, col);
    const char *turn_str = (g->turn == WHITE) ? "White" : "Black";
    char turndisp[40];
    if (g_ui.game_over) {
        snprintf(turndisp, sizeof(turndisp), "Result: %s", g_ui.game_result);
    } else {
        snprintf(turndisp, sizeof(turndisp), "Turn: %s to move", turn_str);
    }
    printf("│ " FG_WHITE "%-33s" ANSI_RESET "  │", turndisp);

    cursor_goto(row++, col);
    char movenum[40];
    snprintf(movenum, sizeof(movenum), "Move: %d  Half: %d",
             g->fullmove_number, g->halfmove_clock);
    printf("│ " FG_WHITE "%-33s" ANSI_RESET "  │", movenum);

    cursor_goto(row++, col);
    char checks[40] = {0};
    if (is_in_check(g, g->turn)) {
        snprintf(checks, sizeof(checks), FG_RED ANSI_BOLD "CHECK!" ANSI_RESET);
        printf("│ %-44s│", checks);
    } else {
        printf("│ %-35s│", "");
    }

    cursor_goto(row++, col);
    printf(FG_YELLOW "└───────────────────────────────────┘" ANSI_RESET);

    /* Controls box */
    row++;
    cursor_goto(row++, col);
    printf(ANSI_BOLD FG_MAGENTA "┌─ CONTROLS ───────────────────────┐" ANSI_RESET);
    cursor_goto(row++, col);
    printf("│ " FG_WHITE "Arrows" ANSI_RESET "  Move cursor              │");
    cursor_goto(row++, col);
    printf("│ " FG_WHITE "Enter/Spc" ANSI_RESET " Select / Move           │");
    cursor_goto(row++, col);
    printf("│ " FG_WHITE "U" ANSI_RESET "       Undo (takeback)           │");
    cursor_goto(row++, col);
    printf("│ " FG_WHITE "F" ANSI_RESET "       Flip board                │");
    cursor_goto(row++, col);
    printf("│ " FG_WHITE "N" ANSI_RESET "       New game                  │");
    cursor_goto(row++, col);
    printf("│ " FG_WHITE "E" ANSI_RESET "       Engine settings           │");
    cursor_goto(row++, col);
    printf("│ " FG_WHITE "H" ANSI_RESET "       Help / PGN export         │");
    cursor_goto(row++, col);
    printf("│ " FG_WHITE "Q" ANSI_RESET "       Quit                      │");
    cursor_goto(row++, col);
    printf(FG_MAGENTA "└───────────────────────────────────┘" ANSI_RESET);
}

void ui_draw_pgn(void) {
    /* Draw PGN in a scrollable area below the board */
    int start_row = BOARD_TOP + 8 * CELL_H + 2;
    int max_lines = 6;
    int col = BOARD_LEFT - 2;

    cursor_goto(start_row, col);
    printf(ANSI_BOLD FG_GREEN "── PGN ─────────────────────────────────────────────" ANSI_RESET);

    /* Build display lines from PGN moves only (skip headers) */
    /* Find the blank line after headers */
    const char *pgn = g_ui.pgn;
    const char *moves_start = strstr(pgn, "\n\n");
    if (moves_start) moves_start += 2;
    else              moves_start = pgn;

    /* Split into display lines */
    char lines[10][80];
    int  nlines = 0;
    int  lpos = 0;
    const char *p = moves_start;

    for (int ln = 0; ln < 10; ln++)
        lines[ln][0] = '\0';

    while (*p && nlines < 9) {
        if (*p == '\n') {
            lines[nlines][lpos] = '\0';
            if (lpos > 0) nlines++;
            lpos = 0;
        } else if (lpos < 78) {
            lines[nlines][lpos++] = *p;
        }
        p++;
    }
    if (lpos > 0) {
        lines[nlines][lpos] = '\0';
        nlines++;
    }

    /* Show last max_lines lines */
    int start_ln = (nlines > max_lines) ? nlines - max_lines : 0;
    for (int i = 0; i < max_lines; i++) {
        cursor_goto(start_row + 1 + i, col);
        printf(CLEAR_LINE);
        if (start_ln + i < nlines) {
            printf(FG_WHITE "%-54s" ANSI_RESET, lines[start_ln + i]);
        } else {
            printf("%-54s", "");
        }
    }
}

void ui_draw_status(void) {
    int row = BOARD_TOP + 8 * CELL_H + 9;
    cursor_goto(row, BOARD_LEFT - 2);
    printf(CLEAR_LINE);
    printf(ANSI_BOLD FG_BRIGHT_WHITE "Status: " ANSI_RESET
           FG_CYAN "%-60s" ANSI_RESET, g_ui.status_msg);
}

void ui_draw(void) {
    printf(CLEAR_SCREEN CURSOR_HOME);

    /* Title bar */
    cursor_goto(1, 1);
    printf(ANSI_BOLD BG_BLUE FG_BRIGHT_WHITE
           "  ♔  TERMINAL CHESS  ─  UCI ENGINE INTERFACE  ♚  "
           ANSI_RESET);

    ui_draw_board();
    ui_draw_info();
    ui_draw_pgn();
    ui_draw_status();

    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Promotion Dialog
 * ═══════════════════════════════════════════════════════════════════════════ */
static int ui_promotion_dialog(int color) {
    int start_row = BOARD_TOP + 3;
    int start_col = INFO_COL;

    cursor_goto(start_row, start_col);
    printf(ANSI_BOLD FG_YELLOW "┌─ PROMOTION ──────────────────────┐" ANSI_RESET);
    cursor_goto(start_row+1, start_col);
    printf("│  Choose promotion piece:         │");
    cursor_goto(start_row+2, start_col);
    int qp = (color==WHITE) ? WHITE_QUEEN  : BLACK_QUEEN;
    int rp = (color==WHITE) ? WHITE_ROOK   : BLACK_ROOK;
    int bp = (color==WHITE) ? WHITE_BISHOP : BLACK_BISHOP;
    int np = (color==WHITE) ? WHITE_KNIGHT : BLACK_KNIGHT;
    printf("│  Q=%s  R=%s  B=%s  N=%s    │",
           piece_glyph(qp), piece_glyph(rp),
           piece_glyph(bp), piece_glyph(np));
    cursor_goto(start_row+3, start_col);
    printf(FG_YELLOW "└───────────────────────────────────┘" ANSI_RESET);
    fflush(stdout);

    while (1) {
        int key = term_read_key();
        switch (toupper(key)) {
            case 'Q': return QUEEN;
            case 'R': return ROOK;
            case 'B': return BISHOP;
            case 'N': return KNIGHT;
            case KEY_ESC: return QUEEN; /* default */
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Game Logic - Human Move
 * ═══════════════════════════════════════════════════════════════════════════ */
void ui_check_game_over(void) {
    GameState *g = &g_game;
    if (is_checkmate(g)) {
        const char *winner = (g->turn == WHITE) ? "Black" : "White";
        g_ui.game_over = 1;
        strcpy(g_ui.game_result, (g->turn == WHITE) ? "0-1" : "1-0");
        ui_set_status("Checkmate! %s wins!", winner);
    } else if (is_stalemate(g)) {
        g_ui.game_over = 1;
        strcpy(g_ui.game_result, "1/2-1/2");
        ui_set_status("Stalemate! Draw.");
    } else if (is_insufficient_material(g)) {
        g_ui.game_over = 1;
        strcpy(g_ui.game_result, "1/2-1/2");
        ui_set_status("Insufficient material. Draw.");
    } else if (is_fifty_move(g)) {
        g_ui.game_over = 1;
        strcpy(g_ui.game_result, "1/2-1/2");
        ui_set_status("50-move rule. Draw.");
    }
    if (g_ui.game_over) {
        pgn_rebuild(&g_ui, &g_game);
    }
}

void ui_select_or_move(void) {
    UIState *ui = &g_ui;
    GameState *g = &g_game;
    int r = ui->cursor_row;
    int c = ui->cursor_col;

    if (ui->game_over) {
        ui_set_status("Game over: %s", ui->game_result);
        return;
    }

    /* If engine's turn, ignore human input */
    if (g_engine.enabled && g_engine.ready &&
        g->turn == g_engine.plays_as) {
        ui_set_status("Engine is thinking...");
        return;
    }

    if (!ui->has_selection) {
        /* Select a piece */
        int piece = g->board[r][c];
        if (IS_EMPTY(piece)) {
            ui_set_status("Empty square.");
            return;
        }
        if (PIECE_COLOR(piece) != g->turn) {
            ui_set_status("Not your piece.");
            return;
        }
        int count = generate_legal_moves(g, r, c, ui->valid_targets);
        if (count == 0) {
            ui_set_status("No legal moves for this piece.");
            return;
        }
        ui->selected_row = r;
        ui->selected_col = c;
        ui->has_selection = 1;
        ui_set_status("Selected %s on %c%d. %d moves.",
                      piece_glyph(piece), 'a'+c, r+1, count);
    } else {
        /* Try to move */
        if (r == ui->selected_row && c == ui->selected_col) {
            /* Deselect */
            ui->has_selection = 0;
            memset(ui->valid_targets, 0, sizeof(ui->valid_targets));
            ui_set_status("Deselected.");
            return;
        }

        /* Check if target is valid */
        if (!ui->valid_targets[r][c]) {
            /* Maybe selecting a different own piece */
            int piece = g->board[r][c];
            if (!IS_EMPTY(piece) && PIECE_COLOR(piece) == g->turn) {
                ui->has_selection = 0;
                memset(ui->valid_targets, 0, sizeof(ui->valid_targets));
                /* Re-select new piece */
                int count = generate_legal_moves(g, r, c, ui->valid_targets);
                if (count > 0) {
                    ui->selected_row = r;
                    ui->selected_col = c;
                    ui->has_selection = 1;
                    ui_set_status("Selected %s on %c%d.",
                                  piece_glyph(piece), 'a'+c, r+1);
                } else {
                    ui_set_status("No legal moves.");
                }
            } else {
                ui_set_status("Invalid move.");
            }
            return;
        }

        /* Check for promotion */
        int promotion = 0;
        int piece = g->board[ui->selected_row][ui->selected_col];
        if (PIECE_TYPE(piece) == PAWN && (r == 0 || r == 7)) {
            promotion = ui_promotion_dialog(PIECE_COLOR(piece));
        }

        Move m;
        make_move(g, ui->selected_row, ui->selected_col,
                  r, c, promotion, &m);

        ui->has_selection = 0;
        memset(ui->valid_targets, 0, sizeof(ui->valid_targets));

        /* Rebuild PGN */
        pgn_rebuild(ui, g);

        ui_check_game_over();
        if (!ui->game_over) {
            if (is_in_check(g, g->turn))
                ui_set_status("Check! %s: %s",
                              m.algebraic,
                              (g->turn==WHITE)?"White":"Black");
            else
                ui_set_status("Move: %s", m.algebraic);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Engine Move
 * ═══════════════════════════════════════════════════════════════════════════ */
void ui_do_engine_move(void) {
    if (!g_engine.enabled || !g_engine.ready) return;
    if (g_game.turn != g_engine.plays_as) return;
    if (g_ui.game_over) return;

    ui_set_status("Engine thinking...");
    ui_draw();

    engine_set_position(&g_engine, &g_game);
    engine_go(&g_engine);

    char best[8] = {0};
    int timeout = ENGINE_TIMEOUT;
    /* Adjust timeout based on TC */
    if (g_engine.tc_mode == TC_MOVETIME)
        timeout = g_engine.movetime / 1000 + 5;
    else if (g_engine.tc_mode == TC_DEPTH)
        timeout = 60;

    if (!engine_wait_bestmove(&g_engine, best, timeout)) {
        ui_set_status("Engine timeout or error.");
        return;
    }

    if (strlen(best) < 4 || strcmp(best, "0000") == 0) {
        ui_set_status("Engine resigned or no move.");
        return;
    }

    int fr, fc, tr, tc, promo;
    engine_parse_bestmove(best, &fr, &fc, &tr, &tc, &promo);

    /* Validate */
    int targets[8][8];
    generate_legal_moves(&g_game, fr, fc, targets);
    if (!targets[tr][tc]) {
        ui_set_status("Engine gave illegal move: %s", best);
        return;
    }

    Move m;
    make_move(&g_game, fr, fc, tr, tc, promo, &m);
    pgn_rebuild(&g_ui, &g_game);
    ui_check_game_over();
    if (!g_ui.game_over)
        ui_set_status("Engine: %s", m.algebraic);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * New Game
 * ═══════════════════════════════════════════════════════════════════════════ */
void ui_new_game(void) {
    board_init();
    if (g_engine.enabled && g_engine.ready) {
        engine_new_game(&g_engine);
    }
    ui_set_status("New game started.");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Settings Menu
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Simple line input (raw mode, visible) */
static int read_line_input(char *buf, int maxlen) {
    /* Temporarily restore some terminal settings for input */
    struct termios cooked = g_orig_termios;
    cooked.c_lflag |= (ECHO | ICANON);
    cooked.c_cc[VMIN]  = 1;
    cooked.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &cooked);
    printf(SHOW_CURSOR);
    fflush(stdout);

    int n = 0;
    int c;
    while (n < maxlen - 1) {
        c = fgetc(stdin);
        if (c == '\n' || c == EOF) break;
        if (c == 127 || c == '\b') {
            if (n > 0) { printf("\b \b"); fflush(stdout); n--; }
            continue;
        }
        buf[n++] = (char)c;
        printf("%c", c); fflush(stdout);
    }
    buf[n] = '\0';

    printf(HIDE_CURSOR);
    term_raw();
    return n;
}

void ui_configure_engine(void) {
    printf(CLEAR_SCREEN CURSOR_HOME);
    printf(ANSI_BOLD FG_CYAN
           "╔══════════════════════════════════════════╗\n"
           "║         ENGINE CONFIGURATION             ║\n"
           "╚══════════════════════════════════════════╝\n"
           ANSI_RESET "\n");

    printf(FG_WHITE "Current engine path: " FG_YELLOW "%s\n" ANSI_RESET,
           strlen(g_engine.path) ? g_engine.path : "(none)");
    printf(FG_WHITE "Engine enabled: " FG_YELLOW "%s\n\n" ANSI_RESET,
           g_engine.enabled ? "Yes" : "No");

    printf(FG_BRIGHT_WHITE
           "1. Set engine path\n"
           "2. Enable/Disable engine\n"
           "3. Set time control\n"
           "4. Set engine color (White/Black)\n"
           "5. Test connection\n"
           "6. Back\n\n" ANSI_RESET);

    printf(FG_CYAN "Choice: " ANSI_RESET);
    fflush(stdout);

    char choice[4];
    read_line_input(choice, sizeof(choice));
    printf("\n");

    switch (choice[0]) {
    case '1': {
        printf(FG_CYAN "\nEngine path (e.g. /usr/local/bin/stockfish):\n> " ANSI_RESET);
        fflush(stdout);
        char path[MAX_ENGINE_PATH];
        read_line_input(path, sizeof(path));
        printf("\n");
        if (strlen(path) > 0) {
            if (g_engine.pid > 0) engine_stop(&g_engine);
            strncpy(g_engine.path, path, MAX_ENGINE_PATH - 1);
        }
        break;
    }
    case '2': {
        if (g_engine.enabled) {
            g_engine.enabled = 0;
            engine_stop(&g_engine);
            printf(FG_RED "\nEngine disabled.\n" ANSI_RESET);
        } else {
            g_engine.enabled = 1;
            printf(FG_GREEN "\nEngine enabled.\n" ANSI_RESET);
            if (strlen(g_engine.path) > 0) {
                printf("Starting engine...\n"); fflush(stdout);
                if (engine_start(&g_engine)) {
                    printf(FG_GREEN "Engine connected!\n" ANSI_RESET);
                    engine_new_game(&g_engine);
                } else {
                    printf(FG_RED "Failed to start engine.\n" ANSI_RESET);
                    g_engine.enabled = 0;
                }
            }
        }
        printf("\nPress Enter..."); fflush(stdout);
        char tmp[4]; read_line_input(tmp, sizeof(tmp));
        break;
    }
    case '3': {
        printf(FG_CYAN
               "\nTime control mode:\n"
               "1. Move time (ms)\n"
               "2. Depth\n"
               "3. Nodes\n"
               "> " ANSI_RESET);
        fflush(stdout);
        char tc[4];
        read_line_input(tc, sizeof(tc));
        printf("\n");
        if (tc[0] == '1') {
            g_engine.tc_mode = TC_MOVETIME;
            printf(FG_CYAN "Move time (ms, e.g. 1000): " ANSI_RESET);
            fflush(stdout);
            char val[16];
            read_line_input(val, sizeof(val));
            printf("\n");
            int v = atoi(val);
            if (v > 0) g_engine.movetime = v;
        } else if (tc[0] == '2') {
            g_engine.tc_mode = TC_DEPTH;
            printf(FG_CYAN "Depth (e.g. 10): " ANSI_RESET);
            fflush(stdout);
            char val[16];
            read_line_input(val, sizeof(val));
            printf("\n");
            int v = atoi(val);
            if (v > 0) g_engine.depth = v;
        } else if (tc[0] == '3') {
            g_engine.tc_mode = TC_NODES;
            printf(FG_CYAN "Nodes (e.g. 1000000): " ANSI_RESET);
            fflush(stdout);
            char val[16];
            read_line_input(val, sizeof(val));
            printf("\n");
            long long v = atoll(val);
            if (v > 0) g_engine.nodes = v;
        }
        break;
    }
    case '4': {
        printf(FG_CYAN "\nEngine plays as (W/B): " ANSI_RESET);
        fflush(stdout);
        char col[4];
        read_line_input(col, sizeof(col));
        printf("\n");
        if (toupper(col[0]) == 'W') g_engine.plays_as = WHITE;
        else                         g_engine.plays_as = BLACK;
        printf(FG_GREEN "Engine will play as %s.\n" ANSI_RESET,
               g_engine.plays_as == WHITE ? "White" : "Black");
        printf("\nPress Enter..."); fflush(stdout);
        char tmp[4]; read_line_input(tmp, sizeof(tmp));
        break;
    }
    case '5': {
        printf(FG_CYAN "\nTesting engine connection...\n" ANSI_RESET);
        fflush(stdout);
        if (g_engine.pid > 0) engine_stop(&g_engine);
        g_engine.ready = 0;
        if (strlen(g_engine.path) == 0) {
            printf(FG_RED "No engine path set.\n" ANSI_RESET);
        } else if (engine_start(&g_engine)) {
            printf(FG_GREEN "Engine connected successfully!\n" ANSI_RESET);
            engine_new_game(&g_engine);
            engine_set_position(&g_engine, &g_game);
        } else {
            printf(FG_RED "Failed to connect to engine at:\n  %s\n" ANSI_RESET,
                   g_engine.path);
        }
        printf("\nPress Enter..."); fflush(stdout);
        char tmp[4]; read_line_input(tmp, sizeof(tmp));
        break;
    }
    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Help Screen
 * ═══════════════════════════════════════════════════════════════════════════ */
void ui_show_help_screen(void) {
    printf(CLEAR_SCREEN CURSOR_HOME);
    printf(ANSI_BOLD FG_CYAN
           "╔══════════════════════════════════════════════════════╗\n"
           "║              TERMINAL CHESS - HELP                  ║\n"
           "╚══════════════════════════════════════════════════════╝\n"
           ANSI_RESET "\n");

    printf(ANSI_BOLD FG_YELLOW "CONTROLS:\n" ANSI_RESET);
    printf("  ← → ↑ ↓      Arrow keys to move cursor around board\n");
    printf("  Enter / Space  Select piece, then select destination\n");
    printf("  U              Undo last move (takeback)\n");
    printf("  F              Flip board orientation\n");
    printf("  N              Start a new game\n");
    printf("  E              Open engine configuration menu\n");
    printf("  H              Show this help screen\n");
    printf("  Q / Ctrl-C     Quit\n\n");

    printf(ANSI_BOLD FG_YELLOW "HOW TO PLAY:\n" ANSI_RESET);
    printf("  1. Use arrow keys to position cursor over your piece\n");
    printf("  2. Press Enter or Space to select it (green highlight)\n");
    printf("  3. Valid moves shown with yellow dots\n");
    printf("  4. Move cursor to destination, press Enter/Space\n");
    printf("  5. For promotion: press Q/R/B/N when prompted\n\n");

    printf(ANSI_BOLD FG_YELLOW "ENGINE SETUP (e.g. Stockfish):\n" ANSI_RESET);
    printf("  1. Install a UCI engine (stockfish, lc0, komodo...)\n");
    printf("  2. Press E to open engine settings\n");
    printf("  3. Set engine path (e.g. /usr/local/bin/stockfish)\n");
    printf("  4. Enable engine and configure time control\n");
    printf("  5. Choose which color the engine plays\n");
    printf("  6. Press N to start a new game\n\n");
    printf("  Install Stockfish: brew install stockfish\n");
    printf("  Engine path:       /opt/homebrew/bin/stockfish\n\n");

    printf(ANSI_BOLD FG_YELLOW "PGN:\n" ANSI_RESET);
    printf("  PGN is shown below the board and updated live.\n");
    printf("  You can copy it from the terminal.\n\n");

    printf(ANSI_BOLD FG_YELLOW "CURRENT PGN:\n" ANSI_RESET FG_WHITE);
    if (strlen(g_ui.pgn) > 0) {
        printf("%s\n", g_ui.pgn);
    } else {
        printf("(no moves yet)\n");
    }
    printf(ANSI_RESET);

    printf(FG_CYAN "\nPress any key to return..." ANSI_RESET);
    fflush(stdout);

    /* Wait for key */
    term_raw();
    while (term_read_key() < 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Key Handler
 * ═══════════════════════════════════════════════════════════════════════════ */
void ui_handle_key(int key) {
    UIState *ui = &g_ui;
    GameState *g = &g_game;

    switch (key) {
    case KEY_UP:
        if (!ui->flipped) {
            if (ui->cursor_row < 7) ui->cursor_row++;
        } else {
            if (ui->cursor_row > 0) ui->cursor_row--;
        }
        break;
    case KEY_DOWN:
        if (!ui->flipped) {
            if (ui->cursor_row > 0) ui->cursor_row--;
        } else {
            if (ui->cursor_row < 7) ui->cursor_row++;
        }
        break;
    case KEY_LEFT:
        if (!ui->flipped) {
            if (ui->cursor_col > 0) ui->cursor_col--;
        } else {
            if (ui->cursor_col < 7) ui->cursor_col++;
        }
        break;
    case KEY_RIGHT:
        if (!ui->flipped) {
            if (ui->cursor_col < 7) ui->cursor_col++;
        } else {
            if (ui->cursor_col > 0) ui->cursor_col--;
        }
        break;
    case KEY_ENTER:
    case KEY_SPACE:
        ui_select_or_move();
        break;
    case 'u':
    case 'U':
        if (g->history_count == 0) {
            ui_set_status("Nothing to undo.");
        } else {
            /* Undo engine move too if it played last */
            if (g_engine.enabled && g_engine.ready &&
                g->history_count >= 2) {
                Move *last = &g->history[g->history_count-1];
                if (PIECE_COLOR(last->piece) == g_engine.plays_as) {
                    undo_move(g);
                }
            }
            undo_move(g);
            ui->has_selection = 0;
            memset(ui->valid_targets, 0, sizeof(ui->valid_targets));
            ui->game_over = 0;
            strcpy(ui->game_result, "*");
            pgn_rebuild(ui, g);
            ui_set_status("Move undone.");
        }
        break;
    case 'f':
    case 'F':
        ui->flipped = !ui->flipped;
        ui_set_status("Board %s.", ui->flipped ? "flipped" : "normal");
        break;
    case 'n':
    case 'N': {
        /* Confirm */
        ui_set_status("Press N again to confirm new game...");
        ui_draw();
        int k2 = KEY_ESC;
        time_t st = time(NULL);
        while (time(NULL) - st < 3) {
            int k = term_read_key();
            if (k == 'n' || k == 'N') { k2 = k; break; }
            if (k > 0 && k != -1) break;
        }
        if (k2 == 'n' || k2 == 'N') ui_new_game();
        else ui_set_status("New game cancelled.");
        break;
    }
    case 'e':
    case 'E':
        ui_configure_engine();
        break;
    case 'h':
    case 'H':
        ui_show_help_screen();
        break;
    case 'q':
    case 'Q':
    case KEY_CTRL_C:
        g_running = 0;
        break;
    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Signal Handler
 * ═══════════════════════════════════════════════════════════════════════════ */
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize engine defaults */
    memset(&g_engine, 0, sizeof(g_engine));
    g_engine.enabled   = 0;
    g_engine.tc_mode   = TC_MOVETIME;
    g_engine.movetime  = 1000;
    g_engine.depth     = 10;
    g_engine.nodes     = 1000000;
    g_engine.plays_as  = BLACK;
    g_engine.path[0]   = '\0';
    g_engine.pid       = 0;
    g_engine.in_fd     = 0;
    g_engine.out_fd    = 0;
    g_engine.ready     = 0;

    /* Check for common stockfish locations */
    const char *common_paths[] = {
        "/opt/homebrew/bin/stockfish",
        "/usr/local/bin/stockfish",
        "/usr/bin/stockfish",
        NULL
    };
    for (int i = 0; common_paths[i]; i++) {
        if (access(common_paths[i], X_OK) == 0) {
            strncpy(g_engine.path, common_paths[i], MAX_ENGINE_PATH-1);
            break;
        }
    }

    board_init();
    term_init();
    pgn_rebuild(&g_ui, &g_game);
    ui_draw();

    /* If stockfish found, ask to enable */
    if (strlen(g_engine.path) > 0) {
        ui_set_status("Stockfish found at %s. Press E to configure.",
                      g_engine.path);
        ui_draw();
    }

    /* Main loop */
    while (g_running) {
        /* Check if engine should move */
        if (g_engine.enabled && g_engine.ready &&
            g_game.turn == g_engine.plays_as && !g_ui.game_over) {
            ui_do_engine_move();
            ui_draw();
            continue;
        }

        int key = term_read_key();
        if (key > 0) {
            ui_handle_key(key);
            ui_draw();
        }
    }

    /* Cleanup */
    printf(CLEAR_SCREEN CURSOR_HOME);
    if (g_engine.pid > 0) {
        engine_stop(&g_engine);
    }

    /* Print final PGN */
    printf(ANSI_BOLD FG_GREEN "Final PGN:\n" ANSI_RESET);
    printf("%s\n", g_ui.pgn);

    printf(ANSI_BOLD FG_CYAN "\nThanks for playing!\n" ANSI_RESET);
    return 0;
}
