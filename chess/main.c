#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

/* Board Representation Constants */
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define WHITE 1
#define BLACK 2

#define SQ(r, c) ((r) * 8 + (c))
#define COLOR(p) ((p) == 0 ? 0 : ((p) <= 6 ? WHITE : BLACK))
#define TYPE(p)  ((p) == 0 ? 0 : ((p) <= 6 ? (p) : (p) - 6))
#define MAKE_PIECE(c, t) ((c) == WHITE ? (t) : (t) + 6)

/* Game Modes */
#define MODE_HUMAN_VS_HUMAN 0
#define MODE_HUMAN_VS_ENGINE 1
#define MODE_ENGINE_VS_HUMAN 2
#define MODE_ENGINE_VS_ENGINE 3

/* Time Controls */
#define TC_DEPTH 1
#define TC_NODES 2
#define TC_TIME 3

/* Keys Definition */
#define KEY_UP 1001
#define KEY_DOWN 1002
#define KEY_LEFT 1003
#define KEY_RIGHT 1004
#define KEY_SPACE 1005
#define KEY_ENTER 1006
#define KEY_UNDO 'u'
#define KEY_CONFIG 'c'
#define KEY_QUIT 'q'

typedef struct {
    int board[64];
    int turn;             // WHITE or BLACK
    int castling[4];      // WK, WQ, BK, BQ
    int ep_square;        // -1 or target index
    int halfmove_clock;
    int fullmove_number;
} BoardState;

typedef struct {
    BoardState state;
    char move_str[8];     // e.g. "e2e4"
    char pgn[16];         // e.g. "Nf3+"
} HistoryEntry;

/* Global Settings and Engine State */
int game_mode = MODE_HUMAN_VS_ENGINE;
int tc_type = TC_DEPTH;
int tc_val = 10;          // Default search depth
char engine_path[256] = "stockfish";

int engine_in_pipe[2];
int engine_out_pipe[2];
pid_t engine_pid = -1;
int engine_thinking = 0;

#define MAX_MOVES 2048
HistoryEntry history[MAX_MOVES];
int history_count = 0;

struct termios orig_termios;

/* Forward declarations of rules engine */
void init_board(BoardState *state);
int is_legal_move(const BoardState *state, int from, int to, int promo_piece);
int is_in_check(const BoardState *state, int color);
int has_legal_moves(const BoardState *state);
int get_pseudo_moves(const BoardState *state, int from, int *moves);
void make_move(const BoardState *prev, BoardState *next, int from, int to, int promo_piece);

/* Terminal UI Control */
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* Engine Process Spawner */
int start_engine(const char *path) {
    if (pipe(engine_in_pipe) < 0 || pipe(engine_out_pipe) < 0) return 0;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(engine_in_pipe[0], STDIN_FILENO);
        dup2(engine_out_pipe[1], STDOUT_FILENO);
        close(engine_in_pipe[1]);
        close(engine_out_pipe[0]);
        char *args[] = {(char*)path, NULL};
        execvp(path, args);
        exit(1);
    }
    close(engine_in_pipe[0]);
    close(engine_out_pipe[1]);
    int flags = fcntl(engine_out_pipe[0], F_GETFL, 0);
    fcntl(engine_out_pipe[0], F_SETFL, flags | O_NONBLOCK);
    write(engine_in_pipe[1], "uci\n", 4);
    write(engine_in_pipe[1], "isready\n", 8);
    return 1;
}

void kill_engine() {
    if (engine_pid != -1) {
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, 0);
        engine_pid = -1;
        engine_thinking = 0;
    }
}

void cleanup() {
    kill_engine();
    disable_raw_mode();
    printf("\033[H\033[J\033[0mThanks for playing Chess!\n");
}

/* UI Formatting: Generate Line-wrapped PGN list */
void get_pgn_lines(char lines[4][128]) {
    for (int i = 0; i < 4; i++) lines[i][0] = '\0';
    char temp[4096] = "";
    for (int i = 0; i < history_count; i++) {
        char move_num_str[32] = "";
        if (i % 2 == 0) sprintf(move_num_str, "%d. ", (i / 2) + 1);
        strcat(temp, move_num_str);
        strcat(temp, history[i].pgn);
        strcat(temp, " ");
    }
    int len = strlen(temp);
    int start = 0, line_idx = 0;
    while (start < len && line_idx < 4) {
        int chunk = 38;
        if (start + chunk > len) {
            chunk = len - start;
        } else {
            while (chunk > 0 && temp[start + chunk] != ' ') chunk--;
            if (chunk == 0) chunk = 38;
        }
        strncpy(lines[line_idx], temp + start, chunk);
        lines[line_idx][chunk] = '\0';
        line_idx++;
        start += chunk;
        if (start < len && temp[start] == ' ') start++;
    }
}

