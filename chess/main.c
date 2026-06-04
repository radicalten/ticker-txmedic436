#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_MOVES 2048

// Piece Definitions
#define EMPTY 0
#define WP 1
#define WN 2
#define WB 3
#define WR 4
#define WQ 5
#define WK 6
#define BP -1
#define BN -2
#define BB -3
#define BR -4
#define BQ -5
#define BK -6

// Time Control Types
typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCType;

// Move Representation
typedef struct {
    int from;
    int to;
    int promo; // 5=Q, 2=N, 3=B, 4=R
} Move;

// Game State Representation
typedef struct {
    int board[64];
    int turn;      // 1 = White, -1 = Black
    int castling;  // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_sq;     // En Passant target square index (0-63), -1 if none
    int halfmoves;
    int fullmoves;
    int last_from;
    int last_to;
} GameState;

// Global Variables
GameState history[MAX_MOVES];
char pgn_history[MAX_MOVES][16];
char move_history_str[MAX_MOVES][8];
int history_count = 0;
int pgn_count = 0;

// Engine Configuration Globals
int engine_in[2];   // GUI writes to engine
int engine_out[2];  // GUI reads from engine
pid_t engine_pid = -1;
char engine_path[512] = "stockfish"; // Configurable engine path
int engine_active = 2;              // 0=None (Pass & Play), 1=Engine is White, 2=Engine is Black, 3=Both
int engine_thinking = 0;
TCType tc_type = TC_DEPTH;
int tc_val = 10;                     // Default: Depth 10

char engine_buf[4096];
int engine_buf_len = 0;

struct termios orig_termios;

// Initial Chess Board State
const int initial_board[64] = {
    BR, BN, BB, BQ, BK, BB, BN, BR,
    BP, BP, BP, BP, BP, BP, BP, BP,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    WP, WP, WP, WP, WP, WP, WP, WP,
    WR, WN, WB, WQ, WK, WB, WN, WR
};

// --- Terminal Operations ---
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
    fflush(stdout);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; // 100ms non-blocking read
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    printf("\033[?25l"); // Hide cursor
    fflush(stdout);
}

// --- UCI Engine Process Management ---
void stop_engine() {
    if (engine_pid > 0) {
        kill(engine_pid, SIGKILL);
        waitpid(engine_pid, NULL, 0);
        close(engine_in[1]);
        close(engine_out[0]);
        engine_pid = -1;
    }
}

int start_engine() {
    stop_engine();
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return 0;

    engine_pid = fork();
    if (engine_pid < 0) return 0;

    if (engine_pid == 0) { // Child
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[1]);
        close(engine_out[0]);
        close(engine_in[0]);
        close(engine_out[1]);

        execlp(engine_path, engine_path, (char *)NULL);
        exit(1); // Exit if exec fails
    }

    // Parent
    close(engine_in[0]);
    close(engine_out[1]);

    // Set non-blocking read on engine stdout
    int flags = fcntl(engine_out[0], F_GETFL, 0);
    fcntl(engine_out[0], F_SETFL, flags | O_NONBLOCK);

    write(engine_in[1], "uci\n", 4);
    write(engine_in[1], "isready\n", 8);
    return 1;
}

// --- Basic Math Helpers ---
int sign(int val) {
    return (val > 0) - (val < 0);
}

// --- Move Generator Rules Engine ---
int is_path_empty(const GameState *state, int from, int to) {
    int r1 = from / 8, c1 = from % 8;
    int r2 = to / 8, c2 = to % 8;
    int dr = sign(r2 - r1);
    int dc = sign(c2 - c1);
    int r = r1 + dr, c = c1 + dc;
    while (r != r2 || c != c2) {
        if (state->board[r * 8 + c] != EMPTY) return 0;
        r += dr;
        c += dc;
    }
    return 1;
}

