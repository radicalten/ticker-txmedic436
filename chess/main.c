#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <signal.h>
#include <termios.h>

#define IN_BOUNDS(r, c) ((r) >= 0 && (r) < 8 && (c) >= 0 && (c) < 8)
#define SQ(r, c) ((r) * 8 + (c))

// Structure definitions
typedef struct {
    int from;
    int to;
    int promo; // 0=none, 2=N, 3=B, 4=R, 5=Q
} Move;

typedef struct {
    char board[64]; // 1=P, 2=N, 3=B, 4=R, 5=Q, 6=K (positive White, negative Black)
    int turn;       // 1 = White, -1 = Black
    int castling;   // Bitmask: 1=W_OO, 2=W_OOO, 4=B_OO, 8=B_OOO
    int ep_square;  // 0-63, or -1 if none
    int halfmove;
    int fullmove;
} Board;

typedef struct {
    Move moves[256];
    int count;
} MoveList;

// Global settings and state
enum TC_Mode { TC_DEPTH, TC_NODES, TC_TIME };
enum TC_Mode current_tc = TC_DEPTH;
int tc_depth = 10;
int tc_nodes = 100000;
int tc_time = 2000; // in milliseconds

Board current_board;
Board history_boards[2048];
Move history_moves[2048];
char history_san[2048][16];
int history_count = 0;

int cursor_sq = 60; // Starts at e1
int selected_sq = -1;

char engine_path[512] = "";
int player_color = 1; // 1 = White, -1 = Black
int to_engine[2], from_engine[2];
pid_t engine_pid = -1;
int engine_active = 0;
int engine_thinking = 0;

struct termios orig_termios;

// Forward declarations
void generate_legal_moves(const Board *b, MoveList *list);
int is_square_attacked(const Board *b, int sq, int attacker_color);
void make_move(const Board *b, Move m, Board *out);
int find_king(const Board *b, int color);

// Raw mode configuration
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
    fflush(stdout);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
    fflush(stdout);
}

// Engine handling
void stop_engine() {
    if (engine_active) {
        write(to_engine[1], "quit\n", 5);
        close(to_engine[1]);
        close(from_engine[0]);
        kill(engine_pid, SIGTERM);
        engine_active = 0;
        engine_thinking = 0;
    }
}

int start_engine(const char *path) {
    stop_engine();
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return 0;
    engine_pid = fork();
    if (engine_pid < 0) return 0;
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]);
        close(from_engine[0]);
        execl(path, path, (char *)NULL);
        exit(1);
    }
    close(to_engine[0]);
    close(from_engine[1]);
    
    // Set non-blocking read
    int flags = fcntl(from_engine[0], F_GETFL, 0);
    fcntl(from_engine[0], F_SETFL, flags | O_NONBLOCK);
    
    write(to_engine[1], "uci\nisready\n", 12);
    engine_active = 1;
    engine_thinking = 0;
    return 1;
}

void send_position_to_engine() {
    if (!engine_active) return;
    char cmd[16384] = "position startpos";
    if (history_count > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < history_count; ++i) {
            char mv[16];
            Move m = history_moves[i];
            int f_col = m.from % 8, f_row = 8 - (m.from / 8);
            int t_col = m.to % 8, t_row = 8 - (m.to / 8);
            sprintf(mv, " %c%d%c%d", 'a' + f_col, f_row, 'a' + t_col, t_row);
            strcat(cmd, mv);
            if (m.promo) {
                char p_char = 'q';
                if (m.promo == 2) p_char = 'n';
                else if (m.promo == 3) p_char = 'b';
                else if (m.promo == 4) p_char = 'r';
                char p_str[2] = {p_char, '\0'};
                strcat(cmd, p_str);
            }
        }
    }
    strcat(cmd, "\n");
    write(to_engine[1], cmd, strlen(cmd));
}

void trigger_engine_thinking() {
    if (!engine_active || engine_thinking) return;
    send_position_to_engine();
    char go_cmd[128];
    if (current_tc == TC_DEPTH) {
        sprintf(go_cmd, "go depth %d\n", tc_depth);
    } else if (current_tc == TC_NODES) {
        sprintf(go_cmd, "go nodes %d\n", tc_nodes);
    } else {
        sprintf(go_cmd, "go movetime %d\n", tc_time);
    }
    write(to_engine[1], go_cmd, strlen(go_cmd));
    engine_thinking = 1;
}

