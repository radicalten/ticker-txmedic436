#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>

/* --- Global Variables for Engine IPC --- */
FILE *engine_in_fp = NULL;
FILE *engine_out_fp = NULL;
pid_t engine_pid = 0;

/* --- Chess Board State --- */
char board[8][8];
char move_history[4096] = "";

/* --- Function Prototypes --- */
void init_board();
void print_board();
void apply_move(const char *move);
void start_engine(const char *engine_path);
void send_to_engine(const char *cmd);
void read_engine_until(const char *target, char *bestmove_out);

/* --- Main Game Loop --- */
int main() {
    printf("Starting Chess GUI...\n");
    
    // Start Stockfish (Default path for brew installations on Mac)
    start_engine("stockfish");
    
    // Initialize UCI Protocol
    send_to_engine("uci\n");
    read_engine_until("uciok", NULL);
    send_to_engine("isready\n");
    read_engine_until("readyok", NULL);
    send_to_engine("ucinewgame\n");

    init_board();
    int is_player_turn = 1; // 1 = White (Human), 0 = Black (Engine)
    char input[32];

    while (1) {
        print_board();

        if (is_player_turn) {
            printf("\nYour move (e.g. e2e4) or 'quit': ");
            if (fgets(input, sizeof(input), stdin) == NULL) break;
            
            // Clean newline
            input[strcspn(input, "\r\n")] = 0;

            if (strcmp(input, "quit") == 0) break;

            // Very basic input validation
            if (strlen(input) < 4 || input[0] < 'a' || input[0] > 'h' || input[1] < '1' || input[1] > '8') {
                printf("Invalid format. Please use UCI format (e.g., e2e4 or e7e8q).\n");
                continue;
            }

            // Apply move visually and add to history
            apply_move(input);
            strcat(move_history, input);
            strcat(move_history, " ");
            is_player_turn = 0;
            
        } else {
            printf("\nEngine is thinking...\n");
            
            // Ask engine to calculate based on all moves played from the start position
            char pos_cmd[5000];
            snprintf(pos_cmd, sizeof(pos_cmd), "position startpos moves %s\n", move_history);
            send_to_engine(pos_cmd);
            
            // Tell engine to calculate (think for 1 second)
            send_to_engine("go movetime 1000\n");
            
            char bestmove[16] = "";
            read_engine_until("bestmove", bestmove);

            if (strlen(bestmove) > 0) {
                printf("Engine plays: %s\n", bestmove);
                apply_move(bestmove);
                strcat(move_history, bestmove);
                strcat(move_history, " ");
            } else {
                printf("Game Over or Engine Error.\n");
                break;
            }
            is_player_turn = 1;
        }
    }

    send_to_engine("quit\n");
    printf("Thanks for playing!\n");
    return 0;
}

/* --- Engine Communication Logic --- */
void start_engine(const char *engine_path) {
    int pipe_gui_to_engine[2];
    int pipe_engine_to_gui[2];

    if (pipe(pipe_gui_to_engine) < 0 || pipe(pipe_engine_to_gui) < 0) {
        perror("Pipe failed");
        exit(1);
    }

    engine_pid = fork();
    if (engine_pid < 0) {
        perror("Fork failed");
        exit(1);
    }

    if (engine_pid == 0) {
        // Child Process (Engine)
        dup2(pipe_gui_to_engine[0], STDIN_FILENO);
        dup2(pipe_engine_to_gui[1], STDOUT_FILENO);

        close(pipe_gui_to_engine[0]);
        close(pipe_gui_to_engine[1]);
        close(pipe_engine_to_gui[0]);
        close(pipe_engine_to_gui[1]);

        execlp(engine_path, engine_path, NULL);
        perror("Failed to start engine (is Stockfish installed?)");
        exit(1);
    } else {
        // Parent Process (GUI)
        close(pipe_gui_to_engine[0]);
        close(pipe_engine_to_gui[1]);

        engine_out_fp = fdopen(pipe_gui_to_engine[1], "w");
        engine_in_fp = fdopen(pipe_engine_to_gui[0], "r");
    }
}