int is_square_attacked(const GameState *state, int sq, int attacker_color) {
    int tr = sq / 8, tc = sq % 8;
    for (int i = 0; i < 64; i++) {
        int p = state->board[i];
        if (p == EMPTY) continue;
        int p_color = (p > 0) ? 1 : -1;
        if (p_color != attacker_color) continue;
        int p_type = abs(p);
        int ar = i / 8, ac = i % 8;
        int dr = abs(ar - tr);
        int dc = abs(ac - tc);

        if (p_type == 1) { // Pawn
            if (attacker_color == 1) {
                if (ar == tr + 1 && dc == 1) return 1;
            } else {
                if (ar == tr - 1 && dc == 1) return 1;
            }
        } else if (p_type == 2) { // Knight
            if (dr * dc == 2) return 1;
        } else if (p_type == 3) { // Bishop
            if (dr == dc && is_path_empty(state, i, sq)) return 1;
        } else if (p_type == 4) { // Rook
            if ((dr == 0 || dc == 0) && is_path_empty(state, i, sq)) return 1;
        } else if (p_type == 5) { // Queen
            if ((dr == dc || dr == 0 || dc == 0) && is_path_empty(state, i, sq)) return 1;
        } else if (p_type == 6) { // King
            if (dr <= 1 && dc <= 1) return 1;
        }
    }
    return 0;
}

int get_pseudo_moves(const GameState *state, Move *moves) {
    int count = 0;
    int turn = state->turn;
    for (int from = 0; from < 64; from++) {
        int p = state->board[from];
        if (p == EMPTY) continue;
        int p_color = (p > 0) ? 1 : -1;
        if (p_color != turn) continue;
        int p_type = abs(p);
        int fr = from / 8, fc = from % 8;

        if (p_type == 1) { // Pawn Rules
            int dir = -turn;
            int to1 = from + dir * 8;
            if (to1 >= 0 && to1 < 64 && state->board[to1] == EMPTY) {
                int to_row = to1 / 8;
                if (to_row == 0 || to_row == 7) {
                    moves[count++] = (Move){from, to1, 5}; // Auto Queen option
                    moves[count++] = (Move){from, to1, 2}; // Knight option
                } else {
                    moves[count++] = (Move){from, to1, 0};
                }
                if ((turn == 1 && fr == 6) || (turn == -1 && fr == 1)) {
                    int to2 = from + dir * 16;
                    if (state->board[to2] == EMPTY) {
                        moves[count++] = (Move){from, to2, 0};
                    }
                }
            }
            int cols[2] = {fc - 1, fc + 1};
            for (int c = 0; c < 2; c++) {
                if (cols[c] >= 0 && cols[c] < 8) {
                    int to = (fr + dir) * 8 + cols[c];
                    if (to >= 0 && to < 64) {
                        int dest_p = state->board[to];
                        if (dest_p != EMPTY && ((dest_p > 0) ? 1 : -1) == -turn) {
                            int to_row = to / 8;
                            if (to_row == 0 || to_row == 7) {
                                moves[count++] = (Move){from, to, 5};
                                moves[count++] = (Move){from, to, 2};
                            } else {
                                moves[count++] = (Move){from, to, 0};
                            }
                        } else if (to == state->ep_sq) {
                            moves[count++] = (Move){from, to, 0};
                        }
                    }
                }
            }
        } else { // Sliding / Jumping Piece Rules
            for (int to = 0; to < 64; to++) {
                if (from == to) continue;
                int dest_p = state->board[to];
                if (dest_p != EMPTY && ((dest_p > 0) ? 1 : -1) == turn) continue;

                int tr = to / 8, tc = to % 8;
                int dr = abs(fr - tr);
                int dc = abs(fc - tc);
                int ok = 0;

                if (p_type == 2) { if (dr * dc == 2) ok = 1; }
                else if (p_type == 3) { if (dr == dc && is_path_empty(state, from, to)) ok = 1; }
                else if (p_type == 4) { if ((dr == 0 || dc == 0) && is_path_empty(state, from, to)) ok = 1; }
                else if (p_type == 5) { if ((dr == dc || dr == 0 || dc == 0) && is_path_empty(state, from, to)) ok = 1; }
                else if (p_type == 6) { if (dr <= 1 && dc <= 1) ok = 1; }

                if (ok) {
                    moves[count++] = (Move){from, to, 0};
                }
            }
        }
    }

    // Castling Rules
    if (turn == 1) {
        if ((state->castling & 1) && state->board[60] == WK && state->board[61] == EMPTY && state->board[62] == EMPTY) {
            if (!is_square_attacked(state, 60, -1) && !is_square_attacked(state, 61, -1) && !is_square_attacked(state, 62, -1)) {
                moves[count++] = (Move){60, 62, 0};
            }
        }
        if ((state->castling & 2) && state->board[60] == WK && state->board[57] == EMPTY && state->board[58] == EMPTY && state->board[59] == EMPTY) {
            if (!is_square_attacked(state, 60, -1) && !is_square_attacked(state, 59, -1) && !is_square_attacked(state, 58, -1)) {
                moves[count++] = (Move){60, 58, 0};
            }
        }
    } else {
        if ((state->castling & 4) && state->board[4] == BK && state->board[5] == EMPTY && state->board[6] == EMPTY) {
            if (!is_square_attacked(state, 4, 1) && !is_square_attacked(state, 5, 1) && !is_square_attacked(state, 6, 1)) {
                moves[count++] = (Move){4, 6, 0};
            }
        }
        if ((state->castling & 8) && state->board[4] == BK && state->board[1] == EMPTY && state->board[2] == EMPTY && state->board[3] == EMPTY) {
            if (!is_square_attacked(state, 4, 1) && !is_square_attacked(state, 3, 1) && !is_square_attacked(state, 2, 1)) {
                moves[count++] = (Move){4, 2, 0};
            }
        }
    }
    return count;
}