// Convert board coordinate to algebraic string (e.g., "e2e4")
void move_to_string(Move m, char *str) {
    sprintf(str, "%c%d%c%d", 'a' + (m.from % 8), 8 - (m.from / 8), 'a' + (m.to % 8), 8 - (m.to / 8));
    if (m.promo) {
        char p = 'q';
        if (m.promo == 2) p = 'n';
        if (m.promo == 3) p = 'b';
        if (m.promo == 4) p = 'r';
        sprintf(str + 4, "%c", p);
    }
}

// Simple rule engine
void init_board(Board *b) {
    static const char initial_state[64] = {
        -4, -2, -3, -5, -6, -3, -2, -4,
        -1, -1, -1, -1, -1, -1, -1, -1,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         1,  1,  1,  1,  1,  1,  1,  1,
         4,  2,  3,  5,  6,  3,  2,  4
    };
    memcpy(b->board, initial_state, 64);
    b->turn = 1;
    b->castling = 1 | 2 | 4 | 8;
    b->ep_square = -1;
    b->halfmove = 0;
    b->fullmove = 1;
}

int is_square_attacked(const Board *b, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;
    
    // Pawn Attacks
    if (attacker_color == 1) { // White attacks from below (increasing rows)
        int r_att = r + 1;
        if (r_att < 8) {
            if (c - 1 >= 0 && b->board[SQ(r_att, c - 1)] == 1) return 1;
            if (c + 1 < 8  && b->board[SQ(r_att, c + 1)] == 1) return 1;
        }
    } else { // Black attacks from above (decreasing rows)
        int r_att = r - 1;
        if (r_att >= 0) {
            if (c - 1 >= 0 && b->board[SQ(r_att, c - 1)] == -1) return 1;
            if (c + 1 < 8  && b->board[SQ(r_att, c + 1)] == -1) return 1;
        }
    }
    
    // Knight Attacks
    static const int knight_dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    static const int knight_dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; ++i) {
        int nr = r + knight_dr[i], nc = c + knight_dc[i];
        if (IN_BOUNDS(nr, nc)) {
            if (b->board[SQ(nr, nc)] == 2 * attacker_color) return 1;
        }
    }
    
    // King Attacks (to avoid infinite loops)
    static const int king_dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static const int king_dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; ++i) {
        int nr = r + king_dr[i], nc = c + king_dc[i];
        if (IN_BOUNDS(nr, nc)) {
            if (b->board[SQ(nr, nc)] == 6 * attacker_color) return 1;
        }
    }
    
    // Bishop / Queen (diagonals)
    static const int diag_dr[] = {-1, -1, 1, 1};
    static const int diag_dc[] = {-1, 1, -1, 1};
    for (int d = 0; d < 4; ++d) {
        int nr = r, nc = c;
        while (1) {
            nr += diag_dr[d]; nc += diag_dc[d];
            if (!IN_BOUNDS(nr, nc)) break;
            int p = b->board[SQ(nr, nc)];
            if (p != 0) {
                if (p == 3 * attacker_color || p == 5 * attacker_color) return 1;
                break;
            }
        }
    }
    
    // Rook / Queen (orthogonals)
    static const int orth_dr[] = {-1, 1, 0, 0};
    static const int orth_dc[] = {0, 0, -1, 1};
    for (int d = 0; d < 4; ++d) {
        int nr = r, nc = c;
        while (1) {
            nr += orth_dr[d]; nc += orth_dc[d];
            if (!IN_BOUNDS(nr, nc)) break;
            int p = b->board[SQ(nr, nc)];
            if (p != 0) {
                if (p == 4 * attacker_color || p == 5 * attacker_color) return 1;
                break;
            }
        }
    }
    return 0;
}

void add_move(MoveList *list, int from, int to, int promo) {
    if (list->count < 256) {
        list->moves[list->count++] = (Move){from, to, promo};
    }
}

