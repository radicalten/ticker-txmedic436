// my commentary: amazing GUI using only the terminal and unicode chess peices. 
// The black and white squares are quite nice as is filling in the white and black pieces with color similarly inspired
// The only change I would make is change white and black squares to true white and black instead of the gray tones used "charcoal"
// Likewise the Blue aka black peices should be bright red ]/ ANSI format instead of bright blue for visibility reasons. 
// Or could change square colors to white and green or white and blue or brown and peach, or some other such visible color. 
// Winning chess gui so far, it auto reads the stockfish path and can allegedly be updated with a new path, that will come in handy. 

/*
Here is a complete, single-file C program that implements a fully functional, beautiful command-line Chess GUI for the macOS Terminal. It communicates with any UCI-compliant engine (like Stockfish) using UNIX pipes, manages game flow, renders a color-coded board with Unicode pieces, handles flips based on player color, and detects checkmates.
Prerequisites

To play against a real chess engine, you should have Stockfish installed. On macOS, you can easily install it via Homebrew:

brew install stockfish

Compilation
You can compile this code directly inside your Mac Terminal using clang or gcc:
clang -O3 -o chess_gui chess_gui.c

How to Run and Play

    Run the compiled executable:
./chess_gui

    The game will auto-detect Stockfish. If not found, it will ask you to supply your custom binary path.
    Choose your color (w for White, b for Black).
    Enter the engine's processing time (default is 1000 milliseconds for a fast but challenging opponent).
    Input your moves using standard UCI algebraic notation:
        e2e4 (Move Pawn from e2 to e4)
        g1f3 (Move Knight from g1 to f3)
        e1g1 (White Kingside Castle)
        e7e8q (Pawn promotion to Queen)
    Type quit at any point to exit cleanly.
*/

