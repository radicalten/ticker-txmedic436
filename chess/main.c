#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <ctype.h>
#include <signal.h>

#define BOARD_SIZE 8
#define MAX_MOVES 1024
#define MAX_LEGAL_MOVES 256

// Piece definitions
#define EMPTY '.'
#define W_PAWN 'P'
#define W_KNIGHT 'N'
#define W_BISHOP 'B'
#define W_ROOK 'R'
#define W_QUEEN 'Q'
#define W_KING 'K'
#define B_PAWN 'p'
#define B_KNIGHT 'n'
#define B_BISHOP 'b'
#define B_ROOK 'r'
#define B_QUEEN 'q'
#define B_KING 'k'

#define WHITE 0
#define BLACK 1

typedef struct {
    int from_row, from_col;
    int to_row, to_col;
    char promotion;
    char moved_piece;
    char captured_piece;
    int is_castle;
    int is_en_passant;
    int prev_en_passant_col;
    int prev_white_king_moved;
    int prev_black_king_moved;
    int prev_white_rook_a_moved;
    int prev_white_rook_h_moved;
    int prev_black_rook_a_moved;
    int prev_black_rook_h_moved;
} Move;

typedef struct {
    char board[BOARD_SIZE][BOARD_SIZE];
    int turn;
    Move move_history[MAX_MOVES];
    int move_count;
    int en_passant_col;
    int white_king_moved;
    int black_king_moved;
    int white_rook_a_moved;
    int white_rook_h_moved;
    int black_rook_a_moved;
    int black_rook_h_moved;
} GameState;

typedef struct {
    int cursor_row;
    int cursor_col;
    int selected;
    int selected_row;
    int selected_col;
    Move legal_moves[MAX_LEGAL_MOVES];
    int legal_move_count;
    Move last_move;
    int has_last_move;
    int in_check;
    int check_row;
    int check_col;
} UIState;

typedef struct {
    FILE *to_engine;
    FILE *from_engine;
    pid_t pid;
    int active;
    char time_control_type[16];
    int time_control_value;
} EngineState;

static struct termios orig_termios;
GameState game;
UIState ui;
EngineState engine;

const char *get_piece_unicode(char piece) {
    switch(piece) {
        case 'P': return "♙";
        case 'N': return "♘";
        case 'B': return "♗";
        case 'R': return "♖";
        case 'Q': return "♕";
        case 'K': return "♔";
        case 'p': return "♟";
        case 'n': return "♞";
        case 'b': return "♝";
        case 'r': return "♜";
        case 'q': return "♛";
        case 'k': return "♚";
        default: return " ";
    }
}

void init_board(void) {
    const char *initial[8] = {
        "rnbqkbnr",
        "pppppppp",
        "........",
        "........",
        "........",
        "........",
        "PPPPPPPP",
        "RNBQKBNR"
    };
    
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            game.board[i][j] = initial[i][j];
        }
    }
    
    game.turn = WHITE;
    game.move_count = 0;
    game.en_passant_col = -1;
    game.white_king_moved = 0;
    game.black_king_moved = 0;
    game.white_rook_a_moved = 0;
    game.white_rook_h_moved = 0;
    game.black_rook_a_moved = 0;
    game.black_rook_h_moved = 0;
    
    ui.cursor_row = 6;
    ui.cursor_col = 4;
    ui.selected = 0;
    ui.legal_move_count = 0;
    ui.has_last_move = 0;
    ui.in_check = 0;
}

int is_white(char piece) {
    return piece >= 'A' && piece <= 'Z';
}

int is_black(char piece) {
    return piece >= 'a' && piece <= 'z';
}

int is_empty(char piece) {
    return piece == EMPTY;
}

int is_valid_square(int row, int col) {
    return row >= 0 && row < BOARD_SIZE && col >= 0 && col < BOARD_SIZE;
}

void find_king_position(int color, int *row, int *col) {
    char king = (color == WHITE) ? W_KING : B_KING;
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            if (game.board[i][j] == king) {
                *row = i;
                *col = j;
                return;
            }
        }
    }
}

