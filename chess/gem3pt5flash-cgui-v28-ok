#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <stdint.h>

#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define WHITE 8
#define BLACK 16
#define COLOR_MASK 24
#define PIECE_MASK 7

typedef struct {
    uint8_t board[64];
    uint8_t turn;
    uint8_t castle; // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int8_t ep_square;
    int halfmove;
    int fullmove;
} BoardState;

typedef struct {
    uint8_t from;
    uint8_t to;
    uint8_t promo;
} MoveRaw;

typedef struct {
    BoardState state;
    MoveRaw raw_move;
    char san[16];
    char coord[6];
} HistoryEntry;

// Globals
BoardState state;
HistoryEntry history[2048];
int history_count = 0;

int cursor_sq = 52; // Starts at e2
int selected_sq = -1;
MoveRaw current_legal_moves[100];
int current_legal_count = 0;

// Time Control Settings
int time_control_type = 2; // 0=Depth, 1=Nodes, 2=Movetime
int tc_depth_val = 10;
int tc_nodes_val = 100000;
int tc_time_val = 2000; // default 2 seconds

// UCI Engine connection variables
int to_engine_fd = -1;
int from_engine_fd = -1;
pid_t engine_pid = -1;
char engine_path[512] = "stockfish"; 
int engine_active = 0;
int engine_thinking = 0;
int engine_turn = BLACK; // Default: player is White, Engine is Black
char engine_buf[4096];
int engine_buf_len = 0;

struct termios orig_termios;

// POSIX Clean Terminal Recovery
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

void handle_sigint(int sig) {
    exit(0);
}

// Board Initialization
void init_board(BoardState *s) {
    memset(s, 0, sizeof(BoardState));
    uint8_t back_rank[] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int i = 0; i < 8; i++) {
        s->board[i] = BLACK | back_rank[i];
        s->board[8 + i] = BLACK | PAWN;
        s->board[48 + i] = WHITE | PAWN;
        s->board[56 + i] = WHITE | back_rank[i];
    }
    s->turn = WHITE;
    s->castle = 15;
    s->ep_square = -1;
    s->halfmove = 0;
    s->fullmove = 1;
}

// Check validation: checks if a square is attacked by an opponent
int is_attacked(const BoardState *s, int sq, int attacker) {
    int r = sq / 8, c = sq % 8;
    int pawn_dir = (attacker == WHITE) ? 1 : -1;

    // Pawns
    int pr = r + pawn_dir;
    for (int dc = -1; dc <= 1; dc += 2) {
        int pc = c + dc;
        if (pr >= 0 && pr < 8 && pc >= 0 && pc < 8) {
            if (s->board[pr * 8 + pc] == (attacker | PAWN)) return 1;
        }
    }

    // Knights
    int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn[i][0], nc = c + kn[i][1];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (s->board[nr * 8 + nc] == (attacker | KNIGHT)) return 1;
        }
    }

    // Kings
    int kg[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    for (int i = 0; i < 8; i++) {
        int kr = r + kg[i][0], kc = c + kg[i][1];
        if (kr >= 0 && kr < 8 && kc >= 0 && kc < 8) {
            if (s->board[kr * 8 + kc] == (attacker | KING)) return 1;
        }
    }

    // Sliders: Rooks / Queens
    int r_dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += r_dirs[i][0]; nc += r_dirs[i][1];
            if (!(nr >= 0 && nr < 8 && nc >= 0 && nc < 8)) break;
            uint8_t p = s->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == (attacker | ROOK) || p == (attacker | QUEEN)) return 1;
                break;
            }
        }
    }

    // Sliders: Bishops / Queens
    int b_dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += b_dirs[i][0]; nc += b_dirs[i][1];
            if (!(nr >= 0 && nr < 8 && nc >= 0 && nc < 8)) break;
            uint8_t p = s->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == (attacker | BISHOP) || p == (attacker | QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

int is_in_check(const BoardState *s, int color) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (s->board[i] == (color | KING)) { king_sq = i; break; }
    }
    if (king_sq == -1) return 0;
    return is_attacked(s, king_sq, color ^ COLOR_MASK);
}