int get_legal_moves(const GameState *state, Move *legal_moves) {
    Move pseudo[256];
    int pseudo_count = get_pseudo_moves(state, pseudo);
    int legal_count = 0;

    for (int i = 0; i < pseudo_count; i++) {
        GameState temp = *state;
        int p = temp.board[pseudo[i].from];
        int p_type = abs(p);

        if (p_type == 1 && pseudo[i].to == temp.ep_sq) {
            temp.board[pseudo[i].to - (-temp.turn) * 8] = EMPTY;
        }

        temp.board[pseudo[i].to] = p;
        temp.board[pseudo[i].from] = EMPTY;

        if (p_type == 1 && pseudo[i].promo != 0) {
            temp.board[pseudo[i].to] = pseudo[i].promo * temp.turn;
        }

        int king_sq = -1;
        for (int k = 0; k < 64; k++) {
            if (temp.board[k] == 6 * temp.turn) {
                king_sq = k;
                break;
            }
        }
        if (king_sq != -1 && is_square_attacked(&temp, king_sq, -temp.turn)) {
            continue; // Leaves King in check
        }
        legal_moves[legal_count++] = pseudo[i];
    }
    return legal_count;
}

void apply_move(GameState *state, Move m) {
    int p = state->board[m.from];
    int p_type = abs(p);
    int turn = state->turn;

    state->last_from = m.from;
    state->last_to = m.to;
    int next_ep_sq = -1;

    if (p_type == 1 && m.to == state->ep_sq) {
        state->board[m.to - (-turn) * 8] = EMPTY;
    }

    if (p_type == 6) {
        if (m.from == 60 && m.to == 62) {
            state->board[61] = state->board[63]; state->board[63] = EMPTY;
        } else if (m.from == 60 && m.to == 58) {
            state->board[59] = state->board[56]; state->board[56] = EMPTY;
        } else if (m.from == 4 && m.to == 6) {
            state->board[5] = state->board[7]; state->board[7] = EMPTY;
        } else if (m.from == 4 && m.to == 2) {
            state->board[3] = state->board[0]; state->board[0] = EMPTY;
        }
    }

    if (p_type == 1 && abs(m.from - m.to) == 16) {
        next_ep_sq = m.from + (-turn) * 8;
    }

    state->board[m.to] = p;
    state->board[m.from] = EMPTY;

    if (p_type == 1 && m.promo != 0) {
        state->board[m.to] = m.promo * turn;
    }

    if (p_type == 6) {
        if (turn == 1) state->castling &= ~3;
        else state->castling &= ~12;
    }
    if (m.from == 56 || m.to == 56) state->castling &= ~2;
    if (m.from == 63 || m.to == 63) state->castling &= ~1;
    if (m.from == 0 || m.to == 0) state->castling &= ~8;
    if (m.from == 7 || m.to == 7) state->castling &= ~4;

    state->ep_sq = next_ep_sq;
    if (turn == -1) state->fullmoves++;
    state->turn = -turn;
}

