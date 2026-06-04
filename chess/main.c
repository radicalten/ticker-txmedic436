#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdbool.h>
#include <ctype.h>

#define MAX_MOVES 2048

// Piece Representation: Positive = White, Negative = Black
// Empty = 0, Pawn = 1, Knight = 2, Bishop = 3, Rook = 4, Queen = 5, King = 6
typedef struct {
    int board[64];
    int active_color;       // 1 = White, -1 = Black
    int castle;             // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_sq;              // EP target square (0-63) or -1
    int halfmove_clock;
    int fullmove_number;
} GameState;

typedef struct {
    int from;
    int to;
    int promotion;          // 0 = None, 2=Knight, 3=Bishop, 4=Rook, 5=Queen
} Move;

typedef enum { MODE_DEPTH, MODE_NODES, MODE_TIME } EngineMode;

// Global State Variables
GameState state_history[MAX_MOVES];
Move move_history[MAX_MOVES];
int state_history_count = 0;

int last_move_from = -1;
int last_move_to = -1;

EngineMode engine_mode = MODE_DEPTH;
int engine_val = 10;        // Default depth = 10 plies

int engine_in[2];           // Pipe to write to engine
int engine_out[2];          // Pipe to read from engine
pid_t engine_pid = -1;
char engine_path[512] = "stockfish";

struct termios orig_termios;

// Forward Declarations
void disable_raw_mode(void);
void make_move(GameState *state, Move m);
bool is_square_attacked(const GameState *state, int sq, int attacker_color);
int get_all_legal_moves(const GameState *state, Move *moves);

// Terminal Configuration
void disable_raw_mode(void) {
    printf("\033[?25h"); // Show cursor
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; // 100ms timeout
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

// UCI Engine Pipe Management
void start_engine(const char *path) {
    pipe(engine_in);
    pipe(engine_out);
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[1]);
        close(engine_out[0]);
        execlp(path, path, (char *)NULL);
        perror("Failed to start UCI engine process!");
        exit(1);
    }
    close(engine_in[0]);
    close(engine_out[1]);
    int flags = fcntl(engine_out[0], F_GETFL, 0);
    fcntl(engine_out[0], F_SETFL, flags | O_NONBLOCK);
}

void send_to_engine(const char *cmd) {
    if (engine_pid <= 0) return;
    write(engine_in[1], cmd, strlen(cmd));
}

bool read_line_nonblock(int fd, char *buf, int max_len) {
    static char temp_buf[8192];
    static int temp_len = 0;
    char c;
    while (read(fd, &c, 1) > 0) {
        if (c == '\n') {
            temp_buf[temp_len] = '\0';
            strncpy(buf, temp_buf, max_len);
            temp_len = 0;
            return true;
        } else if (c != '\r') {
            if (temp_len < (int)sizeof(temp_buf) - 1) {
                temp_buf[temp_len++] = c;
            }
        }
    }
    return false;
}

// Chess Board Initialization
void init_board(GameState *state) {
    memset(state->board, 0, sizeof(state->board));
    // Set pieces for Black Rank 8
    state->board[0] = -4; state->board[1] = -2; state->board[2] = -3; state->board[3] = -5;
    state->board[4] = -6; state->board[5] = -3; state->board[6] = -2; state->board[7] = -4;
    // Set pieces for White Rank 1
    state->board[56] = 4; state->board[57] = 2; state->board[58] = 3; state->board[59] = 5;
    state->board[60] = 6; state->board[61] = 3; state->board[62] = 2; state->board[63] = 4;
    for (int i = 0; i < 8; i++) {
        state->board[8 + i] = -1;  // Black Pawns
        state->board[48 + i] = 1;  // White Pawns
    }
    state->active_color = 1;
    state->castle = 15;
    state->ep_sq = -1;
    state->halfmove_clock = 0;
    state->fullmove_number = 1;
}

bool on_board(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

int find_king(const GameState *state, int color) {
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == color * 6) return i;
    }
    return -1;
}

