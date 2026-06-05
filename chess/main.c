#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#define KEY_UP 1001
#define KEY_DOWN 1002
#define KEY_RIGHT 1003
#define KEY_LEFT 1004
#define KEY_ENTER 1005
#define KEY_ESC 1006

// Base structures
typedef struct {
    int r, c;
} Pos;

typedef struct {
    Pos from;
    Pos to;
    int promotion; // 0 if none, else piece type (e.g., 5 for Queen)
} Move;

typedef struct {
    int board[8][8]; // 1:P, 2:N, 3:B, 4:R, 5:Q, 6:K (positive for White, negative for Black)
    int turn;        // 1 for White, -1 for Black
    int ep_row, ep_col;
    int w_king_side, w_queen_side;
    int b_king_side, b_queen_side;
} BoardState;

// Terminal manipulation
struct termios orig_termios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\e[?25h"); // Show terminal cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\e[?25l"); // Hide terminal cursor
}

int get_key() {
    char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;

    if (c == '\e') {
        char seq[3];
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

        int n2 = read(STDIN_FILENO, &seq[0], 1);
        int n3 = 0;
        if (n2 > 0) n3 = read(STDIN_FILENO, &seq[1], 1);

        fcntl(STDIN_FILENO, F_SETFL, flags); // Restore blocking

        if (n2 <= 0) return KEY_ESC;

        if (seq[0] == '[') {
            switch(seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return KEY_ESC;
    }

    if (c == '\r' || c == '\n' || c == ' ') return KEY_ENTER;
    if (c == 'q' || c == 'Q') return 'q';

    // WASD Fallback
    if (c == 'w' || c == 'W') return KEY_UP;
    if (c == 's' || c == 'S') return KEY_DOWN;
    if (c == 'd' || c == 'D') return KEY_RIGHT;
    if (c == 'a' || c == 'A') return KEY_LEFT;

    return c;
}

// Chess Engine Rules & Math
int is_attacked(BoardState *state, int tr, int tc, int attacker_color) {
    // Knight attacks
    int dr_n[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int dc_n[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int r = tr + dr_n[i], c = tc + dc_n[i];
        if (r >= 0 && r < 8 && c >= 0 && c < 8) {
            int p = state->board[r][c];
            if (abs(p) == 2 && ((p > 0) ? 1 : -1) == attacker_color) return 1;
        }
    }

    // Pawn attacks
    int p_dir = (attacker_color == 1) ? 1 : -1;
    int pawn_r = tr + p_dir;
    for (int dc = -1; dc <= 1; dc += 2) {
        int pawn_c = tc + dc;
        if (pawn_r >= 0 && pawn_r < 8 && pawn_c >= 0 && pawn_c < 8) {
            int p = state->board[pawn_r][pawn_c];
            if (abs(p) == 1 && ((p > 0) ? 1 : -1) == attacker_color) return 1;
        }
    }

    // King attacks
    int dr_k[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int dc_k[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int r = tr + dr_k[i], c = tc + dc_k[i];
        if (r >= 0 && r < 8 && c >= 0 && c < 8) {
            int p = state->board[r][c];
            if (abs(p) == 6 && ((p > 0) ? 1 : -1) == attacker_color) return 1;
        }
    }

    // Sliding attacks (Bishop/Queen)
    int dr_b[] = {-1, -1, 1, 1};
    int dc_b[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        for (int d = 1; d < 8; d++) {
            int r = tr + dr_b[i]*d, c = tc + dc_b[i]*d;
            if (r >= 0 && r < 8 && c >= 0 && c < 8) {
                int p = state->board[r][c];
                if (p != 0) {
                    if (((p > 0) ? 1 : -1) == attacker_color && (abs(p) == 3 || abs(p) == 5)) return 1;
                    break;
                }
            } else break;
        }
    }

    // Sliding attacks (Rook/Queen)
    int dr_r[] = {-1, 1, 0, 0};
    int dc_r[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        for (int d = 1; d < 8; d++) {
            int r = tr + dr_r[i]*d, c = tc + dc_r[i]*d;
            if (r >= 0 && r < 8 && c >= 0 && c < 8) {
                int p = state->board[r][c];
                if (p != 0) {
                    if (((p > 0) ? 1 : -1) == attacker_color && (abs(p) == 4 || abs(p) == 5)) return 1;
                    break;
                }
            } else break;
        }
    }

    return 0;
}

int is_king_in_check(BoardState *state, int color) {
    int kr = -1, kc = -1;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (state->board[r][c] == color * 6) {
                kr = r; kc = c;
                break;
            }
        }
    }
    if (kr == -1) return 0;
    return is_attacked(state, kr, kc, -color);
}

int get_pseudo_moves(BoardState *state, int r, int c, Move moves[]) {
    int count = 0;
    int p = state->board[r][c];
    int type = abs(p);
    int color = (p > 0) ? 1 : -1;

    if (type == 1) { // Pawn
        int dir = -color;
        int start_row = (color == 1) ? 6 : 1;
        int next_r = r + dir;
        
        if (next_r >= 0 && next_r < 8 && state->board[next_r][c] == 0) {
            if (next_r == 0 || next_r == 7) {
                moves[count++] = (Move){{r, c}, {next_r, c}, 5}; // Promote to Queen
            } else {
                moves[count++] = (Move){{r, c}, {next_r, c}, 0};
            }
            int double_r = r + 2 * dir;
            if (r == start_row && state->board[double_r][c] == 0) {
                moves[count++] = (Move){{r, c}, {double_r, c}, 0};
            }
        }
        for (int dc = -1; dc <= 1; dc += 2) {
            int tc = c + dc;
            if (tc >= 0 && tc < 8) {
                int tr = r + dir;
                if (tr >= 0 && tr < 8) {
                    int target_p = state->board[tr][tc];
                    if (target_p != 0 && ((target_p > 0) != (p > 0))) {
                        if (tr == 0 || tr == 7) {
                            moves[count++] = (Move){{r, c}, {tr, tc}, 5};
                        } else {
                            moves[count++] = (Move){{r, c}, {tr, tc}, 0};
                        }
                    }
                    if (tr == state->ep_row && tc == state->ep_col) {
                        moves[count++] = (Move){{r, c}, {tr, tc}, 0};
                    }
                }
            }
        }
    }
    else if (type == 2) { // Knight
        int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
        int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; i++) {
            int tr = r + dr[i], tc = c + dc[i];
            if (tr >= 0 && tr < 8 && tc >= 0 && tc < 8) {
                if (state->board[tr][tc] == 0 || ((state->board[tr][tc] > 0) != (p > 0))) {
                    moves[count++] = (Move){{r, c}, {tr, tc}, 0};
                }
            }
        }
    }
    else if (type == 3 || type == 5) { // Bishop or Queen
        int dr[] = {-1, -1, 1, 1};
        int dc[] = {-1, 1, -1, 1};
        for (int i = 0; i < 4; i++) {
            for (int d = 1; d < 8; d++) {
                int tr = r + dr[i]*d, tc = c + dc[i]*d;
                if (tr >= 0 && tr < 8 && tc >= 0 && tc < 8) {
                    if (state->board[tr][tc] == 0) {
                        moves[count++] = (Move){{r, c}, {tr, tc}, 0};
                    } else {
                        if ((state->board[tr][tc] > 0) != (p > 0)) {
                            moves[count++] = (Move){{r, c}, {tr, tc}, 0};
                        }
                        break;
                    }
                } else break;
            }
        }
    }
    if (type == 4 || type == 5) { // Rook or Queen
        int dr[] = {-1, 1, 0, 0};
        int dc[] = {0, 0, -1, 1};
        for (int i = 0; i < 4; i++) {
            for (int d = 1; d < 8; d++) {
                int tr = r + dr[i]*d, tc = c + dc[i]*d;
                if (tr >= 0 && tr < 8 && tc >= 0 && tc < 8) {
                    if (state->board[tr][tc] == 0) {
                        moves[count++] = (Move){{r, c}, {tr, tc}, 0};
                    } else {
                        if ((state->board[tr][tc] > 0) != (p > 0)) {
                            moves[count++] = (Move){{r, c}, {tr, tc}, 0};
                        }
                        break;
                    }
                } else break;
            }
        }
    }
    else if (type == 6) { // King
        int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
        int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        for (int i = 0; i < 8; i++) {
            int tr = r + dr[i], tc = c + dc[i];
            if (tr >= 0 && tr < 8 && tc >= 0 && tc < 8) {
                if (state->board[tr][tc] == 0 || ((state->board[tr][tc] > 0) != (p > 0))) {
                    moves[count++] = (Move){{r, c}, {tr, tc}, 0};
                }
            }
        }
        // Castling
        if (color == 1) {
            if (state->w_king_side && state->board[7][5] == 0 && state->board[7][6] == 0) {
                moves[count++] = (Move){{7, 4}, {7, 6}, 0};
            }
            if (state->w_queen_side && state->board[7][1] == 0 && state->board[7][2] == 0 && state->board[7][3] == 0) {
                moves[count++] = (Move){{7, 4}, {7, 2}, 0};
            }
        } else {
            if (state->b_king_side && state->board[0][5] == 0 && state->board[0][6] == 0) {
                moves[count++] = (Move){{0, 4}, {0, 6}, 0};
            }
            if (state->b_queen_side && state->board[0][1] == 0 && state->board[0][2] == 0 && state->board[0][3] == 0) {
                moves[count++] = (Move){{0, 4}, {0, 2}, 0};
            }
        }
    }
    return count;
}

void make_move(BoardState *state, Move m) {
    int from_r = m.from.r, from_c = m.from.c;
    int to_r = m.to.r, to_c = m.to.c;
    int p = state->board[from_r][from_c];

    // En Passant Capture
    if (abs(p) == 1 && to_r == state->ep_row && to_c == state->ep_col) {
        state->board[from_r][to_c] = 0;
    }

    state->ep_row = -1;
    state->ep_col = -1;

    // Set new En Passant target square
    if (abs(p) == 1 && abs(from_r - to_r) == 2) {
        state->ep_row = (from_r + to_r) / 2;
        state->ep_col = from_c;
    }

    // Castling execution
    if (abs(p) == 6) {
        if (from_c == 4) {
            if (to_c == 6) {
                state->board[from_r][5] = state->board[from_r][7];
                state->board[from_r][7] = 0;
            } else if (to_c == 2) {
                state->board[from_r][3] = state->board[from_r][0];
                state->board[from_r][0] = 0;
            }
        }
    }

    state->board[to_r][to_c] = p;
    state->board[from_r][from_c] = 0;

    // Pawn Promotion
    if (abs(p) == 1 && (to_r == 0 || to_r == 7)) {
        state->board[to_r][to_c] = (p > 0) ? m.promotion : -m.promotion;
    }

    // Update Castling rights
    if (p == 6) {
        state->w_king_side = 0;
        state->w_queen_side = 0;
    } else if (p == -6) {
        state->b_king_side = 0;
        state->b_queen_side = 0;
    }

    if (p == 4 && from_r == 7 && from_c == 7) state->w_king_side = 0;
    if (p == 4 && from_r == 7 && from_c == 0) state->w_queen_side = 0;
    if (p == -4 && from_r == 0 && from_c == 7) state->b_king_side = 0;
    if (p == -4 && from_r == 0 && from_c == 0) state->b_queen_side = 0;

    if (to_r == 7 && to_c == 7) state->w_king_side = 0;
    if (to_r == 7 && to_c == 0) state->w_queen_side = 0;
    if (to_r == 0 && to_c == 7) state->b_king_side = 0;
    if (to_r == 0 && to_c == 0) state->b_queen_side = 0;

    state->turn = -state->turn;
}

int get_legal_moves(BoardState *state, int r, int c, Move legal[]) {
    int p = state->board[r][c];
    if (p == 0 || ((p > 0) ? 1 : -1) != state->turn) return 0;

    Move pseudo[100];
    int pseudo_count = get_pseudo_moves(state, r, c, pseudo);
    int legal_count = 0;

    for (int i = 0; i < pseudo_count; i++) {
        if (abs(p) == 6 && abs(pseudo[i].from.c - pseudo[i].to.c) == 2) {
            // Cannot castle out of, through, or into check
            if (is_king_in_check(state, state->turn)) continue;

            if (pseudo[i].to.c == 6) {
                BoardState temp = *state;
                temp.board[r][5] = temp.board[r][4];
                temp.board[r][4] = 0;
                if (is_king_in_check(&temp, state->turn)) continue;
            }
            if (pseudo[i].to.c == 2) {
                BoardState temp = *state;
                temp.board[r][3] = temp.board[r][4];
                temp.board[r][4] = 0;
                if (is_king_in_check(&temp, state->turn)) continue;
            }
        }

        BoardState temp = *state;
        make_move(&temp, pseudo[i]);
        if (!is_king_in_check(&temp, state->turn)) {
            legal[legal_count++] = pseudo[i];
        }
    }
    return legal_count;
}

int has_any_legal_moves(BoardState *state) {
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int p = state->board[r][c];
            if (p != 0 && ((p > 0) ? 1 : -1) == state->turn) {
                Move moves[100];
                if (get_legal_moves(state, r, c, moves) > 0) return 1;
            }
        }
    }
    return 0;
}