// --- Coordinate Translation Helpers ---
void square_to_str(int sq, char *str) {
    str[0] = 'a' + (sq % 8);
    str[1] = '8' - (sq / 8);
    str[2] = '\0';
}

void move_to_str(Move m, char *str) {
    char f[3], t[3];
    square_to_str(m.from, f);
    square_to_str(m.to, t);
    if (m.promo != 0) {
        char p_char = 'q';
        if (m.promo == 2) p_char = 'n';
        if (m.promo == 3) p_char = 'b';
        if (m.promo == 4) p_char = 'r';
        snprintf(str, 8, "%s%s%c", f, t, p_char);
    } else {
        snprintf(str, 8, "%s%s", f, t);
    }
}

Move str_to_move(const GameState *state, const char *str) {
    Move m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f_col = str[0] - 'a';
    int f_row = '8' - str[1];
    int t_col = str[2] - 'a';
    int t_row = '8' - str[3];

    m.from = f_row * 8 + f_col;
    m.to = t_row * 8 + t_col;

    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'q') m.promo = 5;
        else if (p == 'n') m.promo = 2;
        else if (p == 'b') m.promo = 3;
        else if (p == 'r') m.promo = 4;
    }
    return m;
}

// --- PGN (Portable Game Notation) Parser ---
void get_san(const GameState *state_before, Move m, char *san) {
    GameState temp = *state_before;
    int p = temp.board[m.from];
    int p_type = abs(p);
    int is_cap = (temp.board[m.to] != EMPTY) || (p_type == 1 && m.to == temp.ep_sq);

    if (p_type == 6 && abs(m.from - m.to) == 2) {
        if (m.to > m.from) strcpy(san, "O-O");
        else strcpy(san, "O-O-O");
    } else {
        char piece_char[2] = "";
        if (p_type == 2) strcpy(piece_char, "N");
        else if (p_type == 3) strcpy(piece_char, "B");
        else if (p_type == 4) strcpy(piece_char, "R");
        else if (p_type == 5) strcpy(piece_char, "Q");
        else if (p_type == 6) strcpy(piece_char, "K");

        char cap_char[4] = "";
        if (is_cap) {
            if (p_type == 1) {
                cap_char[0] = 'a' + (m.from % 8);
                cap_char[1] = 'x';
                cap_char[2] = '\0';
            } else {
                strcpy(cap_char, "x");
            }
        }

        char dest[3];
        square_to_str(m.to, dest);

        char promo_char[4] = "";
        if (m.promo != 0) {
            char p_sym = 'Q';
            if (m.promo == 2) p_sym = 'N';
            if (m.promo == 3) p_sym = 'B';
            if (m.promo == 4) p_sym = 'R';
            snprintf(promo_char, sizeof(promo_char), "=%c", p_sym);
        }

        snprintf(san, 16, "%s%s%s%s", piece_char, cap_char, dest, promo_char);
    }

    apply_move(&temp, m);
    int opp_king_sq = -1;
    for (int k = 0; k < 64; k++) {
        if (temp.board[k] == 6 * temp.turn) {
            opp_king_sq = k;
            break;
        }
    }
    if (opp_king_sq != -1 && is_square_attacked(&temp, opp_king_sq, -temp.turn)) {
        Move escape[256];
        int escape_count = get_legal_moves(&temp, escape);
        if (escape_count == 0) {
            strcat(san, "#");
        } else {
            strcat(san, "+");
        }
    }
}