// Generates raw pseudo-legal moves for a specific square
int gen_moves_from(const BoardState *s, int sq, MoveRaw *moves) {
    uint8_t p = s->board[sq];
    if (p == EMPTY) return 0;
    int color = p & COLOR_MASK;
    if (color != s->turn) return 0;
    int type = p & PIECE_MASK;
    int r = sq / 8, c = sq % 8;
    int count = 0;

    if (type == PAWN) {
        int dir = (color == WHITE) ? -1 : 1;
        int start_row = (color == WHITE) ? 6 : 1;
        int promo_row = (color == WHITE) ? 0 : 7;

        int next_r = r + dir;
        if (next_r >= 0 && next_r < 8 && s->board[next_r * 8 + c] == EMPTY) {
            if (next_r == promo_row) {
                moves[count++] = (MoveRaw){sq, next_r * 8 + c, QUEEN};
                moves[count++] = (MoveRaw){sq, next_r * 8 + c, ROOK};
                moves[count++] = (MoveRaw){sq, next_r * 8 + c, BISHOP};
                moves[count++] = (MoveRaw){sq, next_r * 8 + c, KNIGHT};
            } else {
                moves[count++] = (MoveRaw){sq, next_r * 8 + c, 0};
                if (r == start_row && s->board[(r + 2 * dir) * 8 + c] == EMPTY) {
                    moves[count++] = (MoveRaw){sq, (r + 2 * dir) * 8 + c, 0};
                }
            }
        }
        int dcs[2] = {-1, 1};
        for (int i = 0; i < 2; i++) {
            int next_c = c + dcs[i];
            if (next_r >= 0 && next_r < 8 && next_c >= 0 && next_c < 8) {
                int target = next_r * 8 + next_c;
                uint8_t tp = s->board[target];
                if ((tp != EMPTY && (tp & COLOR_MASK) != color) || target == s->ep_square) {
                    if (next_r == promo_row) {
                        moves[count++] = (MoveRaw){sq, target, QUEEN};
                        moves[count++] = (MoveRaw){sq, target, ROOK};
                        moves[count++] = (MoveRaw){sq, target, BISHOP};
                        moves[count++] = (MoveRaw){sq, target, KNIGHT};
                    } else {
                        moves[count++] = (MoveRaw){sq, target, 0};
                    }
                }
            }
        }
    } else if (type == KNIGHT) {
        int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + kn[i][0], nc = c + kn[i][1];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                if (s->board[target] == EMPTY || (s->board[target] & COLOR_MASK) != color) {
                    moves[count++] = (MoveRaw){sq, target, 0};
                }
            }
        }
    } else if (type == KING) {
        int kg[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + kg[i][0], nc = c + kg[i][1];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                if (s->board[target] == EMPTY || (s->board[target] & COLOR_MASK) != color) {
                    moves[count++] = (MoveRaw){sq, target, 0};
                }
            }
        }
        // Castling
        if (color == WHITE && sq == 60) {
            if ((s->castle & 1) && s->board[61] == EMPTY && s->board[62] == EMPTY) {
                if (!is_attacked(s, 60, BLACK) && !is_attacked(s, 61, BLACK)) moves[count++] = (MoveRaw){60, 62, 0};
            }
            if ((s->castle & 2) && s->board[59] == EMPTY && s->board[58] == EMPTY && s->board[57] == EMPTY) {
                if (!is_attacked(s, 60, BLACK) && !is_attacked(s, 59, BLACK)) moves[count++] = (MoveRaw){60, 58, 0};
            }
        } else if (color == BLACK && sq == 4) {
            if ((s->castle & 4) && s->board[5] == EMPTY && s->board[6] == EMPTY) {
                if (!is_attacked(s, 4, WHITE) && !is_attacked(s, 5, WHITE)) moves[count++] = (MoveRaw){4, 6, 0};
            }
            if ((s->castle & 8) && s->board[3] == EMPTY && s->board[2] == EMPTY && s->board[1] == EMPTY) {
                if (!is_attacked(s, 4, WHITE) && !is_attacked(s, 3, WHITE)) moves[count++] = (MoveRaw){4, 2, 0};
            }
        }
    } else { // Bishops, Rooks, Queens
        int start_d = (type == ROOK) ? 0 : (type == BISHOP) ? 4 : 0;
        int end_d = (type == ROOK) ? 4 : (type == BISHOP) ? 8 : 8;
        int dirs[8][2] = {{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};
        for (int d = start_d; d < end_d; d++) {
            int nr = r, nc = c;
            while (1) {
                nr += dirs[d][0]; nc += dirs[d][1];
                if (!(nr >= 0 && nr < 8 && nc >= 0 && nc < 8)) break;
                int target = nr * 8 + nc;
                uint8_t tp = s->board[target];
                if (tp == EMPTY) {
                    moves[count++] = (MoveRaw){sq, target, 0};
                } else {
                    if ((tp & COLOR_MASK) != color) moves[count++] = (MoveRaw){sq, target, 0};
                    break;
                }
            }
        }
    }
    return count;
}

