/*
 * chess_gui.c - Terminal Chess GUI for Mac
 *
 * Compile: gcc -o chess_gui chess_gui.c
 * Run:     ./chess_gui [uci_engine_path]
 *
 * Controls:
 *   Arrow Keys  - Move cursor
 *   Enter/Space - Select piece / Move piece
 *   U           - Undo last move (takeback)
 *   Q           - Quit
 *   R           - Restart game
 *   F           - Flip board
 *
 * UCI Engine (optional):
 *   ./chess_gui /path/to/stockfish
 *   Engine plays as Black by default.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

/* ─────────────────────────────────────────────
 *  CONSTANTS & TYPES
 * ───────────────────────────────────────────── */

#define BOARD_SIZE   8
#define MAX_MOVES    512
#define MAX_PGN      65536
#define MAX_HIST     1024
#define ENGINE_BUF   4096

/* Piece encoding: positive = White, negative = Black, 0 = empty */
#define EMPTY   0
#define PAWN    1
#define KNIGHT  2
#define BISHOP  3
#define ROOK    4
#define QUEEN   5
#define KING    6

/* Colors */
#define WHITE_SIDE  1
#define BLACK_SIDE -1

/* ANSI color codes */
#define ANSI_RESET       "\033[0m"
#define ANSI_BOLD        "\033[1m"
#define ANSI_DIM         "\033[2m"

/* 256-color backgrounds */
#define BG_LIGHT_SQ      "\033[48;5;223m"   /* wheat */
#define BG_DARK_SQ       "\033[48;5;130m"   /* brown */
#define BG_SELECTED      "\033[48;5;226m"   /* bright yellow */
#define BG_VALID_MOVE    "\033[48;5;154m"   /* lime green */
#define BG_LAST_MOVE     "\033[48;5;215m"   /* orange */
#define BG_CHECK         "\033[48;5;196m"   /* red */
#define BG_CURSOR        "\033[48;5;51m"    /* cyan */

/* Foreground */
#define FG_WHITE_PIECE   "\033[38;5;255m"   /* bright white */
#define FG_BLACK_PIECE   "\033[38;5;16m"    /* near black */
#define FG_LABEL         "\033[38;5;245m"

/* UI Panel */
#define FG_PANEL         "\033[38;5;252m"
#define FG_HEADER        "\033[38;5;220m"
#define FG_PGN           "\033[38;5;159m"
#define FG_STATUS        "\033[38;5;213m"
#define FG_ERROR         "\033[38;5;196m"
#define FG_ENGINE        "\033[38;5;118m"

typedef struct {
    int board[8][8];          /* piece encoding */
    int turn;                 /* WHITE_SIDE or BLACK_SIDE */
    /* Castling rights: wK, wQ, bK, bQ */
    int castle[4];
    int ep_file;              /* en passant file (-1 if none) */
    int ep_rank;
    int halfmove;             /* 50-move rule counter */
    int fullmove;
} BoardState;

typedef struct {
    int from_r, from_c;
    int to_r,   to_c;
    int piece;                /* piece that moved */
    int captured;             /* piece captured (0 if none) */
    int promotion;            /* promotion piece (0 if none) */
    int castle_side;          /* 0=none,1=kingside,2=queenside */
    int ep_capture;           /* en passant capture flag */
    /* saved state for undo */
    int castle_saved[4];
    int ep_file_saved;
    int ep_rank_saved;
    int halfmove_saved;
    /* PGN token for this move */
    char pgn_token[16];
    /* check/mate flags */
    int gives_check;
    int gives_mate;
} Move;

typedef struct {
    BoardState state;
    Move       move;          /* move that led to this state */
} HistoryEntry;

/* ─────────────────────────────────────────────
 *  GLOBALS
 * ───────────────────────────────────────────── */

static BoardState g_board;
static HistoryEntry g_history[MAX_HIST];
static int g_hist_len = 0;

static int g_cursor_r = 7;   /* cursor row (0=top=rank8 when not flipped) */
static int g_cursor_c = 0;   /* cursor col (0=left=fileA when not flipped) */
static int g_selected = 0;
static int g_sel_r = -1;
static int g_sel_c = -1;

static int g_flipped = 0;    /* 1 = board flipped (black at bottom) */

/* Valid moves for selected piece */
static Move g_valid_moves[MAX_MOVES];
static int  g_valid_count = 0;

/* Last move highlight */
static int g_last_from_r = -1, g_last_from_c = -1;
static int g_last_to_r   = -1, g_last_to_c   = -1;

/* PGN string */
static char g_pgn[MAX_PGN];
static int  g_pgn_len = 0;

/* Game status */
static int g_game_over = 0;
static char g_status_msg[256];

/* Engine */
static int  g_engine_pid  = -1;
static int  g_engine_in   = -1;   /* write to engine */
static int  g_engine_out  = -1;   /* read from engine */
static int  g_engine_side = BLACK_SIDE;
static int  g_engine_thinking = 0;
static char g_engine_name[128] = "No engine";
static char g_engine_move_buf[32];
static int  g_engine_move_ready = 0;
static int  g_engine_enabled = 0;
static int  g_engine_movetime = 1000; /* ms */

/* Terminal */
static struct termios g_old_termios;
static int g_rows = 40, g_cols = 120;

/* ─────────────────────────────────────────────
 *  TERMINAL SETUP
 * ───────────────────────────────────────────── */

static void term_raw_mode(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_old_termios);
    raw = g_old_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old_termios);
}

static void term_clear(void) {
    printf("\033[2J\033[H");
}

static void term_goto(int row, int col) {
    printf("\033[%d;%dH", row + 1, col + 1);
}

static void term_hide_cursor(void) { printf("\033[?25l"); }
static void term_show_cursor(void) { printf("\033[?25h"); }

/* Get terminal size */
static void term_get_size(void) {
    /* Try via escape sequence */
    printf("\033[999;999H\033[6n");
    fflush(stdout);
    char buf[32]; int i = 0;
    /* read response: ESC[rows;colsR */
    struct timeval tv = {0, 100000};
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) > 0) {
        while (i < 31) {
            char c;
            if (read(STDIN_FILENO, &c, 1) != 1) break;
            buf[i++] = c;
            if (c == 'R') break;
        }
        buf[i] = 0;
        int r, c;
        if (sscanf(buf, "\033[%d;%dR", &r, &c) == 2) {
            g_rows = r; g_cols = c;
        }
    }
    printf("\033[H");
}

/* Non-blocking read of one byte */
static int term_read_nb(char *c) {
    struct timeval tv = {0, 0};
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) > 0)
        return (int)read(STDIN_FILENO, c, 1);
    return 0;
}

/* ─────────────────────────────────────────────
 *  ENGINE I/O
 * ───────────────────────────────────────────── */

static void engine_write(const char *s) {
    if (g_engine_in < 0) return;
    write(g_engine_in, s, strlen(s));
}

/* Read a line from engine (non-blocking). Returns 1 if line read. */
static int engine_readline(char *buf, int maxlen) {
    if (g_engine_out < 0) return 0;
    struct timeval tv = {0, 0};
    fd_set fds; FD_ZERO(&fds); FD_SET(g_engine_out, &fds);
    if (select(g_engine_out+1, &fds, NULL, NULL, &tv) <= 0) return 0;

    int i = 0;
    while (i < maxlen - 1) {
        char c;
        int n = (int)read(g_engine_out, &c, 1);
        if (n <= 0) break;
        if (c == '\n') { buf[i] = 0; return 1; }
        buf[i++] = c;
    }
    buf[i] = 0;
    return i > 0 ? 1 : 0;
}