// UCI coordinates translation helpers
void format_move(Move m, char *buf) {
    buf[0] = 'a' + m.from.c;
    buf[1] = '8' - m.from.r;
    buf[2] = 'a' + m.to.c;
    buf[3] = '8' - m.to.r;
    if (m.promotion != 0) {
        switch(m.promotion) {
            case 2: buf[4] = 'n'; break;
            case 3: buf[4] = 'b'; break;
            case 4: buf[4] = 'r'; break;
            case 5: buf[4] = 'q'; break;
        }
        buf[5] = '\0';
    } else {
        buf[4] = '\0';
    }
}

Move parse_move(const char *str) {
    Move m;
    m.from.c = str[0] - 'a';
    m.from.r = '8' - str[1];
    m.to.c = str[2] - 'a';
    m.to.r = '8' - str[3];
    m.promotion = 0;
    if (str[4] != '\0' && str[4] != ' ' && str[4] != '\n') {
        switch(str[4]) {
            case 'n': m.promotion = 2; break;
            case 'b': m.promotion = 3; break;
            case 'r': m.promotion = 4; break;
            case 'q': m.promotion = 5; break;
        }
    }
    return m;
}

// GUI Drawing Engine
void draw_game(BoardState *state, int cursor_r, int cursor_c, int sel_r, int sel_c, Move last_move, Move legal_moves[], int legal_count, const char *engine_status, const char *engine_eval, const char *game_status) {
    printf("\e[H"); // Reset cursor home

    // Header Panel
    printf("\n\e[1;35m   🏆  TERMINAL LICHESS  🏆 \e[0m (Use WASD/Arrows, Space/Enter to select, Esc to clear)\n\n");

    for (int r = 0; r < 8; r++) {
        for (int sub_r = 0; sub_r < 3; sub_r++) {
            if (sub_r == 1) {
                printf("  %d ", 8 - r);
            } else {
                printf("    ");
            }

            for (int c = 0; c < 8; c++) {
                int p = state->board[r][c];
                int is_light = (r + c) % 2 == 0;

                // Color Board Selection
                const char *bg_esc = is_light ? "\e[48;5;253m" : "\e[48;5;240m";

                if (r == sel_r && c == sel_c) {
                    bg_esc = "\e[48;5;75m"; // Light Blue for current selection
                } else if ((last_move.from.r == r && last_move.from.c == c) ||
                           (last_move.to.r == r && last_move.to.c == c)) {
                    bg_esc = "\e[48;5;143m"; // Moss green highlight for last moves
                }

                // Check target availability
                int is_target = 0;
                for (int i = 0; i < legal_count; i++) {
                    if (legal_moves[i].to.r == r && legal_moves[i].to.c == c) {
                        is_target = 1;
                        break;
                    }
                }

                printf("%s", bg_esc);

                // Start of square boundary padding
                if (r == cursor_r && c == cursor_c && sub_r == 1) {
                    printf("\e[1;36m[\e[0m%s", bg_esc); // Cyan Cursor indicators
                } else {
                    printf(" ");
                }

                printf(" ");

                // Inner content rendering
                if (sub_r == 1) {
                    if (p != 0) {
                        const char *fg_esc = (p > 0) ? "\e[38;5;231m\e[1m" : "\e[38;5;235m\e[1m"; // Crisp white / sleek black pieces
                        const char *sym = " ";
                        switch(abs(p)) {
                            case 1: sym = "♟"; break;
                            case 2: sym = "♞"; break;
                            case 3: sym = "♝"; break;
                            case 4: sym = "♜"; break;
                            case 5: sym = "♛"; break;
                            case 6: sym = "♚"; break;
                        }
                        printf("%s%s\e[0m%s", fg_esc, sym, bg_esc);
                    } else if (is_target) {
                        printf("\e[38;5;112m●\e[0m%s", bg_esc); // Bright green dot for move suggestion
                    } else {
                        printf(" ");
                    }
                } else {
                    printf(" ");
                }

                printf(" ");

                if (r == cursor_r && c == cursor_c && sub_r == 1) {
                    printf("\e[1;36m]\e[0m%s", bg_esc);
                } else {
                    printf(" ");
                }

                printf("\e[0m"); // Reset ANSI properties
            }

            // Info & Meta Sidebar
            if (sub_r == 1) {
                if (r == 0) printf("    \e[1;34m♟  PLAYER\e[0m: White (You)");
                if (r == 1) printf("    \e[1;31m⚙  ENGINE\e[0m: Black (%s)", engine_status);
                if (r == 2) printf("    \e[1;33m⚖  EVAL\e[0m: %s", engine_eval);
                if (r == 3) printf("    \e[1;32m⚡ STATUS\e[0m: %s", game_status);
                if (r == 4) printf("    \e[1;37mℹ  TURN\e[0m: %s", (state->turn == 1) ? "White's Move" : "Black's Move");
                if (r == 5) printf("    \e[1;36m🏰 CASTLING\e[0m: W[%c%c] B[%c%c]",
                                   state->w_king_side ? 'K' : '-', state->w_queen_side ? 'Q' : '-',
                                   state->b_king_side ? 'k' : '-', state->b_queen_side ? 'q' : '-');
                if (r == 6) printf("    \e[1;35m⭐ COMMANDS\e[0m: Arrow keys / WASD to move cursor.");
                if (r == 7) printf("                 Enter/Space to select. 'q' to forfeit.");
            }
            printf("\n");
        }
    }

    // Horizontal ranks indexes
    printf("     ");
    for (int c = 0; c < 8; c++) {
        printf("  %c   ", 'A' + c);
    }
    printf("\n\e[J"); // Flush screen cleanly
}

