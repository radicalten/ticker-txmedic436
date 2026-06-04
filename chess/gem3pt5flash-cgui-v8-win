#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>

#define MAX_HISTORY 1000

typedef struct {
    char board[8][8];
    int white_to_move;
    int castle_K, castle_Q, castle_k, castle_q;
    char en_passant[3]; // e.g., "e3" or "-"
    char last_move[8];
    char pgn[16];
} BoardState;

// Global States
BoardState history[MAX_HISTORY];
int history_count = 0;

int cursor_row = 6;
int cursor_col = 4; // Start at e2
int selected_row = -1;
int selected_col = -1;

int engine_color = 2; // 0: Player vs Player, 1: Engine plays White, 2: Engine plays Black (default)
int engine_thinking = 0;
int running = 1;

int engine_in = -1;
int engine_out = -1;
pid_t engine_pid = -1;

struct termios orig_termios;

// ANSI escape codes
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\033[0m"); // Restore cursor and text settings
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide terminal cursor
}

void init_board() {
    BoardState initial = {
        .board = {
            {'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'},
            {'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'},
            {'.', '.', '.', '.', '.', '.', '.', '.'},
            {'.', '.', '.', '.', '.', '.', '.', '.'},
            {'.', '.', '.', '.', '.', '.', '.', '.'},
            {'.', '.', '.', '.', '.', '.', '.', '.'},
            {'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'},
            {'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'}
        },
        .white_to_move = 1,
        .castle_K = 1, .castle_Q = 1, .castle_k = 1, .castle_q = 1,
        .en_passant = "-",
        .last_move = "",
        .pgn = ""
    };
    history[0] = initial;
    history_count = 1;
}

void start_engine() {
    int pipe_to[2];
    int pipe_from[2];
    if (pipe(pipe_to) != 0 || pipe(pipe_from) != 0) return;

    engine_pid = fork();
    if (engine_pid == 0) {
        // Child process
        dup2(pipe_to[0], STDIN_FILENO);
        dup2(pipe_from[1], STDOUT_FILENO);
        close(pipe_to[1]);
        close(pipe_from[0]);

        char* paths[] = {
            "stockfish",
            "/opt/homebrew/bin/stockfish",   // Apple Silicon Mac default Homebrew path
            "/usr/local/bin/stockfish",     // Intel Mac default Homebrew path
            "./stockfish",
            NULL
        };

        for (int i = 0; paths[i] != NULL; i++) {
            char* args[] = {paths[i], NULL};
            execvp(paths[i], args);
        }
        exit(1); // Exit if no engine execution succeeds
    } else {
        // Parent process
        close(pipe_to[0]);
        close(pipe_from[1]);
        engine_in = pipe_to[1];
        engine_out = pipe_from[0];

        write(engine_in, "uci\n", 4);
        write(engine_in, "isready\n", 8);
    }
}

void stop_engine() {
    if (engine_pid > 0) {
        kill(engine_pid, SIGKILL);
    }
}