/* In-place Renderer */
void redraw_all(const BoardState *state, int cursor_pos, int selected_pos, int last_from, int last_to) {
    char buf[16384];
    int ptr = 0;
    ptr += sprintf(buf + ptr, "\033[H"); // Cursor Home

    char panel[16][128];
    for (int i = 0; i < 16; i++) strcpy(panel[i], "");

    int check = is_in_check(state, state->turn);
    int moves_exist = has_legal_moves(state);

    sprintf(panel[0], "\033[1;36m[GAME INFORMATION]\033[0m");
    if (!moves_exist) {
        if (check) {
            sprintf(panel[1], "\033[1;31mGame Over: CHECKMATE! (%s won)\033[0m", state->turn == WHITE ? "Black" : "White");
        } else {
            sprintf(panel[1], "\033[1;33mGame Over: STALEMATE (Draw)\033[0m");
        }
    } else {
        sprintf(panel[1], "Active Turn: %s %s",
                state->turn == WHITE ? "\033[1;97mWhite\033[0m" : "\033[1;31mBlack\033[0m",
                check ? "\033[1;5;31m(IN CHECK!)\033[0m" : "");
    }

    char mode_str[64];
    if (game_mode == MODE_HUMAN_VS_HUMAN) strcpy(mode_str, "Human vs Human");
    else if (game_mode == MODE_HUMAN_VS_ENGINE) strcpy(mode_str, "Human (W) vs Engine (B)");
    else if (game_mode == MODE_ENGINE_VS_HUMAN) strcpy(mode_str, "Engine (W) vs Human (B)");
    else strcpy(mode_str, "Engine vs Engine");

    sprintf(panel[2], "Game Mode  : %s", mode_str);

    char tc_str[64];
    if (tc_type == TC_DEPTH) sprintf(tc_str, "Depth %d", tc_val);
    else if (tc_type == TC_NODES) sprintf(tc_str, "%d nodes", tc_val);
    else sprintf(tc_str, "%d ms per move", tc_val);

    sprintf(panel[3], "Time Ctrl  : %s", tc_str);
    sprintf(panel[4], "Engine Path: \033[3m%s\033[0m", engine_path);
    sprintf(panel[5], "Engine State: %s", engine_thinking ? "\033[1;33mThinking...\033[0m" : "Idle");

    sprintf(panel[6], "\033[1;36m[RECENT PGN MOVES]\033[0m");
    char pgn_lines[4][128];
    get_pgn_lines(pgn_lines);
    for (int i = 0; i < 4; i++) {
        sprintf(panel[7 + i], "  %s", strlen(pgn_lines[i]) > 0 ? pgn_lines[i] : "...");
    }

    sprintf(panel[11], "\033[1;36m[KEYS & CONTROLS]\033[0m");
    sprintf(panel[12], "  Arrows / WASD : Move Cursor");
    sprintf(panel[13], "  Space / Enter : Select / Move Piece");
    sprintf(panel[14], "  U : Undo Takeback  |  C : Configuration Menu");
    sprintf(panel[15], "  Q : Quit Application");

    ptr += sprintf(buf + ptr, "\033[1;35m   === CHESS TERMINAL ENGINE GUI ===\033[0m\n\n");
    ptr += sprintf(buf + ptr, "     a  b  c  d  e  f  g  h      %s\n", panel[0]);

    for (int r = 0; r < 8; r++) {
        ptr += sprintf(buf + ptr, "  %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = SQ(r, c);
            int piece = state->board[sq];
            int bg = ((r + c) % 2 == 1);

            char bg_color[32];
            if (sq == cursor_pos) {
                strcpy(bg_color, "\033[48;5;214m"); // Cursor: Orange Highlight
            } else if (sq == selected_pos) {
                strcpy(bg_color, "\033[48;5;71m");  // Selection: Soft Green
            } else if (sq == last_from || sq == last_to) {
                strcpy(bg_color, "\033[48;5;67m");  // Last move: Muted Blue
            } else if (bg == 0) {
                strcpy(bg_color, "\033[48;5;253m"); // Light Square: Cream Grey
            } else {
                strcpy(bg_color, "\033[48;5;241m"); // Dark Square: Slate Grey
            }

            char fg_color[32];
            int cp = COLOR(piece);
            if (cp == WHITE) {
                strcpy(fg_color, "\033[38;5;231;1m"); // Bright White Bold
            } else {
                strcpy(fg_color, "\033[38;5;196;1m"); // Vibrant Crimson Red Bold
            }

            char piece_char[8];
            int tp = TYPE(piece);
            if (piece == EMPTY) {
                strcpy(piece_char, " ");
            } else {
                char *syms[] = {"", "♟", "♞", "♝", "♜", "♛", "♚"};
                strcpy(piece_char, syms[tp]);
            }

            ptr += sprintf(buf + ptr, "%s%s %s \033[0m", bg_color, fg_color, piece_char);
        }
        ptr += sprintf(buf + ptr, " %d    %s\n", 8 - r, panel[1 + r]);
    }
    ptr += sprintf(buf + ptr, "     a  b  c  d  e  f  g  h      %s\n", panel[9]);

    for (int i = 10; i < 16; i++) {
        ptr += sprintf(buf + ptr, "                                 %s\n", panel[i]);
    }
    ptr += sprintf(buf + ptr, "\n");
    write(STDOUT_FILENO, buf, ptr);
}