static int engine_start(const char *path) {
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        /* child */
        dup2(pipe_in[0],  STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[1]); close(pipe_out[0]);
        execlp(path, path, NULL);
        exit(1);
    }
    close(pipe_in[0]); close(pipe_out[1]);
    g_engine_pid = pid;
    g_engine_in  = pipe_in[1];
    g_engine_out = pipe_out[0];

    /* Make engine_out non-blocking */
    int flags = fcntl(g_engine_out, F_GETFL, 0);
    fcntl(g_engine_out, F_SETFL, flags | O_NONBLOCK);

    /* UCI handshake */
    engine_write("uci\n");
    /* Read until uciok */
    char buf[512]; int tries = 200;
    while (tries-- > 0) {
        usleep(10000);
        if (engine_readline(buf, sizeof(buf))) {
            if (strncmp(buf, "id name", 7) == 0) {
                snprintf(g_engine_name, sizeof(g_engine_name), "%s", buf + 8);
            }
            if (strcmp(buf, "uciok") == 0) break;
        }
    }
    engine_write("isready\n");
    tries = 200;
    while (tries-- > 0) {
        usleep(10000);
        if (engine_readline(buf, sizeof(buf)))
            if (strcmp(buf, "readyok") == 0) break;
    }
    engine_write("ucinewgame\n");
    return 1;
}

static void engine_stop(void) {
    if (g_engine_in >= 0) {
        engine_write("quit\n");
        close(g_engine_in);  g_engine_in  = -1;
        close(g_engine_out); g_engine_out = -1;
        if (g_engine_pid > 0) {
            waitpid(g_engine_pid, NULL, WNOHANG);
            g_engine_pid = -1;
        }
    }
}

/* Build FEN for current position */
static void board_to_fen(const BoardState *s, char *fen, int maxlen) {
    char *p = fen;
    int left = maxlen - 1;

    /* Piece placement */
    for (int r = 7; r >= 0; r--) {
        int empty_count = 0;
        for (int c = 0; c < 8; c++) {
            int piece = s->board[r][c];
            if (piece == EMPTY) {
                empty_count++;
            } else {
                if (empty_count > 0) {
                    *p++ = '0' + empty_count; left--;
                    empty_count = 0;
                }
                int ap = abs(piece);
                char ch;
                switch(ap) {
                    case PAWN:   ch = 'p'; break;
                    case KNIGHT: ch = 'n'; break;
                    case BISHOP: ch = 'b'; break;
                    case ROOK:   ch = 'r'; break;
                    case QUEEN:  ch = 'q'; break;
                    case KING:   ch = 'k'; break;
                    default:     ch = '?'; break;
                }
                if (piece > 0) ch = toupper(ch);
                *p++ = ch; left--;
            }
        }
        if (empty_count > 0) { *p++ = '0' + empty_count; left--; }
        if (r > 0) { *p++ = '/'; left--; }
    }

    /* Active color */
    snprintf(p, left, " %c ", s->turn == WHITE_SIDE ? 'w' : 'b');
    p += 3; left -= 3;

    /* Castling */
    char castle[8] = {0}; int ci = 0;
    if (s->castle[0]) castle[ci++] = 'K';
    if (s->castle[1]) castle[ci++] = 'Q';
    if (s->castle[2]) castle[ci++] = 'k';
    if (s->castle[3]) castle[ci++] = 'q';
    if (ci == 0) castle[ci++] = '-';
    castle[ci] = 0;
    int n = snprintf(p, left, "%s ", castle);
    p += n; left -= n;

    /* En passant */
    if (s->ep_file >= 0) {
        n = snprintf(p, left, "%c%c ", 'a' + s->ep_file,
                     '1' + s->ep_rank);
    } else {
        n = snprintf(p, left, "- ");
    }
    p += n; left -= n;

    /* Halfmove / Fullmove */
    snprintf(p, left, "%d %d", s->halfmove, s->fullmove);
}

/* Build move list string for engine (from start) */
static void build_moves_string(char *buf, int maxlen) {
    buf[0] = 0;
    int left = maxlen - 1;
    char *p = buf;
    for (int i = 0; i < g_hist_len; i++) {
        Move *m = &g_history[i].move;
        /* algebraic: e.g. e2e4, e7e8q */
        char token[8];
        int fc = m->from_c, fr = m->from_r;
        int tc = m->to_c,   tr = m->to_r;
        if (m->promotion) {
            char pc;
            switch(abs(m->promotion)) {
                case QUEEN:  pc='q'; break;
                case ROOK:   pc='r'; break;
                case BISHOP: pc='b'; break;
                case KNIGHT: pc='n'; break;
                default:     pc='q'; break;
            }
            snprintf(token, sizeof(token), "%c%d%c%d%c",
                     'a'+fc, fr+1, 'a'+tc, tr+1, pc);
        } else {
            snprintf(token, sizeof(token), "%c%d%c%d",
                     'a'+fc, fr+1, 'a'+tc, tr+1);
        }
        int n = snprintf(p, left, "%s%s", i>0?" ":"", token);
        p += n; left -= n;
    }
}

static void engine_send_position(void) {
    char moves[MAX_HIST * 7 + 64];
    build_moves_string(moves, sizeof(moves));
    char cmd[sizeof(moves) + 64];
    if (g_hist_len == 0) {
        snprintf(cmd, sizeof(cmd), "position startpos\n");
    } else {
        snprintf(cmd, sizeof(cmd), "position startpos moves %s\n", moves);
    }
    engine_write(cmd);
}

static void engine_go(void) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "go movetime %d\n", g_engine_movetime);
    engine_write(cmd);
    g_engine_thinking = 1;
}

/* ─────────────────────────────────────────────
 *  BOARD INIT
 * ───────────────────────────────────────────── */

static void board_init(BoardState *s) {
    memset(s, 0, sizeof(*s));
    s->turn = WHITE_SIDE;
    s->castle[0] = s->castle[1] = s->castle[2] = s->castle[3] = 1;
    s->ep_file = -1;
    s->ep_rank = -1;
    s->fullmove = 1;

    /* White pieces rank 0 */
    s->board[0][0] = ROOK;   s->board[0][7] = ROOK;
    s->board[0][1] = KNIGHT; s->board[0][6] = KNIGHT;
    s->board[0][2] = BISHOP; s->board[0][5] = BISHOP;
    s->board[0][3] = QUEEN;  s->board[0][4] = KING;
    for (int c = 0; c < 8; c++) s->board[1][c] = PAWN;

    /* Black pieces rank 7 */
    s->board[7][0] = -ROOK;   s->board[7][7] = -ROOK;
    s->board[7][1] = -KNIGHT; s->board[7][6] = -KNIGHT;
    s->board[7][2] = -BISHOP; s->board[7][5] = -BISHOP;
    s->board[7][3] = -QUEEN;  s->board[7][4] = -KING;
    for (int c = 0; c < 8; c++) s->board[6][c] = -PAWN;
}

/* ─────────────────────────────────────────────
 *  MOVE GENERATION & VALIDATION
 * ───────────────────────────────────────────── */