// Pseudo-legal move verification (keeps physical movements authentic to basic chess guidelines)
int is_move_pseudo_legal(int r1, int c1, int r2, int c2) {
    BoardState* state = &history[history_count - 1];
    char p = state->board[r1][c1];
    if (p == '.') return 0;

    // Check color turn matching
    if (state->white_to_move && !isupper(p)) return 0;
    if (!state->white_to_move && !islower(p)) return 0;

    char dest = state->board[r2][c2];
    if (dest != '.') {
        if (isupper(p) && isupper(dest)) return 0;
        if (islower(p) && islower(dest)) return 0;
    }

    int dr = r2 - r1;
    int dc = c2 - c1;
    int abs_dr = abs(dr);
    int abs_dc = abs(dc);

    switch (tolower(p)) {
        case 'p': {
            int dir = (isupper(p)) ? -1 : 1;
            int start_row = (isupper(p)) ? 6 : 1;
            if (dc == 0 && dr == dir && dest == '.') return 1;
            if (dc == 0 && r1 == start_row && dr == 2 * dir && dest == '.' && state->board[r1 + dir][c1] == '.') return 1;
            if (abs_dc == 1 && dr == dir) {
                if (dest != '.') return 1;
                // En Passant target check
                char ep_col = state->en_passant[0];
                char ep_row = state->en_passant[1];
                if (ep_col != '-') {
                    int ep_c = ep_col - 'a';
                    int ep_r = '8' - ep_row;
                    if (r2 == ep_r && c2 == ep_c) return 1;
                }
            }
            return 0;
        }
        case 'n':
            return (abs_dr == 2 && abs_dc == 1) || (abs_dr == 1 && abs_dc == 2);
        case 'b': {
            if (abs_dr != abs_dc) return 0;
            int step_r = (dr > 0) ? 1 : -1;
            int step_c = (dc > 0) ? 1 : -1;
            int r = r1 + step_r, c = c1 + step_c;
            while (r != r2 && c != c2) {
                if (state->board[r][c] != '.') return 0;
                r += step_r; c += step_c;
            }
            return 1;
        }
        case 'r': {
            if (dr != 0 && dc != 0) return 0;
            int step_r = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int step_c = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            int r = r1 + step_r, c = c1 + step_c;
            while (r != r2 || c != c2) {
                if (state->board[r][c] != '.') return 0;
                r += step_r; c += step_c;
            }
            return 1;
        }
        case 'q': {
            if (abs_dr != abs_dc && dr != 0 && dc != 0) return 0;
            int step_r = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int step_c = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            if (abs_dr == abs_dc) {
                step_r = (dr > 0) ? 1 : -1;
                step_c = (dc > 0) ? 1 : -1;
            }
            int r = r1 + step_r, c = c1 + step_c;
            while (r != r2 || c != c2) {
                if (state->board[r][c] != '.') return 0;
                r += step_r; c += step_c;
            }
            return 1;
        }
        case 'k': {
            if (abs_dr <= 1 && abs_dc <= 1) return 1;
            // Castling Check
            if (dr == 0 && abs_dc == 2) {
                if (isupper(p)) {
                    if (c2 == 6 && state->castle_K && state->board[7][5] == '.' && state->board[7][6] == '.') return 1;
                    if (c2 == 2 && state->castle_Q && state->board[7][3] == '.' && state->board[7][2] == '.' && state->board[7][1] == '.') return 1;
                } else {
                    if (c2 == 6 && state->castle_k && state->board[0][5] == '.' && state->board[0][6] == '.') return 1;
                    if (c2 == 2 && state->castle_q && state->board[0][3] == '.' && state->board[0][2] == '.' && state->board[0][1] == '.') return 1;
                }
            }
            return 0;
        }
    }
    return 0;
}