// --- Undo System ---
void undo_move(GameState *state) {
    int undo_steps = (engine_active == 1 || engine_active == 2) ? 2 : 1;
    while (undo_steps > 0 && history_count > 0) {
        history_count--;
        *state = history[history_count];
        pgn_count--;
        undo_steps--;
    }
}

// --- UCI Pipeline Synchronization ---
void send_position(char moves_str[][8], int num_moves) {
    char cmd[4096] = "position startpos";
    if (num_moves > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < num_moves; i++) {
            strcat(cmd, " ");
            strcat(cmd, moves_str[i]);
        }
    }
    strcat(cmd, "\n");
    write(engine_in[1], cmd, strlen(cmd));
}

void send_go() {
    char cmd[128];
    if (tc_type == TC_DEPTH) {
        snprintf(cmd, sizeof(cmd), "go depth %d\n", tc_val);
    } else if (tc_type == TC_NODES) {
        snprintf(cmd, sizeof(cmd), "go nodes %d\n", tc_val);
    } else {
        snprintf(cmd, sizeof(cmd), "go movetime %d\n", tc_val);
    }
    write(engine_in[1], cmd, strlen(cmd));
    engine_thinking = 1;
}

int check_engine_output(char *move_out) {
    char temp[512];
    int n = read(engine_out[0], temp, sizeof(temp) - 1);
    if (n <= 0) return 0;

    temp[n] = '\0';
    if (engine_buf_len + n < (int)sizeof(engine_buf)) {
        memcpy(engine_buf + engine_buf_len, temp, n);
        engine_buf_len += n;
        engine_buf[engine_buf_len] = '\0';
    } else {
        engine_buf_len = 0;
        engine_buf[0] = '\0';
    }

    char *line_start = engine_buf;
    char *line_end;
    int found_move = 0;
    while ((line_end = strchr(line_start, '\n')) != NULL) {
        *line_end = '\0';
        if (strncmp(line_start, "bestmove ", 9) == 0) {
            sscanf(line_start, "bestmove %s", move_out);
            found_move = 1;
        }
        line_start = line_end + 1;
    }

    int consumed = line_start - engine_buf;
    if (consumed > 0) {
        memmove(engine_buf, line_start, engine_buf_len - consumed);
        engine_buf_len -= consumed;
        engine_buf[engine_buf_len] = '\0';
    }
    return found_move;
}

// --- Graphical Rendering Module ---
const char* get_piece_char(int piece) {
    switch (abs(piece)) {
        case 1: return "♟";
        case 2: return "♞";
        case 3: return "♝";
        case 4: return "♜";
        case 5: return "♛";
        case 6: return "♚";
        default: return " ";
    }
}