static int in_board(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

/* Find king position for given side */
static int find_king(const BoardState *s, int side, int *kr, int *kc) {
    int king = side * KING;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (s->board[r][c] == king) { *kr = r; *kc = c; return 1; }
    return 0;
}

/* Is square (r,c) attacked by 'attacker' side? */
static int is_attacked(const BoardState *s, int r, int c, int attacker) {
    /* Pawn attacks */
    int pd = (attacker == WHITE_SIDE) ? 1 : -1; /* direction pawns move */
    /* Attacker's pawn at (r - pd, c±1) would attack (r,c) */
    for (int dc = -1; dc <= 1; dc += 2) {
        int pr = r - pd, pc = c + dc;
        if (in_board(pr, pc) && s->board[pr][pc] == attacker * PAWN)
            return 1;
    }
    /* Knight */
    int knight_moves[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},
                                {1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_moves[i][0], nc = c + knight_moves[i][1];
        if (in_board(nr,nc) && s->board[nr][nc] == attacker * KNIGHT)
            return 1;
    }
    /* Sliding pieces */
    /* Rook / Queen (straight) */
    int dirs[4][2] = {{0,1},{0,-1},{1,0},{-1,0}};
    for (int d = 0; d < 4; d++) {
        int nr = r + dirs[d][0], nc = c + dirs[d][1];
        while (in_board(nr, nc)) {
            int p = s->board[nr][nc];
            if (p != EMPTY) {
                if (p == attacker*ROOK || p == attacker*QUEEN) return 1;
                break;
            }
            nr += dirs[d][0]; nc += dirs[d][1];
        }
    }
    /* Bishop / Queen (diagonal) */
    int diag[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int d = 0; d < 4; d++) {
        int nr = r + diag[d][0], nc = c + diag[d][1];
        while (in_board(nr, nc)) {
            int p = s->board[nr][nc];
            if (p != EMPTY) {
                if (p == attacker*BISHOP || p == attacker*QUEEN) return 1;
                break;
            }
            nr += diag[d][0]; nc += diag[d][1];
        }
    }
    /* King */
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (dr==0&&dc==0) continue;
            int nr = r+dr, nc = c+dc;
            if (in_board(nr,nc) && s->board[nr][nc] == attacker*KING)
                return 1;
        }
    return 0;
}

static int is_in_check(const BoardState *s, int side) {
    int kr, kc;
    if (!find_king(s, side, &kr, &kc)) return 0;
    return is_attacked(s, kr, kc, -side);
}

/* Apply a move to board (no legality check for check) */
static void apply_move(BoardState *s, const Move *m) {
    int piece = s->board[m->from_r][m->from_c];
    s->board[m->from_r][m->from_c] = EMPTY;
    s->board[m->to_r][m->to_c] = m->promotion ? m->promotion : piece;

    /* En passant capture */
    if (m->ep_capture) {
        s->board[m->from_r][m->to_c] = EMPTY;
    }

    /* Castling rook move */
    if (m->castle_side == 1) { /* kingside */
        s->board[m->from_r][5] = s->board[m->from_r][7];
        s->board[m->from_r][7] = EMPTY;
    } else if (m->castle_side == 2) { /* queenside */
        s->board[m->from_r][3] = s->board[m->from_r][0];
        s->board[m->from_r][0] = EMPTY;
    }
}

/* Generate pseudo-legal moves for piece at (r,c) */
static int gen_piece_moves(const BoardState *s, int r, int c, Move *moves) {
    int count = 0;
    int piece = s->board[r][c];
    if (piece == EMPTY) return 0;
    int side = (piece > 0) ? WHITE_SIDE : BLACK_SIDE;
    int ap = abs(piece);

    /* Helper macro to add a basic move */
    #define ADD_MOVE(TR, TC) do { \
        moves[count].from_r = r; moves[count].from_c = c; \
        moves[count].to_r = TR;  moves[count].to_c = TC; \
        moves[count].piece = piece; \
        moves[count].captured = s->board[TR][TC]; \
        moves[count].promotion = 0; \
        moves[count].castle_side = 0; \
        moves[count].ep_capture = 0; \
        count++; \
    } while(0)

    switch(ap) {
    case PAWN: {
        int dir = side; /* +1 for white, -1 for black */
        int start_rank = (side == WHITE_SIDE) ? 1 : 6;
        int promo_rank = (side == WHITE_SIDE) ? 7 : 0;
        int nr = r + dir;

        /* Forward one */
        if (in_board(nr, c) && s->board[nr][c] == EMPTY) {
            if (nr == promo_rank) {
                int promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
                for (int pi = 0; pi < 4; pi++) {
                    ADD_MOVE(nr, c);
                    moves[count-1].promotion = side * promos[pi];
                }
            } else {
                ADD_MOVE(nr, c);
            }
            /* Forward two from start */
            if (r == start_rank && s->board[nr + dir][c] == EMPTY) {
                ADD_MOVE(nr + dir, c);
            }
        }
        /* Captures */
        for (int dc = -1; dc <= 1; dc += 2) {
            int nc2 = c + dc;
            if (!in_board(nr, nc2)) continue;
            int target = s->board[nr][nc2];
            /* Normal capture */
            if (target != EMPTY && ((target > 0) != (piece > 0))) {
                if (nr == promo_rank) {
                    int promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
                    for (int pi = 0; pi < 4; pi++) {
                        ADD_MOVE(nr, nc2);
                        moves[count-1].promotion = side * promos[pi];
                    }
                } else {
                    ADD_MOVE(nr, nc2);
                }
            }
            /* En passant */
            if (s->ep_file == nc2 && s->ep_rank == nr) {
                ADD_MOVE(nr, nc2);
                moves[count-1].ep_capture = 1;
                moves[count-1].captured   = -side * PAWN;
            }
        }
        break;
    }
    case KNIGHT: {
        int km[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},
                         {1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r+km[i][0], nc = c+km[i][1];
            if (!in_board(nr,nc)) continue;
            int t = s->board[nr][nc];
            if (t == EMPTY || (t > 0) != (piece > 0)) ADD_MOVE(nr, nc);
        }
        break;
    }
    case BISHOP: {
        int dirs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
        for (int d = 0; d < 4; d++) {
            int nr = r+dirs[d][0], nc = c+dirs[d][1];
            while (in_board(nr,nc)) {
                int t = s->board[nr][nc];
                if (t != EMPTY) {
                    if ((t>0) != (piece>0)) ADD_MOVE(nr,nc);
                    break;
                }
                ADD_MOVE(nr,nc);
                nr += dirs[d][0]; nc += dirs[d][1];
            }
        }
        break;
    }
    case ROOK: {
        int dirs[4][2] = {{0,1},{0,-1},{1,0},{-1,0}};
        for (int d = 0; d < 4; d++) {
            int nr = r+dirs[d][0], nc = c+dirs[d][1];
            while (in_board(nr,nc)) {
                int t = s->board[nr][nc];
                if (t != EMPTY) {
                    if ((t>0) != (piece>0)) ADD_MOVE(nr,nc);
                    break;
                }
                ADD_MOVE(nr,nc);
                nr += dirs[d][0]; nc += dirs[d][1];
            }
        }
        break;
    }
    case QUEEN: {
        int dirs[8][2] = {{0,1},{0,-1},{1,0},{-1,0},
                           {1,1},{1,-1},{-1,1},{-1,-1}};
        for (int d = 0; d < 8; d++) {
            int nr = r+dirs[d][0], nc = c+dirs[d][1];
            while (in_board(nr,nc)) {
                int t = s->board[nr][nc];
                if (t != EMPTY) {
                    if ((t>0) != (piece>0)) ADD_MOVE(nr,nc);
                    break;
                }
                ADD_MOVE(nr,nc);
                nr += dirs[d][0]; nc += dirs[d][1];
            }
        }
        break;
    }
    case KING: {
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++) {
                if (dr==0&&dc==0) continue;
                int nr = r+dr, nc = c+dc;
                if (!in_board(nr,nc)) continue;
                int t = s->board[nr][nc];
                if (t == EMPTY || (t>0) != (piece>0)) ADD_MOVE(nr,nc);
            }
        /* Castling */
        int back_rank = (side == WHITE_SIDE) ? 0 : 7;
        if (r == back_rank && c == 4 && !is_in_check(s, side)) {
            /* Kingside */
            if (s->castle[(side==WHITE_SIDE)?0:2]) {
                if (s->board[r][5] == EMPTY && s->board[r][6] == EMPTY &&
                    !is_attacked(s,r,5,-side) && !is_attacked(s,r,6,-side)) {
                    ADD_MOVE(r, 6);
                    moves[count-1].castle_side = 1;
                }
            }
            /* Queenside */
            if (s->castle[(side==WHITE_SIDE)?1:3]) {
                if (s->board[r][3] == EMPTY && s->board[r][2] == EMPTY &&
                    s->board[r][1] == EMPTY &&
                    !is_attacked(s,r,3,-side) && !is_attacked(s,r,2,-side)) {
                    ADD_MOVE(r, 2);
                    moves[count-1].castle_side = 2;
                }
            }
        }
        break;
    }
    }
    #undef ADD_MOVE
    return count;
}