int is_square_attacked(int row, int col, int by_color) {
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            char piece = game.board[i][j];
            if (is_empty(piece)) continue;
            if ((by_color == WHITE && !is_white(piece)) || 
                (by_color == BLACK && !is_black(piece))) continue;
            
            char p = toupper(piece);
            
            if (p == 'P') {
                int dir = (by_color == WHITE) ? -1 : 1;
                if (i + dir == row && (j - 1 == col || j + 1 == col)) {
                    return 1;
                }
            }
            else if (p == 'N') {
                int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
                int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
                for (int k = 0; k < 8; k++) {
                    if (i + dr[k] == row && j + dc[k] == col) {
                        return 1;
                    }
                }
            }
            else if (p == 'K') {
                if (abs(i - row) <= 1 && abs(j - col) <= 1) {
                    return 1;
                }
            }
            else {
                int directions[8][2] = {{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};
                int start = (p == 'B') ? 4 : (p == 'R') ? 0 : 0;
                int end = (p == 'B') ? 8 : (p == 'R') ? 4 : 8;
                
                for (int d = start; d < end; d++) {
                    int r = i + directions[d][0];
                    int c = j + directions[d][1];
                    while (is_valid_square(r, c)) {
                        if (r == row && c == col) {
                            return 1;
                        }
                        if (!is_empty(game.board[r][c])) break;
                        r += directions[d][0];
                        c += directions[d][1];
                    }
                }
            }
        }
    }
    return 0;
}

int is_in_check(int color) {
    int king_row, king_col;
    find_king_position(color, &king_row, &king_col);
    return is_square_attacked(king_row, king_col, 1 - color);
}

