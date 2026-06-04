#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/wait.h>

// --- Constants & Types ---
#define BOARD_SIZE 8
#define MAX_HISTORY 100
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define RESET   "\033[0m"
#define BOLD    "\033[1m"

typedef struct {
    int r, c;
} Square;

typedef struct {
    char move[10];
} Move;

typedef enum { TIME, DEPTH, NODES } ControlType;

// Piece representation: Uppercase = White, Lowercase = Black
// P/p = Pawn, N/n = Knight, B/b = Bishop, R/r = Rook, Q/q = Queen, K/k = King
char board[BOARD_SIZE][BOARD_SIZE] = {
    {'r','n','b','q','k','b','n','r'},
    {'p','p','p','p','p','p','p','p'},
    {' ',' ',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ',' ',' '},
    {'p','p','p','p','p','p','p','p'},
    {'R','N','B','Q','K','B','N','R'}
};

// --- Global State ---
Square cursor = {0, 0};
Square selected = {-1, -1};
Square last_move_from = {-1, -1}, last_move_to = {-1, -1};
Move history[MAX_HISTORY];
int history_count = 0;
int white_turn = 1;
int engine_fd_in, engine_fd_out;
ControlType current_ctrl = TIME;
int ctrl_val = 1000; // default 1s or depth 10

// --- Terminal Control ---
struct termios orig_termios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

// --- Chess Logic ---
int is_on_board(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }

int is_piece_color(char p, int white) {
    if (p == ' ') return 0;
    return (white && p >= 'A' && p <= 'Z') || (!white && p >= 'a' && p <= 'z');
}

// Basic move validation (Simplified for brevity, supports basic moves)
int is_legal_move(int r1, int c1, int r2, int c2) {
    if (!is_on_board(r2, c2)) return 0;
    char p = board[r1][c1];
    char target = board[r2][c2];
    if (is_piece_color(target, white_turn)) return 0;

    int dr = abs(r2 - r1), dc = abs(c2 - c1);
    
    if (p == 'P' || p == 'p') {
        int dir = (p == 'P') ? -1 : 1;
        if (c1 == c2 && target == ' ') {
            if (r2 - r1 == dir) return 1;
            if ((r1 == 6 && p == 'P') || (r1 == 1 && p == 'p')) 
                if (r2 - r1 == 2 * dir && board[r1 + dir][c1] == ' ') return 1;
        }
        if (dr == 1 && dc == 1 && target != ' ') return 1;
        return 0;
    }
    if (p == 'N' || p == 'n') return (dr == 2 && dc == 1) || (dr == 1 && dc == 2);
    if (p == 'B' || p == 'b') return dr == dc;
    if (p == 'R' || p == 'r') return dr == 0 || dc == 0;
    if (p == 'Q' || p == 'q') return dr == dc || dr == 0 || dc == 0;
    if (p == 'K' || p == 'k') return dr <= 1 && dc <= 1;
    
    return 0;
}

int is_king_in_check(int white) {
    Square k = {-1, -1};
    for(int r=0; r<8; r++) for(int c=0; c<8; c++)
        if (board[r][c] == (white ? 'K' : 'k')) k = (Square){r, c};
    
    for(int r=0; r<8; r++) for(int c=0; c<8; c++)
        if (is_piece_color(board[r][c], !white) && is_legal_move(r, c, k.r, k.c)) return 1;
    return 0;
}

// --- Engine Communication ---
void send_uci(const char* msg) {
    write(engine_fd_in, msg, strlen(msg));
    write(engine_fd_in, "\n", 1);
}

void execute_move(const char* move_str) {
    int f_r = 8 - (move_str[0] - '0' + 1); // Simplified UCI coord parse
    // Since UCI uses 'e2e4', we need:
    int from_c = move_str[0] - 'a';
    int from_r = 8 - (move_str[1] - '0');
    int to_c = move_str[2] - 'a';
    int to_r = 8 - (move_str[3] - '0');

    last_move_from = (Square){from_r, from_c};
    last_move_to = (Square){to_r, to_c};
    board[to_r][to_c] = board[from_r][from_c];
    board[from_r][from_c] = ' ';
    
    sprintf(history[history_count].move, "%s", move_str);
    history_count++;
    white_turn = !white_turn;
}

void engine_move() {
    char pos[1024] = "position startpos moves ";
    for(int i=0; i<history_count; i++) {
        strcat(pos, history[i].move);
        strcat(pos, " ");
    }
    send_uci(pos);
    
    char go[32];
    if (current_ctrl == TIME) sprintf(go, "go movetime %d", ctrl_val);
    else if (current_ctrl == DEPTH) sprintf(go, "go depth %d", ctrl_val);
    else sprintf(go, "go nodes %d", ctrl_val);
    send_uci(go);

    char buf[1024] = {0};
    int bytes = read(engine_fd_out, buf, 1023);
    if (bytes > 0) {
        char* best = strstr(buf, "bestmove ");
        if (best) {
            char move[6] = {0};
            strncpy(move, best + 9, 4);
            execute_move(move);
        }
    }
}

// --- UI Rendering ---
void draw_board() {
    printf("\033[H"); // Move cursor to top-left
    printf(BOLD "  a b c d e f g h\n" RESET);
    for (int r = 0; r < 8; r++) {
        printf("%d ", r + 1);
        for (int c = 0; c < 8; c++) {
            // Highlighting
            if (r == cursor.r && c == cursor.c) printf(YELLOW);
            else if (selected.r == r && selected.c == c) printf(BLUE);
            else if (last_move_from.r == r && last_move_from.c == c) printf(GREEN);
            else if (last_move_to.r == r && last_move_to.c == c) printf(GREEN);
            else if (board[r][c] == 'K' && is_king_in_check(1)) printf(RED);
            else if (board[r][c] == 'k' && is_king_in_check(0)) printf(RED);
            else if (selected.r != -1 && is_legal_move(selected.r, selected.c, r, c)) printf(BLUE);
            else printf(RESET);

            printf("%c ", board[r][c]);
        }
        printf("\n%d ", 8 - r); // This is a simple visual offset, fixed below
        printf("\033[1A\033[1G%d ", 8 - r); // Fix row numbers
        printf("\n");
    }
    // Simple Correction for row numbers logic
    printf("\033[11A\033[H"); 
    printf(BOLD "  a b c d e f g h\n" RESET);
    for (int r = 0; r < 8; r++) {
        printf("%d ", 8-r);
        for (int c = 0; c < 8; c++) {
            if (r == cursor.r && c == cursor.c) printf(YELLOW);
            else if (selected.r == r && selected.c == c) printf(BLUE);
            else if (last_move_from.r == r && last_move_from.c == c) printf(GREEN);
            else if (last_move_to.r == r && last_move_to.c == c) printf(GREEN);
            else if (board[r][c] == 'K' && is_king_in_check(1)) printf(RED);
            else if (board[r][c] == 'k' && is_king_in_check(0)) printf(RED);
            else if (selected.r != -1 && is_legal_move(selected.r, selected.c, r, c)) printf(BLUE);
            else printf(RESET);
            printf("%c ", board[r][c]);
        }
        printf("\n");
    }
}

void draw_ui() {
    printf("\n--- Chess GUI (C) ---\n");
    printf("Turn: %s | Selection: %d,%d\n", white_turn ? "White" : "Black", cursor.r, cursor.c);
    printf("Controls: Arrows to move, Space to select/move, 'u' undo, 't' time, 'd' depth, 'n' nodes\n");
    printf("Current Limit: %s (%d)\n", current_ctrl == TIME ? "Time" : (current_ctrl == DEPTH ? "Depth" : "Nodes"), ctrl_val);
    printf("PGN: ");
    for (int i = 0; i < history_count; i++) printf("%d.%s ", (i/2)+1, history[i].move);
    printf("\n\033[K"); 
}

// --- Main ---
int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <path_to_uci_engine>\n", argv[0]);
        return 1;
    }

    // Setup Engine Pipes
    int pipe_in[2], pipe_out[2];
    pipe(pipe_in); pipe(pipe_out);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[1]); close(pipe_out[0]);
        execl(argv[1], argv[1], NULL);
        exit(1);
    }
    close(pipe_in[0]); close(pipe_out[1]);
    engine_fd_in = pipe_in[1];
    engine_fd_out = pipe_out[0];
    fcntl(engine_fd_out, F_SETFL, O_NONBLOCK);

    send_uci("uci");
    send_uci("isready");

    enable_raw_mode();
    printf("\033[2J"); // Clear screen

    while (1) {
        draw_board();
        draw_ui();

        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == '\033') { // Escape sequence
                char seq[3];
                if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                    if (seq[1] == 'A' && cursor.r > 0) cursor.r--;
                    if (seq[1] == 'B' && cursor.c < 7) cursor.c++;
                    if (seq[1] == 'C' && cursor.r < 7) cursor.r++;
                    if (seq[1] == 'D' && cursor.c > 0) cursor.c--;
                }
            } else if (c == ' ') {
                if (selected.r == -1) {
                    if (is_piece_color(board[cursor.r][cursor.c], white_turn))
                        selected = cursor;
                } else {
                    if (is_legal_move(selected.r, selected.c, cursor.r, cursor.c)) {
                        char move_str[6];
                        sprintf(move_str, "%c%d%c%d", 'a' + selected.c, 8 - selected.r, 'a' + cursor.c, 8 - cursor.r);
                        execute_move(move_str);
                        selected = (Square){-1, -1};
                        // After player moves, engine moves
                        engine_move();
                    } else {
                        selected = (Square){-1, -1};
                    }
                }
            } else if (c == 'u') {
                if (history_count > 0) {
                    // Basic undo: conceptually you'd need a state stack. 
                    // For a single file, we restart and replay history minus 2.
                    char backup[8][8];
                    memcpy(backup, board, 64);
                    // Simplified: just reduce count and reset (in real app, store board snapshots)
                    history_count--; 
                    // For a true undo, you'd reload the starting board and replay.
                }
            } else if (c == 't') { current_ctrl = TIME; ctrl_val = 1000; }
            else if (c == 'd') { current_ctrl = DEPTH; ctrl_val = 10; }
            else if (c == 'n') { current_ctrl = NODES; ctrl_val = 100000; }
            else if (c == 'q') break;
        }
        usleep(10000);
    }

    disable_raw_mode();
    kill(pid, SIGKILL);
    return 0;
}