void apply_move(const char* move_str) {
    if (history_count >= MAX_HISTORY - 1) return;

    BoardState* prev = &history[history_count - 1];
    BoardState* next = &history[history_count];
    memcpy(next, prev, sizeof(BoardState));

    int c1 = move_str[0] - 'a';
    int r1 = '8' - move_str[1];
    int c2 = move_str[2] - 'a';
    int r2 = '8' - move_str[3];
    char promo = move_str[4];

    char p = next->board[r1][c1];
    next->board[r1][c1] = '.';

    // En Passant Capture Execute
    if ((p == 'P' || p == 'p') && (c1 != c2) && (next->board[r2][c2] == '.')) {
        next->board[r1][c2] = '.';
    }

    // Move Piece / Promotion
    if (promo) {
        next->board[r2][c2] = (p == 'P') ? toupper(promo) : promo;
    } else {
        next->board[r2][c2] = p;
    }

    // Castling execution
    if (p == 'K') {
        if (c1 == 4 && c2 == 6) { next->board[7][5] = 'R'; next->board[7][7] = '.'; }
        if (c1 == 4 && c2 == 2) { next->board[7][3] = 'R'; next->board[7][0] = '.'; }
    } else if (p == 'k') {
        if (c1 == 4 && c2 == 6) { next->board[0][5] = 'r'; next->board[0][7] = '.'; }
        if (c1 == 4 && c2 == 2) { next->board[0][3] = 'r'; next->board[0][0] = '.'; }
    }

    // Castling Rights Maintenance
    if (r1 == 7 && c1 == 4) { next->castle_K = 0; next->castle_Q = 0; }
    if (r1 == 0 && c1 == 4) { next->castle_k = 0; next->castle_q = 0; }
    if ((r1 == 7 && c1 == 7) || (r2 == 7 && c2 == 7)) next->castle_K = 0;
    if ((r1 == 7 && c1 == 0) || (r2 == 7 && c2 == 0)) next->castle_Q = 0;
    if ((r1 == 0 && c1 == 7) || (r2 == 0 && c2 == 7)) next->castle_k = 0;
    if ((r1 == 0 && c1 == 0) || (r2 == 0 && c2 == 0)) next->castle_q = 0;

    // Set next available En Passant target
    if (p == 'P' && r1 == 6 && r2 == 4) {
        next->en_passant[0] = 'a' + c1;
        next->en_passant[1] = '3';
        next->en_passant[2] = '\0';
    } else if (p == 'p' && r1 == 1 && r2 == 3) {
        next->en_passant[0] = 'a' + c1;
        next->en_passant[1] = '6';
        next->en_passant[2] = '\0';
    } else {
        strcpy(next->en_passant, "-");
    }

    next->white_to_move = !next->white_to_move;
    strcpy(next->last_move, move_str);

    // Compute standard SAN (Standard Algebraic Notation) / PGN Output
    char capture_char = (prev->board[r2][c2] != '.' || ((p == 'P' || p == 'p') && c1 != c2)) ? 'x' : '\0';
    char pgn_tmp[16];

    if (p == 'K' || p == 'k') {
        if (c1 == 4 && c2 == 6) strcpy(pgn_tmp, "O-O");
        else if (c1 == 4 && c2 == 2) strcpy(pgn_tmp, "O-O-O");
        else {
            if (capture_char) sprintf(pgn_tmp, "Kx%c%d", 'a' + c2, 8 - r2);
            else sprintf(pgn_tmp, "K%c%d", 'a' + c2, 8 - r2);
        }
    } else if (p == 'P' || p == 'p') {
        if (capture_char) {
            sprintf(pgn_tmp, "%cx%c%d", 'a' + c1, 'a' + c2, 8 - r2);
        } else {
            sprintf(pgn_tmp, "%c%d", 'a' + c2, 8 - r2);
        }
        if (promo) {
            int len = strlen(pgn_tmp);
            pgn_tmp[len] = '=';
            pgn_tmp[len + 1] = toupper(promo);
            pgn_tmp[len + 2] = '\0';
        }
    } else {
        char piece_char = toupper(p);
        if (capture_char) {
            sprintf(pgn_tmp, "%cx%c%d", piece_char, 'a' + c2, 8 - r2);
        } else {
            sprintf(pgn_tmp, "%c%c%d", piece_char, 'a' + c2, 8 - r2);
        }
    }
    strcpy(next->pgn, pgn_tmp);

    history_count++;
}

void send_position_to_engine() {
    if (engine_in == -1) return;
    char cmd[16384] = "position startpos moves";
    for (int i = 1; i < history_count; i++) {
        strcat(cmd, " ");
        strcat(cmd, history[i].last_move);
    }
    strcat(cmd, "\n");
    write(engine_in, cmd, strlen(cmd));
}

void trigger_engine() {
    if (engine_in == -1 || engine_thinking) return;
    engine_thinking = 1;
    send_position_to_engine();
    write(engine_in, "go movetime 1000\n", 17); // 1-second calculations
}

void process_engine_line(const char* line) {
    if (strncmp(line, "bestmove", 8) == 0) {
        char move[16];
        if (sscanf(line, "bestmove %s", move) == 1) {
            if (strcmp(move, "(none)") != 0 && strcmp(move, "NULL") != 0) {
                apply_move(move);
            }
            engine_thinking = 0;
        }
    }
}