// Executes a move on a temporary or output BoardState
void make_move(const BoardState *prev, BoardState *next, MoveRaw mv) {
    *next = *prev;
    uint8_t p = next->board[mv.from];
    uint8_t color = p & COLOR_MASK;
    uint8_t type = p & PIECE_MASK;

    next->ep_square = -1;
    next->board[mv.from] = EMPTY;

    // Remove castling rights upon movement of King/Rooks
    if (mv.from == 60) next->castle &= ~3;
    if (mv.from == 4)  next->castle &= ~12;
    if (mv.from == 56 || mv.to == 56) next->castle &= ~2;
    if (mv.from == 63 || mv.to == 63) next->castle &= ~1;
    if (mv.from == 0 || mv.to == 0)   next->castle &= ~8;
    if (mv.from == 7 || mv.to == 7)   next->castle &= ~4;

    // En Passant capture execution
    if (type == PAWN && mv.to == prev->ep_square) {
        int cap_sq = mv.to + ((color == WHITE) ? 8 : -8);
        next->board[cap_sq] = EMPTY;
    }
    // Set En Passant targets
    if (type == PAWN && abs(mv.to - mv.from) == 16) {
        next->ep_square = mv.from + ((color == WHITE) ? -8 : 8);
    }
    // Apply promotion or simple moves
    if (type == PAWN && mv.promo != 0) {
        next->board[mv.to] = color | mv.promo;
    } else {
        next->board[mv.to] = p;
    }
    // Castling rook relocations
    if (type == KING) {
        if (mv.from == 60 && mv.to == 62) { next->board[61] = next->board[63]; next->board[63] = EMPTY; }
        else if (mv.from == 60 && mv.to == 58) { next->board[59] = next->board[56]; next->board[56] = EMPTY; }
        else if (mv.from == 4 && mv.to == 6) { next->board[5] = next->board[7]; next->board[7] = EMPTY; }
        else if (mv.from == 4 && mv.to == 2) { next->board[3] = next->board[0]; next->board[0] = EMPTY; }
    }

    next->turn = color ^ COLOR_MASK;
    if (color == BLACK) next->fullmove++;
    if (type == PAWN || prev->board[mv.to] != EMPTY) next->halfmove = 0;
    else next->halfmove++;
}

int is_legal_move(const BoardState *s, MoveRaw mv) {
    BoardState temp;
    make_move(s, &temp, mv);
    return !is_in_check(&temp, s->turn);
}

int gen_legal_moves_from(const BoardState *s, int sq, MoveRaw *legal_moves) {
    MoveRaw pseudo[100];
    int count = gen_moves_from(s, sq, pseudo);
    int legal_count = 0;
    for (int i = 0; i < count; i++) {
        if (is_legal_move(s, pseudo[i])) {
            legal_moves[legal_count++] = pseudo[i];
        }
    }
    return legal_count;
}

int count_all_legal_moves(const BoardState *s) {
    int total = 0;
    for (int i = 0; i < 64; i++) {
        if ((s->board[i] & COLOR_MASK) == s->turn) {
            MoveRaw dummy[100];
            total += gen_legal_moves_from(s, i, dummy);
        }
    }
    return total;
}