/* User Input Engine Config Dialogue */
void run_configuration() {
    disable_raw_mode();
    printf("\n\033[1;33m--- CONFIGURATION MENU ---\033[0m\n");
    printf("1. Game Mode (0=Human vs Human, 1=Human vs Engine, 2=Engine vs Human, 3=Engine vs Engine)\n");
    printf("2. Time Control (1=Depth, 2=Nodes, 3=Time limit in ms)\n");
    printf("3. Time Control Value\n");
    printf("4. Engine Executable Path\n");
    printf("Choose option (1-4) or enter '0' to return: ");
    fflush(stdout);

    char line[256];
    if (fgets(line, sizeof(line), stdin)) {
        int opt = atoi(line);
        if (opt == 1) {
            printf("Enter Game Mode (0-3): "); fflush(stdout);
            if (fgets(line, sizeof(line), stdin)) {
                int val = atoi(line);
                if (val >= 0 && val <= 3) {
                    game_mode = val;
                    kill_engine();
                }
            }
        } else if (opt == 2) {
            printf("Enter Time Control Type (1=Depth, 2=Nodes, 3=Time): "); fflush(stdout);
            if (fgets(line, sizeof(line), stdin)) {
                int val = atoi(line);
                if (val >= 1 && val <= 3) tc_type = val;
            }
        } else if (opt == 3) {
            printf("Enter Time Control Value (e.g., 10 for Depth, 10000 for Nodes, 2000 for Milliseconds): "); fflush(stdout);
            if (fgets(line, sizeof(line), stdin)) {
                int val = atoi(line);
                if (val > 0) tc_val = val;
            }
        } else if (opt == 4) {
            printf("Enter Engine Binary (current: %s): ", engine_path); fflush(stdout);
            if (fgets(line, sizeof(line), stdin)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (strlen(line) > 0) {
                    strcpy(engine_path, line);
                    kill_engine();
                }
            }
        }
    }
    enable_raw_mode();
    printf("\033[H\033[J"); // Clears the temp prompt screens cleanly
}

/* Keyboard Input Reader */
int read_key() {
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return 0;

    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) == 0) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) == 0) return '\033';

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return '\033';
    }

    if (c == ' ' || c == '\xa0') return KEY_SPACE;
    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 'w' || c == 'W') return KEY_UP;
    if (c == 's' || c == 'S') return KEY_DOWN;
    if (c == 'a' || c == 'A') return KEY_LEFT;
    if (c == 'd' || c == 'D') return KEY_RIGHT;

    return tolower(c);
}

void ensure_engine_started() {
    if (engine_pid == -1 && (game_mode != MODE_HUMAN_VS_HUMAN)) {
        if (!start_engine(engine_path)) {
            game_mode = MODE_HUMAN_VS_HUMAN;
        }
    }
}