void draw_gui(const GameState *state, int cursor_x, int cursor_y, int selected_sq, const Move *legal_moves, int legal_count) {
    printf("\033[H"); // Cursor in-place update sequence

    printf("==========================================\n");
    printf("         C TERMINAL CHESS GUI             \n");
    printf("==========================================\n\n");

    int w_king_check = 0, b_king_check = 0;
    int w_king_sq = -1, b_king_sq = -1;
    for (int k = 0; k < 64; k++) {
        if (state->board[k] == WK) w_king_sq = k;
        if (state->board[k] == BK) b_king_sq = k;
    }
    if (w_king_sq != -1 && is_square_attacked(state, w_king_sq, -1)) w_king_check = 1;
    if (b_king_sq != -1 && is_square_attacked(state, b_king_sq, 1)) b_king_check = 1;

    for (int r = 0; r < 8; r++) {
        printf("  %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int is_light = (r + c) % 2 == 0;
            int bg = is_light ? 250 : 240; // Wood/Grey palette setup

            if (sq == state->last_from || sq == state->last_to) {
                bg = is_light ? 228 : 221; // Last Move Highlight
            }

            int is_legal_target = 0;
            if (selected_sq != -1) {
                for (int i = 0; i < legal_count; i++) {
                    if (legal_moves[i].from == selected_sq && legal_moves[i].to == sq) {
                        is_legal_target = 1;
                        break;
                    }
                }
            }
            if (is_legal_target) {
                bg = is_light ? 120 : 114; // Legal Move Target Highlight
            }

            if (sq == selected_sq) bg = 117; // Selected Square Highlight

            if ((state->board[sq] == WK && w_king_check) || (state->board[sq] == BK && b_king_check)) {
                bg = 196; // Red King Danger Highlight
            }

            if (r == cursor_y && c == cursor_x) bg = 208; // Cursor Highlight (Orange)

            printf("\033[48;5;%dm", bg);
            int p = state->board[sq];
            if (p > 0) {
                printf(" \033[38;5;15m%s\033[48;5;%dm ", get_piece_char(p), bg); // Pure White Pieces
            } else if (p < 0) {
                printf(" \033[38;5;16m%s\033[48;5;%dm ", get_piece_char(p), bg); // Pure Black Pieces
            } else {
                if (is_legal_target) {
                    printf(" • ");
                } else {
                    printf("   ");
                }
            }
        }
        printf("\033[0m\n");
    }
    printf("     a  b  c  d  e  f  g  h\n\n");

    printf("Turn: %s %s\n", (state->turn == 1) ? "WHITE" : "BLACK",
           (w_king_check || b_king_check) ? "\033[1;31m(CHECK)\033[0m" : "");

    char tc_str[32];
    if (tc_type == TC_DEPTH) snprintf(tc_str, sizeof(tc_str), "Depth: %d", tc_val);
    else if (tc_type == TC_NODES) snprintf(tc_str, sizeof(tc_str), "Nodes: %d", tc_val);
    else snprintf(tc_str, sizeof(tc_str), "Time: %d ms", tc_val);

    printf("Engine Status: ");
    if (engine_active == 0) printf("Passive");
    else if (engine_active == 1) printf("Engine as White");
    else if (engine_active == 2) printf("Engine as Black");
    else printf("Engine vs Engine Mode");
    printf(" | %s\n", tc_str);

    printf("Engine Executable: %s [%s]\n", engine_path, (engine_pid > 0) ? "Connected" : "Disconnected");
    if (engine_thinking) {
        printf("\033[5;32mEngine is calculating...\033[0m\n");
    } else {
        printf("\n");
    }

    printf("\nPGN Moves Played:\n");
    for (int i = 0; i < pgn_count; i += 2) {
        printf("%2d. %-8s", (i / 2) + 1, pgn_history[i]);
        if (i + 1 < pgn_count) {
            printf("%-8s", pgn_history[i + 1]);
        }
        if ((i / 2 + 1) % 4 == 0 || i + 1 >= pgn_count) printf("\n");
    }
    printf("\n");

    printf("-----------------------------------------------------------------\n");
    printf("Controls: ARROWS/WASD=Move Cursor | SPACE/ENTER=Select Piece/Move\n");
    printf("          U=Undo | E=Force Engine Turn | C=Setup | Q=Quit\n");
    printf("=================================================================\n");
    fflush(stdout);
}

