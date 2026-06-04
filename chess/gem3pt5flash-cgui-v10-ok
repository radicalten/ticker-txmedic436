#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/select.h>

#define MAX_MOVES 1024

// Color Palette (256-color ANSI codes)
#define COLOR_LIGHT_SQ    "\e[48;5;180m" // Tan / Light wood
#define COLOR_DARK_SQ     "\e[48;5;94m"  // Dark brown / Cocoa
#define COLOR_CURSOR      "\e[48;5;33m"  // Electric Blue
#define COLOR_SELECTED    "\e[48;5;160m" // Dark Red
#define COLOR_LAST_MOVE   "\e[48;5;142m" // Dull Gold
#define COLOR_RESET       "\e[0m"
#define COLOR_WHITE_PIECE "\e[38;5;231m" // Crisp White
#define COLOR_BLACK_PIECE "\e[38;5;234m" // Charcoal Black

typedef struct {
    char board[64];
    int active_color; // 0 = White, 1 = Black
    int castling[4];  // WK, WQ, BK, BQ (1 = available, 0 = lost)
    int ep_square;    // En Passant target square (-1 if none)
} GameState;

// Global App State
GameState state;
char move_history[MAX_MOVES][8];
int move_count = 0;

int cursor_x = 4;
int cursor_y = 6;
int selected_idx = -1;

int last_from_idx = -1;
int last_to_idx = -1;

// UCI Settings
int tc_type = 1; // 1: movetime, 2: depth, 3: nodes
int tc_value = 1000; // default 1000ms

// Engine Communication Pipes
int to_engine[2];
int from_engine[2];
pid_t engine_pid = -1;
struct termios orig_termios;

const char *initial_board =
    "rnbqkbnr"
    "pppppppp"
    "........"
    "........"
    "........"
    "........"
    "PPPPPPPP"
    "RNBQKBNR";

// Cleanup terminal state on exit
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\e[?25h\e[0m\n"); // Restore cursor and reset colors
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\e[?25l"); // Hide cursor
}

void handle_signal(int sig) {
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
    }
    exit(0);
}

// Start the UCI chess engine
void start_engine(const char *path) {
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) {
        perror("Pipe generation failed");
        exit(1);
    }
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]);
        close(from_engine[0]);
        execl(path, path, (char *)NULL);
        perror("Engine launch failed! Make sure the engine is installed and the path is correct.");
        exit(1);
    }
    close(to_engine[0]);
    close(from_engine[1]);

    // Initialize UCI protocol
    write(to_engine[1], "uci\nisready\n", 12);
    char buf[2048];
    int n = read(from_engine[0], buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
    }
}

void send_to_engine(const char *cmd) {
    write(to_engine[1], cmd, strlen(cmd));
}

// Read engine stdout until 'bestmove' prefix is found
void get_engine_move(char *bestmove_out) {
    char line[1024];
    int line_idx = 0;
    while (1) {
        char c;
        if (read(from_engine[0], &c, 1) <= 0) break;
        if (c == '\n') {
            line[line_idx] = '\0';
            if (strncmp(line, "bestmove ", 9) == 0) {
                sscanf(line, "bestmove %s", bestmove_out);
                return;
            }
            line_idx = 0;
        } else if (line_idx < 1023) {
            line[line_idx++] = c;
        }
    }
}