void read_engine_output() {
    static char engine_buf[4096];
    static int engine_buf_len = 0;
    char tmp[1024];

    int n = read(engine_out, tmp, sizeof(tmp) - 1);
    if (n <= 0) return;
    tmp[n] = '\0';

    if (engine_buf_len + n < (int)sizeof(engine_buf)) {
        memcpy(engine_buf + engine_buf_len, tmp, n);
        engine_buf_len += n;
        engine_buf[engine_buf_len] = '\0';
    }

    char* line_start = engine_buf;
    char* newline;
    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';
        process_engine_line(line_start);
        line_start = newline + 1;
    }

    int consumed = line_start - engine_buf;
    if (consumed > 0) {
        memmove(engine_buf, line_start, engine_buf_len - consumed);
        engine_buf_len -= consumed;
        engine_buf[engine_buf_len] = '\0';
    }
}

void undo_move() {
    if (engine_thinking) return;
    // When playing against the engine, standard undo rolls back both calculations and human moves
    int steps = (engine_color == 1 || engine_color == 2) ? 2 : 1;
    if (history_count > steps) {
        history_count -= steps;
        selected_row = -1;
        selected_col = -1;
    }
}

void draw_game() {
    BoardState* state = &history[history_count - 1];

    // Clear Screen, reset cursor coordinate
    printf("\033[H\033[2J");
    printf("\n    CHESS INTERACTIVE TERMINAL GUI (Mac OS)\n\n");

    // Board Structure Rendering
    for (int r = 0; r < 8; r++) {
        printf("  %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int is_dark = (r + c) % 2 != 0;
            int is_cursor = (r == cursor_row && c == cursor_col);
            int is_selected = (r == selected_row && c == selected_col);

            if (is_cursor) {
                printf("\033[48;5;196m"); // Red Cursor block
            } else if (is_selected) {
                printf("\033[48;5;27m");  // Blue Selection highlight
            } else if (is_dark) {
                printf("\033[48;5;94m");  // Wood Dark Brown
            } else {
                printf("\033[48;5;180m"); // Wood Light Beige
            }

            char p = state->board[r][c];
            if (p == '.') {
                printf("   ");
            } else {
                if (isupper(p)) {
                    printf("\033[1;97m %c ", p); // White piece (Bold White)
                } else {
                    printf("\033[1;30m %c ", toupper(p)); // Black piece (Bold Black)
                }
            }
            printf("\033[0m"); // Reset Formatting
        }
        printf(" %d\n", 8 - r);
    }
    printf("     a  b  c  d  e  f  g  h\n\n");

    // Metadata Display Panel
    printf("  Turn: %s\n", state->white_to_move ? "\033[1;33mWhite\033[0m" : "\033[1;36mBlack\033[0m");

    char mode_str[32];
    if (engine_color == 0) strcpy(mode_str, "Player vs Player");
    else if (engine_color == 1) strcpy(mode_str, "Engine plays White");
    else if (engine_color == 2) strcpy(mode_str, "Engine plays Black");
    else if (engine_color == 3) strcpy(mode_str, "Engine vs Engine");

    printf("  Mode: %s [C to cycle]\n", mode_str);

    if (engine_pid != -1) {
        printf("  Engine: \033[1;32mConnected\033[0m %s\n", engine_thinking ? "(\033[5;33mThinking...\033[0m)" : "");
    } else {
        printf("  Engine: \033[1;31mNot Found (Running Offline Mode)\033[0m\n");
    }

    // PGN Moves Stream Output
    printf("\n  PGN: ");
    int move_num = 1;
    for (int i = 1; i < history_count; i++) {
        if (i % 2 != 0) {
            printf("%d. %s ", move_num, history[i].pgn);
            move_num++;
        } else {
            printf("%s ", history[i].pgn);
        }
    }
    printf("\n\n");

    printf("  \033[1;30mControls:\033[0m\n");
    printf("  [Arrows] Navigate   [Space/Enter] Select/Move   [U] Undo\n");
    printf("  [C] Change Mode     [E] Force Engine Play       [Q] Quit\n");
    fflush(stdout);
}