int get_promotion_choice() {
    disable_raw_mode();
    printf("\nChoose Promotion Unit [q = Queen, n = Knight, r = Rook, b = Bishop]: ");
    char ch;
    int selection = 5;
    while (1) {
        if (scanf(" %c", &ch) == 1) {
            if (ch == 'q' || ch == 'Q') { selection = 5; break; }
            if (ch == 'n' || ch == 'N') { selection = 2; break; }
            if (ch == 'r' || ch == 'R') { selection = 4; break; }
            if (ch == 'b' || ch == 'B') { selection = 3; break; }
        }
    }
    enable_raw_mode();
    printf("\033[2J"); // Clear standard terminal interface
    return selection;
}

// --- Configuration Setup Submenu ---
void run_config_menu() {
    printf("\033[2J\033[H");
    printf("=== ENGINE SYSTEM CONFIGURATION ===\n\n");
    printf("1. Set Engine Binary Path (Current: %s)\n", engine_path);
    printf("2. Set Time Control Metrics (Current: %s)\n",
           (tc_type == TC_DEPTH) ? "Depth Bound" : (tc_type == TC_NODES) ? "Nodes Bound" : "Time Window Limit");
    printf("3. Set Performance Variable Value (Current: %d)\n", tc_val);
    printf("4. Select Engine Active Side (Current: ");
    if (engine_active == 0) printf("None");
    else if (engine_active == 1) printf("White");
    else if (engine_active == 2) printf("Black");
    else printf("Self-Play");
    printf(")\n");
    printf("\nPress ESC/Q to exit submenu.\n\n");
    fflush(stdout);

    while (1) {
        char ch;
        int n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) continue;

        if (ch == 'q' || ch == 'Q' || ch == 27) {
            break;
        } else if (ch == '1') {
            disable_raw_mode();
            printf("\nEnter relative/absolute path: ");
            char path[512];
            if (fgets(path, sizeof(path), stdin)) {
                path[strcspn(path, "\n")] = 0;
                if (strlen(path) > 0) {
                    strcpy(engine_path, path);
                    start_engine();
                }
            }
            enable_raw_mode();
            break;
        } else if (ch == '2') {
            if (tc_type == TC_DEPTH) tc_type = TC_NODES;
            else if (tc_type == TC_NODES) tc_type = TC_TIME;
            else tc_type = TC_DEPTH;
            break;
        } else if (ch == '3') {
            disable_raw_mode();
            printf("\nEnter Limit Target Value (Range: 1 to 5000000): ");
            int val;
            if (scanf("%d", &val) == 1) tc_val = val;
            int c;
            while ((c = getchar()) != '\n' && c != EOF);
            enable_raw_mode();
            break;
        } else if (ch == '4') {
            engine_active = (engine_active + 1) % 4;
            break;
        }
    }
    printf("\033[2J");
}

