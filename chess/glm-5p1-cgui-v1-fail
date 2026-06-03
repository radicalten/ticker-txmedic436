#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>

#define ENGINE_PATH "stockfish" // Assumes stockfish is in your PATH. Change to absolute path if needed.
#define MOVE_HISTORY_SIZE 4096
#define BUFFER_SIZE 1024

// Global board state
char board[8][8];
char move_history[MOVE_HISTORY_SIZE];

// Pipe file descriptors
int parent_to_child[2];
int child_to_parent[2];

FILE *engine_write;
FILE *engine_read;

void init_board() {
    char initial[8][8] = {
        {'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'},
        {'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'},
        {'.', '.', '.', '.', '.', '.', '.', '.'},
        {'.', '.', '.', '.', '.', '.', '.', '.'},
        {'.', '.', '.', '.', '.', '.', '.', '.'},
        {'.', '.', '.', '.', '.', '.', '.', '.'},
        {'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'},
        {'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'}
    };
    memcpy(board, initial, sizeof(board));
    move_history[0] = '\0';
}

void print_board() {
    printf("\n  ");
    for (char c = 'a'; c <= 'h'; c++) printf("  %c ", c);
    printf("\n  +---+---+---+---+---+---+---+---+\n");
    
    for (int r = 0; r < 8; r++) {
        printf("%d |", 8 - r);
        for (int c = 0; c < 8; c++) {
            char piece = board[r][c];
            // ANSI colors: White pieces are Bold Green, Black pieces are Bold Red, Empty is grey
            if (piece == '.') {
                printf("   |");
            } else if (isupper(piece)) {
                printf(" \033[1;32m%c\033[0m |", piece);
            } else {
                printf(" \033[1;31m%c\033[0m |", toupper(piece));
            }
        }
        printf(" %d\n  +---+---+---+---+---+---+---+---+\n", 8 - r);
    }
    printf("  ");
    for (char c = 'a'; c <= 'h'; c++) printf("  %c ", c);
    printf("\n\n");
}

void apply_move(const char *move) {
    int c1 = move[0] - 'a';
    int r1 = 8 - (move[1] - '0');
    int c2 = move[2] - 'a';
    int r2 = 8 - (move[3] - '0');

    char piece = board[r1][c1];
    char captured = board[r2][c2];

    // Handle Castling (move the rook too)
    if (toupper(piece) == 'K' && abs(c2 - c1) == 2) {
        if (c2 == 6) { // Kingside
            board[r1][5] = board[r1][7];
            board[r1][7] = '.';
        } else if (c2 == 2) { // Queenside
            board[r1][3] = board[r1][0];
            board[r1][0] = '.';
        }
    }

    // Handle Promotion
    if (strlen(move) == 5) {
        char promo = move[4];
        if (isupper(piece)) promo = toupper(promo);
        else promo = tolower(promo);
        board[r2][c2] = promo;
    } else {
        board[r2][c2] = piece;
    }

    board[r1][c1] = '.';

    // Update move history
    if (strlen(move_history) > 0) strcat(move_history, " ");
    strcat(move_history, move);
}

void send_uci(const char *cmd) {
    fprintf(engine_write, "%s\n", cmd);
    fflush(engine_write);
}

char* read_uci(const char *expected_prefix) {
    static char line[BUFFER_SIZE];
    while (fgets(line, sizeof(line), engine_read)) {
        // Remove trailing newline
        line[strcspn(line, "\n")] = 0;
        if (strncmp(line, expected_prefix, strlen(expected_prefix)) == 0) {
            return line;
        }
    }
    return NULL;
}

void spawn_engine() {
    if (pipe(parent_to_child) == -1 || pipe(child_to_parent) == -1) {
        perror("pipe");
        exit(1);
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) { // Child process (Engine)
        close(parent_to_child[1]); // Close write end of parent->child
        close(child_to_parent[0]); // Close read end of child->parent
        
        dup2(parent_to_child[0], STDIN_FILENO);
        dup2(child_to_parent[1], STDOUT_FILENO);
        
        close(parent_to_child[0]);
        close(child_to_parent[1]);
        
        execlp(ENGINE_PATH, ENGINE_PATH, NULL);
        
        // If execlp fails
        perror("Failed to launch engine. Is '" ENGINE_PATH "' in your PATH?");
        exit(1);
    }

    // Parent process (GUI)
    close(parent_to_child[0]); // Close read end of parent->child
    close(child_to_parent[1]); // Close write end of child->parent
    
    engine_write = fdopen(parent_to_child[1], "w");
    engine_read = fdopen(child_to_parent[0], "r");

    // UCI Handshake
    send_uci("uci");
    read_uci("uciok");
    send_uci("isready");
    read_uci("readyok");
}

void engine_play() {
    char cmd[BUFFER_SIZE];
    if (strlen(move_history) > 0) {
        snprintf(cmd, sizeof(cmd), "position startpos moves %s", move_history);
    } else {
        snprintf(cmd, sizeof(cmd), "position startpos");
    }
    send_uci(cmd);
    send_uci("isready");
    read_uci("readyok");
    
    // Ask engine to calculate (depth 15 is reasonably fast)
    send_uci("go depth 15");
    
    char *response = read_uci("bestmove");
    if (response) {
        char bestmove[6] = {0};
        sscanf(response, "bestmove %5s", bestmove);
        printf("Engine plays: %s\n", bestmove);
        apply_move(bestmove);
    } else {
        printf("Engine stopped responding.\n");
        exit(1);
    }
}

int main() {
    init_board();
    spawn_engine();
    
    printf("\n=== Terminal Chess GUI (UCI) ===\n");
    printf("Playing against: %s\n", ENGINE_PATH);
    printf("Enter moves in UCI format (e.g., e2e4, e7e8q for promotion).\n");
    printf("Type 'quit' to exit, 'new' to start a new game.\n\n");

    send_uci("ucinewgame");
    send_uci("isready");
    read_uci("readyok");

    char input[32];
    while (1) {
        print_board();
        
        if (strlen(move_history) % 2 == 0 || strlen(move_history) == 0) {
            printf("White's turn (You)\n");
        } else {
            printf("Black's turn (Engine)\n");
            engine_play();
            continue;
        }

        printf("> ");
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = 0; // Remove newline

        if (strcmp(input, "quit") == 0) {
            break;
        }
        
        if (strcmp(input, "new") == 0) {
            init_board();
            send_uci("ucinewgame");
            send_uci("isready");
            read_uci("readyok");
            printf("Starting new game.\n");
            continue;
        }

        // Basic input validation (must be at least 4 chars, e.g., e2e4)
        if (strlen(input) < 4 || 
            input[0] < 'a' || input[0] > 'h' ||
            input[1] < '1' || input[1] > '8' ||
            input[2] < 'a' || input[2] > 'h' ||
            input[3] < '1' || input[3] > '8') {
            printf("Invalid move format. Use UCI format like 'e2e4'.\n");
            continue;
        }

        apply_move(input);
    }

    // Cleanup
    send_uci("quit");
    fclose(engine_write);
    fclose(engine_read);
    wait(NULL); // Wait for child process to exit
    
    printf("Goodbye!\n");
    return 0;
}