void send_engine_position() {
    if (engine_pid == -1) return;
    char cmd[8192] = "position startpos";
    if (history_count > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < history_count; i++) {
            strcat(cmd, " ");
            strcat(cmd, history[i].move_str);
        }
    }
    strcat(cmd, "\n");
    write(engine_in_pipe[1], cmd, strlen(cmd));

    char go[128];
    if (tc_type == TC_DEPTH) sprintf(go, "go depth %d\n", tc_val);
    else if (tc_type == TC_NODES) sprintf(go, "go nodes %d\n", tc_val);
    else sprintf(go, "go movetime %d\n", tc_val);
    write(engine_in_pipe[1], go, strlen(go));
    engine_thinking = 1;
}

/* Algebraic PGN Move Stringifier */
void get_pgn_notation(const BoardState *prev, int from, int to, int promo, char *out) {
    int p = prev->board[from];
    int t = TYPE(p);
    int is_capture = (prev->board[to] != EMPTY) || (t == PAWN && to == prev->ep_square);

    char files[] = "abcdefgh";
    char ranks[] = "87654321";

    int from_col = from % 8, from_row = from / 8;
    int to_col = to % 8, to_row = to / 8;

    if (t == KING && abs(from_col - to_col) == 2) {
        if (to_col == 6) strcpy(out, "O-O");
        else strcpy(out, "O-O-O");
        return;
    }

    int ptr = 0;
    if (t == PAWN) {
        if (is_capture) {
            out[ptr++] = files[from_col];
            out[ptr++] = 'x';
        }
        out[ptr++] = files[to_col];
        out[ptr++] = ranks[to_row];
        if (to_row == 0 || to_row == 7) {
            out[ptr++] = '=';
            if (promo == QUEEN) out[ptr++] = 'Q';
            else if (promo == ROOK) out[ptr++] = 'R';
            else if (promo == BISHOP) out[ptr++] = 'B';
            else if (promo == KNIGHT) out[ptr++] = 'N';
        }
    } else {
        char piece_char[] = " NKBRQ K";
        out[ptr++] = piece_char[t];

        int ambiguity = 0;
        for (int sq = 0; sq < 64; sq++) {
            if (sq != from && prev->board[sq] == p) {
                if (is_legal_move(prev, sq, to, 0)) ambiguity = 1;
            }
        }
        if (ambiguity) out[ptr++] = files[from_col];
        if (is_capture) out[ptr++] = 'x';

        out[ptr++] = files[to_col];
        out[ptr++] = ranks[to_row];
    }

    BoardState next;
    make_move(prev, &next, from, to, promo);
    if (is_in_check(&next, next.turn)) {
        out[ptr++] = has_legal_moves(&next) ? '+' : '#';
    }
    out[ptr] = '\0';
}

void undo_move(BoardState *state) {
    if (history_count > 0) {
        history_count--;
        *state = history[history_count].state;
    }
}

void perform_undo(BoardState *state, int *last_from, int *last_to) {
    if (game_mode == MODE_HUMAN_VS_HUMAN) {
        undo_move(state);
    } else if (game_mode == MODE_HUMAN_VS_ENGINE || game_mode == MODE_ENGINE_VS_HUMAN) {
        undo_move(state); // Undo engine's move
        undo_move(state); // Undo player's move
    } else if (game_mode == MODE_ENGINE_VS_ENGINE) {
        undo_move(state);
    }

    if (history_count > 0) {
        char *m = history[history_count - 1].move_str;
        *last_from = SQ('8' - m[1], m[0] - 'a');
        *last_to = SQ('8' - m[3], m[2] - 'a');
    } else {
        *last_from = -1;
        *last_to = -1;
    }
    engine_thinking = 0;
}

int handle_human_promotion_prompt() {
    disable_raw_mode();
    printf("\n\033[1;33m--- PROMOTION ---\033[0m\n");
    printf("Promote Pawn to: (Q)ueen, (R)ook, (B)ishop, (K)night [Default: Q]: ");
    fflush(stdout);
    char line[256];
    int promo = QUEEN;
    if (fgets(line, sizeof(line), stdin)) {
        char choice = tolower(line[0]);
        if (choice == 'q') promo = QUEEN;
        else if (choice == 'r') promo = ROOK;
        else if (choice == 'b') promo = BISHOP;
        else if (choice == 'n') promo = KNIGHT;
    }
    enable_raw_mode();
    printf("\033[H\033[J");
    return promo;
}

