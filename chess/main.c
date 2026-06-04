#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <ncurses.h>

#define EMPTY 0
#define BOARD_X 5
#define BOARD_Y 2
#define MAX_MOVES 1000

// Piece representations: 1=P, 2=N, 3=B, 4=R, 5=Q, 6=K. Positive for White, Negative for Black.
const int initial_board[64] = {
    -4, -2, -3, -5, -6, -3, -2, -4, // Rank 8 (Black)
    -1, -1, -1, -1, -1, -1, -1, -1, // Rank 7
     0,  0,  0,  0,  0,  0,  0,  0, // Rank 6
     0,  0,  0,  0,  0,  0,  0,  0, // Rank 5
     0,  0,  0,  0,  0,  0,  0,  0, // Rank 4
     0,  0,  0,  0,  0,  0,  0,  0, // Rank 3
     1,  1,  1,  1,  1,  1,  1,  1, // Rank 2 (White)
     4,  2,  3,  5,  6,  3,  2,  4  // Rank 1
};

typedef struct {
    char board[64];
    int castle_rights; // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_square;     // En-passant target square (-1 if none)
    int active_color;  // 1=White, -1=Black
    char san[16];      // Standard Algebraic Notation (PGN)
    char uci[6];       // UCI format string (e.g. e2e4)
    int from;
    int to;
} MoveHistory;

// Global Game State
int board[64];
int castle_rights = 15;
int ep_square = -1;
int active_color = 1; // White starts

MoveHistory history[MAX_MOVES];
int history_count = 0;

int cursor_sq = 52;      // Starts on e2 (index 52)
int selected_sq = -1;    // -1 means no selection
int last_move_from = -1;
int last_move_to = -1;

// UCI Engine connection variables
int write_pipe[2];
int read_pipe[2];
pid_t engine_pid = -1;
int engine_active = 0;
char engine_path[256] = "stockfish"; // default path

// Sign helper
int sign(int val) {
    return (val > 0) - (val < 0);
}

// Check if square index is valid
int on_board(int r, int c) {
    return (r >= 0 && r < 8 && c >= 0 && c < 8);
}

// Forward declarations
int is_move_legal(int from, int to);
int is_square_attacked(int sq, int by_color);
void do_move(int from, int to, int is_real);
void generate_san(int from, int to, char *san);

// Kill background engine on close
void cleanup() {
    if (engine_pid > 0) {
        kill(engine_pid, SIGKILL);
    }
    endwin();
}

// Initialize UCI Engine
void start_engine(const char *path) {
    if (pipe(write_pipe) < 0 || pipe(read_pipe) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(write_pipe[0], STDIN_FILENO);
        dup2(read_pipe[1], STDOUT_FILENO);
        close(write_pipe[1]);
        close(read_pipe[0]);
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDERR_FILENO);

        char *args[] = {(char *)path, NULL};
        execvp(path, args);
        exit(1); // Exit child if exec fails
    }
    close(write_pipe[0]);
    close(read_pipe[1]);
}

void send_to_engine(const char *cmd) {
    if (engine_pid > 0) {
        write(write_pipe[1], cmd, strlen(cmd));
        write(write_pipe[1], "\n", 1);
    }
}