// standard PGN (SAN) generator
void get_san(const BoardState *s, MoveRaw mv, char *san) {
    uint8_t p = s->board[mv.from];
    uint8_t type = p & PIECE_MASK;
    uint8_t color = p & COLOR_MASK;

    if (type == KING) {
        if (mv.from == 60 && mv.to == 62) { strcpy(san, "O-O"); goto check_status; }
        if (mv.from == 60 && mv.to == 58) { strcpy(san, "O-O-O"); goto check_status; }
        if (mv.from == 4 && mv.to == 6) { strcpy(san, "O-O"); goto check_status; }
        if (mv.from == 4 && mv.to == 2) { strcpy(san, "O-O-O"); goto check_status; }
    }

    int ptr = 0;
    if (type == PAWN) {
        if (mv.to == s->ep_square || s->board[mv.to] != EMPTY) {
            san[ptr++] = 'a' + (mv.from % 8);
            san[ptr++] = 'x';
        }
        san[ptr++] = 'a' + (mv.to % 8);
        san[ptr++] = '1' + (7 - (mv.to / 8));
        if (mv.promo) {
            san[ptr++] = '=';
            char chars[] = "  NBRQK";
            san[ptr++] = chars[mv.promo];
        }
    } else {
        char piece_chars[] = "  NBRQK";
        san[ptr++] = piece_chars[type];

        // Disambiguation
        int file_conflict = 0, rank_conflict = 0, conflict = 0;
        for (int i = 0; i < 64; i++) {
            if (i != mv.from && s->board[i] == p) {
                MoveRaw alt[100];
                int n = gen_legal_moves_from(s, i, alt);
                for (int j = 0; j < n; j++) {
                    if (alt[j].to == mv.to) {
                        conflict = 1;
                        if (i % 8 == mv.from % 8) rank_conflict = 1;
                        else file_conflict = 1;
                    }
                }
            }
        }
        if (conflict) {
            if (file_conflict || !rank_conflict) san[ptr++] = 'a' + (mv.from % 8);
            if (rank_conflict) san[ptr++] = '1' + (7 - (mv.from / 8));
        }

        if (s->board[mv.to] != EMPTY) san[ptr++] = 'x';
        san[ptr++] = 'a' + (mv.to % 8);
        san[ptr++] = '1' + (7 - (mv.to / 8));
    }
    san[ptr] = '\0';

check_status:;
    BoardState next;
    make_move(s, &next, mv);
    if (is_in_check(&next, color ^ COLOR_MASK)) {
        if (count_all_legal_moves(&next) == 0) strcat(san, "#");
        else strcat(san, "+");
    }
}

void get_coord_str(MoveRaw mv, char *buf) {
    int ptr = sprintf(buf, "%c%d%c%d", 'a' + (mv.from % 8), 8 - (mv.from / 8), 'a' + (mv.to % 8), 8 - (mv.to / 8));
    if (mv.promo) {
        char chars[] = "  nbrqk";
        buf[ptr++] = chars[mv.promo];
    }
    buf[ptr] = '\0';
}

// Non-blocking Unix subprocess mechanics for UCI Engine
void stop_engine() {
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, WNOHANG);
        engine_pid = -1;
    }
    if (to_engine_fd != -1) { close(to_engine_fd); to_engine_fd = -1; }
    if (from_engine_fd != -1) { close(from_engine_fd); from_engine_fd = -1; }
    engine_thinking = 0;
}

int start_engine(const char *path) {
    stop_engine();
    int to_engine[2], from_engine[2];
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return 0;

    engine_pid = fork();
    if (engine_pid < 0) return 0;

    if (engine_pid == 0) { // Child
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[0]); close(to_engine[1]);
        close(from_engine[0]); close(from_engine[1]);
        char *argv[] = {(char*)path, NULL};
        execvp(path, argv);
        exit(127); // Fail code
    }

    close(to_engine[0]); close(from_engine[1]);
    to_engine_fd = to_engine[1];
    from_engine_fd = from_engine[0];

    usleep(50000); // Wait 50ms to check if child failed execvp
    int status;
    if (waitpid(engine_pid, &status, WNOHANG) > 0) {
        stop_engine();
        return 0;
    }

    fcntl(from_engine_fd, F_SETFL, O_NONBLOCK);
    write(to_engine_fd, "uci\nisready\n", 12);
    return 1;
}

