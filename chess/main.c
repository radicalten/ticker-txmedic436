#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

// Piece representations
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define WHITE 8
#define BLACK 16
#define COLOR_MASK 24
#define PIECE_MASK 7

// Struct for move representation
typedef struct {
    int from;
    int to;
    int promotion;
} Move;

// Struct to track list of moves
typedef struct {
    Move moves[256];
    int count;
} MoveList;

// Game state struct
typedef struct {
    int board[64];
    int active_color;
    int castling_rights; // 1: WK, 2: WQ, 4: BK, 8: BQ
    int ep_square;       // ep target square index (0-63), -1 if none
    int halfmove_clock;
    int fullmove_number;
    Move last_move;
    char san[16];        // Standard Algebraic Notation for this move
} GameState;

// Time controls configurations
typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCMode;
typedef struct {
    TCMode mode;
    int value; // Depth in plies, nodes in count, or time in ms
} TimeControl;

// Game histories for Undo
#define MAX_HISTORY 2048
GameState state_history[MAX_HISTORY];
int history_index = 0;

// Terminal and Engine Global States
struct termios orig_termios;
int engine_write_fd = -1;
int engine_read_fd = -1;
pid_t engine_pid = -1;
int engine_active = 0;
int human_color = WHITE;
int engine_thinking = 0;
char engine_io_buffer[16384] = {0};
int io_len = 0;

// Forward declarations of Chess Engine Logic
void generate_legal_moves(const GameState *state, MoveList *list);
int is_square_attacked(const int *board, int sq, int attacker_color);
void apply_move(GameState *state, Move move);
void get_san(const GameState *state, Move move, char *san);

// raw mode logic for responsive key presses
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\n"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;  // Non-blocking read
    raw.c_cc[VTIME] = 1; // 100ms read timeout
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

// UCI Subprocess Launcher
int start_engine(const char *path) {
    int gui_to_eng[2];
    int eng_to_gui[2];

    if (pipe(gui_to_eng) < 0 || pipe(eng_to_gui) < 0) return 0;

    engine_pid = fork();
    if (engine_pid < 0) return 0;

    if (engine_pid == 0) { // Child Process
        dup2(gui_to_eng[0], STDIN_FILENO);
        dup2(eng_to_gui[1], STDOUT_FILENO);
        close(gui_to_eng[0]); close(gui_to_eng[1]);
        close(eng_to_gui[0]); close(eng_to_gui[1]);

        char *argv[] = { (char *)path, NULL };
        execvp(path, argv);
        exit(1); // Exit if exec failed
    }

    // Parent Process
    close(gui_to_eng[0]);
    close(eng_to_gui[1]);
    engine_write_fd = gui_to_eng[1];
    engine_read_fd = eng_to_gui[0];

    // Set engine outputs read stream to non-blocking mode
    int flags = fcntl(engine_read_fd, F_GETFL, 0);
    fcntl(engine_read_fd, F_SETFL, flags | O_NONBLOCK);

    // Standard UCI Handshaking
    write(engine_write_fd, "uci\n", 4);
    char buf[2048];
    int found_uciok = 0;
    for (int i = 0; i < 30; i++) {
        usleep(100000);
        int n = read(engine_read_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "uciok")) { found_uciok = 1; break; }
        }
    }
    if (!found_uciok) return 0;

    write(engine_write_fd, "isready\n", 8);
    int found_readyok = 0;
    for (int i = 0; i < 30; i++) {
        usleep(100000);
        int n = read(engine_read_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "readyok")) { found_readyok = 1; break; }
        }
    }
    return found_readyok;
}

void stop_engine() {
    if (engine_pid > 0) {
        write(engine_write_fd, "quit\n", 5);
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, 0);
    }
}

// Convert algebraic coordinate format "e2e4" to Move format
int parse_uci_move(const char *str, const GameState *state, Move *move) {
    if (strlen(str) < 4) return 0;
    int f_from = str[0] - 'a';
    int r_from = 8 - (str[1] - '0');
    int f_to = str[2] - 'a';
    int r_to = 8 - (str[3] - '0');

    if (f_from < 0 || f_from >= 8 || r_from < 0 || r_from >= 8 ||
        f_to < 0 || f_to >= 8 || r_to < 0 || r_to >= 8) return 0;

    move->from = r_from * 8 + f_from;
    move->to = r_to * 8 + f_to;
    move->promotion = EMPTY;

    if (strlen(str) >= 5) {
        char p = str[4];
        if (p == 'q') move->promotion = QUEEN;
        else if (p == 'r') move->promotion = ROOK;
        else if (p == 'b') move->promotion = BISHOP;
        else if (p == 'n') move->promotion = KNIGHT;
    }
    return 1;
}