int read_line_timeout(char *buf, int max_len, int timeout_sec) {
    fd_set set;
    struct timeval timeout;
    FD_ZERO(&set);
    FD_SET(read_pipe[0], &set);
    timeout.tv_sec = timeout_sec;
    timeout.tv_usec = 0;

    int rv = select(read_pipe[0] + 1, &set, NULL, NULL, &timeout);
    if (rv <= 0) return -1;

    int i = 0;
    char c;
    while (i < max_len - 1) {
        int n = read(read_pipe[0], &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

void init_uci() {
    start_engine(engine_path);
    send_to_engine("uci");
    char line[256];
    int connected = 0;
    while (read_line_timeout(line, sizeof(line), 2) > 0) {
        if (strstr(line, "uciok") != NULL) {
            connected = 1;
            break;
        }
    }
    if (connected) {
        send_to_engine("isready");
        while (read_line_timeout(line, sizeof(line), 2) > 0) {
            if (strstr(line, "readyok") != NULL) {
                engine_active = 1;
                break;
            }
        }
    }
    if (!engine_active && engine_pid > 0) {
        kill(engine_pid, SIGKILL);
        engine_pid = -1;
    }
}

// Game State Reset
void reset_board() {
    memcpy(board, initial_board, sizeof(board));
    castle_rights = 15;
    ep_square = -1;
    active_color = 1;
    selected_sq = -1;
    last_move_from = -1;
    last_move_to = -1;
}

// Pseudo-legality validator
int is_move_pseudo_legal(int from, int to) {
    int p = board[from];
    if (p == EMPTY) return 0;
    int target = board[to];
    int color = sign(p);
    int type = abs(p);

    if (target != EMPTY && sign(target) == color) return 0;

    int r1 = from / 8, c1 = from % 8;
    int r2 = to / 8, c2 = to % 8;
    int dr = r2 - r1;
    int df = c2 - c1;

    switch (type) {
        case 1: // Pawn
            if (color == 1) { // White
                if (df == 0 && dr == -1 && target == EMPTY) return 1;
                if (df == 0 && dr == -2 && r1 == 6 && target == EMPTY && board[from - 8] == EMPTY) return 1;
                if (abs(df) == 1 && dr == -1 && (target < 0 || to == ep_square)) return 1;
            } else { // Black
                if (df == 0 && dr == 1 && target == EMPTY) return 1;
                if (df == 0 && dr == 2 && r1 == 1 && target == EMPTY && board[from + 8] == EMPTY) return 1;
                if (abs(df) == 1 && dr == 1 && (target > 0 || to == ep_square)) return 1;
            }
            return 0;

        case 2: // Knight
            if ((abs(df) == 1 && abs(dr) == 2) || (abs(df) == 2 && abs(dr) == 1)) return 1;
            return 0;

        case 3: // Bishop
            if (abs(df) != abs(dr)) return 0;
            goto check_path;

        case 4: // Rook
            if (df != 0 && dr != 0) return 0;
            goto check_path;

        case 5: // Queen
            if (abs(df) != abs(dr) && df != 0 && dr != 0) return 0;
            goto check_path;

        check_path: {
            int step_r = (dr > 0) - (dr < 0);
            int step_c = (df > 0) - (df < 0);
            int r = r1 + step_r, c = c1 + step_c;
            while (r != r2 || c != c2) {
                if (board[r * 8 + c] != EMPTY) return 0;
                r += step_r; c += step_c;
            }
            return 1;
        }

        case 6: // King
            if (abs(df) <= 1 && abs(dr) <= 1) return 1;

            // Castling
            if (color == 1 && from == 60) {
                if (to == 62 && (castle_rights & 1)) {
                    if (board[61] == EMPTY && board[62] == EMPTY) {
                        if (!is_square_attacked(60, -1) && !is_square_attacked(61, -1) && !is_square_attacked(62, -1)) return 1;
                    }
                }
                if (to == 58 && (castle_rights & 2)) {
                    if (board[59] == EMPTY && board[58] == EMPTY && board[57] == EMPTY) {
                        if (!is_square_attacked(60, -1) && !is_square_attacked(59, -1) && !is_square_attacked(58, -1)) return 1;
                    }
                }
            } else if (color == -1 && from == 4) {
                if (to == 6 && (castle_rights & 4)) {
                    if (board[5] == EMPTY && board[6] == EMPTY) {
                        if (!is_square_attacked(4, 1) && !is_square_attacked(5, 1) && !is_square_attacked(6, 1)) return 1;
                    }
                }
                if (to == 2 && (castle_rights & 8)) {
                    if (board[3] == EMPTY && board[2] == EMPTY && board[1] == EMPTY) {
                        if (!is_square_attacked(4, 1) && !is_square_attacked(3, 1) && !is_square_attacked(2, 1)) return 1;
                    }
                }
            }
            return 0;
    }
    return 0;
}

int is_square_attacked(int sq, int by_color) {
    for (int i = 0; i < 64; i++) {
        int p = board[i];
        if (p != EMPTY && sign(p) == by_color) {
            int type = abs(p);
            if (type == 6) {
                int r1 = i / 8, c1 = i % 8;
                int r2 = sq / 8, c2 = sq % 8;
                if (abs(r2 - r1) <= 1 && abs(c2 - c1) <= 1) return 1;
            } else if (type == 1) {
                int r1 = i / 8, c1 = i % 8;
                int r2 = sq / 8, c2 = sq % 8;
                int df = c2 - c1;
                int dr = r2 - r1;
                if (abs(df) == 1 && dr == -by_color) return 1;
            } else {
                if (is_move_pseudo_legal(i, sq)) return 1;
            }
        }
    }
    return 0;
}

int is_in_check(int color) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (board[i] == color * 6) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(king_sq, -color);
}

int is_move_legal(int from, int to) {
    int p = board[from];
    if (p == EMPTY) return 0;
    if (sign(p) != active_color) return 0;
    if (!is_move_pseudo_legal(from, to)) return 0;

    int saved_board[64];
    memcpy(saved_board, board, sizeof(board));
    int saved_castle = castle_rights;
    int saved_ep = ep_square;

    // Simulate move
    board[to] = board[from];
    board[from] = EMPTY;
    if (abs(p) == 1 && to == saved_ep) {
        board[to + ((active_color > 0) ? 8 : -8)] = EMPTY;
    }

    int legal = !is_in_check(active_color);

    memcpy(board, saved_board, sizeof(board));
    castle_rights = saved_castle;
    ep_square = saved_ep;

    return legal;
}

int has_legal_moves(int color) {
    int saved_active = active_color;
    active_color = color;
    for (int i = 0; i < 64; i++) {
        if (board[i] != EMPTY && sign(board[i]) == color) {
            for (int j = 0; j < 64; j++) {
                if (is_move_legal(i, j)) {
                    active_color = saved_active;
                    return 1;
                }
            }
        }
    }
    active_color = saved_active;
    return 0;
}

void get_uci_move_string(int from, int to, char *uci) {
    int p = board[from];
    sprintf(uci, "%c%d%c%d", 'a' + (from % 8), 8 - (from / 8), 'a' + (to % 8), 8 - (to / 8));
    if (abs(p) == 1 && (to / 8 == 0 || to / 8 == 7)) {
        strcat(uci, "q"); // Auto-promote to Queen
    }
}

void do_move(int from, int to, int is_real) {
    int p = board[from];

    if (is_real) {
        MoveHistory h;
        memcpy(h.board, board, sizeof(board));
        h.castle_rights = castle_rights;
        h.ep_square = ep_square;
        h.active_color = active_color;
        h.from = from;
        h.to = to;
        generate_san(from, to, h.san);
        get_uci_move_string(from, to, h.uci);
        history[history_count++] = h;
    }

    board[to] = board[from];
    board[from] = EMPTY;

    // Execute Castling
    if (abs(p) == 6 && abs(from - to) == 2) {
        if (to == 62) { board[61] = board[63]; board[63] = EMPTY; }
        else if (to == 58) { board[59] = board[56]; board[56] = EMPTY; }
        else if (to == 6) { board[5] = board[7]; board[7] = EMPTY; }
        else if (to == 2) { board[3] = board[0]; board[0] = EMPTY; }
    }

    // Execute En Passant
    if (abs(p) == 1 && to == ep_square) {
        board[to + ((p > 0) ? 8 : -8)] = EMPTY;
    }

    // Execute Promotion
    if (abs(p) == 1 && (to / 8 == 0 || to / 8 == 7)) {
        board[to] = (p > 0) ? 5 : -5;
    }

    // Update En Passant availability
    if (abs(p) == 1 && abs(from - to) == 16) {
        ep_square = (p > 0) ? from - 8 : from + 8;
    } else {
        ep_square = -1;
    }

    // Update Castling Rights
    if (from == 60) castle_rights &= ~3;
    if (from == 4) castle_rights &= ~12;
    if (from == 56 || to == 56) castle_rights &= ~2;
    if (from == 63 || to == 63) castle_rights &= ~1;
    if (from == 0 || to == 0) castle_rights &= ~8;
    if (from == 7 || to == 7) castle_rights &= ~4;

    active_color = -active_color;
}

// Generate standard algebraic chess notation (SAN) for moves
void generate_san(int from, int to, char *san) {
    int p = board[from];
    int type = abs(p);
    int target = board[to];

    if (type == 6 && abs(from - to) == 2) {
        if (to > from) strcpy(san, "O-O");
        else strcpy(san, "O-O-O");
        return;
    }

    char *ptr = san;
    if (type != 1) {
        char piece_letters[] = " PNBRQK";
        *ptr++ = piece_letters[type];

        // Check for disambiguation conflict
        int conflict = 0, need_file = 0, need_rank = 0;
        for (int i = 0; i < 64; i++) {
            if (i != from && board[i] == p) {
                if (is_move_legal(i, to)) {
                    conflict = 1;
                    if (i % 8 == from % 8) need_rank = 1;
                    else need_file = 1;
                }
            }
        }
        if (conflict) {
            if (need_file || !need_rank) *ptr++ = 'a' + (from % 8);
            if (need_rank) *ptr++ = '8' - (from / 8);
        }
    } else {
        if (target != EMPTY || to == ep_square) {
            *ptr++ = 'a' + (from % 8);
        }
    }

    if (target != EMPTY || (type == 1 && to == ep_square)) {
        *ptr++ = 'x';
    }

    *ptr++ = 'a' + (to % 8);
    *ptr++ = '8' - (to / 8);

    if (type == 1 && (to / 8 == 0 || to / 8 == 7)) {
        *ptr++ = '=';
        *ptr++ = 'Q';
    }

    // Check simulation
    int saved_board[64];
    memcpy(saved_board, board, sizeof(board));
    int saved_castle = castle_rights;
    int saved_ep = ep_square;

    board[to] = board[from];
    board[from] = EMPTY;
    if (type == 1 && to == ep_square) {
        board[to + ((p > 0) ? 8 : -8)] = EMPTY;
    }
    if (type == 1 && (to / 8 == 0 || to / 8 == 7)) {
        board[to] = (p > 0) ? 5 : -5;
    }

    int in_check = is_in_check(-active_color);
    int has_moves = has_legal_moves(-active_color);

    if (in_check) {
        if (!has_moves) *ptr++ = '#';
        else *ptr++ = '+';
    }

    *ptr = '\0';

    memcpy(board, saved_board, sizeof(board));
    castle_rights = saved_castle;
    ep_square = saved_ep;
}

void undo_move() {
    int steps = (engine_active) ? 2 : 1;
    if (history_count >= steps) {
        history_count -= steps;
        reset_board();
        int count = history_count;
        history_count = 0;
        for (int i = 0; i < count; i++) {
            do_move(history[i].from, history[i].to, 1);
        }
    }
}

char get_piece_char(int p) {
    switch (abs(p)) {
        case 1: return p > 0 ? 'P' : 'p';
        case 2: return p > 0 ? 'N' : 'n';
        case 3: return p > 0 ? 'B' : 'b';
        case 4: return p > 0 ? 'R' : 'r';
        case 5: return p > 0 ? 'Q' : 'q';
        case 6: return p > 0 ? 'K' : 'k';
        default: return ' ';
    }
}

void draw_interface() {
    clear();

    // Draw Board Squares
    for (int r = 0; r < 8; r++) {
        // Draw rank index on the left
        mvprintw(BOARD_Y + r * 3 + 1, BOARD_X - 3, "%d", 8 - r);

        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int is_light = (r + c) % 2 == 0;
            int pair = is_light ? 1 : 2;

            if (sq == selected_sq) {
                pair = 3; // Selected
            } else if (sq == last_move_from || sq == last_move_to) {
                pair = 4; // Last move highlight
            } else if (is_in_check(active_color) && board[sq] == active_color * 6) {
                pair = 4; // Red alert if checking king
            }

            attron(COLOR_PAIR(pair));
            for (int sy = 0; sy < 3; sy++) {
                mvprintw(BOARD_Y + r * 3 + sy, BOARD_X + c * 6, "      ");
            }

            char piece = get_piece_char(board[sq]);
            if (piece != ' ') {
                if (board[sq] > 0) attron(A_BOLD);
                mvprintw(BOARD_Y + r * 3 + 1, BOARD_X + c * 6 + 2, "%c", piece);
                if (board[sq] > 0) attroff(A_BOLD);
            }
            attroff(COLOR_PAIR(pair));

            // Overlay keyboard selection cursor brackets
            if (sq == cursor_sq) {
                attron(COLOR_PAIR(5) | A_BOLD);
                mvprintw(BOARD_Y + r * 3 + 1, BOARD_X + c * 6 + 1, "[");
                mvprintw(BOARD_Y + r * 3 + 1, BOARD_X + c * 6 + 4, "]");
                attroff(COLOR_PAIR(5) | A_BOLD);
            }
        }
    }

    // Draw files letters below
    for (int c = 0; c < 8; c++) {
        mvprintw(BOARD_Y + 24, BOARD_X + c * 6 + 2, "%c", 'a' + c);
    }

    // Right-hand Panel - Information
    int panel_x = BOARD_X + 54;
    mvprintw(BOARD_Y, panel_x, "Mac Terminal Chess");
    mvprintw(BOARD_Y + 1, panel_x, "===================");
    mvprintw(BOARD_Y + 3, panel_x, "Mode:   %s", engine_active ? "vs CPU (Stockfish)" : "2-Player Pass-and-Play");
    mvprintw(BOARD_Y + 4, panel_x, "Turn:   %s", active_color == 1 ? "WHITE (Bold/UPPERCASE)" : "BLACK (Lowercase)");

    // Game over status checks
    if (!has_legal_moves(active_color)) {
        if (is_in_check(active_color)) {
            mvprintw(BOARD_Y + 6, panel_x, "STATUS: CHECKMATE! %s wins.", active_color == 1 ? "Black" : "White");
        } else {
            mvprintw(BOARD_Y + 6, panel_x, "STATUS: STALEMATE!");
        }
    } else if (is_in_check(active_color)) {
        mvprintw(BOARD_Y + 6, panel_x, "STATUS: CHECK!");
    } else {
        mvprintw(BOARD_Y + 6, panel_x, "STATUS: Ready.");
    }

    // Help documentation
    mvprintw(BOARD_Y + 8, panel_x, "CONTROLS:");
    mvprintw(BOARD_Y + 9, panel_x, " Arrows : Navigate board");
    mvprintw(BOARD_Y + 10, panel_x, " Space  : Select / Move");
    mvprintw(BOARD_Y + 11, panel_x, " 'u'    : Undo last move");
    mvprintw(BOARD_Y + 12, panel_x, " 'r'    : Restart Match");
    mvprintw(BOARD_Y + 13, panel_x, " 'q'    : Quit application");

    // Dynamic Move PGN printout
    mvprintw(BOARD_Y + 15, panel_x, "PGN RECORD:");
    mvprintw(BOARD_Y + 16, panel_x, "-------------------");
    int row_draw = BOARD_Y + 17;
    for (int i = 0; i < history_count; i += 2) {
        if (row_draw > BOARD_Y + 24) {
            mvprintw(row_draw, panel_x, "... more moves");
            break;
        }
        if (i + 1 < history_count) {
            mvprintw(row_draw, panel_x, "%3d. %-8s %-8s", (i / 2) + 1, history[i].san, history[i + 1].san);
        } else {
            mvprintw(row_draw, panel_x, "%3d. %-8s", (i / 2) + 1, history[i].san);
        }
        row_draw++;
    }

    refresh();
}

int main(int argc, char **argv) {
    if (argc > 1) {
        strncpy(engine_path, argv[1], sizeof(engine_path) - 1);
    }

    // Setup program cleanup handles
    atexit(cleanup);

    // Initialize Screen Graphic context
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0); // Hide physical cursor
    start_color();

    // Assign UI color schemes
    init_pair(1, COLOR_BLACK, COLOR_WHITE);     // Light square
    init_pair(2, COLOR_WHITE, COLOR_CYAN);      // Dark square
    init_pair(3, COLOR_BLACK, COLOR_GREEN);     // Selection Highlight
    init_pair(4, COLOR_WHITE, COLOR_RED);       // Move alerts/Check
    init_pair(5, COLOR_YELLOW, COLOR_BLACK);    // Select border pointer

    // Splash Engine start loader
    mvprintw(12, 30, "Connecting to UCI Engine: '%s'...", engine_path);
    refresh();
    init_uci();

    reset_board();

    while (1) {
        draw_interface();

        // Check and parse engine response on cpu's turn
        if (engine_active && active_color == -1 && has_legal_moves(active_color)) {
            mvprintw(BOARD_Y + 6, BOARD_X + 54, "STATUS: Engine is thinking...");
            refresh();

            char cmd[4096] = "position startpos moves";
            for (int i = 0; i < history_count; i++) {
                strcat(cmd, " ");
                strcat(cmd, history[i].uci);
            }
            send_to_engine(cmd);
            send_to_engine("go movetime 1000"); // Request move response within 1 second

            char line[1024];
            while (1) {
                if (read_line_timeout(line, sizeof(line), 5) > 0) {
                    if (strncmp(line, "bestmove ", 9) == 0) {
                        char move_str[6];
                        sscanf(line + 9, "%s", move_str);
                        if (strcmp(move_str, "(none)") != 0 && strlen(move_str) >= 4) {
                            int eng_from = (move_str[0] - 'a') + (8 - (move_str[1] - '0')) * 8;
                            int eng_to = (move_str[2] - 'a') + (8 - (move_str[3] - '0')) * 8;
                            last_move_from = eng_from;
                            last_move_to = eng_to;
                            do_move(eng_from, eng_to, 1);
                        }
                        break;
                    }
                } else {
                    // Force PvP mode if the engine crashes or times out
                    engine_active = 0;
                    break;
                }
            }
            continue;
        }

        int ch = getch();
        int r = cursor_sq / 8;
        int c = cursor_sq % 8;

        if (ch == KEY_UP && r > 0) cursor_sq -= 8;
        else if (ch == KEY_DOWN && r < 7) cursor_sq += 8;
        else if (ch == KEY_LEFT && c > 0) cursor_sq -= 1;
        else if (ch == KEY_RIGHT && c < 7) cursor_sq += 1;
        else if (ch == ' ' || ch == '\n' || ch == KEY_ENTER) {
            if (selected_sq == -1) {
                // Select a piece matching the current turn's color
                if (board[cursor_sq] != EMPTY && sign(board[cursor_sq]) == active_color) {
                    selected_sq = cursor_sq;
                }
            } else {
                if (cursor_sq == selected_sq) {
                    selected_sq = -1; // Deselect on same-square selection click
                } else if (is_move_legal(selected_sq, cursor_sq)) {
                    last_move_from = selected_sq;
                    last_move_to = cursor_sq;
                    do_move(selected_sq, cursor_sq, 1);
                    selected_sq = -1;
                } else {
                    // If illegal move, reset or transition selection if selecting another of player's own pieces
                    if (board[cursor_sq] != EMPTY && sign(board[cursor_sq]) == active_color) {
                        selected_sq = cursor_sq;
                    } else {
                        selected_sq = -1;
                    }
                }
            }
        } else if (ch == 'u' || ch == 'U') {
            undo_move();
        } else if (ch == 'r' || ch == 'R') {
            reset_board();
            history_count = 0;
        } else if (ch == 'q' || ch == 'Q') {
            break;
        }
    }

    cleanup();
    return 0;
}