/* Generate all legal moves for current side */
static int gen_legal_moves(const BoardState *s, Move *moves) {
    int count = 0;
    Move pseudo[MAX_MOVES];
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int p = s->board[r][c];
            if (p == EMPTY) continue;
            int pside = (p > 0) ? WHITE_SIDE : BLACK_SIDE;
            if (pside != s->turn) continue;

            int n = gen_piece_moves(s, r, c, pseudo);
            for (int i = 0; i < n; i++) {
                /* Test legality: make move, check if own king in check */
                BoardState tmp = *s;
                apply_move(&tmp, &pseudo[i]);
                if (!is_in_check(&tmp, s->turn)) {
                    moves[count++] = pseudo[i];
                }
            }
        }
    }
    return count;
}

/* ─────────────────────────────────────────────
 *  PGN NOTATION
 * ───────────────────────────────────────────── */

/* Generate SAN (Standard Algebraic Notation) for a move */
static void move_to_san(const BoardState *s, const Move *m, char *san) {
    int piece  = abs(m->piece);
    int side   = (m->piece > 0) ? WHITE_SIDE : BLACK_SIDE;
    char buf[32]; int bi = 0;

    /* Castling */
    if (m->castle_side == 1) { strcpy(san, "O-O");   return; }
    if (m->castle_side == 2) { strcpy(san, "O-O-O"); return; }

    /* Piece letter */
    if (piece != PAWN) {
        char pc[] = {0,'P','N','B','R','Q','K'};
        buf[bi++] = pc[piece];
    }

    /* Disambiguation */
    if (piece != PAWN) {
        /* Find other pieces of same type that can reach same square */
        int ambig_file = 0, ambig_rank = 0;
        Move all[MAX_MOVES];
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                if (r == m->from_r && c == m->from_c) continue;
                if (s->board[r][c] != m->piece) continue;
                /* Check if this piece can move to same destination */
                Move tmp_moves[MAX_MOVES];
                int n = gen_piece_moves(s, r, c, tmp_moves);
                for (int i = 0; i < n; i++) {
                    if (tmp_moves[i].to_r == m->to_r &&
                        tmp_moves[i].to_c == m->to_c) {
                        /* Verify legality */
                        BoardState ts = *s;
                        apply_move(&ts, &tmp_moves[i]);
                        if (!is_in_check(&ts, side)) {
                            if (c != m->from_c) ambig_file = 1;
                            else                ambig_rank = 1;
                        }
                    }
                }
            }
        }
        if (ambig_file) buf[bi++] = 'a' + m->from_c;
        if (ambig_rank) buf[bi++] = '1' + m->from_r;
    } else {
        /* Pawn: add source file if capture */
        if (m->captured || m->ep_capture)
            buf[bi++] = 'a' + m->from_c;
    }

    /* Capture */
    if (m->captured || m->ep_capture) buf[bi++] = 'x';

    /* Destination */
    buf[bi++] = 'a' + m->to_c;
    buf[bi++] = '1' + m->to_r;

    /* Promotion */
    if (m->promotion) {
        buf[bi++] = '=';
        char pc[] = {0,'P','N','B','R','Q','K'};
        buf[bi++] = pc[abs(m->promotion)];
    }

    buf[bi] = 0;

    /* Check / Checkmate */
    BoardState tmp = *s;
    apply_move(&tmp, m);
    tmp.turn = -side;

    /* Update castling rights for tmp */
    /* (simplified - full update done in make_move) */

    int in_chk = is_in_check(&tmp, -side);
    if (in_chk) {
        /* Check if checkmate */
        Move legal[MAX_MOVES];
        tmp.turn = -side;
        int n = gen_legal_moves(&tmp, legal);
        if (n == 0) buf[bi++] = '#';
        else        buf[bi++] = '+';
    }
    buf[bi] = 0;
    strcpy(san, buf);
}

/* Append move to PGN string */
static void pgn_append(const Move *m, int move_num, int side) {
    char san[32];
    /* We already have pgn_token in the move */
    if (side == WHITE_SIDE) {
        int n = snprintf(g_pgn + g_pgn_len,
                         MAX_PGN - g_pgn_len,
                         "%d. %s ", move_num, m->pgn_token);
        if (n > 0) g_pgn_len += n;
    } else {
        int n = snprintf(g_pgn + g_pgn_len,
                         MAX_PGN - g_pgn_len,
                         "%s ", m->pgn_token);
        if (n > 0) g_pgn_len += n;
    }
}

/* Rebuild entire PGN from history */
static void pgn_rebuild(void) {
    g_pgn_len = 0;
    g_pgn[0]  = 0;
    for (int i = 0; i < g_hist_len; i++) {
        Move *m   = &g_history[i].move;
        int move_num = (i / 2) + 1;
        int side  = (i % 2 == 0) ? WHITE_SIDE : BLACK_SIDE;
        pgn_append(m, move_num, side);
    }
}

/* ─────────────────────────────────────────────
 *  MAKE / UNDO MOVE
 * ───────────────────────────────────────────── */

/* Update board state after a move is applied */
static void update_state_after_move(BoardState *s, const Move *m) {
    int piece = abs(m->piece);
    int side  = (m->piece > 0) ? WHITE_SIDE : BLACK_SIDE;

    /* En passant target */
    s->ep_file = -1; s->ep_rank = -1;
    if (piece == PAWN && abs(m->to_r - m->from_r) == 2) {
        s->ep_file = m->from_c;
        s->ep_rank = (m->from_r + m->to_r) / 2;
    }

    /* Update castling rights */
    if (piece == KING) {
        if (side == WHITE_SIDE) { s->castle[0] = 0; s->castle[1] = 0; }
        else                    { s->castle[2] = 0; s->castle[3] = 0; }
    }
    if (piece == ROOK) {
        if (m->from_r == 0 && m->from_c == 0) s->castle[1] = 0; /* wQ */
        if (m->from_r == 0 && m->from_c == 7) s->castle[0] = 0; /* wK */
        if (m->from_r == 7 && m->from_c == 0) s->castle[3] = 0; /* bQ */
        if (m->from_r == 7 && m->from_c == 7) s->castle[2] = 0; /* bK */
    }
    /* If rook is captured, remove castling right */
    if (m->to_r == 0 && m->to_c == 0) s->castle[1] = 0;
    if (m->to_r == 0 && m->to_c == 7) s->castle[0] = 0;
    if (m->to_r == 7 && m->to_c == 0) s->castle[3] = 0;
    if (m->to_r == 7 && m->to_c == 7) s->castle[2] = 0;

    /* Halfmove clock */
    if (piece == PAWN || m->captured || m->ep_capture)
        s->halfmove = 0;
    else
        s->halfmove++;

    /* Fullmove */
    if (side == BLACK_SIDE) s->fullmove++;

    /* Switch turn */
    s->turn = -side;
}