void send_engine_position() {
    if (to_engine_fd == -1) return;
    char cmd[8192];
    int ptr = sprintf(cmd, "position startpos");
    if (history_count > 0) {
        ptr += sprintf(cmd + ptr, " moves");
        for (int i = 0; i < history_count; i++) {
            ptr += sprintf(cmd + ptr, " %s", history[i].coord);
        }
    }
    ptr += sprintf(cmd + ptr, "\n");
    write(to_engine_fd, cmd, strlen(cmd));

    char go_cmd[128];
    if (time_control_type == 0) sprintf(go_cmd, "go depth %d\n", tc_depth_val);
    else if (time_control_type == 1) sprintf(go_cmd, "go nodes %d\n", tc_nodes_val);
    else sprintf(go_cmd, "go movetime %d\n", tc_time_val);

    write(to_engine_fd, go_cmd, strlen(go_cmd));
    engine_thinking = 1;
}

int poll_engine(MoveRaw *out_move) {
    if (from_engine_fd == -1 || !engine_thinking) return 0;
    char temp[1024];
    int n = read(from_engine_fd, temp, sizeof(temp) - 1);
    if (n > 0) {
        temp[n] = '\0';
        if (engine_buf_len + n < (int)sizeof(engine_buf)) {
            memcpy(engine_buf + engine_buf_len, temp, n);
            engine_buf_len += n;
            engine_buf[engine_buf_len] = '\0';
        } else {
            engine_buf_len = 0; // fallback buffer overflow protection
        }

        char *line_start = engine_buf;
        char *newline;
        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';
            if (strncmp(line_start, "bestmove ", 9) == 0) {
                char move_str[16];
                sscanf(line_start, "bestmove %s", move_str);
                engine_thinking = 0;
                if (strlen(move_str) >= 4) {
                    out_move->from = (move_str[0] - 'a') + (8 - (move_str[1] - '0')) * 8;
                    out_move->to = (move_str[2] - 'a') + (8 - (move_str[3] - '0')) * 8;
                    out_move->promo = 0;
                    if (strlen(move_str) == 5) {
                        char p = move_str[4];
                        if (p == 'q') out_move->promo = QUEEN;
                        else if (p == 'r') out_move->promo = ROOK;
                        else if (p == 'b') out_move->promo = BISHOP;
                        else if (p == 'n') out_move->promo = KNIGHT;
                    }
                    int consumed = (newline + 1) - engine_buf;
                    memmove(engine_buf, newline + 1, engine_buf_len - consumed);
                    engine_buf_len -= consumed;
                    return 1;
                }
            }
            line_start = newline + 1;
        }
        int consumed = line_start - engine_buf;
        if (consumed > 0) {
            memmove(engine_buf, line_start, engine_buf_len - consumed);
            engine_buf_len -= consumed;
        }
    }
    return 0;
}

// Render logic: ANSI background choices
const char* get_bg_color(int sq, int selected_sq, int is_check_sq, int is_last_src, int is_last_dst, int is_legal_target) {
    if (sq == is_check_sq) return "\033[48;5;196m";       // Red (Check)
    if (sq == selected_sq) return "\033[48;5;220m";       // Gold (Selection)
    if (is_legal_target) return "\033[48;5;121m";         // Soft Green (Legal moves)
    if (sq == is_last_src || sq == is_last_dst) return "\033[48;5;153m"; // Blue (Last move)

    int r = sq / 8, c = sq % 8;
    return ((r + c) % 2 == 0) ? "\033[48;5;179m" : "\033[48;5;94m"; // Wood light / Wood dark
}

const char* get_piece_fg(uint8_t p) {
    return ((p & COLOR_MASK) == WHITE) ? "\033[38;5;15m\033[1m" : "\033[38;5;232m"; // Bold White / Jet Black
}