void generate_pseudo_moves(const Board *b, MoveList *list) {
    int turn = b->turn;
    for (int sq = 0; sq < 64; ++sq) {
        int p = b->board[sq];
        if (p * turn <= 0) continue;
        
        int r = sq / 8, c = sq % 8;
        if (abs(p) == 1) { // Pawn
            int dir = -turn; // White moves up (-1), Black moves down (+1)
            int target = SQ(r + dir, c);
            if (IN_BOUNDS(r + dir, c) && b->board[target] == 0) {
                if (r + dir == 0 || r + dir == 7) {
                    add_move(list, sq, target, 2);
                    add_move(list, sq, target, 3);
                    add_move(list, sq, target, 4);
                    add_move(list, sq, target, 5);
                } else {
                    add_move(list, sq, target, 0);
                    // Double step
                    int start_row = (turn == 1) ? 6 : 1;
                    if (r == start_row && b->board[SQ(r + 2 * dir, c)] == 0) {
                        add_move(list, sq, SQ(r + 2 * dir, c), 0);
                    }
                }
            }
            // Captures
            int caps[2] = {c - 1, c + 1};
            for (int i = 0; i < 2; ++i) {
                int nc = caps[i];
                if (IN_BOUNDS(r + dir, nc)) {
                    int dest = SQ(r + dir, nc);
                    if (b->board[dest] * turn < 0) {
                        if (r + dir == 0 || r + dir == 7) {
                            add_move(list, sq, dest, 2);
                            add_move(list, sq, dest, 3);
                            add_move(list, sq, dest, 4);
                            add_move(list, sq, dest, 5);
                        } else {
                            add_move(list, sq, dest, 0);
                        }
                    }
                    // En Passant
                    if (dest == b->ep_square) {
                        add_move(list, sq, dest, 0);
                    }
                }
            }
        } else if (abs(p) == 2) { // Knight
            static const int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
            static const int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
            for (int i = 0; i < 8; ++i) {
                int nr = r + dr[i], nc = c + dc[i];
                if (IN_BOUNDS(nr, nc)) {
                    int dest = SQ(nr, nc);
                    if (b->board[dest] * turn <= 0) add_move(list, sq, dest, 0);
                }
            }
        } else if (abs(p) == 3 || abs(p) == 5) { // Bishop or Queen (diagonals)
            static const int dr[] = {-1, -1, 1, 1};
            static const int dc[] = {-1, 1, -1, 1};
            for (int d = 0; d < 4; ++d) {
                int nr = r, nc = c;
                while (1) {
                    nr += dr[d]; nc += dc[d];
                    if (!IN_BOUNDS(nr, nc)) break;
                    int dest = SQ(nr, nc);
                    if (b->board[dest] == 0) {
                        add_move(list, sq, dest, 0);
                    } else {
                        if (b->board[dest] * turn < 0) add_move(list, sq, dest, 0);
                        break;
                    }
                }
            }
        }
        
        if (abs(p) == 4 || abs(p) == 5) { // Rook or Queen (orthogonals)
            static const int dr[] = {-1, 1, 0, 0};
            static const int dc[] = {0, 0, -1, 1};
            for (int d = 0; d < 4; ++d) {
                int nr = r, nc = c;
                while (1) {
                    nr += dr[d]; nc += dc[d];
                    if (!IN_BOUNDS(nr, nc)) break;
                    int dest = SQ(nr, nc);
                    if (b->board[dest] == 0) {
                        add_move(list, sq, dest, 0);
                    } else {
                        if (b->board[dest] * turn < 0) add_move(list, sq, dest, 0);
                        break;
                    }
                }
            }
        }
        
        if (abs(p) == 6) { // King
            static const int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
            static const int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
            for (int i = 0; i < 8; ++i) {
                int nr = r + dr[i], nc = c + dc[i];
                if (IN_BOUNDS(nr, nc)) {
                    int dest = SQ(nr, nc);
                    if (b->board[dest] * turn <= 0) add_move(list, sq, dest, 0);
                }
            }
        }
    }
}