// Convert inner Move struct back into "e2e4" style coordinates
void get_move_coord_str(Move m, char *buf) {
    int f_from = m.from % 8;
    int r_from = 8 - (m.from / 8);
    int f_to = m.to % 8;
    int r_to = 8 - (m.to / 8);
    if (m.promotion != EMPTY) {
        char p_chars[] = "  nbrqk";
        sprintf(buf, "%c%d%c%d%c", 'a' + f_from, r_from, 'a' + f_to, r_to, p_chars[m.promotion]);
    } else {
        sprintf(buf, "%c%d%c%d", 'a' + f_from, r_from, 'a' + f_to, r_to);
    }
}

// Board Initialization
void init_board(GameState *state) {
    memset(state, 0, sizeof(GameState));
    int back_rank[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
    for (int f = 0; f < 8; f++) {
        state->board[0 * 8 + f] = BLACK | back_rank[f];
        state->board[1 * 8 + f] = BLACK | PAWN;
        for (int r = 2; r < 6; r++) state->board[r * 8 + f] = EMPTY;
        state->board[6 * 8 + f] = WHITE | PAWN;
        state->board[7 * 8 + f] = WHITE | back_rank[f];
    }
    state->active_color = WHITE;
    state->castling_rights = 15; // WK | WQ | BK | BQ
    state->ep_square = -1;
    state->fullmove_number = 1;
    state->from = -1; state->to = -1;
    state->san[0] = '\0';
}

// Get the Solid Unicode piece string
const char *get_piece_symbol(int piece) {
    switch (piece & PIECE_MASK) {
        case PAWN:   return "♟";
        case KNIGHT: return "♞";
        case BISHOP: return "♝";
        case ROOK:   return "♜";
        case QUEEN:  return "♛";
        case KING:   return "♚";
        default:     return " ";
    }
}

// In-place UI Drawing
void draw_interface(const GameState *state, int cursor_sq, int selected_sq, const TimeControl *tc) {
    printf("\033[H"); // Reset cursor back to top-left of terminal

    printf("\033[1;33m  === TERMINAL CHESS SYSTEM ===\033[0m\n\n");

    MoveList legal_moves;
    if (selected_sq != -1) {
        generate_legal_moves(state, &legal_moves);
    } else {
        legal_moves.count = 0;
    }

    int white_king_check = 0, black_king_check = 0;
    int w_king_sq = -1, b_king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == (WHITE | KING)) w_king_sq = i;
        if (state->board[i] == (BLACK | KING)) b_king_sq = i;
    }
    if (w_king_sq != -1 && is_square_attacked(state->board, w_king_sq, BLACK)) white_king_check = 1;
    if (b_king_sq != -1 && is_square_attacked(state->board, b_king_sq, WHITE)) black_king_check = 1;

    for (int r = 0; r < 8; r++) {
        printf("  %d ", 8 - r);
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            int p = state->board[sq];

            int bg_color = 0;
            if ((white_king_check && sq == w_king_sq) || (black_king_check && sq == b_king_sq)) {
                bg_color = 196; // Bright Red for Check
            } else if (sq == cursor_sq) {
                bg_color = 214; // Bright Golden Orange for Cursor
            } else if (sq == selected_sq) {
                bg_color = 226; // Bright Yellow for Selection
            } else {
                int is_dest = 0;
                for (int i = 0; i < legal_moves.count; i++) {
                    if (legal_moves.moves[i].from == selected_sq && legal_moves.moves[i].to == sq) {
                        is_dest = 1; break;
                    }
                }
                if (is_dest) {
                    bg_color = 113; // Soft Green for legal moves
                } else if (sq == state->from || sq == state->to) {
                    bg_color = 75;  // Pastel Blue for last played move
                } else {
                    bg_color = ((r + f) % 2 == 0) ? 223 : 137; // Sand beige & Chocolate brown board tiles
                }
            }

            int fg_color = ((p & COLOR_MASK) == WHITE) ? 231 : 16; // Polar White vs deep black pieces
            if (p != EMPTY && (p & COLOR_MASK) == WHITE) {
                printf("\033[48;5;%dm\033[38;5;%dm\033[1m  %s  \033[0m", bg_color, fg_color, get_piece_symbol(p));
            } else {
                printf("\033[48;5;%dm\033[38;5;%dm  %s  \033[0m", bg_color, fg_color, get_piece_symbol(p));
            }
        }

        // Side display panels
        if (r == 0) {
            printf("    COLOR TURN: %s", (state->active_color == WHITE) ? "\033[1;37mWHITE\033[0m" : "\033[1;30mBLACK\033[0m");
        } else if (r == 1) {
            printf("    ENGINE: %s", engine_thinking ? "\033[1;31mThinking...\033[0m" : "\033[1;32mIdle\033[0m");
        } else if (r == 2) {
            const char *mode_str = (tc->mode == TC_DEPTH) ? "Depth" : (tc->mode == TC_NODES) ? "Nodes" : "Time Control";
            if (tc->mode == TC_DEPTH) printf("    TC: \033[1;36m%s\033[0m (%d plies)", mode_str, tc->value);
            else if (tc->mode == TC_NODES) printf("    TC: \033[1;36m%s\033[0m (%d nodes)", mode_str, tc->value);
            else printf("    TC: \033[1;36m%s\033[0m (%d ms)", mode_str, tc->value);
        } else if (r == 3) {
            printf("    CONTROLS: \033[1mArrows/WASD\033[0m Navigate");
        } else if (r == 4) {
            printf("              \033[1mSpace/Enter\033[0m Select & Move");
        } else if (r == 5) {
            printf("              \033[1mU/Backspace\033[0m Undo Move");
        } else if (r == 6) {
            printf("              \033[1mT\033[0m Change Time Controls");
        } else if (r == 7) {
            printf("              \033[1mQ/Esc\033[0m Exit Program");
        }
        printf("\n");
    }

    printf("     ");
    for (int f = 0; f < 8; f++) printf("  %c  ", 'A' + f);
    printf("\n\n");

    // Check status announcements
    MoveList test_ml;
    generate_legal_moves(state, &test_ml);
    if (test_ml.count == 0) {
        if (white_king_check) printf("  \033[1;31m*** CHECKMATE! BLACK WINS! ***\033[0m\n");
        else if (black_king_check) printf("  \033[1;32m*** CHECKMATE! WHITE WINS! ***\033[0m\n");
        else printf("  \033[1;33m*** STALEMATE! DRAW! ***\033[0m\n");
    }

    printf("  ---------------------------------------------------------\n");
    printf("  MOST RECENT MOVES:\n  ");
    int move_num = 1;
    for (int i = 1; i <= history_index; i++) {
        if (i % 2 != 0) {
            printf("%d. %s ", move_num, state_history[i].san);
            move_num++;
        } else {
            printf("%s  ", state_history[i].san);
        }
        if (i % 16 == 0) printf("\n  ");
    }
    printf("\n");
}