static int make_move(const Move *m, int is_engine_move) {
    if (g_hist_len >= MAX_HIST) return 0;

    /* Save current state + move in history */
    g_history[g_hist_len].state = g_board;
    g_history[g_hist_len].move  = *m;

    /* Save state for undo in the move record */
    g_history[g_hist_len].move.castle_saved[0] = g_board.castle[0];
    g_history[g_hist_len].move.castle_saved[1] = g_board.castle[1];
    g_history[g_hist_len].move.castle_saved[2] = g_board.castle[2];
    g_history[g_hist_len].move.castle_saved[3] = g_board.castle[3];
    g_history[g_hist_len].move.ep_file_saved   = g_board.ep_file;
    g_history[g_hist_len].move.ep_rank_saved   = g_board.ep_rank;
    g_history[g_hist_len].move.halfmove_saved  = g_board.halfmove;

    /* Generate SAN before applying move */
    char san[32];
    move_to_san(&g_board, m, san);
    strncpy(g_history[g_hist_len].move.pgn_token, san,
            sizeof(g_history[g_hist_len].move.pgn_token) - 1);

    /* Apply move */
    apply_move(&g_board, m);
    update_state_after_move(&g_board, m);

    g_hist_len++;

    /* Update last move highlight */
    g_last_from_r = m->from_r; g_last_from_c = m->from_c;
    g_last_to_r   = m->to_r;   g_last_to_c   = m->to_c;

    /* Append to PGN */
    pgn_append(&g_history[g_hist_len-1].move,
               (g_hist_len + 1) / 2,
               (g_hist_len % 2 == 0) ? BLACK_SIDE : WHITE_SIDE);

    /* Check for game over */
    Move legal[MAX_MOVES];
    int n = gen_legal_moves(&g_board, legal);
    if (n == 0) {
        if (is_in_check(&g_board, g_board.turn)) {
            /* Checkmate */
            if (g_board.turn == BLACK_SIDE)
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "Checkmate! White wins! 1-0");
            else
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "Checkmate! Black wins! 0-1");
            /* Add result to PGN */
            const char *result = (g_board.turn == BLACK_SIDE) ? "1-0" : "0-1";
            snprintf(g_pgn + g_pgn_len, MAX_PGN - g_pgn_len, "%s", result);
        } else {
            snprintf(g_status_msg, sizeof(g_status_msg), "Stalemate! 1/2-1/2");
            snprintf(g_pgn + g_pgn_len, MAX_PGN - g_pgn_len, "1/2-1/2");
        }
        g_game_over = 1;
    } else if (g_board.halfmove >= 100) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Draw by 50-move rule! 1/2-1/2");
        g_game_over = 1;
    } else {
        /* Update status */
        if (is_in_check(&g_board, g_board.turn)) {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "%s is in check!",
                     g_board.turn == WHITE_SIDE ? "White" : "Black");
        } else {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "%s to move",
                     g_board.turn == WHITE_SIDE ? "White" : "Black");
        }
    }

    return 1;
}

static void undo_move(void) {
    if (g_hist_len == 0) return;

    /* If engine is thinking, stop it */
    if (g_engine_thinking) {
        engine_write("stop\n");
        g_engine_thinking = 0;
        g_engine_move_ready = 0;
    }

    /* Undo engine move too if it's our turn */
    /* Undo until it's the human side again */
    int undo_count = 1;
    if (g_engine_enabled && g_hist_len >= 2) {
        /* If last move was engine's, undo two */
        int last_was_engine =
            (g_history[g_hist_len-1].state.turn == g_engine_side);
        if (last_was_engine) undo_count = 2;
    }
    /* But if engine is not involved, just undo 1 */
    if (!g_engine_enabled) undo_count = 1;

    for (int k = 0; k < undo_count && g_hist_len > 0; k++) {
        g_hist_len--;
        g_board = g_history[g_hist_len].state;
    }

    /* Update last move highlight */
    if (g_hist_len > 0) {
        Move *m = &g_history[g_hist_len-1].move;
        g_last_from_r = m->from_r; g_last_from_c = m->from_c;
        g_last_to_r   = m->to_r;   g_last_to_c   = m->to_c;
    } else {
        g_last_from_r = g_last_from_c = g_last_to_r = g_last_to_c = -1;
    }

    /* Reset selection */
    g_selected = 0; g_sel_r = -1; g_sel_c = -1;
    g_valid_count = 0;
    g_game_over = 0;

    /* Rebuild PGN */
    pgn_rebuild();

    snprintf(g_status_msg, sizeof(g_status_msg),
             "%s to move (after takeback)",
             g_board.turn == WHITE_SIDE ? "White" : "Black");

    /* Re-sync engine */
    if (g_engine_enabled) {
        engine_send_position();
    }
}

/* ─────────────────────────────────────────────
 *  PARSE ENGINE MOVE (UCI bestmove a1b2[promo])
 * ───────────────────────────────────────────── */

static int parse_uci_move(const char *uci, Move *out) {
    if (strlen(uci) < 4) return 0;
    int fc = uci[0] - 'a';
    int fr = uci[1] - '1';
    int tc = uci[2] - 'a';
    int tr = uci[3] - '1';
    if (fc<0||fc>7||fr<0||fr>7||tc<0||tc>7||tr<0||tr>7) return 0;

    /* Find matching legal move */
    Move legal[MAX_MOVES];
    int n = gen_legal_moves(&g_board, legal);
    for (int i = 0; i < n; i++) {
        if (legal[i].from_r == fr && legal[i].from_c == fc &&
            legal[i].to_r   == tr && legal[i].to_c   == tc) {
            /* Check promotion */
            if (strlen(uci) >= 5) {
                char pc = tolower(uci[4]);
                int want;
                switch(pc) {
                    case 'q': want=QUEEN;  break;
                    case 'r': want=ROOK;   break;
                    case 'b': want=BISHOP; break;
                    case 'n': want=KNIGHT; break;
                    default:  want=QUEEN;  break;
                }
                if (legal[i].promotion != 0 &&
                    abs(legal[i].promotion) != want) continue;
            } else {
                if (legal[i].promotion && abs(legal[i].promotion)!=QUEEN)
                    continue; /* prefer queen */
            }
            *out = legal[i];
            return 1;
        }
    }
    return 0;
}

/* ─────────────────────────────────────────────
 *  RENDERING
 * ───────────────────────────────────────────── */

static const char *piece_unicode(int piece) {
    /* Use ASCII alternatives that render reliably in all terminals */
    if (piece == 0) return "  ";
    switch(piece) {
        case  PAWN:   return "WP";
        case  KNIGHT: return "WN";
        case  BISHOP: return "WB";
        case  ROOK:   return "WR";
        case  QUEEN:  return "WQ";
        case  KING:   return "WK";
        case -PAWN:   return "BP";
        case -KNIGHT: return "BN";
        case -BISHOP: return "BB";
        case -ROOK:   return "BR";
        case -QUEEN:  return "BQ";
        case -KING:   return "BK";
    }
    return "  ";
}

/* Unicode chess pieces (may render better on some terminals) */
static const char *piece_utf8(int piece) {
    switch(piece) {
        case  KING:   return "♔";
        case  QUEEN:  return "♕";
        case  ROOK:   return "♖";
        case  BISHOP: return "♗";
        case  KNIGHT: return "♘";
        case  PAWN:   return "♙";
        case -KING:   return "♚";
        case -QUEEN:  return "♛";
        case -ROOK:   return "♜";
        case -BISHOP: return "♝";
        case -KNIGHT: return "♞";
        case -PAWN:   return "♟";
        default:      return " ";
    }
}

/* Check if (r,c) is a valid destination for selected piece */
static int is_valid_dest(int r, int c) {
    for (int i = 0; i < g_valid_count; i++) {
        if (g_valid_moves[i].to_r == r && g_valid_moves[i].to_c == c)
            return 1;
    }
    return 0;
}