// --- Main Event Loop ---
int main() {
    GameState state;
    memcpy(state.board, initial_board, sizeof(initial_board));
    state.turn = 1;
    state.castling = 15;
    state.ep_sq = -1;
    state.halfmoves = 0;
    state.fullmoves = 1;
    state.last_from = -1;
    state.last_to = -1;

    enable_raw_mode();
    start_engine();

    int cursor_x = 4, cursor_y = 6;
    int selected_sq = -1;
    Move legal_moves[256];
    int legal_count = get_legal_moves(&state, legal_moves);

    printf("\033[2J"); // Complete initial terminal refresh

    int quit = 0;
    while (!quit) {
        draw_gui(&state, cursor_x, cursor_y, selected_sq, legal_moves, legal_count);

        int engine_turn = 0;
        if (engine_active == 3) {
            engine_turn = 1;
        } else if (engine_active == 1 && state.turn == 1) {
            engine_turn = 1;
        } else if (engine_active == 2 && state.turn == -1) {
            engine_turn = 1;
        }

        if (engine_turn && !engine_thinking && engine_pid > 0) {
            send_position(move_history_str, history_count);
            send_go();
            draw_gui(&state, cursor_x, cursor_y, selected_sq, legal_moves, legal_count);
        }

        if (engine_thinking && engine_pid > 0) {
            char bestmove_str[16];
            if (check_engine_output(bestmove_str)) {
                engine_thinking = 0;
                Move m = str_to_move(&state, bestmove_str);
                if (m.from != -1 && m.to != -1) {
                    history[history_count] = state;
                    get_san(&state, m, pgn_history[history_count]);
                    move_to_str(m, move_history_str[history_count]);
                    history_count++;
                    pgn_count = history_count;

                    apply_move(&state, m);
                    legal_count = get_legal_moves(&state, legal_moves);
                    selected_sq = -1;
                }
            }
        }

        char ch;
        int n = read(STDIN_FILENO, &ch, 1);
        if (n > 0) {
            if (ch == 'q' || ch == 'Q') {
                quit = 1;
            } else if (ch == 'u' || ch == 'U') {
                if (!engine_thinking) {
                    undo_move(&state);
                    legal_count = get_legal_moves(&state, legal_moves);
                    selected_sq = -1;
                }
            } else if (ch == 'c' || ch == 'C') {
                if (!engine_thinking) {
                    run_config_menu();
                    legal_count = get_legal_moves(&state, legal_moves);
                }
            } else if (ch == 'e' || ch == 'E') {
                if (!engine_thinking && engine_pid > 0) {
                    send_position(move_history_str, history_count);
                    send_go();
                }
            } else if (ch == 27) { // Arrow key escape sequence translation
                char seq[3];
                if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                    if (seq[0] == '[') {
                        switch (seq[1]) {
                            case 'A': if (cursor_y > 0) cursor_y--; break;
                            case 'B': if (cursor_y < 7) cursor_y++; break;
                            case 'C': if (cursor_x < 7) cursor_x++; break;
                            case 'D': if (cursor_x > 0) cursor_x--; break;
                        }
                    }
                }
            } else if (ch == 'w' || ch == 'W') { if (cursor_y > 0) cursor_y--; }
            else if (ch == 's' || ch == 'S') { if (cursor_y < 7) cursor_y++; }
            else if (ch == 'd' || ch == 'D') { if (cursor_x < 7) cursor_x++; }
            else if (ch == 'a' || ch == 'A') { if (cursor_x > 0) cursor_x--; }
            else if (ch == ' ' || ch == '\n' || ch == '\r') {
                if (!engine_turn && !engine_thinking) {
                    int target_sq = cursor_y * 8 + cursor_x;
                    if (selected_sq == -1) {
                        int p = state.board[target_sq];
                        if (p != EMPTY && ((p > 0) ? 1 : -1) == state.turn) {
                            selected_sq = target_sq;
                        }
                    } else {
                        int valid = 0;
                        Move chosen_move = {-1, -1, 0};
                        for (int i = 0; i < legal_count; i++) {
                            if (legal_moves[i].from == selected_sq && legal_moves[i].to == target_sq) {
                                valid = 1;
                                chosen_move = legal_moves[i];
                                break;
                            }
                        }

                        if (valid) {
                            int p_type = abs(state.board[selected_sq]);
                            if (p_type == 1 && (target_sq / 8 == 0 || target_sq / 8 == 7)) {
                                chosen_move.promo = get_promotion_choice();
                            }

                            history[history_count] = state;
                            get_san(&state, chosen_move, pgn_history[history_count]);
                            move_to_str(chosen_move, move_history_str[history_count]);
                            history_count++;
                            pgn_count = history_count;

                            apply_move(&state, chosen_move);
                            legal_count = get_legal_moves(&state, legal_moves);
                            selected_sq = -1;
                        } else {
                            int p = state.board[target_sq];
                            if (p != EMPTY && ((p > 0) ? 1 : -1) == state.turn) {
                                selected_sq = target_sq;
                            } else {
                                selected_sq = -1;
                            }
                        }
                    }
                }
            }
        } else {
            usleep(10000); // 10ms frame throttling to conserve cpu cycles
        }
    }

    disable_raw_mode();
    stop_engine();
    return 0;
}