// UCI Engine Subprocess Pipeline (POSIX fork/pipes)
int start_engine(const char *executable, int *to_engine_fd, int *from_engine_fd, pid_t *pid) {
    int pipe_in[2];
    int pipe_out[2];

    if (pipe(pipe_in) != 0 || pipe(pipe_out) != 0) return 0;

    pid_t p = fork();
    if (p < 0) return 0;

    if (p == 0) { // Child exec path
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);

        // Silent stderr
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        close(pipe_in[0]);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_out[1]);

        execlp(executable, executable, (char*)NULL);
        exit(1);
    }

    // Parent path
    close(pipe_in[0]);
    close(pipe_out[1]);

    // Make engine output non-blocking
    fcntl(pipe_out[0], F_SETFL, O_NONBLOCK);

    *to_engine_fd = pipe_in[1];
    *from_engine_fd = pipe_out[0];
    *pid = p;

    // Send UCI handshakes
    write(pipe_in[1], "uci\nisready\n", 12);
    return 1;
}

char engine_buffer[4096];
int engine_buf_len = 0;

void process_engine_output(char *line, BoardState *state, Move *last_move, char *eval_str, int *engine_thinking, char *move_history_str) {
    if (strstr(line, "bestmove") == line) {
        char move_str[10];
        if (sscanf(line, "bestmove %s", move_str) == 1) {
            if (strcmp(move_str, "(none)") != 0) {
                Move m = parse_move(move_str);
                Move legal[100];
                int count = get_legal_moves(state, m.from.r, m.from.c, legal);
                int valid = 0;
                for (int i = 0; i < count; i++) {
                    if (legal[i].to.r == m.to.r && legal[i].to.c == m.to.c) {
                        valid = 1;
                        m = legal[i];
                        break;
                    }
                }
                if (valid) {
                    make_move(state, m);
                    *last_move = m;

                    // Append to game move history
                    char m_str[16];
                    format_move(m, m_str);
                    if (strlen(move_history_str) > 0) {
                        strcat(move_history_str, " ");
                        strcat(move_history_str, m_str);
                    } else {
                        strcpy(move_history_str, m_str);
                    }
                }
            }
            *engine_thinking = 0;
        }
    } else if (strstr(line, "info ") != NULL) {
        char *score_pos = strstr(line, "score cp ");
        if (score_pos != NULL) {
            int cp;
            if (sscanf(score_pos, "score cp %d", &cp) == 1) {
                float val = cp / 100.0f;
                if (state->turn == -1) val = -val; // Show static absolute evaluation
                snprintf(eval_str, 32, "%+.2f", val);
            }
        } else {
            char *mate_pos = strstr(line, "score mate ");
            if (mate_pos != NULL) {
                int mate;
                if (sscanf(mate_pos, "score mate %d", &mate) == 1) {
                    snprintf(eval_str, 32, "Mate in %d", abs(mate));
                }
            }
        }
    }
}