int is_engine_turn(const BoardState *state) {
    if (game_mode == MODE_HUMAN_VS_ENGINE && state->turn == BLACK) return 1;
    if (game_mode == MODE_ENGINE_VS_HUMAN && state->turn == WHITE) return 1;
    if (game_mode == MODE_ENGINE_VS_ENGINE) return 1;
    return 0;
}

/* Non-blocking Engine Response Parser */
char engine_line_buffer[4096];
int engine_line_len = 0;

void process_engine_output(BoardState *state, int *last_from, int *last_to) {
    if (engine_pid == -1) return;
    char tmp[2048];
    int n = read(engine_out_pipe[0], tmp, sizeof(tmp) - 1);
    if (n <= 0) return;

    for (int i = 0; i < n; i++) {
        if (tmp[i] == '\n' || tmp[i] == '\r') {
            if (engine_line_len > 0) {
                engine_line_buffer[engine_line_len] = '\0';
                if (strncmp(engine_line_buffer, "bestmove ", 9) == 0) {
                    char move_str[16];
                    sscanf(engine_line_buffer + 9, "%s", move_str);

                    int from = SQ('8' - move_str[1], move_str[0] - 'a');
                    int to = SQ('8' - move_str[3], move_str[2] - 'a');

                    int promo = 0;
                    if (strlen(move_str) == 5) {
                        char p = move_str[4];
                        if (p == 'q') promo = QUEEN;
                        else if (p == 'r') promo = ROOK;
                        else if (p == 'b') promo = BISHOP;
                        else if (p == 'n') promo = KNIGHT;
                    }

                    if (is_legal_move(state, from, to, promo)) {
                        history[history_count].state = *state;
                        strcpy(history[history_count].move_str, move_str);
                        get_pgn_notation(state, from, to, promo, history[history_count].pgn);

                        BoardState next;
                        make_move(state, &next, from, to, promo);
                        *state = next;
                        history_count++;

                        *last_from = from;
                        *last_to = to;
                    }
                    engine_thinking = 0;
                }
                engine_line_len = 0;
            }
        } else {
            if (engine_line_len < sizeof(engine_line_buffer) - 2) {
                engine_line_buffer[engine_line_len++] = tmp[i];
            }
        }
    }
}