void generate_legal_moves(const Board *b, MoveList *list) {
    list->count = 0;
    MoveList pseudo;
    pseudo.count = 0;
    generate_pseudo_moves(b, &pseudo);
    
    for (int i = 0; i < pseudo.count; ++i) {
        Move m = pseudo.moves[i];
        Board temp;
        make_move(b, m, &temp);
        int ksq = find_king(&temp, b->turn);
        if (ksq != -1 && !is_square_attacked(&temp, ksq, -b->turn)) {
            add_move(list, m.from, m.to, m.promo);
        }
    }
    
    // Castling legality check
    int turn = b->turn;
    int ksq = find_king(b, turn);
    if (ksq != -1 && !is_square_attacked(b, ksq, -turn)) {
        if (turn == 1) {
            if ((b->castling & 1) && b->board[61] == 0 && b->board[62] == 0) {
                if (!is_square_attacked(b, 61, -1) && !is_square_attacked(b, 62, -1)) {
                    add_move(list, 60, 62, 0);
                }
            }
            if ((b->castling & 2) && b->board[59] == 0 && b->board[58] == 0 && b->board[57] == 0) {
                if (!is_square_attacked(b, 59, -1) && !is_square_attacked(b, 58, -1)) {
                    add_move(list, 60, 58, 0);
                }
            }
        } else {
            if ((b->castling & 4) && b->board[5] == 0 && b->board[6] == 0) {
                if (!is_square_attacked(b, 5, 1) && !is_square_attacked(b, 6, 1)) {
                    add_move(list, 4, 6, 0);
                }
            }
            if ((b->castling & 8) && b->board[3] == 0 && b->board[2] == 0 && b->board[1] == 0) {
                if (!is_square_attacked(b, 3, 1) && !is_square_attacked(b, 2, 1)) {
                    add_move(list, 4, 2, 0);
                }
            }
        }
    }
}

int find_king(const Board *b, int color) {
    for (int i = 0; i < 64; ++i) {
        if (b->board[i] == 6 * color) return i;
    }
    return -1;
}

void make_move(const Board *b, Move m, Board *out) {
    *out = *b;
    int p = b->board[m.from];
    int turn = b->turn;
    
    out->board[m.from] = 0;
    if (m.promo) {
        out->board[m.to] = m.promo * turn;
    } else {
        out->board[m.to] = p;
    }
    
    // EP Capture execution
    if (abs(p) == 1 && m.to == b->ep_square) {
        out->board[m.to + 8 * turn] = 0;
    }
    
    // Set new EP square
    if (abs(p) == 1 && abs(m.from - m.to) == 16) {
        out->ep_square = (m.from + m.to) / 2;
    } else {
        out->ep_square = -1;
    }
    
    // Castling execution
    if (abs(p) == 6) {
        if (m.from == 60 && m.to == 62) { out->board[63] = 0; out->board[61] = 4; }
        else if (m.from == 60 && m.to == 58) { out->board[56] = 0; out->board[59] = 4; }
        else if (m.from == 4 && m.to == 6) { out->board[7] = 0; out->board[5] = -4; }
        else if (m.from == 4 && m.to == 2) { out->board[0] = 0; out->board[3] = -4; }
    }
    
    // Castling rights update
    if (m.from == 60) out->castling &= ~3;
    if (m.from == 4)  out->castling &= ~12;
    if (m.from == 56 || m.to == 56) out->castling &= ~2;
    if (m.from == 63 || m.to == 63) out->castling &= ~1;
    if (m.from == 0  || m.to == 0)  out->castling &= ~8;
    if (m.from == 7  || m.to == 7)  out->castling &= ~4;
    
    out->turn = -turn;
    if (abs(p) == 1 || b->board[m.to] != 0) {
        out->halfmove = 0;
    } else {
        out->halfmove++;
    }
    if (turn == -1) out->fullmove++;
}