void read_engine_pipes(int fd, BoardState *state, Move *last_move, char *eval_str, int *engine_thinking, char *move_history_str) {
    char chunk[1024];
    int n = read(fd, chunk, sizeof(chunk) - 1);
    if (n <= 0) return;
    chunk[n] = '\0';

    for (int i = 0; i < n; i++) {
        if (engine_buf_len < sizeof(engine_buffer) - 2) {
            engine_buffer[engine_buf_len++] = chunk[i];
            if (chunk[i] == '\n') {
                engine_buffer[engine_buf_len] = '\0';
                process_engine_output(engine_buffer, state, last_move, eval_str, engine_thinking, move_history_str);
                engine_buf_len = 0;
            }
        } else {
            engine_buf_len = 0;
        }
    }
}

int main() {
    BoardState state;
    int initial_board[8][8] = {
        {-4, -2, -3, -5, -6, -3, -2, -4},
        {-1, -1, -1, -1, -1, -1, -1, -1},
        { 0,  0,  0,  0,  0,  0,  0,  0},
        { 0,  0,  0,  0,  0,  0,  0,  0},
        { 0,  0,  0,  0,  0,  0,  0,  0},
        { 0,  0,  0,  0,  0,  0,  0,  0},
        { 1,  1,  1,  1,  1,  1,  1,  1},
        { 4,  2,  3,  5,  6,  3,  2,  4}
    };
    memcpy(state.board, initial_board, sizeof(initial_board));
    state.turn = 1;
    state.ep_row = -1; state.ep_col = -1;
    state.w_king_side = 1; state.w_queen_side = 1;
    state.b_king_side = 1; state.b_queen_side = 1;

    int cursor_r = 6, cursor_c = 4;
    int sel_r = -1, sel_c = -1;
    Move legal_moves[100];
    int legal_count = 0;
    Move last_move = {{-1,-1},{-1,-1},0};

    char eval_str[32] = "0.00";
    char game_status[64] = "Ready to play!";
    char engine_status[64] = "Connecting...";
    char move_history_str[4096] = "";
    int engine_thinking = 0;
    int is_engine_active = 0;

    int to_engine_fd = -1, from_engine_fd = -1;
    pid_t engine_pid = -1;

    // Search targets for Mac OS binary installation
    const char* paths[] = {
        "stockfish",
        "/opt/homebrew/bin/stockfish",
        "/usr/local/bin/stockfish",
        "/usr/games/stockfish"
    };

    for (int i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        if (start_engine(paths[i], &to_engine_fd, &from_engine_fd, &engine_pid)) {
            is_engine_active = 1;
            snprintf(engine_status, sizeof(engine_status), "Connected (AI Active)");
            break;
        }
    }

    if (!is_engine_active) {
        snprintf(engine_status, sizeof(engine_status), "Not Found (Local PvP Active)");
    }

    enable_raw_mode();
    printf("\e[2J"); // Complete Screen Clear

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = is_engine_active ? from_engine_fd : -1;
    fds[1].events = POLLIN;

    while (1) {
        // Game-over assessment
        if (!has_any_legal_moves(&state)) {
            if (is_king_in_check(&state, state.turn)) {
                if (state.turn == 1) {
                    snprintf(game_status, sizeof(game_status), "CHECKMATE! Black wins.");
                } else {
                    snprintf(game_status, sizeof(game_status), "CHECKMATE! White wins.");
                }
            } else {
                snprintf(game_status, sizeof(game_status), "STALEMATE! Game is a draw.");
            }
        } else if (is_king_in_check(&state, state.turn)) {
            snprintf(game_status, sizeof(game_status), "CHECK!");
        } else {
            if (state.turn == 1) {
                snprintf(game_status, sizeof(game_status), "Your Move (White)");
            } else {
                snprintf(game_status, sizeof(game_status), "Engine Thinking...");
            }
        }

        draw_game(&state, cursor_r, cursor_c, sel_r, sel_c, last_move, legal_moves, legal_count, engine_status, eval_str, game_status);

        // Dispatch engine instructions if Engine Turn
        if (state.turn == -1 && is_engine_active && !engine_thinking) {
            engine_thinking = 1;
            char cmd[4096];
            if (strlen(move_history_str) > 0) {
                snprintf(cmd, sizeof(cmd), "position startpos moves %s\ngo depth 12\n", move_history_str);
            } else {
                snprintf(cmd, sizeof(cmd), "position startpos\ngo depth 12\n");
            }
            write(to_engine_fd, cmd, strlen(cmd));
        }

        int poll_ret = poll(fds, 2, 80);
        if (poll_ret < 0) break;

        // Process asynchronous Engine stream
        if (is_engine_active && (fds[1].revents & POLLIN)) {
            read_engine_pipes(from_engine_fd, &state, &last_move, eval_str, &engine_thinking, move_history_str);
        }

        // Process Player input
        if (fds[0].revents & POLLIN) {
            int key = get_key();
            if (key == 'q') {
                break;
            }
            if (key == KEY_ESC) {
                sel_r = -1; sel_c = -1;
                legal_count = 0;
            }
            else if (key == KEY_UP && cursor_r > 0) cursor_r--;
            else if (key == KEY_DOWN && cursor_r < 7) cursor_r++;
            else if (key == KEY_LEFT && cursor_c > 0) cursor_c--;
            else if (key == KEY_RIGHT && cursor_c < 7) cursor_c++;
            else if (key == KEY_ENTER) {
                // Ignore user plays during AI processing
                if (is_engine_active && state.turn == -1) {
                    continue;
                }

                if (sel_r == -1) {
                    int p = state.board[cursor_r][cursor_c];
                    if (p != 0 && ((p > 0) ? 1 : -1) == state.turn) {
                        sel_r = cursor_r;
                        sel_c = cursor_c;
                        legal_count = get_legal_moves(&state, sel_r, sel_c, legal_moves);
                    }
                } else {
                    int is_valid = 0;
                    Move selected_move;
                    for (int i = 0; i < legal_count; i++) {
                        if (legal_moves[i].to.r == cursor_r && legal_moves[i].to.c == cursor_c) {
                            is_valid = 1;
                            selected_move = legal_moves[i];
                            break;
                        }
                    }

                    if (is_valid) {
                        char m_str[16];
                        format_move(selected_move, m_str);
                        if (strlen(move_history_str) > 0) {
                            strcat(move_history_str, " ");
                            strcat(move_history_str, m_str);
                        } else {
                            strcpy(move_history_str, m_str);
                        }

                        make_move(&state, selected_move);
                        last_move = selected_move;

                        sel_r = -1; sel_c = -1;
                        legal_count = 0;
                    } else {
                        // Recalculate selection if another valid owned piece is chosen
                        int p = state.board[cursor_r][cursor_c];
                        if (p != 0 && ((p > 0) ? 1 : -1) == state.turn) {
                            sel_r = cursor_r;
                            sel_c = cursor_c;
                            legal_count = get_legal_moves(&state, sel_r, sel_c, legal_moves);
                        } else {
                            sel_r = -1; sel_c = -1;
                            legal_count = 0;
                        }
                    }
                }
            }
        }
    }

    // Clean up
    if (is_engine_active) {
        write(to_engine_fd, "quit\n", 5);
        close(to_engine_fd);
        close(from_engine_fd);
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, 0);
    }

    disable_raw_mode();
    printf("\e[2J\e[HGame terminated. Goodbye!\n");
    return 0;
}