/* Get the first valid move from (sel_r,sel_c) to (r,c) */
static int get_valid_move(int r, int c, Move *out) {
    for (int i = 0; i < g_valid_count; i++) {
        if (g_valid_moves[i].to_r == r && g_valid_moves[i].to_c == c) {
            *out = g_valid_moves[i];
            return 1;
        }
    }
    return 0;
}

/* Board drawing constants */
#define SQ_W  4   /* square width in chars */
#define SQ_H  2   /* square height in rows */

/* Draw the board starting at terminal position (top_row, left_col) */
static void draw_board(int top_row, int left_col) {
    /* Column labels */
    term_goto(top_row + BOARD_SIZE * SQ_H + 1, left_col + 2);
    printf(FG_LABEL);
    for (int c = 0; c < BOARD_SIZE; c++) {
        int dc = g_flipped ? (7 - c) : c;
        printf("  %c ", 'a' + dc);
    }
    printf(ANSI_RESET);

    /* King in check */
    int wkr=-1, wkc=-1, bkr=-1, bkc=-1;
    find_king(&g_board, WHITE_SIDE, &wkr, &wkc);
    find_king(&g_board, BLACK_SIDE, &bkr, &bkc);
    int white_in_check = is_in_check(&g_board, WHITE_SIDE);
    int black_in_check = is_in_check(&g_board, BLACK_SIDE);

    for (int row = 0; row < BOARD_SIZE; row++) {
        /* Map display row to board rank */
        int r = g_flipped ? row : (7 - row);
        int rank_label = r + 1;

        for (int sq_line = 0; sq_line < SQ_H; sq_line++) {
            int term_row = top_row + row * SQ_H + sq_line;
            term_goto(term_row, left_col);

            /* Rank label */
            if (sq_line == SQ_H / 2) {
                printf(FG_LABEL " %d " ANSI_RESET, rank_label);
            } else {
                printf("   ");
            }

            for (int col = 0; col < BOARD_SIZE; col++) {
                int c = g_flipped ? (7 - col) : col;
                int piece = g_board.board[r][c];
                int is_dark = (r + c) % 2 == 0;

                /* Determine background */
                const char *bg;

                /* Priority: cursor > selected > valid dest > last move > check > normal */
                int is_cursor    = (r == g_cursor_r && c == g_cursor_c &&
                                    !(g_selected && g_sel_r == r && g_sel_c == c));
                int is_sel       = (g_selected && r == g_sel_r && c == g_sel_c);
                int is_valid     = (g_selected && is_valid_dest(r, c));
                int is_last_from = (r == g_last_from_r && c == g_last_from_c);
                int is_last_to   = (r == g_last_to_r   && c == g_last_to_c);
                int is_chk = (white_in_check && r == wkr && c == wkc) ||
                             (black_in_check && r == bkr && c == bkc);

                if (is_sel)
                    bg = BG_SELECTED;
                else if (is_cursor)
                    bg = BG_CURSOR;
                else if (is_valid)
                    bg = BG_VALID_MOVE;
                else if (is_chk)
                    bg = BG_CHECK;
                else if (is_last_from || is_last_to)
                    bg = BG_LAST_MOVE;
                else if (is_dark)
                    bg = BG_DARK_SQ;
                else
                    bg = BG_LIGHT_SQ;

                /* Foreground */
                const char *fg = (piece > 0) ? FG_WHITE_PIECE : FG_BLACK_PIECE;

                printf("%s%s", bg, fg);

                /* Middle line: show piece */
                if (sq_line == SQ_H / 2) {
                    if (piece != EMPTY) {
                        /* Show piece symbol */
                        printf(ANSI_BOLD " %s " ANSI_RESET, piece_utf8(piece));
                        printf("%s%s", bg, fg); /* re-apply for trailing space */
                    } else {
                        /* Show dot on valid destination squares */
                        if (is_valid)
                            printf(FG_BLACK_PIECE "  ● " ANSI_RESET);
                        else
                            printf("    ");
                    }
                } else {
                    printf("    ");
                }
                printf(ANSI_RESET);
            }

            /* Right rank label */
            if (sq_line == SQ_H / 2)
                printf(FG_LABEL " %d" ANSI_RESET, rank_label);
            else
                printf("  ");
        }
    }

    /* Top column labels */
    term_goto(top_row - 1, left_col + 2);
    printf(FG_LABEL);
    for (int c = 0; c < BOARD_SIZE; c++) {
        int dc = g_flipped ? (7 - c) : c;
        printf("  %c ", 'a' + dc);
    }
    printf(ANSI_RESET);
}