void draw_ui() {
    int last_src = -1, last_dst = -1;
    if (history_count > 0) {
        last_src = history[history_count - 1].raw_move.from;
        last_dst = history[history_count - 1].raw_move.to;
    }

    int check_sq = -1;
    if (is_in_check(&state, state.turn)) {
        for (int i = 0; i < 64; i++) {
            if (state.board[i] == (state.turn | KING)) { check_sq = i; break; }
        }
    }

    // Reset cursor to top-left to update graphics in place
    printf("\033[H");
    printf("\n    ==== TERMINAL CHESS C-GUI ====\n\n");
    printf("     a  b  c  d  e  f  g  h\n");
    printf("   ┌────────────────────────┐\n");
    for (int r = 0; r < 8; r++) {
        printf(" %d │", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;

            int is_legal_target = 0;
            for (int i = 0; i < current_legal_count; i++) {
                if (current_legal_moves[i].to == sq) { is_legal_target = 1; break; }
            }

            const char *bg = get_bg_color(sq, selected_sq, check_sq, last_src, last_dst, is_legal_target);
            uint8_t p = state.board[sq];
            printf("%s%s", bg, get_piece_fg(p));

            char left = (sq == cursor_sq) ? '[' : ' ';
            char right = (sq == cursor_sq) ? ']' : ' ';

            int type = p & PIECE_MASK;
            if (type == EMPTY) {
                if (is_legal_target) printf("%c•%c", left, right);
                else printf("%c %c", left, right);
            } else {
                const char *sym = "";
                switch (type) {
                    case PAWN:   sym = "♟"; break;
                    case KNIGHT: sym = "♞"; break;
                    case BISHOP: sym = "♝"; break;
                    case ROOK:   sym = "♜"; break;
                    case QUEEN:  sym = "♛"; break;
                    case KING:   sym = "♚"; break;
                }
                printf("%c%s%c", left, sym, right);
            }
            printf("\033[0m"); // Reset ANSI colors
        }
        printf("│ %d\n", 8 - r);
    }
    printf("   └────────────────────────┐\n");
    printf("     a  b  c  d  e  f  g  h\n\n");

    // Game Info Block
    int legal_moves_all = count_all_legal_moves(&state);
    if (legal_moves_all == 0) {
        if (check_sq != -1) printf("  \033[1;31m[GAME OVER - CHECKMATE! %s WINS]\033[0m\n", (state.turn == WHITE) ? "BLACK" : "WHITE");
        else printf("  \033[1;33m[GAME OVER - STALEMATE (DRAW)]\033[0m\n");
    } else {
        printf("  Turn: %s %s                      \n", 
            (state.turn == WHITE) ? "\033[1;37mWHITE (Player)\033[0m" : "\033[1;33mBLACK\033[0m", 
            (check_sq != -1) ? "\033[1;31m[IN CHECK]\033[0m" : "          ");
    }

    printf("  Mode: %s", (engine_active) ? "\033[1;32mVs Engine\033[0m" : "Player vs Player");
    if (engine_active) {
        printf(" (Engine: %s, Playing: %s)", engine_path, (engine_turn == WHITE) ? "WHITE" : "BLACK");
        if (engine_thinking) printf(" \033[5;32m[THINKING...]\033[0m   ");
        else printf("                   ");
    }
    printf("     \n");

    printf("  Time Control: ");
    if (time_control_type == 0) printf("Depth = %d plies\n", tc_depth_val);
    else if (time_control_type == 1) printf("Nodes = %d\n", tc_nodes_val);
    else printf("Movetime = %.2fs\n", (double)tc_time_val / 1000.0);

    // Track standard 5-move history
    printf("  Last 5 moves: ");
    int show_count = (history_count < 5) ? history_count : 5;
    if (show_count == 0) printf("(none)                                                 ");
    for (int i = history_count - show_count; i < history_count; i++) {
        int full_move_num = (i / 2) + 1;
        if (i % 2 == 0) printf("%d.%s ", full_move_num, history[i].san);
        else printf("%d...%s ", full_move_num, history[i].san);
    }
    printf("                                  \n\n");

    printf("  Controls:\n");
    printf("    [Arrows / WASD] Move Cursor  | [Space / Enter] Select/Move Piece\n");
    printf("    [U] Undo Last Move           | [E] Toggle Engine On/Off\n");
    printf("    [T] Change Time Controls     | [C] Change Engine Binary Path\n");
    printf("    [Q] Quit Chess Game\n");
    fflush(stdout);
}