int main() {
    enable_raw_mode();
    printf("\033[H\033[J\033[?25l"); // Clean and hide cursor

    BoardState state;
    init_board(&state);

    int cursor_pos = SQ(6, 4); // Start cursor on e2
    int selected_pos = -1;
    int last_from = -1;
    int last_to = -1;

    atexit(cleanup);

    redraw_all(&state, cursor_pos, selected_pos, last_from, last_to);

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;

    while (1) {
        ensure_engine_started();

        if (engine_pid != -1) {
            fds[1].fd = engine_out_pipe[0];
            fds[1].events = POLLIN;
        }

        int timeout = 50; // Wake up loop every 50ms
        int poll_count = poll(fds, engine_pid != -1 ? 2 : 1, timeout);

        if (poll_count > 0) {
            if (fds[0].revents & POLLIN) {
                int key = read_key();
                if (key == KEY_QUIT) {
                    break;
                } else if (key == KEY_UNDO) {
                    perform_undo(&state, &last_from, &last_to);
                    redraw_all(&state, cursor_pos, selected_pos, last_from, last_to);
                } else if (key == KEY_CONFIG) {
                    run_configuration();
                    redraw_all(&state, cursor_pos, selected_pos, last_from, last_to);
                } else if (!is_engine_turn(&state) && has_legal_moves(&state)) {
                    int r = cursor_pos / 8;
                    int c = cursor_pos % 8;

                    if (key == KEY_UP && r > 0) r--;
                    else if (key == KEY_DOWN && r < 7) r++;
                    else if (key == KEY_LEFT && c > 0) c--;
                    else if (key == KEY_RIGHT && c < 7) c++;
                    else if (key == KEY_SPACE || key == KEY_ENTER) {
                        if (selected_pos == -1) {
                            if (COLOR(state.board[cursor_pos]) == state.turn) {
                                selected_pos = cursor_pos;
                            }
                        } else {
                            if (cursor_pos == selected_pos) {
                                selected_pos = -1; // Deselect
                            } else if (COLOR(state.board[cursor_pos]) == state.turn) {
                                selected_pos = cursor_pos; // Swap selected piece
                            } else {
                                int promo = 0;
                                if (TYPE(state.board[selected_pos]) == PAWN && (cursor_pos / 8 == 0 || cursor_pos / 8 == 7)) {
                                    promo = handle_human_promotion_prompt();
                                }
                                if (is_legal_move(&state, selected_pos, cursor_pos, promo)) {
                                    history[history_count].state = state;
                                    sprintf(history[history_count].move_str, "%c%d%c%d%s",
                                            'a' + (selected_pos % 8), 8 - (selected_pos / 8),
                                            'a' + (cursor_pos % 8), 8 - (cursor_pos / 8),
                                            promo == QUEEN ? "q" : promo == ROOK ? "r" : promo == BISHOP ? "b" : promo == KNIGHT ? "n" : "");
                                    get_pgn_notation(&state, selected_pos, cursor_pos, promo, history[history_count].pgn);

                                    BoardState next;
                                    make_move(&state, &next, selected_pos, cursor_pos, promo);
                                    state = next;
                                    history_count++;

                                    last_from = selected_pos;
                                    last_to = cursor_pos;
                                    selected_pos = -1;
                                }
                            }
                        }
                    }
                    cursor_pos = SQ(r, c);
                    redraw_all(&state, cursor_pos, selected_pos, last_from, last_to);
                }
            }
            if (engine_pid != -1 && (fds[1].revents & POLLIN)) {
                process_engine_output(&state, &last_from, &last_to);
                redraw_all(&state, cursor_pos, selected_pos, last_from, last_to);
            }
        }

        if (is_engine_turn(&state) && !engine_thinking && has_legal_moves(&state)) {
            send_engine_position();
            redraw_all(&state, cursor_pos, selected_pos, last_from, last_to);
        }
    }

    printf("\033[?25h"); // Restore terminal cursor
    return 0;
}

/* ==========================================
   CHESS GAME RULES & LEGALITY GENERATOR ENGINE
   ========================================== */

void init_board(BoardState *state) {
    memset(state, 0, sizeof(BoardState));
    int back_row[] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int col = 0; col < 8; col++) {
        state->board[SQ(0, col)] = MAKE_PIECE(BLACK, back_row[col]);
        state->board[SQ(1, col)] = MAKE_PIECE(BLACK, PAWN);
        state->board[SQ(6, col)] = MAKE_PIECE(WHITE, PAWN);
        state->board[SQ(7, col)] = MAKE_PIECE(WHITE, back_row[col]);
    }
    state->turn = WHITE;
    state->castling[0] = state->castling[1] = state->castling[2] = state->castling[3] = 1;
    state->ep_square = -1;
    state->halfmove_clock = 0;
    state->fullmove_number = 1;
}