/* Draw the info panel to the right of the board */
static void draw_panel(int top_row, int left_col) {
    int row = top_row;
    int w = 38; /* panel width */

    /* Title */
    term_goto(row++, left_col);
    printf(FG_HEADER ANSI_BOLD "╔══════════════════════════════════╗" ANSI_RESET);
    term_goto(row++, left_col);
    printf(FG_HEADER ANSI_BOLD "║       TERMINAL CHESS  v1.0       ║" ANSI_RESET);
    term_goto(row++, left_col);
    printf(FG_HEADER ANSI_BOLD "╚══════════════════════════════════╝" ANSI_RESET);
    row++;

    /* Engine info */
    term_goto(row++, left_col);
    printf(FG_ENGINE ANSI_BOLD "Engine: " ANSI_RESET FG_PANEL);
    if (g_engine_enabled) {
        printf("%-26s", g_engine_name);
        printf(ANSI_RESET);
        term_goto(row++, left_col);
        printf(FG_PANEL "Plays: %s  Movetime: %dms",
               g_engine_side == BLACK_SIDE ? "Black" : "White",
               g_engine_movetime);
        if (g_engine_thinking)
            printf(FG_STATUS "  [thinking...]" ANSI_RESET);
    } else {
        printf("None (2-player mode)");
    }
    printf(ANSI_RESET);
    row++;

    /* Status */
    term_goto(row++, left_col);
    printf(FG_HEADER "─────────────────────────────────────" ANSI_RESET);
    term_goto(row++, left_col);
    printf(FG_STATUS ANSI_BOLD "Status: " ANSI_RESET);
    if (g_game_over)
        printf(FG_ERROR "%s" ANSI_RESET, g_status_msg);
    else
        printf(FG_PANEL "%s" ANSI_RESET, g_status_msg);

    /* Turn indicator */
    row++;
    term_goto(row++, left_col);
    if (!g_game_over) {
        if (g_board.turn == WHITE_SIDE) {
            printf(FG_WHITE_PIECE BG_DARK_SQ ANSI_BOLD
                   "  ♔  WHITE TO MOVE  ♔  " ANSI_RESET);
        } else {
            printf(FG_BLACK_PIECE BG_LIGHT_SQ ANSI_BOLD
                   "  ♚  BLACK TO MOVE  ♚  " ANSI_RESET);
        }
    }
    row++;

    /* Piece captured / material */
    term_goto(row++, left_col);
    printf(FG_HEADER "─────────────────────────────────────" ANSI_RESET);
    term_goto(row++, left_col);
    printf(FG_HEADER ANSI_BOLD "Captured pieces:" ANSI_RESET);

    /* Count captured pieces */
    int wcp[7]={0}, bcp[7]={0}; /* white/black captured */
    for (int i = 0; i < g_hist_len; i++) {
        Move *m = &g_history[i].move;
        if (m->captured != EMPTY) {
            if (m->captured > 0) wcp[abs(m->captured)]++; /* white piece captured */
            else                  bcp[abs(m->captured)]++;
        }
        if (m->ep_capture) {
            if (m->piece > 0) bcp[PAWN]++;
            else              wcp[PAWN]++;
        }
    }

    /* Display captured white pieces */
    term_goto(row++, left_col);
    printf(FG_WHITE_PIECE "White lost: " ANSI_RESET FG_PANEL);
    const char *pnames[] = {"","","♞","♝","♜","♛","♚"};
    for (int p = PAWN; p <= QUEEN; p++)
        for (int i = 0; i < wcp[p]; i++)
            printf("%s", piece_utf8(p)); /* white piece */

    /* Display captured black pieces */
    term_goto(row++, left_col);
    printf(FG_BLACK_PIECE "Black lost: " ANSI_RESET FG_PANEL);
    for (int p = PAWN; p <= QUEEN; p++)
        for (int i = 0; i < bcp[p]; i++)
            printf("%s", piece_utf8(-p)); /* black piece */

    printf(ANSI_RESET);

    /* Move history (PGN) */
    row++;
    term_goto(row++, left_col);
    printf(FG_HEADER "─────────────────────────────────────" ANSI_RESET);
    term_goto(row++, left_col);
    printf(FG_HEADER ANSI_BOLD "Moves (PGN):" ANSI_RESET);

    /* Display last ~12 lines of PGN */
    int pgn_rows = 10;
    int pgn_line_width = w - 2;
    char pgn_copy[MAX_PGN];
    strncpy(pgn_copy, g_pgn, MAX_PGN);

    /* Word-wrap PGN */
    char lines[32][64];
    int  line_count = 0;
    int  pi = 0;
    int  plen = g_pgn_len;

    while (pi < plen && line_count < 32) {
        int llen = 0;
        while (pi < plen && llen < pgn_line_width - 1) {
            /* Find next space or end */
            int word_start = pi;
            while (pi < plen && pgn_copy[pi] != ' ') pi++;
            int word_len = pi - word_start;
            if (llen + word_len + (llen > 0 ? 1 : 0) > pgn_line_width - 1) {
                /* Would overflow, start new line */
                pi = word_start;
                break;
            }
            if (llen > 0) lines[line_count][llen++] = ' ';
            memcpy(lines[line_count] + llen, pgn_copy + word_start, word_len);
            llen += word_len;
            if (pi < plen && pgn_copy[pi] == ' ') pi++;
        }
        lines[line_count][llen] = 0;
        line_count++;
    }

    /* Show last pgn_rows lines */
    int start_line = line_count > pgn_rows ? line_count - pgn_rows : 0;
    for (int li = start_line; li < line_count; li++) {
        if (row >= g_rows - 3) break;
        term_goto(row++, left_col);
        printf(FG_PGN "%-*s" ANSI_RESET, pgn_line_width, lines[li]);
    }
    /* Fill remaining PGN lines */
    for (int li = line_count; li < start_line + pgn_rows; li++) {
        if (row >= g_rows - 3) break;
        term_goto(row++, left_col);
        printf("%*s", pgn_line_width, "");
    }

    /* Controls help */
    row++;
    term_goto(row++, left_col);
    printf(FG_HEADER "─────────────────────────────────────" ANSI_RESET);
    term_goto(row++, left_col);
    printf(FG_LABEL "Arrows/WASD:Move  Enter/Spc:Select" ANSI_RESET);
    term_goto(row++, left_col);
    printf(FG_LABEL "U:Undo  R:Restart  F:Flip  Q:Quit" ANSI_RESET);
    term_goto(row++, left_col);
    printf(FG_LABEL "+/-:Engine speed  E:Toggle engine" ANSI_RESET);
}

static void draw_all(void) {
    /* Board at col 0, panel to the right */
    int board_left = 0;
    int board_top  = 1;
    int panel_left = board_left + 2 + BOARD_SIZE * SQ_W + 4 + 2;

    /* Clear screen */
    printf("\033[H");
    /* Clear each line to avoid artifacts */
    for (int r = 0; r < g_rows - 1; r++) {
        term_goto(r, 0);
        printf("\033[2K");
    }

    draw_board(board_top, board_left);
    draw_panel(board_top, panel_left);

    fflush(stdout);
}

/* ─────────────────────────────────────────────
 *  PROMOTION DIALOG
 * ───────────────────────────────────────────── */

static int promotion_dialog(int side) {
    /* Simple text dialog */
    printf("\033[%d;0H", g_rows - 4);
    printf(FG_STATUS ANSI_BOLD
           "Promote to: Q=Queen  R=Rook  B=Bishop  N=Knight > "
           ANSI_RESET);
    fflush(stdout);

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            c = toupper(c);
            if (c == 'Q') return side * QUEEN;
            if (c == 'R') return side * ROOK;
            if (c == 'B') return side * BISHOP;
            if (c == 'N') return side * KNIGHT;
        }
    }
}

/* ─────────────────────────────────────────────
 *  INPUT HANDLING
 * ───────────────────────────────────────────── */