// Basic move validator to block user mistakes instantly
int is_pseudo_legal(GameState *s, int from, int to) {
    char p = s->board[from];
    char target = s->board[to];
    int from_r = from / 8, from_c = from % 8;
    int to_r = to / 8, to_c = to % 8;
    int dr = to_r - from_r;
    int dc = to_c - from_c;

    if (from == to) return 0;
    if (s->active_color == 0 && !isupper(p)) return 0;
    if (s->active_color == 1 && !islower(p)) return 0;

    if (target != '.') {
        if (s->active_color == 0 && isupper(target)) return 0;
        if (s->active_color == 1 && islower(target)) return 0;
    }

    char up = toupper(p);
    if (up == 'R' || up == 'Q') {
        if (dr == 0 || dc == 0) {
            int step_r = (dr > 0) - (dr < 0);
            int step_c = (dc > 0) - (dc < 0);
            int r = from_r + step_r, c = from_c + step_c;
            while (r != to_r || c != to_c) {
                if (s->board[r * 8 + c] != '.') return 0;
                r += step_r; c += step_c;
            }
            if (up == 'R' || up == 'Q') return 1;
        }
    }
    if (up == 'B' || up == 'Q') {
        if (abs(dr) == abs(dc)) {
            int step_r = (dr > 0) - (dr < 0);
            int step_c = (dc > 0) - (dc < 0);
            int r = from_r + step_r, c = from_c + step_c;
            while (r != to_r || c != to_c) {
                if (s->board[r * 8 + c] != '.') return 0;
                r += step_r; c += step_c;
            }
            return 1;
        }
    }
    if (up == 'N') {
        if ((abs(dr) == 2 && abs(dc) == 1) || (abs(dr) == 1 && abs(dc) == 2)) return 1;
    }
    if (up == 'K') {
        if (abs(dr) <= 1 && abs(dc) <= 1) return 1;
        // Castling
        if (dr == 0 && abs(dc) == 2) {
            if (s->active_color == 0) {
                if (from != 60) return 0;
                if (to == 62 && s->castling[0] && s->board[61] == '.' && s->board[62] == '.') return 1;
                if (to == 58 && s->castling[1] && s->board[59] == '.' && s->board[58] == '.' && s->board[57] == '.') return 1;
            } else {
                if (from != 4) return 0;
                if (to == 6 && s->castling[2] && s->board[5] == '.' && s->board[6] == '.') return 1;
                if (to == 2 && s->castling[3] && s->board[3] == '.' && s->board[2] == '.' && s->board[1] == '.') return 1;
            }
        }
    }
    if (up == 'P') {
        int dir = (s->active_color == 0) ? -1 : 1;
        int start_r = (s->active_color == 0) ? 6 : 1;
        if (dc == 0) {
            if (dr == dir && target == '.') return 1;
            if (dr == 2 * dir && from_r == start_r && target == '.' && s->board[(from_r + dir) * 8 + from_c] == '.') return 1;
        } else if (abs(dc) == 1 && dr == dir) {
            if (target != '.') return 1;
            if (to == s->ep_square) return 1; // En Passant
        }
    }
    return 0;
}

// Execute state changes for a move
void execute_move_internal(GameState *s, const char *m) {
    int from_c = m[0] - 'a';
    int from_r = 8 - (m[1] - '0');
    int to_c = m[2] - 'a';
    int to_r = 8 - (m[3] - '0');
    int from = from_r * 8 + from_c;
    int to = to_r * 8 + to_c;

    char piece = s->board[from];
    s->board[to] = piece;
    s->board[from] = '.';

    // Castling adjustments
    if (toupper(piece) == 'K') {
        if (abs(to_c - from_c) == 2) {
            if (to_c == 6) { // Kingside
                s->board[to_r * 8 + 5] = s->board[to_r * 8 + 7];
                s->board[to_r * 8 + 7] = '.';
            } else if (to_c == 2) { // Queenside
                s->board[to_r * 8 + 3] = s->board[to_r * 8 + 0];
                s->board[to_r * 8 + 0] = '.';
            }
        }
        if (s->active_color == 0) { s->castling[0] = s->castling[1] = 0; }
        else { s->castling[2] = s->castling[3] = 0; }
    }

    // Castling rights updates when rooks are moved/taken
    if (from == 56) s->castling[1] = 0;
    if (from == 63) s->castling[0] = 0;
    if (from == 0)  s->castling[3] = 0;
    if (from == 7)  s->castling[2] = 0;

    // En Passant execution
    if (toupper(piece) == 'P' && to == s->ep_square) {
        int target_pawn = (s->active_color == 0) ? (to + 8) : (to - 8);
        s->board[target_pawn] = '.';
    }

    // Determine next Ep targets
    s->ep_square = -1;
    if (toupper(piece) == 'P' && abs(to_r - from_r) == 2) {
        s->ep_square = (s->active_color == 0) ? (from - 8) : (from + 8);
    }

    // Pawn Promotion
    if (toupper(piece) == 'P' && (to_r == 0 || to_r == 7)) {
        char promo = 'q';
        if (strlen(m) == 5) promo = m[4];
        s->board[to] = (s->active_color == 0) ? toupper(promo) : tolower(promo);
    }

    s->active_color = 1 - s->active_color;
}

// Complete board state rebuild (makes undos fully robust and sync-safe)
void rebuild_board() {
    memcpy(state.board, initial_board, 64);
    state.active_color = 0;
    for (int i = 0; i < 4; i++) state.castling[i] = 1;
    state.ep_square = -1;

    for (int i = 0; i < move_count; i++) {
        execute_move_internal(&state, move_history[i]);
    }

    if (move_count > 0) {
        char *last = move_history[move_count - 1];
        last_from_idx = (8 - (last[1] - '0')) * 8 + (last[0] - 'a');
        last_to_idx = (8 - (last[3] - '0')) * 8 + (last[2] - 'a');
    } else {
        last_from_idx = last_to_idx = -1;
    }
}