// Generates Standard Algebraic Notation for tracking histories
void get_san(const Board *before, Move m, char *out) {
    int p = abs(before->board[m.from]);
    int f_col = m.from % 8, f_row = m.from / 8;
    int t_col = m.to % 8, t_row = m.to / 8;
    
    if (p == 6 && abs(f_col - t_col) == 2) {
        if (t_col == 6) strcpy(out, "O-O");
        else strcpy(out, "O-O-O");
    } else {
        int idx = 0;
        if (p != 1) {
            char p_chars[] = {'?', 'P', 'N', 'B', 'R', 'Q', 'K'};
            out[idx++] = p_chars[p];
            
            // Disambiguate if needed
            MoveList legals;
            generate_legal_moves(before, &legals);
            int need_col = 0, need_row = 0, count_same = 0;
            for (int i = 0; i < legals.count; ++i) {
                Move tm = legals.moves[i];
                if (tm.from != m.from && tm.to == m.to && abs(before->board[tm.from]) == p) {
                    count_same++;
                    if (tm.from % 8 == f_col) need_row = 1;
                    else need_col = 1;
                }
            }
            if (count_same > 0) {
                if (need_col || !need_row) out[idx++] = 'a' + f_col;
                if (need_row) out[idx++] = '8' - f_row;
            }
        }
        
        int is_cap = (before->board[m.to] != 0) || (p == 1 && m.to == before->ep_square);
        if (is_cap) {
            if (p == 1) out[idx++] = 'a' + f_col;
            out[idx++] = 'x';
        }
        
        out[idx++] = 'a' + t_col;
        out[idx++] = '8' - t_row;
        
        if (m.promo) {
            out[idx++] = '=';
            char p_chars[] = {'?', 'P', 'N', 'B', 'R', 'Q', 'K'};
            out[idx++] = p_chars[m.promo];
        }
        out[idx] = '\0';
    }
    
    Board next;
    make_move(before, m, &next);
    int opp_king = find_king(&next, next.turn);
    if (opp_king != -1 && is_square_attacked(&next, opp_king, -next.turn)) {
        MoveList opp_legals;
        generate_legal_moves(&next, &opp_legals);
        if (opp_legals.count == 0) strcat(out, "#");
        else strcat(out, "+");
    }
}

// UI Rendering Core
const char* get_piece_symbol(int p) {
    switch (p) {
        case  1: return "♙";
        case  2: return "♘";
        case  3: return "♗";
        case  4: return "♖";
        case  5: return "♕";
        case  6: return "♔";
        case -1: return "♟";
        case -2: return "♞";
        case -3: return "♝";
        case -4: return "♜";
        case -5: return "♛";
        case -6: return "♚";
        default: return " ";
    }
}

int get_square_bg(int sq, const MoveList *legals) {
    int r = sq / 8, c = sq % 8;
    
    // King check highlighting
    if (current_board.board[sq] == 6 * current_board.turn) {
        if (is_square_attacked(&current_board, sq, -current_board.turn)) return 196; // Crimson Red
    }
    
    if (sq == selected_sq) return 32; // Selected state (Ice Blue)
    if (sq == cursor_sq) return 27;   // Active user cursor
    
    // Legal moves hint highlights
    if (selected_sq != -1) {
        for (int i = 0; i < legals->count; ++i) {
            if (legals->moves[i].from == selected_sq && legals->moves[i].to == sq) {
                return 71; // Mint Green
            }
        }
    }
    
    // Last move coordinates highlighted
    if (history_count > 0) {
        Move lm = history_moves[history_count - 1];
        if (sq == lm.from || sq == lm.to) return 142; // Soft Brass
    }
    
    return ((r + c) % 2 == 0) ? 253 : 242; // Checkerboard styling
}