// Pseudo-Legal Move Generator
void generate_pseudo_moves(const GameState *state, int from, Move *moves, int *count) {
    int piece = state->board[from];
    if (piece == 0) return;
    int color = (piece > 0) ? 1 : -1;
    if (color != state->active_color) return;
    int type = abs(piece);
    int r = from / 8;
    int c = from % 8;

    if (type == 1) { // Pawn
        int dir = (color == 1) ? -1 : 1;
        int promo_row = (color == 1) ? 0 : 7;
        int start_row = (color == 1) ? 6 : 1;

        int nr = r + dir;
        int nc = c;
        if (on_board(nr, nc) && state->board[nr * 8 + nc] == 0) {
            if (nr == promo_row) {
                for (int promo = 2; promo <= 5; promo++) moves[(*count)++] = (Move){from, nr * 8 + nc, promo};
            } else {
                moves[(*count)++] = (Move){from, nr * 8 + nc, 0};
                int nnr = r + 2 * dir;
                if (r == start_row && state->board[nnr * 8 + nc] == 0) {
                    moves[(*count)++] = (Move){from, nnr * 8 + nc, 0};
                }
            }
        }
        int dcs[2] = {-1, 1};
        for (int i = 0; i < 2; i++) {
            nc = c + dcs[i];
            nr = r + dir;
            if (on_board(nr, nc)) {
                int target_sq = nr * 8 + nc;
                int target_p = state->board[target_sq];
                if (target_p != 0 && ((target_p > 0) != (color > 0))) {
                    if (nr == promo_row) {
                        for (int promo = 2; promo <= 5; promo++) moves[(*count)++] = (Move){from, target_sq, promo};
                    } else {
                        moves[(*count)++] = (Move){from, target_sq, 0};
                    }
                } else if (target_sq == state->ep_sq) {
                    moves[(*count)++] = (Move){from, target_sq, 0};
                }
            }
        }
    } else if (type == 2) { // Knight
        int offsets[8][2] = {{-2, -1}, {-2, 1}, {-1, -2}, {-1, 2}, {1, -2}, {1, 2}, {2, -1}, {2, 1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + offsets[i][0], nc = c + offsets[i][1];
            if (on_board(nr, nc)) {
                int tp = state->board[nr * 8 + nc];
                if (tp == 0 || ((tp > 0) != (color > 0))) moves[(*count)++] = (Move){from, nr * 8 + nc, 0};
            }
        }
    } else if (type == 3 || type == 5) { // Bishop / Queen Diagonal
        int dirs[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
        for (int i = 0; i < 4; i++) {
            int nr = r, nc = c;
            while (1) {
                nr += dirs[i][0]; nc += dirs[i][1];
                if (!on_board(nr, nc)) break;
                int tp = state->board[nr * 8 + nc];
                if (tp == 0) moves[(*count)++] = (Move){from, nr * 8 + nc, 0};
                else {
                    if ((tp > 0) != (color > 0)) moves[(*count)++] = (Move){from, nr * 8 + nc, 0};
                    break;
                }
            }
        }
    }
    if (type == 4 || type == 5) { // Rook / Queen Straight
        int dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (int i = 0; i < 4; i++) {
            int nr = r, nc = c;
            while (1) {
                nr += dirs[i][0]; nc += dirs[i][1];
                if (!on_board(nr, nc)) break;
                int tp = state->board[nr * 8 + nc];
                if (tp == 0) moves[(*count)++] = (Move){from, nr * 8 + nc, 0};
                else {
                    if ((tp > 0) != (color > 0)) moves[(*count)++] = (Move){from, nr * 8 + nc, 0};
                    break;
                }
            }
        }
    } else if (type == 6) { // King
        int offsets[8][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + offsets[i][0], nc = c + offsets[i][1];
            if (on_board(nr, nc)) {
                int tp = state->board[nr * 8 + nc];
                if (tp == 0 || ((tp > 0) != (color > 0))) moves[(*count)++] = (Move){from, nr * 8 + nc, 0};
            }
        }
        // Castling
        if (color == 1) {
            if ((state->castle & 1) && state->board[61] == 0 && state->board[62] == 0) {
                if (!is_square_attacked(state, 60, -1) && !is_square_attacked(state, 61, -1) && !is_square_attacked(state, 62, -1))
                    moves[(*count)++] = (Move){60, 62, 0};
            }
            if ((state->castle & 2) && state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0) {
                if (!is_square_attacked(state, 60, -1) && !is_square_attacked(state, 59, -1) && !is_square_attacked(state, 58, -1))
                    moves[(*count)++] = (Move){60, 58, 0};
            }
        } else {
            if ((state->castle & 4) && state->board[5] == 0 && state->board[6] == 0) {
                if (!is_square_attacked(state, 4, 1) && !is_square_attacked(state, 5, 1) && !is_square_attacked(state, 6, 1))
                    moves[(*count)++] = (Move){4, 6, 0};
            }
            if ((state->castle & 8) && state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0) {
                if (!is_square_attacked(state, 4, 1) && !is_square_attacked(state, 3, 1) && !is_square_attacked(state, 2, 1))
                    moves[(*count)++] = (Move){4, 2, 0};
            }
        }
    }
}

// Attack Mapping (Determines Check status)
bool is_square_attacked(const GameState *state, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;

    int knight_offsets[8][2] = {{-2, -1}, {-2, 1}, {-1, -2}, {-1, 2}, {1, -2}, {1, 2}, {2, -1}, {2, 1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_offsets[i][0], nc = c + knight_offsets[i][1];
        if (on_board(nr, nc) && state->board[nr * 8 + nc] == attacker_color * 2) return true;
    }

    int king_offsets[8][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + king_offsets[i][0], nc = c + king_offsets[i][1];
        if (on_board(nr, nc) && state->board[nr * 8 + nc] == attacker_color * 6) return true;
    }

    int p_dir = (attacker_color == 1) ? -1 : 1;
    int ar = r - p_dir;
    for (int dc = -1; dc <= 1; dc += 2) {
        int ac = c + dc;
        if (on_board(ar, ac) && state->board[ar * 8 + ac] == attacker_color * 1) return true;
    }

    int rook_dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += rook_dirs[i][0]; nc += rook_dirs[i][1];
            if (!on_board(nr, nc)) break;
            int p = state->board[nr * 8 + nc];
            if (p != 0) {
                if (p == attacker_color * 4 || p == attacker_color * 5) return true;
                break;
            }
        }
    }

    int bishop_dirs[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += bishop_dirs[i][0]; nc += bishop_dirs[i][1];
            if (!on_board(nr, nc)) break;
            int p = state->board[nr * 8 + nc];
            if (p != 0) {
                if (p == attacker_color * 3 || p == attacker_color * 5) return true;
                break;
            }
        }
    }
    return false;
}