int get_pseudo_moves(const BoardState *state, int from, int *moves) {
    int count = 0, p = state->board[from];
    if (p == EMPTY) return 0;
    int c = COLOR(p), t = TYPE(p);
    int r = from / 8, col = from % 8;

    if (t == PAWN) {
        int dir = (c == WHITE) ? -1 : 1;
        int start_row = (c == WHITE) ? 6 : 1;
        int nr = r + dir;
        if (nr >= 0 && nr < 8 && state->board[SQ(nr, col)] == EMPTY) {
            moves[count++] = SQ(nr, col);
            if (r == start_row && state->board[SQ(r + 2 * dir, col)] == EMPTY) {
                moves[count++] = SQ(r + 2 * dir, col);
            }
        }
        int dc[] = {-1, 1};
        for (int i = 0; i < 2; i++) {
            int nc = col + dc[i];
            if (nc >= 0 && nc < 8) {
                int target_sq = SQ(nr, nc);
                if ((state->board[target_sq] != EMPTY && COLOR(state->board[target_sq]) != c) || target_sq == state->ep_square) {
                    moves[count++] = target_sq;
                }
            }
        }
    } else if (t == KNIGHT) {
        int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
        int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + dr[i], nc = col + dc[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target_p = state->board[SQ(nr, nc)];
                if (target_p == EMPTY || COLOR(target_p) != c) moves[count++] = SQ(nr, nc);
            }
        }
    } else if (t == BISHOP || t == ROOK || t == QUEEN) {
        int dr[8], dc[8], dirs = 0;
        if (t == BISHOP || t == QUEEN) {
            dr[dirs] = -1; dc[dirs] = -1; dirs++;
            dr[dirs] = -1; dc[dirs] = 1; dirs++;
            dr[dirs] = 1; dc[dirs] = -1; dirs++;
            dr[dirs] = 1; dc[dirs] = 1; dirs++;
        }
        if (t == ROOK || t == QUEEN) {
            dr[dirs] = -1; dc[dirs] = 0; dirs++;
            dr[dirs] = 1; dc[dirs] = 0; dirs++;
            dr[dirs] = 0; dc[dirs] = -1; dirs++;
            dr[dirs] = 0; dc[dirs] = 1; dirs++;
        }
        for (int d = 0; d < dirs; d++) {
            int nr = r + dr[d], nc = col + dc[d];
            while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target_sq = SQ(nr, nc), target_p = state->board[target_sq];
                if (target_p == EMPTY) {
                    moves[count++] = target_sq;
                } else {
                    if (COLOR(target_p) != c) moves[count++] = target_sq;
                    break;
                }
                nr += dr[d]; nc += dc[d];
            }
        }
    } else if (t == KING) {
        int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
        int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + dr[i], nc = col + dc[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target_p = state->board[SQ(nr, nc)];
                if (target_p == EMPTY || COLOR(target_p) != c) moves[count++] = SQ(nr, nc);
            }
        }
        if (c == WHITE) {
            if (state->castling[0] && state->board[SQ(7, 5)] == EMPTY && state->board[SQ(7, 6)] == EMPTY) moves[count++] = SQ(7, 6);
            if (state->castling[1] && state->board[SQ(7, 3)] == EMPTY && state->board[SQ(7, 2)] == EMPTY && state->board[SQ(7, 1)] == EMPTY) moves[count++] = SQ(7, 2);
        } else {
            if (state->castling[2] && state->board[SQ(0, 5)] == EMPTY && state->board[SQ(0, 6)] == EMPTY) moves[count++] = SQ(0, 6);
            if (state->castling[3] && state->board[SQ(0, 3)] == EMPTY && state->board[SQ(0, 2)] == EMPTY && state->board[SQ(0, 1)] == EMPTY) moves[count++] = SQ(0, 2);
        }
    }
    return count;
}

int is_square_attacked(const BoardState *state, int sq, int attacker_color) {
    int sr = sq / 8, sc = sq % 8;

    int p_dir = (attacker_color == WHITE) ? 1 : -1;
    int ar = sr + p_dir;
    if (ar >= 0 && ar < 8) {
        if (sc - 1 >= 0 && state->board[SQ(ar, sc - 1)] == MAKE_PIECE(attacker_color, PAWN)) return 1;
        if (sc + 1 < 8  && state->board[SQ(ar, sc + 1)] == MAKE_PIECE(attacker_color, PAWN)) return 1;
    }

    int knight_dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int knight_dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = sr + knight_dr[i], nc = sc + knight_dc[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[SQ(nr, nc)] == MAKE_PIECE(attacker_color, KNIGHT)) return 1;
        }
    }

    int king_dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int king_dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = sr + king_dr[i], nc = sc + king_dc[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[SQ(nr, nc)] == MAKE_PIECE(attacker_color, KING)) return 1;
        }
    }

    int b_dr[] = {-1, -1, 1, 1}, b_dc[] = {-1, 1, -1, 1};
    for (int d = 0; d < 4; d++) {
        int nr = sr + b_dr[d], nc = sc + b_dc[d];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = state->board[SQ(nr, nc)];
            if (p != EMPTY) {
                if (COLOR(p) == attacker_color && (TYPE(p) == BISHOP || TYPE(p) == QUEEN)) return 1;
                break;
            }
            nr += b_dr[d]; nc += b_dc[d];
        }
    }

    int r_dr[] = {-1, 1, 0, 0}, r_dc[] = {0, 0, -1, 1};
    for (int d = 0; d < 4; d++) {
        int nr = sr + r_dr[d], nc = sc + r_dc[d];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = state->board[SQ(nr, nc)];
            if (p != EMPTY) {
                if (COLOR(p) == attacker_color && (TYPE(p) == ROOK || TYPE(p) == QUEEN)) return 1;
                break;
            }
            nr += r_dr[d]; nc += r_dc[d];
        }
    }
    return 0;
}

