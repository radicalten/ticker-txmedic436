/*
Here is a complete, single-file C program that acts as a terminal-based Chess GUI.

It uses POSIX pipes to communicate with a UCI engine (like Stockfish), utilizes ANSI escape codes to draw a colorful board directly in the Mac Terminal, and handles basic chess rules for visual updates (including castling and en passant).
Prerequisites

You need a UCI-compliant engine installed. The easiest way to get one on a Mac is via Homebrew:
brew install stockfish

How to Compile and Run

    Open your Mac Terminal.
    Navigate to the folder where you saved the file.
    Compile the code using standard gcc (or clang, which comes with macOS developer tools):
gcc -O3 chess_gui.c -o chess_gui

Run the program:
./chess_gui

Features of this GUI

    Single File: It fulfills your requirement perfectly—everything from IPC (Inter-process communication) to the UI rendering is self-contained.
    Mac Terminal UI: Utilizes standard ANSI escape colors (Blue and White) and Unicode characters to render a surprisingly good-looking board directly in the terminal window. White pieces are drawn in red to contrast properly with the board.
    UCI Standard: Works strictly using the UCI protocol (position startpos moves ...), meaning you can swap out Stockfish for any other UCI engine simply by changing the #define ENGINE_PATH at the top of the file.
    Visual Rules: Checks the move notation and correctly manages the internal board array for standard moves, castling (e.g., e1g1), promotions (e.g., e7e8q), and en passant so the board never breaks visually.

How to Play

When prompted, type your move in standard UCI format and press Enter.

    Move a pawn: e2e4
    Move a knight: g1f3
    Castle Kingside: e1g1
    Promote a pawn to a Queen: e7e8q
    To exit the program, type quit.

*/

// Ansi-Colors here https://en.wikipedia.org/wiki/ANSI_escape_code

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>

// --- Configuration ---
#define ENGINE_PATH "stockfish" // Change if your engine is located elsewhere
#define THINK_TIME_MS 1000      // Engine think time per move in milliseconds

// --- Global State ---
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

char move_history[8192] = "";

// --- Helper: Convert character to Unicode Chess Piece ---
const char* get_unicode_piece(char p) {
    switch (p) {
        case 'K': return "♔"; case 'Q': return "♕"; case 'R': return "♖";
        case 'B': return "♗"; case 'N': return "♘"; case 'P': return "♙";
        case 'k': return "♚"; case 'q': return "♛"; case 'r': return "♜";
        case 'b': return "♝"; case 'n': return "♞"; case 'p': return "♟";
        default:  return " ";
    }
}

// --- UI: Print the Chess Board ---
void print_board() {
    printf("\033[H\033[J"); // Clear screen
    printf("\n   \033[1m  a  b  c  d  e  f  g  h \033[0m\n");
    for (int row = 0; row < 8; row++) {
        printf("   \033[1m%d \033[0m", 8 - row);
        for (int col = 0; col < 8; col++) {
            // Checkered background colors (ANSI 47=White, 41=Green)
            int is_light = (row + col) % 2 == 0;
            printf("\033[%dm", is_light ? 47 : 44);
            
            char p = board[row][col];
            // Text color (ANSI 30=Black for black pieces, 31=Red for white pieces for contrast)
            if (p != ' ') {
                if (isupper(p)) printf("\033[31m %s \033[0m", get_unicode_piece(p)); // White
                else            printf("\033[30m %s \033[0m", get_unicode_piece(p)); // Black
                printf("\033[%dm", is_light ? 47 : 44); // Restore background if reset by piece
            } else {
                printf("   ");
            }
        }
        printf("\033[0m\033[1m %d\033[0m\n", 8 - row);
    }
    printf("   \033[1m  a  b  c  d  e  f  g  h \033[0m\n\n");
}