// Legal Move Filtering
bool is_legal_move(const GameState *state, Move m) {
    GameState next_state = *state;
    int color = state->active_color;
    make_move(&next_state, m);
    int king_sq = find_king(&next_state, color);
    if (king_sq == -1) return false;
    return !is_square_attacked(&next_state, king_sq, -color);
}

int get_all_legal_moves(const GameState *state, Move *moves) {
    int count = 0;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] != 0 && ((state->board[i] > 0) == (state->active_color == 1))) {
            Move pseudo[64];
            int p_count = 0;
            generate_pseudo_moves(state, i, pseudo, &p_count);
            for (int j = 0; j < p_count; j++) {
                if (is_legal_move(state, pseudo[j])) {
                    moves[count++] = pseudo[j];
                }
            }
        }
    }
    return count;
}

// State transition
void make_move(GameState *state, Move m) {
    int piece = state->board[m.from];
    int type = abs(piece);
    int color = state->active_color;

    if (type == 1 && m.to == state->ep_sq) {
        int ep_pawn_sq = m.to + (color == 1 ? 8 : -8);
        state->board[ep_pawn_sq] = 0;
    }

    int next_ep_sq = -1;
    if (type == 1 && abs(m.to - m.from) == 16) {
        next_ep_sq = m.from + (color == 1 ? -8 : 8);
    }

    if (type == 6) {
        if (m.from == 60 && m.to == 62) { state->board[61] = state->board[63]; state->board[63] = 0; }
        else if (m.from == 60 && m.to == 58) { state->board[59] = state->board[56]; state->board[56] = 0; }
        else if (m.from == 4 && m.to == 6) { state->board[5] = state->board[7]; state->board[7] = 0; }
        else if (m.from == 4 && m.to == 2) { state->board[3] = state->board[0]; state->board[0] = 0; }
    }

    if (m.promotion != 0) {
        state->board[m.to] = color * m.promotion;
    } else {
        state->board[m.to] = piece;
    }
    state->board[m.from] = 0;

    // Castling updates
    if (m.from == 60) state->castle &= ~3;
    if (m.from == 4)  state->castle &= ~12;
    if (m.from == 56 || m.to == 56) state->castle &= ~2;
    if (m.from == 63 || m.to == 63) state->castle &= ~1;
    if (m.from == 0  || m.to == 0)  state->castle &= ~8;
    if (m.from == 7  || m.to == 7)  state->castle &= ~4;

    state->ep_sq = next_ep_sq;
    state->active_color = -color;
}