void generate_pseudo_legal_moves(int row, int col, Move *moves, int *count) {
    char piece = game.board[row][col];
    if (is_empty(piece)) return;
    
    int color = is_white(piece) ? WHITE : BLACK;
    char p = toupper(piece);
    
    *count = 0;
    
    if (p == 'P') {
        int dir = (color == WHITE) ? -1 : 1;
        int start_row = (color == WHITE) ? 6 : 1;
        
        if (is_valid_square(row + dir, col) && is_empty(game.board[row + dir][col])) {
            moves[*count].from_row = row;
            moves[*count].from_col = col;
            moves[*count].to_row = row + dir;
            moves[*count].to_col = col;
            moves[*count].promotion = '\0';
            moves[*count].is_castle = 0;
            moves[*count].is_en_passant = 0;
            (*count)++;
            
            if (row == start_row && is_empty(game.board[row + 2*dir][col])) {
                moves[*count].from_row = row;
                moves[*count].from_col = col;
                moves[*count].to_row = row + 2*dir;
                moves[*count].to_col = col;
                moves[*count].promotion = '\0';
                moves[*count].is_castle = 0;
                moves[*count].is_en_passant = 0;
                (*count)++;
            }
        }
        
        for (int dc = -1; dc <= 1; dc += 2) {
            if (is_valid_square(row + dir, col + dc)) {
                char target = game.board[row + dir][col + dc];
                if ((color == WHITE && is_black(target)) || 
                    (color == BLACK && is_white(target))) {
                    moves[*count].from_row = row;
                    moves[*count].from_col = col;
                    moves[*count].to_row = row + dir;
                    moves[*count].to_col = col + dc;
                    moves[*count].promotion = '\0';
                    moves[*count].is_castle = 0;
                    moves[*count].is_en_passant = 0;
                    (*count)++;
                }
                if (game.en_passant_col == col + dc && row == (color == WHITE ? 3 : 4)) {
                    moves[*count].from_row = row;
                    moves[*count].from_col = col;
                    moves[*count].to_row = row + dir;
                    moves[*count].to_col = col + dc;
                    moves[*count].promotion = '\0';
                    moves[*count].is_castle = 0;
                    moves[*count].is_en_passant = 1;
                    (*count)++;
                }
            }
        }
    }
    else if (p == 'N') {
        int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
        int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; i++) {
            int nr = row + dr[i];
            int nc = col + dc[i];
            if (is_valid_square(nr, nc)) {
                char target = game.board[nr][nc];
                if (is_empty(target) || 
                    (color == WHITE && is_black(target)) ||
                    (color == BLACK && is_white(target))) {
                    moves[*count].from_row = row;
                    moves[*count].from_col = col;
                    moves[*count].to_row = nr;
                    moves[*count].to_col = nc;
                    moves[*count].promotion = '\0';
                    moves[*count].is_castle = 0;
                    moves[*count].is_en_passant = 0;
                    (*count)++;
                }
            }
        }
    }
    else if (p == 'K') {
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = row + dr;
                int nc = col + dc;
                if (is_valid_square(nr, nc)) {
                    char target = game.board[nr][nc];
                    if (is_empty(target) || 
                        (color == WHITE && is_black(target)) ||
                        (color == BLACK && is_white(target))) {
                        moves[*count].from_row = row;
                        moves[*count].from_col = col;
                        moves[*count].to_row = nr;
                        moves[*count].to_col = nc;
                        moves[*count].promotion = '\0';
                        moves[*count].is_castle = 0;
                        moves[*count].is_en_passant = 0;
                        (*count)++;
                    }
                }
            }
        }
        
        if (color == WHITE && !game.white_king_moved && !is_in_check(WHITE)) {
            if (!game.white_rook_h_moved && 
                is_empty(game.board[7][5]) && is_empty(game.board[7][6]) &&
                !is_square_attacked(7, 5, BLACK) && !is_square_attacked(7, 6, BLACK)) {
                moves[*count].from_row = 7;
                moves[*count].from_col = 4;
                moves[*count].to_row = 7;
                moves[*count].to_col = 6;
                moves[*count].is_castle = 1;
                moves[*count].is_en_passant = 0;
                moves[*count].promotion = '\0';
                (*count)++;
            }
            if (!game.white_rook_a_moved && 
                is_empty(game.board[7][1]) && is_empty(game.board[7][2]) && is_empty(game.board[7][3]) &&
                !is_square_attacked(7, 2, BLACK) && !is_square_attacked(7, 3, BLACK)) {
                moves[*count].from_row = 7;
                moves[*count].from_col = 4;
                moves[*count].to_row = 7;
                moves[*count].to_col = 2;
                moves[*count].is_castle = 1;
                moves[*count].is_en_passant = 0;
                moves[*count].promotion = '\0';
                (*count)++;
            }
        }
        else if (color == BLACK && !game.black_king_moved && !is_in_check(BLACK)) {
            if (!game.black_rook_h_moved && 
                is_empty(game.board[0][5]) && is_empty(game.board[0][6]) &&
                !is_square_attacked(0, 5, WHITE) && !is_square_attacked(0, 6, WHITE)) {
                moves[*count].from_row = 0;
                moves[*count].from_col = 4;
                moves[*count].to_row = 0;
                moves[*count].to_col = 6;
                moves[*count].is_castle = 1;
                moves[*count].is_en_passant = 0;
                moves[*count].promotion = '\0';
                (*count)++;
            }
            if (!game.black_rook_a_moved && 
                is_empty(game.board[0][1]) && is_empty(game.board[0][2]) && is_empty(game.board[0][3]) &&
                !is_square_attacked(0, 2, WHITE) && !is_square_attacked(0, 3, WHITE)) {
                moves[*count].from_row = 0;
                moves[*count].from_col = 4;
                moves[*count].to_row = 0;
                moves[*count].to_col = 2;
                moves[*count].is_castle = 1;
                moves[*count].is_en_passant = 0;
                moves[*count].promotion = '\0';
                (*count)++;
            }
        }
    }
    else {
        int directions[8][2] = {{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};
        int start = (p == 'B') ? 4 : (p == 'R') ? 0 : 0;
        int end = (p == 'B') ? 8 : (p == 'R') ? 4 : 8;
        
        for (int d = start; d < end; d++) {
            int nr = row + directions[d][0];
            int nc = col + directions[d][1];
            while (is_valid_square(nr, nc)) {
                char target = game.board[nr][nc];
                if (is_empty(target)) {
                    moves[*count].from_row = row;
                    moves[*count].from_col = col;
                    moves[*count].to_row = nr;
                    moves[*count].to_col = nc;
                    moves[*count].promotion = '\0';
                    moves[*count].is_castle = 0;
                    moves[*count].is_en_passant = 0;
                    (*count)++;
                }
                else if ((color == WHITE && is_black(target)) ||
                         (color == BLACK && is_white(target))) {
                    moves[*count].from_row = row;
                    moves[*count].from_col = col;
                    moves[*count].to_row = nr;
                    moves[*count].to_col = nc;
                    moves[*count].promotion = '\0';
                    moves[*count].is_castle = 0;
                    moves[*count].is_en_passant = 0;
                    (*count)++;
                    break;
                }
                else {
                    break;
                }
                nr += directions[d][0];
                nc += directions[d][1];
            }
        }
    }
}

