#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <signal.h>

// Global State
struct termios orig_termios;
pid_t engine_pid = -1;
int engine_in[2];
int engine_out[2];
bool engine_turn = false;
bool engine_dead = false;
char engine_path[256] = "stockfish";

char board[8][8] = {
    {'r','n','b','q','k','b','n','r'},
    {'p','p','p','p','p','p','p','p'},
    {' ',' ',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ',' ',' '},
    {'P','P','P','P','P','P','P','P'},
    {'R','N','B','Q','K','B','N','R'}
};

int cursor_x = 4, cursor_y = 6;
int selected_x = -1, selected_y = -1;
char last_move[10] = "None";

char history[1024][6];
int history_count = 0;

char eng_buf[4096];
int eng_buf_len = 0;

// Cleanup terminal and processes on exit
void cleanup() {
    if (engine_pid > 0) {
        kill(engine_pid, SIGKILL);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    printf("\x1b[?25h\x1b[2J\x1b[H"); // Show cursor, clear screen
}

void setup_terminal() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(cleanup);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    // Set non-blocking input
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
    printf("\x1b[?25l"); // Hide cursor
}

void start_engine(const char* path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) exit(1);
    
    engine_pid = fork();
    if (engine_pid < 0) exit(1);
    
    if (engine_pid == 0) {
        // Child process - engine
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[0]); close(engine_in[1]);
        close(engine_out[0]); close(engine_out[1]);
        execlp(path, path, NULL);
        exit(1); // Exit if engine not found
    }
    
    // Parent process
    close(engine_in[0]);
    close(engine_out[1]);
    fcntl(engine_out[0], F_SETFL, O_NONBLOCK);
    
    write(engine_in[1], "uci\n", 4);
    write(engine_in[1], "isready\n", 8);
}

const char* get_piece_str(char p) {
    switch(p) {
        case 'K': return "♔"; case 'Q': return "♕";
        case 'R': return "♖"; case 'B': return "♗";
        case 'N': return "♘"; case 'P': return "♙";
        case 'k': return "♚"; case 'q': return "♛";
        case 'r': return "♜"; case 'b': return "♝";
        case 'n': return "♞"; case 'p': return "♟";
        default:  return " ";
    }
}

void draw_board() {
    printf("\x1b[H"); // Move terminal cursor to top-left
    printf("  \x1b[1mTerminal Chess GUI\x1b[0m (Engine: %s)\r\n", engine_path);
    printf("  Arrows: Move | SPACE/ENTER: Select | 'q': Quit\r\n\r\n");

    for (int y = 0; y < 8; ++y) {
        printf(" \x1b[90m%d\x1b[0m ", 8 - y); // Rank label
        for (int x = 0; x < 8; ++x) {
            bool is_light = (x + y) % 2 == 0;
            bool is_cursor = (x == cursor_x && y == cursor_y);
            bool is_selected = (x == selected_x && y == selected_y);
            
            // Determine background ANSI color
            if (is_selected) printf("\x1b[42m"); // Green
            else if (is_cursor) printf("\x1b[43m"); // Yellow
            else if (is_light) printf("\x1b[47m"); // White
            else printf("\x1b[46m"); // Cyan (Dark Square)
            
            // Print piece in black text for contrast against colored backgrounds
            printf("\x1b[30m %s \x1b[0m", get_piece_str(board[y][x]));
        }
        printf("\r\n");
    }
    printf("    \x1b[90ma  b  c  d  e  f  g  h\x1b[0m\r\n\r\n");
    
    if (engine_dead) printf(" \x1b[31mStatus: Engine died or not found in PATH!\x1b[0m            \r\n");
    else if (engine_turn) printf(" Status: Engine is thinking...                        \r\n");
    else printf(" Status: Your turn (You play White)                   \r\n");
    
    printf(" Last Move: %-10s                                \r\n", last_move);
    fflush(stdout);
}