// UCI Conversions
void move_to_uci(Move m, char *buf) {
    int f1 = m.from % 8, r1 = 8 - (m.from / 8);
    int f2 = m.to % 8, r2 = 8 - (m.to / 8);
    sprintf(buf, "%c%c%c%c", 'a' + f1, '0' + r1, 'a' + f2, '0' + r2);
    if (m.promotion != 0) {
        char p_chars[] = "  nbrq";
        sprintf(buf + 4, "%c", p_chars[m.promotion]);
    }
}

Move parse_uci(const GameState *state, const char *str) {
    Move m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f1 = str[0] - 'a', r1 = 8 - (str[1] - '0');
    int f2 = str[2] - 'a', r2 = 8 - (str[3] - '0');
    m.from = r1 * 8 + f1;
    m.to = r2 * 8 + f2;
    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'n') m.promotion = 2;
        else if (p == 'b') m.promotion = 3;
        else if (p == 'r') m.promotion = 4;
        else if (p == 'q') m.promotion = 5;
    }
    return m;
}

// PGN Output Generation
void get_pgn_move_string(const GameState *prev_state, Move m, char *buf) {
    int piece = prev_state->board[m.from];
    int type = abs(piece);
    int target = prev_state->board[m.to];
    int buf_idx = 0;

    if (type == 6) {
        if (m.from == 60 && m.to == 62) { strcpy(buf, "O-O"); return; }
        if (m.from == 60 && m.to == 58) { strcpy(buf, "O-O-O"); return; }
        if (m.from == 4 && m.to == 6) { strcpy(buf, "O-O"); return; }
        if (m.from == 4 && m.to == 2) { strcpy(buf, "O-O-O"); return; }
    }

    if (type != 1) {
        char p_chars[] = "  NBRQK";
        buf[buf_idx++] = p_chars[type];
    } else if (target != 0 || m.to == prev_state->ep_sq) {
        buf[buf_idx++] = 'a' + (m.from % 8);
    }

    if (target != 0 || (type == 1 && m.to == prev_state->ep_sq)) {
        buf[buf_idx++] = 'x';
    }

    buf[buf_idx++] = 'a' + (m.to % 8);
    buf[buf_idx++] = '0' + (8 - (m.to / 8));

    if (m.promotion != 0) {
        buf[buf_idx++] = '=';
        char p_chars[] = "  NBRQ";
        buf[buf_idx++] = p_chars[m.promotion];
    }

    GameState next_state = *prev_state;
    make_move(&next_state, m);
    int enemy_king_sq = find_king(&next_state, next_state.active_color);
    if (enemy_king_sq != -1 && is_square_attacked(&next_state, enemy_king_sq, -next_state.active_color)) {
        Move esc_moves[256];
        if (get_all_legal_moves(&next_state, esc_moves) == 0) buf[buf_idx++] = '#';
        else buf[buf_idx++] = '+';
    }
    buf[buf_idx] = '\0';
}