int is_move_legal(Move *move) {
    char piece = game.board[move->from_row][move->from_col];
    int color = is_white(piece) ? WHITE : BLACK;
    
    char temp_to = game.board[move->to_row][move->to_col];
    char temp_en_passant = '\0';
    
    game.board[move->to_row][move->to_col] = piece;
    game.board[move->from_row][move->from_col] = EMPTY;
    
    if (move->is_en_passant) {
        int capture_row = (color == WHITE) ? move->to_row + 1 : move->to_row - 1;
        temp_en_passant = game.board[capture_row][move->to_col];
        game.board[capture_row][move->to_col] = EMPTY;
    }
    
    if (move->is_castle) {
        if (move->to_col == 6) {
            game.board[move->to_row][5] = game.board[move->to_row][7];
            game.board[move->to_row][7] = EMPTY;
        } else {
            game.board[move->to_row][3] = game.board[move->to_row][0];
            game.board[move->to_row][0] = EMPTY;
        }
    }
    
    int legal = !is_in_check(color);
    
    game.board[move->from_row][move->from_col] = piece;
    game.board[move->to_row][move->to_col] = temp_to;
    
    if (move->is_en_passant) {
        int capture_row = (color == WHITE) ? move->to_row + 1 : move->to_row - 1;
        game.board[capture_row][move->to_col] = temp_en_passant;
    }
    
    if (move->is_castle) {
        if (move->to_col == 6) {
            game.board[move->to_row][7] = game.board[move->to_row][5];
            game.board[move->to_row][5] = EMPTY;
        } else {
            game.board[move->to_row][0] = game.board[move->to_row][3];
            game.board[move->to_row][3] = EMPTY;
        }
    }
    
    return legal;
}

void generate_legal_moves(int row, int col) {
    Move pseudo_moves[MAX_LEGAL_MOVES];
    int pseudo_count = 0;
    
    generate_pseudo_legal_moves(row, col, pseudo_moves, &pseudo_count);
    
    ui.legal_move_count = 0;
    for (int i = 0; i < pseudo_count; i++) {
        if (is_move_legal(&pseudo_moves[i])) {
            ui.legal_moves[ui.legal_move_count++] = pseudo_moves[i];
        }
    }
}