// Promotional Piece Interactive Selection
int prompt_promotion() {
    disable_raw_mode();
    printf("\n\033[1;33mPawn Promotion! Pick Choice: (Q)ueen, (R)ook, (B)ishop, K(N)ight > \033[0m");
    fflush(stdout);
    char choice[100];
    int promo = QUEEN;
    if (fgets(choice, sizeof(choice), stdin)) {
        char c = choice[0];
        if (c == 'r' || c == 'R') promo = ROOK;
        else if (c == 'b' || c == 'B') promo = BISHOP;
        else if (c == 'n' || c == 'N') promo = KNIGHT;
    }
    enable_raw_mode();
    printf("\033[2J"); // Clear standard terminal
    return promo;
}

// Time Control Settings Modifiers
void prompt_change_tc(TimeControl *tc) {
    disable_raw_mode();
    printf("\n\n\033[1;33m=== TIME CONTROL SELECTION ===\033[0m\n");
    printf("1. Set Depth (plies count, e.g. 10)\n");
    printf("2. Set Nodes (compute node limits, e.g. 500000)\n");
    printf("3. Set Time limit (thinking limit in milliseconds, e.g. 2000)\n");
    printf("Pick setting (1-3) > ");
    fflush(stdout);

    char mode_choice[100];
    if (fgets(mode_choice, sizeof(mode_choice), stdin)) {
        int m = atoi(mode_choice);
        if (m >= 1 && m <= 3) {
            if (m == 1) tc->mode = TC_DEPTH;
            else if (m == 2) tc->mode = TC_NODES;
            else tc->mode = TC_TIME;

            printf("Enter limit value > ");
            fflush(stdout);
            char val_choice[100];
            if (fgets(val_choice, sizeof(val_choice), stdin)) {
                int val = atoi(val_choice);
                if (val > 0) {
                    tc->value = val;
                    printf("Time controls updated!\n");
                } else {
                    printf("Error. Setting unmodified.\n");
                }
            }
        }
    }
    sleep(1);
    enable_raw_mode();
    printf("\033[2J");
}