void print_hud(int row, const MoveList *legals) {
    switch (row) {
        case 0: printf("   \033[1;36m┌──────────────────────────────────────────────┐\033[0m"); break;
        case 1: printf("   \033[1;36m│\033[0m   \033[1;37mTERMINAL CHESS ENGINE GUI\033[0m               \033[1;36m│\033[0m"); break;
        case 2: printf("   \033[1;36m├──────────────────────────────────────────────┤\033[0m"); break;
        case 3: 
            printf("   \033[1;36m│\033[0m   Active Turn: %-25s \033[1;36m│\033[0m", 
                   (current_board.turn == 1) ? "\033[1;37mWhite\033[0m" : "\033[1;35mBlack\033[0m"); 
            break;
        case 4: {
            char tc_str[64];
            if (current_tc == TC_DEPTH) sprintf(tc_str, "Depth: %d", tc_depth);
            else if (current_tc == TC_NODES) sprintf(tc_str, "Nodes: %d", tc_nodes);
            else sprintf(tc_str, "Movetime: %.1fs", tc_time / 1000.0);
            printf("   \033[1;36m│\033[0m   Time Limit: %-26s \033[1;36m│\033[0m", tc_str);
            break;
        }
        case 5: {
            char eng_str[64];
            if (engine_active) {
                char *slash = strrchr(engine_path, '/');
                sprintf(eng_str, "\033[1;32m%s\033[0m", slash ? slash + 1 : engine_path);
            } else {
                sprintf(eng_str, "Local PvP Mode");
            }
            printf("   \033[1;36m│\033[0m   Engine: %-35s \033[1;36m│\033[0m", eng_str);
            break;
        }
        case 6: {
            char state[64] = "\033[1;32mActive Game\033[0m";
            if (is_square_attacked(&current_board, find_king(&current_board, current_board.turn), -current_board.turn)) {
                if (legals->count == 0) strcpy(state, "\033[1;31mCheckmate!\033[0m");
                else strcpy(state, "\033[1;31mCheck!\033[0m");
            } else if (legals->count == 0) {
                strcpy(state, "\033[1;33mStalemate Draw\033[0m");
            } else if (current_board.halfmove >= 100) {
                strcpy(state, "\033[1;33mDraw (50-move rule)\033[0m");
            } else if (engine_thinking) {
                strcpy(state, "\033[1;35mEngine is thinking...\033[0m");
            }
            printf("   \033[1;36m│\033[0m   Game State: %-30s \033[1;36m│\033[0m", state);
            break;
        }
        case 7: printf("   \033[1;36m└──────────────────────────────────────────────┘\033[0m"); break;
    }
}

void draw_screen() {
    MoveList legals;
    generate_legal_moves(&current_board, &legals);
    
    printf("\033[H"); // Home cursor for in-place rendering
    
    printf("\n      \033[1;37m[  a   b   c   d   e   f   g   h  ]\033[0m\n");
    for (int r = 0; r < 8; ++r) {
        printf("  \033[1;37m%d  \033[0m", 8 - r);
        for (int c = 0; c < 8; ++c) {
            int sq = SQ(r, c);
            int bg = get_square_bg(sq, &legals);
            int p = current_board.board[sq];
            int fg = (p > 0) ? 231 : 16; // Pure white vs solid black piece styling
            
            // Format square layouts to render perfectly square cells
            printf("\033[48;5;%dm\033[38;5;%dm %s  \033[0m", bg, fg, get_piece_symbol(p));
        }
        printf(" \033[1;37m %d\033[0m", 8 - r);
        print_hud(r, &legals);
        printf("\n");
    }
    printf("      \033[1;37m[  a   b   c   d   e   f   g   h  ]\033[0m\n\n");
    
    // Help details
    printf("\033[1;30m  Controls:\033[0m ARROWS to navigate | SPACE/ENTER to Select/Move | ESC to Deselect\n");
    printf("            \033[1;33m[u]\033[0m Undo | \033[1;33m[t]\033[0m Config TC | \033[1;33m[e]\033[0m Load UCI Engine | \033[1;31m[q]\033[0m Quit\n\n");
    
    // PGN Move History Box
    printf("  \033[1;34mMove History (PGN):\033[0m\n  ");
    int print_len = 2;
    for (int i = 0; i < history_count; ++i) {
        char item[32];
        if (i % 2 == 0) {
            sprintf(item, "\033[1;37m%d.\033[0m %s ", (i / 2) + 1, history_san[i]);
        } else {
            sprintf(item, "%s  ", history_san[i]);
        }
        if (print_len + strlen(item) > 85) {
            printf("\n  ");
            print_len = 2;
        }
        printf("%s", item);
        print_len += strlen(item);
    }
    printf("\033[K\n\n"); // Clear remains
}

int ask_promotion() {
    disable_raw_mode();
    printf("\033[?25h"); // Restore cursor
    printf("\n  \033[1;33mPromote pawn to (q = Queen, r = Rook, b = Bishop, n = Knight): \033[0m");
    fflush(stdout);
    char buf[128];
    int promo = 5; // Queen default
    if (fgets(buf, sizeof(buf), stdin)) {
        if (buf[0] == 'n' || buf[0] == 'N') promo = 2;
        else if (buf[0] == 'b' || buf[0] == 'B') promo = 3;
        else if (buf[0] == 'r' || buf[0] == 'R') promo = 4;
    }
    enable_raw_mode();
    return promo;
}