void make_move(Move *move) {
    move->moved_piece = game.board[move->from_row][move->from_col];
    move->captured_piece = game.board[move->to_row][move->to_col];
    move->prev_en_passant_col = game.en_passant_col;
    move->prev_white_king_moved = game.white_king_moved;
    move->prev_black_king_moved = game.black_king_moved;
    move->prev_white_rook_a_moved = game.white_rook_a_moved;
    move->prev_white_rook_h_moved = game.white_rook_h_moved;
    move->prev_black_rook_a_moved = game.black_rook_a_moved;
    move->prev_black_rook_h_moved = game.black_rook_h_moved;
    
    char piece = game.board[move->from_row][move->from_col];
    int color = is_white(piece) ? WHITE : BLACK;
    char p = toupper(piece);
    
    game.board[move->to_row][move->to_col] = piece;
    game.board[move->from_row][move->from_col] = EMPTY;
    
    game.en_passant_col = -1;
    if (p == 'P') {
        if (abs(move->to_row - move->from_row) == 2) {
            game.en_passant_col = move->from_col;
        }
        if (move->is_en_passant) {
            int capture_row = (color == WHITE) ? move->to_row + 1 : move->to_row - 1;
            game.board[capture_row][move->to_col] = EMPTY;
        }
        if (move->to_row == 0 || move->to_row == 7) {
            if (move->promotion) {
                game.board[move->to_row][move->to_col] = move->promotion;
            } else {
                game.board[move->to_row][move->to_col] = (color == WHITE) ? W_QUEEN : B_QUEEN;
            }
        }
    }
    
    if (move->is_castle) {
        if (move->to_col == 6) {
            game.board[move->to_row][5] = game.board[move->to_row][7];
            game.board[move->to_row][7] = EMPTY;
        } else {
            game.board[move->to_row][3] = game.board[move->to_row][0];
            game.board[move->to_row][0] = EMPTY;
        }
    }
    
    if (p == 'K') {
        if (color == WHITE) game.white_king_moved = 1;
        else game.black_king_moved = 1;
    }
    if (p == 'R') {
        if (color == WHITE) {
            if (move->from_col == 0) game.white_rook_a_moved = 1;
            if (move->from_col == 7) game.white_rook_h_moved = 1;
        } else {
            if (move->from_col == 0) game.black_rook_a_moved = 1;
            if (move->from_col == 7) game.black_rook_h_moved = 1;
        }
    }
    
    game.move_history[game.move_count++] = *move;
    ui.last_move = *move;
    ui.has_last_move = 1;
    
    game.turn = 1 - game.turn;
    
    if (is_in_check(game.turn)) {
        ui.in_check = 1;
        find_king_position(game.turn, &ui.check_row, &ui.check_col);
    } else {
        ui.in_check = 0;
    }
}

void undo_move(void) {
    if (game.move_count == 0) return;
    
    Move *move = &game.move_history[--game.move_count];
    
    game.board[move->from_row][move->from_col] = move->moved_piece;
    game.board[move->to_row][move->to_col] = move->captured_piece;
    
    char p = toupper(move->moved_piece);
    int color = is_white(move->moved_piece) ? WHITE : BLACK;
    
    if (move->is_en_passant) {
        int capture_row = (color == WHITE) ? move->to_row + 1 : move->to_row - 1;
        game.board[capture_row][move->to_col] = (color == WHITE) ? B_PAWN : W_PAWN;
        game.board[move->to_row][move->to_col] = EMPTY;
    }
    
    if (move->is_castle) {
        if (move->to_col == 6) {
            game.board[move->to_row][7] = game.board[move->to_row][5];
            game.board[move->to_row][5] = EMPTY;
        } else {
            game.board[move->to_row][0] = game.board[move->to_row][3];
            game.board[move->to_row][3] = EMPTY;
        }
    }
    
    game.en_passant_col = move->prev_en_passant_col;
    game.white_king_moved = move->prev_white_king_moved;
    game.black_king_moved = move->prev_black_king_moved;
    game.white_rook_a_moved = move->prev_white_rook_a_moved;
    game.white_rook_h_moved = move->prev_white_rook_h_moved;
    game.black_rook_a_moved = move->prev_black_rook_a_moved;
    game.black_rook_h_moved = move->prev_black_rook_h_moved;
    
    game.turn = 1 - game.turn;
    
    if (game.move_count > 0) {
        ui.last_move = game.move_history[game.move_count - 1];
        ui.has_last_move = 1;
    } else {
        ui.has_last_move = 0;
    }
    
    if (is_in_check(game.turn)) {
        ui.in_check = 1;
        find_king_position(game.turn, &ui.check_row, &ui.check_col);
    } else {
        ui.in_check = 0;
    }
}

char *move_to_uci(Move *move) {
    static char uci[6];
    sprintf(uci, "%c%d%c%d", 
            'a' + move->from_col, 8 - move->from_row,
            'a' + move->to_col, 8 - move->to_row);
    if (move->promotion && (move->to_row == 0 || move->to_row == 7)) {
        char prom[2] = {tolower(move->promotion), '\0'};
        strcat(uci, prom);
    }
    return uci;
}