/* Returns 1 to continue, 0 to quit */
static int handle_input(void) {
    char c;
    int n = (int)read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 1;

    /* Arrow key / escape sequence */
    if (c == '\033') {
        char seq[4] = {0};
        /* Try to read more */
        struct timeval tv = {0, 50000};
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) > 0)
            read(STDIN_FILENO, seq, 1);
        if (seq[0] == '[') {
            fd_set fds2; FD_ZERO(&fds2); FD_SET(STDIN_FILENO, &fds2);
            tv.tv_usec = 50000;
            if (select(STDIN_FILENO+1, &fds2, NULL, NULL, &tv) > 0)
                read(STDIN_FILENO, seq+1, 1);
            switch(seq[1]) {
                case 'A': c = 'w'; break; /* Up */
                case 'B': c = 's'; break; /* Down */
                case 'C': c = 'd'; break; /* Right */
                case 'D': c = 'a'; break; /* Left */
                default:  return 1;
            }
        } else {
            return 1; /* just ESC */
        }
    }

    c = tolower(c);

    /* Navigation */
    if (c == 'w' || c == 'k') {
        if (g_flipped) { if (g_cursor_r < 7) g_cursor_r++; }
        else           { if (g_cursor_r < 7) g_cursor_r++; }
        return 1;
    }
    if (c == 's' || c == 'j') {
        if (g_flipped) { if (g_cursor_r > 0) g_cursor_r--; }
        else           { if (g_cursor_r > 0) g_cursor_r--; }
        return 1;
    }
    if (c == 'a' || c == 'h') {
        if (g_cursor_c > 0) g_cursor_c--;
        return 1;
    }
    if (c == 'd' || c == 'l') {
        if (g_cursor_c < 7) g_cursor_c++;
        return 1;
    }

    /* Quit */
    if (c == 'q') return 0;

    /* Restart */
    if (c == 'r') {
        board_init(&g_board);
        g_hist_len = 0;
        g_selected = 0; g_sel_r = -1; g_sel_c = -1;
        g_valid_count = 0;
        g_last_from_r = g_last_from_c = g_last_to_r = g_last_to_c = -1;
        g_pgn_len = 0; g_pgn[0] = 0;
        g_game_over = 0;
        g_engine_thinking = 0;
        g_engine_move_ready = 0;
        snprintf(g_status_msg, sizeof(g_status_msg), "New game started");
        if (g_engine_enabled) {
            engine_write("ucinewgame\n");
            engine_write("isready\n");
        }
        return 1;
    }

    /* Flip board */
    if (c == 'f') {
        g_flipped = !g_flipped;
        return 1;
    }

    /* Undo */
    if (c == 'u') {
        undo_move();
        return 1;
    }

    /* Engine toggle */
    if (c == 'e') {
        if (g_engine_enabled) {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "Engine disabled");
            g_engine_enabled = 0;
            g_engine_thinking = 0;
        } else if (g_engine_pid > 0) {
            g_engine_enabled = 1;
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "Engine enabled");
            engine_send_position();
        }
        return 1;
    }

    /* Engine speed */
    if (c == '+' || c == '=') {
        g_engine_movetime += 500;
        if (g_engine_movetime > 10000) g_engine_movetime = 10000;
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Engine movetime: %dms", g_engine_movetime);
        return 1;
    }
    if (c == '-') {
        g_engine_movetime -= 500;
        if (g_engine_movetime < 100) g_engine_movetime = 100;
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Engine movetime: %dms", g_engine_movetime);
        return 1;
    }

    /* Select / Move */
    if (c == '\r' || c == '\n' || c == ' ') {
        if (g_game_over) return 1;
        if (g_engine_enabled && g_board.turn == g_engine_side) return 1;

        int r = g_cursor_r, col = g_cursor_c;
        int piece = g_board.board[r][col];
        int piece_side = (piece > 0) ? WHITE_SIDE : BLACK_SIDE;

        if (!g_selected) {
            /* Select piece */
            if (piece != EMPTY && piece_side == g_board.turn) {
                g_selected = 1;
                g_sel_r = r; g_sel_c = col;
                /* Generate valid moves */
                Move all[MAX_MOVES];
                int all_count = gen_legal_moves(&g_board, all);
                g_valid_count = 0;
                for (int i = 0; i < all_count; i++) {
                    if (all[i].from_r == r && all[i].from_c == col) {
                        g_valid_moves[g_valid_count++] = all[i];
                    }
                }
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "Selected %s at %c%d (%d moves)",
                         piece_utf8(piece),
                         'a' + col, r + 1, g_valid_count);
            }
        } else {
            /* Try to move */
            Move mv;
            int can_move = get_valid_move(r, col, &mv);

            if (can_move) {
                /* Handle promotion */
                if (abs(mv.piece) == PAWN &&
                    ((mv.piece > 0 && mv.to_r == 7) ||
                     (mv.piece < 0 && mv.to_r == 0))) {
                    /* If multiple promotion choices exist, ask */
                    /* Count promotion moves to this square */
                    int promo_choices = 0;
                    for (int i = 0; i < g_valid_count; i++) {
                        if (g_valid_moves[i].to_r == r &&
                            g_valid_moves[i].to_c == col &&
                            g_valid_moves[i].promotion)
                            promo_choices++;
                    }
                    if (promo_choices > 1) {
                        int pp = promotion_dialog(g_board.turn);
                        /* Find matching move */
                        for (int i = 0; i < g_valid_count; i++) {
                            if (g_valid_moves[i].to_r == r &&
                                g_valid_moves[i].to_c == col &&
                                g_valid_moves[i].promotion == pp) {
                                mv = g_valid_moves[i];
                                break;
                            }
                        }
                    }
                }

                make_move(&mv, 0);

                /* Trigger engine if needed */
                if (g_engine_enabled &&
                    g_board.turn == g_engine_side &&
                    !g_game_over) {
                    engine_send_position();
                    engine_go();
                }
            } else if (piece != EMPTY && piece_side == g_board.turn) {
                /* Re-select different piece */
                g_sel_r = r; g_sel_c = col;
                Move all[MAX_MOVES];
                int all_count = gen_legal_moves(&g_board, all);
                g_valid_count = 0;
                for (int i = 0; i < all_count; i++) {
                    if (all[i].from_r == r && all[i].from_c == col)
                        g_valid_moves[g_valid_count++] = all[i];
                }
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "Selected %s at %c%d (%d moves)",
                         piece_utf8(piece),
                         'a' + col, r + 1, g_valid_count);
                return 1;
            } else {
                /* Deselect */
                g_selected = 0; g_sel_r = -1; g_sel_c = -1;
                g_valid_count = 0;
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "%s to move",
                         g_board.turn == WHITE_SIDE ? "White" : "Black");
                return 1;
            }

            /* Deselect after move */
            g_selected = 0; g_sel_r = -1; g_sel_c = -1;
            g_valid_count = 0;
        }
    }

    return 1;
}

/* ─────────────────────────────────────────────
 *  ENGINE POLL
 * ───────────────────────────────────────────── */

static void poll_engine(void) {
    if (!g_engine_enabled || g_engine_out < 0) return;

    char buf[ENGINE_BUF];
    while (engine_readline(buf, sizeof(buf))) {
        /* Check for bestmove response */
        if (strncmp(buf, "bestmove", 8) == 0) {
            g_engine_thinking = 0;
            /* Parse: bestmove e2e4 [ponder ...] */
            char *p = buf + 9;
            while (*p == ' ') p++;
            char uci[8] = {0};
            int i = 0;
            while (*p && *p != ' ' && i < 7) uci[i++] = *p++;
            uci[i] = 0;

            if (strcmp(uci, "(none)") != 0 && strcmp(uci, "0000") != 0) {
                strncpy(g_engine_move_buf, uci, sizeof(g_engine_move_buf)-1);
                g_engine_move_ready = 1;
            }
        }
    }

    if (g_engine_move_ready && !g_game_over) {
        g_engine_move_ready = 0;
        Move mv;
        if (parse_uci_move(g_engine_move_buf, &mv)) {
            make_move(&mv, 1);
        }
    }
}

/* ─────────────────────────────────────────────
 *  MAIN
 * ───────────────────────────────────────────── */

static void cleanup(void) {
    engine_stop();
    term_show_cursor();
    term_restore();
    printf("\033[?1049l"); /* restore normal screen */
    printf(ANSI_RESET "\n");
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
    signal(SIGPIPE, SIG_IGN);

    /* Switch to alternate screen buffer */
    printf("\033[?1049h");
    fflush(stdout);

    term_raw_mode();
    term_get_size();
    term_hide_cursor();
    term_clear();

    /* Initialize board */
    board_init(&g_board);
    snprintf(g_status_msg, sizeof(g_status_msg), "White to move");

    /* Start engine if provided */
    if (argc >= 2) {
        if (engine_start(argv[1])) {
            g_engine_enabled = 1;
            engine_send_position();
            /* Engine plays black by default */
            g_engine_side = BLACK_SIDE;
        } else {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "Failed to start engine: %s", argv[1]);
            strncpy(g_engine_name, "Failed", sizeof(g_engine_name));
        }
    }

    /* Main loop */
    int running = 1;
    int redraw   = 1;

    while (running) {
        /* Poll engine output */
        poll_engine();

        /* Check if engine should be triggered (e.g., after board init) */
        if (g_engine_enabled && !g_engine_thinking && !g_game_over &&
            g_board.turn == g_engine_side) {
            /* Only start engine if we haven't already */
            static int last_engine_hist = -1;
            if (last_engine_hist != g_hist_len) {
                last_engine_hist = g_hist_len;
                engine_send_position();
                engine_go();
            }
        }

        /* Handle keyboard input (non-blocking check) */
        {
            struct timeval tv = {0, 16000}; /* ~60fps */
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            int nfds = STDIN_FILENO + 1;
            if (g_engine_out >= 0) {
                FD_SET(g_engine_out, &fds);
                if (g_engine_out + 1 > nfds) nfds = g_engine_out + 1;
            }

            int sel = select(nfds, &fds, NULL, NULL, &tv);

            if (sel > 0) {
                if (FD_ISSET(STDIN_FILENO, &fds)) {
                    int prev_hist = g_hist_len;
                    running = handle_input();
                    if (g_hist_len != prev_hist) redraw = 1;
                    else redraw = 1; /* always redraw on input */
                }
            } else {
                /* Timeout - check if engine moved */
                redraw = 1;
            }
        }

        if (redraw) {
            draw_all();
            redraw = 0;
        }
    }

    cleanup();

    /* Print PGN to stdout after exit */
    printf("\n\n─── Game PGN ───\n%s\n\n", g_pgn);

    return 0;
}