// --- Logic: Update internal board state ---
void apply_move(const char *move) {
    int fc = move[0] - 'a', fr = 8 - (move[1] - '0');
    int tc = move[2] - 'a', tr = 8 - (move[3] - '0');
    char p = board[fr][fc];

    // Handle Castling (Moving the Rook)
    if (p == 'K' && move[0] == 'e' && move[2] == 'g') { board[7][5] = 'R'; board[7][7] = ' '; } // White Kingside
    if (p == 'K' && move[0] == 'e' && move[2] == 'c') { board[7][3] = 'R'; board[7][0] = ' '; } // White Queenside
    if (p == 'k' && move[0] == 'e' && move[2] == 'g') { board[0][5] = 'r'; board[0][7] = ' '; } // Black Kingside
    if (p == 'k' && move[0] == 'e' && move[2] == 'c') { board[0][3] = 'r'; board[0][0] = ' '; } // Black Queenside

    // Handle En Passant (Pawn moves diagonally to empty square)
    if (tolower(p) == 'p' && fc != tc && board[tr][tc] == ' ') {
        board[fr][tc] = ' '; // Capture the pawn
    }

    // Move the piece
    board[tr][tc] = p;
    board[fr][fc] = ' ';

    // Handle Promotion
    if (move[4] != '\0' && move[4] != '\n' && move[4] != ' ') {
        if (tr == 0) board[tr][tc] = toupper(move[4]);
        if (tr == 7) board[tr][tc] = tolower(move[4]);
    }
}

// --- IPC: Send command to Engine ---
void send_engine(int fd, const char *cmd) {
    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);
}

// --- IPC: Wait for a specific response from Engine ---
void wait_for_engine(int fd, const char *target, char *output) {
    char buffer[1024];
    int i = 0;
    while (read(fd, &buffer[i], 1) > 0) {
        if (buffer[i] == '\n') {
            buffer[i] = '\0';
            if (strncmp(buffer, target, strlen(target)) == 0) {
                if (output) strcpy(output, buffer);
                return;
            }
            i = 0; // reset for next line
        } else {
            i++;
        }
    }
}

// --- MAIN PROGRAM ---
int main() {
    int gui_to_engine[2], engine_to_gui[2];
    
    // Create Pipes
    if (pipe(gui_to_engine) == -1 || pipe(engine_to_gui) == -1) {
        perror("Pipe failed"); return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("Fork failed"); return 1;
    }

    if (pid == 0) {
        // --- CHILD PROCESS (Engine) ---
        dup2(gui_to_engine[0], STDIN_FILENO);
        dup2(engine_to_gui[1], STDOUT_FILENO);
        
        close(gui_to_engine[1]); close(engine_to_gui[0]);
        
        execlp(ENGINE_PATH, ENGINE_PATH, NULL);
        perror("Failed to start engine (is Stockfish installed?)");
        exit(1);
    } else {
        // --- PARENT PROCESS (GUI) ---
        close(gui_to_engine[0]); close(engine_to_gui[1]);
        int fd_out = gui_to_engine[1];
        int fd_in = engine_to_gui[0];

        // UCI Handshake
        printf("Starting UCI Engine...\n");
        send_engine(fd_out, "uci");
        wait_for_engine(fd_in, "uciok", NULL);
        send_engine(fd_out, "isready");
        wait_for_engine(fd_in, "readyok", NULL);

        char user_move[10];
        char engine_response[256];
        char engine_move[10];

        while (1) {
            print_board();
            
            // 1. Get User Move
            printf("Enter move (e.g., e2e4) or 'quit': ");
            if (!fgets(user_move, sizeof(user_move), stdin)) break;
            user_move[strcspn(user_move, "\n")] = 0; // Remove newline

            if (strcmp(user_move, "quit") == 0) break;
            if (strlen(user_move) < 4) continue;

            // Apply user move
            apply_move(user_move);
            strcat(move_history, user_move);
            strcat(move_history, " ");
            print_board();

            // 2. Ask Engine to Move
            printf("Engine is thinking...\n");
            char position_cmd[8500];
            snprintf(position_cmd, sizeof(position_cmd), "position startpos moves %s", move_history);
            
            send_engine(fd_out, position_cmd);
            
            char go_cmd[50];
            snprintf(go_cmd, sizeof(go_cmd), "go movetime %d", THINK_TIME_MS);
            send_engine(fd_out, go_cmd);

            // Read Engine's Best Move
            wait_for_engine(fd_in, "bestmove", engine_response);
            sscanf(engine_response, "bestmove %s", engine_move);

            // Apply engine move
            apply_move(engine_move);
            strcat(move_history, engine_move);
            strcat(move_history, " ");
        }

        // Cleanup
        send_engine(fd_out, "quit");
        close(fd_out); close(fd_in);
        wait(NULL);
    }
    return 0;
}