// In-Place Frame Redraw
void draw_game(const GameState *state, int cursor_sq, int selected_sq, const char *status_msg) {
    // Escape code moves cursor to top left, clearing board area
    printf("\033[H");

    printf("  \033[1;36m┌──────────────────────────────────────────────┐\033[0m\n");
    printf("  \033[1;36m│          TERMINAL CHESS GUI (UCI)            │\033[0m\n");
    printf("  \033[1;36m└──────────────────────────────────────────────┘\033[0m\n\n");

    Move legal_moves[128];
    int legal_count = 0;
    if (selected_sq != -1) {
        Move pseudo[64];
        int p_count = 0;
        generate_pseudo_moves(state, selected_sq, pseudo, &p_count);
        for (int i = 0; i < p_count; i++) {
            if (is_legal_move(state, pseudo[i])) {
                legal_moves[legal_count++] = pseudo[i];
            }
        }
    }

    int active_king_sq = find_king(state, state->active_color);
    bool in_check = false;
    if (active_king_sq != -1) {
        in_check = is_square_attacked(state, active_king_sq, -state->active_color);
    }

    for (int r = 0; r < 8; r++) {
        printf("   %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            bool is_light = (r + c) % 2 == 0;
            const char *bg_color = is_light ? "\033[48;5;252m" : "\033[48;5;239m"; // Light/dark grey

            if (last_move_from == sq || last_move_to == sq) {
                bg_color = "\033[48;5;74m"; // Sky blue (previous move)
            }
            if (in_check && sq == active_king_sq) {
                bg_color = "\033[48;5;196m"; // Deep Red (check alert)
            }
            for (int i = 0; i < legal_count; i++) {
                if (legal_moves[i].to == sq) {
                    bg_color = "\033[48;5;71m"; // Light green (legal moves)
                    break;
                }
            }
            if (sq == selected_sq) {
                bg_color = "\033[48;5;214m"; // Orange (selected piece)
            }
            if (sq == cursor_sq) {
                bg_color = "\033[48;5;226m"; // Yellow (active cursor)
            }

            int p = state->board[sq];
            const char *fg_color = "";
            const char *sym = "   ";
            if (p > 0) {
                fg_color = "\033[38;5;255;1m"; // Bold white piece
                const char *white_syms[] = {"   ", " ♙ ", " ♘ ", " ♗ ", " ♖ ", " ♕ ", " ♔ "};
                sym = white_syms[p];
            } else if (p < 0) {
                fg_color = "\033[38;5;232;1m"; // Bold black piece
                const char *black_syms[] = {"   ", " ♟ ", " ♞ ", " ♝ ", " ♜ ", " ♛ ", " ♚ "};
                sym = black_syms[-p];
            }
            printf("%s%s%s\033[0m", bg_color, fg_color, sym);
        }

        printf("   ");
        switch (r) {
            case 0:
                printf("\033[1mActive Side:\033[0m %s", state->active_color == 1 ? "White (Player)" : "Black (Engine)");
                break;
            case 1:
                printf("\033[1mCastling:\033[0m ");
                if (state->castle & 1) printf("K");
                if (state->castle & 2) printf("Q");
                if (state->castle & 4) printf("k");
                if (state->castle & 8) printf("q");
                if (state->castle == 0) printf("-");
                break;
            case 2:
                printf("\033[1mEP Square:\033[0m ");
                if (state->ep_sq != -1) printf("%c%d", 'a' + (state->ep_sq % 8), 8 - (state->ep_sq / 8));
                else printf("-");
                break;
            case 3:
                printf("\033[1mEngine Mode:\033[0m %s", engine_mode == MODE_DEPTH ? "Depth Limit" :
                                                      (engine_mode == MODE_NODES ? "Nodes Limit" : "Time Limit"));
                break;
            case 4:
                printf("\033[1mEngine Target:\033[0m %d %s", engine_val, engine_mode == MODE_DEPTH ? "plies" :
                                                                       (engine_mode == MODE_NODES ? "nodes" : "ms"));
                break;
            case 5:
                printf("\033[1mRecent Moves:\033[0m");
                break;
            case 6:
            case 7: {
                int offset = (state_history_count - 1) / 2;
                int start_move = (offset > 1) ? offset - 1 : 0;
                int row_idx = r - 6;
                int m_idx = (start_move + row_idx) * 2;
                if (m_idx < state_history_count - 1) {
                    printf("  %d. ", start_move + row_idx + 1);
                    char pgn1[16] = "", pgn2[16] = "";
                    get_pgn_move_string(&state_history[m_idx], move_history[m_idx], pgn1);
                    printf("%-8s", pgn1);
                    if (m_idx + 1 < state_history_count - 1) {
                        get_pgn_move_string(&state_history[m_idx + 1], move_history[m_idx + 1], pgn2);
                        printf("%s", pgn2);
                    }
                }
                break;
            }
        }
        printf("\n");
    }
    printf("     a  b  c  d  e  f  g  h\n\n");
    printf("  \033[1mStatus:\033[0m %s\n", status_msg);
    printf("  \033[1;30mControls: [WASD/Arrows] Nav | [Space/Enter] Click | [U] Undo | [T] Change Mode | [C] Set Target | [Q] Quit\033[0m\n");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        strncpy(engine_path, argv[1], sizeof(engine_path) - 1);
    }

    GameState current_state;
    init_board(&current_state);

    state_history[0] = current_state;
    state_history_count = 1;

    start_engine(engine_path);
    send_to_engine("uci\n");
    send_to_engine("isready\n");

    // Clear Screen and save current layout state
    printf("\033[2J\033[H");
    enable_raw_mode();

    int cursor_sq = 52; // Starts over the e2 pawn
    int selected_sq = -1;
    char status_msg[512] = "Welcome! Move cursor and select your piece.";
    bool engine_thinking = false;
    char engine_buf[2048];

    // Brief handshake wait
    usleep(150000);

    while (1) {
        draw_game(&current_state, cursor_sq, selected_sq, status_msg);

        Move legal_moves[256];
        int legal_count = get_all_legal_moves(&current_state, legal_moves);
        int king_sq = find_king(&current_state, current_state.active_color);
        bool check = (king_sq != -1) && is_square_attacked(&current_state, king_sq, -current_state.active_color);

        if (legal_count == 0) {
            if (check) {
                sprintf(status_msg, "\033[1;31mCHECKMATE! %s wins.\033[0m", current_state.active_color == 1 ? "Black (Engine)" : "White (Player)");
            } else {
                sprintf(status_msg, "\033[1;33mSTALEMATE! The game is drawn.\033[0m");
            }
            draw_game(&current_state, cursor_sq, selected_sq, status_msg);
        }

        // Process Engine Search responses
        if (engine_thinking) {
            while (read_line_nonblock(engine_out[0], engine_buf, sizeof(engine_buf))) {
                if (strncmp(engine_buf, "bestmove", 8) == 0) {
                    char move_str[16];
                    sscanf(engine_buf, "bestmove %s", move_str);
                    Move m = parse_uci(&current_state, move_str);
                    if (m.from != -1) {
                        last_move_from = m.from;
                        last_move_to = m.to;
                        move_history[state_history_count - 1] = m;
                        make_move(&current_state, m);
                        state_history[state_history_count++] = current_state;
                        sprintf(status_msg, "Engine played %s", move_str);
                    } else {
                        sprintf(status_msg, "Engine passed or made an invalid move.");
                    }
                    engine_thinking = false;
                    break;
                }
            }
        }

        // Trigger Engine turn
        if (current_state.active_color == -1 && !engine_thinking && legal_count > 0) {
            char cmd[8192];
            int offset = sprintf(cmd, "position startpos moves");
            for (int i = 0; i < state_history_count - 1; i++) {
                char uci_m[16];
                move_to_uci(move_history[i], uci_m);
                offset += sprintf(cmd + offset, " %s", uci_m);
            }
            sprintf(cmd + offset, "\n");
            send_to_engine(cmd);

            if (engine_mode == MODE_DEPTH) {
                sprintf(cmd, "go depth %d\n", engine_val);
            } else if (engine_mode == MODE_NODES) {
                sprintf(cmd, "go nodes %d\n", engine_val);
            } else {
                sprintf(cmd, "go wtime %d btime %d\n", engine_val, engine_val);
            }
            send_to_engine(cmd);
            engine_thinking = true;
            strcpy(status_msg, "Engine is thinking...");
            continue;
        }

        // Process keyboard inputs
        char ch = '\0';
        int n_read = read(STDIN_FILENO, &ch, 1);
        if (n_read > 0) {
            if (ch == '\033') { // Arrow Key Parsing
                char seq[3];
                if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                    if (seq[0] == '[') {
                        int r = cursor_sq / 8, c = cursor_sq % 8;
                        switch (seq[1]) {
                            case 'A': if (r > 0) r--; break; // Up
                            case 'B': if (r < 7) r++; break; // Down
                            case 'C': if (c < 7) c++; break; // Right
                            case 'D': if (c > 0) c--; break; // Left
                        }
                        cursor_sq = r * 8 + c;
                    }
                }
            } else if (ch == 'w' || ch == 'W') {
                int r = cursor_sq / 8; if (r > 0) r--; cursor_sq = r * 8 + (cursor_sq % 8);
            } else if (ch == 's' || ch == 'S') {
                int r = cursor_sq / 8; if (r < 7) r++; cursor_sq = r * 8 + (cursor_sq % 8);
            } else if (ch == 'a' || ch == 'A') {
                int c = cursor_sq % 8; if (c > 0) c--; cursor_sq = (cursor_sq / 8) * 8 + c;
            } else if (ch == 'd' || ch == 'D') {
                int c = cursor_sq % 8; if (c < 7) c++; cursor_sq = (cursor_sq / 8) * 8 + c;
            } else if (ch == ' ' || ch == '\n') {
                if (current_state.active_color == 1) {
                    if (selected_sq == -1) {
                        int p = current_state.board[cursor_sq];
                        if (p > 0) {
                            selected_sq = cursor_sq;
                            strcpy(status_msg, "Square selected. Target target location.");
                        } else {
                            strcpy(status_msg, "Select one of your White pieces!");
                        }
                    } else {
                        Move player_move = {selected_sq, cursor_sq, 0};
                        if (abs(current_state.board[selected_sq]) == 1 && cursor_sq / 8 == 0) {
                            player_move.promotion = 5; // Queen auto-promotion
                        }
                        Move legals[256];
                        int l_count = get_all_legal_moves(&current_state, legals);
                        bool is_legal = false;
                        for (int i = 0; i < l_count; i++) {
                            if (legals[i].from == player_move.from && legals[i].to == player_move.to) {
                                player_move = legals[i];
                                is_legal = true;
                                break;
                            }
                        }
                        if (is_legal) {
                            last_move_from = player_move.from;
                            last_move_to = player_move.to;
                            move_history[state_history_count - 1] = player_move;
                            make_move(&current_state, player_move);
                            state_history[state_history_count++] = current_state;
                            selected_sq = -1;
                            strcpy(status_msg, "Move submitted. Waiting for engine search...");
                        } else {
                            selected_sq = -1;
                            strcpy(status_msg, "Move not legal. Select piece again.");
                        }
                    }
                }
            } else if (ch == 'u' || ch == 'U') { // Undo both turns
                if (state_history_count > 2) {
                    if (engine_thinking) {
                        send_to_engine("stop\n");
                        engine_thinking = false;
                    }
                    state_history_count -= 2;
                    current_state = state_history[state_history_count - 1];
                    if (state_history_count > 1) {
                        last_move_from = move_history[state_history_count - 2].from;
                        last_move_to = move_history[state_history_count - 2].to;
                    } else {
                        last_move_from = -1; last_move_to = -1;
                    }
                    selected_sq = -1;
                    strcpy(status_msg, "Move sequence undone.");
                } else {
                    strcpy(status_msg, "No historical states found!");
                }
            } else if (ch == 't' || ch == 'T') {
                engine_mode = (engine_mode + 1) % 3;
                if (engine_mode == MODE_DEPTH) engine_val = 10;
                else if (engine_mode == MODE_NODES) engine_val = 50000;
                else engine_val = 2000;
                sprintf(status_msg, "Time control switched to %s.", engine_mode == MODE_DEPTH ? "Depth limit" :
                                                                   (engine_mode == MODE_NODES ? "Nodes limit" : "Time limit"));
            } else if (ch == 'c' || ch == 'C') {
                disable_raw_mode();
                printf("\033[H\033[2J");
                printf("Enter integer parameter value for %s: ", engine_mode == MODE_DEPTH ? "depth limit" :
                                                                  (engine_mode == MODE_NODES ? "nodes limit" : "time limit (ms)"));
                fflush(stdout);
                int val;
                if (scanf("%d", &val) == 1) {
                    engine_val = val;
                    sprintf(status_msg, "Constraints target adjusted to %d.", engine_val);
                } else {
                    strcpy(status_msg, "Invalid entry.");
                }
                enable_raw_mode();
            } else if (ch == 'q' || ch == 'Q') {
                break;
            }
        }
        usleep(15000); // UI Refresh cycle tick
    }

    disable_raw_mode();
    if (engine_pid > 0) {
        kill(engine_pid, SIGKILL);
        waitpid(engine_pid, NULL, 0);
    }
    printf("\033[2J\033[HSession closed. Thank you for playing!\n");
    return 0;
}