char *move_to_pgn(Move *move) {
    static char pgn[16];
    pgn[0] = '\0';
    
    char piece = toupper(move->moved_piece);
    
    if (move->is_castle) {
        if (move->to_col == 6) {
            strcpy(pgn, "O-O");
        } else {
            strcpy(pgn, "O-O-O");
        }
    } else {
        if (piece != 'P') {
            char piece_char[2] = {piece, '\0'};
            strcat(pgn, piece_char);
        }
        
        if (move->captured_piece != EMPTY || move->is_en_passant) {
            if (piece == 'P') {
                char file[2] = {'a' + move->from_col, '\0'};
                strcat(pgn, file);
            }
            strcat(pgn, "x");
        }
        
        char dest[3];
        sprintf(dest, "%c%d", 'a' + move->to_col, 8 - move->to_row);
        strcat(pgn, dest);
        
        if (move->promotion && (move->to_row == 0 || move->to_row == 7)) {
            strcat(pgn, "=");
            char prom[2] = {toupper(move->promotion), '\0'};
            strcat(pgn, prom);
        }
    }
    
    return pgn;
}

void setup_terminal(void) {
    struct termios new_termios;
    tcgetattr(STDIN_FILENO, &orig_termios);
    new_termios = orig_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);
    new_termios.c_cc[VMIN] = 0;
    new_termios.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
}

void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

int kbhit(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

char getch(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) > 0) {
        return c;
    }
    return 0;
}

void clear_screen(void) {
    printf("\033[2J\033[H");
}

void draw_board(void) {
    clear_screen();
    
    printf("  Chess Game - %s to move\n\n", game.turn == WHITE ? "White" : "Black");
    
    printf("    a   b   c   d   e   f   g   h\n");
    printf("  ┌───┬───┬───┬───┬───┬───┬───┬───┐\n");
    
    for (int i = 0; i < BOARD_SIZE; i++) {
        printf("%d │", 8 - i);
        for (int j = 0; j < BOARD_SIZE; j++) {
            int is_light = (i + j) % 2 == 0;
            int is_cursor = (i == ui.cursor_row && j == ui.cursor_col);
            int is_selected = (ui.selected && i == ui.selected_row && j == ui.selected_col);
            int is_legal_dest = 0;
            int is_last_move = 0;
            int is_check = (ui.in_check && i == ui.check_row && j == ui.check_col);
            
            if (ui.selected) {
                for (int k = 0; k < ui.legal_move_count; k++) {
                    if (ui.legal_moves[k].to_row == i && ui.legal_moves[k].to_col == j) {
                        is_legal_dest = 1;
                        break;
                    }
                }
            }
            
            if (ui.has_last_move) {
                if ((ui.last_move.from_row == i && ui.last_move.from_col == j) ||
                    (ui.last_move.to_row == i && ui.last_move.to_col == j)) {
                    is_last_move = 1;
                }
            }
            
            if (is_check) {
                printf("\033[41m");
            } else if (is_selected) {
                printf("\033[43m");
            } else if (is_legal_dest) {
                printf("\033[42m");
            } else if (is_last_move) {
                printf("\033[46m");
            } else if (is_light) {
                printf("\033[47m");
            } else {
                printf("\033[100m");
            }
            
            char piece = game.board[i][j];
            if (is_white(piece)) {
                printf("\033[97;1m");
            } else if (is_black(piece)) {
                printf("\033[30;1m");
            }
            
            if (is_cursor) {
                printf("\033[7m");
            }
            
            printf(" %s ", get_piece_unicode(piece));
            printf("\033[0m");
            
            if (j < 7) printf("│");
        }
        printf("│%d\n", 8 - i);
        if (i < 7) {
            printf("  ├───┼───┼───┼───┼───┼───┼───┼───┤\n");
        }
    }
    printf("  └───┴───┴───┴───┴───┴───┴───┴───┘\n");
    printf("    a   b   c   d   e   f   g   h\n\n");
    
    printf("  Recent moves: ");
    int start = game.move_count > 6 ? game.move_count - 6 : 0;
    for (int i = start; i < game.move_count; i++) {
        if (i % 2 == 0) {
            printf("%d.", i/2 + 1);
        }
        printf("%s ", move_to_pgn(&game.move_history[i]));
    }
    printf("\n\n");
    
    printf("  Controls: Arrow keys=move  Space=select/move  U=undo  E=engine\n");
    printf("            T=time control   Q=quit\n");
    
    if (ui.selected) {
        printf("\n  Selected: %c%d (%d legal moves)", 
               'a' + ui.selected_col, 8 - ui.selected_row, ui.legal_move_count);
    }
    
    if (engine.active) {
        printf("\n  Engine: Stockfish - %s=%d", 
               engine.time_control_type, engine.time_control_value);
    }
    
    if (ui.in_check) {
        printf("\n  \033[1;31mCheck!\033[0m");
    }
    
    printf("\n");
    fflush(stdout);
}