// Convert Unicode representation based on standard Chess Glyphs
const char *get_piece_glyph(char p) {
    switch (p) {
        case 'K': return "♔"; case 'Q': return "♕"; case 'R': return "♖";
        case 'B': return "♗"; case 'N': return "♘"; case 'P': return "♙";
        case 'k': return "♚"; case 'q': return "♛"; case 'r': return "♜";
        case 'b': return "♝"; case 'n': return "♞"; case 'p': return "♟";
        default: return " ";
    }
}

// High-Fidelity TUI board drawing using 256 ANSI Escape Sequences
void draw_interface(const char *status_msg) {
    printf("\e[H\e[J"); // Move cursor to home and clean frame
    printf("\n  "  "  === terminal uci chess gui === " COLOR_RESET "\n\n");

    for (int r = 0; r < 8; r++) {
        printf(" %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int idx = r * 8 + c;
            int is_light = (r + c) % 2 == 0;

            // Resolve proper background coloring
            if (r == cursor_y && c == cursor_x) {
                printf(COLOR_CURSOR);
            } else if (idx == selected_idx) {
                printf(COLOR_SELECTED);
            } else if (idx == last_from_idx || idx == last_to_idx) {
                printf(COLOR_LAST_MOVE);
            } else if (is_light) {
                printf(COLOR_LIGHT_SQ);
            } else {
                printf(COLOR_DARK_SQ);
            }

            // Write piece structure
            char p = state.board[idx];
            if (p != '.') {
                if (isupper(p)) {
                    printf(COLOR_WHITE_PIECE " %s " COLOR_RESET, get_piece_glyph(p));
                } else {
                    printf(COLOR_BLACK_PIECE " %s " COLOR_RESET, get_piece_glyph(p));
                }
            } else {
                printf("   " COLOR_RESET);
            }
        }

        // Side-panel layout structure
        switch (r) {
            case 0: printf("    Active Turn : %s", state.active_color == 0 ? "White (You)" : "Black (Engine)"); break;
            case 1: printf("    Castling    : W:%s%s B:%s%s", 
                            state.castling[0] ? "K" : "", state.castling[1] ? "Q" : "",
                            state.castling[2] ? "k" : "", state.castling[3] ? "q" : ""); break;
            case 2: printf("    Engine Limit: ");
                    if (tc_type == 1) printf("movetime %d ms", tc_value);
                    else if (tc_type == 2) printf("depth %d plies", tc_value);
                    else printf("nodes %d", tc_value);
                    break;
            case 4: printf("    PGN Record: "); break;
            default:
                if (r >= 5 && r <= 7) {
                    int pgn_offset = (r - 5) * 4;
                    printf("    ");
                    for (int m = pgn_offset; m < pgn_offset + 4 && m < move_count; m++) {
                        if (m % 2 == 0) printf("%d. %s ", (m / 2) + 1, move_history[m]);
                        else printf("%s  ", move_history[m]);
                    }
                }
                break;
        }
        printf("\n");
    }
    printf("     a  b  c  d  e  f  g  h\n\n");
    printf(" [Arrows/WASD] Navigate | [Space/Enter] Select & Move | [U] Undo | [T] Settings | [Q] Exit\n");
    if (status_msg && strlen(status_msg) > 0) {
        printf("\n "  "System Log: %s" COLOR_RESET "\n", status_msg);
    } else {
        printf("\n\n");
    }
}

// User setup menu for Engine parameters
void configure_time_control() {
    disable_raw_mode();
    printf("\n--- uci search configuration ---\n");
    printf("1. Move Time Limit (ms)\n");
    printf("2. Fixed Search Depth (plies)\n");
    printf("3. Fixed Search Node Count\n");
    printf("Select option (1-3): ");
    char choice[16];
    if (fgets(choice, sizeof(choice), stdin)) {
        int opt = atoi(choice);
        if (opt >= 1 && opt <= 3) {
            tc_type = opt;
            printf("Enter limit value: ");
            char val_str[16];
            if (fgets(val_str, sizeof(val_str), stdin)) {
                tc_value = atoi(val_str);
            }
        }
    }
    enable_raw_mode();
}