void execute_player_move(MoveRaw mv) {
    char san[16];
    get_san(&state, mv, san);
    char coord[6];
    get_coord_str(mv, coord);

    history[history_count].state = state;
    history[history_count].raw_move = mv;
    strcpy(history[history_count].san, san);
    strcpy(history[history_count].coord, coord);
    history_count++;

    BoardState next;
    make_move(&state, &next, mv);
    state = next;

    selected_sq = -1;
    current_legal_count = 0;
}

int check_and_handle_promotion(MoveRaw *mv) {
    uint8_t p = state.board[mv->from];
    if ((p & PIECE_MASK) == PAWN) {
        int r = mv->to / 8;
        if (r == 0 || r == 7) {
            printf("\n  \033[1;33m[PROMOTION] Select: [Q]ueen | [R]ook | [B]ishop | [N]ight:\033[0m ");
            fflush(stdout);
            char c;
            while (1) {
                while (read(STDIN_FILENO, &c, 1) != 1);
                c = toupper((unsigned char)c);
                if (c == 'Q') { mv->promo = QUEEN; break; }
                if (c == 'R') { mv->promo = ROOK; break; }
                if (c == 'B') { mv->promo = BISHOP; break; }
                if (c == 'N') { mv->promo = KNIGHT; break; }
            }
            return 1;
        }
    }
    return 0;
}

void time_control_menu() {
    printf("\033[H\033[2J=== CHOOSE TIME CONTROL TYPE ===\n\n");
    printf("  [1] Depth (Current: %d plies)\n", tc_depth_val);
    printf("  [2] Nodes (Current: %d nodes)\n", tc_nodes_val);
    printf("  [3] Movetime (Current: %.2fs)\n", (double)tc_time_val / 1000.0);
    printf("\n  Choose Setting Option (any other key exits): ");
    fflush(stdout);

    char c;
    while (read(STDIN_FILENO, &c, 1) != 1);
    if (c == '1') {
        printf("\n  Enter Target Ply Depth Limit: "); fflush(stdout);
        disable_raw_mode();
        int val;
        if (scanf("%d", &val) == 1 && val > 0) { tc_depth_val = val; time_control_type = 0; }
        enable_raw_mode();
    } else if (c == '2') {
        printf("\n  Enter Target Node Limit: "); fflush(stdout);
        disable_raw_mode();
        int val;
        if (scanf("%d", &val) == 1 && val > 0) { tc_nodes_val = val; time_control_type = 1; }
        enable_raw_mode();
    } else if (c == '3') {
        printf("\n  Enter Target Seconds per move (e.g. 1.5): "); fflush(stdout);
        disable_raw_mode();
        double val;
        if (scanf("%lf", &val) == 1 && val > 0) { tc_time_val = (int)(val * 1000.0); time_control_type = 2; }
        enable_raw_mode();
    }
    printf("\033[H\033[2J");
}

void engine_path_menu() {
    printf("\033[H\033[2J=== CONFIGURE ENGINE PATH ===\n\n");
    printf("  Current Path Setting: %s\n\n", engine_path);
    printf("  Enter Absolute or Path Directory (e.g. /opt/homebrew/bin/stockfish): ");
    fflush(stdout);

    disable_raw_mode();
    char path[512];
    if (scanf("%511s", path) == 1) {
        strcpy(engine_path, path);
        if (engine_active) {
            if (!start_engine(engine_path)) {
                printf("\n  [ERROR] Engine launch failed at: %s. Reverting modes.\n", engine_path);
                engine_active = 0;
                fflush(stdout);
                sleep(2);
            }
        }
    }
    enable_raw_mode();
    printf("\033[H\033[2J");
}

int read_key() {
    char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n != 1) return -1;
    if (c == '\033') { // Escape Sequence for Arrow keys
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'W'; // Arrow Up -> mapped to W
                case 'B': return 'S'; // Arrow Down -> mapped to S
                case 'C': return 'D'; // Arrow Right -> mapped to D
                case 'D': return 'A'; // Arrow Left -> mapped to A
            }
        }
        return '\033';
    }
    return toupper((unsigned char)c);
}