// Global Move Generation & Legality Validation
int is_square_attacked(const int *board, int sq, int attacker_color) {
    int r = sq / 8, f = sq % 8;

    // Knight attacks
    int knight_offsets[8][2] = { {-2,-1}, {-2,1}, {-1,-2}, {-1,2}, {1,-2}, {1,2}, {2,-1}, {2,1} };
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_offsets[i][0], nf = f + knight_offsets[i][1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int p = board[nr * 8 + nf];
            if ((p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == KNIGHT) return 1;
        }
    }

    // King attacks
    int king_offsets[8][2] = { {-1,-1}, {-1,0}, {-1,1}, {0,-1}, {0,1}, {1,-1}, {1,0}, {1,1} };
    for (int i = 0; i < 8; i++) {
        int nr = r + king_offsets[i][0], nf = f + king_offsets[i][1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int p = board[nr * 8 + nf];
            if ((p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == KING) return 1;
        }
    }

    // Pawn attacks
    if (attacker_color == WHITE) {
        int pr = r + 1;
        for (int df = -1; df <= 1; df += 2) {
            int pf = f + df;
            if (pr >= 0 && pr < 8 && pf >= 0 && pf < 8) {
                if (board[pr * 8 + pf] == (WHITE | PAWN)) return 1;
            }
        }
    } else {
        int pr = r - 1;
        for (int df = -1; df <= 1; df += 2) {
            int pf = f + df;
            if (pr >= 0 && pr < 8 && pf >= 0 && pf < 8) {
                if (board[pr * 8 + pf] == (BLACK | PAWN)) return 1;
            }
        }
    }

    // Diagonal slider attacks (Bishops & Queens)
    int diag_dirs[4][2] = { {-1,-1}, {-1,1}, {1,-1}, {1,1} };
    for (int d = 0; d < 4; d++) {
        int nr = r, nf = f;
        while (1) {
            nr += diag_dirs[d][0]; nf += diag_dirs[d][1];
            if (nr < 0 || nr >= 8 || nf < 0 || nf >= 8) break;
            int p = board[nr * 8 + nf];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color && ((p & PIECE_MASK) == BISHOP || (p & PIECE_MASK) == QUEEN)) return 1;
                break;
            }
        }
    }

    // Orthogonal slider attacks (Rooks & Queens)
    int card_dirs[4][2] = { {-1,0}, {1,0}, {0,-1}, {0,1} };
    for (int d = 0; d < 4; d++) {
        int nr = r, nf = f;
        while (1) {
            nr += card_dirs[d][0]; nf += card_dirs[d][1];
            if (nr < 0 || nr >= 8 || nf < 0 || nf >= 8) break;
            int p = board[nr * 8 + nf];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color && ((p & PIECE_MASK) == ROOK || (p & PIECE_MASK) == QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

void add_move(MoveList *list, int from, int to, int promo) {
    list->moves[list->count].from = from;
    list->moves[list->count].to = to;
    list->moves[list->count].promotion = promo;
    list->count++;
}

void add_pawn_move(MoveList *list, int from, int to, int color) {
    int r = to / 8;
    if ((color == WHITE && r == 0) || (color == BLACK && r == 7)) {
        add_move(list, from, to, QUEEN); add_move(list, from, to, ROOK);
        add_move(list, from, to, BISHOP); add_move(list, from, to, KNIGHT);
    } else {
        add_move(list, from, to, EMPTY);
    }
}

void generate_pseudo_moves(const GameState *state, MoveList *list) {
    int color = state->active_color;
    int opp_color = (color == WHITE) ? BLACK : WHITE;

    for (int sq = 0; sq < 64; sq++) {
        int p = state->board[sq];
        if ((p & COLOR_MASK) != color) continue;

        int piece = p & PIECE_MASK;
        int r = sq / 8, f = sq % 8;

        if (piece == PAWN) {
            int dir = (color == WHITE) ? -1 : 1;
            int to = sq + dir * 8;
            if (to >= 0 && to < 64 && state->board[to] == EMPTY) {
                add_pawn_move(list, sq, to, color);
                int start_rank = (color == WHITE) ? 6 : 1;
                int double_to = sq + dir * 16;
                if (r == start_rank && state->board[double_to] == EMPTY) {
                    add_move(list, sq, double_to, EMPTY);
                }
            }
            int files[2] = { f - 1, f + 1 };
            for (int i = 0; i < 2; i++) {
                int target_f = files[i];
                if (target_f >= 0 && target_f < 8) {
                    int target_sq = (r + dir) * 8 + target_f;
                    if (target_sq >= 0 && target_sq < 64) {
                        int dest_p = state->board[target_sq];
                        if (dest_p != EMPTY && (dest_p & COLOR_MASK) == opp_color) {
                            add_pawn_move(list, sq, target_sq, color);
                        }
                        if (target_sq == state->ep_square) {
                            add_move(list, sq, target_sq, EMPTY);
                        }
                    }
                }
            }
        } else if (piece == KNIGHT) {
            int offsets[8][2] = { {-2,-1}, {-2,1}, {-1,-2}, {-1,2}, {1,-2}, {1,2}, {2,-1}, {2,1} };
            for (int i = 0; i < 8; i++) {
                int nr = r + offsets[i][0], nf = f + offsets[i][1];
                if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    int target_sq = nr * 8 + nf;
                    int dest_p = state->board[target_sq];
                    if (dest_p == EMPTY || (dest_p & COLOR_MASK) == opp_color) {
                        add_move(list, sq, target_sq, EMPTY);
                    }
                }
            }
        } else if (piece == KING) {
            int offsets[8][2] = { {-1,-1}, {-1,0}, {-1,1}, {0,-1}, {0,1}, {1,-1}, {1,0}, {1,1} };
            for (int i = 0; i < 8; i++) {
                int nr = r + offsets[i][0], nf = f + offsets[i][1];
                if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    int target_sq = nr * 8 + nf;
                    int dest_p = state->board[target_sq];
                    if (dest_p == EMPTY || (dest_p & COLOR_MASK) == opp_color) {
                        add_move(list, sq, target_sq, EMPTY);
                    }
                }
            }
            // Castling checks
            if (color == WHITE) {
                if (sq == 60) {
                    if ((state->castling_rights & 1) && state->board[61] == EMPTY && state->board[62] == EMPTY) {
                        if (!is_square_attacked(state->board, 60, BLACK) && !is_square_attacked(state->board, 61, BLACK) && !is_square_attacked(state->board, 62, BLACK)) {
                            add_move(list, 60, 62, EMPTY);
                        }
                    }
                    if ((state->castling_rights & 2) && state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) {
                        if (!is_square_attacked(state->board, 60, BLACK) && !is_square_attacked(state->board, 59, BLACK) && !is_square_attacked(state->board, 58, BLACK)) {
                            add_move(list, 60, 58, EMPTY);
                        }
                    }
                }
            } else {
                if (sq == 4) {
                    if ((state->castling_rights & 4) && state->board[5] == EMPTY && state->board[6] == EMPTY) {
                        if (!is_square_attacked(state->board, 4, WHITE) && !is_square_attacked(state->board, 5, WHITE) && !is_square_attacked(state->board, 6, WHITE)) {
                            add_move(list, 4, 6, EMPTY);
                        }
                    }
                    if ((state->castling_rights & 8) && state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) {
                        if (!is_square_attacked(state->board, 4, WHITE) && !is_square_attacked(state->board, 3, WHITE) && !is_square_attacked(state->board, 2, WHITE)) {
                            add_move(list, 4, 2, EMPTY);
                        }
                    }
                }
            }
        } else if (piece == BISHOP || piece == ROOK || piece == QUEEN) {
            int dirs[8][2] = { {-1,-1}, {-1,1}, {1,-1}, {1,1}, {-1,0}, {1,0}, {0,-1}, {0,1} };
            int start = (piece == ROOK) ? 4 : 0;
            int end = (piece == BISHOP) ? 4 : 8;
            for (int d = start; d < end; d++) {
                int nr = r, nf = f;
                while (1) {
                    nr += dirs[d][0]; nf += dirs[d][1];
                    if (nr < 0 || nr >= 8 || nf < 0 || nf >= 8) break;
                    int target_sq = nr * 8 + nf;
                    int dest_p = state->board[target_sq];
                    if (dest_p == EMPTY) {
                        add_move(list, sq, target_sq, EMPTY);
                    } else {
                        if ((dest_p & COLOR_MASK) == opp_color) add_move(list, sq, target_sq, EMPTY);
                        break;
                    }
                }
            }
        }
    }
}

int is_move_legal(const GameState *state, Move move) {
    GameState temp;
    memcpy(&temp, state, sizeof(GameState));
    apply_move(&temp, move);
    int king_sq = -1;
    int king_to_find = state->active_color | KING;
    for (int i = 0; i < 64; i++) {
        if (temp.board[i] == king_to_find) { king_sq = i; break; }
    }
    if (king_sq == -1) return 0;
    // Ensure the player who made the move didn't leave or place their King in check
    if (is_square_attacked(temp.board, king_sq, temp.active_color)) return 0;
    return 1;
}

void generate_legal_moves(const GameState *state, MoveList *list) {
    list->count = 0;
    MoveList pseudo;
    pseudo.count = 0;
    generate_pseudo_moves(state, &pseudo);
    for (int i = 0; i < pseudo.count; i++) {
        if (is_move_legal(state, pseudo.moves[i])) {
            list->moves[list->count++] = pseudo.moves[i];
        }
    }
}

void apply_move(GameState *state, Move move) {
    int from = move.from, to = move.to;
    int p = state->board[from];
    int piece = p & PIECE_MASK, color = p & COLOR_MASK;

    state->board[from] = EMPTY;

    // En Passant captures handling
    if (piece == PAWN && to == state->ep_square) {
        state->board[(color == WHITE) ? (to + 8) : (to - 8)] = EMPTY;
    }

    // Set En Passant targets
    int next_ep = -1;
    if (piece == PAWN && abs(from - to) == 16) {
        next_ep = (from + to) / 2;
    }

    // King Castling moves adjustments
    if (piece == KING) {
        if (from == 60 && to == 62) { state->board[61] = WHITE | ROOK; state->board[63] = EMPTY; }
        else if (from == 60 && to == 58) { state->board[59] = WHITE | ROOK; state->board[56] = EMPTY; }
        else if (from == 4 && to == 6) { state->board[5] = BLACK | ROOK; state->board[7] = EMPTY; }
        else if (from == 4 && to == 2) { state->board[3] = BLACK | ROOK; state->board[0] = EMPTY; }
    }

    // Move implementation & Promotions
    if (move.promotion != EMPTY) state->board[to] = color | move.promotion;
    else state->board[to] = p;

    // Castling Rights management
    if (piece == KING) {
        state->castling_rights &= (color == WHITE) ? ~3 : ~12;
    }
    if (from == 56 || to == 56) state->castling_rights &= ~2;
    if (from == 63 || to == 63) state->castling_rights &= ~1;
    if (from == 0 || to == 0) state->castling_rights &= ~8;
    if (from == 7 || to == 7) state->castling_rights &= ~4;

    state->ep_square = next_ep;
    state->active_color = (color == WHITE) ? BLACK : WHITE;
    if (color == BLACK) state->fullmove_number++;
    state->from = from; state->to = to;
}

// Convert moves into Standard Algebraic Notation (SAN)
void get_san(const GameState *state, Move move, char *san) {
    int from = move.from, to = move.to;
    int p = state->board[from];
    int piece = p & PIECE_MASK, color = p & COLOR_MASK;

    if (piece == KING) {
        if (from == 60 && to == 62) { strcpy(san, "O-O"); return; }
        if (from == 60 && to == 58) { strcpy(san, "O-O-O"); return; }
        if (from == 4 && to == 6) { strcpy(san, "O-O"); return; }
        if (from == 4 && to == 2) { strcpy(san, "O-O-O"); return; }
    }

    int len = 0;
    if (piece == PAWN) {
        if (abs(from - to) % 8 != 0) {
            san[len++] = 'a' + (from % 8);
            san[len++] = 'x';
        }
    } else {
        char p_chars[] = "  NBRQK";
        san[len++] = p_chars[piece];

        MoveList ml;
        generate_legal_moves(state, &ml);
        int same_type_can_move = 0, same_file = 0, same_rank = 0;
        for (int i = 0; i < ml.count; i++) {
            Move m = ml.moves[i];
            if (m.from != from && m.to == to && state->board[m.from] == p) {
                same_type_can_move = 1;
                if (m.from % 8 == from % 8) same_file = 1;
                if (m.from / 8 == from / 8) same_rank = 1;
            }
        }
        if (same_type_can_move) {
            if (!same_file) san[len++] = 'a' + (from % 8);
            else if (!same_rank) san[len++] = '8' - (from / 8);
            else {
                san[len++] = 'a' + (from % 8);
                san[len++] = '8' - (from / 8);
            }
        }
        if (state->board[to] != EMPTY) san[len++] = 'x';
    }

    san[len++] = 'a' + (to % 8);
    san[len++] = '8' - (to / 8);

    if (move.promotion != EMPTY) {
        san[len++] = '=';
        char p_chars[] = "  NBRQK";
        san[len++] = p_chars[move.promotion];
    }

    GameState temp;
    memcpy(&temp, state, sizeof(GameState));
    apply_move(&temp, move);
    int opp_color = (color == WHITE) ? BLACK : WHITE;
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (temp.board[i] == (opp_color | KING)) { king_sq = i; break; }
    }
    if (king_sq != -1 && is_square_attacked(temp.board, king_sq, color)) {
        MoveList opp_ml;
        generate_legal_moves(&temp, &opp_ml);
        san[len++] = (opp_ml.count == 0) ? '#' : '+';
    }
    san[len] = '\0';
}