int main(int argc, char **argv) {
    // Determine engine path (defaults to stockfish, can override via args)
    const char *engine_path = "stockfish";
    if (argc > 1) {
        engine_path = argv[1];
    }

    // Set up standard signals to preserve terminal state
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    start_engine(engine_path);
    enable_raw_mode();
    rebuild_board();

    char sys_log[256] = "Welcome! Use WASD/Arrows, Select with Space/Enter.";
    draw_interface(sys_log);

    while (1) {
        // Render system loop if it's the engine's turn to play
        if (state.active_color == 1 && move_count < MAX_MOVES) {
            draw_interface("Engine is calculating standard reply...");
            
            // Build absolute legal engine instruction line
            char cmd[8192] = "position startpos moves";
            for (int i = 0; i < move_count; i++) {
                strcat(cmd, " ");
                strcat(cmd, move_history[i]);
            }
            strcat(cmd, "\n");
            send_to_engine(cmd);

            // Trigger engine execution constraints
            char go_cmd[128];
            if (tc_type == 1) sprintf(go_cmd, "go movetime %d\n", tc_value);
            else if (tc_type == 2) sprintf(go_cmd, "go depth %d\n", tc_value);
            else sprintf(go_cmd, "go nodes %d\n", tc_value);
            send_to_engine(go_cmd);

            char engine_move[16] = {0};
            get_engine_move(engine_move);

            if (strlen(engine_move) >= 4) {
                strncpy(move_history[move_count++], engine_move, 7);
                rebuild_board();
                sprintf(sys_log, "Engine plays %s", engine_move);
            } else {
                sprintf(sys_log, "No viable moves found. Game ended.");
            }
            draw_interface(sys_log);
            continue;
        }

        // Catch user raw inputs
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) continue;

        // Process Escape Sequences (for Standard Terminal Arrow Keys)
        if (c == '\e') {
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                if (seq[0] == '[') {
                    switch (seq[1]) {
                        case 'A': c = 'w'; break; // Up Arrow
                        case 'B': c = 's'; break; // Down Arrow
                        case 'C': c = 'd'; break; // Right Arrow
                        case 'D': c = 'a'; break; // Left Arrow
                    }
                }
            }
        }

        // Handle raw keystrokes
        if (c == 'q' || c == 'Q') {
            break;
        } 
        else if (c == 'w' || c == 'W') {
            if (cursor_y > 0) cursor_y--;
            sys_log[0] = '\0';
        } 
        else if (c == 's' || c == 'S') {
            if (cursor_y < 7) cursor_y++;
            sys_log[0] = '\0';
        } 
        else if (c == 'a' || c == 'A') {
            if (cursor_x > 0) cursor_x--;
            sys_log[0] = '\0';
        } 
        else if (c == 'd' || c == 'D') {
            if (cursor_x < 7) cursor_x++;
            sys_log[0] = '\0';
        } 
        else if (c == 'u' || c == 'U') {
            if (move_count >= 2) {
                move_count -= 2; // rollback user + engine move
                rebuild_board();
                strcpy(sys_log, "Undo successfully executed.");
            } else {
                strcpy(sys_log, "Nothing to undo!");
            }
        } 
        else if (c == 't' || c == 'T') {
            configure_time_control();
            strcpy(sys_log, "Settings successfully configured.");
        } 
        else if (c == ' ' || c == '\n') {
            int current_idx = cursor_y * 8 + cursor_x;
            if (selected_idx == -1) {
                // Select your color piece
                char select_p = state.board[current_idx];
                if (select_p != '.' && isupper(select_p)) {
                    selected_idx = current_idx;
                    sprintf(sys_log, "Selected %c on %c%d", select_p, 'a' + cursor_x, 8 - cursor_y);
                } else {
                    strcpy(sys_log, "Select your own piece (White)!");
                }
            } else {
                // Attempt execution to target square
                if (is_pseudo_legal(&state, selected_idx, current_idx)) {
                    char move_str[8];
                    int from_col = selected_idx % 8;
                    int from_row = selected_idx / 8;
                    int to_col = current_idx % 8;
                    int to_row = current_idx / 8;

                    sprintf(move_str, "%c%d%c%d", 'a' + from_col, 8 - from_row, 'a' + to_col, 8 - to_row);

                    // Auto-append promotion format to UCI syntax
                    if (toupper(state.board[selected_idx]) == 'P' && (to_row == 0 || to_row == 7)) {
                        strcat(move_str, "q"); // Auto promotion to queen
                    }

                    strncpy(move_history[move_count++], move_str, 7);
                    rebuild_board();
                    selected_idx = -1;
                    sprintf(sys_log, "Executed move %s", move_str);
                } else {
                    selected_idx = -1; // Deselect on illegal move
                    strcpy(sys_log, "Illegal move sequence! Target deselected.");
                }
            }
        }
        draw_interface(sys_log);
    }

    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
    }
    return 0;
}