int is_in_check(const BoardState *state, int color) {
    int king_p = MAKE_PIECE(color, KING), king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == king_p) { king_sq = i; break; }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(state, king_sq, (color == WHITE) ? BLACK : WHITE);
}

void make_move(const BoardState *prev, BoardState *next, int from, int to, int promo_piece) {
    *next = *prev;
    int p = prev->board[from], c = COLOR(p), t = TYPE(p);

    next->board[from] = EMPTY;
    next->board[to] = MAKE_PIECE(c, t);
    next->ep_square = -1;

    if (t == PAWN) {
        int from_r = from / 8, to_r = to / 8;
        if (abs(from_r - to_r) == 2) {
            next->ep_square = SQ((from_r + to_r) / 2, from % 8);
        }
        if (to == prev->ep_square) {
            next->board[SQ(from_r, to % 8)] = EMPTY;
        }
        if (to_r == 0 || to_r == 7) {
            next->board[to] = MAKE_PIECE(c, promo_piece ? promo_piece : QUEEN);
        }
    }

    if (t == KING) {
        if (c == WHITE) {
            next->castling[0] = next->castling[1] = 0;
            if (from == SQ(7, 4)) {
                if (to == SQ(7, 6)) { next->board[SQ(7, 5)] = MAKE_PIECE(WHITE, ROOK); next->board[SQ(7, 7)] = EMPTY; }
                else if (to == SQ(7, 2)) { next->board[SQ(7, 3)] = MAKE_PIECE(WHITE, ROOK); next->board[SQ(7, 0)] = EMPTY; }
            }
        } else {
            next->castling[2] = next->castling[3] = 0;
            if (from == SQ(0, 4)) {
                if (to == SQ(0, 6)) { next->board[SQ(0, 5)] = MAKE_PIECE(BLACK, ROOK); next->board[SQ(0, 7)] = EMPTY; }
                else if (to == SQ(0, 2)) { next->board[SQ(0, 3)] = MAKE_PIECE(BLACK, ROOK); next->board[SQ(0, 0)] = EMPTY; }
            }
        }
    }

    if (from == SQ(7, 7) || to == SQ(7, 7)) next->castling[0] = 0;
    if (from == SQ(7, 0) || to == SQ(7, 0)) next->castling[1] = 0;
    if (from == SQ(0, 7) || to == SQ(0, 7)) next->castling[2] = 0;
    if (from == SQ(0, 0) || to == SQ(0, 0)) next->castling[3] = 0;

    next->turn = (c == WHITE) ? BLACK : WHITE;
    if (t == PAWN || prev->board[to] != EMPTY) next->halfmove_clock = 0;
    else next->halfmove_clock++;
    if (c == BLACK) next->fullmove_number++;
}

int is_legal_move(const BoardState *state, int from, int to, int promo_piece) {
    int p = state->board[from];
    if (p == EMPTY || COLOR(p) != state->turn) return 0;

    int moves[64], count = get_pseudo_moves(state, from, moves), found = 0;
    for (int i = 0; i < count; i++) {
        if (moves[i] == to) { found = 1; break; }
    }
    if (!found) return 0;

    if (TYPE(p) == KING && abs((from % 8) - (to % 8)) == 2) {
        int opponent = (state->turn == WHITE) ? BLACK : WHITE;
        if (is_square_attacked(state, from, opponent)) return 0;
        int step = (to > from) ? 1 : -1;
        if (is_square_attacked(state, from + step, opponent)) return 0;
    }

    BoardState next;
    make_move(state, &next, from, to, promo_piece);
    if (is_in_check(&next, state->turn)) return 0;

    return 1;
}

int has_legal_moves(const BoardState *state) {
    for (int from = 0; from < 64; from++) {
        if (COLOR(state->board[from]) == state->turn) {
            int moves[64], count = get_pseudo_moves(state, from, moves);
            for (int i = 0; i < count; i++) {
                if (is_legal_move(state, from, moves[i], QUEEN)) return 1;
            }
        }
    }
    return 0;
}