void prompt_engine() {
    disable_raw_mode();
    printf("\033[?25h\n");
    printf("  \033[1;32mEnter path to UCI engine (e.g. /opt/homebrew/bin/stockfish):\033[0m\n  ");
    fflush(stdout);
    char buf[512];
    if (fgets(buf, sizeof(buf), stdin)) {
        buf[strcspn(buf, "\n")] = 0;
        if (strlen(buf) > 0) {
            strcpy(engine_path, buf);
            if (!start_engine(engine_path)) {
                printf("  \033[1;31mError launching engine at path!\033[0m Press enter...\n");
                getchar();
                engine_path[0] = '\0';
            } else {
                printf("  \033[1;32mEngine loaded successfully!\033[0m Set your color (w/b): ");
                fflush(stdout);
                char col_buf[64];
                if (fgets(col_buf, sizeof(col_buf), stdin)) {
                    if (col_buf[0] == 'b' || col_buf[0] == 'B') player_color = -1;
                    else player_color = 1;
                }
            }
        } else {
            stop_engine();
            engine_path[0] = '\0';
        }
    }
    enable_raw_mode();
    printf("\033[2J"); // Clear Screen
}

void prompt_time_control() {
    disable_raw_mode();
    printf("\033[?25h\n");
    printf("  \033[1;32mChoose Time Control Mode:\033[0m\n");
    printf("  1. Fixed Depth\n  2. Target Nodes\n  3. Move Time Limit\n  Choice: ");
    fflush(stdout);
    char choice[64];
    if (fgets(choice, sizeof(choice), stdin)) {
        int c = atoi(choice);
        if (c == 1) {
            current_tc = TC_DEPTH;
            printf("  Enter target Depth limit (1-30) [current: %d]: ", tc_depth);
            fflush(stdout);
            if (fgets(choice, sizeof(choice), stdin)) tc_depth = atoi(choice);
        } else if (c == 2) {
            current_tc = TC_NODES;
            printf("  Enter target Nodes count [current: %d]: ", tc_nodes);
            fflush(stdout);
            if (fgets(choice, sizeof(choice), stdin)) tc_nodes = atoi(choice);
        } else if (c == 3) {
            current_tc = TC_TIME;
            printf("  Enter move time limit in ms [current: %d]: ", tc_time);
            fflush(stdout);
            if (fgets(choice, sizeof(choice), stdin)) tc_time = atoi(choice);
        }
    }
    enable_raw_mode();
    printf("\033[2J");
}

void parse_engine_response() {
    char chunk[4096];
    int n = read(from_engine[0], chunk, sizeof(chunk) - 1);
    if (n <= 0) return;
    chunk[n] = '\0';
    
    // Simple line by line parser
    char *line = strtok(chunk, "\n");
    while (line != NULL) {
        if (strncmp(line, "bestmove", 8) == 0) {
            char mv_str[32];
            if (sscanf(line, "bestmove %s", mv_str) == 1) {
                int f_col = mv_str[0] - 'a';
                int f_row = '8' - mv_str[1];
                int t_col = mv_str[2] - 'a';
                int t_row = '8' - mv_str[3];
                int promo = 0;
                if (strlen(mv_str) > 4) {
                    char p = mv_str[4];
                    if (p == 'n') promo = 2;
                    else if (p == 'b') promo = 3;
                    else if (p == 'r') promo = 4;
                    else if (p == 'q') promo = 5;
                }
                
                Move m = {SQ(f_row, f_col), SQ(t_row, t_col), promo};
                
                // Confirm legality from engine
                MoveList legals;
                generate_legal_moves(&current_board, &legals);
                int valid = 0;
                for (int i = 0; i < legals.count; ++i) {
                    if (legals.moves[i].from == m.from && legals.moves[i].to == m.to) {
                        m.promo = legals.moves[i].promo; // Ensure promo field parity
                        valid = 1;
                        break;
                    }
                }
                
                if (valid) {
                    char san[16];
                    get_san(&current_board, m, san);
                    strcpy(history_san[history_count], san);
                    
                    history_boards[history_count] = current_board;
                    history_moves[history_count] = m;
                    history_count++;
                    
                    Board next;
                    make_move(&current_board, m, &next);
                    current_board = next;
                }
                engine_thinking = 0;
            }
        }
        line = strtok(NULL, "\n");
    }
}