void send_to_engine(const char *cmd) {
    if (engine_out_fp) {
        fprintf(engine_out_fp, "%s", cmd);
        fflush(engine_out_fp);
    }
}

void read_engine_until(const char *target, char *bestmove_out) {
    char line[2048];
    while (fgets(line, sizeof(line), engine_in_fp) != NULL) {
        // Optional: print engine output for debugging
        // printf("ENGINE: %s", line); 
        
        if (strncmp(line, target, strlen(target)) == 0) {
            if (strcmp(target, "bestmove") == 0 && bestmove_out != NULL) {
                // Extract the actual move, e.g., "bestmove e7e5 ponder d2d4"
                sscanf(line, "bestmove %s", bestmove_out);
            }
            break;
        }
    }
}

/* --- Board Logic & Rendering --- */
void init_board() {
    const char *initial = 
        "rnbqkbnr"
        "pppppppp"
        "        "
        "        "
        "        "
        "        "
        "PPPPPPPP"
        "RNBQKBNR";
    
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            board[r][c] = initial[r * 8 + c];
        }
    }
}

// Convert internal char to Unicode Chess Piece
const char* get_unicode_piece(char p) {
    switch (p) {
        case 'K': return "♔"; case 'Q': return "♕";
        case 'R': return "♖"; case 'B': return "♗";
        case 'N': return "♘"; case 'P': return "♙";
        case 'k': return "♚"; case 'q': return "♛";
        case 'r': return "♜"; case 'b': return "♝";
        case 'n': return "♞"; case 'p': return "♟";
        default:  return " ";
    }
}

void print_board() {
    printf("\n\n");
    for (int r = 0; r < 8; r++) {
        printf(" %d ", 8 - r); // Print Rank number
        for (int c = 0; c < 8; c++) {
            // Alternate colors for standard chessboard pattern
            int is_light_square = (r + c) % 2 == 0;
            
            // ANSI escape codes: 47=White bg, 100=Dark Grey bg, 30=Black fg
            if (is_light_square) {
                printf("\033[47;30m %s \033[0m", get_unicode_piece(board[r][c]));
            } else {
                printf("\033[100;30m %s \033[0m", get_unicode_piece(board[r][c]));
            }
        }
        printf("\n");
    }
    printf("    a  b  c  d  e  f  g  h\n");
}

void apply_move(const char *move) {
    // Parse standard coordinate format (e.g., e2e4)
    int c1 = move[0] - 'a';
    int r1 = 7 - (move[1] - '1'); // internal array: row 0 is Rank 8
    int c2 = move[2] - 'a';
    int r2 = 7 - (move[3] - '1');

    char piece = board[r1][c1];
    char dest_piece = board[r2][c2];

    // Handle Visual En Passant (Pawn moves diagonally to empty square)
    if (toupper(piece) == 'P' && c1 != c2 && dest_piece == ' ') {
        board[r1][c2] = ' '; // Capture the pawn adjacent to the original rank
    }

    // Move piece
    board[r2][c2] = piece;
    board[r1][c1] = ' ';

    // Handle Visual Promotion (e.g., e7e8q)
    if (strlen(move) >= 5) {
        char prom = move[4];
        if (piece == 'P') board[r2][c2] = toupper(prom);
        if (piece == 'p') board[r2][c2] = tolower(prom);
    }

    // Handle Visual Castling (King moves two squares)
    if (piece == 'K' && c1 == 4 && r1 == 7) {
        if (c2 == 6) { board[7][5] = 'R'; board[7][7] = ' '; } // Kingside
        if (c2 == 2) { board[7][3] = 'R'; board[7][0] = ' '; } // Queenside
    }
    if (piece == 'k' && c1 == 4 && r1 == 0) {
        if (c2 == 6) { board[0][5] = 'r'; board[0][7] = ' '; } // Kingside
        if (c2 == 2) { board[0][3] = 'r'; board[0][0] = ' '; } // Queenside
    }
}