int main() {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    init_board(&state);
    enable_raw_mode();

    printf("\033[2J"); // Initial terminal clean

    while (1) {
        draw_ui();

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int max_fd = STDIN_FILENO;

        if (engine_active && from_engine_fd != -1) {
            FD_SET(from_engine_fd, &fds);
            if (from_engine_fd > max_fd) max_fd = from_engine_fd;
        }

        // Trigger engine thoughts asynchronously if it is the engine's turn
        if (engine_active && !engine_thinking && state.turn == engine_turn) {
            if (count_all_legal_moves(&state) > 0) {
                send_engine_position();
                draw_ui();
            }
        }

        int act = select(max_fd + 1, &fds, NULL, NULL, NULL);
        if (act < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (engine_active && from_engine_fd != -1 && FD_ISSET(from_engine_fd, &fds)) {
            MoveRaw engine_mv;
            if (poll_engine(&engine_mv)) {
                execute_player_move(engine_mv);
            }
        }

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            int key = read_key();
            if (key == -1) continue;

            if (key == 'Q') {
                break;
            } else if (key == 'U') {
                if (engine_active) { // Undo 2 states to let player keep control of active side
                    if (history_count >= 2) {
                        history_count--; state = history[history_count].state;
                        history_count--; state = history[history_count].state;
                    }
                } else if (history_count >= 1) {
                    history_count--; state = history[history_count].state;
                }
                selected_sq = -1;
                current_legal_count = 0;
            } else if (key == 'E') {
                if (engine_active) {
                    stop_engine();
                    engine_active = 0;
                } else {
                    printf("\033[H\033[2JLaunching Engine Process (%s)...\n", engine_path); fflush(stdout);
                    if (start_engine(engine_path)) {
                        engine_active = 1;
                        engine_turn = state.turn; // Engine plays the current active side
                    } else {
                        printf("\n  [ERROR] Engine launch failed. Please enter target bin path via [C]\n");
                        printf("  Press any key to return..."); fflush(stdout);
                        char dummy;
                        while (read(STDIN_FILENO, &dummy, 1) != 1);
                    }
                    printf("\033[H\033[2J");
                }
            } else if (key == 'T') {
                time_control_menu();
            } else if (key == 'C') {
                engine_path_menu();
            } else if (key == 'W' || key == 'S' || key == 'A' || key == 'D') {
                int r = cursor_sq / 8, c = cursor_sq % 8;
                if (key == 'W' && r > 0) r--;
                if (key == 'S' && r < 7) r++;
                if (key == 'A' && c > 0) c--;
                if (key == 'D' && c < 7) c++;
                cursor_sq = r * 8 + c;
            } else if (key == ' ' || key == '\n' || key == '\r') {
                if (!(engine_active && engine_thinking)) {
                    if (selected_sq == -1) {
                        uint8_t p = state.board[cursor_sq];
                        if (p != EMPTY && (p & COLOR_MASK) == state.turn) {
                            selected_sq = cursor_sq;
                            current_legal_count = gen_legal_moves_from(&state, selected_sq, current_legal_moves);
                        }
                    } else {
                        int match = -1;
                        for (int i = 0; i < current_legal_count; i++) {
                            if (current_legal_moves[i].to == cursor_sq) { match = i; break; }
                        }
                        if (match != -1) {
                            MoveRaw final_mv = current_legal_moves[match];
                            check_and_handle_promotion(&final_mv);
                            execute_player_move(final_mv);
                        } else {
                            uint8_t p = state.board[cursor_sq];
                            if (p != EMPTY && (p & COLOR_MASK) == state.turn) { // Re-select alternate piece
                                selected_sq = cursor_sq;
                                current_legal_count = gen_legal_moves_from(&state, selected_sq, current_legal_moves);
                            } else {
                                selected_sq = -1;
                                current_legal_count = 0;
                            }
                        }
                    }
                }
            }
        }
    }

    stop_engine();
    return 0;
}