int main() {
    printf("\033[2J\033[H"); // Complete clear and home
    init_board(&current_board);
    enable_raw_mode();
    
    while (1) {
        draw_screen();
        
        // Handle CPU moves asynchronously
        if (engine_active && current_board.turn != player_color && !engine_thinking) {
            MoveList legals;
            generate_legal_moves(&current_board, &legals);
            if (legals.count > 0) {
                trigger_engine_thinking();
            }
        }
        
        // I/O multiplexer (100ms timeout loop to maintain terminal refreshes)
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int max_fd = STDIN_FILENO;
        if (engine_active && engine_thinking) {
            FD_SET(from_engine[0], &fds);
            if (from_engine[0] > max_fd) max_fd = from_engine[0];
        }
        
        struct timeval tv = {0, 100000};
        int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) break;
        
        if (engine_active && FD_ISSET(from_engine[0], &fds)) {
            parse_engine_response();
            continue;
        }
        
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            unsigned char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 27) { // ANSI Escape sequences (Arrow keys)
                    unsigned char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if (seq[0] == 91) {
                            switch (seq[1]) {
                                case 65: if (cursor_sq >= 8) cursor_sq -= 8; break; // Up
                                case 66: if (cursor_sq < 56) cursor_sq += 8; break; // Down
                                case 67: if (cursor_sq % 8 < 7) cursor_sq += 1; break; // Right
                                case 68: if (cursor_sq % 8 > 0) cursor_sq -= 1; break; // Left
                            }
                        }
                    } else {
                        selected_sq = -1; // Deselect on ESC
                    }
                } else if (c == ' ' || c == '\n' || c == '\r') {
                    // Do not allow player inputs during engine turn
                    if (engine_active && current_board.turn != player_color) continue;
                    
                    if (selected_sq == -1) {
                        if (current_board.board[cursor_sq] * current_board.turn > 0) {
                            selected_sq = cursor_sq;
                        }
                    } else {
                        if (cursor_sq == selected_sq) {
                            selected_sq = -1;
                        } else {
                            // Find matching valid moves
                            MoveList legals;
                            generate_legal_moves(&current_board, &legals);
                            int index = -1;
                            for (int i = 0; i < legals.count; ++i) {
                                if (legals.moves[i].from == selected_sq && legals.moves[i].to == cursor_sq) {
                                    index = i;
                                    break;
                                }
                            }
                            
                            if (index != -1) {
                                Move m = legals.moves[index];
                                if (m.promo) {
                                    m.promo = ask_promotion();
                                }
                                
                                char san[16];
                                get_san(&current_board, m, san);
                                strcpy(history_san[history_count], san);
                                
                                history_boards[history_count] = current_board;
                                history_moves[history_count] = m;
                                history_count++;
                                
                                Board next;
                                make_move(&current_board, m, &next);
                                current_board = next;
                                selected_sq = -1;
                            } else {
                                // Pivot selection directly to new friendly piece if clicked
                                if (current_board.board[cursor_sq] * current_board.turn > 0) {
                                    selected_sq = cursor_sq;
                                }
                            }
                        }
                    }
                } else if (c == 'u' || c == 'U') {
                    // Backtrack engine and player states
                    if (engine_active) {
                        if (history_count >= 2) {
                            history_count -= 2;
                            current_board = history_boards[history_count];
                        }
                    } else if (history_count >= 1) {
                        history_count--;
                        current_board = history_boards[history_count];
                    }
                    selected_sq = -1;
                } else if (c == 'e' || c == 'E') {
                    prompt_engine();
                } else if (c == 't' || c == 'T') {
                    prompt_time_control();
                } else if (c == 'q' || c == 'Q') {
                    break;
                }
            }
        }
    }
    
    stop_engine();
    disable_raw_mode();
    return 0;
}