void handle_selection() {
    BoardState* state = &history[history_count - 1];
    if (selected_row == -1) {
        char p = state->board[cursor_row][cursor_col];
        if (p != '.') {
            if ((state->white_to_move && isupper(p)) || (!state->white_to_move && islower(p))) {
                selected_row = cursor_row;
                selected_col = cursor_col;
            }
        }
    } else {
        if (selected_row == cursor_row && selected_col == cursor_col) {
            selected_row = -1;
            selected_col = -1;
        } else {
            if (is_move_pseudo_legal(selected_row, selected_col, cursor_row, cursor_col)) {
                char move_str[6];
                move_str[0] = 'a' + selected_col;
                move_str[1] = '8' - selected_row;
                move_str[2] = 'a' + cursor_col;
                move_str[3] = '8' - cursor_row;

                // Auto-Queen Promotion execution on raw backranks
                char p = state->board[selected_row][selected_col];
                if ((p == 'P' && cursor_row == 0) || (p == 'p' && cursor_row == 7)) {
                    move_str[4] = 'q';
                    move_str[5] = '\0';
                } else {
                    move_str[4] = '\0';
                }

                apply_move(move_str);
                selected_row = -1;
                selected_col = -1;

                // Fire automated engine response
                int engine_must_move = (engine_color == 1 && history[history_count - 1].white_to_move) ||
                                       (engine_color == 2 && !history[history_count - 1].white_to_move) ||
                                       (engine_color == 3);
                if (engine_must_move) {
                    trigger_engine();
                }
            } else {
                selected_row = -1;
                selected_col = -1;
            }
        }
    }
}

void handle_key() {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return;

    if (c == 27) { // ANSI Arrow key escape processing
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return;
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return;

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': if (cursor_row > 0) cursor_row--; break; // Up
                case 'B': if (cursor_row < 7) cursor_row++; break; // Down
                case 'C': if (cursor_col < 7) cursor_col++; break; // Right
                case 'D': if (cursor_col > 0) cursor_col--; break; // Left
            }
        }
    } else if (c == ' ' || c == '\n' || c == '\r') {
        handle_selection();
    } else if (c == 'u' || c == 'U') {
        undo_move();
    } else if (c == 'e' || c == 'E') {
        trigger_engine();
    } else if (c == 'c' || c == 'C') {
        engine_color = (engine_color + 1) % 4;
        selected_row = -1;
        selected_col = -1;
        int engine_must_move = (engine_color == 1 && history[history_count - 1].white_to_move) ||
                               (engine_color == 2 && !history[history_count - 1].white_to_move) ||
                               (engine_color == 3);
        if (engine_must_move) {
            trigger_engine();
        }
    } else if (c == 'q' || c == 'Q') {
        running = 0;
    }
}

int main() {
    enable_raw_mode();
    init_board();
    start_engine();

    draw_game();

    // Trigger engine if starting configuration requires it
    if (engine_color == 1) {
        trigger_engine();
    }

    fd_set fds;
    while (running) {
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (engine_out != -1) {
            FD_SET(engine_out, &fds);
        }

        int max_fd = (engine_out > STDIN_FILENO) ? engine_out : STDIN_FILENO;
        int activity = select(max_fd + 1, &fds, NULL, NULL, NULL);

        if (activity < 0) continue;

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            handle_key();
            draw_game();
        }

        if (engine_out != -1 && FD_ISSET(engine_out, &fds)) {
            read_engine_output();
            draw_game();
            
            // Auto loop when Engine vs Engine is running
            if (engine_color == 3 && !engine_thinking) {
                trigger_engine();
            }
        }
    }

    stop_engine();
    disable_raw_mode();
    return 0;
}