void apply_move(const char* m) {
    if (strlen(m) < 4) return;
    int sx = m[0] - 'a', sy = 7 - (m[1] - '1');
    int dx = m[2] - 'a', dy = 7 - (m[3] - '1');
    char p = board[sy][sx];

    // Basic en passant clear (pawn moved diagonally to empty square)
    if ((p == 'P' || p == 'p') && sx != dx && board[dy][dx] == ' ') board[sy][dx] = ' ';

    // Basic castling rook move
    if ((p == 'K' || p == 'k') && abs(sx - dx) == 2) {
        if (dx == 6) { board[sy][5] = board[sy][7]; board[sy][7] = ' '; } // Kingside
        if (dx == 2) { board[sy][3] = board[sy][0]; board[sy][0] = ' '; } // Queenside
    }

    board[dy][dx] = p;
    board[sy][sx] = ' ';

    // Promotion
    if (m[4] != '\0' && m[4] != ' ' && m[4] != '\n') {
        if (p == 'P') board[dy][dx] = (m[4] == 'q') ? 'Q' : (m[4] - 32);
        else if (p == 'p') board[dy][dx] = m[4];
    } else if ((p == 'P' && dy == 0) || (p == 'p' && dy == 7)) {
        board[dy][dx] = (p == 'P') ? 'Q' : 'q'; // Default to Queen
    }
}

void send_history_to_engine() {
    char cmd[8192] = "position startpos moves";
    for (int i = 0; i < history_count; ++i) {
        strcat(cmd, " "); strcat(cmd, history[i]);
    }
    strcat(cmd, "\ngo movetime 1000\n"); // Give engine 1 second
    write(engine_in[1], cmd, strlen(cmd));
}

void handle_input() {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return;
    
    if (c == 'q' || c == 'Q' || c == 3) exit(0); // 3 is Ctrl-C
    
    if (c == '\x1b') { // Arrow keys generate a 3-byte escape sequence
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
            if (seq[0] == '[') {
                switch(seq[1]) {
                    case 'A': if (cursor_y > 0) cursor_y--; break; // Up
                    case 'B': if (cursor_y < 7) cursor_y++; break; // Down
                    case 'C': if (cursor_x < 7) cursor_x++; break; // Right
                    case 'D': if (cursor_x > 0) cursor_x--; break; // Left
                }
            }
        }
    } else if ((c == ' ' || c == '\r' || c == '\n') && !engine_turn) {
        if (selected_x == -1) {
            // Select piece (only allow selecting white pieces)
            char p = board[cursor_y][cursor_x];
            if (p != ' ' && p >= 'A' && p <= 'Z') {
                selected_x = cursor_x; selected_y = cursor_y;
            }
        } else {
            // Deselect if same square is clicked
            if (selected_x == cursor_x && selected_y == cursor_y) {
                selected_x = -1; selected_y = -1;
            } else {
                // Apply Move
                char m[6];
                m[0] = 'a' + selected_x; m[1] = '1' + (7 - selected_y);
                m[2] = 'a' + cursor_x;   m[3] = '1' + (7 - cursor_y);
                
                // Auto promote to Queen if moving pawn to last rank
                if (board[selected_y][selected_x] == 'P' && cursor_y == 0) {
                    m[4] = 'q'; m[5] = '\0';
                } else {
                    m[4] = '\0';
                }

                apply_move(m);
                strcpy(history[history_count++], m);
                strcpy(last_move, m);
                
                selected_x = -1; selected_y = -1;
                engine_turn = true;
                send_history_to_engine();
            }
        }
    }
}

void poll_engine() {
    char buf[256];
    int n = read(engine_out[0], buf, sizeof(buf));
    if (n == 0) engine_dead = true; // Engine pipe closed
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                eng_buf[eng_buf_len] = '\0';
                if (strncmp(eng_buf, "bestmove ", 9) == 0) {
                    char best[6];
                    if (sscanf(eng_buf + 9, "%5s", best) == 1 && strcmp(best, "(none)") != 0) {
                        apply_move(best);
                        strcpy(history[history_count++], best);
                        strcpy(last_move, best);
                        engine_turn = false;
                    }
                }
                eng_buf_len = 0;
            } else if (buf[i] != '\r') {
                if (eng_buf_len < sizeof(eng_buf) - 1) eng_buf[eng_buf_len++] = buf[i];
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc > 1) strncpy(engine_path, argv[1], sizeof(engine_path) - 1);
    
    printf("\x1b[2J"); // Clear screen initially
    setup_terminal();
    start_engine(engine_path);
    
    while (1) {
        draw_board();
        
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        if (!engine_dead) FD_SET(engine_out[0], &read_fds);
        
        struct timeval tv = {0, 50000}; // Update screen/poll every 50ms
        int max_fd = STDIN_FILENO > engine_out[0] ? STDIN_FILENO : engine_out[0];
        
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret > 0) {
            if (FD_ISSET(STDIN_FILENO, &read_fds)) handle_input();
            if (!engine_dead && FD_ISSET(engine_out[0], &read_fds)) poll_engine();
        }
    }
    return 0;
}