int main() {
    // Initial Menu selection interface
    printf("=== MAC TERMINAL CHESS GUI ===\n");
    printf("Please specify complete path to stockfish/UCI Chess Engine,\n");
    printf("or leave empty/type 'none' to run Pass-and-Play (2 Player Local) mode:\n");
    printf("Engine Path > ");
    fflush(stdout);

    char path[1024];
    if (fgets(path, sizeof(path), stdin)) {
        path[strcspn(path, "\r\n")] = '\0';
        if (strlen(path) > 0 && strcmp(path, "none") != 0) {
            if (start_engine(path)) {
                printf("Successfully interfaced with the engine!\n");
                sleep(1);
                printf("Choose color: 1. White, 2. Black > ");
                fflush(stdout);
                char choice[100];
                if (fgets(choice, sizeof(choice), stdin)) {
                    if (choice[0] == '2') human_color = BLACK;
                }
            } else {
                printf("Initialization failed. Falling back to Pass-and-Play.\n");
                engine_active = 0;
                sleep(2);
            }
        }
    }

    init_board(&state_history[0]);
    enable_raw_mode();
    printf("\033[2J"); // Complete initial terminal clear

    int cursor_sq = 44; // Target square: E3
    int selected_sq = -1;
    TimeControl tc = { TC_TIME, 2000 }; // Default: 2000ms response delay time

    // Principal update event loops
    while (1) {
        GameState *curr = &state_history[history_index];

        // Engine automation calls
        if (engine_active && curr->active_color != human_color && !engine_thinking) {
            engine_thinking = 1;
            char pos_cmd[8192] = "position startpos moves";
            for (int i = 1; i <= history_index; i++) {
                char m_str[10];
                get_move_coord_str(state_history[i].last_move, m_str);
                strcat(pos_cmd, " ");
                strcat(pos_cmd, m_str);
            }
            strcat(pos_cmd, "\n");
            write(engine_write_fd, pos_cmd, strlen(pos_cmd));

            char go_cmd[100];
            if (tc.mode == TC_DEPTH) sprintf(go_cmd, "go depth %d\n", tc.value);
            else if (tc.mode == TC_NODES) sprintf(go_cmd, "go nodes %d\n", tc.value);
            else sprintf(go_cmd, "go movetime %d\n", tc.value);
            write(engine_write_fd, go_cmd, strlen(go_cmd));
        }

        draw_interface(curr, cursor_sq, selected_sq, &tc);

        // Parse Engine Output streams
        if (engine_thinking) {
            char chunk[4096];
            int n = read(engine_read_fd, chunk, sizeof(chunk) - 1);
            if (n > 0) {
                if (io_len + n < (int)sizeof(engine_io_buffer) - 1) {
                    memcpy(engine_io_buffer + io_len, chunk, n);
                    io_len += n;
                    engine_io_buffer[io_len] = '\0';
                }
                char *line = engine_io_buffer;
                char *next_line;
                while ((next_line = strchr(line, '\n')) != NULL) {
                    *next_line = '\0';
                    if (strncmp(line, "bestmove", 8) == 0) {
                        char best[32] = {0};
                        sscanf(line, "bestmove %s", best);
                        Move eng_move;
                        if (parse_uci_move(best, curr, &eng_move)) {
                            GameState *next_state = &state_history[history_index + 1];
                            memcpy(next_state, curr, sizeof(GameState));
                            get_san(curr, eng_move, next_state->san);
                            next_state->last_move = eng_move;
                            apply_move(next_state, eng_move);
                            history_index++;
                            engine_thinking = 0;
                            selected_sq = -1;
                        }
                    }
                    line = next_line + 1;
                }
                int consumed = line - engine_io_buffer;
                if (consumed > 0) {
                    memmove(engine_io_buffer, line, io_len - consumed + 1);
                    io_len -= consumed;
                }
            }
        }

        // Process Human inputs
        char ch;
        int bytes = read(STDIN_FILENO, &ch, 1);
        if (bytes > 0) {
            if (ch == 'q' || ch == 'Q' || ch == 27) { // Quit
                // Parse ESC sequence
                if (ch == 27) {
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                        int r = cursor_sq / 8, f = cursor_sq % 8;
                        if (seq[1] == 'A') r = (r - 1 + 8) % 8; // Arrow Up
                        else if (seq[1] == 'B') r = (r + 1) % 8; // Arrow Down
                        else if (seq[1] == 'C') f = (f + 1) % 8; // Arrow Right
                        else if (seq[1] == 'D') f = (f - 1 + 8) % 8; // Arrow Left
                        cursor_sq = r * 8 + f;
                    } else {
                        break; // Pure ESC key pressed, Quit
                    }
                } else {
                    break;
                }
            } else if (ch == 'w' || ch == 'W') {
                cursor_sq = ((cursor_sq / 8 - 1 + 8) % 8) * 8 + (cursor_sq % 8);
            } else if (ch == 's' || ch == 'S') {
                cursor_sq = ((cursor_sq / 8 + 1) % 8) * 8 + (cursor_sq % 8);
            } else if (ch == 'a' || ch == 'A') {
                cursor_sq = (cursor_sq / 8) * 8 + (cursor_sq % 8 - 1 + 8) % 8;
            } else if (ch == 'd' || ch == 'D') {
                cursor_sq = (cursor_sq / 8) * 8 + (cursor_sq % 8 + 1) % 8;
            } else if (ch == 't' || ch == 'T') {
                prompt_change_tc(&tc);
            } else if (ch == 127 || ch == 'u' || ch == 'U') { // Backspace or 'U' for undo
                if (engine_active) {
                    if (history_index >= 2) history_index -= 2;
                } else {
                    if (history_index >= 1) history_index--;
                }
                selected_sq = -1;
            } else if (ch == ' ' || ch == '\n') { // Select & Move action
                if (!engine_active || curr->active_color == human_color) {
                    if (selected_sq == -1) {
                        if (curr->board[cursor_sq] != EMPTY && (curr->board[cursor_sq] & COLOR_MASK) == curr->active_color) {
                            selected_sq = cursor_sq;
                        }
                    } else {
                        if (cursor_sq == selected_sq) {
                            selected_sq = -1;
                        } else {
                            MoveList ml;
                            generate_legal_moves(curr, &ml);
                            int is_legal = 0;
                            Move matched_move;
                            for (int i = 0; i < ml.count; i++) {
                                if (ml.moves[i].from == selected_sq && ml.moves[i].to == cursor_sq) {
                                    is_legal = 1; matched_move = ml.moves[i]; break;
                                }
                            }
                            if (is_legal) {
                                GameState *next_state = &state_history[history_index + 1];
                                memcpy(next_state, curr, sizeof(GameState));

                                if ((curr->board[matched_move.from] & PIECE_MASK) == PAWN &&
                                    ((matched_move.to / 8 == 0) || (matched_move.to / 8 == 7))) {
                                    matched_move.promotion = prompt_promotion();
                                }

                                get_san(curr, matched_move, next_state->san);
                                next_state->last_move = matched_move;
                                apply_move(next_state, matched_move);
                                history_index++;
                                selected_sq = -1;
                            } else {
                                if (curr->board[cursor_sq] != EMPTY && (curr->board[cursor_sq] & COLOR_MASK) == curr->active_color) {
                                    selected_sq = cursor_sq;
                                } else {
                                    selected_sq = -1;
                                }
                            }
                        }
                    }
                }
            }
        }
        usleep(15000); // 15ms throttling limits CPU overhead cleanly
    }

    stop_engine();
    return 0;
}