char *find_stockfish(void) {
    const char *paths[] = {
        "/usr/local/bin/stockfish",
        "/usr/bin/stockfish",
        "/opt/homebrew/bin/stockfish",
        "/opt/local/bin/stockfish",
        "./stockfish",
        NULL
    };
    
    static char found_path[256];
    struct stat st;
    
    for (int i = 0; paths[i] != NULL; i++) {
        if (stat(paths[i], &st) == 0) {
            strcpy(found_path, paths[i]);
            return found_path;
        }
    }
    
    return NULL;
}

void init_engine(const char *path) {
    int to_engine_pipe[2];
    int from_engine_pipe[2];
    
    if (pipe(to_engine_pipe) == -1 || pipe(from_engine_pipe) == -1) {
        return;
    }
    
    engine.pid = fork();
    
    if (engine.pid == 0) {
        dup2(to_engine_pipe[0], STDIN_FILENO);
        dup2(from_engine_pipe[1], STDOUT_FILENO);
        dup2(from_engine_pipe[1], STDERR_FILENO);
        close(to_engine_pipe[0]);
        close(to_engine_pipe[1]);
        close(from_engine_pipe[0]);
        close(from_engine_pipe[1]);
        
        execlp(path, path, NULL);
        exit(1);
    }
    
    close(to_engine_pipe[0]);
    close(from_engine_pipe[1]);
    
    engine.to_engine = fdopen(to_engine_pipe[1], "w");
    engine.from_engine = fdopen(from_engine_pipe[0], "r");
    engine.active = 1;
    
    strcpy(engine.time_control_type, "depth");
    engine.time_control_value = 15;
    
    fprintf(engine.to_engine, "uci\n");
    fflush(engine.to_engine);
    usleep(100000);
    fprintf(engine.to_engine, "isready\n");
    fflush(engine.to_engine);
    usleep(100000);
    fprintf(engine.to_engine, "ucinewgame\n");
    fflush(engine.to_engine);
}

void send_to_engine(const char *cmd) {
    if (!engine.active) return;
    fprintf(engine.to_engine, "%s", cmd);
    fflush(engine.to_engine);
}

char *read_from_engine_timeout(int timeout_ms) {
    static char buffer[4096];
    if (!engine.active) return NULL;
    
    fd_set fds;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    FD_ZERO(&fds);
    FD_SET(fileno(engine.from_engine), &fds);
    
    if (select(fileno(engine.from_engine) + 1, &fds, NULL, NULL, &tv) > 0) {
        if (fgets(buffer, sizeof(buffer), engine.from_engine)) {
            return buffer;
        }
    }
    
    return NULL;
}

void close_engine(void) {
    if (engine.active) {
        send_to_engine("quit\n");
        fclose(engine.to_engine);
        fclose(engine.from_engine);
        engine.active = 0;
    }
}