/* 
Ansi-Colors here https://en.wikipedia.org/wiki/ANSI_escape_code
ANSI sets background and foreground colors independently so you can have a white page (BG) with black text (FG) on top, or any combination. 

Select Graphic Redition Parameters
The control sequence CSI n m, named Select Graphic Rendition (SGR), sets display attributes. Several attributes can be set in the same sequence, separated by semicolons.[25] Each display attribute remains in effect until a following occurrence of SGR resets it.[16] If no codes are given, CSI m is treated as CSI 0 m (reset / normal). 

The original specification only had 8 colors, and just gave them names. The SGR parameters 30–37 selected the foreground color, while 40–47 selected the background. Quite a few terminals implemented "bold" (SGR code 1) as a brighter color rather than a different font, thus providing 8 additional foreground colors. Usually you could not get these as background colors, though sometimes inverse video (SGR code 7) would allow that. Examples: to get black letters on white background use ESC[30;47m, to get red use ESC[31m, to get bright red use ESC[1;31m. To reset colors to their defaults, use ESC[39;49m (not supported on some terminals), or reset all attributes with ESC[0m. Later terminals added the ability to directly specify the "bright" colors with 90–97 and 100–107.  The chart below shows a few examples of how classical standards and modern terminal emulators translate the 4-bit color codes into 24-bit color codes. 

Examples: to get black letters on white background use ESC[30;47m

Black:  FG30, BG40
Red:    FG31, BG41
Green:  FG32, BG42
Yellow: FG33, BG43
Blue:   FG34, BG44
Magenta:FG35, BG45
Cyan:   FG36, BG46
White:  FG37, BG47

BrightBlack:  FG90, BG100
BrightRed:    FG91, BG101
BrightGreen:  FG92, BG102
BrightYellow: FG93, BG103
BrightBlue:   FG94, BG104
BrightMagenta:FG95, BG105
BrightCyan:   FG96, BG106
BrightWhite:  FG97, BG107

256-color mode
ESC[38;5;⟨n⟩m Select foreground color      where n is a number from the table below
ESC[48;5;⟨n⟩m Select background color
  0-  7:  standard colors (as in ESC [ 30–37 m)
  8- 15:  high intensity colors (as in ESC [ 90–97 m)
 16-231:  6 × 6 × 6 cube (216 colors): 16 + 36 × r + 6 × g + b (0 ≤ r, g, b ≤ 5)
232-255:  grayscale from dark to light in 24 steps

For Chess:
Dark Squares:  printf("\e[48;5;172m"); #d78700 (215, 135, 0) Harvest Gold color
Light Squares: printf("\e[48;5;223m"); #ffd7af (255, 215, 175) Peach color
White Pieces:  printf("\e[97m"); BrightWhite (255, 255, 255) White color 1; = bold
Black Pieces:  printf("\e[30m"); Black (0, 0, 0) Black color 0; = reset modifiers.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>

// Global State
char board[8][8] = {
    {'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'},
    {'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'},
    {'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'}
};

char history[1024][8];
int move_count = 0;

int to_engine[2];
int from_engine[2];
pid_t engine_pid = -1;

// UCI Engine I/O Helpers
void send_to_engine(int fd, const char *cmd) {
    write(fd, cmd, strlen(cmd));
}

int read_line(int fd, char *buf, int max_len) {
    int i = 0;
    char c;
    while (i < max_len - 1) {
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) {
            buf[i] = '\0';
            return i; 
        }
        if (c == '\n') break;
        if (c != '\r') {
            buf[i++] = c;
        }
    }
    buf[i] = '\0';
    return i;
}

// Check if an engine is reachable/executable
int check_engine(const char *path) {
    if (access(path, X_OK) == 0) return 1;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "which %s > /dev/null 2>&1", path);
    if (system(cmd) == 0) return 1;
    return 0;
}

// Start UCI engine subprocess
int start_engine(const char *path) {
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) {
        perror("Pipe creation failed");
        return 0;
    }

    engine_pid = fork();
    if (engine_pid < 0) {
        perror("Fork failed");
        return 0;
    }

    if (engine_pid == 0) { // Child Process
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);

        close(to_engine[0]);
        close(to_engine[1]);
        close(from_engine[0]);
        close(from_engine[1]);

        execl(path, path, (char *)NULL);
        // If execl fails:
        perror("Failed to execute engine binary");
        exit(1);
    } else { // Parent Process
        close(to_engine[0]);
        close(from_engine[1]);
    }
    return 1;
}

void init_uci() {
    char buf[1024];
    send_to_engine(to_engine[1], "uci\n");
    while (1) {
        read_line(from_engine[0], buf, sizeof(buf));
        if (strcmp(buf, "uciok") == 0) break;
    }

    send_to_engine(to_engine[1], "isready\n");
    while (1) {
        read_line(from_engine[0], buf, sizeof(buf));
        if (strcmp(buf, "readyok") == 0) break;
    }
    send_to_engine(to_engine[1], "ucinewgame\n");
}

void get_engine_move(char history_list[][8], int count, char *best_move, int movetime_ms) {
    char cmd[8192] = "position startpos";
    if (count > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < count; i++) {
            strcat(cmd, " ");
            strcat(cmd, history_list[i]);
        }
    }
    strcat(cmd, "\n");
    send_to_engine(to_engine[1], cmd);

    char go_cmd[64];
    snprintf(go_cmd, sizeof(go_cmd), "go movetime %d\n", movetime_ms);
    send_to_engine(to_engine[1], go_cmd);

    char buf[1024];
    best_move[0] = '\0';
    while (1) {
        if (read_line(from_engine[0], buf, sizeof(buf)) <= 0) break;
        if (strncmp(buf, "bestmove", 8) == 0) {
            sscanf(buf, "bestmove %s", best_move);
            break;
        }
    }
}

// Clean termination
void cleanup() {
    if (engine_pid > 0) {
        send_to_engine(to_engine[1], "quit\n");
        close(to_engine[1]);
        close(from_engine[0]);
        int status;
        waitpid(engine_pid, &status, 0);
    }
}

// Move Execution on internal Board State
int parse_move(const char *input, int *from_r, int *from_c, int *to_r, int *to_c) {
    if (strlen(input) < 4) return 0;
    *from_c = input[0] - 'a';
    *from_r = 8 - (input[1] - '0');
    *to_c = input[2] - 'a';
    *to_r = 8 - (input[3] - '0');
    return 1;
}

void make_move(const char *move_str) {
    int from_r, from_c, to_r, to_c;
    if (!parse_move(move_str, &from_r, &from_c, &to_r, &to_c)) return;

    char piece = board[from_r][from_c];

    // Handle Castling
    if (tolower(piece) == 'k' && abs(to_c - from_c) == 2) {
        if (to_c == 6) { // Kingside
            board[to_r][5] = board[to_r][7];
            board[to_r][7] = '.';
        } else if (to_c == 2) { // Queenside
            board[to_r][3] = board[to_r][0];
            board[to_r][0] = '.';
        }
    }

    // Handle En Passant
    if (tolower(piece) == 'p' && from_c != to_c && board[to_r][to_c] == '.') {
        board[from_r][to_c] = '.';
    }

    // Standard execution
    board[to_r][to_c] = piece;
    board[from_r][from_c] = '.';

    // Handle Promotion
    if (strlen(move_str) == 5) {
        char promo = move_str[4];
        if (isupper(piece)) {
            board[to_r][to_c] = toupper(promo);
        } else {
            board[to_r][to_c] = tolower(promo);
        }
    }
}

// Graphical UI rendering helpers
const char* get_piece_char(char piece) {
    switch(tolower(piece)) {
        case 'p': return "♟";
        case 'r': return "♜";
        case 'n': return "♞";
        case 'b': return "♝";
        case 'q': return "♛";
        case 'k': return "♚";
        default:  return " ";
    }
}

void draw_square(int r, int c) {
    int is_dark = (r + c) % 2;
    // Set Background Colors (Charcoal for dark, light grey for light)
    if (is_dark) {
        printf("\e[48;5;172m");
    } else {
        printf("\e[48;5;223m");
    }

    char p = board[r][c];
    if (p != '.') {
        // Foreground styling (White pieces are White, Black pieces are Black)
        if (isupper(p)) {
            printf("\e[97m"); 
        } else {
            printf("\e[30m"); 
        }
        printf(" %s ", get_piece_char(p));
    } else {
        printf("   ");
    }
}

void draw_board(char player_color) {
    printf("\e[1;1H\e[2J"); // Clear Screen
    printf("\n   =================================\n");
    printf("        Mac Terminal Chess GUI\n");
    printf("   =================================\n\n");

    if (player_color == 'w') {
        printf("     A  B  C  D  E  F  G  H\n");
        for (int r = 0; r < 8; r++) {
            printf("  %d ", 8 - r);
            for (int c = 0; c < 8; c++) {
                draw_square(r, c);
            }
            printf("\e[0m %d\n", 8 - r);
        }
        printf("     A  B  C  D  E  F  G  H\n\n");
    } else {
        printf("     H  G  F  E  D  C  B  A\n");
        for (int r = 7; r >= 0; r--) {
            printf("  %d ", 8 - r);
            for (int c = 7; c >= 0; c--) {
                draw_square(r, c);
            }
            printf("\e[0m %d\n", 8 - r);
        }
        printf("     H  G  F  E  D  C  B  A\n\n");
    }
}

int is_valid_input_format(const char *input) {
    if (strlen(input) < 4) return 0;
    if (input[0] < 'a' || input[0] > 'h') return 0;
    if (input[1] < '1' || input[1] > '8') return 0;
    if (input[2] < 'a' || input[2] > 'h') return 0;
    if (input[3] < '1' || input[3] > '8') return 0;
    if (strlen(input) == 5) {
        char p = tolower(input[4]);
        if (p != 'q' && p != 'r' && p != 'b' && p != 'n') return 0;
    }
    return 1;
}

int main() {
    char engine_path[1024] = "";
    const char *default_paths[] = {
        "/opt/homebrew/bin/stockfish", // Apple Silicon Homebrew
        "/usr/local/bin/stockfish",   // Intel Homebrew
        "./stockfish",
        "stockfish"
    };

    int found = 0;
    for (int i = 0; i < 4; i++) {
        if (check_engine(default_paths[i])) {
            strcpy(engine_path, default_paths[i]);
            found = 1;
            break;
        }
    }

    if (!found) {
        printf("Could not find Stockfish in default Mac paths.\n");
        while (1) {
            printf("Please enter path to a UCI engine (e.g., Stockfish binary path): ");
            if (fgets(engine_path, sizeof(engine_path), stdin) == NULL) return 1;
            engine_path[strcspn(engine_path, "\n")] = 0;
            if (check_engine(engine_path)) break;
            printf("Invalid binary path or file not executable!\n");
        }
    }

    if (!start_engine(engine_path)) {
        fprintf(stderr, "Could not start the chess engine.\n");
        return 1;
    }

    init_uci();

    char player_color = 'w';
    printf("Choose your color (w/b) [default: w]: ");
    char color_input[10];
    if (fgets(color_input, sizeof(color_input), stdin) != NULL) {
        if (color_input[0] == 'b' || color_input[0] == 'B') {
            player_color = 'b';
        }
    }

    int movetime = 1000;
    printf("Set engine thinking time (ms) [default: 1000]: ");
    char time_input[32];
    if (fgets(time_input, sizeof(time_input), stdin) != NULL && time_input[0] != '\n') {
        int temp = atoi(time_input);
        if (temp > 50) movetime = temp;
    }

    // Main Game Loop Setup
    char current_turn = 'w';
    
    // If Engine is White, it moves first
    if (player_color == 'b') {
        draw_board(player_color);
        printf("Engine is thinking...\n");
        char engine_move[16];
        get_engine_move(history, move_count, engine_move, movetime);
        make_move(engine_move);
        strcpy(history[move_count++], engine_move);
    }

    while (1) {
        draw_board(player_color);

        // Instantaneous, low-cost engine check (10ms limit) to see if Player is mated/stalemated
        char check_move[16];
        get_engine_move(history, move_count, check_move, 10);
        if (strcmp(check_move, "(none)") == 0 || strcmp(check_move, "0000") == 0 || strlen(check_move) == 0) {
            printf("Game Over! Checkmate or Stalemate.\n");
            break;
        }

        // Get Player Move
        char input[64];
        int from_r, from_c, to_r, to_c;
        while (1) {
            printf("Your turn (e.g. e2e4, or 'quit'): ");
            if (fgets(input, sizeof(input), stdin) == NULL) {
                cleanup();
                return 0;
            }
            input[strcspn(input, "\n")] = 0;

            if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
                cleanup();
                return 0;
            }

            if (!is_valid_input_format(input)) {
                printf("Error: Use coordinates (e.g. e2e4) or e7e8q for promotion.\n");
                continue;
            }

            parse_move(input, &from_r, &from_c, &to_r, &to_c);
            char piece = board[from_r][from_c];

            if (piece == '.') {
                printf("Error: Empty start square!\n");
                continue;
            }

            if (player_color == 'w' && !isupper(piece)) {
                printf("Error: That is a Black piece!\n");
                continue;
            }
            if (player_color == 'b' && !islower(piece)) {
                printf("Error: That is a White piece!\n");
                continue;
            }

            // Move is conceptually valid, perform it.
            make_move(input);
            strcpy(history[move_count++], input);
            break;
        }

        draw_board(player_color);

        // Engine's Turn
        printf("Engine is thinking...\n");
        char engine_move[16];
        get_engine_move(history, move_count, engine_move, movetime);

        if (strcmp(engine_move, "(none)") == 0 || strcmp(engine_move, "0000") == 0 || strlen(engine_move) == 0) {
            printf("Game Over! You win!\n");
            break;
        }

        make_move(engine_move);
        strcpy(history[move_count++], engine_move);
        printf("Engine played: %s\n", engine_move);
    }

    cleanup();
    return 0;
}