void engine_move(void) {
    if (!engine.active) return;
    
    char position[4096] = "position startpos";
    
    if (game.move_count > 0) {
        strcat(position, " moves");
        for (int i = 0; i < game.move_count; i++) {
            strcat(position, " ");
            strcat(position, move_to_uci(&game.move_history[i]));
        }
    }
    strcat(position, "\n");
    
    send_to_engine(position);
    
    char go_cmd[128];
    if (strcmp(engine.time_control_type, "depth") == 0) {
        sprintf(go_cmd, "go depth %d\n", engine.time_control_value);
    } else if (strcmp(engine.time_control_type, "nodes") == 0) {
        sprintf(go_cmd, "go nodes %d\n", engine.time_control_value);
    } else {
        sprintf(go_cmd, "go movetime %d\n", engine.time_control_value);
    }
    
    send_to_engine(go_cmd);
    
    char *response;
    char best_move[16] = "";
    
    while ((response = read_from_engine_timeout(30000)) != NULL) {
        if (strncmp(response, "bestmove", 8) == 0) {
            sscanf(response, "bestmove %s", best_move);
            break;
        }
    }
    
    if (strlen(best_move) >= 4) {
        int from_col = best_move[0] - 'a';
        int from_row = 8 - (best_move[1] - '0');
        int to_col = best_move[2] - 'a';
        int to_row = 8 - (best_move[3] - '0');
        
        Move pseudo_moves[MAX_LEGAL_MOVES];
        int count = 0;
        generate_pseudo_legal_moves(from_row, from_col, pseudo_moves, &count);
        
        for (int i = 0; i < count; i++) {
            if (pseudo_moves[i].to_row == to_row && pseudo_moves[i].to_col == to_col) {
                if (strlen(best_move) == 5) {
                    char prom = toupper(best_move[4]);
                    if (is_black(game.board[from_row][from_col])) {
                        prom = tolower(prom);
                    }
                    pseudo_moves[i].promotion = prom;
                }
                if (is_move_legal(&pseudo_moves[i])) {
                    make_move(&pseudo_moves[i]);
                    break;
                }
            }
        }
    }
}

void handle_input(char c) {
    if (c == 'q' || c == 'Q') {
        close_engine();
        restore_terminal();
        clear_screen();
        exit(0);
    }
    else if (c == 'u' || c == 'U') {
        undo_move();
        ui.selected = 0;
    }
    else if (c == 'e' || c == 'E') {
        if (engine.active) {
            engine_move();
            ui.selected = 0;
        }
    }
    else if (c == 't' || c == 'T') {
        if (strcmp(engine.time_control_type, "depth") == 0) {
            strcpy(engine.time_control_type, "nodes");
            engine.time_control_value = 1000000;
        } else if (strcmp(engine.time_control_type, "nodes") == 0) {
            strcpy(engine.time_control_type, "movetime");
            engine.time_control_value = 1000;
        } else {
            strcpy(engine.time_control_type, "depth");
            engine.time_control_value = 15;
        }
    }
    else if (c == ' ') {
        if (!ui.selected) {
            char piece = game.board[ui.cursor_row][ui.cursor_col];
            if (!is_empty(piece)) {
                int color = is_white(piece) ? WHITE : BLACK;
                if (color == game.turn) {
                    ui.selected = 1;
                    ui.selected_row = ui.cursor_row;
                    ui.selected_col = ui.cursor_col;
                    generate_legal_moves(ui.cursor_row, ui.cursor_col);
                }
            }
        } else {
            for (int i = 0; i < ui.legal_move_count; i++) {
                if (ui.legal_moves[i].to_row == ui.cursor_row && 
                    ui.legal_moves[i].to_col == ui.cursor_col) {
                    make_move(&ui.legal_moves[i]);
                    break;
                }
            }
            ui.selected = 0;
            ui.legal_move_count = 0;
        }
    }
    else if (c == 27) {
        char seq1 = getch();
        if (seq1 == '[') {
            char seq2 = getch();
            if (seq2 == 'A' && ui.cursor_row > 0) ui.cursor_row--;
            else if (seq2 == 'B' && ui.cursor_row < 7) ui.cursor_row++;
            else if (seq2 == 'C' && ui.cursor_col < 7) ui.cursor_col++;
            else if (seq2 == 'D' && ui.cursor_col > 0) ui.cursor_col--;
        }
    }
}

void cleanup(int sig) {
    close_engine();
    restore_terminal();
    clear_screen();
    exit(0);
}

int main(void) {
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
    
    init_board();
    
    char *stockfish_path = find_stockfish();
    if (stockfish_path) {
        init_engine(stockfish_path);
    }
    
    setup_terminal();
    
    while (1) {
        draw_board();
        
        if (kbhit()) {
            char c = getch();
            handle_input(c);
        }
        
        usleep(16000);
    }
    
    cleanup(0);
    return 0;
}
